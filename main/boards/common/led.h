#pragma once

#include <cstdint>
#include <mutex>

#include <driver/gpio.h>
#include <esp_timer.h>

// =================================================================
// 抽象基类：Led
// =================================================================
class Led {
public:
    virtual ~Led() = default;
    // 纯虚函数：当设备状态改变时，主程序会调用这个函数。
    virtual void OnStateChanged() = 0;
};

// =================================================================
// 派生类：GpioLed (单色普通 LED 控制类)
// 作用：通过控制普通 GPIO 引脚的高低电平来控制灯的亮灭。
// 业务逻辑：当设备处于录音状态（kDeviceStateListening）时闪烁，其他时候熄灭。
// =================================================================
class GpioLed : public Led {
public:
    // 构造函数参数解释：
    // - gpio: 绑定的 ESP32 引脚号
    // - active_level: 点亮时的电平（比如填0表示低电平灯亮，填1表示高电平灯亮）
    // - blink_interval_ms: 闪烁间隔，默认500ms（即 500ms亮，500ms灭）
    GpioLed(gpio_num_t gpio, uint8_t active_level, uint32_t blink_interval_ms = 500);
    ~GpioLed() override;

    void OnStateChanged() override; // 当设备状态改变时，主程序会调用这个函数。

private:
    void SetLit(bool lit);  // 点亮或熄灭 LED
    void StartBlink();      // 开始闪烁 LED
    void StopBlink();       // 停止闪烁 LED
    static void BlinkTimerCallback(void* arg);  // 定时器回调函数

    gpio_num_t gpio_;            // 绑定的 GPIO
    uint8_t active_level_;       // 点亮时的电平
    uint32_t blink_interval_ms_;     // 闪烁间隔

    esp_timer_handle_t timer_ = nullptr;    // 定时器句柄
    volatile bool blinking_ = false;  // 是否处于闪烁态
    bool lit_ = false;                // 当前是否点亮
    std::mutex mutex_;                // 保护 lit_ 与 GPIO 写入
};
