#include "wifi_board.h"

#include "display.h"
#include "application.h"
#include "system_info.h"
#include "settings.h"
#include "assets/lang_config.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_network.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <utility>

#include <material_symbols.h>
#include <wifi_manager.h>
#include <wifi_station.h>
#include <ssid_manager.h>

static const char *TAG = "WifiBoard";

// 连接超时时间，单位秒，默认 60 秒，超时强制进入配网模式
static constexpr int CONNECT_TIMEOUT_SEC = 60;

WifiBoard::WifiBoard() {
    // 创建连接超时定时器
    esp_timer_create_args_t timer_args = {
        .callback = OnWifiConnectTimeout,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wifi_connect_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&timer_args, &connect_timer_); // 创建连接超时定时器
}

WifiBoard::~WifiBoard() {
    if (connect_timer_) {
        // 如果连接超时定时器存在，停止并删除定时器
        esp_timer_stop(connect_timer_);
        esp_timer_delete(connect_timer_);
    }
}

// 获取板子类型，返回 "wifi"
std::string WifiBoard::GetBoardType() {
    return "wifi";
}

// 异步启动网络
// 该函数会立即返回。网络状态会通过 SetNetworkEventCallback 注册的回调函数异步通知
// 该函数会自动处理连接超时，超时后会进入配网模式
void WifiBoard::StartNetwork() {
    auto& wifi_manager = WifiManager::GetInstance();

    // 初始化 WiFi 管理器
    WifiManagerConfig config;
    // ** 配置 SSID 前缀，用于生成 SSID(改成xiaowei)
    config.ssid_prefix = "Xiaowei";
    config.language = Lang::CODE;
    config.show_ota_config = true;
    config.show_sleep_config = true;

    // 设置 DHCP 主机主机名，使路由器显示友好的名称而不是 "espressif"。
    // 使用与配置 AP SSID 相同的方案，即 "<prefix>-<last 2 MAC bytes>"。
    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        char hostname[32];
        snprintf(hostname, sizeof(hostname), "%s-%02X%02X", config.ssid_prefix.c_str(), mac[4], mac[5]);
        config.station_hostname = hostname;
    }
    wifi_manager.Initialize(config);

    // 设置统一事件回调函数，将事件转发到 NetworkEvent 并包含 SSID 数据
    wifi_manager.SetEventCallback([this](WifiEvent event, const std::string& data) {
        // 处理 WiFi 事件
        switch (event) {
            case WifiEvent::Scanning:
                OnNetworkEvent(NetworkEvent::Scanning); // 扫描 WiFi 网络
                break;
            case WifiEvent::Connecting:
                OnNetworkEvent(NetworkEvent::Connecting, data); // 连接 WiFi 网络
                break;
            case WifiEvent::Connected:
                OnNetworkEvent(NetworkEvent::Connected, data); // 连接成功
                break;
            case WifiEvent::Disconnected:
                OnNetworkEvent(NetworkEvent::Disconnected); // 断开连接
                               break;
            case WifiEvent::ConfigModeEnter:
                OnNetworkEvent(NetworkEvent::WifiConfigModeEnter); // 进入配网模式
                break;
            case WifiEvent::ConfigModeExit:
                OnNetworkEvent(NetworkEvent::WifiConfigModeExit); // 退出配网模式
                break;
        }
    });

    // 尝试连接或进入配网模式
    TryWifiConnect();
}

// 尝试连接或进入配网模式
void WifiBoard::TryWifiConnect() {
    auto& ssid_manager = SsidManager::GetInstance();    // 获取 SSID 管理器实例
    bool have_ssid = !ssid_manager.GetSsidList().empty(); // 检查是否有配置的 SSID

    if (have_ssid) {
        // 开始连接尝试，设置超时时间
        ESP_LOGI(TAG, "Starting WiFi connection attempt");
        esp_timer_start_once(connect_timer_, CONNECT_TIMEOUT_SEC * 1000000ULL);
        WifiManager::GetInstance().StartStation();  // 启动 WiFi 站点模式
    } else {
        // 如果没有配置的 SSID，进入配网模式
        vTaskDelay(pdMS_TO_TICKS(1500));
        StartWifiConfigMode();
    }
}

// 处理网络事件
void WifiBoard::OnNetworkEvent(NetworkEvent event, const std::string& data) {
    switch (event) {  
        case NetworkEvent::Connected:   // 连接成功
            // 停止连接超时定时器
            esp_timer_stop(connect_timer_);
            in_config_mode_ = false;
            ESP_LOGI(TAG, "Connected to WiFi: %s", data.c_str());
            break;
        case NetworkEvent::Scanning:    // 扫描 WiFi 网络
            ESP_LOGI(TAG, "WiFi scanning");
            break;
        case NetworkEvent::Connecting:  // 连接 WiFi 网络
            ESP_LOGI(TAG, "WiFi connecting to %s", data.c_str());
            break;
        case NetworkEvent::Disconnected: // 断开连接
            ESP_LOGW(TAG, "WiFi disconnected");
            break;
        case NetworkEvent::WifiConfigModeEnter: // 进入配网模式
            ESP_LOGI(TAG, "WiFi config mode entered");
            in_config_mode_ = true;
            break;
        case NetworkEvent::WifiConfigModeExit: // 退出配网模式
            ESP_LOGI(TAG, "WiFi config mode exited");
            in_config_mode_ = false;
            // 尝试连接或进入配网模式
            TryWifiConnect();
            break;
        default:
            break;
    }

    // 调用外部回调函数，通知网络事件
    if (network_event_callback_) {
        network_event_callback_(event, data);
    }
}

// 设置网络事件回调函数
void WifiBoard::SetNetworkEventCallback(NetworkEventCallback callback) {
    network_event_callback_ = std::move(callback);
}

// 处理连接超时事件
void WifiBoard::OnWifiConnectTimeout(void* arg) {
    auto* board = static_cast<WifiBoard*>(arg);
    ESP_LOGW(TAG, "WiFi connection timeout, entering config mode");

    // 停止当前连接尝试，进入配网模式
    WifiManager::GetInstance().StopStation();
    board->StartWifiConfigMode();
}

// 开始配网模式
void WifiBoard::StartWifiConfigMode() {
    in_config_mode_ = true;
    // 过渡到配网状态
    Application::GetInstance().SetDeviceState(kDeviceStateWifiConfiguring);
#ifdef CONFIG_USE_HOTSPOT_WIFI_PROVISIONING         // 如果启用了热点模式
    auto& wifi_manager = WifiManager::GetInstance();     // 获取 WiFi 管理器实例

    wifi_manager.StartConfigAp();  // 启动热点模式

    // 显示热点 SSID 和浏览器访问 URL 提示
    Application::GetInstance().Schedule([&wifi_manager]() { 
        std::string hint = Lang::Strings::CONNECT_TO_HOTSPOT;   // 连接到热点 SSID 提示
        hint += wifi_manager.GetApSsid();                      // 添加热点 SSID
        hint += Lang::Strings::ACCESS_VIA_BROWSER;              // 添加浏览器访问提示
        hint += wifi_manager.GetApWebUrl();                    // 添加热点访问 URL

        // 显示热点 SSID 和浏览器访问 URL 提示
        Application::GetInstance().Alert(Lang::Strings::WIFI_CONFIG_MODE, hint.c_str(), "gear", Lang::Sounds::OGG_WIFICONFIG);
    });
#endif
}

// 进入配网模式
void WifiBoard::EnterWifiConfigMode() {
    ESP_LOGI(TAG, "EnterWifiConfigMode called");
    // 显示配网模式提示
    GetDisplay()->ShowNotification(Lang::Strings::ENTERING_WIFI_CONFIG_MODE);

    auto& app = Application::GetInstance(); // 获取应用实例
    auto state = app.GetDeviceState();   // 获取当前设备状态

    // 如果当前设备状态是说话、监听或空闲，重置协议资源
    if (state == kDeviceStateSpeaking || state == kDeviceStateListening || state == kDeviceStateIdle) {
        // 重置协议资源
        Application::GetInstance().ResetProtocol();

        // 创建一个任务，等待 1 秒后进入配网模式
        xTaskCreate([](void* arg) {
            auto* board = static_cast<WifiBoard*>(arg);

            // 等待 1 秒，确保协议资源重置完成
            vTaskDelay(pdMS_TO_TICKS(1000));

            // 停止当前连接尝试
            esp_timer_stop(board->connect_timer_);
            WifiManager::GetInstance().StopStation();

            // 进入配网模式
            board->StartWifiConfigMode();

            vTaskDelete(NULL);
        }, "wifi_cfg_delay", 4096, this, 2, NULL);
        return;
    }

    // 如果当前设备状态不是启动状态，直接进入配网模式
    if (state != kDeviceStateStarting) {
        ESP_LOGE(TAG, "EnterWifiConfigMode called but device state is not starting or speaking, device state: %d", state);
        return;
    }

    // 停止当前连接尝试
    esp_timer_stop(connect_timer_);
    WifiManager::GetInstance().StopStation();

    // 进入配网模式
    StartWifiConfigMode();
}

// 是否在配网模式
bool WifiBoard::IsInWifiConfigMode() const {
    return WifiManager::GetInstance().IsConfigMode();
}

// 获取网络接口
NetworkInterface* WifiBoard::GetNetwork() {
    static EspNetwork network;
    return &network;
}

// 获取网络状态图标
const char* WifiBoard::GetNetworkStateIcon() {
    auto& wifi = WifiManager::GetInstance();

    if (wifi.IsConfigMode()) {
        return MATERIAL_SYMBOLS_WIFI;   // 配网模式图标
    }
    if (!wifi.IsConnected()) {
        return MATERIAL_SYMBOLS_WIFI_OFF;   // 未连接图标
    }

    int rssi = wifi.GetRssi();
    if (rssi >= -65) {
        return MATERIAL_SYMBOLS_WIFI;   // 连接图标
    } else if (rssi >= -75) {
        return MATERIAL_SYMBOLS_WIFI_2_BAR;   // 连接图标
    }
    return MATERIAL_SYMBOLS_WIFI_1_BAR;   // 连接图标
}

// 获取板子 JSON 字符串
std::string WifiBoard::GetBoardJson() {
    auto& wifi = WifiManager::GetInstance();
    std::string json = R"({"type":")" + std::string(BOARD_TYPE) + R"(",)";
    json += R"("name":")" + std::string(BOARD_NAME) + R"(",)";
    json += R"("manufacturer":")" + std::string(BOARD_MANUFACTURER) + R"(",)";

    if (!wifi.IsConfigMode()) {
        json += R"("ssid":")" + wifi.GetSsid() + R"(",)";
        json += R"("rssi":)" + std::to_string(wifi.GetRssi()) + R"(,)";
        json += R"("channel":)" + std::to_string(wifi.GetChannel()) + R"(,)";
        json += R"("ip":")" + wifi.GetIpAddress() + R"(",)";
    }

    json += R"("mac":")" + SystemInfo::GetMacAddress() + R"("})";
    return json;
}

// 设置电源保存级别
void WifiBoard::SetPowerSaveLevel(PowerSaveLevel level) {
    WifiPowerSaveLevel wifi_level;
    switch (level) {
        case PowerSaveLevel::LOW_POWER:
            wifi_level = WifiPowerSaveLevel::LOW_POWER;
            break;
        case PowerSaveLevel::BALANCED:
            wifi_level = WifiPowerSaveLevel::BALANCED;
            break;
        case PowerSaveLevel::PERFORMANCE:
        default:
            wifi_level = WifiPowerSaveLevel::PERFORMANCE;
            break;
    }
    WifiManager::GetInstance().SetPowerSaveLevel(wifi_level);
}

// 获取设备状态 JSON 字符串
std::string WifiBoard::GetDeviceStatusJson() {
    auto& board = Board::GetInstance();
    auto root = cJSON_CreateObject();

    // Audio speaker
    auto audio_speaker = cJSON_CreateObject();
    if (auto codec = board.GetAudioCodec()) {
        cJSON_AddNumberToObject(audio_speaker, "volume", codec->output_volume());
    }
    cJSON_AddItemToObject(root, "audio_speaker", audio_speaker);

    // Screen
    auto screen = cJSON_CreateObject();
    if (auto backlight = board.GetBacklight()) {
        cJSON_AddNumberToObject(screen, "brightness", backlight->brightness());
    }
    if (auto display = board.GetDisplay(); display && display->height() > 64) {
        if (auto theme = display->GetTheme()) {
            cJSON_AddStringToObject(screen, "theme", theme->name().c_str());
        }
    }
    cJSON_AddItemToObject(root, "screen", screen);

    // Battery
    int level = 0;
    bool charging = false, discharging = false;
    if (board.GetBatteryLevel(level, charging, discharging)) {
        auto battery = cJSON_CreateObject();
        cJSON_AddNumberToObject(battery, "level", level);
        cJSON_AddBoolToObject(battery, "charging", charging);
        // 显式状态字段: 充电中/放电中/已充满, 供模型直接引用,
        // 避免模型看到未充电就自行编造"已经充满"
        cJSON_AddStringToObject(battery, "status",
                                charging ? "charging" : (level >= 100 ? "full" : "discharging"));
        cJSON_AddItemToObject(root, "battery", battery);
    }

    // Network
    auto& wifi = WifiManager::GetInstance();
    auto network = cJSON_CreateObject();
    cJSON_AddStringToObject(network, "type", "wifi");
    cJSON_AddStringToObject(network, "ssid", wifi.GetSsid().c_str());
    int rssi = wifi.GetRssi();
    const char* signal = rssi >= -60 ? "strong" : (rssi >= -70 ? "medium" : "weak");
    cJSON_AddStringToObject(network, "signal", signal);
    cJSON_AddItemToObject(root, "network", network);

    auto str = cJSON_PrintUnformatted(root);
    std::string result(str);
    cJSON_free(str);
    cJSON_Delete(root);
    return result;
}
