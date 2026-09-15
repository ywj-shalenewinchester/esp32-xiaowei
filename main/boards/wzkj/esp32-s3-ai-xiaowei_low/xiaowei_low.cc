#include "board.h"
#include "wifi_board.h"
#include "config.h"
#include "sy6206.h"

#include "codecs/es8311_audio_codec.h"
#include "display/lcd_display.h"
#include "backlight.h"
#include "led.h"
#include "button.h"
#include "application.h"
#include "mcp_server.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st77916.h"
#include "driver/rtc_io.h"
#include "esp_timer.h"

#define TAG "WzkjBoard"

// ST77916 初始化命令集（参考 taiji-pi-s3 标准 QSPI 模式）
static const st77916_lcd_init_cmd_t lcd_init_cmds[] = {
    {0xF0, (uint8_t[]){0x28}, 1, 0},
    {0xF2, (uint8_t[]){0x28}, 1, 0},
    {0x73, (uint8_t[]){0xF0}, 1, 0},
    {0x7C, (uint8_t[]){0xD1}, 1, 0},
    {0x83, (uint8_t[]){0xE0}, 1, 0},
    {0x84, (uint8_t[]){0x61}, 1, 0},
    {0xF2, (uint8_t[]){0x82}, 1, 0},
    {0xF0, (uint8_t[]){0x00}, 1, 0},
    {0xF0, (uint8_t[]){0x01}, 1, 0},
    {0xF1, (uint8_t[]){0x01}, 1, 0},
    {0xB0, (uint8_t[]){0x56}, 1, 0},
    {0xB1, (uint8_t[]){0x4D}, 1, 0},
    {0xB2, (uint8_t[]){0x24}, 1, 0},
    {0xB4, (uint8_t[]){0x87}, 1, 0},
    {0xB5, (uint8_t[]){0x44}, 1, 0},
    {0xB6, (uint8_t[]){0x8B}, 1, 0},
    {0xB7, (uint8_t[]){0x40}, 1, 0},
    {0xB8, (uint8_t[]){0x86}, 1, 0},
    {0xBA, (uint8_t[]){0x00}, 1, 0},
    {0xBB, (uint8_t[]){0x08}, 1, 0},
    {0xBC, (uint8_t[]){0x08}, 1, 0},
    {0xBD, (uint8_t[]){0x00}, 1, 0},
    {0xC0, (uint8_t[]){0x80}, 1, 0},
    {0xC1, (uint8_t[]){0x10}, 1, 0},
    {0xC2, (uint8_t[]){0x37}, 1, 0},
    {0xC3, (uint8_t[]){0x80}, 1, 0},
    {0xC4, (uint8_t[]){0x10}, 1, 0},
    {0xC5, (uint8_t[]){0x37}, 1, 0},
    {0xC6, (uint8_t[]){0xA9}, 1, 0},
    {0xC7, (uint8_t[]){0x41}, 1, 0},
    {0xC8, (uint8_t[]){0x01}, 1, 0},
    {0xC9, (uint8_t[]){0xA9}, 1, 0},
    {0xCA, (uint8_t[]){0x41}, 1, 0},
    {0xCB, (uint8_t[]){0x01}, 1, 0},
    {0xD0, (uint8_t[]){0x91}, 1, 0},
    {0xD1, (uint8_t[]){0x68}, 1, 0},
    {0xD2, (uint8_t[]){0x68}, 1, 0},
    {0xF5, (uint8_t[]){0x00, 0xA5}, 2, 0},
    {0xDD, (uint8_t[]){0x4F}, 1, 0},
    {0xDE, (uint8_t[]){0x4F}, 1, 0},
    {0xF1, (uint8_t[]){0x10}, 1, 0},
    {0xF0, (uint8_t[]){0x00}, 1, 0},
    {0xF0, (uint8_t[]){0x02}, 1, 0},
    {0xE0, (uint8_t[]){0xF0, 0x0A, 0x10, 0x09, 0x09, 0x36, 0x35, 0x33, 0x4A, 0x29, 0x15, 0x15, 0x2E, 0x34}, 14, 0},
    {0xE1, (uint8_t[]){0xF0, 0x0A, 0x0F, 0x08, 0x08, 0x05, 0x34, 0x33, 0x4A, 0x39, 0x15, 0x15, 0x2D, 0x33}, 14, 0},
    {0xF0, (uint8_t[]){0x10}, 1, 0},
    {0xF3, (uint8_t[]){0x10}, 1, 0},
    {0xE0, (uint8_t[]){0x07}, 1, 0},
    {0xE1, (uint8_t[]){0x00}, 1, 0},
    {0xE2, (uint8_t[]){0x00}, 1, 0},
    {0xE3, (uint8_t[]){0x00}, 1, 0},
    {0xE4, (uint8_t[]){0xE0}, 1, 0},
    {0xE5, (uint8_t[]){0x06}, 1, 0},
    {0xE6, (uint8_t[]){0x21}, 1, 0},
    {0xE7, (uint8_t[]){0x01}, 1, 0},
    {0xE8, (uint8_t[]){0x05}, 1, 0},
    {0xE9, (uint8_t[]){0x02}, 1, 0},
    {0xEA, (uint8_t[]){0xDA}, 1, 0},
    {0xEB, (uint8_t[]){0x00}, 1, 0},
    {0xEC, (uint8_t[]){0x00}, 1, 0},
    {0xED, (uint8_t[]){0x0F}, 1, 0},
    {0xEE, (uint8_t[]){0x00}, 1, 0},
    {0xEF, (uint8_t[]){0x00}, 1, 0},
    {0xF8, (uint8_t[]){0x00}, 1, 0},
    {0xF9, (uint8_t[]){0x00}, 1, 0},
    {0xFA, (uint8_t[]){0x00}, 1, 0},
    {0xFB, (uint8_t[]){0x00}, 1, 0},
    {0xFC, (uint8_t[]){0x00}, 1, 0},
    {0xFD, (uint8_t[]){0x00}, 1, 0},
    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xFF, (uint8_t[]){0x00}, 1, 0},
    {0x60, (uint8_t[]){0x40}, 1, 0},
    {0x61, (uint8_t[]){0x04}, 1, 0},
    {0x62, (uint8_t[]){0x00}, 1, 0},
    {0x63, (uint8_t[]){0x42}, 1, 0},
    {0x64, (uint8_t[]){0xD9}, 1, 0},
    {0x65, (uint8_t[]){0x00}, 1, 0},
    {0x66, (uint8_t[]){0x00}, 1, 0},
    {0x67, (uint8_t[]){0x00}, 1, 0},
    {0x68, (uint8_t[]){0x00}, 1, 0},
    {0x69, (uint8_t[]){0x00}, 1, 0},
    {0x6A, (uint8_t[]){0x00}, 1, 0},
    {0x6B, (uint8_t[]){0x00}, 1, 0},
    {0x70, (uint8_t[]){0x40}, 1, 0},
    {0x71, (uint8_t[]){0x03}, 1, 0},
    {0x72, (uint8_t[]){0x00}, 1, 0},
    {0x73, (uint8_t[]){0x42}, 1, 0},
    {0x74, (uint8_t[]){0xD8}, 1, 0},
    {0x75, (uint8_t[]){0x00}, 1, 0},
    {0x76, (uint8_t[]){0x00}, 1, 0},
    {0x77, (uint8_t[]){0x00}, 1, 0},
    {0x78, (uint8_t[]){0x00}, 1, 0},
    {0x79, (uint8_t[]){0x00}, 1, 0},
    {0x7A, (uint8_t[]){0x00}, 1, 0},
    {0x7B, (uint8_t[]){0x00}, 1, 0},
    {0x80, (uint8_t[]){0x48}, 1, 0},
    {0x81, (uint8_t[]){0x00}, 1, 0},
    {0x82, (uint8_t[]){0x06}, 1, 0},
    {0x83, (uint8_t[]){0x02}, 1, 0},
    {0x84, (uint8_t[]){0xD6}, 1, 0},
    {0x85, (uint8_t[]){0x04}, 1, 0},
    {0x86, (uint8_t[]){0x00}, 1, 0},
    {0x87, (uint8_t[]){0x00}, 1, 0},
    {0x88, (uint8_t[]){0x48}, 1, 0},
    {0x89, (uint8_t[]){0x00}, 1, 0},
    {0x8A, (uint8_t[]){0x08}, 1, 0},
    {0x8B, (uint8_t[]){0x02}, 1, 0},
    {0x8C, (uint8_t[]){0xD8}, 1, 0},
    {0x8D, (uint8_t[]){0x04}, 1, 0},
    {0x8E, (uint8_t[]){0x00}, 1, 0},
    {0x8F, (uint8_t[]){0x00}, 1, 0},
    {0x90, (uint8_t[]){0x48}, 1, 0},
    {0x91, (uint8_t[]){0x00}, 1, 0},
    {0x92, (uint8_t[]){0x0A}, 1, 0},
    {0x93, (uint8_t[]){0x02}, 1, 0},
    {0x94, (uint8_t[]){0xDA}, 1, 0},
    {0x95, (uint8_t[]){0x04}, 1, 0},
    {0x96, (uint8_t[]){0x00}, 1, 0},
    {0x97, (uint8_t[]){0x00}, 1, 0},
    {0x98, (uint8_t[]){0x48}, 1, 0},
    {0x99, (uint8_t[]){0x00}, 1, 0},
    {0x9A, (uint8_t[]){0x0C}, 1, 0},
    {0x9B, (uint8_t[]){0x02}, 1, 0},
    {0x9C, (uint8_t[]){0xDC}, 1, 0},
    {0x9D, (uint8_t[]){0x04}, 1, 0},
    {0x9E, (uint8_t[]){0x00}, 1, 0},
    {0x9F, (uint8_t[]){0x00}, 1, 0},
    {0xA0, (uint8_t[]){0x48}, 1, 0},
    {0xA1, (uint8_t[]){0x00}, 1, 0},
    {0xA2, (uint8_t[]){0x05}, 1, 0},
    {0xA3, (uint8_t[]){0x02}, 1, 0},
    {0xA4, (uint8_t[]){0xD5}, 1, 0},
    {0xA5, (uint8_t[]){0x04}, 1, 0},
    {0xA6, (uint8_t[]){0x00}, 1, 0},
    {0xA7, (uint8_t[]){0x00}, 1, 0},
    {0xA8, (uint8_t[]){0x48}, 1, 0},
    {0xA9, (uint8_t[]){0x00}, 1, 0},
    {0xAA, (uint8_t[]){0x07}, 1, 0},
    {0xAB, (uint8_t[]){0x02}, 1, 0},
    {0xAC, (uint8_t[]){0xD7}, 1, 0},
    {0xAD, (uint8_t[]){0x04}, 1, 0},
    {0xAE, (uint8_t[]){0x00}, 1, 0},
    {0xAF, (uint8_t[]){0x00}, 1, 0},
    {0xB0, (uint8_t[]){0x48}, 1, 0},
    {0xB1, (uint8_t[]){0x00}, 1, 0},
    {0xB2, (uint8_t[]){0x09}, 1, 0},
    {0xB3, (uint8_t[]){0x02}, 1, 0},
    {0xB4, (uint8_t[]){0xD9}, 1, 0},
    {0xB5, (uint8_t[]){0x04}, 1, 0},
    {0xB6, (uint8_t[]){0x00}, 1, 0},
    {0xB7, (uint8_t[]){0x00}, 1, 0},
    {0xB8, (uint8_t[]){0x48}, 1, 0},
    {0xB9, (uint8_t[]){0x00}, 1, 0},
    {0xBA, (uint8_t[]){0x0B}, 1, 0},
    {0xBB, (uint8_t[]){0x02}, 1, 0},
    {0xBC, (uint8_t[]){0xDB}, 1, 0},
    {0xBD, (uint8_t[]){0x04}, 1, 0},
    {0xBE, (uint8_t[]){0x00}, 1, 0},
    {0xBF, (uint8_t[]){0x00}, 1, 0},
    {0xC0, (uint8_t[]){0x10}, 1, 0},
    {0xC1, (uint8_t[]){0x47}, 1, 0},
    {0xC2, (uint8_t[]){0x56}, 1, 0},
    {0xC3, (uint8_t[]){0x65}, 1, 0},
    {0xC4, (uint8_t[]){0x74}, 1, 0},
    {0xC5, (uint8_t[]){0x88}, 1, 0},
    {0xC6, (uint8_t[]){0x99}, 1, 0},
    {0xC7, (uint8_t[]){0x01}, 1, 0},
    {0xC8, (uint8_t[]){0xBB}, 1, 0},
    {0xC9, (uint8_t[]){0xAA}, 1, 0},
    {0xD0, (uint8_t[]){0x10}, 1, 0},
    {0xD1, (uint8_t[]){0x47}, 1, 0},
    {0xD2, (uint8_t[]){0x56}, 1, 0},
    {0xD3, (uint8_t[]){0x65}, 1, 0},
    {0xD4, (uint8_t[]){0x74}, 1, 0},
    {0xD5, (uint8_t[]){0x88}, 1, 0},
    {0xD6, (uint8_t[]){0x99}, 1, 0},
    {0xD7, (uint8_t[]){0x01}, 1, 0},
    {0xD8, (uint8_t[]){0xBB}, 1, 0},
    {0xD9, (uint8_t[]){0xAA}, 1, 0},
    {0xF3, (uint8_t[]){0x01}, 1, 0},
    {0xF0, (uint8_t[]){0x00}, 1, 0},
    {0x21, (uint8_t[]){0x00}, 1, 0},
    {0x11, (uint8_t[]){0x00}, 1, 120},
    {0x29, (uint8_t[]){0x00}, 1, 0},
};

// 
/**
 * @class WzkjLcdDisplay
 * @brief 继承自SpiLcdDisplay的圆屏显示类，用于处理360x360分辨率的圆屏显示逻辑
 */
class WzkjLcdDisplay : public SpiLcdDisplay {
public:
    // 使用父类构造函数
    using SpiLcdDisplay::SpiLcdDisplay;

    /**
     * @brief 设置用户界面布局
     * @override 重写父类的SetupUI方法，针对圆屏进行特殊布局调整
     */
    void SetupUI() override {
        // 首先调用父类的UI设置方法
        SpiLcdDisplay::SetupUI();
        // 获取显示锁，确保UI操作线程安全
        DisplayLockGuard lock(this);

        // 圆屏 360x360 安全区对齐
        // 将状态栏对象在顶部居中显示，并设置垂直偏移量
        lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, DISPLAY_STATUS_BAR_TOP_OFFSET);

        // 检查底部栏和聊天消息标签是否存在
        if (bottom_bar_ != nullptr && chat_message_label_ != nullptr) {
            // 设置底部栏的宽度为聊天栏的标准宽度
            lv_obj_set_width(bottom_bar_, DISPLAY_CHAT_BAR_WIDTH);
            // 将底部栏在底部居中显示，并设置垂直偏移量
            lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, -DISPLAY_CHAT_BAR_BOTTOM_OFFSET);

            // 计算水平方向的内边距总和（左侧 + 右侧）
            const lv_coord_t horizontal_padding =
                lv_obj_get_style_pad_left(bottom_bar_, LV_PART_MAIN) +
                lv_obj_get_style_pad_right(bottom_bar_, LV_PART_MAIN);
            // 设置聊天消息标签的宽度，减去水平内边距以确保文本不超出边界
            lv_obj_set_width(chat_message_label_, DISPLAY_CHAT_BAR_WIDTH - horizontal_padding);
        }
    }
};

/**
 * @class WzkjBoard
 * @brief 继承自WifiBoard的WzkjBoard类，实现了特定硬件平台的功能
 */
class WzkjBoard : public WifiBoard {
private:
    // I2C总线句柄，用于音频设备和电源管理芯片的通信
    i2c_master_bus_handle_t audio_i2c_bus_;
    i2c_master_bus_handle_t pmic_i2c_bus_;
    // 电源管理芯片指针
    Sy6206                 *pmic_;
    // 按键对象，包括启动按钮和电源按钮
    Button                  boot_button_;
    Button                  pwr_button_;  // 对应 QON 按键
    // 显示设备相关句柄
    LcdDisplay             *display_ = nullptr;
    esp_lcd_panel_handle_t  panel_ = nullptr;  // 关机时用于关闭显示

    // 1. 初始化 音频 I2C 总线
    void InitializeAudioI2c() {
        ESP_LOGI(TAG, "Initialize Audio I2C: SDA=%d SCL=%d port=%d",
                 AUDIO_I2C_SDA_PIN, AUDIO_I2C_SCL_PIN, AUDIO_I2C_PORT);

        i2c_master_bus_config_t audio_bus_cfg = {
            .i2c_port = AUDIO_I2C_PORT,
            .sda_io_num = (gpio_num_t)AUDIO_I2C_SDA_PIN,
            .scl_io_num = (gpio_num_t)AUDIO_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&audio_bus_cfg, &audio_i2c_bus_));   
    }

    // 2. 初始化 PMIC I2C 总线和 SY6206
    void InitializePmic() {
        ESP_LOGI(TAG, "Initialize PMIC I2C: SDA=%d SCL=%d port=%d",
                 PMIC_I2C_SDA_PIN, PMIC_I2C_SCL_PIN, PMIC_I2C_PORT);

        i2c_master_bus_config_t pmic_bus_cfg = {
            .i2c_port = PMIC_I2C_PORT,
            .sda_io_num = (gpio_num_t)PMIC_I2C_SDA_PIN,
            .scl_io_num = (gpio_num_t)PMIC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&pmic_bus_cfg, &pmic_i2c_bus_));

        pmic_ = new Sy6206(pmic_i2c_bus_, SY6206_ADDR);
        pmic_->SetHardwareChargeEnable(true);
        pmic_->SetChargeVoltageMv(4200);  // 4.2V 充满电压
        pmic_->SetChargeCurrentMa(512);   // 512mA 充电电流
        pmic_->SetChargeEnable(true);
        pmic_->EnableAdc(true);           // 连续模式 ADC

        uint8_t chip_id = 0, rev = 0;
        if (pmic_->ReadChipId(&chip_id, &rev) == ESP_OK) {
            ESP_LOGI(TAG, "SY6206 ready: chip_id=%u rev=%u", chip_id, rev);
        }
    }

    // 3. 初始化 QSPI 总线和 ST77916 LCD 面板
    void InitializeLcdDisplay() {
        ESP_LOGI(TAG, "Initialize QSPI bus");
        const spi_bus_config_t bus_config = WZKJ_ST77916_PANEL_BUS_QSPI_CONFIG(
            LCD_QSPI_CLK_PIN, LCD_QSPI_D0_PIN, LCD_QSPI_D1_PIN,
            LCD_QSPI_D2_PIN, LCD_QSPI_D3_PIN,
            DISPLAY_WIDTH * 80 * sizeof(uint16_t));
        ESP_ERROR_CHECK(spi_bus_initialize(QSPI_LCD_HOST, &bus_config, SPI_DMA_CH_AUTO));

        ESP_LOGI(TAG, "Install panel IO");
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = LCD_CS_PIN;
        io_config.dc_gpio_num = GPIO_NUM_NC;  // QSPI 不需要独立 DC
        io_config.spi_mode = 0;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 32;
        io_config.lcd_param_bits = 8;
        io_config.flags.quad_mode = true;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)QSPI_LCD_HOST,
                                                 &io_config, &panel_io));

        ESP_LOGI(TAG, "Install ST77916 panel driver");
        st77916_vendor_config_t vendor_config = {
            .init_cmds = lcd_init_cmds,
            .init_cmds_size = sizeof(lcd_init_cmds) / sizeof(st77916_lcd_init_cmd_t),
            .flags = {
                .use_qspi_interface = 1,
            },
        };
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;  // RGB565
        panel_config.reset_gpio_num = LCD_RST_PIN;
        panel_config.vendor_config = &vendor_config;

        ESP_ERROR_CHECK(esp_lcd_new_panel_st77916(panel_io, &panel_config, &panel_));
        esp_lcd_panel_reset(panel_);
        esp_lcd_panel_init(panel_);
        esp_lcd_panel_disp_on_off(panel_, true);
        esp_lcd_panel_swap_xy(panel_, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel_, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);

        display_ = new WzkjLcdDisplay(panel_io, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                      DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
                                      DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

    // 4. 初始化按键逻辑
    void InitializeButtons() {
        // BOOT 按键：短按唤醒 / 切换对话
        //   启动期短按 → 进入配网
        //   运行期短按 → ToggleChatState()，空闲时开始对话，speaking 时打断当前回复
        boot_button_.OnClick([this]() {
            auto &app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });

        // QON 长按：进入深度睡眠，通过 QON 重新唤醒
        pwr_button_.OnLongPress([this]() {
            // 关机流程由主任务执行，避免在按键回调里阻塞
            Application::GetInstance().Schedule([this]() {
                ESP_LOGI(TAG, "Shutting down");

                if (display_ != nullptr) {
                    display_->SetChatMessage("system", "OFF");
                }
                // 给一次渲染机会，让关机提示可见
                vTaskDelay(pdMS_TO_TICKS(500));

                GetBacklight()->SetBrightness(0);
                // 关闭显示，避免睡眠期间残留
                if (panel_ != nullptr) {
                    esp_lcd_panel_disp_on_off(panel_, false);
                }

                // QON 是 RTC GPIO，睡眠期间保持上拉，避免悬空误唤醒
                ESP_ERROR_CHECK(esp_sleep_enable_ext0_wakeup((gpio_num_t)PMIC_QON_BUTTON_PIN, 0));
                ESP_ERROR_CHECK(rtc_gpio_pullup_en((gpio_num_t)PMIC_QON_BUTTON_PIN));
                ESP_ERROR_CHECK(rtc_gpio_pulldown_dis((gpio_num_t)PMIC_QON_BUTTON_PIN));

                esp_deep_sleep_start();
            });
        });
    }

    // 5. 注册 MCP 工具
    void InitializeTools() {
        auto &mcp_server = McpServer::GetInstance();
        mcp_server.AddTool("self.disp.network", "重新配网",
                           PropertyList(),
                           [this](const PropertyList &) -> ReturnValue {
                               EnterWifiConfigMode();
                               return true;
                           });
    }

public:
    WzkjBoard() : boot_button_(BOOT_BUTTON_GPIO), pwr_button_(PMIC_QON_BUTTON_PIN) {
        InitializePmic();        // 必须最先通电
        InitializeAudioI2c();    // 初始化音频总线
        InitializeLcdDisplay();  // 初始化屏幕
        InitializeButtons();     // 注册按键
        InitializeTools();       // 注册配网等工具服务
        GetBacklight()->RestoreBrightness();    // 恢复用户上次设置的亮度
    }

    
    virtual AudioCodec *GetAudioCodec() override {
        static Es8311AudioCodec audio_codec(
            (void *)audio_i2c_bus_,
            AUDIO_I2C_PORT,
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            (gpio_num_t)AUDIO_ES8311_MCLK_PIN,
            (gpio_num_t)AUDIO_ES8311_SCLK_PIN,
            (gpio_num_t)AUDIO_ES8311_LRCK_PIN,
            (gpio_num_t)AUDIO_ES8311_DAC_DATA_PIN,
            (gpio_num_t)AUDIO_ES8311_ADC_DATA_PIN,
            (gpio_num_t)AUDIO_NS4150B_CTRL_PIN,
            AUDIO_CODEC_ES8311_ADDR);
        return &audio_codec;
    }

    virtual Display *GetDisplay() override {
        return display_;
    }

    virtual Backlight *GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    virtual Led *GetLed() override {
        static GpioLed led(R_LED_PIN, R_LED_ACTIVE_LEVEL, R_LED_BLINK_INTERVAL_MS);
        return &led;
    }

    virtual bool GetBatteryLevel(int &level, bool &charging, bool &discharging) override {
        if (!pmic_) {
            level = 0;
            charging = false;
            discharging = false;
            return false;
        }

        // 1. 获取充电状态
        Sy6206ChargeStatus chg;
        Sy6206VbusStatus vbus;
        bool vbus_pg;
        if (pmic_->GetChargeStatus(&chg, &vbus, &vbus_pg) == ESP_OK) {
            charging = (chg == SY6206_CHG_TRICKLE_PRE || chg == SY6206_CHG_CC || chg == SY6206_CHG_CV);
            discharging = !charging;
        } else {
            charging = false;
            discharging = false;
        }

        // 2. 读取并映射真实电压
        float vbat_mv = 0;
        if (pmic_->ReadAdcVbat(vbat_mv) == ESP_OK) {
            if (vbat_mv >= 4200.0f) {
                level = 100;
            } else if (vbat_mv <= 3300.0f) {
                level = 0;
            } else {
                level = (int)((vbat_mv - 3300.0f) / (4200.0f - 3300.0f) * 100);
            }

            // 充电完成
            if (chg == SY6206_CHG_DONE) level = 100;

            // 充电中且未完成时最高锁定 99%
            if (charging && level == 100 && chg != SY6206_CHG_DONE) {
                level = 99;
            }
            return true;
        }

        level = 0;
        return false;
    }
};

DECLARE_BOARD(WzkjBoard);
