#ifndef __SY6206_H__
#define __SY6206_H__

#include "i2c_device.h"

// SY6206 充电状态
enum Sy6206ChargeStatus
{
    SY6206_CHG_NOT_CHARGING = 0, // 未充电 (000)
    SY6206_CHG_TRICKLE_PRE = 1,  // 涓流/预充 (001)
    SY6206_CHG_CC = 2,           // 恒流充电 (010)
    SY6206_CHG_CV = 3,           // 恒压充电 (011)
    SY6206_CHG_DONE = 4,         // 充电完成 (100)
};

// SY6206 VBUS 状态
enum Sy6206VbusStatus
{
    SY6206_VBUS_NONE = 0,
    SY6206_VBUS_SDP = 1,
    SY6206_VBUS_CDP = 2,
    SY6206_VBUS_DCP = 3,
    SY6206_VBUS_UNKNOWN = 5,
    SY6206_VBUS_NONSTD = 6,
    SY6206_VBUS_OTG = 7,
};

class Sy6206 : public I2cDevice
{
public:
    Sy6206(i2c_master_bus_handle_t i2c_bus, uint8_t addr);

    // 硬件引脚控制 (已移除 CE 和 PG，仅保留 STAT 和 INT)
    // STAT 引脚: 0=充电中; 1=空闲或已充满
    bool IsCharging();
    // INT 引脚: 0=有中断
    bool HasInterrupt();

    // 寄存器读写统一走 I2cDevice 的非致命接口 ReadRegSafe / WriteRegSafe
    //（基类的 ReadReg/WriteReg 内含 ESP_ERROR_CHECK，I2C 没应答就整机重启，本驱动不用）
    esp_err_t ReadChipId(uint8_t *chip_id, uint8_t *revision);

    // 充电配置
    esp_err_t SetChargeEnable(bool enable);
    esp_err_t SetChargeVoltageMv(uint16_t mv);
    esp_err_t SetChargeCurrentMa(uint16_t ma);
    esp_err_t EnableAdc(bool continuous);
    esp_err_t GetChargeStatus(Sy6206ChargeStatus *chg, Sy6206VbusStatus *vbus, bool *vbus_pg);

    // 电池综合状态结构体 (包含6个参数)
    struct BatteryState
    {
        int percentage;        // 电量百分比 (0~100)
        float voltage_mv;      // 当前电池电压 (mV)
        float ibat_ma;         // 当前电池电流 (mA)
        float voltage_mv_comp; // 补偿电压 (mV)
        bool is_charging;      // 是否正在充电
        bool is_full;          // 是否已经充满
    };

    esp_err_t DisableNtcCheck();
    esp_err_t FeedWatchdog();
    esp_err_t GetBatteryState(BatteryState &state, float max_mv = 4200.0f, float min_mv = 3300.0f);

    // ---- REG20 中断标志位 (读清零寄存器) ----
    static constexpr uint8_t FLAG_ADC_DONE = 0x80;
    static constexpr uint8_t FLAG_BC12_DONE = 0x40;
    static constexpr uint8_t FLAG_THERMAL_REG = 0x20;
    static constexpr uint8_t FLAG_QON_PRESS = 0x10;
    static constexpr uint8_t FLAG_IINDPM = 0x08;
    static constexpr uint8_t FLAG_VINDPM = 0x04;
    static constexpr uint8_t FLAG_OTG_VBUS = 0x02;
    static constexpr uint8_t FLAG_WATCHDOG = 0x01;

    // ---- REG22 故障标志位 (读清零寄存器) ----
    static constexpr uint8_t FAULT_BAT_OVP = 0x40;
    static constexpr uint8_t FAULT_OTG = 0x10;
    static constexpr uint8_t FAULT_INPUT = 0x08;
    static constexpr uint8_t FAULT_TSHUT = 0x04;
    static constexpr uint8_t FAULT_SAFETY_TMR = 0x02;
    static constexpr uint8_t FAULT_NTC = 0x01;

    esp_err_t ReadInterruptFlags(uint8_t &flags, uint8_t &faults);
    esp_err_t HandleInterrupt();
    // 打印充电配置和实时状态，用于与正常工程逐寄存器对比。
    esp_err_t LogChargeDiagnostics();

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
    // HandleInterrupt 每 2 秒跑一次。REG20/REG22 里有些位是**持续置位**的
    // （例如 VBUS 在位），每次都报会把控制台刷满，所以只在状态变化时打印。
    uint8_t last_int_flags_ = 0;
    uint8_t last_int_faults_ = 0;
    // 读失败同样只在"正常 -> 失败"翻转时报一次，避免 I2C 掉线时每 2 秒刷一行。
    bool int_read_failed_ = false;

    static uint16_t AdcCombine12(uint8_t hi, uint8_t lo);
    static int16_t AdcCombineSigned(uint8_t hi, uint8_t lo);
    // 读一对 (低字节, 高字节) 的 12 位无符号 ADC 寄存器，失败原样返回错误码
    esp_err_t ReadAdcPair(uint8_t reg_lo, uint8_t reg_hi, uint16_t &raw);
};

#endif // __SY6206_H__
