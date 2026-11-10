#include "face_sprite.h"

#include <esp_heap_caps.h>
#include <esp_log.h>

#include <cmath>
#include <cstring>

#define TAG "FaceSprite"

namespace face {
namespace {

// 开机 logo 按 v9 的原始尺寸是 244x240（v9 不对 logo 做 0.8 缩放），
// 所以上限放到 256。
constexpr int kMaxSpriteW = 256;
constexpr int kMaxSpriteH = 256;
constexpr int kMaxEdges = 192;
constexpr int kMaxSubW = kMaxSpriteW * 4;
constexpr float kPi = 3.14159265358979323846f;

struct Rgb {
    int r, g, b;
};

inline Rgb unpack(uint32_t c) {
    return {(int)((c >> 16) & 0xFF), (int)((c >> 8) & 0xFF), (int)(c & 0xFF)};
}

// 直通 alpha 的 source-over 混合（LVGL ARGB8888 的字节序是 B,G,R,A）。
inline void blend_pixel(uint8_t* p, int r, int g, int b, int a) {
    if (a <= 0) return;
    if (a >= 255) {
        p[0] = (uint8_t)b;
        p[1] = (uint8_t)g;
        p[2] = (uint8_t)r;
        p[3] = 255;
        return;
    }
    const int da = p[3];
    const int ia = 255 - a;
    const int out_a255 = a * 255 + da * ia;
    if (out_a255 <= 0) {
        return;
    }
    p[0] = (uint8_t)((b * a * 255 + p[0] * da * ia) / out_a255);
    p[1] = (uint8_t)((g * a * 255 + p[1] * da * ia) / out_a255);
    p[2] = (uint8_t)((r * a * 255 + p[2] * da * ia) / out_a255);
    p[3] = (uint8_t)(out_a255 / 255);
}

inline int clamp_int(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

inline bool point_in_spans(float x, const float* xs, int n) {
    // n 一定是偶数（按 even-odd 成对取区间）。
    for (int i = 0; i + 1 < n; i += 2) {
        if (x >= xs[i] && x < xs[i + 1]) {
            return true;
        }
    }
    return false;
}

inline void sort_floats(float* v, int n) {
    for (int i = 1; i < n; ++i) {
        float key = v[i];
        int j = i - 1;
        while (j >= 0 && v[j] > key) {
            v[j + 1] = v[j];
            --j;
        }
        v[j + 1] = key;
    }
}

}  // namespace

void Sprite::release() {
    if (data != nullptr) {
        heap_caps_free(data);
    }
    data = nullptr;
    w = 0;
    h = 0;
    std::memset(&dsc, 0, sizeof(dsc));
}

Sprite create(int w, int h) {
    Sprite s;
    if (w <= 0 || h <= 0 || w > kMaxSpriteW || h > kMaxSpriteH) {
        return s;
    }

    const size_t stride = (size_t)w * 4;
    const size_t bytes = stride * (size_t)h;
    // 精灵体积较大（几十 KB），只放 PSRAM，绝不回退到内部 RAM 抢 Wi-Fi/音频的资源。
    uint8_t* data =
        (uint8_t*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (data == nullptr) {
        ESP_LOGE(TAG, "PSRAM alloc failed: %dx%d (%u bytes)", w, h, (unsigned)bytes);
        return s;
    }
    std::memset(data, 0, bytes);

    s.data = data;
    s.w = w;
    s.h = h;
    s.dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s.dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
    s.dsc.header.flags = 0;
    s.dsc.header.w = w;
    s.dsc.header.h = h;
    s.dsc.header.stride = (uint32_t)stride;
    s.dsc.data_size = (uint32_t)bytes;
    s.dsc.data = data;
    return s;
}

void fill_ellipse(Sprite& s, float cx, float cy, float rx, float ry, float angle_deg,
                  uint32_t fill, uint32_t edge, uint8_t edge_alpha, float edge_width) {
    if (!s.ok() || rx <= 0.0f || ry <= 0.0f) {
        return;
    }

    const Rgb fc = unpack(fill);
    const Rgb ec = unpack(edge);
    const float ea = (float)edge_alpha / 255.0f;
    // 边环不透明地覆盖在填充上：预先混出最终颜色（和 GDI+ 的 FillPath + DrawPath 接近）。
    const int er = (int)(ec.r * ea + fc.r * (1.0f - ea) + 0.5f);
    const int eg = (int)(ec.g * ea + fc.g * (1.0f - ea) + 0.5f);
    const int eb = (int)(ec.b * ea + fc.b * (1.0f - ea) + 0.5f);

    const float a = angle_deg * kPi / 180.0f;
    const float ca = std::cos(a);
    const float sa = std::sin(a);
    const float rmin = std::fmin(rx, ry);
    const bool has_edge = edge_alpha > 0 && edge_width > 0.0f;

    const float reach = std::fmax(rx, ry) + edge_width + 2.0f;
    const int x0 = clamp_int((int)std::floor(cx - reach), 0, s.w - 1);
    const int x1 = clamp_int((int)std::ceil(cx + reach), 0, s.w - 1);
    const int y0 = clamp_int((int)std::floor(cy - reach), 0, s.h - 1);
    const int y1 = clamp_int((int)std::ceil(cy + reach), 0, s.h - 1);

    const float inv_rx2 = 1.0f / (rx * rx);
    const float inv_ry2 = 1.0f / (ry * ry);

    for (int py = y0; py <= y1; ++py) {
        for (int px = x0; px <= x1; ++px) {
            int sum_r = 0, sum_g = 0, sum_b = 0, count = 0;
            for (int sy = 0; sy < 4; ++sy) {
                const float dy = (float)py + ((float)sy + 0.5f) * 0.25f - cy;
                for (int sx = 0; sx < 4; ++sx) {
                    const float dx = (float)px + ((float)sx + 0.5f) * 0.25f - cx;
                    // 旋转到椭圆本地坐标（绕中心转 -angle）。
                    const float u = dx * ca + dy * sa;
                    const float v = -dx * sa + dy * ca;
                    const float rr = u * u * inv_rx2 + v * v * inv_ry2;
                    if (rr > 1.0f) {
                        continue;
                    }
                    bool is_edge = false;
                    if (has_edge) {
                        const float dist = (1.0f - std::sqrt(rr)) * rmin;
                        is_edge = dist < edge_width;
                    }
                    if (is_edge) {
                        sum_r += er;
                        sum_g += eg;
                        sum_b += eb;
                    } else {
                        sum_r += fc.r;
                        sum_g += fc.g;
                        sum_b += fc.b;
                    }
                    ++count;
                }
            }
            if (count == 0) {
                continue;
            }
            const int alpha = count * 255 / 16;
            uint8_t* p = s.data + ((size_t)py * s.w + px) * 4;
            blend_pixel(p, sum_r / count, sum_g / count, sum_b / count, alpha);
        }
    }
}

void fill_polygon(Sprite& s, const float* xy, int n, uint32_t color0, uint32_t color1,
                  float grad_x0, float grad_x1, uint8_t alpha) {
    fill_polygon_gradient(s, xy, n, color0, color1, grad_x0, 0.0f, grad_x1, 0.0f, alpha);
}

void fill_polygon_gradient(Sprite& s, const float* xy, int n, uint32_t color0, uint32_t color1,
                           float ax, float ay, float bx, float by, uint8_t alpha) {
    if (!s.ok() || n < 3 || alpha == 0) {
        return;
    }

    float min_x = xy[0], max_x = xy[0], min_y = xy[1], max_y = xy[1];
    for (int i = 1; i < n; ++i) {
        min_x = std::fmin(min_x, xy[i * 2]);
        max_x = std::fmax(max_x, xy[i * 2]);
        min_y = std::fmin(min_y, xy[i * 2 + 1]);
        max_y = std::fmax(max_y, xy[i * 2 + 1]);
    }

    const int x0 = clamp_int((int)std::floor(min_x), 0, s.w - 1);
    const int x1 = clamp_int((int)std::ceil(max_x), 0, s.w - 1);
    const int y0 = clamp_int((int)std::floor(min_y), 0, s.h - 1);
    const int y1 = clamp_int((int)std::ceil(max_y), 0, s.h - 1);
    if (x1 < x0 || y1 < y0) {
        return;
    }

    const Rgb c0 = unpack(color0);
    const Rgb c1 = unpack(color1);
    const float gdx = bx - ax;
    const float gdy = by - ay;
    const float glen2 = gdx * gdx + gdy * gdy;
    const bool gradient = glen2 > 0.0001f;

    float xs[kMaxEdges];
    uint8_t sub[kMaxSubW];
    const int row_px = x1 - x0 + 1;
    if (row_px * 4 > kMaxSubW) {
        return;
    }

    for (int py = y0; py <= y1; ++py) {
        uint8_t cov[kMaxSpriteW];
        std::memset(cov, 0, (size_t)row_px);

        for (int sy = 0; sy < 4; ++sy) {
            const float yy = (float)py + ((float)sy + 0.5f) * 0.25f;
            int m = 0;
            for (int i = 0; i < n; ++i) {
                const int j = (i + 1) % n;
                const float ax = xy[i * 2];
                const float ay = xy[i * 2 + 1];
                const float bx = xy[j * 2];
                const float by = xy[j * 2 + 1];
                if ((ay <= yy && by > yy) || (by <= yy && ay > yy)) {
                    const float t = (yy - ay) / (by - ay);
                    if (m < kMaxEdges) {
                        xs[m++] = ax + t * (bx - ax);
                    }
                }
            }
            if (m < 2) {
                continue;
            }
            sort_floats(xs, m);
            std::memset(sub, 0, (size_t)(row_px * 4));
            for (int i = 0; i + 1 < m; i += 2) {
                const float a = xs[i];
                const float b = xs[i + 1];
                if (b <= a) {
                    continue;
                }
                // 直接按 4x 水平采样填这一行。
                int sx0 = (int)std::ceil((a - (float)x0) * 4.0f - 0.5f);
                int sx1 = (int)std::floor((b - (float)x0) * 4.0f - 0.5f);
                sx0 = clamp_int(sx0, 0, row_px * 4 - 1);
                sx1 = clamp_int(sx1, 0, row_px * 4 - 1);
                for (int k = sx0; k <= sx1; ++k) {
                    sub[k] = 1;
                }
            }
            for (int i = 0; i < row_px; ++i) {
                cov[i] = (uint8_t)(cov[i] + sub[i * 4] + sub[i * 4 + 1] + sub[i * 4 + 2] +
                                   sub[i * 4 + 3]);
            }
        }

        for (int i = 0; i < row_px; ++i) {
            const int c = cov[i];
            if (c == 0) {
                continue;
            }
            const int px = x0 + i;
            int r = c0.r, g = c0.g, b = c0.b;
            if (gradient) {
                const float px_c = (float)px + 0.5f;
                const float py_c = (float)py + 0.5f;
                float u = ((px_c - ax) * gdx + (py_c - ay) * gdy) / glen2;
                u = u < 0.0f ? 0.0f : (u > 1.0f ? 1.0f : u);
                r = (int)(c0.r + (c1.r - c0.r) * u + 0.5f);
                g = (int)(c0.g + (c1.g - c0.g) * u + 0.5f);
                b = (int)(c0.b + (c1.b - c0.b) * u + 0.5f);
            }
            const int out_alpha = (int)c * (int)alpha / 16;
            if (out_alpha <= 0) {
                continue;
            }
            uint8_t* p = s.data + ((size_t)py * s.w + px) * 4;
            blend_pixel(p, r, g, b, out_alpha);
        }
    }
}

void stroke_polyline(Sprite& s, const float* xy, int n, float width, uint32_t color,
                     uint8_t alpha) {
    if (!s.ok() || n < 2 || alpha == 0 || width <= 0.0f) {
        return;
    }

    const Rgb c = unpack(color);
    const float half = width * 0.5f;
    const float pad = half + 1.5f;

    float min_x = xy[0], max_x = xy[0], min_y = xy[1], max_y = xy[1];
    for (int i = 1; i < n; ++i) {
        min_x = std::fmin(min_x, xy[i * 2]);
        max_x = std::fmax(max_x, xy[i * 2]);
        min_y = std::fmin(min_y, xy[i * 2 + 1]);
        max_y = std::fmax(max_y, xy[i * 2 + 1]);
    }
    const int x0 = clamp_int((int)std::floor(min_x - pad), 0, s.w - 1);
    const int x1 = clamp_int((int)std::ceil(max_x + pad), 0, s.w - 1);
    const int y0 = clamp_int((int)std::floor(min_y - pad), 0, s.h - 1);
    const int y1 = clamp_int((int)std::ceil(max_y + pad), 0, s.h - 1);

    for (int py = y0; py <= y1; ++py) {
        for (int px = x0; px <= x1; ++px) {
            const float qx = (float)px + 0.5f;
            const float qy = (float)py + 0.5f;
            float best = 1e9f;
            for (int i = 0; i + 1 < n; ++i) {
                const float ax = xy[i * 2];
                const float ay = xy[i * 2 + 1];
                const float bx = xy[i * 2 + 2];
                const float by = xy[i * 2 + 3];
                const float vx = bx - ax;
                const float vy = by - ay;
                const float wx = qx - ax;
                const float wy = qy - ay;
                const float len2 = vx * vx + vy * vy;
                float t = 0.0f;
                if (len2 > 0.0001f) {
                    t = (wx * vx + wy * vy) / len2;
                    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
                }
                const float dx = wx - vx * t;
                const float dy = wy - vy * t;
                const float d2 = dx * dx + dy * dy;
                if (d2 < best) {
                    best = d2;
                }
            }
            const float d = std::sqrt(best);
            if (d >= half + 0.5f) {
                continue;
            }
            float cov = half + 0.5f - d;
            if (cov > 1.0f) {
                cov = 1.0f;
            }
            const int a = (int)(cov * (float)alpha + 0.5f);
            if (a <= 0) {
                continue;
            }
            uint8_t* p = s.data + ((size_t)py * s.w + px) * 4;
            blend_pixel(p, c.r, c.g, c.b, a);
        }
    }
}

int flatten_cubic(float x0, float y0, float x1, float y1, float x2, float y2, float x3,
                  float y3, float* out_xy, int max_points, int segments) {
    if (segments < 1) {
        segments = 1;
    }
    int count = 0;
    for (int i = 1; i <= segments && count < max_points; ++i) {
        const float t = (float)i / (float)segments;
        const float mt = 1.0f - t;
        const float a = mt * mt * mt;
        const float b = 3.0f * mt * mt * t;
        const float c = 3.0f * mt * t * t;
        const float d = t * t * t;
        out_xy[count * 2] = a * x0 + b * x1 + c * x2 + d * x3;
        out_xy[count * 2 + 1] = a * y0 + b * y1 + c * y2 + d * y3;
        ++count;
    }
    return count;
}

}  // namespace face
