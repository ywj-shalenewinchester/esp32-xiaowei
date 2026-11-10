#pragma once

#include <lvgl.h>

// LVGL 字体类
// 用于管理字体
class LvglFont {
public:
    virtual const lv_font_t* font() const = 0;
    virtual void SetFallback(const lv_font_t* fallback) = 0;
    virtual ~LvglFont() = default;
};

// LVGL 内置字体类
// 用于管理内置字体
class LvglBuiltInFont : public LvglFont {
public:
    LvglBuiltInFont(const lv_font_t* font) : font_(*font) {}
    virtual const lv_font_t* font() const override { return &font_; }
    virtual void SetFallback(const lv_font_t* fallback) override { font_.fallback = fallback; }

private:
    lv_font_t font_{};
};

// LVGL CBIN字体类
// 用于管理CBIN字体
class LvglCBinFont : public LvglFont {
public:
    LvglCBinFont(void* data);
    virtual ~LvglCBinFont();
    virtual const lv_font_t* font() const override { return font_; }
    uint8_t bpp() const {
        if (font_ == nullptr || font_->dsc == nullptr) {
            return 0;
        }
        return static_cast<const lv_font_fmt_txt_dsc_t*>(font_->dsc)->bpp;
    }
    virtual void SetFallback(const lv_font_t* fallback) override {
        if (font_ != nullptr) {
            font_->fallback = fallback;
        }
    }

private:
    lv_font_t* font_;
};
