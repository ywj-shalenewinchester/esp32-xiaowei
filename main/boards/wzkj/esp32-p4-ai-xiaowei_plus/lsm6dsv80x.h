#ifndef __LSM6DSV80X_H__
#define __LSM6DSV80X_H__

#include "i2c_device.h"
#include <esp_err.h>

class Lsm6dsv80x : public I2cDevice {
public:
    // 默认 I2C 地址为 0x6A (由 SA0 引脚决定)
    Lsm6dsv80x(i2c_master_bus_handle_t i2c_bus, uint8_t addr = 0x6A);

    // 初始化：读 WHO_AM_I，配 120Hz / ±2g / ±250dps
    esp_err_t Init();

    // 读一次数据：温度(℃) / 三轴加速度(mg) / 三轴角速度(mdps)
    // 传 nullptr 表示不读取该项数据
    esp_err_t ReadData(float* temp_c, float acc_mg[3], float gyr_mdps[3]);

private:
    static constexpr uint8_t REG_WHO_AM_I = 0x0F;
    static constexpr uint8_t REG_CTRL1    = 0x10;
    static constexpr uint8_t REG_CTRL2    = 0x11;
    static constexpr uint8_t REG_CTRL3    = 0x12;
    static constexpr uint8_t REG_TEMP_L   = 0x20;
    static constexpr uint8_t REG_GYRO_L   = 0x22;
    static constexpr uint8_t REG_ACCEL_L  = 0x28;

    static constexpr uint8_t WHO_AM_I_VAL = 0x73;
    static constexpr float ACC_LSB_MG     = 0.061f; // ±2g 档
    static constexpr float GYR_LSB_MDPS   = 8.75f;  // ±250dps 档
};

#endif // __LSM6DSV80X_H__