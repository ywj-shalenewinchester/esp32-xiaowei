#ifndef LCD_DISPLAY_H
#define LCD_DISPLAY_H

#include "gif/lvgl_gif.h"
#include "lvgl_display.h"

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <atomic>
#include <memory>

// 预览图片显示时间，单位毫秒
#define PREVIEW_IMAGE_DURATION_MS 5000

class LcdDisplay : public LvglDisplay {
protected:
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;

    lv_draw_buf_t draw_buf_;
    lv_obj_t* top_bar_ = nullptr;
    lv_obj_t* status_bar_ = nullptr;
    lv_obj_t* content_ = nullptr;
    lv_obj_t* container_ = nullptr;
    lv_obj_t* side_bar_ = nullptr;
    lv_obj_t* bottom_bar_ = nullptr;
    lv_obj_t* preview_image_ = nullptr;
    lv_obj_t* screen_image_ = nullptr;  // 服务端下发的整屏图片（见 SetScreenImage）
    lv_obj_t* emoji_label_ = nullptr;
    lv_obj_t* emoji_image_ = nullptr;
    std::unique_ptr<LvglGif> gif_controller_ = nullptr;
    lv_obj_t* emoji_box_ = nullptr;
    lv_obj_t* chat_message_label_ = nullptr;
    esp_timer_handle_t preview_timer_ = nullptr;
    std::unique_ptr<LvglImage> preview_image_cached_ = nullptr;
    std::unique_ptr<LvglImage> screen_image_cached_ = nullptr;
    bool hide_subtitle_ = false;  // Control whether to hide chat messages/subtitles

    void InitializeLcdThemes();
    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;

protected:
    // Add protected constructor
    LcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
               int height);

public:
    ~LcdDisplay();
    virtual void SetEmotion(const char* emotion) override;
    virtual void SetChatMessage(const char* role, const char* content) override;
    virtual void ClearChatMessages() override;
    virtual void SetPreviewImage(std::unique_ptr<LvglImage> image) override;
    // 服务端下发的图片铺满整屏（AI 画图），与上面那个半屏预览框互不干扰
    virtual void SetScreenImage(std::unique_ptr<LvglImage> image) override;
    // 收起整屏图片（点一下图片、或超时都会调到这里）
    void HideScreenImage();
    // 整屏图片刚从"显示中"变成"收起"（点击 / 超时两条路都会走到）。
    // 子类在这里做收尾 —— 比如小未那套"回到看图之前那一屏"。
    // 回调时**LVGL 锁已在手上**（lvgl_port_lock 是可重入的）。
    virtual void OnScreenImageHidden() {}
    // 当前是否正显示着一张服务端下发的整屏图片
    bool HasScreenImage() const { return screen_image_cached_ != nullptr; }
    // 那张整屏图片的像素描述符（CF/W/H/stride/data 都在里面），没显示时是 nullptr。
    // 给"把 AI 画的图存进相册"这类需要拿原始像素的调用方用 —— 这套像素只在这张图
    // 显示期间有效（收起时 SetScreenImage 换掉的 unique_ptr 会把它一起释放），
    // 所以调用方必须在回调里当场拷走，不能留指针。返回的 dsc 属于本对象，别改它，
    // 也别 delete。带锁调用：DisplayLockGuard 之下。
    const lv_img_dsc_t* GetScreenImageDsc() const {
        return screen_image_cached_ ? screen_image_cached_->image_dsc() : nullptr;
    }
    virtual void SetupUI() override;
    // Add theme switching function
    virtual void SetTheme(Theme* theme) override;

    // Set whether to hide chat messages/subtitles
    void SetHideSubtitle(bool hide);
};

// SPI LCD display
class SpiLcdDisplay : public LcdDisplay {
public:
    SpiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                  int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                  bool swap_xy);
};

// RGB LCD display
class RgbLcdDisplay : public LcdDisplay {
public:
    RgbLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                  int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                  bool swap_xy);
};

// MIPI LCD display
class MipiLcdDisplay : public LcdDisplay {
public:
    MipiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                   int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                   bool swap_xy);
};

#endif  // LCD_DISPLAY_H
