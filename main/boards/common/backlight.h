#pragma once

#include <cstdint>
#include <functional>

#include <driver/gpio.h>
#include <esp_timer.h>

// =================================================================
// 核心基类：Backlight (背光逻辑控制类)
// 作用：负责处理所有跟硬件无关的逻辑，比如记忆亮度、计算渐变过程、定时器管理。
// =================================================================
class Backlight {
public:
    Backlight();   // 构造函数：初始化定时器
    ~Backlight();  // 析构函数：清理资源

    void RestoreBrightness();
    void SetBrightness(uint8_t brightness,
                       bool permanent = false);  // 设置目标亮度 (brightness: 0-100的亮度值,
                                                 // permanent: 是否永久保存到设置中)
    inline uint8_t brightness() const {
        return brightness_;
    }  // 获取当前真实的亮度值 (inline 表示内联函数，运行效率高)

protected:
    void OnTransitionTimer();  // 定时器每次“滴答”作响时，就会执行这个函数，用来一步步改变亮度
    virtual void SetBrightnessImpl(uint8_t brightness) = 0;  // 由派生类重写

    // 内部状态变量
    esp_timer_handle_t transition_timer_ = nullptr;
    uint8_t brightness_ = 0;
    uint8_t target_brightness_ = 0;
    uint8_t step_ = 1;
};

// =================================================================
// 派生类：PwmBacklight (PWM 硬件背光控制类)
// 作用：继承自 Backlight。
// =================================================================
class PwmBacklight : public Backlight {
public:
    // 初始化引脚、是否反转输出、PWM频率(默认25000Hz)
    PwmBacklight(gpio_num_t pin, bool output_invert = false, uint32_t freq_hz = 25000);
    ~PwmBacklight();

    void SetBrightnessImpl(uint8_t brightness) override;  // 实现具体怎么控制硬件引脚改变亮度
};
