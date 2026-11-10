// esp32-p4-ai-xiaowei_plus 配置文件

#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>
#include <driver/i2c.h>

// ==========================================
// 1. 系统、电源与控制 (System & Power)
// ==========================================
// SY6206 PMIC 充放电管理
#define SY6206_ADDR                     0x6B
#define PMIC_INT_PIN                    GPIO_NUM_16     // 充电中断输入 (读 SY6206 状态寄存器)
#define PMIC_STAT_PIN                   GPIO_NUM_17     // 充电状态 (勿只靠电平判断剩余电量)
#define PMIC_QON_BUTTON_PIN             GPIO_NUM_18     // 唤醒/按键 ~QON/SW1 (短按/长按/关机结合固件策略)

// 外部输入电源保护与故障监测 (SY5321)
#define POWER_PROT_EN_PIN               GPIO_NUM_5      // ~EN 输入保护使能 (低有效)
#define POWER_PROT_FLT_PIN              GPIO_NUM_6      // ~FLT 输入故障监测 (低有效)

// 启动与调试 (Boot & Debug)
#define BOOT_BUTTON_GPIO                GPIO_NUM_35     // BOOT 按钮引脚 / TP4 (进入下载/启动模式前确认外部电平与复位时序)
#define DEBUG_TP5_PIN                   GPIO_NUM_4      // 调试/测试点 TP5

// 底层硬件控制 (无标准 GPIO 编号，由硬件或专用外设接管)
// #define CHIP_PU_PIN                  (P103)          // 主控复位，连接 U15.2 REST (电源正常后由复位监控释放)
// #define EN_DCDC_PIN                  (P79)           // 降压使能，连接 U5.5 EN (控制 ESP_VDD_HP 电源级)
// #define USB_JTAG_D_MINUS             (TP1)           // 内部 USB_JTAG D-
// #define USB_JTAG_D_PLUS              (TP2)           // 内部 USB_JTAG D+

// ==========================================
// 2. I2C 总线分配 (I2C Buses)
// ==========================================
// 外设 I2C 总线 (连接 SY6206, U6 LSM6DSV80X, U8 AW88082QNR, U9 AW86937, 经 RN6 上拉至 +1V8)
#define PERIPH_I2C_SCL_PIN              GPIO_NUM_40
#define PERIPH_I2C_SDA_PIN              GPIO_NUM_39
#define PERIPH_I2C_PORT                 (I2C_NUM_0)

// 摄像头与触摸共享 I2C 总线 (连接 J3 DVP, J4 DSI 触摸)
#define CAM_TOUCH_I2C_SCL_PIN           GPIO_NUM_49     // CAM_SCL
#define CAM_TOUCH_I2C_SDA_PIN           GPIO_NUM_50     // CAM_SDA
#define CAM_TOUCH_I2C_PORT              (I2C_NUM_1)

// ==========================================
// 3. 显示与触摸 (MIPI DSI & Touch)
// ==========================================
// MIPI DSI 数据和时钟走专用 PHY 引脚 (J4.23/24, J4.26/27, J4.29/30)，无标准 GPIO 编号
// 显示控制与触摸中断
#define DISPLAY_RST_PIN                 GPIO_NUM_21     // DSI_RESET
#define DISPLAY_TE_PIN                  GPIO_NUM_22     // DSI_TE

#define TOUCH_INT_PIN                   GPIO_NUM_19     // DSI_TP_INT
#define TOUCH_RST_PIN                   GPIO_NUM_20     // DSI_TP_RST

// 面板：CO5300，466x466 圆屏 AMOLED，单 lane MIPI-DSI
#define DISPLAY_WIDTH                   466
#define DISPLAY_HEIGHT                  466
#define DISPLAY_MIRROR_X                false
#define DISPLAY_MIRROR_Y                false
#define DISPLAY_SWAP_XY                 false
#define DISPLAY_OFFSET_X                0
#define DISPLAY_OFFSET_Y                0

// 本板**没有**独立背光脚：亮度由面板自己的 0x51 寄存器控制
// （见 lcd.cc 的 Lcd::SetBrightness / Co5300Backlight）。
// 下面这行只是记录"确实没有这颗脚"，不要拿去建 PwmBacklight —— 会去配一个不存在的 GPIO。
#define DISPLAY_BACKLIGHT_PIN           GPIO_NUM_NC
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT false

// 圆屏安全区（状态栏/字幕往内缩，避开四角圆弧）—— 留给后续 UI 层移植时使用
#define DISPLAY_STATUS_BAR_TOP_OFFSET   0
#define DISPLAY_CHAT_BAR_WIDTH          400
#define DISPLAY_CHAT_BAR_BOTTOM_OFFSET  0

// CST820B 触摸：和摄像头共用 J4 那路 I2C（见上面第 2 节的 CAM_TOUCH_I2C_*）
#define TOUCH_I2C_PORT                  CAM_TOUCH_I2C_PORT
#define TOUCH_I2C_SDA_PIN               CAM_TOUCH_I2C_SDA_PIN
#define TOUCH_I2C_SCL_PIN               CAM_TOUCH_I2C_SCL_PIN
#define TOUCH_I2C_ADDRESS               0x15

// ==========================================
// 4. 摄像头 (DVP Camera)
// ==========================================

// 数据线 D0-D7 (经 RN1/RN2/RN3)
#define CAM_D0_PIN                      GPIO_NUM_29
#define CAM_D1_PIN                      GPIO_NUM_27
#define CAM_D2_PIN                      GPIO_NUM_26
#define CAM_D3_PIN                      GPIO_NUM_28
#define CAM_D4_PIN                      GPIO_NUM_30
#define CAM_D5_PIN                      GPIO_NUM_32
#define CAM_D6_PIN                      GPIO_NUM_33
#define CAM_D7_PIN                      GPIO_NUM_36

// 控制与时钟信号
#define CAM_PCLK_PIN                    GPIO_NUM_31
#define CAM_HSYNC_PIN                   GPIO_NUM_38     // DVP 的 DE/HREF
#define CAM_VSYNC_PIN                   GPIO_NUM_37
#define CAM_XCLK_PIN                    GPIO_NUM_34     // GC2145 的 MCLK
#define CAM_PWDN_PIN                    GPIO_NUM_52
#define CAM_RST_PIN                     GPIO_NUM_51

// ==========================================
// 5. 音频系统 (Audio)
// ==========================================
// 采样率
//
// 输入 48kHz（不是 24kHz）：PDM 麦克风的时钟 = 输入采样率 × 64
// （64 = ESP32-P4 hal 的 I2S_LL_PDM_BCK_FACTOR，即 PDM 的 CIC 抽取比），
// 所以 24kHz 只给出 1.536MHz 的 PDM 时钟 —— 虽仍在 LMD2718B261 手册标准模式的
// 1.12~4.8MHz 区间内，但贴着下限（典型值 2.4MHz）；厂商参考工程 board_audio.c 在这块
// 板子上实机验证过的是 48kHz -> 3.072MHz。这里先对齐参考值，配合板级开机自检
// （XiaoweiPlusBoard::SelfTestMicrophone）确认麦克风到底出不出数据。
//
// 输出按正常根工程使用 24kHz。功放 I2SCTRL1 必须写完整的 0x0405：
// 0x0400 是 Philips/16bit/32fs 格式位，0x5 是 24kHz SSR 码。
// 输入/输出是两条独立通道（NoAudioCodecSimplexPdm 里 duplex_ = false），可以不同；
// 48k->16k 那层重采样由 AudioService 的 esp_ae_rate_cvt 负责。
#define AUDIO_INPUT_SAMPLE_RATE         48000
#define AUDIO_OUTPUT_SAMPLE_RATE        24000

// 数字麦克风 (U7)
#define AUDIO_MIC_DATA_PIN              GPIO_NUM_47
#define AUDIO_MIC_CLK_PIN               GPIO_NUM_48

// 数字功放 (U8: AW88082QNR) 
#define AUDIO_AMP_WCK_PIN               GPIO_NUM_41     // I2S WS/LRCK
#define AUDIO_AMP_DATAI_PIN             GPIO_NUM_42     // I2S DATA_IN
#define AUDIO_AMP_BCK_PIN               GPIO_NUM_43     // I2S BCK
#define AUDIO_AMP_DATAO_PIN             GPIO_NUM_44     // I2S DATA_OUT

// ==========================================
// 6. 振动马达 (Haptic - U9: AW86937)
// ==========================================
#define HAPTIC_RST_PIN                  GPIO_NUM_0      // RSTN 复位控制
#define HAPTIC_TRIG3_PIN                GPIO_NUM_1      // 触发脉冲 3
#define HAPTIC_TRIG2_PIN                GPIO_NUM_2      // 触发脉冲 2
#define HAPTIC_TRIG1_PIN                GPIO_NUM_3      // 触发脉冲 1

// ==========================================
// 7. 无线协处理器 (ESP32-C61-MINI-1/U)
// ==========================================
// 使能与串口 (调试/控制通道)
#define WIFI_EN_PIN                     GPIO_NUM_7      // EN
#define WIFI_UART_TX_PIN                GPIO_NUM_8      // 串口 TX0
#define WIFI_UART_RX_PIN                GPIO_NUM_9      // 串口 RX0

// 注意：上面两个宏是**站在 C61 视角**命名的（TX0/RX0 是 C61 的串口脚）。
// 画到 P4 这边方向要反过来：P4 的 RX = WIFI_UART_TX_PIN(GPIO8) <- C61 TX0，
// P4 的 TX = WIFI_UART_RX_PIN(GPIO9) -> C61 RX0。接反了就是一个字都收不到。

// 下载模式 strapping：C61 的 IO9 在复位时为低 = 进 UART 下载模式，为高 = 正常启动。
// 本板 IO9 是 NC（没引出来），所以 P4 这边没法用 esptool 那套自动下载时序把它拉进下载模式；
// 只有飞线接到 TP5(GPIO4, DEBUG_TP5_PIN) 时才由 P4 控制。需要 P4 直接给 C61 烧固件时
// 打开下面这行即可（c61_module.cc 里按它决定要不要把 IO9 拉高）：
// #define C61_IO9_PIN                  DEBUG_TP5_PIN

// SDIO 通信接口
#define WIFI_SDIO_D3_PIN                GPIO_NUM_10
#define WIFI_SDIO_D2_PIN                GPIO_NUM_11
#define WIFI_SDIO_D1_PIN                GPIO_NUM_12
#define WIFI_SDIO_D0_PIN                GPIO_NUM_13
#define WIFI_SDIO_CLK_PIN               GPIO_NUM_14
#define WIFI_SDIO_CMD_PIN               GPIO_NUM_15

// ==========================================
// 8. QSPI Flash (U16: W25Q12RVCPJQ)
// ==========================================
// Flash 信号经 RN9/RN10 连接至 U1 (CS/Q/WP/HOLD/CK/D)。
// 在 ESP32-P4 中，Flash 挂载在专用的 MSPI (SPI0/1) 总线上，
// 硬件层直接映射，无需 (也不应) 在用户代码中配置为常规 GPIO。
// 仅做引脚占用记录：
// #define FLASH_SPI_CS                 (MSPI 专用引脚)
// #define FLASH_SPI_Q                  (MSPI 专用引脚)
// #define FLASH_SPI_WP                 (MSPI 专用引脚)
// #define FLASH_SPI_HOLD               (MSPI 专用引脚)
// #define FLASH_SPI_CK                 (MSPI 专用引脚)
// #define FLASH_SPI_D                  (MSPI 专用引脚)

#endif // _BOARD_CONFIG_H_
