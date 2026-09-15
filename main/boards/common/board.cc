#include "board.h"
#include "system_info.h"
#include "settings.h"
#include "display/display.h"
#include "assets/lang_config.h"

#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_chip_info.h>
#include <esp_random.h>

#define TAG "Board"

Board::Board() {     // 构造函数
    Settings settings("board", true); // 从配置文件加载设置
    uuid_ = settings.GetString("uuid"); // 从配置文件获取 UUID
    if (uuid_.empty()) {
        uuid_ = GenerateUuid(); // 如果配置文件中没有 UUID，则生成新的 UUID
        settings.SetString("uuid", uuid_); // 将新的 UUID 保存到配置文件
    }
    ESP_LOGI(TAG, "UUID=%s SKU=%s", uuid_.c_str(), BOARD_NAME); // 打印 UUID 和 SKU
}

std::string Board::GenerateUuid() { // 生成设备唯一标识
    // UUID v4 需要 16 字节的随机数据
    uint8_t uuid[16];
    
    // 使用 ESP32 的硬件随机数生成器
    esp_fill_random(uuid, sizeof(uuid));
    
    // 设置版本 (版本 4) 和变体位
    uuid[6] = (uuid[6] & 0x0F) | 0x40;    // 版本 4
    uuid[8] = (uuid[8] & 0x3F) | 0x80;    // 变体 1
    
    // 将字节转换为标准的 UUID 字符串格式
    char uuid_str[37];
    snprintf(uuid_str, sizeof(uuid_str),
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        uuid[0], uuid[1], uuid[2], uuid[3],
        uuid[4], uuid[5], uuid[6], uuid[7],
        uuid[8], uuid[9], uuid[10], uuid[11],
        uuid[12], uuid[13], uuid[14], uuid[15]);
    
    return std::string(uuid_str);
}

// 获取系统信息 JSON 字符串
std::string Board::GetSystemInfoJson() {
    /* 
        {
            "version": 2,           // 系统信息版本
            "flash_size": 4194304,  // 系统闪存大小
            "psram_size": 0,         // 系统PSRAM大小
            "minimum_free_heap_size": 123456, // 最小空闲堆大小
            "mac_address": "00:00:00:00:00:00", // 设备MAC地址
            "uuid": "00000000-0000-0000-0000-000000000000", // 设备唯一标识
            "chip_model_name": "esp32s3", // 芯片型号
            "chip_info": {          // 芯片信息
                "model": 1,           // 芯片型号
                "cores": 2,           // 芯片核心数
                "revision": 0,        // 芯片修订版本
                "features": 0         // 芯片功能
            },
            "application": {        // 应用信息
                "name": "my-app",   // 应用名称
                "version": "1.0.0", // 应用版本
                "compile_time": "2021-01-01T00:00:00Z", // 编译时间
                "idf_version": "4.2-dev", // IDF 版本
                "elf_sha256": "" // ELF 文件 SHA256 哈希
            },
            "partition_table": [ // 分区表
                "app": {                // 应用分区
                    "label": "app",     // 应用分区标签
                    "type": 1,          // 应用分区类型
                    "subtype": 2,       // 应用分区子类型
                    "address": 0x10000, // 应用分区地址
                    "size": 0x100000 // 应用分区大小
                }
            ],
            "ota": {                // OTA分区
                "label": "ota_0"    // OTA分区标签
            },
            "board": {                // 板信息
                ...
            }
        }
    */
    std::string json = R"({"version":2,"language":")" + std::string(Lang::CODE) + R"(",)";
    json += R"("flash_size":)" + std::to_string(SystemInfo::GetFlashSize()) + R"(,)";
    json += R"("minimum_free_heap_size":")" + std::to_string(SystemInfo::GetMinimumFreeHeapSize()) + R"(",)";
    json += R"("mac_address":")" + SystemInfo::GetMacAddress() + R"(",)";
    json += R"("uuid":")" + uuid_ + R"(",)";
    json += R"("chip_model_name":")" + SystemInfo::GetChipModelName() + R"(",)";

    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    json += R"("chip_info":{)";
    json += R"("model":)" + std::to_string(chip_info.model) + R"(,)";
    json += R"("cores":)" + std::to_string(chip_info.cores) + R"(,)";
    json += R"("revision":)" + std::to_string(chip_info.revision) + R"(,)";
    json += R"("features":)" + std::to_string(chip_info.features) + R"(},)";

    auto app_desc = esp_app_get_description();
    json += R"("application":{)";
    json += R"("name":")" + std::string(app_desc->project_name) + R"(",)";
    json += R"("version":")" + std::string(app_desc->version) + R"(",)";
    json += R"("compile_time":")" + std::string(app_desc->date) + R"(T)" + std::string(app_desc->time) + R"(Z",)";
    json += R"("idf_version":")" + std::string(app_desc->idf_ver) + R"(",)";
    char sha256_str[65];
    for (int i = 0; i < 32; i++) {
        snprintf(sha256_str + i * 2, sizeof(sha256_str) - i * 2, "%02x", app_desc->app_elf_sha256[i]);
    }
    json += R"("elf_sha256":")" + std::string(sha256_str) + R"(")";
    json += R"(},)";

    json += R"("partition_table": [)";
    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
    while (it) {
        const esp_partition_t *partition = esp_partition_get(it);
        json += R"({)";
        json += R"("label":")" + std::string(partition->label) + R"(",)";
        json += R"("type":)" + std::to_string(partition->type) + R"(,)";
        json += R"("subtype":)" + std::to_string(partition->subtype) + R"(,)";
        json += R"("address":)" + std::to_string(partition->address) + R"(,)";
        json += R"("size":)" + std::to_string(partition->size) + R"(},)";;
        it = esp_partition_next(it);
    }
    json.pop_back(); // Remove the last comma
    json += R"(],)";

    json += R"("ota":{)";
    auto ota_partition = esp_ota_get_running_partition();
    json += R"("label":")" + std::string(ota_partition->label) + R"(")";
    json += R"(},)";

    // Append display info
    auto display = GetDisplay();
    if (display) {
        json += R"("display":{)";
        json += R"("monochrome":)" + std::string("false") + R"(,)";
        json += R"("width":)" + std::to_string(display->width()) + R"(,)";
        json += R"("height":)" + std::to_string(display->height()) + R"(,)";
        json.pop_back(); // Remove the last comma
    }
    json += R"(},)";

    json += R"("board":)" + GetBoardJson();

    // Close the JSON object
    json += R"(})";
    return json;
}
