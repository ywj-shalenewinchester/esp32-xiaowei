#ifndef WIFI_BOARD_H
#define WIFI_BOARD_H

#include "board.h"
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <esp_timer.h>

/**
 * @brief Wi-Fi 开发板基类 (继承自 Board)
 * 
 * 专为基于 Wi-Fi 通信的设备设计，封装了 Wi-Fi 的 Station(连路由) 和 AP(开热点配网) 模式。
 * 内部集成了连接超时管理、网络状态回调转发，以及配网 UI/音频联动逻辑。
 */
class WifiBoard : public Board {
protected:
    esp_timer_handle_t connect_timer_ = nullptr;        // 用于检测 Wi-Fi 连接超时的定时器句柄
    bool in_config_mode_ = false;                      // 标记当前是否处于配网模式 (AP模式)
    NetworkEventCallback network_event_callback_ = nullptr;// 向应用层上报网络状态的回调函数

    virtual std::string GetBoardJson() override;  // 获取板子 JSON 字符串，包含所有状态信息

    /**
     * @brief 处理底层 Wi-Fi 管理器上报的网络事件
     * @param event 网络事件类型 (如连接中、已连接、断开等)
     * @param data 附加数据 (例如连接成功时携带 SSID 字符串)
     */
    void OnNetworkEvent(NetworkEvent event, const std::string& data = "");

    /**
     * @brief 尝试发起 Wi-Fi 连接
     * 如果本地存有 SSID，则启动定时器并尝试连接；如果没有，直接进入配网模式。
     */
    void TryWifiConnect();

    /**
     * @brief 启动 Wi-Fi 配网模式 (底层实现)
     * 开启 SoftAP，并触发语音播报和屏幕 UI 提示。
     */
    void StartWifiConfigMode();

    /**
     * @brief Wi-Fi 连接超时回调函数 (静态函数，供 esp_timer 调用)
     */
    static void OnWifiConnectTimeout(void* arg);

public:
    WifiBoard();
    virtual ~WifiBoard();
    
    virtual std::string GetBoardType() override;    // 获取板子类型，返回 "wifi"
    
    /**
     * @brief 异步启动网络
     * 该函数会立即返回。网络状态会通过 SetNetworkEventCallback 注册的回调函数异步通知。
     */
    virtual void StartNetwork() override;
    
    virtual NetworkInterface* GetNetwork() override;    // 获取网络接口实例，返回 nullptr
    virtual void SetNetworkEventCallback(NetworkEventCallback callback) override;    // 设置网络状态回调函数
    virtual const char* GetNetworkStateIcon() override;    // 获取当前网络状态图标，如 "wifi"、"wifi_connected" 等
    virtual void SetPowerSaveLevel(PowerSaveLevel level) override;    // 设置电源保存等级
    virtual std::string GetDeviceStatusJson() override;    // 获取设备状态 JSON 字符串，包含所有状态信息
    
    /**
     * @brief 强制进入 Wi-Fi 配网模式 (线程安全，可在任何 FreeRTOS 任务中调用)
     * 绑定到设备的物理按键事件上，会安全地打断当前音频流。
     */
    void EnterWifiConfigMode();
    
    /**
     * @brief 检查当前是否处于配网模式
     */
    bool IsInWifiConfigMode() const;
};

#endif // WIFI_BOARD_H
