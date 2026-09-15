#ifndef BUTTON_H_
#define BUTTON_H_

#include <driver/gpio.h>
#include <iot_button.h>
#include <button_types.h>
#include <button_adc.h>
#include <button_gpio.h>
#include <functional>

// =================================================================
// 核心基类：Button
// 作用：对底层 iot_button 组件进行 C++ 封装，管理按键的生命周期，
//       并将底层的 C 语言回调完美转换为 C++ 的 std::function。
// =================================================================
class Button {
public:
    Button(button_handle_t button_handle);  // 构造函数 1：直接传入已经创建好的底层句柄
    
    // 构造函数 2：最常用的构造方式，用于创建一个普通的 GPIO 按键
    // active_high: 默认 false 表示低电平有效（按下导通到 GND）；true 表示高电平有效
    // long/short_press_time: 自定义长按和短按的判定时间（毫秒）
    // enable_power_save: 是否允许在按键空闲时让 ESP32 进入深度睡眠
    Button(gpio_num_t gpio_num, bool active_high = false, uint16_t long_press_time = 0,
           uint16_t short_press_time = 0,
           bool enable_power_save =
               false);  
    ~Button();

    // =============================================================
    // 事件注册接口：接收 std::function，这意味着可以直接传入 Lambda 表达式！
    // =============================================================
    void OnPressDown(std::function<void()> callback);  // 按下瞬间触发
    void OnPressUp(std::function<void()> callback);    // 松开瞬间触发
    void OnLongPress(std::function<void()> callback);   // 长按触发
    void OnClick(std::function<void()> callback);       // 单击触发
    void OnDoubleClick(std::function<void()> callback);  // 双击触发    
    void OnMultipleClick(std::function<void()> callback, uint8_t click_count = 3);  // 多次点击触发

protected:
    gpio_num_t gpio_num_;  // 绑定的 GPIO 引脚
    button_handle_t button_handle_ = nullptr;  // 底层 iot_button 的灵魂句柄

    // 这里保存了上层传入的 C++ 函数对象 (比如 Lambda 表达式)
    std::function<void()> on_press_down_;
    std::function<void()> on_press_up_;
    std::function<void()> on_long_press_;
    std::function<void()> on_click_;
    std::function<void()> on_double_click_;
    std::function<void()> on_multiple_click_;
};

// =================================================================
// 派生类：AdcButton (ADC 按键)
// 作用：如果芯片支持 ADC，可以用这个类。ADC 按键允许在一个 GPIO 引脚上
//       通过串联不同阻值的电阻，连接多个按键，节省 GPIO 资源。
// =================================================================
#if CONFIG_SOC_ADC_SUPPORTED
class AdcButton : public Button {
public:
    AdcButton(const button_adc_config_t& adc_config);
};
#endif

// =================================================================
// 派生类：PowerSaveButton (低功耗按键)
// 作用：一个快捷类，继承自 Button。内部直接写死 enable_power_save = true，
//       适合用作唤醒设备的电源键。
// =================================================================
class PowerSaveButton : public Button {
public:
    PowerSaveButton(gpio_num_t gpio_num) : Button(gpio_num, false, 0, 0, true) {
    }
};

#endif // BUTTON_H_
