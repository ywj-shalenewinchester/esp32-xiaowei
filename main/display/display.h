#ifndef DISPLAY_H
#define DISPLAY_H

#include "emoji_collection.h"
#include "text_glyph.h"

// 如果没有定义使用特定的表情消息样式，则默认启用 LVGL 图形库
#ifndef CONFIG_USE_EMOTE_MESSAGE_STYLE
#define HAVE_LVGL 1
#include <lvgl.h>
#endif

#include <esp_log.h>
#include <esp_pm.h>
#include <esp_timer.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

// ================= Theme (主题) 类 =================
class Theme {
public:
    // 构造函数，传入主题名称
    Theme(const std::string& name) : name_(name) {}
    virtual ~Theme() = default;

    // 获取主题名称
    inline std::string name() const { return name_; }

private:
    std::string name_;
};

// ================= Display (显示器) 基类 =================
class Display {
public:
    Display();
    virtual ~Display();

    // --------- 虚函数接口：供具体的屏幕子类（LCD）重写 ---------

    // 设置设备状态（如：WiFi连接中、录音中等）
    virtual void SetStatus(const char* status);

    // 显示通知消息（支持 C 字符串和 C++ std::string），默认显示 3000 毫秒
    virtual void ShowNotification(const char* notification, int duration_ms = 3000);
    virtual void ShowNotification(const std::string& notification, int duration_ms = 3000);
    
    // 设置设备的情感状态（用于屏幕显示不同的表情，如开心、发呆）
    virtual void SetEmotion(const char* emotion);

    // 设置聊天消息（用于显示用户或助手的对话）
    virtual void SetChatMessage(const char* role, const char* content);
    
    // 清除聊天消息（用于显示新的对话）
    virtual void ClearChatMessages();

    // 设置与获取 UI 主题
    virtual void SetTheme(Theme* theme);
    virtual Theme* GetTheme() { return current_theme_; }

    // 刷新状态栏（update_all 为 true 时全局刷新）
    virtual void UpdateStatusBar(bool update_all = false);

    // 设置屏幕的省电模式（如息屏、降低刷新率）
    virtual void SetPowerSaveMode(bool on);

    // 动态添加和清除字形（用于动态加载字体、节省内存）
    virtual bool AddTextGlyphs(const std::vector<TextGlyph>& glyphs, uint8_t bpp) { return false; }
    virtual void ClearTextGlyphs() {}

    // 设置表情包图片集合
    virtual void SetEmojiCollection(std::shared_ptr<EmojiCollection>) {}

    // 专门为电子墨水屏 (E-paper) 预留的刷新位图接口
    virtual void DisplayEpaperBitmap(const uint8_t* bitmap, int width, int height) {}
    
    // 初始化 UI 界面
    virtual void SetupUI() { setup_ui_called_ = true; }

    // --------- 内联属性获取 ---------
    inline int width() const { return width_; }         // 获取屏幕宽度
    inline int height() const { return height_; }       // 获取屏幕高度
    inline bool IsSetupUICalled() const { return setup_ui_called_; }    // UI 是否已初始化

protected:
    int width_ = 0;         // 屏幕宽度
    int height_ = 0;        // 屏幕高度
    bool setup_ui_called_ = false;  // 标记位：记录 SetupUI() 是否被调用过

    Theme* current_theme_ = nullptr;    // 当前使用的主题指针

    friend class DisplayLockGuard;
    virtual bool Lock(int timeout_ms = 0) = 0;
    virtual void Unlock() = 0;
};

// ================= DisplayLockGuard (显示锁守卫) =================
// 采用 RAII 机制管理屏幕的线程锁
class DisplayLockGuard {
public:
    DisplayLockGuard(Display* display) : display_(display) {
        // 构造时自动尝试加锁，超时时间设置为 30000 毫秒（30秒）
        if (!display_->Lock(30000)) {
            ESP_LOGE("Display", "Failed to lock display"); // 如果加锁失败，打印错误日志
        }
    }
    // 析构时自动解锁
    ~DisplayLockGuard() { display_->Unlock(); }

private:
    Display* display_; // 关联的 Display 对象
};

#endif
