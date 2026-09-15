#ifndef LVGL_DISPLAY_H
#define LVGL_DISPLAY_H

#include "display.h"
#include "lvgl_image.h"

#include <esp_log.h>
#include <esp_pm.h>
#include <esp_timer.h>
#include <lvgl.h>

#include <chrono>
#include <memory>
#include <string>

class DynamicGlyphCache;    // 动态字形缓存
class LvglFont;             // LVGL 字体封装

// LvglDisplay 继承自 Display 基类，专门实现基于 LVGL 的通用显示逻辑
class LvglDisplay : public Display {
public:
    LvglDisplay();
    virtual ~LvglDisplay();

    // 重写基类的虚函数
    virtual void SetStatus(const char* status);
    virtual void ShowNotification(const char* notification, int duration_ms = 3000);
    virtual void ShowNotification(const std::string& notification, int duration_ms = 3000);
    virtual void SetPreviewImage(std::unique_ptr<LvglImage> image);
    virtual void UpdateStatusBar(bool update_all = false);
    virtual void SetPowerSaveMode(bool on);     // 息屏/省电模式

    // LVGL 特有功能：将当前屏幕截屏并编码为 JPEG 格式
    virtual bool SnapshotToJpeg(std::string& jpeg_data, int quality = 80);

    // 动态字体渲染接口
    virtual bool AddTextGlyphs(const std::vector<TextGlyph>& glyphs, uint8_t bpp) override;
    virtual void ClearTextGlyphs() override;
    bool SetTextFont(std::shared_ptr<LvglFont> text_font);

protected:
    esp_pm_lock_handle_t pm_lock_ = nullptr;        // 电源管理锁（防止刷屏时 CPU 降频导致花屏）
    lv_display_t* display_ = nullptr;               // LVGL 的显示器句柄

    // 以下全是 LVGL 的 UI 控件指针 (Label 标签)
    lv_obj_t* network_label_ = nullptr;         // 网络状态图标
    lv_obj_t* status_label_ = nullptr;          // 状态标签
    lv_obj_t* notification_label_ = nullptr;     // 通知标签
    lv_obj_t* mute_label_ = nullptr;             // 静音标签    
    lv_obj_t* battery_label_ = nullptr;          // 电池状态标签
    lv_obj_t* low_battery_popup_ = nullptr;      // 低电池弹窗
    lv_obj_t* low_battery_label_ = nullptr;      // 低电池标签

    const char* battery_icon_ = nullptr;        // 当前显示的电池图标
    const char* network_icon_ = nullptr;        // 当前显示的网络图标
    bool muted_ = false;                        // 当前是否静音

    std::chrono::system_clock::time_point last_status_update_time_; // 记录上次状态更新的时间
    esp_timer_handle_t notification_timer_ = nullptr;               // 用于自动隐藏通知的定时器
    std::unique_ptr<DynamicGlyphCache> dynamic_glyph_cache_;        // 动态字形缓存实例

    friend class DisplayLockGuard;
    virtual bool Lock(int timeout_ms = 0) = 0;
    virtual void Unlock() = 0;
};

#endif
