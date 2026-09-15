#include "backlight.h"
#include "settings.h"

#include <driver/ledc.h>
#include <esp_log.h>

#define TAG "Backlight"

Backlight::Backlight() {
    // 创建背光渐变定时器
    const esp_timer_create_args_t timer_args = {
        .callback =
            [](void* arg) {
                auto self = static_cast<Backlight*>(arg);
                self->OnTransitionTimer();  // 执行渐变逻辑
            },
        .arg = this,  // 把当前的自己 (this) 传给上面的 callback
        .dispatch_method = ESP_TIMER_TASK,
        .name = "backlight_timer",
        .skip_unhandled_events = true,  // 跳过未处理的事件
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &transition_timer_));  // 正式创建定时器
}

Backlight::~Backlight() {
    // 关闭定时器资源
    if (transition_timer_ != nullptr) {
        esp_timer_stop(transition_timer_);
        esp_timer_delete(transition_timer_);
    }
}

void Backlight::RestoreBrightness() {
    // 从系统设置("display"分区)中读取上次保存的亮度，如果找不到，默认给 75
    Settings settings("display");
    int saved_brightness = settings.GetInt("brightness", 75);

    // 保护机制：如果读到的亮度是 0 或者负数（可能是数据坏了），强行设为 10
    // 否则屏幕全黑，用户可能会以为设备坏了
    if (saved_brightness <= 0) {
        ESP_LOGW(TAG, "Brightness value (%d) is too small, setting to default (10)",
                 saved_brightness);
        saved_brightness = 10;  // 设置一个较低的默认值
    }

    SetBrightness(saved_brightness);  // 开始调整亮度
}

// 设置亮度，并可选择是否永久保存到系统设置中
void Backlight::SetBrightness(uint8_t brightness, bool permanent) {
    // 限制最大值为 100，防止数值溢出
    if (brightness > 100) {
        brightness = 100;
    }

    // 如果亮度已经是最新的，就不需要更新了
    if (brightness_ == brightness) {
        return;
    }

    // 如果要求永久保存，就把它写进 Flash 芯片里
    if (permanent) {
        Settings settings("display", true);
        settings.SetInt("brightness", brightness);
    }

    // 如果目标大于当前，步长为正(+1)；否则步长为负(-1)
    target_brightness_ = brightness;
    step_ = (target_brightness_ > brightness_) ? 1 : -1;

    if (transition_timer_ != nullptr) {
        // 启动定时器，每 5ms 更新一次
        esp_timer_start_periodic(transition_timer_, 5 * 1000);
    }
    ESP_LOGI(TAG, "Set brightness to %d", brightness);
}

void Backlight::OnTransitionTimer() {
    // 【定时器核心逻辑：每 5ms 运行一次】，根据当前亮度与目标亮度比较，调整亮度

    // 如果已经到达目标亮度，马上停表退出
    if (brightness_ == target_brightness_) {
        esp_timer_stop(transition_timer_);
        return;
    }

    // 朝着目标走一步（+1 或 -1）
    brightness_ += step_;
    SetBrightnessImpl(brightness_);

    // 如果已经到达目标亮度，马上停表退出
    if (brightness_ == target_brightness_) {
        esp_timer_stop(transition_timer_);
    }
}

PwmBacklight::PwmBacklight(gpio_num_t pin, bool output_invert, uint32_t freq_hz) : Backlight() {
    // 1. 配置 ESP32 的 LEDC 定时器 (用来产生 PWM 信号)
    const ledc_timer_config_t backlight_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution =
            LEDC_TIMER_10_BIT,  // 分辨率为 10 位 (意味着亮度级别有 2^10 = 1024 个等级，0到1023)
        .timer_num = LEDC_TIMER_0,
        .freq_hz = freq_hz,  // 背光pwm频率需要高一点，防止电感啸叫
        .clk_cfg = LEDC_AUTO_CLK,
        .deconfigure = false};
    ESP_ERROR_CHECK(ledc_timer_config(&backlight_timer));

    // 2. 配置 LEDC 的输出通道（把刚刚配置的信号连到具体的物理引脚上）
    const ledc_channel_config_t backlight_channel = {
        .gpio_num = pin,
        .speed_mode = LEDC_LOW_SPEED_MODE,  // 传入的背光控制引脚
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,  // 不需要中断
        .timer_sel = LEDC_TIMER_0,       // 绑定上面的定时器0
        .duty = 0,                       // 初始占空比(亮度)为 0
        .hpoint = 0,
        .flags = {
            .output_invert = output_invert,  // 如果引脚是低电平点亮，这里设为 true 可以反转逻辑
        }};
    ESP_ERROR_CHECK(ledc_channel_config(&backlight_channel));
}

PwmBacklight::~PwmBacklight() {
    //  关闭该通道的 PWM 输出
    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
}

void PwmBacklight::SetBrightnessImpl(uint8_t brightness) {
    // 将 0-100 映射到 0-1023 之间。公式： (1023 * 当前亮度) / 100
    uint32_t duty_cycle = (1023 * brightness) / 100;
    // 更新占空比
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty_cycle);
    // 更新输出
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}
