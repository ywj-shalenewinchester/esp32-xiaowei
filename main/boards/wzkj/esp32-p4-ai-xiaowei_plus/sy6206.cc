// ============================================================================
//  Sy6206 —— SY6206 电源管理（充电 + 电量计）驱动
//
//  错误处理约定：全部走 I2cDevice 的 **非致命** 接口（ReadRegSafe / WriteRegSafe）。
//  基类的 ReadReg/WriteReg 内部是 ESP_ERROR_CHECK，I2C 一旦没应答就 abort -> panic
//  -> 整芯片重启。本驱动有两处会在运行期被周期性调用（2s 喂狗定时器里的
//  FeedWatchdog/HandleInterrupt，以及框架轮询的 GetBatteryLevel），拿这种接口去读
//  等于把一次 I2C 抖动放大成整机重启，所以这里逐个把错误码往上报，让调用方决定
//  "这一轮跳过"。
// ============================================================================

#include "sy6206.h"
#include "config.h"

#include <esp_log.h>
#include <driver/gpio.h>
#include <cstring>

#define TAG "Sy6206"

// 寄存器地址
static constexpr uint8_t REG02 = 0x02;
static constexpr uint8_t REG04 = 0x04;
static constexpr uint8_t REG16 = 0x16;
static constexpr uint8_t REG16_WDT_RST = (1U << 2);
static constexpr uint8_t REG17 = 0x17;
static constexpr uint8_t REG1A = 0x1A;
static constexpr uint8_t REG1A_TS_DIS = (1U << 7);
static constexpr uint8_t REG1E = 0x1E;
static constexpr uint8_t REG20 = 0x20;
static constexpr uint8_t REG22 = 0x22;
static constexpr uint8_t REG24 = 0x24;
static constexpr uint8_t REG27 = 0x27;

// ADC 数据寄存器
static constexpr uint8_t REG28_VBAT_LO = 0x28;
static constexpr uint8_t REG29_VBAT_HI = 0x29;
static constexpr uint8_t REG2A_NTC_LO = 0x2A;
static constexpr uint8_t REG2B_NTC_HI = 0x2B;
static constexpr uint8_t REG2C_VBUS_LO = 0x2C;
static constexpr uint8_t REG2D_VBUS_HI = 0x2D;
static constexpr uint8_t REG2E_IBAT_LO = 0x2E;
static constexpr uint8_t REG2F_IBAT_HI = 0x2F;
static constexpr uint8_t REG30_IBUS_LO = 0x30;
static constexpr uint8_t REG31_IBUS_HI = 0x31;
static constexpr uint8_t REG32_VSYS_LO = 0x32;
static constexpr uint8_t REG33_VSYS_HI = 0x33;
static constexpr uint8_t REG34_TDIE_LO = 0x34;
static constexpr uint8_t REG35_TDIE_HI = 0x35;
static constexpr uint8_t REG36_PMID_LO = 0x36;
static constexpr uint8_t REG37_PMID_HI = 0x37;

// 位域
static constexpr uint8_t REG16_CHG_EN = (1U << 5);
static constexpr uint8_t REG17_ADC_EN = (1U << 1);
static constexpr uint8_t REG17_ADC_MODE_ONESHOT = (1U << 0);
static constexpr uint8_t REG1E_CHG_STAT_MASK = (7U << 3);
static constexpr uint8_t REG1E_CHG_STAT_SHIFT = 3;
static constexpr uint8_t REG1E_VBUS_STAT_MASK = (7U << 0);
static constexpr uint8_t REG1E_VBUS_PG_STAT = (1U << 6);

// ============================================================================
//  内部小工具
// ============================================================================

uint16_t Sy6206::AdcCombine12(uint8_t hi, uint8_t lo)
{
    return (uint16_t)(((hi & 0x0F) << 8) | lo);
}

int16_t Sy6206::AdcCombineSigned(uint8_t hi, uint8_t lo)
{
    uint16_t raw = (uint16_t)(((hi & 0x0F) << 8) | lo);
    if (raw & 0x0800)
    {
        raw |= 0xF000;
    }
    return (int16_t)raw;
}

// 读一对 (低字节, 高字节) 的 ADC 寄存器，失败就把错误码原样往上报
esp_err_t Sy6206::ReadAdcPair(uint8_t reg_lo, uint8_t reg_hi, uint16_t &raw)
{
    uint8_t lo = 0, hi = 0;
    esp_err_t err = ReadRegSafe(reg_lo, &lo);
    if (err != ESP_OK)
    {
        return err;
    }
    err = ReadRegSafe(reg_hi, &hi);
    if (err != ESP_OK)
    {
        return err;
    }
    raw = AdcCombine12(hi, lo);
    return ESP_OK;
}

// ============================================================================
//  构造
// ============================================================================

Sy6206::Sy6206(i2c_master_bus_handle_t i2c_bus, uint8_t addr)
    : I2cDevice(i2c_bus, addr)
{

    // 移除了 CE 和 PG 引脚，只配置 STAT, INT 和 QON 作为输入
    gpio_config_t in_conf = {
        .pin_bit_mask = (1ULL << PMIC_STAT_PIN) |
                        (1ULL << PMIC_INT_PIN) | (1ULL << PMIC_QON_BUTTON_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&in_conf);

    ESP_LOGI(TAG, "Sy6206 initialized at addr 0x%02X", addr);
}

bool Sy6206::IsCharging()
{
    return gpio_get_level(PMIC_STAT_PIN) == 0;
}

bool Sy6206::HasInterrupt()
{
    return gpio_get_level(PMIC_INT_PIN) == 0;
}

// ============================================================================
//  配置类
// ============================================================================

esp_err_t Sy6206::ReadChipId(uint8_t *chip_id, uint8_t *revision)
{
    uint8_t v = 0;
    esp_err_t err = ReadRegSafe(REG24, &v);
    if (err != ESP_OK)
    {
        return err;
    }
    if (chip_id)
    {
        *chip_id = (uint8_t)((v >> 1) & 0x03);
    }
    if (revision)
    {
        *revision = (uint8_t)((v >> 5) & 0x07);
    }
    return ESP_OK;
}

esp_err_t Sy6206::SetChargeEnable(bool enable)
{
    uint8_t v = 0;
    esp_err_t err = ReadRegSafe(REG16, &v);
    if (err != ESP_OK)
    {
        return err;
    }
    if (enable)
    {
        v |= REG16_CHG_EN;
    }
    else
    {
        v &= (uint8_t)~REG16_CHG_EN;
    }
    return WriteRegSafe(REG16, v);
}

esp_err_t Sy6206::SetChargeVoltageMv(uint16_t mv)
{
    uint8_t code;
    if (mv < 3500)
        mv = 3500;
    if (mv > 4650)
        mv = 4650;

    if (mv >= 4000 && mv <= 4300)
    {
        code = (uint8_t)((mv - 4000) / 50);
    }
    else if (mv >= 4310 && mv <= 4650)
    {
        code = (uint8_t)(0x07 + (mv - 4310) / 10);
    }
    else if (mv >= 3500 && mv <= 4000)
    {
        code = (uint8_t)(0x2A + (mv - 3500) / 50);
    }
    else
    {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t v = 0;
    esp_err_t err = ReadRegSafe(REG04, &v);
    if (err != ESP_OK)
    {
        return err;
    }
    v = (uint8_t)((v & 0x03) | ((code & 0x3F) << 2));
    return WriteRegSafe(REG04, v);
}

esp_err_t Sy6206::SetChargeCurrentMa(uint16_t ma)
{
    uint8_t code;
    if (ma < 20)
        ma = 20;
    if (ma > 2020)
        ma = 2020;

    if (ma <= 1300)
    {
        code = (uint8_t)(ma / 20);
    }
    else
    {
        code = (uint8_t)(65 + (ma - 1300) / 60);
    }

    uint8_t v = 0;
    esp_err_t err = ReadRegSafe(REG02, &v);
    if (err != ESP_OK)
    {
        return err;
    }
    v = (uint8_t)((v & 0x80) | (code & 0x7F));
    return WriteRegSafe(REG02, v);
}

esp_err_t Sy6206::EnableAdc(bool continuous)
{
    uint8_t v = 0;
    esp_err_t err = ReadRegSafe(REG17, &v);
    if (err != ESP_OK)
    {
        return err;
    }
    v |= REG17_ADC_EN;
    if (continuous)
    {
        v &= (uint8_t)~REG17_ADC_MODE_ONESHOT;
    }
    else
    {
        v |= REG17_ADC_MODE_ONESHOT;
    }
    err = WriteRegSafe(REG17, v);
    if (err != ESP_OK)
    {
        return err;
    }
    return WriteRegSafe(REG27, 0xFF);
}

esp_err_t Sy6206::DisableNtcCheck()
{
    uint8_t v = 0;
    esp_err_t err = ReadRegSafe(REG1A, &v);
    if (err != ESP_OK)
    {
        return err;
    }
    return WriteRegSafe(REG1A, v | REG1A_TS_DIS);
}

esp_err_t Sy6206::FeedWatchdog()
{
    uint8_t v = 0;
    esp_err_t err = ReadRegSafe(REG16, &v);
    if (err != ESP_OK)
    {
        return err;
    }
    return WriteRegSafe(REG16, v | REG16_WDT_RST);
}

// ============================================================================
//  状态读取
// ============================================================================

esp_err_t Sy6206::GetChargeStatus(Sy6206ChargeStatus *chg, Sy6206VbusStatus *vbus, bool *vbus_pg)
{
    uint8_t v = 0;
    esp_err_t err = ReadRegSafe(REG1E, &v);
    if (err != ESP_OK)
    {
        return err;
    }
    if (chg)
    {
        *chg = (Sy6206ChargeStatus)((v & REG1E_CHG_STAT_MASK) >> REG1E_CHG_STAT_SHIFT);
    }
    if (vbus)
    {
        *vbus = (Sy6206VbusStatus)(v & REG1E_VBUS_STAT_MASK);
    }
    if (vbus_pg)
    {
        *vbus_pg = (v & REG1E_VBUS_PG_STAT) != 0;
    }
    return ESP_OK;
}

esp_err_t Sy6206::ReadInterruptFlags(uint8_t &flags, uint8_t &faults)
{
    // REG20/REG22 是"读清零"寄存器：读一次就清掉中断并放开 INT 引脚。
    // 所以第一句读失败时不能拿默认值往下走，否则会误判成"有中断"。
    esp_err_t err = ReadRegSafe(REG20, &flags);
    if (err != ESP_OK)
    {
        return err;
    }
    return ReadRegSafe(REG22, &faults);
}

esp_err_t Sy6206::HandleInterrupt()
{
    uint8_t flags = 0, faults = 0;
    esp_err_t err = ReadInterruptFlags(flags, faults);
    if (err != ESP_OK)
    {
        // 这一轮读不到就跳过。本函数跑在 2s 的 PMIC 定时器里（和喂狗同一拍），
        // 单次 I2C 失败只意味着"这次没看到中断原因"，绝不该升级成整机重启。
        // 但也不能每 2 秒刷一行：只在"正常 -> 失败"翻转时报一次。
        if (!int_read_failed_) {
            int_read_failed_ = true;
            ESP_LOGW(TAG, "读 PMIC 中断标志失败: %s（持续失败不再重复打印）",
                     esp_err_to_name(err));
        }
        return err;
    }
    if (int_read_failed_) {
        int_read_failed_ = false;
        ESP_LOGI(TAG, "PMIC 中断标志读取恢复正常");
    }

    flags &= (uint8_t)~FLAG_ADC_DONE;

    // 关键：只有状态相对上一次**发生变化**时才打印。
    // REG20/REG22 里有些位是持续置位的（例如 VBUS 在位时 FLAG_OTG_VBUS），
    // 2 秒轮询如果每次照打，控制台就会被同一行永久刷屏。
    if (flags == last_int_flags_ && faults == last_int_faults_) {
        return ESP_OK;
    }
    const uint8_t changed_flags  = (uint8_t)(flags  ^ last_int_flags_);
    const uint8_t changed_faults = (uint8_t)(faults ^ last_int_faults_);
    const bool had_fault = (last_int_faults_ != 0);
    last_int_flags_  = flags;
    last_int_faults_ = faults;

    if (flags == 0 && faults == 0) {
        // 全部恢复：只说一句，不再逐位解释。
        if (had_fault) ESP_LOGI(TAG, "PMIC 故障已清除，状态恢复正常");
        return ESP_OK;
    }

    // IINDPM/VINDPM 是调节状态，不是故障；只有 REG22 非零才标为故障。
    if (faults != 0) {
        ESP_LOGW(TAG, "[PMIC中断] 标志(REG20): 0x%02X | 故障(REG22): 0x%02X", flags, faults);
    } else {
        ESP_LOGI(TAG, "[PMIC状态] 标志(REG20): 0x%02X | 故障(REG22): 0x00", flags);
    }

    // 只解释**这次新出现**的位（changed & flags），一直存在的位不重复解释。
    // IINDPM/VINDPM 只是 PMIC 的内部调节状态，不在中断里打印。
    const uint8_t new_flags = (uint8_t)(changed_flags & flags);
    if (new_flags & FLAG_WATCHDOG)
        ESP_LOGE(TAG, "  └─ 看门狗超时！");
    if (new_flags & FLAG_QON_PRESS)
        ESP_LOGI(TAG, "  └─ QON 按键动作");
    if (new_flags & FLAG_THERMAL_REG)
        ESP_LOGW(TAG, "  └─ 芯片过热");
    if (new_flags & FLAG_OTG_VBUS)
        ESP_LOGI(TAG, "  └─ VBUS 插入/拔出");
    if (new_flags & FLAG_BC12_DONE)
        ESP_LOGI(TAG, "  └─ BC1.2 检测完成");

    const uint8_t new_faults = (uint8_t)(changed_faults & faults);
    if (new_faults & FAULT_NTC)
        ESP_LOGE(TAG, "  └─ NTC 故障");
    if (new_faults & FAULT_BAT_OVP)
        ESP_LOGE(TAG, "  └─ 电池过压");
    if (new_faults & FAULT_INPUT)
        ESP_LOGE(TAG, "  └─ 输入故障");
    if (new_faults & FAULT_SAFETY_TMR)
        ESP_LOGE(TAG, "  └─ 定时器超时");
    if (new_faults & FAULT_TSHUT)
        ESP_LOGE(TAG, "  └─ 过温关断");
    if (changed_faults && faults == 0)
        ESP_LOGI(TAG, "  └─ 故障已清除");

    return ESP_OK;
}

esp_err_t Sy6206::LogChargeDiagnostics()
{
    uint8_t r00=0, r02=0, r04=0, r16=0, r1a=0, r1e=0;
    esp_err_t err = ReadRegSafe(0x00, &r00); if (err != ESP_OK) return err;
    err = ReadRegSafe(0x02, &r02); if (err != ESP_OK) return err;
    err = ReadRegSafe(0x04, &r04); if (err != ESP_OK) return err;
    err = ReadRegSafe(0x16, &r16); if (err != ESP_OK) return err;
    err = ReadRegSafe(0x1A, &r1a); if (err != ESP_OK) return err;
    err = ReadRegSafe(0x1E, &r1e); if (err != ESP_OK) return err;
    float vbus=0, ibus=0, vbat=0, vsys=0, tdie=0;
    ReadAdcVbus(vbus); ReadAdcIbus(ibus); ReadAdcVbat(vbat); ReadAdcVsys(vsys); ReadAdcTdie(tdie);
    ESP_LOGI(TAG, "充电诊断: R00=0x%02X R02=0x%02X R04=0x%02X R16=0x%02X R1A=0x%02X R1E=0x%02X | VBUS=%.0fmV IBUS=%.0fmA VBAT=%.0fmV VSYS=%.0fmV TDIE=%.1fC",
             r00,r02,r04,r16,r1a,r1e,vbus,ibus,vbat,vsys,tdie);
    return ESP_OK;
}

esp_err_t Sy6206::GetBatteryState(BatteryState &state, float max_mv, float min_mv)
{
    // 1. 读取当前电池端电压
    float vbat = 0;
    esp_err_t err = ReadAdcVbat(vbat);
    if (err != ESP_OK)
        return err;
    state.voltage_mv = vbat;

    // 2. 读取当前电池电流 (正数=充电，负数=放电)
    float ibat = 0;
    err = ReadAdcIbat(ibat);
    if (err != ESP_OK)
        return err;
    state.ibat_ma = ibat;

    // 3. 内阻补偿，计算真实的开路电压 (OCV)
    static constexpr float SYSTEM_RESISTANCE_OHM = 0.32f;
    float real_ocv_mv = vbat - (ibat * SYSTEM_RESISTANCE_OHM);
    state.voltage_mv_comp = real_ocv_mv;

    // 4. 读取芯片充电状态和VBUS状态
    Sy6206ChargeStatus chg;
    Sy6206VbusStatus vbus;
    bool vbus_pg;
    err = GetChargeStatus(&chg, &vbus, &vbus_pg);
    if (err != ESP_OK)
    {
        return err;
    }

    bool vbus_present = (vbus != SY6206_VBUS_NONE) || vbus_pg;
    state.is_charging = vbus_present && (chg == SY6206_CHG_TRICKLE_PRE || chg == SY6206_CHG_CC || chg == SY6206_CHG_CV);
    state.is_full = vbus_present && (chg == SY6206_CHG_DONE);

    // 5. 计算电量百分比
    if (state.is_full)
    {
        state.percentage = 100;
        return ESP_OK;
    }

    float ratio = (real_ocv_mv - min_mv) / (max_mv - min_mv);
    if (ratio < 0.0f)
        ratio = 0.0f;
    if (ratio > 1.0f)
        ratio = 1.0f;
    float norm_v = 3300.0f + ratio * (4200.0f - 3300.0f);

    static constexpr float voltage_table[] = {4200, 4060, 3980, 3920, 3870, 3820, 3790, 3770, 3740, 3680, 3450, 3300};
    static constexpr int percent_table[] = {100, 90, 80, 70, 60, 50, 40, 30, 20, 10, 5, 0};
    constexpr int table_size = sizeof(percent_table) / sizeof(percent_table[0]);

    int pct = 0;
    if (norm_v >= voltage_table[0])
    {
        pct = 100;
    }
    else if (norm_v <= voltage_table[table_size - 1])
    {
        pct = 0;
    }
    else
    {
        for (int i = 0; i < table_size - 1; i++)
        {
            if (norm_v <= voltage_table[i] && norm_v > voltage_table[i + 1])
            {
                float v_diff = voltage_table[i] - voltage_table[i + 1];
                float c_diff = norm_v - voltage_table[i + 1];
                int p_diff = percent_table[i] - percent_table[i + 1];
                pct = percent_table[i + 1] + (int)((c_diff / v_diff) * p_diff);
                break;
            }
        }
    }

    // 充电极化修正
    if (state.is_charging && pct == 100)
    {
        pct = 99;
    }

    state.percentage = pct;
    return ESP_OK;
}

// ============================================================================
//  ADC 读取
//
//  每对都是 (低字节寄存器, 高字节寄存器) 连续两读，统一走 ReadAdcPair。
// ============================================================================

esp_err_t Sy6206::ReadAdcVbat(float &mv)
{
    uint16_t raw = 0;
    esp_err_t err = ReadAdcPair(REG28_VBAT_LO, REG29_VBAT_HI, raw);
    if (err != ESP_OK)
    {
        return err;
    }
    mv = raw * 2.0f;
    return ESP_OK;
}

esp_err_t Sy6206::ReadAdcVbus(float &mv)
{
    uint16_t raw = 0;
    esp_err_t err = ReadAdcPair(REG2C_VBUS_LO, REG2D_VBUS_HI, raw);
    if (err != ESP_OK)
    {
        return err;
    }
    mv = raw * 5.0f;
    return ESP_OK;
}

esp_err_t Sy6206::ReadAdcVsys(float &mv)
{
    uint16_t raw = 0;
    esp_err_t err = ReadAdcPair(REG32_VSYS_LO, REG33_VSYS_HI, raw);
    if (err != ESP_OK)
    {
        return err;
    }
    mv = raw * 2.0f;
    return ESP_OK;
}

esp_err_t Sy6206::ReadAdcPmid(float &mv)
{
    uint16_t raw = 0;
    esp_err_t err = ReadAdcPair(REG36_PMID_LO, REG37_PMID_HI, raw);
    if (err != ESP_OK)
    {
        return err;
    }
    mv = raw * 5.0f;
    return ESP_OK;
}

esp_err_t Sy6206::ReadAdcIbat(float &ma)
{
    // 电流是**有符号**量（正=充电，负=放电），所以不能走 ReadAdcPair 的 12 位无符号合并
    uint8_t lo = 0, hi = 0;
    esp_err_t err = ReadRegSafe(REG2E_IBAT_LO, &lo);
    if (err != ESP_OK)
    {
        return err;
    }
    err = ReadRegSafe(REG2F_IBAT_HI, &hi);
    if (err != ESP_OK)
    {
        return err;
    }
    ma = (float)AdcCombineSigned(hi, lo) * 4.0f;
    return ESP_OK;
}

esp_err_t Sy6206::ReadAdcIbus(float &ma)
{
    uint8_t lo = 0, hi = 0;
    esp_err_t err = ReadRegSafe(REG30_IBUS_LO, &lo);
    if (err != ESP_OK)
    {
        return err;
    }
    err = ReadRegSafe(REG31_IBUS_HI, &hi);
    if (err != ESP_OK)
    {
        return err;
    }
    ma = (float)AdcCombineSigned(hi, lo) * 2.0f;
    return ESP_OK;
}

esp_err_t Sy6206::ReadAdcTdie(float &degc)
{
    // 管芯温度同样是**有符号**量（可能低于 0℃）
    uint8_t lo = 0, hi = 0;
    esp_err_t err = ReadRegSafe(REG34_TDIE_LO, &lo);
    if (err != ESP_OK)
    {
        return err;
    }
    err = ReadRegSafe(REG35_TDIE_HI, &hi);
    if (err != ESP_OK)
    {
        return err;
    }
    degc = (float)AdcCombineSigned(hi, lo) * 0.5f;
    return ESP_OK;
}

esp_err_t Sy6206::ReadAdcNtc(float &percent_regen)
{
    uint16_t raw = 0;
    esp_err_t err = ReadAdcPair(REG2A_NTC_LO, REG2B_NTC_HI, raw);
    if (err != ESP_OK)
    {
        return err;
    }
    percent_regen = raw * 0.0961f;
    return ESP_OK;
}
