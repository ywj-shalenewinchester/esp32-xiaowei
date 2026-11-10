#include "lcd.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_heap_caps.h>
#include <esp_lcd_co5300.h>
#include <esp_log.h>

#include "config.h"

static const char* TAG = "Lcd";

// DSI PHY 供电：P4 上 MIPI DSI 的 PHY 由芯片内部 LDO 通道 3 供 2.5V。
static constexpr int LCD_DSI_PHY_LDO_CHAN = 3;
static constexpr int LCD_DSI_PHY_LDO_MV = 2500;

Lcd::~Lcd() {
    // 逆序拆：面板 -> DBI IO -> DSI 总线 -> PHY 供电。
    // 面板/IO 只有在没移交给显示对象时才由这里删 —— 见 lcd.h 里 HandOffPanel 的说明，
    // 两边都删就是 double free。
    if (owns_panel_ && panel_) {
        esp_lcd_panel_del(panel_);
    }
    if (owns_panel_ && panel_io_) {
        esp_lcd_panel_io_del(panel_io_);
    }
    panel_ = nullptr;
    panel_io_ = nullptr;
    if (dsi_bus_) {
        esp_lcd_del_dsi_bus(dsi_bus_);
        dsi_bus_ = nullptr;
    }
    if (dsi_phy_ldo_) {
        esp_ldo_release_channel(dsi_phy_ldo_);
        dsi_phy_ldo_ = nullptr;
    }
}

esp_err_t Lcd::Init() {
    if (panel_ != nullptr) {
        return ESP_OK;  // 幂等
    }

    // Init() 中途失败时已经把拿到的句柄留在成员里了：Ready() 仍是 false（板级会退回
    // 无 UI 后端），析构函数负责把半途申请的资源放掉。
    esp_err_t err = ESP_OK;

    // ------------------------------------------------------------------
    // 1) DSI PHY 的供电必须先于 DSI 主机建立，否则后面建总线/发命令全都不通。
    // ------------------------------------------------------------------
    esp_ldo_channel_config_t ldo_cfg = {};
    ldo_cfg.chan_id = LCD_DSI_PHY_LDO_CHAN;
    ldo_cfg.voltage_mv = LCD_DSI_PHY_LDO_MV;
    err = esp_ldo_acquire_channel(&ldo_cfg, &dsi_phy_ldo_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "DSI PHY LDO(chan %d @ %dmV) 获取失败: %s", LCD_DSI_PHY_LDO_CHAN, LCD_DSI_PHY_LDO_MV,
                 esp_err_to_name(err));
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(10));  // 等 PHY 供电稳下来

    // ------------------------------------------------------------------
    // 2) DSI 总线：本板 J4 只走 1 条数据 lane
    // ------------------------------------------------------------------
    esp_lcd_dsi_bus_config_t bus_cfg = {};
    bus_cfg.bus_id = 0;
    bus_cfg.num_data_lanes = 1;
    bus_cfg.phy_clk_src = (mipi_dsi_phy_pllref_clock_source_t)0;  // 0 = 让 IDF 自己挑 P4 可用的 PHY 参考时钟
    bus_cfg.lane_bit_rate_mbps = 480;
    err = esp_lcd_new_dsi_bus(&bus_cfg, &dsi_bus_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "DSI 总线创建失败: %s", esp_err_to_name(err));
        return err;
    }

    // DBI：走命令通道（CO5300 的初始化表、亮度、MADCTL 都从这里发）
    esp_lcd_dbi_io_config_t dbi_cfg = {};
    dbi_cfg.virtual_channel = 0;
    dbi_cfg.lcd_cmd_bits = 8;
    dbi_cfg.lcd_param_bits = 8;
    err = esp_lcd_new_panel_io_dbi(dsi_bus_, &dbi_cfg, &panel_io_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "DBI IO 创建失败: %s", esp_err_to_name(err));
        return err;
    }

    // ------------------------------------------------------------------
    // 3) DPI：像素流那一路。时序按面板手册填，别随手改 —— 改错就是花屏/不亮。
    //    dpi_clock 16MHz + 这组前后肩 => 60Hz（厂商给的组合）。
    //    这里**故意**逐字段赋值而不用 co5300 头文件里的
    //    CO5300_466_466_PANEL_60HZ_DPI_CONFIG() 宏：那个宏把 dpi_clk_src 写在最前面，
    //    而 esp_lcd_dpi_panel_config_t 的第一个成员是 virtual_channel —— 指示初始化器
    //    顺序和声明顺序不一致，C++ 里是硬错误（C 里才无所谓）。
    // ------------------------------------------------------------------
    esp_lcd_dpi_panel_config_t dpi_cfg = {};
    dpi_cfg.dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT;
    dpi_cfg.dpi_clock_freq_mhz = 16;
    dpi_cfg.virtual_channel = 0;
    dpi_cfg.pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565;
    // 2 块帧缓冲，和 display/lcd_display.cc 里的 avoid_tearing = true **配套**，
    // 少一样都起不来或者没效果：
    //   - avoid_tearing 的实现就是 lvgl_port 调
    //     esp_lcd_dpi_panel_get_frame_buffer(panel, 2, &fb0, &fb1)（见
    //     managed_components/espressif__esp_lvgl_port/src/lvgl9/esp_lvgl_port_disp.c:369），
    //     把**面板自己的**两块 fb 直接当 LVGL 的绘制缓冲（DIRECT 模式），
    //     画完在 vsync 上换扫描指针。
    //   - 所以 num_fbs 必须 >= 2，否则那个调用直接失败、显示根本起不来。
    //
    // 原来那套 num_fbs = 1 + avoid_tearing = false 是：LVGL 单缓冲，直接往
    // **正在被扫描输出**的那块 fb 里写。画面一变就有一条横带 —— 摄像头预览这种
    // 大面积、高频变化的页面上尤其刺眼，静态界面看不出来。
    //
    // 代价：多一块 466×466×2 = 434KB 帧缓冲（PSRAM）。
    dpi_cfg.num_fbs = 2;
    dpi_cfg.video_timing.h_size = DISPLAY_WIDTH;
    dpi_cfg.video_timing.v_size = DISPLAY_HEIGHT;
    dpi_cfg.video_timing.hsync_pulse_width = 4;
    dpi_cfg.video_timing.hsync_back_porch = 32;
    dpi_cfg.video_timing.hsync_front_porch = 32;
    dpi_cfg.video_timing.vsync_pulse_width = 4;
    dpi_cfg.video_timing.vsync_back_porch = 12;
    dpi_cfg.video_timing.vsync_front_porch = 18;
    // 注意：IDF 6 把这块 API 换成了 in_color_format + esp_lcd_dpi_panel_enable_dma2d()，
    // 以后升 IDF 时这里要一起改。
    dpi_cfg.flags.use_dma2d = true;

    co5300_vendor_config_t vendor_cfg = {};
    vendor_cfg.flags.use_mipi_interface = 1;  // 不置 1 会按 SPI/QSPI 建面板
    vendor_cfg.mipi_config.dsi_bus = dsi_bus_;
    vendor_cfg.mipi_config.dpi_config = &dpi_cfg;
    // init_cmds 留空 = 用驱动内置的默认初始化表（里面已经带了 0x51=0xFF 满亮度）

    esp_lcd_panel_dev_config_t panel_cfg = {};
    panel_cfg.reset_gpio_num = DISPLAY_RST_PIN;
    panel_cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_cfg.bits_per_pixel = 16;
    panel_cfg.vendor_config = &vendor_cfg;
    err = esp_lcd_new_panel_co5300(panel_io_, &panel_cfg, &panel_);
    if (err != ESP_OK) {
        // 这里失败最常见的原因是内存：DPI 帧缓冲 + DMA2D 描述符都很大，
        // 所以把剩余内存一起打出来，省得下一轮再去猜。
        ESP_LOGE(TAG, "CO5300 面板创建失败: %s (内部剩余 %u, PSRAM 剩余 %u)", esp_err_to_name(err),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        return err;
    }
    owns_panel_ = true;  // 从这里起到 HandOffPanel() 之前，面板/IO 归本类删

    // 复位 -> 初始化 -> 退出睡眠（AMOLED 要显式唤醒才出画面）
    err = esp_lcd_panel_reset(panel_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CO5300 复位失败: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_lcd_panel_init(panel_);
    if (err != ESP_OK) {
        // 注意：panel_init 头一步就是读面板 ID（真实 DCS 读），所以"面板没插/没供电"
        // 会在这里失败 —— 这正是板级退回无 UI 后端所依赖的信号，不需要额外探测。
        ESP_LOGE(TAG, "CO5300 初始化失败: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_lcd_panel_disp_sleep(panel_, false);
    if (err != ESP_OK) {
        // 只是唤醒没成功，面板本体已经建好，不值得让整块屏判死
        ESP_LOGW(TAG, "CO5300 唤醒返回: %s", esp_err_to_name(err));
    }
    // 不要调 esp_lcd_panel_mirror/swap_xy：CO5300 的 MIPI 驱动没实现这两个，
    // 真要转屏得走面板的 MADCTL 命令。

    // 驱动写一次亮度就 ESP_LOGI 一行（"set brightness to N%"）。而框架的
    // RestoreBrightness() 是从 0 到目标值 5ms 一步地渐变上去的 —— 开机就会刷几十行
    // 无意义的亮度日志。这里把驱动自己的日志压到 WARN，我们自己的日志照常。
    esp_log_level_set("co5300_mipi", ESP_LOG_WARN);

    ready_ = true;
    ESP_LOGI(TAG, "CO5300 就绪: %dx%d, 1 lane, dpi 16MHz", DISPLAY_WIDTH, DISPLAY_HEIGHT);
    return ESP_OK;
}

esp_err_t Lcd::SetBrightness(uint8_t percent) {
    if (panel_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_lcd_panel_co5300_set_brightness(panel_, percent);
}

// ---------------------------------------------------------------------------
//  Co5300Backlight
// ---------------------------------------------------------------------------

void Co5300Backlight::SetBrightnessImpl(uint8_t brightness) {
    if (lcd_ == nullptr) {
        return;
    }
    // 这个回调由 Backlight 的渐变定时器以 5ms 一次的节奏调进来，失败不要刷屏 ——
    // 屏还没起来（或已经拆了）时静默返回即可。
    esp_err_t err = lcd_->SetBrightness(brightness);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "设置面板亮度 %u%% 失败: %s", (unsigned)brightness, esp_err_to_name(err));
    }
}
