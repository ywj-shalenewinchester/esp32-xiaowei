#ifndef BOARD_H
#define BOARD_H

#include <http.h>
#include <web_socket.h>
#include <mqtt.h>
#include <udp.h>
#include <string>
#include <functional>
#include <network_interface.h>

#include "led.h"
#include "backlight.h"
#include "assets.h"

/**
 * 网络事件
 */
enum class NetworkEvent {
    Scanning,              // 网络扫描扫描中
    Connecting,            // 连接中
    Connected,             // 已连接
    Disconnected,          // 断开连接
    WifiConfigModeEnter,   // 进入 WiFi 配置模式
    WifiConfigModeExit,    // 退出 WiFi 配置模式
    // 模块特定事件
    ModemDetecting,        // 检测模块
    ModemErrorNoSim,       // 未检测到 SIM 卡错误
    ModemErrorRegDenied,   // 网络注册被拒绝错误
    ModemErrorInitFailed,  // Modem 初始化失败错误
    ModemErrorTimeout      // 操作超时错误
};

// 电源节能等级
enum class PowerSaveLevel {
    LOW_POWER,    // 最低功耗
    BALANCED,     // 中功耗
    PERFORMANCE,  // 最大功耗
};

// 网络事件回调函数类型
using NetworkEventCallback = std::function<void(NetworkEvent event, const std::string& data)>;

// 板子类基类
void* create_board();
// 音频编码器
class AudioCodec;
// 显示屏
class Display;

/**
 * @brief 硬件板级抽象基类 (Hardware Board Abstraction Base Class)
 * 
 * 该类充当整个系统的硬件抽象层 (HAL) 和全局硬件管理器，采用单例模式设计。
 * 主要功能：
 * 1. 屏蔽硬件差异：为应用层提供统一的 API，解耦底层具体开发板的物理实现。
 * 2. 外设接口中心：提供获取屏幕、LED、背光、音频编码器、电池、网络等外设实例的标准入口。
 * 3. 规范子类实现：外设与网络接口全部声明为纯虚函数，强制具体开发板子类提供实现，
 *    基类不提供任何默认实现，避免"上游返回空对象、下游凭空判断"的隐式约定。
 * 4. 内置通用系统行为：自带 UUID 自动生成、全局唯一标识符维护，以及系统软硬件信息的 JSON 序列化功能。
 */
class Board {
private:
    Board(const Board&) = delete; // 禁用拷贝构造函数
    Board& operator=(const Board&) = delete; // 禁用赋值操作

protected:
    Board();
    std::string GenerateUuid();// 生成设备唯一标识

    // 软件生成的设备唯一标识
    std::string uuid_;

public:
    static Board& GetInstance() {       // 获取单例实例
        static Board* instance = static_cast<Board*>(create_board());
        return *instance;
    }

    virtual ~Board() = default;                         // 析构函数
    virtual std::string GetBoardType() = 0;             // 获取板子类型
    virtual std::string GetUuid() { return uuid_; }      // 获取设备唯一标识

    // ===== 外设接口（全部纯虚，板级子类必须实现）=====
    // 本工程只服务 wzkj/esp32-s3-ai-xiaowei_low 一块板子，
    // 板子有的外设就在这里声明，板子没有的外设（相机、温度）不声明。
    virtual Led* GetLed() = 0;                          // 获取 LED
    virtual Backlight* GetBacklight() = 0;              // 获取背光
    virtual Display* GetDisplay() = 0;                  // 获取显示屏
    virtual AudioCodec* GetAudioCodec() = 0;            // 获取音频编码器
    virtual bool GetBatteryLevel(int &level, bool& charging, bool& discharging) = 0;  // 获取电池电量

    // ===== 网络接口 =====
    virtual NetworkInterface* GetNetwork() = 0;             // 获取网络接口
    virtual void StartNetwork() = 0;                      // 启动网络
    virtual void SetNetworkEventCallback(NetworkEventCallback callback) { (void)callback; } // 设置网络事件回调函数
    virtual const char* GetNetworkStateIcon() = 0;          // 获取网络状态图标
    virtual std::string GetSystemInfoJson();                // 获取系统信息 JSON 字符串
    virtual void SetPowerSaveLevel(PowerSaveLevel level) = 0; // 设置电源节能等级
    virtual std::string GetBoardJson() = 0;                 // 获取板子 JSON 字符串
    virtual std::string GetDeviceStatusJson() = 0;          // 获取设备状态 JSON 字符串
};

// 声明板子类：在板级 .cc 文件末尾写 DECLARE_BOARD(WzkjBoard); 即可把
// create_board() 绑定到具体板类，供 Board::GetInstance() 调用。
// 注意：续行符 \ 必须紧贴行尾，后面不能跟空格或注释，否则续行不成立。
#define DECLARE_BOARD(BOARD_CLASS_NAME) \
void* create_board() { \
    return new BOARD_CLASS_NAME(); \
}

#endif // BOARD_H
