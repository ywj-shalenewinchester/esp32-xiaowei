#ifndef I2C_DEVICE_H
#define I2C_DEVICE_H

#include <driver/i2c_master.h>

// =================================================================
// 基础基类：I2cDevice (I2C 设备抽象类)
// 作用：负责处理 I2C 底层通信逻辑。
// =================================================================
class I2cDevice {
public:
    // 构造函数：需要传入已经初始化好的 I2C 总线句柄，以及当前这个设备的 I2C 地址
    I2cDevice(i2c_master_bus_handle_t i2c_bus, uint8_t addr);

protected:
    i2c_master_bus_handle_t i2c_bus_;  // I2C 总线的大管家（比如 I2C0）
    i2c_master_dev_handle_t i2c_device_;  // 当前这个特定设备的专有句柄
    uint8_t device_address_;              // 设备的 I2C 地址（比如 0x68）

    void WriteReg(uint8_t reg, uint8_t value);  // 向设备的某个寄存器写入 1 字节数据
    uint8_t ReadReg(uint8_t reg);               // 从设备的某个寄存器读取 1 字节数据，并返回
    void ReadRegs(uint8_t reg, uint8_t* buffer,
                  size_t length);  // 从设备的某个寄存器连续读取一串数据（存入 buffer）
    esp_err_t ResetBus(const char* reason);  // 当 I2C 总线卡死（比如 SDA线被拉低）时，可以调用这个重置总线
};

#endif  // I2C_DEVICE_H
