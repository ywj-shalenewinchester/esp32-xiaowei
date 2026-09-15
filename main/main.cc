#include <esp_log.h>
#include <esp_err.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_event.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "application.h"

#define TAG "main"

extern "C" void app_main(void)
{
    // 初始化NVS Flash，用于WiFi配置
    esp_err_t ret = nvs_flash_init();

    // NVS 容错处理：当检测到以下两种异常时，自动擦除并重建 NVS 分区
    // 1. ESP_ERR_NVS_NO_FREE_PAGES: NVS 分区空间耗尽或数据结构严重损坏
    // 2. ESP_ERR_NVS_NEW_VERSION_FOUND: OTA 固件升级后 NVS 数据格式版本不兼容
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS flash to fix corruption");
        ESP_ERROR_CHECK(nvs_flash_erase());     // 擦除整个 NVS 分区
        ret = nvs_flash_init();                 // 重新初始化空的 NVS 分区
    }
    ESP_ERROR_CHECK(ret);       // 检查最终初始化结果，若仍失败则打印错误信息并触发 abort() 重启设备

    // 获取 Application 单例引用
    auto& app = Application::GetInstance();
    // 执行一次性初始化：硬件外设、FreeRTOS 任务创建、事件监听注册、协议栈加载等
    app.Initialize();

    app.Run();   // 进入主事件循环
}
