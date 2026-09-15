#pragma once

#include "display.h"
#include <memory>
#include <string>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include "expression_emote.h"   // 底层的动效渲染引擎库

namespace emote {

// EmoteDisplay 继承自基础的 Display 类，专门用于接管并全屏显示动态表情
class EmoteDisplay : public Display {
public:
    // 构造函数：需要传入 ESP32 底层的 panel 句柄和 io 句柄，以及你圆屏的分辨率 (350, 350)
    EmoteDisplay(esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t panel_io, int width, int height);
    virtual ~EmoteDisplay();

    // ================= 核心行为重载 =================
    // 直接设置具体的情绪（比如 "happy", "sad", "angry"），由 AI 主动下发
    virtual void SetEmotion(const char* emotion) override;

    // 设置设备当前状态，情绪系统会据此切换待机/聆听等基础动画
    virtual void SetStatus(const char* status) override;

    // 劫持了“聊天消息”接口。这里不再显示文字，而是让表情进入“说话(SPEAK)”状态
    virtual void SetChatMessage(const char* role, const char* content) override;
    
    // 情绪引擎自己控制画面，不需要 LVGL 的主题，这里为空实现
    virtual void SetTheme(Theme* theme) override;
    
    // 劫持了通知接口，让表情系统根据通知内容做出反应（比如显示一个系统级的提示图标）
    virtual void ShowNotification(const char* notification, int duration_ms = 3000) override;
    
    virtual void UpdateStatusBar(bool update_all = false) override; // 更新状态栏
    virtual void SetPowerSaveMode(bool on) override;    // 设置省电模式
    virtual void SetPreviewImage(const void* image);    // 设置预览图片

    // ================= 强插画（主动弹窗）接口 =================
    // 停止当前的强制动画，恢复到默认的待机/对话情绪
    bool StopAnimDialog();
    // 强制插入一个动画（例如：眨眼、冒爱心），duration_ms 为持续时间，时间到了自动恢复
    bool InsertAnimDialog(const char* emoji_name, uint32_t duration_ms);

    // 刷新整个表情系统
    void RefreshAll();

    // 获取底层的 emote 句柄，供内部高级功能直接调用底层 API
    emote_handle_t GetEmoteHandle() const { return emote_handle_; }

private:
    // 情绪系统内部有自己的线程安全队列，所以这里的锁机制直接架空（返回 true）
    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;

    // 指向底层表情引擎实例的指针（句柄）
    emote_handle_t emote_handle_ = nullptr;

};

} // namespace emote
