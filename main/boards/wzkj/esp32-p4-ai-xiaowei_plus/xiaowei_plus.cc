// ============================================================================
//  XiaoweiPlusBoard —— esp32-p4-ai-xiaowei_plus 板级实现
//
//  结构照抄官方 P4 板（my-esp32-main/main/boards/espressif/esp32-p4-function-ev-board/
//  esp32-p4-function-ev-board.cc）：
//    继承 WifiBoard -> 构造函数里按"分批私有方法"逐组 bring-up -> 覆写板级 getter
//    -> 文件末尾 DECLARE_BOARD。网络部分一行都不用写：WifiBoard 已经实现了
//    GetBoardType/StartNetwork/GetNetwork/GetNetworkStateIcon/SetPowerSaveLevel/
//    GetBoardJson/GetDeviceStatusJson，本类不重写。
//
//  与官方 P4 板的关键差异 —— 无线：
//    官方那块是 P4 + C6（C6 走 SDIO/SPI 做 WiFi 协处理器），本板是 P4 + C61。
//    两者在软件上完全同构：Kconfig 里把协处理器型号选成 C61，esp_hosted 自己起
//    SDIO 链路，esp_wifi_remote 把 esp_wifi_* 转发过去。所以板级 .cc 里同样
//    **一个 esp_hosted 调用都没有**，只有 c61_module 那点复位/探针的诊断代码。
//
//  一个板级目录只允许一个 DECLARE_BOARD（官方规范），本文件是这块板唯一的那个。
// ============================================================================

#include "board.h"
#include "wifi_board.h"
#include "config.h"

#include "c61_module.h"
#include "sy6206.h"
#include "aw86937.h"
#include "lsm6dsv80x.h"
#include "lcd.h"
#include "touch.h"
#include "gc2145.h"
#include "xiaowei_plus_audio.h"

#include "backlight.h"
#include "led.h"
#include "button.h"
#include "application.h"
#include "audio/codecs/no_audio_codec.h"
#include "display/display.h"
#include "display/lcd_display.h"
// XiaoweiUiDisplay : MipiLcdDisplay —— 本板专属的主界面（display/src/ui/ 那套 LCD UI）
#include "display/xiaowei_ui_display.h"
#include "mcp_server.h"
#include "camera_vision.h"

// 相机相册（拍照入册 / 张数查询）。display/src/ui 在 INCLUDE_DIRS 里，所以从
// ui 根往下写就行 —— 和 UI 内部各 app 之间互相引用的写法一致。
#include "apps/camera/photo_store.h"

#include <esp_log.h>
#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <esp_timer.h>
#include <freertos/task.h>
#include <algorithm>
#include <mutex>
#include <vector>

#define TAG "XiaoweiPlusBoard"

// ============================================================================
//  引脚之外的常量
// ============================================================================

// 外设 I2C 总线上的三个从机地址（总线定义见 config.h 第 2 节）
static constexpr uint8_t ADDR_SY6206     = SY6206_ADDR;  // 0x6B  PMIC
static constexpr uint8_t ADDR_LSM6DSV80X = 0x6A;         // IMU（SA0 接地）
static constexpr uint8_t ADDR_AW86937    = 0x5A;         // 振动马达（AD 接地）
static constexpr uint8_t ADDR_AW88082    = 0x34;         // I2S 数字功放

// SY6206 的 I2C 看门狗超时会自动停充，必须周期性喂狗（2s 一次，和 S3 板一致）
static constexpr int PMIC_WDT_PERIOD_US = 2 * 1000 * 1000;

// ---- 麦克风开机自检的参数（见 XiaoweiPlusBoard::SelfTestMicrophone）----
// PDM 时钟 = 输入采样率 × 64。64 来自 ESP32-P4 hal 的 I2S_LL_PDM_BCK_FACTOR
// （components/hal/esp32p4/include/hal/i2s_ll.h），它同时是 PDM 的 CIC 抽取比。
static constexpr int PDM_BCK_FACTOR = 64;
// 使能 PDM 后先等 40ms 再采：LMD2718B261 手册 Wake-up Time ≤35ms，
// 时钟刚恢复的头一小段数据不可信。
static constexpr int MIC_SELFTEST_WARMUP_MS = 40;
// 8 × 10ms = 80ms 采样
static constexpr int MIC_SELFTEST_CHUNKS = 8;
// 交流峰值超过它就算"拾到声音"（厂商参考工程用的也是 64/32767 这个阈值）
static constexpr int MIC_SELFTEST_SIGNAL_THRESHOLD = 64;
// 直流贴着一端：说明 DATA 脚没被麦克风驱动（PDM 静止时是 ~50% 占空比，解调后应接近 0）
static constexpr int MIC_SELFTEST_STUCK_THRESHOLD = 30000;

// 屏幕起不来时的兜底显示后端。Application::Initialize() 会**无条件**调用
// display->SetupUI()，所以 GetDisplay() 绝不能返回 nullptr；CO5300 初始化失败时
// 就退到 this，让网络和音频先跑起来（总比整机起不来强）。
class XiaoweiPlusHeadlessDisplay final : public Display {
private:
    std::timed_mutex mutex_;

    bool Lock(int timeout_ms = 0) override {
        if (timeout_ms <= 0) {
            return mutex_.try_lock();
        }
        return mutex_.try_lock_for(std::chrono::milliseconds(timeout_ms));
    }

    void Unlock() override { mutex_.unlock(); }
};

// 板上没有状态指示灯，但 Application 在 VAD 状态变化时会无条件通知 LED。
// 用空实现保持这个可选硬件接口安全，避免 nullptr 导致异常重启。
class XiaoweiPlusNoopLed final : public Led {
public:
    void OnStateChanged() override {}
};

// ============================================================================
//  平台负载锁回调
//
//  aw86937.cc（以及本工程其它振动驱动）按"平台提供负载锁"的约定声明了这两个符号，
//  用它在马达启动前申请电流余量、结束后归还。本板没有电源域仲裁硬件，这里给空实现。
//  必须定义在**板级** .cc 里：只有选中本板时 aw86937.cc 才会被编进来，符号才配得上对。
// ============================================================================
extern "C" {

void board_load_release(int load_type) {
    (void)load_type;
}

bool board_load_acquire(int load_type, int timeout) {
    (void)load_type;
    (void)timeout;
    return true;
}

}  // extern "C"

// ============================================================================
//  板级类
// ============================================================================

/**
 * @class XiaoweiPlusBoard
 * @brief ESP32-P4 + ESP32-C61 小未 Plus 板级类
 */
class XiaoweiPlusBoard : public WifiBoard {
private:
    // 外设 I2C 总线：全板只建一条，SY6206 / LSM6DSV80X / AW86937 共用。
    // 分开建两条总线在这里没有任何好处，反而会各自持有一份 GPIO 矩阵配置。
    i2c_master_bus_handle_t periph_i2c_bus_ = nullptr;

    Sy6206*            pmic_ = nullptr;            // 电源管理（充放电 + 电量）
    esp_timer_handle_t pmic_wdt_timer_ = nullptr;  // PMIC 看门狗喂狗定时器

    // 外设对象都由板级持有，和 pmic_ / lcd_ / touch_ 一样。UI 是纯 C，
    // 通过文件末尾的 extern "C" 原语访问（见 HapticPlayWave / ImuRead）。
    Lsm6dsv80x*        imu_ = nullptr;             // 六轴 IMU（探测失败时为 nullptr）
    Aw86937*           haptic_ = nullptr;          // 振动马达（异步初始化；失败时为 nullptr）

    // 芯片 RAM 里当前装的是哪一号波形。所有播放都走 HapticPlayWave()，所以这个
    // 缓存不会失效 —— 换波形才下载，滑条连续触发只有几个寄存器的写。
    int                haptic_loaded_wave_ = -1;
    // 强度百分比：UI 可能在任何时刻设置，马达还没就绪就先记住，Init 完再应用。
    int                haptic_intensity_ = 80;

    i2c_master_dev_handle_t amp_ = nullptr;        // AW88082 控制句柄

    C61Module          c61_;                       // 无线协处理器（复位 + UART 探针）

    // ---- 显示与触摸 ----
    // 声明顺序 = 构造顺序，这里不能随意调换：backlight_ 存的是 lcd_ 的地址，
    // display_ 存的是 headless_display_ 的地址，被依赖的必须先声明。
    Lcd                       lcd_;                // CO5300 466x466 AMOLED（J4，MIPI-DSI）
    LcdTouch                  touch_;              // CST820B 触摸（和摄像头共用 J4 那路 I2C）
    Co5300Backlight           backlight_{&lcd_};   // 面板亮度 -> 框架 Backlight 抽象
    XiaoweiPlusHeadlessDisplay headless_display_;  // 屏起不来时的安全显示后端
    Display*                  display_ = &headless_display_;  // 实际用的后端（见 InitializeLcd）
    XiaoweiPlusNoopLed noop_led_;                  // 未装状态灯时的安全 LED 后端

    Button             boot_button_;               // BOOT 键（GPIO35）

    // ---- 1. 外设 I2C 总线 ----
    // 失败只记日志：总线没了意味着 PMIC/IMU/马达都用不了，但屏幕和 C61 不受影响，
    // 不该让整台设备起不来。
    void InitializePeriphI2c() {
        ESP_LOGI(TAG, "初始化外设 I2C：SDA=GPIO%d SCL=GPIO%d port=%d",
                 PERIPH_I2C_SDA_PIN, PERIPH_I2C_SCL_PIN, PERIPH_I2C_PORT);

        i2c_master_bus_config_t bus_cfg = {};
        bus_cfg.i2c_port = PERIPH_I2C_PORT;
        bus_cfg.sda_io_num = (gpio_num_t)PERIPH_I2C_SDA_PIN;
        bus_cfg.scl_io_num = (gpio_num_t)PERIPH_I2C_SCL_PIN;
        bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
        bus_cfg.glitch_ignore_cnt = 7;
        bus_cfg.flags.enable_internal_pullup = true;

        esp_err_t err = i2c_new_master_bus(&bus_cfg, &periph_i2c_bus_);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "外设 I2C 总线初始化失败: %s —— PMIC/IMU/马达全部不可用",
                     esp_err_to_name(err));
            periph_i2c_bus_ = nullptr;
            return;
        }

        // 顺手扫一遍，日志里能直接看到"哪颗芯片没焊/没应答"
        BusScan();
    }

    // 总线扫描：i2c_master_probe 是非致命接口，探测某个地址没应答只返回错误码。
    // 先探到再实例化驱动 —— 这是避免"芯片不在线 -> 读寄存器 abort -> 重启"的关键，
    // 因为 I2cDevice 基类的 ReadReg/WriteReg 内部是 ESP_ERROR_CHECK。
    void BusScan() {
        ESP_LOGI(TAG, "扫描外设 I2C 总线...");
        int found = 0;
        for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
            if (i2c_master_probe(periph_i2c_bus_, addr, 50) != ESP_OK) {
                continue;
            }
            const char* name = "未知设备";
            if (addr == ADDR_SY6206) {
                name = "SY6206 PMIC";
            } else if (addr == ADDR_LSM6DSV80X) {
                name = "LSM6DSV80X IMU";
            } else if (addr == ADDR_AW86937) {
                name = "AW86937 Haptic";
            }
            ESP_LOGI(TAG, "  [0x%02X] %s", addr, name);
            found++;
        }
        ESP_LOGI(TAG, "扫描结束，共发现 %d 个设备", found);
    }

    // ---- 2. PMIC（SY6206）----
    // 必须最先通电：它是整板的电源来源，而且它的 I2C 看门狗超时会复位 SoC。
    void InitializePmic() {
        if (periph_i2c_bus_ == nullptr) {
            ESP_LOGW(TAG, "I2C 总线不可用，跳过 PMIC 初始化");
            return;
        }
        if (i2c_master_probe(periph_i2c_bus_, ADDR_SY6206, 50) != ESP_OK) {
            ESP_LOGW(TAG, "SY6206(0x%02X) 无应答，跳过电源管理（电量显示会不可用）", ADDR_SY6206);
            return;
        }

        pmic_ = new Sy6206(periph_i2c_bus_, ADDR_SY6206);

        // 与已验证可充电的 lcd 工程保持完全一致：不改写 REG00 输入限流。
        ESP_LOGI(TAG, "SY6206 配置充电（lcd 流程）：CV=4200mV CC=500mA");
        esp_err_t charge_err = pmic_->SetChargeVoltageMv(4200);
        if (charge_err != ESP_OK) ESP_LOGE(TAG, "设置充电电压失败: %s", esp_err_to_name(charge_err));
        charge_err = pmic_->SetChargeCurrentMa(500);
        if (charge_err != ESP_OK) ESP_LOGE(TAG, "设置充电电流失败: %s", esp_err_to_name(charge_err));
        charge_err = pmic_->SetChargeEnable(true);
        if (charge_err != ESP_OK) ESP_LOGE(TAG, "开启充电失败: %s", esp_err_to_name(charge_err));
        charge_err = pmic_->EnableAdc(true); // lcd 工程顺序
        if (charge_err != ESP_OK) ESP_LOGE(TAG, "开启 ADC 失败: %s", esp_err_to_name(charge_err));
        // NTC 引脚悬空：与 lcd 工程一致，在充电和 ADC 配置后禁用 NTC 检查。
        charge_err = pmic_->DisableNtcCheck();
        if (charge_err != ESP_OK) ESP_LOGE(TAG, "禁用 NTC 检测失败: %s", esp_err_to_name(charge_err));
        charge_err = pmic_->SetChargeEnable(true);
        ESP_LOGI(TAG, "SY6206 充电使能确认: %s", esp_err_to_name(charge_err));
        pmic_->LogChargeDiagnostics();

        // 看门狗超时会自动停充，周期性喂狗；同时轮询中断标志（REG20/REG22 读清零，
        // 读取即清中断并恢复 INT 引脚），解析触发原因：插拔电源 / QON / 过温 / 过压等。
        const esp_timer_create_args_t wdt_args = {
            .callback = [](void* arg) {
                Sy6206* pmic = static_cast<Sy6206*>(arg);
                pmic->FeedWatchdog();     // 核心逻辑一：定期喂狗
                pmic->HandleInterrupt();  // 核心逻辑二：读中断原因并解析
            },
            .arg = pmic_,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "pmic_wdt",
        };
        esp_err_t err = esp_timer_create(&wdt_args, &pmic_wdt_timer_);
        if (err == ESP_OK) {
            err = esp_timer_start_periodic(pmic_wdt_timer_, PMIC_WDT_PERIOD_US);
        }
        if (err != ESP_OK) {
            // 定时器建不起来不是致命的：只是充电动不动被看门狗掐断，日志里会看得到
            ESP_LOGE(TAG, "PMIC 看门狗定时器启动失败: %s（充电可能被看门狗中断）",
                     esp_err_to_name(err));
            pmic_wdt_timer_ = nullptr;
        }

        uint8_t chip_id = 0, rev = 0;
        if (pmic_->ReadChipId(&chip_id, &rev) == ESP_OK) {
            ESP_LOGI(TAG, "SY6206 就绪：chip_id=%u rev=%u", chip_id, rev);
        }
    }

    // ---- 3. IMU（LSM6DSV80X）----
    // 不上电也能跑：初始化失败只体现在 xiaowei_imu_get() 一直返回 false，
    // IMU 测试页会显示 OFFLINE，其它外设一律不受影响。
    void InitializeImu() {
        if (periph_i2c_bus_ == nullptr) {
            return;
        }
        // 先探一下，确认器件真的在线再建对象 —— 没应答时不必让驱动反复失败。
        if (i2c_master_probe(periph_i2c_bus_, ADDR_LSM6DSV80X, 50) != ESP_OK) {
            ESP_LOGW(TAG, "LSM6DSV80X(0x%02X) 无应答，IMU 测试页将显示 OFFLINE",
                     ADDR_LSM6DSV80X);
            return;
        }
        imu_ = new Lsm6dsv80x(periph_i2c_bus_, ADDR_LSM6DSV80X);
        if (imu_->Init() != ESP_OK) {
            ESP_LOGW(TAG, "LSM6DSV80X 初始化失败，IMU 测试页将显示 OFFLINE");
            delete imu_;
            imu_ = nullptr;
            return;
        }
        ESP_LOGI(TAG, "LSM6DSV80X 就绪");
    }

    // ---- 4. 振动马达（AW86937）----
    // 注意：扫描时扫不到 0x5A 是正常的 —— 芯片此刻还处在 RSTN 复位态（Power-Down），
    // RSTN 是在 Aw86937::Init() 里才释放的。所以这里**不**用扫描结果做门槛。
    //
    // Init() 里有 F0 校准 + RAM 波形下载，要数秒，不能放在板级构造里同步跑，
    // 否则屏幕和网络都要等它。交给独立任务；就绪前 UI 调播放接口只是空操作。
    void InitializeHaptic() {
        if (periph_i2c_bus_ == nullptr) {
            return;
        }
        if (xTaskCreate(HapticInitTask, "haptic_init", 8192, this, 4, nullptr) != pdPASS) {
            ESP_LOGW(TAG, "AW86937 初始化任务创建失败，马达不可用");
        }
    }

    static void HapticInitTask(void* arg) {
        auto* self = static_cast<XiaoweiPlusBoard*>(arg);
        auto* haptic = new Aw86937(self->periph_i2c_bus_, ADDR_AW86937);
        if (haptic->Init() != ESP_OK) {
            ESP_LOGW(TAG, "AW86937 初始化失败，马达不可用");
            delete haptic;
            vTaskDelete(nullptr);
            return;
        }
        haptic->SetIntensity((uint8_t)self->haptic_intensity_);
        // 指针最后赋值：UI 看到非空时，Init 和强度都已经应用完了。
        self->haptic_ = haptic;
        ESP_LOGI(TAG, "AW86937 就绪：F0=%d Hz", haptic->GetF0Hz());
        vTaskDelete(nullptr);
    }

    void InitializeAudioAmp() {
        if (periph_i2c_bus_ == nullptr ||
            i2c_master_probe(periph_i2c_bus_, ADDR_AW88082, 50) != ESP_OK) {
            ESP_LOGW(TAG, "AW88082(0x%02X) 无应答，音频功放未初始化", ADDR_AW88082);
            return;
        }
        const i2c_device_config_t cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = ADDR_AW88082,
            .scl_speed_hz = 100000,
        };
        if (i2c_master_bus_add_device(periph_i2c_bus_, &cfg, &amp_) != ESP_OK) {
            ESP_LOGW(TAG, "添加 AW88082 I2C 设备失败");
            amp_ = nullptr;
            return;
        }

        // 这里不读 ID 寄存器：XiaoweiPlusAudioCodec::AmpConfigure() 会读，并且
        // 会把它和手册值 0x2116 比对后打出来。板上读两遍没有意义。
        //
        // 无论冷启动还是软复位，都先把 AW88082 明确拉回 PWDN + HMUTE。
        // 软复位不会保证外部功放寄存器复位；若它保留上次的 0x2246，PA/PLL 会在
        // 音频服务启动前持续工作并快速发热。SYSCTRL(0x04)=0x0101。
        const uint8_t amp_power_down[] = {0x04, 0x01, 0x01};
        esp_err_t power_down_err = i2c_master_transmit(amp_, amp_power_down,
                                                       sizeof(amp_power_down), 100);
        if (power_down_err != ESP_OK) {
            ESP_LOGW(TAG, "AW88082 开机强制关断失败: %s", esp_err_to_name(power_down_err));
        } else {
            uint8_t sysctrl_reg = 0x04;
            uint8_t sysctrl_rx[2] = {};
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdangling-pointer"
            esp_err_t readback_err = i2c_master_transmit_receive(
                amp_, &sysctrl_reg, 1, sysctrl_rx, sizeof(sysctrl_rx), 100);
#pragma GCC diagnostic pop
            uint16_t sysctrl = ((uint16_t)sysctrl_rx[0] << 8) | sysctrl_rx[1];
            if (readback_err == ESP_OK && sysctrl == 0x0101) {
                ESP_LOGI(TAG, "AW88082 已确认关断：SYSCTRL=0x%04X (PWDN+HMUTE)", sysctrl);
            } else {
                ESP_LOGE(TAG, "AW88082 关断未生效：read=%s SYSCTRL=0x%04X（期望 0x0101）",
                         esp_err_to_name(readback_err), sysctrl);
            }
        }
        //
        // 功放的整套上电握手（I2SCTRL1/3、SYSCTRL2、两步上电 + 轮询 SYSST 等 PLL）
        // 搬到了 XiaoweiPlusAudioCodec::EnableOutput()。原因是那个握手**依赖 BCK/WCK
        // 已经在跑**（AW88082 的 PLL 拿 BCK 当参考时钟），而本函数跑在板级构造期 ——
        // 那会儿 GetAudioCodec() 还没被调过，TX 通道根本不存在，时钟是停的，
        // 写下去只会让 PLL 永远锁不上。
        //
        // 原来的做法是这里一次性写 0x2246（跳过等 PLL + 等开关两步），
        // 也就是"喇叭不响"的原因之一。
        ESP_LOGI(TAG, "AW88082 I2C 就绪（完整配置推迟到首次放音）");
    }

    // ---- 5. 显示与触摸（CO5300 MIPI-DSI + CST820B）----
    // 屏起不来不是致命的：GetDisplay() 会继续返回无 UI 后端，网络/音频/电量照常。
    // 但触摸只有在屏起来之后才有意义（它要挂到 LVGL 上），所以整段绑在屏的成功上。
    void InitializeLcd() {
        esp_err_t err = lcd_.Init();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "CO5300 面板初始化失败 (%s)，退回无 UI 显示后端", esp_err_to_name(err));
            return;
        }

        // MipiLcdDisplay 内部会做 lv_init + lvgl_port_init + lvgl_port_add_disp_dsi，
        // 构造完 LVGL 任务就已经在跑、默认 display 也就位了。
        // 用 XiaoweiUiDisplay（不是 MipiLcdDisplay）：前者只是重写了 SetupUI() 和几个
        // setter，把界面换成 lcd 工程那套主 UI。构造、LVGL 起机、锁、析构全部原样继承，
        // 所以这里的参数和所有权语义与原来完全一致。
        auto* lcd_display = new XiaoweiUiDisplay(lcd_.panel_io(), lcd_.panel(), DISPLAY_WIDTH,
                                                 DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
                                                 DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
        // 立刻交出面板句柄的所有权：MipiLcdDisplay 的析构会 esp_lcd_panel_del + io_del，
        // 本类再删一次就是 double free（见 lcd.h 的 HandOffPanel 说明）。
        // 必须在下面那个失败分支之前调 —— 那个分支会 delete 掉这个显示对象。
        lcd_.HandOffPanel();

        // 分配失败会抛 bad_alloc（new 不失败就一定是非空），这里只防驱动内部建 display 失败
        if (lv_display_get_default() == nullptr) {
            ESP_LOGE(TAG, "LVGL display 未创建成功，退回无 UI 显示后端");
            lcd_.ForgetPanel();  // 马上要删掉持有面板的对象了，先把引用清掉
            delete lcd_display;
            return;
        }
        display_ = lcd_display;

        if (touch_.Init() != ESP_OK) {
            ESP_LOGW(TAG, "CST820B 触摸初始化失败，屏幕可用但没有触摸输入");
            return;
        }
        // 坐标 1:1 —— 本工程的 LVGL 屏就是 466x466，不需要参考工程里那套 240->466 缩放
        if (touch_.AttachToLvgl(lv_display_get_default()) != ESP_OK) {
            ESP_LOGW(TAG, "触摸注册到 LVGL 失败");
        }
    }

    // ---- 6. 按键 ----
    void InitializeButtons() {
        // BOOT 键：短按唤醒 / 切换对话
        //   启动期短按 -> 进入配网
        //   运行期短按 -> ToggleChatState()，空闲时开始对话，speaking 时打断当前回复
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });

        // QON（PMIC_QON_BUTTON_PIN = GPIO18）这一版**故意不注册**，原因有三条，都不是懒得写：
        //   1) P4 只支持 EXT1 唤醒，没有 EXT0 —— SOC_PM_SUPPORT_EXT0_WAKEUP 未定义，
        //      esp_sleep_enable_ext0_wakeup() 在 P4 上根本编不出来；照抄 S3 板那句会编译失败。
        //   2) P4 的 LP/RTC GPIO 只有 GPIO0~15（SOC_RTCIO_PIN_COUNT = 16），QON 挂在 GPIO18，
        //      不在其中，做不了 EXT1 唤醒源。
        //   3) 本板 QON 是 SY6206 自己的电源键：长按硬关机、短按开机，PMIC 硬件语义就完成了，
        //      本来不需要 SoC 参与。
        // 以后真要在固件里处理它，只有两条路：把 QON 挪到 LP GPIO 走 EXT1，
        // 或者通过 I2C 让 PMIC 执行关机/深睡。
        ESP_LOGI(TAG, "按键就绪（BOOT=GPIO%d；QON=GPIO%d 由 PMIC 硬件处理，固件未接管）",
                 BOOT_BUTTON_GPIO, PMIC_QON_BUTTON_PIN);
    }

    // ---- 7. MCP 工具 ----
    void InitializeTools() {
        auto& mcp_server = McpServer::GetInstance();
        mcp_server.AddTool("self.disp.network", "重新配网",
                           PropertyList(),
                           [this](const PropertyList&) -> ReturnValue {
                               EnterWifiConfigMode();
                               return true;
                           });

        // 拍照。拍到的照片进相册（和手动快门、AI 绘画那张混在一起，最多 10 张）。
        //
        // ⚠ 这段描述就是**触发条件**本身，别按"够用就行"去精简。
        //   模型选不选这个工具，完全看描述里有没有它认得出的触发语：
        //   原来的写法只说了"拍照并存进相册"，结果用户说"看看我在干什么"
        //   "看看桌面上有什么"时，模型去调 self.get_device_status 了
        //   （那支工具的描述里写了 Use this tool for: ... ），
        //   说"我手上是什么"时干脆一个工具都不调，直接回"我看不到"。
        //   服务端的系统提示词里没有任何一句提到摄像头，所以触发语只能写在这里。
        //   老固件那支相机工具的英文描述（You MUST call this tool when user asks
        //   to 'take a photo', 'what do you see'...）就是干这个用的，照这个思路写。
        //
        // 返回的是 JSON 文本而不是图片：服务端 mcp_handler.py 收工具结果时只取
        // content[0]["text"]，MCP 的 image 内容项在那里会被整条丢掉，回图片等于
        // 白拍。要图片得另开一条上行通道（比如自己发 display 类的消息）。
        //
        // 画面内容走 vision 字段：拍完照把那张图上传到服务器视觉分析接口
        // （地址是服务器在 MCP initialize 里下发的，见 camera_vision.cc），
        // 把服务器返回的描述塞进工具结果，模型就能照着念。
        //
        // 回调跑在 Application 主任务上（mcp_server.cc 的 DoToolCall 走
        // app.Schedule），栈只有 10KB，所以这里不能有大局部变量 ——
        // 拷贝到 PSRAM 那步在 photo_store.c 里，本地只留一个指针；
        // JPEG 缓冲在 camera_vision.cc 里也是堆上分配，不放栈上。
        mcp_server.AddTool("self.camera.take_photo",
                           "用摄像头拍一张照片：照片存进设备相册，同时上传给服务器的视觉分析接口，"
                           "返回画面内容的文字描述。\n"
                           "**必须**在用户提出下面这些请求时调用本工具：\n"
                           "1. 要求拍照：拍张照、拍个照片、给我拍一张、拍照\n"
                           "2. 要求看现场：看看我在干什么、看看桌面上有什么、我手里/手上是什么、"
                           "你看到了什么、帮我看看这是什么\n"
                           "参数 question：你想从这张照片里知道什么，用中文写一句话，"
                           "比如\"用户在做什么\"、\"用户手里拿的是什么东西\"。\n"
                           "返回 {\"success\": 是否拍成功, \"album_count\": 相册现有张数, "
                           "\"vision\": 画面内容的描述}。\n"
                           "照片存在设备相册（最近 10 张，用户可在相册页左右翻看）。\n"
                           "拿到 vision 后**直接用中文把里面的内容讲给用户听**（可以口语化、简短），"
                           "不要再说自己看不到。vision 是空的时候说明这次没拿到分析，"
                           "那就只说照片已经存进相册了，不要编造画面里有什么。",
                           PropertyList({
                               Property("question", kPropertyTypeString,
                                        std::string("这张照片里有什么？用一句话描述。")),
                           }),
                           [](const PropertyList& properties) -> ReturnValue {
                               const bool ok = xiaowei_photo_capture(XIAOWEI_PHOTO_AI_CAM);

                               // 上传的就是刚存进相册的那张（0 = 最新）。get() 给的是一份
                               // 描述符浅拷，pixels 仍归相册所有，**不能 free**。
                               std::string vision;
                               if (ok) {
                                   xiaowei_photo_t photo;
                                   if (xiaowei_photo_get(0, &photo)) {
                                       vision = xiaowei_vision_explain(
                                           photo.pixels, photo.w, photo.h, photo.stride,
                                           properties["question"].value<std::string>());
                                   }
                               }

                               cJSON* json = cJSON_CreateObject();
                               cJSON_AddBoolToObject(json, "success", ok);
                               cJSON_AddNumberToObject(json, "album_count", xiaowei_photo_count());
                               cJSON_AddStringToObject(json, "vision", vision.c_str());
                               return json;
                           });
    }

    // ---- 8. 麦克风开机自检 ----
    // 把 PDM 通道开起来读 80ms，打印峰值 / 直流 / 交流峰值 / 非零采样数。
    // 目的是让**一条开机日志**就能把几种情况分开：麦克风根本没出数据（PDM 时钟/1V8
    // 供电/DATA 脚）、DATA 脚是死的（一直贴着一端）、麦活着但没拾到声音 ——
    // 而不是一上来就怀疑功放或唤醒词。思路搬自厂商参考工程 board_audio.c 的麦克风自检。
    //
    // 放在**板级**而不是 NoAudioCodecSimplexPdm 里：那个是框架共享文件，板级诊断不该
    // 污染它（S3 LOW 板走 Es8311AudioCodec，根本不经过 PDM 那条路径）。
    //
    // ⚠ 如果日志停在"开始麦克风自检"这一行不动了，这本身就是结论：PDM RX 读不出数据
    //   （NoAudioCodecSimplexPdm::Read 内部是 portMAX_DELAY，没有超时）。
    void SelfTestMicrophone() {
        XiaoweiPlusAudioCodec& plus = AudioCodecInstance();
        AudioCodec* codec = &plus;
        const int sample_rate = codec->input_sample_rate();
        if (sample_rate <= 0) {
            ESP_LOGW(TAG, "麦克风自检跳过：输入采样率无效 (%d)", sample_rate);
            return;
        }

        // 10ms 一块（48kHz 时 480 个采样）。用 std::vector 而不是栈数组：板级构造跑在
        // main 任务上，栈本来就不宽裕。
        const int chunk_samples = sample_rate / 100;
        std::vector<int16_t> chunk(chunk_samples, 0);
        int peak = 0;  // 增益**之后**的峰值，也就是实际喂给 AFE 的波形
        size_t nonzero = 0, total = 0;
        int failed = 0;

        ESP_LOGI(TAG, "开始麦克风自检：PDM CLK=GPIO%d DATA=GPIO%d，%dHz（PDM 时钟≈%.2fMHz）",
                 AUDIO_MIC_CLK_PIN, AUDIO_MIC_DATA_PIN, sample_rate,
                 sample_rate * (double)PDM_BCK_FACTOR / 1000000.0);
        // 采的是哪一侧是排查麦克风问题的第一手信息：PDM 麦的 LR 选择脚决定数据落在
        // 哪个 slot，采错侧只能拿到恒定直流。探测的左右 RMS 在上面 "PDM 声道探测"
        // 那行日志里，两边一对比就知道是"选错了"还是"两边都没数据"。
        ESP_LOGI(TAG, "本机采的是%s声道（构造期探测结果）",
                 plus.mic_uses_right_channel() ? "右" : "左");

        codec->EnableInput(true);
        vTaskDelay(pdMS_TO_TICKS(MIC_SELFTEST_WARMUP_MS));
        for (int i = 0; i < MIC_SELFTEST_CHUNKS; i++) {
            std::fill(chunk.begin(), chunk.end(), 0);
            // InputData 只告诉我们"读到没读到"，读满与否自己按 chunk 大小算
            if (!codec->InputData(chunk)) {
                failed++;
                continue;
            }
            for (int16_t s : chunk) {
                // 这里量的是 Read() 的返回值，也就是**加过增益、减过直流**之后
                // 真正喂给 AFE 的波形；和下面 raw_* 那两个"增益前"的数不是一个视角。
                const int a = (s < 0) ? -(int)s : (int)s;
                if (a > peak) {
                    peak = a;
                }
                if (s != 0) {
                    nonzero++;
                }
                total++;
            }
        }
        // 必须在 EnableInput(false) 之前读：那个覆写会把电平归零（免得 UI 显示残留值）。
        // 这三个数都由 Read() 在**去直流之后、加增益之前**测出，和 audio 页电平表同源。
        const int raw_rms = plus.InputLevelRms();
        const int raw_peak = plus.InputLevelPeak();
        const int dc = plus.InputDcOffset();

        codec->EnableInput(false);  // 交还给 AudioService，它自己会按需使能

        if (total == 0) {
            ESP_LOGE(TAG, "麦克风自检：一个采样都没读到（%d/%d 块读失败）—— PDM 接收通道没起来",
                     failed, MIC_SELFTEST_CHUNKS);
            return;
        }

        // 判定一律用**增益前**的 raw_*，和 MIC_SELFTEST_* 那几个阈值同一量纲。
        // 判定里不要再出现增益后的 peak —— 阈值是按麦克风原始输出定的，
        // 混进 8× 增益后，"贴着阈值"这种边界的结论会随 MIC_GAIN 漂移。
        const char* verdict;
        if (raw_rms == 0 && raw_peak == 0) {
            // 电平读数为 0 = Read() 一次都没成功返回过数据。这比"没拾到声音"硬得多：
            // 通道根本没出数据，先查 GPIO 和供电，别在增益上打转。
            verdict = "PDM 接收通道没出过数据 —— 查 CLK/DATA GPIO 与 1V8 供电";
        } else if ((dc >= MIC_SELFTEST_STUCK_THRESHOLD || dc <= -MIC_SELFTEST_STUCK_THRESHOLD) &&
                   raw_peak < MIC_SELFTEST_SIGNAL_THRESHOLD) {
            // DATA 线不动时，PDM 解调器输出的不是 0 而是一个恒定值：减掉直流后交流
            // 几乎为空，而那个直流本身大得离谱。两个条件一起判，才不会把"很吵的房间"
            // 误判成"线是死的"。
            verdict = "DATA 脚是死的（只解调出一个恒定值）—— 查 PDM 时钟/1V8 供电/焊接";
        } else if (raw_peak < MIC_SELFTEST_SIGNAL_THRESHOLD) {
            verdict = "交流分量很小 —— 对着麦克风说话再看一次；若两侧 RMS 都是几十量级那是环境静音，"
                      "若某一侧明显高出一截则说明声道探测选对了";
        } else {
            verdict = "有信号";
        }
        ESP_LOGI(TAG, "麦克风自检：增益前 交流RMS=%d 峰值=%d（直流偏置=%d）；增益后 峰值=%d，"
                      "非零采样=%u/%u（%s）",
                 raw_rms, raw_peak, dc, peak,
                 (unsigned)nonzero, (unsigned)total, verdict);
    }

    // ---- 音频底层单例 ----
    //
    // 做成成员函数里的 static（而不是 GetAudioCodec() 里的 static）是为了让板级自检
    // 和 UI 的电平表都能拿到**同一个**实例 —— 电平存在对象里，换个实例读到的永远是 0。
    // C++11 起函数级 static 的初始化有 guard 保护，多任务并发首调不会建出两份。
    //
    // amp_ 在构造时传入：InitializeAudioAmp() 在构造函数里排在 SelfTestMicrophone()
    // 之前，而 GetAudioCodec() 更是远在其后（Application::Initialize() 里），
    // 所以走到这里 amp_ 一定已经就绪（失败时是 nullptr，codec 会优雅降级）。
    XiaoweiPlusAudioCodec& AudioCodecInstance() {
        static XiaoweiPlusAudioCodec audio_codec(
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_AMP_BCK_PIN, AUDIO_AMP_WCK_PIN, AUDIO_AMP_DATAI_PIN,
            AUDIO_MIC_CLK_PIN, AUDIO_MIC_DATA_PIN,
            amp_);
        return audio_codec;
    }

public:
    XiaoweiPlusBoard() : boot_button_((gpio_num_t)BOOT_BUTTON_GPIO) {
        // 顺序有讲究：外设 I2C 总线必须最先建（PMIC/IMU/马达都挂在它上面），
        // PMIC 紧跟其后（整板电源 + 看门狗，不能拖），其余外设谁先谁后都行。
        InitializePeriphI2c();
        InitializePmic();
        InitializeImu();
        InitializeAudioAmp();
        InitializeHaptic();
        InitializeLcd();         // 屏 + 触摸（非致命：失败则继续用无 UI 显示后端）
        if (xiaowei_camera_i2c_bus() != nullptr && !xiaowei_camera_init()) {
            ESP_LOGW(TAG, "GC2145 摄像头初始化失败，摄像头页面不可用");
        }
        c61_.Init();             // 无线协处理器：复位 + 抓启动日志（非致命）
        InitializeButtons();
        InitializeTools();
        // 亮度从 Settings 读回（默认 75），这样断电前调过的亮度不会丢。
        // 屏没起来时 GetBacklight() 返回 nullptr，判空后再调。
        if (auto* backlight = GetBacklight()) {
            backlight->RestoreBrightness();
        }
        // 麦克风开机自检（~120ms）：只打印诊断，不改变状态 —— 自检完会把输入使能关掉，
        // 和没自检过一样，AudioService 之后照常按需 EnableInput(true)。
        // 放最后：别拖慢屏幕和网络。
        SelfTestMicrophone();
    }

    ~XiaoweiPlusBoard() override {
        if (pmic_wdt_timer_ != nullptr) {
            esp_timer_stop(pmic_wdt_timer_);
            esp_timer_delete(pmic_wdt_timer_);
            pmic_wdt_timer_ = nullptr;
        }
        // 先删显示再让值成员自动析构：display_ 用的是 lcd_ 的面板句柄，而 lcd_/touch_
        // 作为成员会在析构体跑完之后才析构，顺序刚好对（先撤 UI，再撤面板和触摸）。
        if (display_ != &headless_display_) {
            // ForgetPanel 必须在这行**之前**：delete 会把面板句柄一起删掉，而亮度渐变的
            // 定时器要到 backlight_ 成员析构时才停 —— 中间那一小段必须以"面板不可用"
            // 而不是"野指针"的形式被 SetBrightness 看到。
            lcd_.ForgetPanel();
            delete display_;
        }
        display_ = &headless_display_;
        // 先删驱动再删总线，否则设备句柄挂在一条已经不存在总线上
        delete haptic_;
        haptic_ = nullptr;
        delete imu_;
        imu_ = nullptr;
        delete pmic_;
        pmic_ = nullptr;
    }

    // ========================================================================
    //  UI 用的外设原语
    //
    //  这里只做「怎么驱动」这一件事，不含 UI 语义（哪个模式对应哪号波形、
    //  测试页按什么顺序播）—— 那些在 display/src/ui/apps/legacy/legacy_app.c。
    //  纯 C 的 UI 通过文件末尾的 extern "C" 符号调到这几个方法。
    // ========================================================================

    // 播放 RAM 波形库里的第 index 号（1..7，映射见 aw86937.cc 的 LoadWaveform）。
    // 芯片里换过波形才重新下载；同一号连续触发只有几个寄存器写。
    bool HapticPlayWave(int index) {
        if (haptic_ == nullptr || index < 1 || index > 7) {
            return false;
        }
        if (index != haptic_loaded_wave_) {
            // 下载会覆盖 RAM，先让当前这次振动彻底停下，避免和播放任务的 I2C 交错。
            if (haptic_->IsPlaying()) {
                haptic_->Stop();
                for (int waited = 0; waited < 200 && haptic_->IsPlaying(); waited += 2) {
                    vTaskDelay(pdMS_TO_TICKS(2));
                }
            }
            if (haptic_->LoadWaveform((uint8_t)index) != ESP_OK) {
                haptic_loaded_wave_ = -1;  // 下载失败，缓存不可信
                return false;
            }
            haptic_loaded_wave_ = index;
        }
        // RAM 模式纯触发：WAVCFG/GAIN/PLAYGO，微秒级。
        return haptic_->PlayWave(1, 1) == ESP_OK;
    }

    void HapticSetIntensity(int percent) {
        if (percent < 0) percent = 0;
        if (percent > 100) percent = 100;
        haptic_intensity_ = percent;
        if (haptic_ != nullptr) {
            haptic_->SetIntensity((uint8_t)percent);
        }
    }

    bool HapticIsPlaying() const {
        return haptic_ != nullptr && haptic_->IsPlaying();
    }

    // 一次采样。驱动给的是 mg / mdps，UI 契约要 g / deg/s。
    bool ImuRead(float* ax, float* ay, float* az,
                 float* gx, float* gy, float* gz, float* temp) {
        if (imu_ == nullptr) {
            return false;
        }
        float temp_c = 0.0f;
        float acc_mg[3] = {0};
        float gyr_mdps[3] = {0};
        if (imu_->ReadData(&temp_c, acc_mg, gyr_mdps) != ESP_OK) {
            return false;
        }
        if (ax != nullptr) *ax = acc_mg[0] / 1000.0f;
        if (ay != nullptr) *ay = acc_mg[1] / 1000.0f;
        if (az != nullptr) *az = acc_mg[2] / 1000.0f;
        if (gx != nullptr) *gx = gyr_mdps[0] / 1000.0f;
        if (gy != nullptr) *gy = gyr_mdps[1] / 1000.0f;
        if (gz != nullptr) *gz = gyr_mdps[2] / 1000.0f;
        if (temp != nullptr) *temp = temp_c;
        return true;
    }

    // ========================================================================
    //  板级外设接口
    //
    //  ⚠ 框架里有几处是无条件解引用的（application.cc 的 display->SetupUI()
    //  / codec->... / led->OnStateChanged()），所以这几个 nullptr 会让整机固件
    //  启动时踩空。上面每个返回 nullptr 的地方都写清了缺什么驱动。
    // ========================================================================

    // 本板 config.h 里没有任何 LED 引脚定义，参考工程 board_pins.h 也没有 —— 硬件上就没这颗灯
    virtual Led* GetLed() override {
        return &noop_led_;
    }

    // 本板没有独立 PWM 背光脚，亮度走面板自己的 0x51 寄存器（见 Co5300Backlight）。
    // 屏没起来时返回 nullptr —— 面板都不在，对外宣称"能调亮度"是骗人；
    // mcp_server 注册 self.screen.set_brightness 前会判空，所以返回 nullptr 是安全的。
    virtual Backlight* GetBacklight() override {
        return lcd_.Ready() ? &backlight_ : nullptr;
    }

    // 正常是 MipiLcdDisplay（CO5300 466x466）；面板初始化失败时退到无 UI 后端 ——
    // 绝不能返回 nullptr，application.cc 会无条件调 display->SetupUI()。
    virtual Display* GetDisplay() override {
        return display_;
    }

    // 音频是 I2S 数字功放（U8: AW88082QNR）+ PDM 数字麦（U7），不是 I2C 编解码器，
    // 框架现有的 Es8311AudioCodec 完全不适用。参考工程里能用的那套在 audio_test.c
    // （不是 README 里写的 board_audio.c，那个文件名不存在），已移植成
    // XiaoweiPlusAudioCodec —— 见同目录 xiaowei_plus_audio.h 开头列的三处差异。
    virtual AudioCodec* GetAudioCodec() override {
        return &AudioCodecInstance();
    }

    // 电量是真的：SY6206 驱动在板级目录里，已初始化
    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        if (pmic_ == nullptr) {
            level = 0;
            charging = false;
            discharging = false;
            return false;
        }

        // 内阻补偿后的开路电压 + 放电曲线查表，避免充电时裸 VBAT 读数虚高导致插拔电量跳变
        Sy6206::BatteryState state;
        if (pmic_->GetBatteryState(state) == ESP_OK) {
            level = state.percentage;
            charging = state.is_charging;
            discharging = !charging;
            return true;
        }

        level = 0;
        charging = false;
        discharging = false;
        return false;
    }
};

DECLARE_BOARD(XiaoweiPlusBoard);

// ============================================================================
//  给 C 文件（display/src/ui/platform/xiaowei_ui_hw.cc）读麦克风电平的入口
//
//  为什么绕这一道而不是让 UI 直接调 codec：UI 层在 display 组件里，不该 include
//  板级头文件（那会把 P4 板的类型塞进本来与板型无关的 UI 代码）。这里出一个纯 C
//  符号，UI 侧只需要一行 extern 声明。
//
//  为什么电平由 codec 出而不是 AudioService：框架的录音测试队列里存的是 **opus 包**，
//  不是 PCM，AudioService 也从不暴露输入电平。codec 的 Read() 是唯一能看到原始 PCM
//  的地方，而且它测的是**加增益之前**的电平 —— 调增益时不会被自己的增益绕进去。
//
//  非 XiaoweiPlusAudioCodec（理论上不会发生）时返回 0，不清空调用方的值。
// ============================================================================
extern "C" void xiaowei_plus_audio_input_levels(int* rms, int* peak)
{
    AudioCodec* codec = Board::GetInstance().GetAudioCodec();
    auto* plus = static_cast<XiaoweiPlusAudioCodec*>(codec);
    if (plus == nullptr) {
        return;
    }
    if (rms != nullptr) {
        *rms = plus->InputLevelRms();
    }
    if (peak != nullptr) {
        *peak = plus->InputLevelPeak();
    }
}

// 同一件事的第二路：**加增益之后**、真正送进 AFE 的电平。
// 和上面那一路的分工写在 xiaowei_plus_audio.h 里 —— 简言之，调 MIC_GAIN 时看这一路。
extern "C" void xiaowei_plus_audio_afe_levels(int* rms, int* peak)
{
    AudioCodec* codec = Board::GetInstance().GetAudioCodec();
    auto* plus = static_cast<XiaoweiPlusAudioCodec*>(codec);
    if (plus == nullptr) {
        return;
    }
    if (rms != nullptr) {
        *rms = plus->InputGainLevelRms();
    }
    if (peak != nullptr) {
        *peak = plus->InputGainLevelPeak();
    }
}

// ============================================================================
//  给 C 文件（display/src/ui/apps/...）用的马达 / IMU 原语
//
//  和上面音频两路同样的理由：UI 层不该 include 板级头文件。这里只暴露
//  “播第几号波形 / 当前在不在播 / 设强度 / 读一次 IMU”；模式映射和测试序列
//  这类 UI 语义留在 legacy_app.c，板级不猜 UI 想播什么。
// ============================================================================
static XiaoweiPlusBoard& XwBoard()
{
    return static_cast<XiaoweiPlusBoard&>(Board::GetInstance());
}

extern "C" bool xiaowei_haptic_play_wave(int index)
{
    return XwBoard().HapticPlayWave(index);
}

extern "C" bool xiaowei_haptic_is_playing(void)
{
    return XwBoard().HapticIsPlaying();
}

extern "C" void xiaowei_haptic_set_intensity(int percent)
{
    XwBoard().HapticSetIntensity(percent);
}

extern "C" bool xiaowei_imu_get(float* ax, float* ay, float* az,
                                float* gx, float* gy, float* gz, float* temp)
{
    return XwBoard().ImuRead(ax, ay, az, gx, gy, gz, temp);
}
