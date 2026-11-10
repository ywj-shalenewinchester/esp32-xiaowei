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
    // STAT 引脚: 0=充电中; 1=空闲或已充满 (高低电平无法区分"空闲"和"充满"!,
    // 判断是否充满必须用 GetBatteryState() 的 is_full, 其基于 I2C 的 CHG_STAT 且要求 VBUS 在位)
    bool IsCharging();
    bool HasInterrupt();                         // INT=0 表示有中断

    // 寄存器读写（I2cDevice 已提供 ReadReg/WriteReg/ReadRegs）
    esp_err_t ReadChipId(uint8_t *chip_id, uint8_t *revision);

    // 充电配置
    esp_err_t SetChargeEnable(bool enable); // true: 使能充电(CE=0)
    esp_err_t SetChargeVoltageMv(uint16_t mv);  // 设置充电电压(mV)
    esp_err_t SetChargeCurrentMa(uint16_t ma);  // 设置充电电流(mA)
    esp_err_t EnableAdc(bool continuous);  // true: 使能连续转换, false: 使能单次转换
    esp_err_t GetChargeStatus(Sy6206ChargeStatus *chg, Sy6206VbusStatus *vbus, bool *vbus_pg);  // 获取充电状态和 VBUS 状态

    // 综合电池状态（含内阻补偿后的开路电压与查表电量）
    struct BatteryState {
        int percentage;    // 电量百分比 (0~100)
        float voltage_mv;  // 补偿后的开路电压 (mV)
        bool is_charging;  // 正在充电
        bool is_full;      // 已充满
    };

    esp_err_t DisableNtcCheck();  // 禁用 NTC 温度检测 (NTC 引脚悬空时必须禁用, 否则芯片拒绝充电)
    esp_err_t FeedWatchdog();     // 喂 PMIC I2C 看门狗 (超时会自动停止充电, 需周期性调用)
    esp_err_t GetBatteryState(BatteryState &state, float max_mv = 4200.0f, float min_mv = 3300.0f);

    // ---- REG20 中断标志位 (读清零寄存器) ----
    static constexpr uint8_t FLAG_ADC_DONE       = 0x80;  // [7] ADC 单次转换完成 (连续模式下忽略)
    static constexpr uint8_t FLAG_BC12_DONE      = 0x40;  // [6] BC1.2 充电器类型检测完成
    static constexpr uint8_t FLAG_THERMAL_REG   = 0x20;  // [5] 热调节 (过温降流)
    static constexpr uint8_t FLAG_QON_PRESS      = 0x10;  // [4] QON 按键动作 (短按/长按)
    static constexpr uint8_t FLAG_IINDPM         = 0x08;  // [3] 输入电流限制调节 (IINDPM)
    static constexpr uint8_t FLAG_VINDPM         = 0x04;  // [2] 输入电压限制调节 (VINDPM)
    static constexpr uint8_t FLAG_OTG_VBUS       = 0x02;  // [1] OTG 模式下 VBUS 插入
    static constexpr uint8_t FLAG_WATCHDOG       = 0x01;  // [0] 看门狗超时 (充电已被挂起)

    // ---- REG22 故障标志位 (读清零寄存器) ----
    static constexpr uint8_t FAULT_BAT_OVP      = 0x40;  // [6] 电池端过压保护 (OVP)
    static constexpr uint8_t FAULT_OTG           = 0x10;  // [4] OTG 故障 (VBAT低/Boost OCP/OVP/RCP)
    static constexpr uint8_t FAULT_INPUT        = 0x08;  // [3] 输入电源异常 (适配器过压/弱源)
    static constexpr uint8_t FAULT_TSHUT        = 0x04;  // [2] 热关断 (芯片过温)
    static constexpr uint8_t FAULT_SAFETY_TMR   = 0x02;  // [1] 安全充电定时器超时
    static constexpr uint8_t FAULT_NTC          = 0x01;  // [0] NTC 状态变化

    // 读中断/故障标志 (REG20/REG22 均为读清零, 读取后 INT 引脚自动恢复高电平)
    esp_err_t ReadInterruptFlags(uint8_t &flags, uint8_t &faults);
    // 读标志并解析触发原因打日志 (对应 demo main_old_2.cpp 的中断解析逻辑)
    esp_err_t HandleInterrupt();

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
