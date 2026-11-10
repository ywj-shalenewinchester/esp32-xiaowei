// 机器人表情用的软件光栅器。
//
// Windows 版表情是用 GDI+ 现画的矢量图形（旋转椭圆 + 贝塞尔眉毛/水滴）。
// S3 上 LVGL 没有开矢量绘制（ThorVG 太重），这里把那些矢量形状一次性烘焙成
// 小尺寸 RGBA 精灵，运行时交给 lv_image 做旋转/缩放，只更新少量控件区域。
//
// 所有坐标都是精灵内部像素坐标，颜色是 0xRRGGBB；输出格式是 LVGL 的
// ARGB8888（内存字节序 B,G,R,A，直通 alpha）。

#pragma once

#include <lvgl.h>

#include <cstdint>

namespace face {

struct Sprite {
    lv_image_dsc_t dsc{};
    uint8_t* data = nullptr;
    int w = 0;
    int h = 0;

    bool ok() const { return data != nullptr; }
    void release();
};

// 分配一块 w*h 的透明 ARGB8888 精灵（优先 PSRAM）。
Sprite create(int w, int h);

// 椭圆：中心 (cx, cy)，半径 rx/ry，绕中心旋转 angle_deg。内部用 fill 填满，
// 靠近边缘 edge_width 像素的环用 edge 以 edge_alpha 覆盖在 fill 上
// （edge_alpha == 0 时不画边）。
void fill_ellipse(Sprite& s, float cx, float cy, float rx, float ry, float angle_deg,
                  uint32_t fill, uint32_t edge, uint8_t edge_alpha, float edge_width = 1.0f);

// 闭合多边形（even-odd）。颜色沿 x 从 grad_x0 处的 color0 线性过渡到
// grad_x1 处的 color1；grad_x0 == grad_x1 时使用纯色 color0。
void fill_polygon(Sprite& s, const float* xy, int n, uint32_t color0, uint32_t color1,
                  float grad_x0, float grad_x1, uint8_t alpha);

// 任意方向的线性渐变：color0 在 (ax, ay)，color1 在 (bx, by)。两端之外按端点颜色钳制。
// （v9 的 logo 是竖向渐变、眉毛是横向且右侧反向，所以需要通用版本。）
void fill_polygon_gradient(Sprite& s, const float* xy, int n, uint32_t color0, uint32_t color1,
                           float ax, float ay, float bx, float by, uint8_t alpha);

// 折线描边，圆头圆角，width 单位为像素。
void stroke_polyline(Sprite& s, const float* xy, int n, float width, uint32_t color,
                     uint8_t alpha);

// 把一段三次贝塞尔展平成折线，返回写入的点数。out_xy 需要 2*max_points 个 float。
// 只写 t = 1/t .. 1 的点（不写 t = 0），方便接在已有折线后面。
int flatten_cubic(float x0, float y0, float x1, float y1, float x2, float y2, float x3,
                  float y3, float* out_xy, int max_points, int segments = 12);

}  // namespace face
