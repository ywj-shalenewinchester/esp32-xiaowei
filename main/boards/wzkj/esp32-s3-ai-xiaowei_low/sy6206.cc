#include "sy6206.h"
#include "config.h"

#include <esp_log.h>
#include <driver/gpio.h>
#include <cstring>

#define TAG "Sy6206"

// 寄存器地址
static constexpr uint8_t REG00 = 0x00;  // 充电控制 & VBUS过压保护
static constexpr uint8_t REG02 = 0x02;  // 快充电流设置
static constexpr uint8_t REG04 = 0x04;  // 电池电压调节
static constexpr uint8_t REG16 = 0x16;  // 模式控制
static constexpr uint8_t REG17 = 0x17;  // ADC & 系统控制
static constexpr uint8_t REG1E = 0x1E;  // 充电 / VBUS 状态 (只读)
static constexpr uint8_t REG24 = 0x24;  // 芯片版本 & ID (只读)
static constexpr uint8_t REG27 = 0x27;  // ADC 通道使能

// ADC 数据寄存器
// VBAT ADC: LSB=2mv 量程：0mv-5000mv
static constexpr uint8_t REG28_VBAT_LO  = 0x28;
static constexpr uint8_t REG29_VBAT_HI  = 0x29;
// NTC ADC: LSB=0.0961% of REGN, 量程 0%~90%
static constexpr uint8_t REG2A_NTC_LO   = 0x2A;
static constexpr uint8_t REG2B_NTC_HI   = 0x2B;
// VBUS ADC: LSB=5mv 量程：0mv-15000mv
static constexpr uint8_t REG2C_VBUS_LO  = 0x2C;
static constexpr uint8_t REG2D_VBUS_HI  = 0x2D;
// IBAT ADC: LSB=4mA, 量程 -5000mA~4000mA (二进制补码, 正=充电, 负=放电)
static constexpr uint8_t REG2E_IBAT_LO  = 0x2E;
static constexpr uint8_t REG2F_IBAT_HI  = 0x2F;
// IBUS ADC: LSB=2mA, 量程 -2400mA~3200mA (正=VBUS→PMID, 负=PMID→VBUS)
static constexpr uint8_t REG30_IBUS_LO  = 0x30;
static constexpr uint8_t REG31_IBUS_HI  = 0x31;
//  VSYS ADC: LSB=2mV, 量程 0mV~6000mV
static constexpr uint8_t REG32_VSYS_LO  = 0x32;
static constexpr uint8_t REG33_VSYS_HI  = 0x33;
// TDIE ADC: LSB=0.5°C, 量程 -40°C~150°C
static constexpr uint8_t REG34_TDIE_LO  = 0x34;
static constexpr uint8_t REG35_TDIE_HI  = 0x35;
// PMID ADC: LSB=5mV, 量程 0mV~15000mV 
static constexpr uint8_t REG36_PMID_LO  = 0x36;
static constexpr uint8_t REG37_PMID_HI  = 0x37;

// 位域
static constexpr uint8_t REG16_CHG_EN          = (1U << 5);/* [5] 充电使能: 0=禁用, 1=使能 (需CE/为低)   */
static constexpr uint8_t REG17_ADC_EN          = (1U << 1);/* [1] ADC使能: 0=禁用, 1=使能               */
static constexpr uint8_t REG17_ADC_MODE_ONESHOT = (1U << 0);/* [0] ADC转换模式: 0=连续, 1=单次           */
static constexpr uint8_t REG1E_CHG_STAT_MASK   = (7U << 3);/* [5:3] 充电状态掩码 (000=未充,001=预充,010=CC,011=CV,100=完成) */
static constexpr uint8_t REG1E_CHG_STAT_SHIFT  = 3; /* [5:3] 充电状态移位量                         */
static constexpr uint8_t REG1E_VBUS_STAT_MASK  = (7U << 0);/* [2:0] VBUS状态掩码 (000=无,001=SDP,010=CDP,011=DCP,101=未知,110=非标,111=OTG) */
static constexpr uint8_t REG1E_VBUS_PG_STAT    = (1U << 6); /* [6] VBUS电源良好: 0=不良, 1=良好            */

// ADC 数据合并函数
uint16_t Sy6206::AdcCombine12(uint8_t hi, uint8_t lo) {
    return (uint16_t)(((hi & 0x0F) << 8) | lo);
}

// ADC 数据合并函数
int16_t Sy6206::AdcCombineSigned(uint8_t hi, uint8_t lo) {
    uint16_t mag = (uint16_t)(((hi & 0x07) << 8) | lo);
    return (int16_t)(((hi & 0x08) != 0) ? -(int16_t)mag : (int16_t)mag);
}

Sy6206::Sy6206(i2c_master_bus_handle_t i2c_bus, uint8_t addr)
    : I2cDevice(i2c_bus, addr) {
    // 配置 CE 引脚为输出 (默认拉低，允许充电)
    gpio_config_t ce_conf = {
        .pin_bit_mask = (1ULL << PMIC_CE_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&ce_conf);
    gpio_set_level(PMIC_CE_PIN, 0);

    // 配置状态引脚 (PG, STAT, INT, QON) 为输入，并开启上拉
    gpio_config_t in_conf = {
        .pin_bit_mask = (1ULL << PMIC_PG_PIN) | (1ULL << PMIC_STAT_PIN) |
                        (1ULL << PMIC_INT_PIN) | (1ULL << PMIC_QON_BUTTON_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&in_conf);

    ESP_LOGI(TAG, "Sy6206 initialized at addr 0x%02X", addr);
}

// 硬件引脚
void Sy6206::SetHardwareChargeEnable(bool enable) {
    gpio_set_level(PMIC_CE_PIN, enable ? 0 : 1);
}

bool Sy6206::IsPowerGood() {
    return gpio_get_level(PMIC_PG_PIN) == 0;
}

bool Sy6206::IsCharging() {
    return gpio_get_level(PMIC_STAT_PIN) == 0;
}

bool Sy6206::HasInterrupt() {
    return gpio_get_level(PMIC_INT_PIN) == 0;
}

// 芯片ID
esp_err_t Sy6206::ReadChipId(uint8_t *chip_id, uint8_t *revision) {
    uint8_t v = ReadReg(REG24);
    if (chip_id) {
        *chip_id = (uint8_t)((v >> 1) & 0x03);
    }
    if (revision) {
        *revision = (uint8_t)((v >> 5) & 0x07);
    }
    return ESP_OK;
}

// 充电使能
esp_err_t Sy6206::SetChargeEnable(bool enable) {
    uint8_t v = ReadReg(REG16);
    if (enable) {
        v |= REG16_CHG_EN;
    } else {
        v &= (uint8_t)~REG16_CHG_EN;
    }
    WriteReg(REG16, v);
    return ESP_OK;
}

// 充电电压
esp_err_t Sy6206::SetChargeVoltageMv(uint16_t mv) {
    uint8_t code;
    if (mv < 3500) mv = 3500;
    if (mv > 4650) mv = 4650;

    if (mv >= 4000 && mv <= 4300) {
        code = (uint8_t)((mv - 4000) / 50);
    } else if (mv >= 4310 && mv <= 4650) {
        code = (uint8_t)(0x07 + (mv - 4310) / 10);
    } else if (mv >= 3500 && mv <= 4000) {
        code = (uint8_t)(0x2A + (mv - 3500) / 50);
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t v = ReadReg(REG04);
    v = (uint8_t)((v & 0x03) | ((code & 0x3F) << 2));
    WriteReg(REG04, v);
    return ESP_OK;
}

// 充电电流
esp_err_t Sy6206::SetChargeCurrentMa(uint16_t ma) {
    uint8_t code;
    if (ma < 20) ma = 20;
    if (ma > 2020) ma = 2020;

    if (ma <= 1300) {
        code = (uint8_t)(ma / 20);
    } else {
        code = (uint8_t)(65 + (ma - 1300) / 60);
    }

    uint8_t v = ReadReg(REG02);
    v = (uint8_t)((v & 0x80) | (code & 0x7F));
    WriteReg(REG02, v);
    return ESP_OK;
}

// ADC 使能
esp_err_t Sy6206::EnableAdc(bool continuous) {
    uint8_t v = ReadReg(REG17);
    v |= REG17_ADC_EN;
    if (continuous) {
        v &= (uint8_t)~REG17_ADC_MODE_ONESHOT;
    } else {
        v |= REG17_ADC_MODE_ONESHOT;
    }
    WriteReg(REG17, v);
    WriteReg(REG27, 0xFF);
    return ESP_OK;
}

// 充电状态
esp_err_t Sy6206::GetChargeStatus(Sy6206ChargeStatus *chg, Sy6206VbusStatus *vbus, bool *vbus_pg) {
    uint8_t v = ReadReg(REG1E);
    if (chg) {
        *chg = (Sy6206ChargeStatus)((v & REG1E_CHG_STAT_MASK) >> REG1E_CHG_STAT_SHIFT);
    }
    if (vbus) {
        *vbus = (Sy6206VbusStatus)(v & REG1E_VBUS_STAT_MASK);
    }
    if (vbus_pg) {
        *vbus_pg = (v & REG1E_VBUS_PG_STAT) != 0;
    }
    return ESP_OK;
}

// ADC 读数
esp_err_t Sy6206::ReadAdcVbat(float &mv) {
    uint8_t lo = ReadReg(REG28_VBAT_LO);
    uint8_t hi = ReadReg(REG29_VBAT_HI);
    mv = AdcCombine12(hi, lo) * 2.0f;
    return ESP_OK;
}

esp_err_t Sy6206::ReadAdcVbus(float &mv) {
    uint8_t lo = ReadReg(REG2C_VBUS_LO);
    uint8_t hi = ReadReg(REG2D_VBUS_HI);
    mv = AdcCombine12(hi, lo) * 5.0f;
    return ESP_OK;
}

esp_err_t Sy6206::ReadAdcVsys(float &mv) {
    uint8_t lo = ReadReg(REG32_VSYS_LO);
    uint8_t hi = ReadReg(REG33_VSYS_HI);
    mv = AdcCombine12(hi, lo) * 2.0f;
    return ESP_OK;
}

esp_err_t Sy6206::ReadAdcPmid(float &mv) {
    uint8_t lo = ReadReg(REG36_PMID_LO);
    uint8_t hi = ReadReg(REG37_PMID_HI);
    mv = AdcCombine12(hi, lo) * 5.0f;
    return ESP_OK;
}

esp_err_t Sy6206::ReadAdcIbat(float &ma) {
    uint8_t lo = ReadReg(REG2E_IBAT_LO);
    uint8_t hi = ReadReg(REG2F_IBAT_HI);
    ma = (float)AdcCombineSigned(hi, lo) * 4.0f;
    return ESP_OK;
}

esp_err_t Sy6206::ReadAdcIbus(float &ma) {
    uint8_t lo = ReadReg(REG30_IBUS_LO);
    uint8_t hi = ReadReg(REG31_IBUS_HI);
    ma = (float)AdcCombineSigned(hi, lo) * 2.0f;
    return ESP_OK;
}

esp_err_t Sy6206::ReadAdcTdie(float &degc) {
    uint8_t lo = ReadReg(REG34_TDIE_LO);
    uint8_t hi = ReadReg(REG35_TDIE_HI);
    degc = (float)AdcCombineSigned(hi, lo) * 0.5f;
    return ESP_OK;
}

esp_err_t Sy6206::ReadAdcNtc(float &percent_regen) {
    uint8_t lo = ReadReg(REG2A_NTC_LO);
    uint8_t hi = ReadReg(REG2B_NTC_HI);
    percent_regen = AdcCombine12(hi, lo) * 0.0961f;
    return ESP_OK;
}
