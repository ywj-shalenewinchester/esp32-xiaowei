#include "lsm6dsv80x.h"
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char* TAG = "Lsm6dsv80x";

Lsm6dsv80x::Lsm6dsv80x(i2c_master_bus_handle_t i2c_bus, uint8_t addr)
    : I2cDevice(i2c_bus, addr) {
}

esp_err_t Lsm6dsv80x::Init() {
    // 全部改用基类的非致命接口：失败只回错误码，不会 abort 重启芯片
    uint8_t who = 0;
    esp_err_t err = ReadRegSafe(REG_WHO_AM_I, &who);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "读取 WHO_AM_I 失败 (I2C 地址 0x%02X): %s",
                 device_address_, esp_err_to_name(err));
        return err;
    }
    if (who != WHO_AM_I_VAL) {
        ESP_LOGW(TAG, "WHO_AM_I = 0x%02X (期望 0x%02X) - 设备不匹配?", who, WHO_AM_I_VAL);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "LSM6DSV80X 在线：WHO_AM_I = 0x%02X", who);

    if ((err = WriteRegSafe(REG_CTRL3, 0x44)) != ESP_OK) return err; // BDU=1, IF_INC=1
    if ((err = WriteRegSafe(REG_CTRL1, 0x06)) != ESP_OK) return err; // 加速度 120Hz / ±2g
    if ((err = WriteRegSafe(REG_CTRL2, 0x06)) != ESP_OK) return err; // 角速度 120Hz / ±250dps

    vTaskDelay(pdMS_TO_TICKS(50));
    return ESP_OK;
}

esp_err_t Lsm6dsv80x::ReadData(float* temp_c, float acc_mg[3], float gyr_mdps[3]) {
    uint8_t raw[6] = {0};
    esp_err_t err;

    if (temp_c) {
        if ((err = ReadRegsSafe(REG_TEMP_L, raw, 2)) != ESP_OK) return err;
        int16_t t = (int16_t)(((uint16_t)raw[1] << 8) | raw[0]);
        *temp_c = (float)t / 256.0f + 25.0f; // LSM6DSV：0 LSB = 25℃
    }
    if (acc_mg) {
        if ((err = ReadRegsSafe(REG_ACCEL_L, raw, 6)) != ESP_OK) return err;
        for (int i = 0; i < 3; i++) {
            int16_t v = (int16_t)(((uint16_t)raw[i * 2 + 1] << 8) | raw[i * 2]);
            acc_mg[i] = (float)v * ACC_LSB_MG;
        }
    }
    if (gyr_mdps) {
        if ((err = ReadRegsSafe(REG_GYRO_L, raw, 6)) != ESP_OK) return err;
        for (int i = 0; i < 3; i++) {
            int16_t v = (int16_t)(((uint16_t)raw[i * 2 + 1] << 8) | raw[i * 2]);
            gyr_mdps[i] = (float)v * GYR_LSB_MDPS;
        }
    }
    return ESP_OK;
}
