#include "i2c_device.h"

#include <esp_log.h>

#define TAG "I2cDevice"

// ---------------------------------------------------------
// 初始化：将设备挂载到总线上
// ---------------------------------------------------------
I2cDevice::I2cDevice(i2c_master_bus_handle_t i2c_bus, uint8_t addr)
    : i2c_bus_(i2c_bus), device_address_(addr) {
    // 1. 配置 I2C 设备的具体参数
    i2c_device_config_t i2c_device_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,  // I2C 设备地址长度通常都是 7 位
        .device_address = addr,                 // 传入的设备地址
        .scl_speed_hz = 400 * 1000,             // 通信速度：400 kHz (Fast Mode 快速模式)
        .scl_wait_us = 0,                       // 时钟拉伸等待时间（0表示使用默认值）
        .flags =
            {
                .disable_ack_check = 0,  // 开启 ACK 应答检查（确保设备真的收到了数据）
            },
    };
    // 2. 将这个设备注册到 I2C 总线上，并获取该设备的专属控制句柄 (i2c_device_)
    ESP_ERROR_CHECK(i2c_master_bus_add_device(i2c_bus, &i2c_device_cfg, &i2c_device_));
    // 确保设备句柄不为空，否则程序直接报错停机
    assert(i2c_device_ != NULL);
}

// ---------------------------------------------------------
// 写寄存器操作 (I2C 协议：主机发地址 -> 主机发数据)
// ---------------------------------------------------------
void I2cDevice::WriteReg(uint8_t reg, uint8_t value) {
    // I2C 写寄存器的标准时序是：连续发送 [寄存器地址] 和 [要写入的值]
    uint8_t buffer[2] = {reg, value};
    // 调用底层发送接口
    // 参数说明：设备句柄, 要发送的数据数组, 数据长度(2), 超时时间(100ms)
    ESP_ERROR_CHECK(i2c_master_transmit(i2c_device_, buffer, 2, 100));
}

// ---------------------------------------------------------
// 读单字节寄存器操作 (I2C 协议：主机发寄存器地址 -> 设备回传数据)
// ---------------------------------------------------------
uint8_t I2cDevice::ReadReg(uint8_t reg) {
    uint8_t buffer[1];
    // 时序：START -> 发设备地址(写) -> 发寄存器地址(reg) -> RESTART -> 发设备地址(读) -> 接收 1
    // 字节 -> STOP 这个 API 自动帮你把上面一长串复杂的时序处理了！ 100 是 100 毫秒的超时时间
    ESP_ERROR_CHECK(i2c_master_transmit_receive(i2c_device_, &reg, 1, buffer, 1, 100));
    return buffer[0];
}

// ---------------------------------------------------------
// 读多字节寄存器操作（常用于连续读取大量数据，如传感器缓冲）
// ---------------------------------------------------------
void I2cDevice::ReadRegs(uint8_t reg, uint8_t* buffer, size_t length) {
    // 原理同上，只是把接收长度变成了 length，数据直接存入外界传入的 buffer 数组中
    ESP_ERROR_CHECK(i2c_master_transmit_receive(i2c_device_, &reg, 1, buffer, length, 100));
}

// ---------------------------------------------------------
// 异常处理：总线重置
// ---------------------------------------------------------
esp_err_t I2cDevice::ResetBus(const char* reason) {
    ESP_LOGW(TAG, "Resetting I2C bus: %s", reason ? reason : "unspecified");
    
    // 调用底层 API 重置整个 I2C 总线状态机
    return i2c_master_bus_reset(i2c_bus_);
}
