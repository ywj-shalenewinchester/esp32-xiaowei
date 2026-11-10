// ============================================================================
//  C61Module —— ESP32-C61 无线协处理器驱动（复位 + UART 探针）
//
//  本板 ESP32-P4 自身没有无线能力：WiFi 由板载的 ESP32-C61 无线协处理器（U14）提供，
//  P4 通过 SDIO 用 esp-hosted 连它（那部分由组件自己完成，见下）。本文件只做最基础的
//  一步：复位 C61，并把它 UART0 的启动日志抓到 P4 的日志里 —— 用来判断模块里到底
//  有没有固件、有没有真的跑起来。
//
//  联网其实不归这里管：esp_hosted 在 Kconfig 打开后会自己初始化，esp_wifi_remote
//  把标准的 esp_wifi_* 调用转发给协处理器，所以 WifiBoard / WifiManager 一行板级
//  代码都不用改。官方 P4 板就是这么做的（板级 .cc 里零 esp_hosted 调用）。
//
//  硬件接线（取自 config.h 第 7 节，已与参考实现核对：ai_toy_hwt/main/board_c61.c）
//    C61 EN   <- P4 GPIO7（WIFI_EN_PIN）      低 = 复位
//    C61 TX0  -> P4 GPIO8（WIFI_UART_TX_PIN）
//    C61 RX0  <- P4 GPIO9（WIFI_UART_RX_PIN）
//
//  ⚠ 这两根线极易接反：config.h 里的宏是**站在 C61 视角**命名的
//  （WIFI_UART_TX_PIN 指的是"C61 的 TX0"），所以配置 P4 这边时是：
//      P4 的 RX = WIFI_UART_TX_PIN (GPIO8) <- C61 的 TX0
//      P4 的 TX = WIFI_UART_RX_PIN (GPIO9) -> C61 的 RX0
//
//  错误处理约定（和框架其它驱动一致）：
//    C61 相关操作全部返回 esp_err_t，失败只打日志、继续往下跑，
//    绝不用 ESP_ERROR_CHECK 包裹 —— 它一旦 abort 就是全芯片重启，
//    而 C61 没起来只应该意味着"无线不可用"，不该拖垮整台设备。
// ============================================================================

#include "c61_module.h"

#include "config.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <driver/gpio.h>
#include <driver/uart.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "C61Module"

// ---- C61 串口（P4 侧的 UART1，对端是 C61 的 UART0）----
// 只用来收日志，所以不需要发送缓冲区
static constexpr uart_port_t C61_UART_PORT        = UART_NUM_1;
static constexpr int         C61_UART_BAUD        = 115200;
static constexpr int         C61_UART_RX_BUF_SIZE = 2048;

// P4 侧 UART 的引脚方向：见文件头 ⚠ 的说明，这里不能想当然
static constexpr gpio_num_t  C61_UART_P4_TX_PIN   = (gpio_num_t)WIFI_UART_RX_PIN;  // GPIO9 -> C61 RX0
static constexpr gpio_num_t  C61_UART_P4_RX_PIN   = (gpio_num_t)WIFI_UART_TX_PIN;  // GPIO8 <- C61 TX0

// EN 复位脉冲：拉低总时长必须 >50ms，模组才会真正复位
static constexpr int C61_EN_LOW_HOLD_MS   = 60;
static constexpr int C61_EN_FIRST_HOLD_MS = 10;
// 抓 C61 启动日志的时间窗口（从复位那一刻起算，够看完 ROM 横幅 + 固件启动）
static constexpr int C61_PROBE_DURATION_MS = 3000;

// ============================================================================
//  构造 / 析构
// ============================================================================

C61Module::C61Module() {
}

C61Module::~C61Module() {
    if (uart_ready_) {
        uart_driver_delete(C61_UART_PORT);
        uart_ready_ = false;
    }
}

// ============================================================================
//  对外接口
// ============================================================================

// 完整初始化：装串口 -> 复位 -> 抓启动日志
esp_err_t C61Module::Init() {
    ESP_LOGI(TAG, "初始化 C61 无线协处理器：EN=GPIO%d，串口 UART%d（P4 TX=GPIO%d -> C61 RX0，"
                  "P4 RX=GPIO%d <- C61 TX0，%d bps）",
             WIFI_EN_PIN, C61_UART_PORT, C61_UART_P4_TX_PIN, C61_UART_P4_RX_PIN, C61_UART_BAUD);

    esp_err_t err = EnsureUart();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "C61 串口不可用，跳过探针（只影响无线，不影响其它外设）");
        return err;
    }

    err = Probe(C61_PROBE_DURATION_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "C61 探针没跑完，模块状态未知");
        return err;
    }

    if (IsOnline()) {
        ESP_LOGI(TAG, "C61 已启动（串口上看到了启动日志）");
    } else {
        ESP_LOGW(TAG, "C61 一个字都没吐：模块没跑起来 / flash 里没固件 / UART 没接通，三选一");
    }
    return ESP_OK;
}

// 复位 C61 并把它 UART 上出来的东西逐行打到日志里
esp_err_t C61Module::Probe(int duration_ms) {
    if (!uart_ready_) {
        ESP_LOGE(TAG, "C61 串口未就绪，无法探针");
        return ESP_ERR_INVALID_STATE;
    }

    // 丢掉复位前的残留字节，保证看到的是从上电第一条开始的日志
    uart_flush_input(C61_UART_PORT);

    ESP_LOGI(TAG, "复位 C61 并抓 %dms 串口输出...", duration_ms);
    HoldIo9ForBoot();

    esp_err_t err = Reset();
    if (err != ESP_OK) {
        return err;
    }

    // 复位之后立刻开收，中间**不要**加等待：ROM 自己的启动横幅在复位后几十毫秒内
    // 就打完了，晚一点开始收就看不到"到底有没有 ROM 在跑"这个最关键的证据了。
    char line[129];         // 一行最多 128 字符，超了就切一刀继续
    size_t line_len = 0;
    int    total    = 0;
    const int64_t deadline = esp_timer_get_time() + (int64_t)duration_ms * 1000;

    while (esp_timer_get_time() < deadline) {
        uint8_t ch = 0;
        if (uart_read_bytes(C61_UART_PORT, &ch, 1, pdMS_TO_TICKS(50)) != 1) {
            continue;
        }
        total++;

        if (ch == '\n' || ch == '\r') {
            if (line_len > 0) {
                line[line_len] = '\0';
                ESP_LOGI(TAG, "C61> %s", line);
                line_len = 0;
            }
            continue;
        }

        // C61 的日志里混着颜色码之类的控制字符，不可打印的统一打成 '.'，看着才不乱
        line[line_len++] = (ch >= 0x20 && ch < 0x7F) ? (char)ch : '.';
        if (line_len >= sizeof(line) - 1) {
            line[line_len] = '\0';
            ESP_LOGI(TAG, "C61> %s", line);
            line_len = 0;
        }
    }

    if (line_len > 0) {
        line[line_len] = '\0';
        ESP_LOGI(TAG, "C61> %s", line);
    }

    online_ = (total > 0);
    ESP_LOGI(TAG, "C61 串口抓取结束：共收到 %d 字节", total);
    return ESP_OK;
}

// ============================================================================
//  C61 内部实现
// ============================================================================

// 安装并配置 C61 串口，幂等
esp_err_t C61Module::EnsureUart() {
    if (uart_ready_) {
        return ESP_OK;
    }

    uart_config_t uart_cfg = {};
    uart_cfg.baud_rate  = C61_UART_BAUD;
    uart_cfg.data_bits  = UART_DATA_8_BITS;
    uart_cfg.parity     = UART_PARITY_DISABLE;
    uart_cfg.stop_bits  = UART_STOP_BITS_1;
    uart_cfg.flow_ctrl  = UART_HW_FLOWCTRL_DISABLE;
    uart_cfg.source_clk = UART_SCLK_DEFAULT;

    esp_err_t err = uart_driver_install(C61_UART_PORT, C61_UART_RX_BUF_SIZE, 0, 0, nullptr, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "安装 C61 UART%d 驱动失败: %s", C61_UART_PORT, esp_err_to_name(err));
        return err;
    }

    err = uart_param_config(C61_UART_PORT, &uart_cfg);
    if (err == ESP_OK) {
        err = uart_set_pin(C61_UART_PORT, C61_UART_P4_TX_PIN, C61_UART_P4_RX_PIN,
                           UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (err != ESP_OK) {
        // 失败要回滚，否则下次再调 install 会报"驱动已安装"
        ESP_LOGE(TAG, "配置 C61 串口失败: %s", esp_err_to_name(err));
        uart_driver_delete(C61_UART_PORT);
        return err;
    }

    uart_ready_ = true;
    ESP_LOGI(TAG, "C61 串口就绪（UART%d，%d bps）", C61_UART_PORT, C61_UART_BAUD);
    return ESP_OK;
}

// EN 拉低再放开，硬复位 C61
esp_err_t C61Module::Reset() {
    // EN 配成 GPIO_MODE_INPUT_OUTPUT：发复位脉冲的同时能回读真实电平。
    // 万一外部把 EN 拉住了（漏焊/短接），日志里当场就能看出来，不用上示波器。
    gpio_config_t en_cfg = {};
    en_cfg.pin_bit_mask = 1ULL << WIFI_EN_PIN;
    en_cfg.mode         = GPIO_MODE_INPUT_OUTPUT;
    en_cfg.pull_up_en   = GPIO_PULLUP_DISABLE;
    en_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    en_cfg.intr_type    = GPIO_INTR_DISABLE;

    esp_err_t err = gpio_config(&en_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "配置 C61 EN(GPIO%d) 失败: %s", WIFI_EN_PIN, esp_err_to_name(err));
        return err;
    }

    gpio_set_level(WIFI_EN_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(C61_EN_FIRST_HOLD_MS));
    const int en_low_readback = gpio_get_level(WIFI_EN_PIN);
    vTaskDelay(pdMS_TO_TICKS(C61_EN_LOW_HOLD_MS - C61_EN_FIRST_HOLD_MS));
    gpio_set_level(WIFI_EN_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(5));
    const int en_high_readback = gpio_get_level(WIFI_EN_PIN);

    ESP_LOGI(TAG, "C61 复位脉冲已发（EN=GPIO%d 拉低 %dms）：拉低时回读=%d，放开后回读=%d",
             WIFI_EN_PIN, C61_EN_LOW_HOLD_MS, en_low_readback, en_high_readback);

    if (en_low_readback != 0 || en_high_readback != 1) {
        ESP_LOGW(TAG, "EN 回读和设置值不一致 —— 检查 EN 是否被外部短接/拉住，或模组 EN 脚虚焊");
    }
    return ESP_OK;
}

// 把 C61 的 IO9 置高，让它复位后跑 flash 里的固件
void C61Module::HoldIo9ForBoot() {
#ifdef C61_IO9_PIN
    // IO9 是"复位时为低就进 UART 下载模式"的 strapping 脚，也就是下载模式和正常启动的开关。
    // 本板 IO9 是 NC（没引出来），只有飞线接到 DEBUG_TP5_PIN(GPIO4) 时才受 P4 控制；
    // 要"P4 直接给 C61 烧固件"时才有必要动它，这一版只做复位 + 看日志，用不到。
    gpio_config_t io9_cfg = {};
    io9_cfg.pin_bit_mask = 1ULL << C61_IO9_PIN;
    io9_cfg.mode         = GPIO_MODE_INPUT_OUTPUT;
    io9_cfg.pull_up_en   = GPIO_PULLUP_DISABLE;
    io9_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io9_cfg.intr_type    = GPIO_INTR_DISABLE;
    if (gpio_config(&io9_cfg) == ESP_OK) {
        gpio_set_level(C61_IO9_PIN, 1);
        ESP_LOGI(TAG, "C61 IO9(GPIO%d) 已置高：下次复位后跑 flash 里的固件", C61_IO9_PIN);
    } else {
        ESP_LOGW(TAG, "配置 C61 IO9(GPIO%d) 失败，按默认启动处理", C61_IO9_PIN);
    }
#else
    // 没飞线时 IO9 悬空，板上默认拉到高（正常启动），这里什么都不用做
    ESP_LOGD(TAG, "未定义 C61_IO9_PIN（本板 IO9 未引出），按默认启动处理");
#endif
}
