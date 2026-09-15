#pragma once

#include <lvgl.h>

// LVGL 图片类
// 用于管理图片
class LvglImage {
public:
    virtual const lv_img_dsc_t* image_dsc() const = 0;
    virtual bool IsGif() const { return false; }
    virtual ~LvglImage() = default;
};

// LVGL 原始图片类
// 用于管理原始图片
class LvglRawImage : public LvglImage {
public:
    LvglRawImage(void* data, size_t size);
    virtual const lv_img_dsc_t* image_dsc() const override { return &image_dsc_; }
    virtual bool IsGif() const;

private:
    lv_img_dsc_t image_dsc_;
};

// LVGL CBIN图片类
// 用于管理CBIN图片
class LvglCBinImage : public LvglImage {
public:
    LvglCBinImage(void* data);
    virtual ~LvglCBinImage();
    virtual const lv_img_dsc_t* image_dsc() const override { return image_dsc_; }

private:
    lv_img_dsc_t* image_dsc_ = nullptr;
};

// LVGL 分配图片类
// 用于管理分配的图片
class LvglAllocatedImage : public LvglImage {
public:
    LvglAllocatedImage(void* data, size_t size);
    LvglAllocatedImage(void* data, size_t size, int width, int height, int stride,
                       int color_format);
    virtual ~LvglAllocatedImage();
    virtual const lv_img_dsc_t* image_dsc() const override { return &image_dsc_; }

private:
    lv_img_dsc_t image_dsc_;
};
