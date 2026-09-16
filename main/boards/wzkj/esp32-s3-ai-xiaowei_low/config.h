#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>
#include <driver/spi_master.h>

// LED
#define R_LED_PIN                       GPIO_NUM_13     // 红色LED引脚
#define R_LED_ACTIVE_LEVEL              0               // 0：低电平点亮 1：高电平点亮
#define R_LED_BLINK_INTERVAL_MS         500             // 录音时闪烁的半周期，500ms亮+500ms灭=1Hz

// SY6206 PMIC
#define PMIC_I2C_SCL_PIN                GPIO_NUM_40
#define PMIC_I2C_SDA_PIN                GPIO_NUM_39
#define PMIC_I2C_PORT                   (I2C_NUM_0)
#define SY6206_ADDR                     0x6B

#define PMIC_PG_PIN                     GPIO_NUM_14     // /PG,绿灯 0：电源正常 1：电源异常
#define PMIC_STAT_PIN                   GPIO_NUM_47     // STAT,蓝灯 0:充电中 1:空闲或已充满(高电平≠充满, 不能单独作为充满判据!)
#define PMIC_CE_PIN                     GPIO_NUM_38     // 充电使能：CE, 0：使能 1：禁用
#define PMIC_INT_PIN                    GPIO_NUM_41     // 中断引脚

// key
#define BOOT_BUTTON_GPIO                GPIO_NUM_0      // BOOT按钮引脚
#define PMIC_QON_BUTTON_PIN             GPIO_NUM_21     // QON/按键 0：按下

// audio
#define AUDIO_INPUT_SAMPLE_RATE         24000
#define AUDIO_OUTPUT_SAMPLE_RATE        24000

#define AUDIO_ES8311_MCLK_PIN           GPIO_NUM_10
#define AUDIO_ES8311_SCLK_PIN           GPIO_NUM_18
#define AUDIO_ES8311_LRCK_PIN           GPIO_NUM_19

#define AUDIO_ES8311_DAC_DATA_PIN       GPIO_NUM_9
#define AUDIO_ES8311_ADC_DATA_PIN       GPIO_NUM_8

#define AUDIO_I2C_SCL_PIN               GPIO_NUM_11
#define AUDIO_I2C_SDA_PIN               GPIO_NUM_12
#define AUDIO_I2C_PORT                 (I2C_NUM_1)
#define AUDIO_CODEC_ES8311_ADDR          0x30

#define AUDIO_NS4150B_CTRL_PIN          GPIO_NUM_48

// H0153Y002 V1/ST77916 QSPI LCD
#define DISPLAY_WIDTH                   360
#define DISPLAY_HEIGHT                  360
#define DISPLAY_MIRROR_X                false
#define DISPLAY_MIRROR_Y                false
#define DISPLAY_SWAP_XY                 false
#define DISPLAY_OFFSET_X                0
#define DISPLAY_OFFSET_Y                0

// 圆屏UI安全区
#define DISPLAY_STATUS_BAR_TOP_OFFSET   15
#define DISPLAY_CHAT_BAR_WIDTH           250
#define DISPLAY_CHAT_BAR_BOTTOM_OFFSET  40

#define QSPI_LCD_HOST                   SPI2_HOST
#define LCD_QSPI_D0_PIN                 GPIO_NUM_16
#define LCD_QSPI_D1_PIN                 GPIO_NUM_15
#define LCD_QSPI_D2_PIN                 GPIO_NUM_7
#define LCD_QSPI_D3_PIN                 GPIO_NUM_6

#define LCD_QSPI_CLK_PIN                GPIO_NUM_5
#define LCD_CS_PIN                      GPIO_NUM_17

#define LCD_RST_PIN                     GPIO_NUM_4
#define LCD_BL_PIN                      GPIO_NUM_3

#define DISPLAY_BACKLIGHT_PIN           LCD_BL_PIN
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT false

#define WZKJ_ST77916_PANEL_BUS_QSPI_CONFIG(sclk, d0, d1, d2, d3, max_trans_sz) \
    {                                                                            \
        .data0_io_num = d0,                                                       \
        .data1_io_num = d1,                                                       \
        .sclk_io_num = sclk,                                                      \
        .data2_io_num = d2,                                                       \
        .data3_io_num = d3,                                                       \
        .max_transfer_sz = max_trans_sz,                                          \
    }

#endif // _BOARD_CONFIG_H_
