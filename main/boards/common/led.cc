#include "led.h"

#include "application.h"
#include "device_state.h"


GpioLed::GpioLed(gpio_num_t gpio, uint8_t active_level, uint32_t blink_interval_ms)
        : gpio_(gpio), active_level_(active_level), blink_interval_ms_(blink_interval_ms) {
    // 1. 配置 ESP32 的 GPIO 引脚为输出模式
    gpio_config_t config = {};
    config.pin_bit_mask = 1ULL << gpio_;
    config.mode = GPIO_MODE_OUTPUT;
    config.pull_up_en = GPIO_PULLUP_DISABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&config));

    // 2. 创建一个定时器，专门用来控制闪烁
    esp_timer_create_args_t timer_args = {
        .callback = BlinkTimerCallback,  // 时间到了就叫醒这个函数
        .arg = this,                     // 把当前的 GpioLed 对象传进去
        .dispatch_method = ESP_TIMER_TASK,
        .name = "led_blink",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timer_));

    // 3. 初始化时，默认把灯熄灭
    SetLit(false);
}

GpioLed::~GpioLed() {
    // 停止定时器，并把灯熄灭
    if (timer_ != nullptr) {
        blinking_ = false;
        esp_timer_stop(timer_);
        esp_timer_delete(timer_);
        timer_ = nullptr;
    }
    SetLit(false);
}

void GpioLed::SetLit(bool lit) {
    // active_level_ 是"点亮"对应的电平，熄灭就是它的反
    gpio_set_level(gpio_, lit ? active_level_ : !active_level_);
}

void GpioLed::OnStateChanged() {
    // 获取全局 Application 中的设备当前状态
    // 如果当前设备正在“监听/录音” (kDeviceStateListening)
    if (Application::GetInstance().GetDeviceState() == kDeviceStateListening) {
        StartBlink();   // 开始闪烁
    } else {
        StopBlink();    // 停止闪烁
    }
}

void GpioLed::StartBlink() {
    if (blinking_) {
        return;  // 已经在闪了，不要重置相位
    }
    blinking_ = true;

    // 定时器开始闪烁，第一次回调会立刻执行
    std::lock_guard<std::mutex> lock(mutex_);
    lit_ = true;
    SetLit(true);
    esp_timer_start_periodic(timer_, blink_interval_ms_ * 1000ULL); // 500ms闪烁一次
}

void GpioLed::StopBlink() {
    // 先落标志再停定时器：即使回调正在执行，它也会看到 blinking_ 为 false 而直接返回，
    // 这里随后的 SetLit(false) 一定是最后一次写 GPIO，灯不会卡在亮。
    blinking_ = false;
    esp_timer_stop(timer_);

    std::lock_guard<std::mutex> lock(mutex_);
    lit_ = false;
    SetLit(false);
}

void GpioLed::BlinkTimerCallback(void* arg) {
    // 把 void* 指针还原回我们自己的类对象
    auto* self = static_cast<GpioLed*>(arg);

    // 加锁，防止正在闪烁时主程序突然调用 StopBlink()
    std::lock_guard<std::mutex> lock(self->mutex_);
    // 再次确认：主程序是不是已经让我停了
    if (!self->blinking_) {
        return;
    }
    // 状态翻转：亮变灭，灭变亮
    self->lit_ = !self->lit_;
    // 更新 GPIO
    self->SetLit(self->lit_);
}
