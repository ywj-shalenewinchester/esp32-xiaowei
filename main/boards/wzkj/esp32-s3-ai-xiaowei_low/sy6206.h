#ifndef __SY6206_H__
#define __SY6206_H__

#include "i2c_device.h"

// SY6206 充电状态
enum Sy6206ChargeStatus {
    SY6206_CHG_NOT_CHARGING = 0,    // 未充电 (000)
    SY6206_CHG_TRICKLE_PRE  = 1,   // 涓流/预充 (001)
    SY6206_CHG_CC           = 2,   // 恒流充电 (010)
    SY6206_CHG_CV           = 3,   // 恒压充电 (011)
    SY6206_CHG_DONE         = 4,   // 充电完成 (100)
};

// SY6206 VBUS 状态
enum Sy6206VbusStatus {
    SY6206_VBUS_NONE     = 0,
    SY6206_VBUS_SDP      = 1,
    SY6206_VBUS_CDP      = 2,
    SY6206_VBUS_DCP      = 3,
    SY6206_VBUS_UNKNOWN  = 5,
    SY6206_VBUS_NONSTD   = 6,
    SY6206_VBUS_OTG      = 7,
};

/**
 * @brief SY6206 PMIC 驱动
 *
 * 与 common/sy6970 风格一致：构造时接收外部 I2C 总线 handle，
 * 由板子负责创建总线、把 PMIC 与 codec 挂到同一总线或不同总线。
 */
class Sy6206 : public I2cDevice {
public:
    Sy6206(i2c_master_bus_handle_t i2c_bus, uint8_t addr);

    // 硬件引脚（CE/PG/STAT/INT）控制
    void SetHardwareChargeEnable(bool enable);  // true: 使能充电(CE=0)
    bool IsPowerGood();                          // PG=0 表示电源正常
    bool IsCharging();                           // STAT=0 表示正在充电
    bool HasInterrupt();                         // INT=0 表示有中断

    // 寄存器读写（I2cDevice 已提供 ReadReg/WriteReg/ReadRegs）
    esp_err_t ReadChipId(uint8_t *chip_id, uint8_t *revision);

    // 充电配置
    esp_err_t SetChargeEnable(bool enable); // true: 使能充电(CE=0)
    esp_err_t SetChargeVoltageMv(uint16_t mv);  // 设置充电电压(mV)
    esp_err_t SetChargeCurrentMa(uint16_t ma);  // 设置充电电流(mA)
    esp_err_t EnableAdc(bool continuous);  // true: 使能连续转换, false: 使能单次转换
    esp_err_t GetChargeStatus(Sy6206ChargeStatus *chg, Sy6206VbusStatus *vbus, bool *vbus_pg);  // 获取充电状态和 VBUS 状态

    // ADC 读数
    esp_err_t ReadAdcVbat(float &mv);
    esp_err_t ReadAdcVbus(float &mv);
    esp_err_t ReadAdcVsys(float &mv);
    esp_err_t ReadAdcPmid(float &mv);
    esp_err_t ReadAdcIbat(float &ma);
    esp_err_t ReadAdcIbus(float &ma);
    esp_err_t ReadAdcTdie(float &degc);
    esp_err_t ReadAdcNtc(float &percent_regen);

private:
    static uint16_t AdcCombine12(uint8_t hi, uint8_t lo);
    static int16_t AdcCombineSigned(uint8_t hi, uint8_t lo);
};

#endif // __SY6206_H__
