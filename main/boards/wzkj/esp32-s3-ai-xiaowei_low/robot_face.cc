#include "robot_face.h"

#include "assets/lang_config.h"
#include "expression_geometry.h"

#include <esp_heap_caps.h>
#include <esp_log.h>

#include <cmath>
#include <cstring>

#define TAG "RobotFace"

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr int kFrameMs = 33;

// 眼睛/眉毛的轮廓来自 expression_geometry.h，坐标是 450 设计空间的 Q2 定点数。
// v9 用 part() 把它们缩放 0.8 后摆到屏幕上，这里把 0.8 直接烘焙进精灵。
constexpr float kBakeScale = 0.8f;

// ---- 程序化绘制的精灵尺寸（眼睛/眉毛/logo 的尺寸在烘焙时按轮廓算） ----
constexpr int kClosedW = 120;
constexpr int kClosedH = 48;
constexpr int kDropW = 26;
constexpr int kDropH = 32;
constexpr int kMarkW = 64;
constexpr int kMarkH = 48;
constexpr int kHeartW = 44;
constexpr int kHeartH = 44;
constexpr int kStarW = 24;
constexpr int kStarH = 24;
constexpr int kNoteW = 56;
constexpr int kNoteH = 48;
constexpr int kWifiW = 116;
constexpr int kWifiH = 52;
constexpr int kZW = 28;
constexpr int kZH = 30;

// v9 的 450 设计空间矩形与屏幕放置中心。
struct RectDef {
    float x, y, w, h;
};
constexpr RectDef kIdleEyeL{13, 187, 196, 216};
constexpr RectDef kIdleEyeR{241, 187, 196, 216};
constexpr RectDef kAngryEyeL{0, 222, 163, 137};
constexpr RectDef kAngryEyeR{273, 222, 165, 137};
constexpr RectDef kIdleBrowL{86, 118, 91, 75};
constexpr RectDef kIdleBrowR{273, 118, 91, 75};
constexpr RectDef kAngryBrowL{6, 136, 158, 67};
constexpr RectDef kAngryBrowR{282, 137, 163, 88};

constexpr float kIdleEyeLX = 89.0f;
constexpr float kIdleEyeRX = 271.0f;
constexpr float kIdleEyeY = 236.0f;
constexpr float kAngryEyeLX = 65.0f;
constexpr float kAngryEyeRX = 284.0f;
constexpr float kAngryEyeY = 232.0f;

constexpr float kIdleBrowLX = 105.0f;
constexpr float kIdleBrowRX = 255.0f;
constexpr float kIdleBrowY = 124.0f;
constexpr float kAngryBrowLX = 68.0f;
constexpr float kAngryBrowRX = 291.0f;
constexpr float kAngryBrowLY = 136.0f;
constexpr float kAngryBrowRY = 145.0f;

float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

float smoothstep(float a, float b, float x) {
    const float u = clamp01((x - a) / (b - a));
    return u * u * (3.0f - 2.0f * u);
}

// v9: 3.4 s 一个周期，1.25 s 处眨眼 0.19 s。
float blink_value(float t, float phase = 0.0f) {
    float p = std::fmod(t + phase, 3.4f);
    if (p < 0.0f) {
        p += 3.4f;
    }
    if (p < 1.25f || p > 1.44f) {
        return 0.0f;
    }
    return std::sin((p - 1.25f) / 0.19f * kPi);
}

// v9: 9.6 s 一个周期的东张西望。
float gaze(float t) {
    float p = std::fmod(t, 9.6f);
    if (p < 1.4f) return 0.0f;
    if (p < 2.1f) return -7.0f * smoothstep(1.4f, 2.1f, p);
    if (p < 3.7f) return -7.0f;
    if (p < 4.4f) return -7.0f + 7.0f * smoothstep(3.7f, 4.4f, p);
    if (p < 5.5f) return 0.0f;
    if (p < 6.2f) return 7.0f * smoothstep(5.5f, 6.2f, p);
    if (p < 7.8f) return 7.0f;
    if (p < 8.5f) return 7.0f - 7.0f * smoothstep(7.8f, 8.5f, p);
    return 0.0f;
}

uint32_t image_zoom(float scale) {
    if (scale <= 0.0f) {
        return 1;
    }
    const long z = lroundf(scale * 256.0f);
    return (uint32_t)(z < 1 ? 1 : (z > 4096 ? 4096 : z));
}

void show_obj(lv_obj_t* obj, bool show) {
    if (obj == nullptr) {
        return;
    }
    if (show) {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

// 线的 self-size 取所有点的最大坐标。若直接用屏幕绝对坐标，每帧都会让整屏失效；
// 这里把点改成以包围盒左上角为原点的局部坐标，并把对象移到包围盒处。
void place_line(lv_obj_t* obj, lv_point_precise_t* pts, uint32_t n) {
    if (obj == nullptr || pts == nullptr || n == 0) {
        return;
    }
    int32_t min_x = (int32_t)pts[0].x, min_y = (int32_t)pts[0].y;
    int32_t max_x = min_x, max_y = min_y;
    for (uint32_t i = 1; i < n; ++i) {
        const int32_t x = (int32_t)pts[i].x;
        const int32_t y = (int32_t)pts[i].y;
        min_x = x < min_x ? x : min_x;
        min_y = y < min_y ? y : min_y;
        max_x = x > max_x ? x : max_x;
        max_y = y > max_y ? y : max_y;
    }
    // 2 px 边距，给 3 px 线宽的圆头描边留余量。
    for (uint32_t i = 0; i < n; ++i) {
        pts[i].x = pts[i].x - min_x + 2;
        pts[i].y = pts[i].y - min_y + 2;
    }
    lv_obj_set_pos(obj, min_x - 2, min_y - 2);
    lv_obj_set_size(obj, max_x - min_x + 5, max_y - min_y + 5);
    lv_line_set_points(obj, pts, n);
}

lv_obj_t* create_sprite_obj(lv_obj_t* parent, const face::Sprite& sprite, int pivot_x,
                            int pivot_y) {
    if (!sprite.ok()) {
        return nullptr;
    }
    lv_obj_t* obj = lv_image_create(parent);
    lv_image_set_src(obj, &sprite.dsc);
    lv_obj_set_size(obj, sprite.w, sprite.h);
    lv_image_set_pivot(obj, pivot_x, pivot_y);
    lv_image_set_antialias(obj, true);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

// ---- 精确轮廓烘焙（expression_geometry.h 的 Q2 贝塞尔数据） ----

struct ShapeXform {
    float scale;
    float ox, oy;
    float tx, ty;

    float px(int q2x) const { return (q2x * 0.25f + ox) * scale + tx; }
    float py(int q2y) const { return (q2y * 0.25f + oy) * scale + ty; }
    float dx(float d) const { return (d + ox) * scale + tx; }
    float dy(float d) const { return (d + oy) * scale + ty; }
};

// 对应 v9 的 part()：先平移 -rect 中心，再缩放 0.8，最后放到 (cx, cy)。
ShapeXform part_xform(const RectDef& r, float cx, float cy) {
    return {kBakeScale, -r.x - r.w * 0.5f, -r.y - r.h * 0.5f, cx, cy};
}

void shape_bounds(const ShapeData* const* shapes, int n, const ShapeXform& xf, float* min_x,
                  float* max_x, float* min_y, float* max_y) {
    bool first = true;
    for (int s = 0; s < n; ++s) {
        const ShapeData& shape = *shapes[s];
        for (int i = 0; i < shape.count * 3; ++i) {
            const float x = xf.px(shape.xy[i * 2]);
            const float y = xf.py(shape.xy[i * 2 + 1]);
            if (first) {
                *min_x = *max_x = x;
                *min_y = *max_y = y;
                first = false;
            } else {
                *min_x = x < *min_x ? x : *min_x;
                *max_x = x > *max_x ? x : *max_x;
                *min_y = y < *min_y ? y : *min_y;
                *max_y = y > *max_y ? y : *max_y;
            }
        }
    }
}

// 把闭合的三次贝塞尔链展平成精灵局部坐标的折线。返回点数。
int flatten_shape(const ShapeData& shape, const ShapeXform& xf, int origin_x, int origin_y,
                  float* out) {
    float px = xf.px(shape.xy[0]) - origin_x;
    float py = xf.py(shape.xy[1]) - origin_y;
    int n = 0;
    out[n * 2] = px;
    out[n * 2 + 1] = py;
    ++n;
    constexpr int kSegments = 10;
    for (int i = 0; i < shape.count; ++i) {
        const int next = (i + 1) % shape.count;
        const float c1x = xf.px(shape.xy[(i * 3 + 1) * 2]) - origin_x;
        const float c1y = xf.py(shape.xy[(i * 3 + 1) * 2 + 1]) - origin_y;
        const float c2x = xf.px(shape.xy[(i * 3 + 2) * 2]) - origin_x;
        const float c2y = xf.py(shape.xy[(i * 3 + 2) * 2 + 1]) - origin_y;
        const float ex = xf.px(shape.xy[next * 6]) - origin_x;
        const float ey = xf.py(shape.xy[next * 6 + 1]) - origin_y;
        n += face::flatten_cubic(px, py, c1x, c1y, c2x, c2y, ex, ey, out + n * 2, 400 - n,
                                 kSegments);
        px = ex;
        py = ey;
    }
    return n;
}

void fill_shape(face::Sprite& sprite, const ShapeData& shape, const ShapeXform& xf, int origin_x,
                int origin_y, uint32_t color, float* scratch, float stroke_width = 0.0f) {
    const int n = flatten_shape(shape, xf, origin_x, origin_y, scratch);
    face::fill_polygon(sprite, scratch, n, color, color, 0.0f, 0.0f, 255);
    if (stroke_width > 0.0f) {
        scratch[n * 2] = scratch[0];
        scratch[n * 2 + 1] = scratch[1];
        face::stroke_polyline(sprite, scratch, n + 1, stroke_width, color, 255);
    }
}

void fill_shape_gradient(face::Sprite& sprite, const ShapeData& shape, const ShapeXform& xf,
                         int origin_x, int origin_y, uint32_t color0, uint32_t color1, float ax,
                         float ay, float bx, float by, float* scratch) {
    const int n = flatten_shape(shape, xf, origin_x, origin_y, scratch);
    face::fill_polygon_gradient(sprite, scratch, n, color0, color1, ax, ay, bx, by, 255);
}

float* alloc_scratch() {
    return (float*)heap_caps_malloc(800 * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

// 眼睛：眼白、rim、瞳孔（含黑边）、高光、圆点，整只眼烘焙成一张精灵。
// 这样运行时的一次缩放就等价于 v9 对整只眼的 part() 变换。
face::Sprite bake_eye(bool angry, bool right, uint16_t pivot[2]) {
    const RectDef& rect = angry ? (right ? kAngryEyeR : kAngryEyeL)
                                : (right ? kIdleEyeR : kIdleEyeL);
    const float cx = angry ? (right ? kAngryEyeRX : kAngryEyeLX)
                           : (right ? kIdleEyeRX : kIdleEyeLX);
    const float cy = angry ? kAngryEyeY : kIdleEyeY;
    const ShapeXform xf = part_xform(rect, cx, cy);

    const ShapeData* layers[5];
    int count = 0;
    if (angry) {
        layers[count++] = right ? &ANGRY_WHITE_R : &ANGRY_WHITE_L;
        layers[count++] = right ? &ANGRY_BLACK_R : &ANGRY_BLACK_L;
        layers[count++] = right ? &ANGRY_GLINT_R : &ANGRY_GLINT_L;
        layers[count++] = right ? &ANGRY_DOT_R : &ANGRY_DOT_L;
    } else {
        layers[count++] = right ? &IDLE_WHITE_R : &IDLE_WHITE_L;
        layers[count++] = right ? &IDLE_RIM_R : &IDLE_RIM_L;
        layers[count++] = right ? &IDLE_BLACK_R : &IDLE_BLACK_L;
        layers[count++] = right ? &IDLE_GLINT_R : &IDLE_GLINT_L;
        layers[count++] = right ? &IDLE_DOT_R : &IDLE_DOT_L;
    }

    float min_x, max_x, min_y, max_y;
    shape_bounds(layers, count, xf, &min_x, &max_x, &min_y, &max_y);
    // 留 2 px 边：瞳孔黑边和抗锯齿都不会被裁掉。
    const int origin_x = (int)std::floor(min_x) - 2;
    const int origin_y = (int)std::floor(min_y) - 2;
    const int w = (int)std::ceil(max_x) - origin_x + 3;
    const int h = (int)std::ceil(max_y) - origin_y + 3;

    face::Sprite sprite = face::create(w, h);
    if (!sprite.ok()) {
        return sprite;
    }
    pivot[0] = (uint16_t)lroundf(cx - origin_x);
    pivot[1] = (uint16_t)lroundf(cy - origin_y);

    float* scratch = alloc_scratch();
    if (scratch == nullptr) {
        sprite.release();
        return sprite;
    }

    int i = 0;
    fill_shape(sprite, *layers[i++], xf, origin_x, origin_y, 0xFFFFFF, scratch);
    if (angry) {
        fill_shape(sprite, *layers[i++], xf, origin_x, origin_y, 0x000000, scratch);
    } else {
        fill_shape(sprite, *layers[i++], xf, origin_x, origin_y, 0x464646, scratch);
        // v9 给瞳孔加 2.4 设计像素的黑边，0.8 缩放后约 1.92 px。
        fill_shape(sprite, *layers[i++], xf, origin_x, origin_y, 0x000000, scratch, 1.92f);
    }
    fill_shape(sprite, *layers[i++], xf, origin_x, origin_y, 0x666666, scratch);
    fill_shape(sprite, *layers[i++], xf, origin_x, origin_y, 0xFFFFFF, scratch);

    heap_caps_free(scratch);
    return sprite;
}

// 眉毛：横向橙金渐变。左眉从深到浅、右眉反向，渐变端点按 v9 的 gradientShape。
face::Sprite bake_brow(bool angry, bool right, uint16_t pivot[2]) {
    const RectDef& rect = angry ? (right ? kAngryBrowR : kAngryBrowL)
                                : (right ? kIdleBrowR : kIdleBrowL);
    const float cx = angry ? (right ? kAngryBrowRX : kAngryBrowLX)
                           : (right ? kIdleBrowRX : kIdleBrowLX);
    const float cy = angry ? (right ? kAngryBrowRY : kAngryBrowLY) : kIdleBrowY;
    const ShapeXform xf = part_xform(rect, cx, cy);
    const ShapeData& shape = angry ? (right ? ANGRY_BROW_R : ANGRY_BROW_L)
                                   : (right ? IDLE_BROW_R : IDLE_BROW_L);

    const ShapeData* one = &shape;
    float min_x, max_x, min_y, max_y;
    shape_bounds(&one, 1, xf, &min_x, &max_x, &min_y, &max_y);
    const int origin_x = (int)std::floor(min_x) - 1;
    const int origin_y = (int)std::floor(min_y) - 1;
    const int w = (int)std::ceil(max_x) - origin_x + 2;
    const int h = (int)std::ceil(max_y) - origin_y + 2;

    face::Sprite sprite = face::create(w, h);
    if (!sprite.ok()) {
        return sprite;
    }
    pivot[0] = (uint16_t)lroundf(cx - origin_x);
    pivot[1] = (uint16_t)lroundf(cy - origin_y);

    float* scratch = alloc_scratch();
    if (scratch == nullptr) {
        sprite.release();
        return sprite;
    }

    const float gx0 = angry ? (right ? 440.0f : 11.0f) : (right ? 359.0f : 91.0f);
    const float gx1 = angry ? (right ? 287.0f : 160.0f) : (right ? 278.0f : 172.0f);
    const float ax = xf.dx(gx0) - origin_x;
    const float bx = xf.dx(gx1) - origin_x;
    const float gy = xf.dy(160.0f) - origin_y;
    fill_shape_gradient(sprite, shape, xf, origin_x, origin_y, 0xF19D24, 0xFFDC99, ax, gy, bx,
                        gy, scratch);

    heap_caps_free(scratch);
    return sprite;
}

// 生气时鼻梁上的两撇浅色皱纹（v9 只在静态生气脸里画）。
face::Sprite bake_wrinkles(int* pos_x, int* pos_y) {
    const ShapeXform xf{0.8f, 0.0f, 0.0f, 0.0f, 0.0f};
    const ShapeData* layers[2] = {&ANGRY_WRINKLE_L, &ANGRY_WRINKLE_R};
    float min_x, max_x, min_y, max_y;
    shape_bounds(layers, 2, xf, &min_x, &max_x, &min_y, &max_y);
    const int origin_x = (int)std::floor(min_x) - 1;
    const int origin_y = (int)std::floor(min_y) - 1;
    const int w = (int)std::ceil(max_x) - origin_x + 2;
    const int h = (int)std::ceil(max_y) - origin_y + 2;

    face::Sprite sprite = face::create(w, h);
    if (!sprite.ok()) {
        return sprite;
    }
    *pos_x = origin_x;
    *pos_y = origin_y;

    float* scratch = alloc_scratch();
    if (scratch == nullptr) {
        sprite.release();
        return sprite;
    }
    fill_shape(sprite, *layers[0], xf, origin_x, origin_y, 0xFDDAB3, scratch);
    fill_shape(sprite, *layers[1], xf, origin_x, origin_y, 0xFDDAB3, scratch);
    heap_caps_free(scratch);
    return sprite;
}

// 开机 logo：v9 不做 0.8 缩放，按原始 450 空间尺寸（约 244x240）烘焙，
// 运行时再用 0.72~1.0 的缩放播放动画。渐变是竖向的。
face::Sprite bake_logo(uint16_t pivot[2]) {
    const ShapeXform xf{1.0f, -126.0f, -124.0f, 0.0f, 0.0f};
    const ShapeData* body = &LOGO_BODY;
    float min_x, max_x, min_y, max_y;
    shape_bounds(&body, 1, xf, &min_x, &max_x, &min_y, &max_y);
    const int origin_x = (int)std::floor(min_x) - 1;
    const int origin_y = (int)std::floor(min_y) - 1;
    const int w = (int)std::ceil(max_x) - origin_x + 2;
    const int h = (int)std::ceil(max_y) - origin_y + 2;

    face::Sprite sprite = face::create(w, h);
    if (!sprite.ok()) {
        return sprite;
    }
    // v9 的 logo() 以 (0,0) 为缩放中心，也就是精灵里的这个点。
    pivot[0] = (uint16_t)(-origin_x);
    pivot[1] = (uint16_t)(-origin_y);

    float* scratch = alloc_scratch();
    if (scratch == nullptr) {
        sprite.release();
        return sprite;
    }
    const float gy0 = -120.0f - (float)origin_y;
    const float gy1 = 120.0f - (float)origin_y;
    fill_shape_gradient(sprite, LOGO_BODY, xf, origin_x, origin_y, 0x5E238F, 0x3B0777, 0.0f,
                        gy0, 0.0f, gy1, scratch);
    fill_shape(sprite, LOGO_HOLE_L, xf, origin_x, origin_y, 0x000000, scratch);
    fill_shape(sprite, LOGO_HOLE_R, xf, origin_x, origin_y, 0x000000, scratch);
    heap_caps_free(scratch);
    return sprite;
}

bool ieq(const char* a, const char* b) {
    while (*a != '\0' && *b != '\0') {
        char ca = *a;
        char cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) {
            return false;
        }
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

enum class EmotionAction : uint8_t {
    Ignore,  // 不认识：保持当前表情
    Clear,   // neutral：回到基础表情
    Set,
};

// 服务端（core/utils/textUtils.py 的 EMOJI_MAP）实际会下发这些名字：
// 开心 happy/laughing/funny/loving/silly/cool/delicious/kissy/confident/winking，
// 生气 angry，伤心 sad/crying，恐惧 shocked，厌恶 disgusted，惊讶 surprised，
// 中性 neutral，另外还有 thinking/sleepy/relaxed/confused/embarrassed。
// 用户只要求前 7 类；这里把同一类的同义词都收进来，剩下没列的忽略。
EmotionAction map_emotion(const char* e, FaceExpression* out) {
    if (e == nullptr || e[0] == '\0') {
        return EmotionAction::Ignore;
    }
    if (ieq(e, "neutral") || ieq(e, "idle") || ieq(e, "relaxed") ||
        std::strcmp(e, "中性") == 0 || std::strcmp(e, "空闲") == 0) {
        return EmotionAction::Clear;
    }
    if (ieq(e, "happy") || ieq(e, "happiness") || ieq(e, "joy") || ieq(e, "laughing") ||
        ieq(e, "funny") || ieq(e, "loving") || ieq(e, "silly") || ieq(e, "cool") ||
        ieq(e, "delicious") || ieq(e, "kissy") || ieq(e, "confident") || ieq(e, "winking") ||
        std::strcmp(e, "开心") == 0 || std::strcmp(e, "高兴") == 0) {
        *out = FaceExpression::Happy;
        return EmotionAction::Set;
    }
    if (ieq(e, "angry") || ieq(e, "anger") || ieq(e, "mad") || ieq(e, "furious") ||
        std::strcmp(e, "生气") == 0 || std::strcmp(e, "愤怒") == 0) {
        *out = FaceExpression::Angry;
        return EmotionAction::Set;
    }
    if (ieq(e, "sad") || ieq(e, "sadness") || ieq(e, "sorrow") || ieq(e, "crying") ||
        std::strcmp(e, "伤心") == 0 || std::strcmp(e, "悲伤") == 0 ||
        std::strcmp(e, "难过") == 0) {
        *out = FaceExpression::Sad;
        return EmotionAction::Set;
    }
    // 服务端 😱 对应的是 shocked，这是“恐惧”唯一的实际来源。
    if (ieq(e, "fear") || ieq(e, "fearful") || ieq(e, "afraid") || ieq(e, "scared") ||
        ieq(e, "shocked") || ieq(e, "terrified") ||
        std::strcmp(e, "恐惧") == 0 || std::strcmp(e, "害怕") == 0) {
        *out = FaceExpression::Fear;
        return EmotionAction::Set;
    }
    if (ieq(e, "disgust") || ieq(e, "disgusted") || ieq(e, "disgusting") ||
        std::strcmp(e, "厌恶") == 0 || std::strcmp(e, "反感") == 0) {
        *out = FaceExpression::Disgust;
        return EmotionAction::Set;
    }
    if (ieq(e, "surprise") || ieq(e, "surprised") || ieq(e, "astonished") ||
        ieq(e, "amazed") || std::strcmp(e, "惊讶") == 0) {
        *out = FaceExpression::Surprise;
        return EmotionAction::Set;
    }
    // 注意：服务端的 thinking/sleepy 只是模型正文里带的 🤔/😴 表情，
    // 会一直挂到整段回复（甚至一首歌）播完。Think/Sleep 必须由真实的
    // 功能信号驱动，这里不能映射，否则放歌时脸会一直停在“思考”。
    // 服务端在“歌曲流真正开始”时补发的信号（base.py 的 STREAM 分支），
    // 开场白阶段不发，所以开场白仍按普通说话表情播。
    if (ieq(e, "music")) {
        *out = FaceExpression::Music;
        return EmotionAction::Set;
    }
    return EmotionAction::Ignore;
}

bool map_status(const char* s, FaceExpression* out) {
    if (s == nullptr || s[0] == '\0') {
        return false;
    }
    auto eq = [s](const char* v) { return std::strcmp(s, v) == 0; };

    if (eq(Lang::Strings::STANDBY) || eq(Lang::Strings::CONNECTING)) {
        *out = FaceExpression::Idle;
        return true;
    }
    if (eq(Lang::Strings::LISTENING)) {
        *out = FaceExpression::Listen;
        return true;
    }
    // 说话中不单独做表情，交给服务端情绪；没有情绪时就是自然待机。
    if (eq(Lang::Strings::SPEAKING)) {
        *out = FaceExpression::Idle;
        return true;
    }
    if (eq(Lang::Strings::INITIALIZING) || eq(Lang::Strings::LOADING_PROTOCOL)) {
        *out = FaceExpression::Boot;
        return true;
    }
    if (eq(Lang::Strings::ACTIVATION) || eq(Lang::Strings::REGISTERING_NETWORK) ||
        eq(Lang::Strings::DETECTING_MODULE) || eq(Lang::Strings::SCANNING_WIFI) ||
        eq(Lang::Strings::ENTERING_WIFI_CONFIG_MODE) || eq(Lang::Strings::WIFI_CONFIG_MODE) ||
        eq(Lang::Strings::SWITCH_TO_WIFI_NETWORK) || eq(Lang::Strings::SWITCH_TO_4G_NETWORK) ||
        eq(Lang::Strings::CONNECT_TO_HOTSPOT) || eq(Lang::Strings::CONNECT_TO) ||
        eq(Lang::Strings::ACCESS_VIA_BROWSER) || eq(Lang::Strings::PLEASE_WAIT)) {
        *out = FaceExpression::Wifi;
        return true;
    }
    if (eq(Lang::Strings::CHECKING_NEW_VERSION) || eq(Lang::Strings::UPGRADING) ||
        eq(Lang::Strings::OTA_UPGRADE) || eq(Lang::Strings::LOADING_ASSETS) ||
        eq(Lang::Strings::FOUND_NEW_ASSETS) || eq(Lang::Strings::NEW_VERSION) ||
        eq(Lang::Strings::VERSION)) {
        *out = FaceExpression::Upgrade;
        return true;
    }
    if (eq(Lang::Strings::ERROR) || eq(Lang::Strings::WARNING) ||
        eq(Lang::Strings::MODEM_INIT_ERROR) || eq(Lang::Strings::PIN_ERROR) ||
        eq(Lang::Strings::REG_ERROR) || eq(Lang::Strings::SERVER_ERROR) ||
        eq(Lang::Strings::UPGRADE_FAILED) || eq(Lang::Strings::CHECK_NEW_VERSION_FAILED) ||
        eq(Lang::Strings::DOWNLOAD_ASSETS_FAILED) ||
        eq(Lang::Strings::SERVER_NOT_CONNECTED) || eq(Lang::Strings::SERVER_NOT_FOUND) ||
        eq(Lang::Strings::SERVER_TIMEOUT) || eq(Lang::Strings::BATTERY_LOW) ||
        eq(Lang::Strings::BATTERY_NEED_CHARGE)) {
        *out = FaceExpression::Alert;
        return true;
    }
    if (eq(Lang::Strings::BATTERY_CHARGING) || eq(Lang::Strings::BATTERY_FULL)) {
        *out = FaceExpression::Idle;
        return true;
    }

    // 动态文案（带 %s / 数字）：先认特殊表情，再按错误 → 升级 → 网络兜底。
    if (std::strstr(s, "关机") != nullptr || ieq(s, "off") || ieq(s, "shutdown")) {
        *out = FaceExpression::Off;
        return true;
    }
    if (std::strstr(s, "睡觉") != nullptr || std::strstr(s, "困") != nullptr ||
        std::strstr(s, "休眠") != nullptr) {
        *out = FaceExpression::Sleep;
        return true;
    }
    if (std::strstr(s, "音乐") != nullptr || std::strstr(s, "播放") != nullptr) {
        *out = FaceExpression::Music;
        return true;
    }
    if (std::strstr(s, "思考") != nullptr) {
        *out = FaceExpression::Think;
        return true;
    }
    if (std::strstr(s, "卖萌") != nullptr || std::strstr(s, "撒娇") != nullptr ||
        std::strstr(s, "可爱") != nullptr) {
        *out = FaceExpression::Cute;
        return true;
    }
    if (std::strstr(s, "东张西望") != nullptr) {
        *out = FaceExpression::Glance;
        return true;
    }
    if (std::strstr(s, "失败") != nullptr || std::strstr(s, "错误") != nullptr ||
        std::strstr(s, "警告") != nullptr || std::strstr(s, "无法") != nullptr ||
        std::strstr(s, "不足") != nullptr) {
        *out = FaceExpression::Alert;
        return true;
    }
    if (std::strstr(s, "升级") != nullptr || std::strstr(s, "版本") != nullptr ||
        std::strstr(s, "资源") != nullptr) {
        *out = FaceExpression::Upgrade;
        return true;
    }
    if (std::strstr(s, "配网") != nullptr || std::strstr(s, "扫描") != nullptr ||
        std::strstr(s, "网络") != nullptr || std::strstr(s, "激活") != nullptr ||
        std::strstr(s, "等待") != nullptr || std::strstr(s, "热点") != nullptr) {
        *out = FaceExpression::Wifi;
        return true;
    }
    return false;
}

}  // namespace

RobotFace::~RobotFace() {
    if (timer_ != nullptr) {
        lv_timer_delete(timer_);
        timer_ = nullptr;
    }
    if (root_ != nullptr) {
        lv_obj_delete(root_);
        root_ = nullptr;
    }
    logo_sprite_.release();
    for (int side = 0; side < 2; ++side) {
        for (int style = 0; style < 2; ++style) {
            eye_sprite_[side][style].release();
        }
    }
    closed_sprite_.release();
    wrinkle_sprite_.release();
    mark_sprite_.release();
    wifi_sprite_.release();
    heart_sprite_.release();
    star_sprite_.release();
    note_sprite_[0].release();
    note_sprite_[1].release();
    drop_sad_sprite_.release();
    drop_fear_sprite_.release();
    z_sprite_.release();
    for (int side = 0; side < 2; ++side) {
        for (int style = 0; style < 2; ++style) {
            brow_sprite_[side][style].release();
        }
    }
}

void RobotFace::BakeBaseSprites() {
    // 眼睛、眉毛、皱纹、logo 全部用 v9 的精确轮廓（expression_geometry.h）。
    for (int side = 0; side < 2; ++side) {
        for (int style = 0; style < 2; ++style) {
            eye_sprite_[side][style] =
                bake_eye(style != 0, side != 0, eye_pivot_[side][style]);
            brow_sprite_[side][style] =
                bake_brow(style != 0, side != 0, brow_pivot_[side][style]);
        }
    }
    wrinkle_sprite_ = bake_wrinkles(&wrinkle_x_, &wrinkle_y_);
    logo_sprite_ = bake_logo(logo_pivot_);

    // 闭眼：灰色阴影 + 白色弧线（v9 closedEye）。
    closed_sprite_ = face::create(kClosedW, kClosedH);
    {
        float pts[2 * 40];
        int n = 0;
        const float cx = kClosedW * 0.5f;
        pts[n * 2] = cx - 56.0f;
        pts[n * 2 + 1] = 10.0f;
        ++n;
        n += face::flatten_cubic(cx - 56.0f, 10.0f, cx - 33.6f, 35.0f, cx + 33.6f, 35.0f,
                                 cx + 56.0f, 10.0f, &pts[n * 2], 32, 18);
        face::stroke_polyline(closed_sprite_, pts, n, 12.0f, 0x57585F, 255);
        face::stroke_polyline(closed_sprite_, pts, n, 7.0f, 0xFFFFFF, 255);
    }

    // 警告三角 + 感叹号（v9: (180,42)-(156,83)-(204,83)）。
    mark_sprite_ = face::create(kMarkW, kMarkH);
    {
        const float tri[6] = {32.0f, 4.0f, 4.0f, 44.0f, 60.0f, 44.0f};
        face::fill_polygon(mark_sprite_, tri, 3, 0xFF4B48, 0xFF4B48, 0.0f, 0.0f, 255);
        const float ex[4] = {32.0f, 16.0f, 32.0f, 31.0f};
        face::stroke_polyline(mark_sprite_, ex, 2, 3.5f, 0xFFFFFF, 255);
        face::fill_ellipse(mark_sprite_, 32.0f, 38.0f, 2.0f, 2.0f, 0.0f, 0xFFFFFF, 0, 0);
    }

    // 配网 Wi-Fi：三条弧 + 圆点（v9 wifiIcon）。
    wifi_sprite_ = face::create(kWifiW, kWifiH);
    {
        const float ax = kWifiW * 0.5f;
        const float ay = 46.0f;
        const float radii[3] = {35.0f, 25.0f, 14.0f};
        const float ctrl[3] = {-46.0f, -30.0f, -14.0f};
        const float width[3] = {4.0f, 3.7f, 3.4f};
        for (int i = 0; i < 3; ++i) {
            float pts[2 * 24];
            int n = 1;
            pts[0] = ax - radii[i];
            pts[1] = ay;
            n += face::flatten_cubic(ax - radii[i], ay, ax - radii[i] * 0.55f, ay + ctrl[i],
                                     ax + radii[i] * 0.55f, ay + ctrl[i], ax + radii[i], ay,
                                     &pts[n * 2], 20, 16);
            face::stroke_polyline(wifi_sprite_, pts, n, width[i], 0x00D2F5, 255);
        }
        face::fill_ellipse(wifi_sprite_, ax, ay - 1.0f, 4.0f, 4.0f, 0.0f, 0x00D2F5, 0, 0);
    }

    // 水滴（难过用的偏蓝、害怕用的偏青）。
    auto bake_drop = [](face::Sprite& sprite, uint32_t color) {
        sprite = face::create(kDropW, kDropH);
        float dp[2 * 32];
        int dn = 0;
        const float anchor = kDropW * 0.5f;
        auto push = [&dp, &dn, anchor](float x, float y) {
            if (dn < 30) {
                dp[dn * 2] = x + anchor;
                dp[dn * 2 + 1] = y + anchor;
                ++dn;
            }
        };
        push(0.0f, -11.0f);
        float tmp[2 * 12];
        int k = face::flatten_cubic(0.0f, -11.0f, 7.0f, -2.0f, 11.0f, 4.0f, 8.0f, 10.0f, tmp,
                                    12, 8);
        for (int i = 0; i < k; ++i) push(tmp[i * 2], tmp[i * 2 + 1]);
        k = face::flatten_cubic(8.0f, 10.0f, 5.0f, 17.0f, -5.0f, 17.0f, -8.0f, 10.0f, tmp, 12,
                                8);
        for (int i = 0; i < k; ++i) push(tmp[i * 2], tmp[i * 2 + 1]);
        k = face::flatten_cubic(-8.0f, 10.0f, -11.0f, 4.0f, -6.0f, -2.0f, 0.0f, -11.0f, tmp,
                                12, 8);
        for (int i = 0; i < k; ++i) push(tmp[i * 2], tmp[i * 2 + 1]);
        face::fill_polygon(sprite, dp, dn, color, color, 0.0f, 0.0f, 255);
        const float shine[4] = {anchor + 1.0f, anchor + 3.0f, anchor + 2.0f, anchor + 8.0f};
        face::stroke_polyline(sprite, shine, 2, 1.4f, 0xB4F2FF, 255);
    };
    bake_drop(drop_sad_sprite_, 0x2596F5);
    bake_drop(drop_fear_sprite_, 0x46D2FF);

    // 爱心（v9 heart，s = 18）。
    heart_sprite_ = face::create(kHeartW, kHeartH);
    {
        float hp[2 * 64];
        int hn = 0;
        const float cx = kHeartW * 0.5f;
        const float cy = kHeartH * 0.5f;
        const float s = 18.0f;
        auto push = [&hp, &hn](float x, float y) {
            if (hn < 60) {
                hp[hn * 2] = x;
                hp[hn * 2 + 1] = y;
                ++hn;
            }
        };
        float tmp[2 * 12];
        push(cx, cy + 0.4f * s);
        struct Seg { float x1, y1, x2, y2, x3, y3; };
        const Seg segs[4] = {
            {cx - 0.2f * s, cy + 0.18f * s, cx - 0.62f * s, cy - 0.07f * s, cx - 0.42f * s,
             cy - 0.36f * s},
            {cx - 0.24f * s, cy - 0.62f * s, cx - 0.03f * s, cy - 0.38f * s, cx, cy - 0.24f * s},
            {cx + 0.03f * s, cy - 0.38f * s, cx + 0.24f * s, cy - 0.62f * s, cx + 0.42f * s,
             cy - 0.36f * s},
            {cx + 0.62f * s, cy - 0.07f * s, cx + 0.2f * s, cy + 0.18f * s, cx, cy + 0.4f * s},
        };
        float px = cx, py = cy + 0.4f * s;
        for (int i = 0; i < 4; ++i) {
            const int k = face::flatten_cubic(px, py, segs[i].x1, segs[i].y1, segs[i].x2,
                                              segs[i].y2, segs[i].x3, segs[i].y3, tmp, 12, 10);
            for (int q = 0; q < k; ++q) push(tmp[q * 2], tmp[q * 2 + 1]);
            px = segs[i].x3;
            py = segs[i].y3;
        }
        face::fill_polygon(heart_sprite_, hp, hn, 0xFF709D, 0xFF709D, 0.0f, 0.0f, 255);
    }

    // 星星（v9 star，r = 9；运行时缩放到 r = 6/8）。
    star_sprite_ = face::create(kStarW, kStarH);
    {
        float sp[20];
        const float cx = kStarW * 0.5f;
        const float cy = kStarH * 0.5f;
        const float r = 9.0f;
        for (int i = 0; i < 10; ++i) {
            const float a = -kPi / 2.0f + (float)i * kPi / 5.0f;
            const float rr = (i % 2) ? r * 0.38f : r;
            sp[i * 2] = cx + std::cos(a) * rr;
            sp[i * 2 + 1] = cy + std::sin(a) * rr;
        }
        face::fill_polygon(star_sprite_, sp, 10, 0xFFCC5D, 0xFFCC5D, 0.0f, 0.0f, 255);
    }

    // 音符：单个 / 双联（v9 note）。
    for (int pair = 0; pair < 2; ++pair) {
        note_sprite_[pair] = face::create(kNoteW, kNoteH);
        const float bx = 10.0f;
        const float by = 36.0f;
        face::fill_ellipse(note_sprite_[pair], bx, by + 1.0f, 6.0f, 4.0f, 0.0f, 0xFFC350,
                           0, 0);
        const float stem[4] = {bx + 5.0f, by, bx + 5.0f, by - 25.0f};
        face::stroke_polyline(note_sprite_[pair], stem, 2, 2.7f, 0xFFC350, 255);
        if (pair) {
            face::fill_ellipse(note_sprite_[pair], bx + 22.0f, by + 6.0f, 6.0f, 4.0f, 0.0f,
                               0xFFC350, 0, 0);
            const float stem2[4] = {bx + 27.0f, by + 5.0f, bx + 27.0f, by - 22.0f};
            face::stroke_polyline(note_sprite_[pair], stem2, 2, 2.7f, 0xFFC350, 255);
            const float beam[4] = {bx + 5.0f, by - 25.0f, bx + 27.0f, by - 22.0f};
            face::stroke_polyline(note_sprite_[pair], beam, 2, 2.7f, 0xFFC350, 255);
        } else {
            float flag[2 * 12];
            int n = 1;
            flag[0] = bx + 5.0f;
            flag[1] = by - 25.0f;
            n += face::flatten_cubic(bx + 5.0f, by - 25.0f, bx + 15.0f, by - 23.0f,
                                     bx + 19.0f, by - 16.0f, bx + 14.0f, by - 12.0f, &flag[n * 2],
                                     10, 8);
            face::stroke_polyline(note_sprite_[pair], flag, n, 2.7f, 0xFFC350, 255);
        }
    }

    // 困倦的 Z（v9 用字体画的，这里烘焙成一笔折线）。
    z_sprite_ = face::create(kZW, kZH);
    {
        const float zp[8] = {5.0f, 5.0f, 23.0f, 5.0f, 5.0f, 25.0f, 23.0f, 25.0f};
        face::stroke_polyline(z_sprite_, zp, 4, 3.5f, 0x8FBCFF, 255);
    }

    if (!eye_sprite_[0][0].ok() || !eye_sprite_[1][0].ok() || !closed_sprite_.ok() ||
        !logo_sprite_.ok()) {
        ESP_LOGE(TAG, "Failed to allocate face sprites (PSRAM?)");
    }
}

void RobotFace::SetBrowSource(int index, const face::Sprite* sprite, int style) {
    if (index < 0 || index > 1 || brow_obj_[index] == nullptr) {
        return;
    }
    if (brow_src_[index] == sprite) {
        return;
    }
    brow_src_[index] = sprite;
    if (sprite != nullptr) {
        lv_image_set_src(brow_obj_[index], &sprite->dsc);
        lv_obj_set_size(brow_obj_[index], sprite->w, sprite->h);
        lv_image_set_pivot(brow_obj_[index], brow_pivot_[index][style][0],
                           brow_pivot_[index][style][1]);
    }
}

void RobotFace::Create(lv_obj_t* parent, int width, int height) {
    if (root_ != nullptr || parent == nullptr) {
        return;
    }

    BakeBaseSprites();
    if (!eye_sprite_[0][0].ok() || !eye_sprite_[1][0].ok() || !closed_sprite_.ok() ||
        !brow_sprite_[0][0].ok() || !brow_sprite_[0][1].ok() || !logo_sprite_.ok()) {
        return;
    }

    root_ = lv_obj_create(parent);
    lv_obj_set_size(root_, width, height);
    lv_obj_set_pos(root_, 0, 0);
    lv_obj_set_style_bg_color(root_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root_, 0, 0);
    lv_obj_set_style_radius(root_, 0, 0);
    lv_obj_set_style_pad_all(root_, 0, 0);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_CLICKABLE);

    // 绘制顺序与 v9 一致：logo → 顶部图标 → 眉毛 → 皱纹 → 眼睛 → 眼泪/爱心/音符等。
    logo_obj_ = create_sprite_obj(root_, logo_sprite_, logo_pivot_[0], logo_pivot_[1]);
    if (logo_obj_ != nullptr) {
        lv_obj_set_pos(logo_obj_, width / 2 - logo_pivot_[0], height / 2 - logo_pivot_[1]);
        lv_obj_add_flag(logo_obj_, LV_OBJ_FLAG_HIDDEN);
    }

    mark_obj_ = create_sprite_obj(root_, mark_sprite_, kMarkW / 2, kMarkH / 2);
    if (mark_obj_ != nullptr) {
        lv_obj_add_flag(mark_obj_, LV_OBJ_FLAG_HIDDEN);
    }

    arc_obj_ = lv_arc_create(root_);
    lv_obj_set_pos(arc_obj_, 151, 40);
    lv_obj_set_size(arc_obj_, 58, 58);
    lv_obj_remove_flag(arc_obj_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_style(arc_obj_, nullptr, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(arc_obj_, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_border_width(arc_obj_, 0, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(arc_obj_, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(arc_obj_, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(arc_obj_, 0, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc_obj_, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc_obj_, lv_color_hex(0x73A0B1), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc_obj_, 90, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(arc_obj_, true, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc_obj_, 5, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc_obj_, lv_color_hex(0x00D2F5), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc_obj_, true, LV_PART_INDICATOR);
    lv_arc_set_bg_angles(arc_obj_, 0, 360);
    lv_obj_add_flag(arc_obj_, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < 3; ++i) {
        star_obj_[i] = create_sprite_obj(root_, star_sprite_, kStarW / 2, kStarH / 2);
        if (star_obj_[i] != nullptr) {
            lv_obj_add_flag(star_obj_[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    wifi_obj_ = create_sprite_obj(root_, wifi_sprite_, kWifiW / 2, kWifiH / 2);
    if (wifi_obj_ != nullptr) {
        lv_obj_add_flag(wifi_obj_, LV_OBJ_FLAG_HIDDEN);
    }

    for (int i = 0; i < 2; ++i) {
        brow_obj_[i] = create_sprite_obj(root_, brow_sprite_[i][0], brow_pivot_[i][0][0],
                                         brow_pivot_[i][0][1]);
        if (brow_obj_[i] != nullptr) {
            brow_src_[i] = &brow_sprite_[i][0];
            lv_obj_add_flag(brow_obj_[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    wrinkle_obj_ = create_sprite_obj(root_, wrinkle_sprite_, 0, 0);
    if (wrinkle_obj_ != nullptr) {
        lv_obj_set_pos(wrinkle_obj_, wrinkle_x_, wrinkle_y_);
        lv_obj_add_flag(wrinkle_obj_, LV_OBJ_FLAG_HIDDEN);
    }

    for (int i = 0; i < 2; ++i) {
        for (int k = 0; k < 2; ++k) {
            eye_obj_[i][k] = create_sprite_obj(root_, eye_sprite_[i][k], eye_pivot_[i][k][0],
                                               eye_pivot_[i][k][1]);
            if (eye_obj_[i][k] != nullptr) {
                lv_obj_add_flag(eye_obj_[i][k], LV_OBJ_FLAG_HIDDEN);
            }
        }
        closed_obj_[i] = create_sprite_obj(root_, closed_sprite_, kClosedW / 2, 10);
        if (closed_obj_[i] != nullptr) {
            lv_obj_add_flag(closed_obj_[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    for (int i = 0; i < 2; ++i) {
        heart_obj_[i] = create_sprite_obj(root_, heart_sprite_, kHeartW / 2, kHeartH / 2);
        if (heart_obj_[i] != nullptr) {
            lv_obj_add_flag(heart_obj_[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    for (int i = 0; i < 2; ++i) {
        const face::Sprite& sprite = (i == 0) ? drop_sad_sprite_ : drop_fear_sprite_;
        drop_obj_[i] = create_sprite_obj(root_, sprite, kDropW / 2, kDropH / 2);
        if (drop_obj_[i] != nullptr) {
            lv_obj_add_flag(drop_obj_[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    for (int i = 0; i < 2; ++i) {
        note_obj_[i] = create_sprite_obj(root_, note_sprite_[i], 10, 37);
        if (note_obj_[i] != nullptr) {
            lv_obj_add_flag(note_obj_[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    for (int i = 0; i < 3; ++i) {
        lv_obj_t* dot = lv_obj_create(root_);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(0xFFCD65), 0);
        lv_obj_set_style_bg_opa(dot, 220, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_set_style_pad_all(dot, 0, 0);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);
        think_obj_[i] = dot;
    }

    // 聆听声波是静态曲线（v9 只改显隐），在这里一次性算好，动画帧里不再重算。
    for (int i = 0; i < 4; ++i) {
        wave_obj_[i] = lv_line_create(root_);
        lv_obj_set_style_line_color(wave_obj_[i], lv_color_hex(0x00CEF5), 0);
        lv_obj_set_style_line_width(wave_obj_[i], 3, 0);
        lv_obj_set_style_line_rounded(wave_obj_[i], true, 0);
        lv_obj_set_style_line_opa(wave_obj_[i], 210, 0);

        const int side_i = i / 2;
        const int j = i % 2;
        const float side = (side_i == 0) ? -1.0f : 1.0f;
        const float x = 180.0f + side * (159.0f + (float)j * 9.0f);
        const float y0 = 222.0f;
        const float y1 = 254.0f;
        wave_points_[i][0].x = (lv_value_precise_t)lroundf(x);
        wave_points_[i][0].y = (lv_value_precise_t)lroundf(y0);
        float tmp[2 * 12];
        const int k = face::flatten_cubic(x, y0, x + side * 8.0f, 238.0f - 7.0f,
                                          x + side * 8.0f, 238.0f + 7.0f, x, y1, tmp, 12, 9);
        for (int q = 0; q < k && q + 1 < 10; ++q) {
            wave_points_[i][q + 1].x = (lv_value_precise_t)lroundf(tmp[q * 2]);
            wave_points_[i][q + 1].y = (lv_value_precise_t)lroundf(tmp[q * 2 + 1]);
        }
        place_line(wave_obj_[i], wave_points_[i], (uint32_t)(k + 1));
        lv_obj_add_flag(wave_obj_[i], LV_OBJ_FLAG_HIDDEN);
    }

    z_obj_ = create_sprite_obj(root_, z_sprite_, kZW / 2, kZH / 2);
    if (z_obj_ != nullptr) {
        lv_obj_set_style_image_opa(z_obj_, 220, 0);
        lv_obj_add_flag(z_obj_, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_move_foreground(root_);
    state_start_ms_ = lv_tick_get();
    last_active_ms_ = state_start_ms_;
    next_glance_ms_ = 0;
    current_ = base_;

    timer_ = lv_timer_create(
        [](lv_timer_t* timer) { static_cast<RobotFace*>(lv_timer_get_user_data(timer))->Tick(); },
        kFrameMs, this);
    Tick();
}

void RobotFace::SetBaseExpression(FaceExpression expression) {
    if (expression >= FaceExpression::Count) {
        return;
    }
    last_active_ms_ = lv_tick_get();
    // 开机动画只放一次：INITIALIZING 之后隔十几秒才来的 LOADING_PROTOCOL
    // 也会映射成 Boot，不拦下来会完整重播一遍 5 s 动画。
    if (expression == FaceExpression::Boot && boot_done_) {
        expression = FaceExpression::Idle;
    }
    // 开机动画 5 s：中途到来的状态先记下来，等动画结束再切，别把睁眼截断。
    if (current_ == FaceExpression::Boot && base_ == FaceExpression::Boot &&
        expression != FaceExpression::Boot && expression != FaceExpression::Off &&
        expression != FaceExpression::Alert) {
        pending_base_ = expression;
        return;
    }
    if (expression == base_) {
        return;
    }
    base_ = expression;
    pending_base_ = FaceExpression::Count;
    RefreshActive();
}

void RobotFace::SetEmotion(const char* emotion) {
    FaceExpression mapped = FaceExpression::Idle;
    const EmotionAction action = map_emotion(emotion, &mapped);
    if (action == EmotionAction::Ignore) {
        return;
    }
    last_active_ms_ = lv_tick_get();
    if (action == EmotionAction::Clear) {
        if (!has_emotion_) {
            return;
        }
        has_emotion_ = false;
    } else if (mapped == FaceExpression::Music) {
        // 服务端的 music 是“歌曲流真正开始”的功能信号，不是情绪：
        // 放进功能槽，优先级高于思考/情绪，放歌期间不会被别的信号顶掉。
        has_emotion_ = false;
        SetFunctionExpression(FaceExpression::Music);
        return;
    } else {
        has_emotion_ = true;
        emotion_ = mapped;
    }
    RefreshActive();
}

void RobotFace::SetStatus(const char* status) {
    last_active_ms_ = lv_tick_get();
    FaceExpression mapped = FaceExpression::Idle;
    if (map_status(status, &mapped)) {
        SetBaseExpression(mapped);
    }
}

void RobotFace::SetFunctionExpression(FaceExpression expression) {
    last_active_ms_ = lv_tick_get();
    if (expression >= FaceExpression::Count) {
        expression = FaceExpression::Count;  // 清除
    }
    if (function_ == expression) {
        return;  // 同一功能表情重复上报（每句字幕都会走这里）：不重启动画
    }
    function_ = expression;
    RefreshActive();
}

void RobotFace::SetPowerSave(bool on) {
    if (on) {
        if (!power_save_) {
            power_save_ = true;
            base_before_power_save_ = base_;
        }
        SetBaseExpression(FaceExpression::Sleep);
    } else if (power_save_) {
        power_save_ = false;
        if (current_ == FaceExpression::Boot) {
            // 开机动画还没放完：丢掉之前记下的 Sleep，让动画自然结束。
            pending_base_ = FaceExpression::Count;
            base_ = FaceExpression::Boot;
        } else {
            // 开机动画不是“可恢复”的状态，唤醒后回自然待机。
            SetBaseExpression(base_before_power_save_ == FaceExpression::Boot
                                  ? FaceExpression::Idle
                                  : base_before_power_save_);
        }
    }
}

void RobotFace::SetVisible(bool visible) {
    visible_ = visible;
    if (root_ == nullptr) {
        return;
    }
    if (visible) {
        lv_obj_remove_flag(root_, LV_OBJ_FLAG_HIDDEN);
        Tick();
    } else {
        lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
    }
}

void RobotFace::EnterExpression(FaceExpression expression) {
    // 已经在放开机动画时再次请求 Boot：保持时间轴，绝不能从头再放一遍。
    if (expression == FaceExpression::Boot && current_ == FaceExpression::Boot) {
        return;
    }
    current_ = expression;
    state_start_ms_ = lv_tick_get();
    next_glance_ms_ = state_start_ms_ + 18000;
}

void RobotFace::RefreshActive() {
    FaceExpression active = base_;
    // 功能表情（放歌/等待回答）压过服务端情绪：放歌时服务端往往给 neutral，
    // 甚至给 happy，但用户要看的是 Music。
    const bool neutral_base = (base_ == FaceExpression::Idle ||
                               base_ == FaceExpression::Listen ||
                               base_ == FaceExpression::Glance ||
                               base_ == FaceExpression::Sleep);
    if (function_ != FaceExpression::Count && neutral_base) {
        active = function_;
    } else if (has_emotion_ && base_ == FaceExpression::Idle) {
        // 情绪只覆盖“待机/说话”这种中性状态；配网、升级、错误、聆听保持功能表情。
        active = emotion_;
    }
    if (active != current_) {
        EnterExpression(active);
    }
    if (root_ != nullptr && visible_) {
        Tick();
    }
}

void RobotFace::ApplyFaceAlpha(uint8_t alpha) {
    if (alpha == face_alpha_) {
        return;
    }
    face_alpha_ = alpha;
    for (int i = 0; i < 2; ++i) {
        if (brow_obj_[i] != nullptr) {
            lv_obj_set_style_image_opa(brow_obj_[i], (lv_opa_t)alpha, 0);
        }
        for (int k = 0; k < 2; ++k) {
            if (eye_obj_[i][k] != nullptr) {
                lv_obj_set_style_image_opa(eye_obj_[i][k], (lv_opa_t)alpha, 0);
            }
        }
    }
}

void RobotFace::ApplyLogo(uint8_t alpha, uint16_t zoom) {
    if (logo_obj_ == nullptr) {
        return;
    }
    if (alpha != logo_alpha_) {
        lv_obj_set_style_image_opa(logo_obj_, (lv_opa_t)alpha, 0);
        logo_alpha_ = alpha;
    }
    if (zoom != logo_zoom_) {
        lv_image_set_scale(logo_obj_, zoom);
        logo_zoom_ = zoom;
    }
    show_obj(logo_obj_, alpha > 0);
}

void RobotFace::Tick() {
    if (root_ == nullptr || !visible_) {
        return;
    }

    const uint32_t now = lv_tick_get();
    float elapsed = (float)(now - state_start_ms_) * 0.001f;

    // 开机动画结束：切到期间收到的状态，没有就自然待机。
    if (current_ == FaceExpression::Boot && elapsed >= 5.0f) {
        boot_done_ = true;
        FaceExpression next = (pending_base_ != FaceExpression::Count)
                                  ? pending_base_
                                  : FaceExpression::Idle;
        pending_base_ = FaceExpression::Count;
        base_ = next;
        EnterExpression(next);
        elapsed = 0.0f;
    }

    // 长时间（3 分钟）没有状态/情绪/功能表情变化就困了，闭眼睡觉；
    // 下一次 SetStatus（唤醒、按键、联网）会自然把 base_ 从 Sleep 换走。
    if (!power_save_ && base_ == FaceExpression::Idle &&
        function_ == FaceExpression::Count && !has_emotion_ &&
        current_ != FaceExpression::Sleep &&
        (uint32_t)(now - last_active_ms_) >= 180000u) {
        base_ = FaceExpression::Sleep;
        EnterExpression(FaceExpression::Sleep);
        elapsed = 0.0f;
    }

    // 待机一段时间后自动东张西望，回来后隔 18 s 再来一次。
    if (current_ == FaceExpression::Idle && base_ == FaceExpression::Idle && !has_emotion_ &&
        (int32_t)(now - next_glance_ms_) >= 0) {
        EnterExpression(FaceExpression::Glance);
        elapsed = 0.0f;
    }
    if (current_ == FaceExpression::Glance && elapsed >= 6.0f) {
        EnterExpression(FaceExpression::Idle);
        elapsed = 0.0f;
    }

    const float t = (float)now * 0.001f;
    const bool boot = (current_ == FaceExpression::Boot);
    const bool off = (current_ == FaceExpression::Off);
    const bool opening = boot || off;

    // ---- logo 与脸的交叉淡入淡出 ----
    uint8_t logo_alpha = 0;
    float logo_scale = 1.0f;
    float face_alpha = 1.0f;
    if (boot) {
        logo_alpha = (uint8_t)lroundf(255.0f * (1.0f - smoothstep(2.1f, 2.8f, elapsed)));
        logo_scale = 0.72f + 0.25f * smoothstep(0.0f, 2.3f, elapsed);
        face_alpha = smoothstep(2.1f, 2.8f, elapsed);
    } else if (off) {
        logo_alpha = (uint8_t)lroundf(255.0f * smoothstep(1.85f, 2.6f, elapsed));
        logo_scale = 0.88f + 0.12f * smoothstep(1.85f, 3.5f, elapsed);
        face_alpha = 1.0f - smoothstep(1.85f, 2.6f, elapsed);
    }
    const uint8_t face_opa = (uint8_t)lroundf(255.0f * face_alpha);
    ApplyFaceAlpha(face_opa);
    ApplyLogo(logo_alpha, (uint16_t)image_zoom(logo_scale));

    // ---- 姿态 ----
    Pose p;
    float open = 1.0f;
    if (boot) {
        open = smoothstep(2.9f, 4.5f, elapsed);
    } else if (off) {
        open = 1.0f - smoothstep(0.0f, 1.8f, elapsed);
    } else {
        switch (current_) {
            case FaceExpression::Idle:
                p.eyeShiftX = gaze(t);
                break;
            case FaceExpression::Angry:
                p.angryEyes = p.angryBrows = true;
                break;
            case FaceExpression::Wifi:
                p.browShiftY = -3.0f;
                p.eyeShiftY = 1.5f * std::sin(t * 1.4f);
                break;
            case FaceExpression::Upgrade:
                p.browShiftY = -1.0f;
                p.eyeShiftY = 1.2f * std::sin(t * 1.4f);
                break;
            case FaceExpression::AiLoad:
                p.leftBrowLift = -2.0f;
                p.rightBrowLift = 2.0f;
                p.eyeShiftY = -1.5f;
                break;
            case FaceExpression::Alert:
                p.angryEyes = p.angryBrows = true;
                p.browShiftY = -3.0f;
                break;
            case FaceExpression::Happy: {
                const float breath = std::sin(t * 1.4f);
                p.browShiftY = -4.0f - 2.0f * breath;
                p.eyeShiftY = -2.0f * breath;
                p.leftEyeScale = p.rightEyeScale = 0.96f;
                break;
            }
            case FaceExpression::Sad:
                p.browShiftY = 4.0f;
                p.leftBrowTurn = -12.0f;
                p.rightBrowTurn = 12.0f;
                p.eyeShiftY = 3.0f;
                break;
            case FaceExpression::Fear:
                p.browShiftY = -9.0f;
                p.eyeScale = 1.04f;
                p.eyeShiftX = -3.0f + 2.0f * std::sin(t * 1.4f);
                break;
            case FaceExpression::Disgust:
                p.angryEyes = true;
                p.leftBrowLift = -5.0f;
                p.rightBrowLift = 5.0f;
                p.leftBrowTurn = -9.0f;
                p.rightBrowTurn = 9.0f;
                break;
            case FaceExpression::Surprise:
                p.browShiftY = -12.0f;
                p.eyeScale = 1.06f;
                break;
            case FaceExpression::Glance:
                p.eyeShiftX = gaze(t);
                p.browShiftY = 0.8f * std::sin(t * 1.4f);
                break;
            case FaceExpression::Sleep:
                p.closed = true;
                p.browShiftY = 6.0f + 1.5f * std::sin(t * 1.4f);
                p.leftBrowTurn = 3.0f;
                p.rightBrowTurn = -3.0f;
                break;
            case FaceExpression::Cute:
                p.eyeScale = 1.025f;
                p.browShiftY = -3.0f;
                p.eyeShiftY = -1.5f * std::sin(t * 1.4f);
                break;
            case FaceExpression::Think:
                p.eyeShiftX = 3.0f;
                p.eyeShiftY = -2.0f;
                p.leftBrowLift = -8.0f;
                p.rightBrowLift = 2.0f;
                p.leftBrowTurn = -8.0f;
                p.rightBrowTurn = 5.0f;
                p.leftEyeScale = 0.94f;
                break;
            case FaceExpression::Listen:
                p.eyeShiftX = 2.0f;
                p.eyeShiftY = -1.0f;
                p.browShiftY = -4.0f;
                p.leftBrowTurn = -4.0f;
                p.rightBrowTurn = 4.0f;
                break;
            case FaceExpression::Music: {
                const float bounce = std::sin(t * 2.0f * kPi * 1.8f);
                p.browShiftY = -2.0f - 2.0f * std::fmax(0.0f, bounce);
                p.eyeShiftY = 2.8f * bounce;
                p.eyeShiftX = 2.0f * std::sin(t * 2.2f);
                break;
            }
            default:
                break;
        }
    }

    // ---- 眉毛 ----
    const int brow_style = p.angryBrows ? 1 : 0;
    const float brow_lx = p.angryBrows ? kAngryBrowLX : kIdleBrowLX;
    const float brow_rx = p.angryBrows ? kAngryBrowRX : kIdleBrowRX;
    const float brow_ly = p.angryBrows ? kAngryBrowLY : kIdleBrowY;
    const float brow_ry = p.angryBrows ? kAngryBrowRY : kIdleBrowY;
    for (int i = 0; i < 2; ++i) {
        const bool right = (i == 1);
        const face::Sprite& sprite = brow_sprite_[i][brow_style];
        if (!sprite.ok() || brow_obj_[i] == nullptr) {
            show_obj(brow_obj_[i], false);
            continue;
        }
        show_obj(brow_obj_[i], true);
        SetBrowSource(i, &sprite, brow_style);
        const float cx = right ? brow_rx : brow_lx;
        const float cy = (right ? brow_ry : brow_ly) + p.browShiftY +
                         (right ? p.rightBrowLift : p.leftBrowLift);
        const float turn = right ? p.rightBrowTurn : p.leftBrowTurn;
        lv_obj_set_pos(brow_obj_[i], (lv_coord_t)lroundf(cx - brow_pivot_[i][brow_style][0]),
                       (lv_coord_t)lroundf(cy - brow_pivot_[i][brow_style][1]));
        lv_image_set_rotation(brow_obj_[i], (int32_t)lroundf(turn * 10.0f));
    }

    // ---- 眼睛 / 闭眼 ----
    const bool angry_eyes = p.angryEyes;
    const int eye_style = angry_eyes ? 1 : 0;
    const float eye_lx = angry_eyes ? kAngryEyeLX : kIdleEyeLX;
    const float eye_rx = angry_eyes ? kAngryEyeRX : kIdleEyeRX;
    const float eye_y = angry_eyes ? kAngryEyeY : kIdleEyeY;
    const float blink_l = blink_value(t, 0.0f);
    const float blink_r = blink_value(t, 0.035f);

    auto set_zoom = [](lv_obj_t* obj, uint32_t* cache_x, uint32_t* cache_y, uint32_t zx,
                       uint32_t zy) {
        if (obj == nullptr) {
            return;
        }
        if (*cache_x != zx) {
            *cache_x = zx;
            lv_image_set_scale_x(obj, zx);
        }
        if (*cache_y != zy) {
            *cache_y = zy;
            lv_image_set_scale_y(obj, zy);
        }
    };

    for (int i = 0; i < 2; ++i) {
        const bool right = (i == 1);
        const float cx = (right ? eye_rx : eye_lx) + p.eyeShiftX;
        const float cy = eye_y + p.eyeShiftY;

        const float sx = p.eyeScale;
        float sy;
        if (opening) {
            sy = 0.04f + 0.96f * smoothstep(0.15f, 1.0f, open);
        } else {
            const float b = right ? blink_r : blink_l;
            const float per_eye = right ? p.rightEyeScale : p.leftEyeScale;
            sy = p.eyeScale * per_eye * (1.0f - 0.9f * b);
        }

        const bool draw_eyes = !p.closed && !(opening && open <= 0.15f) && sx > 0.0f;
        lv_obj_t* eye = eye_obj_[i][eye_style];
        // 两种眼型各有一张精灵；切换表情时只换显隐，不用改 src/尺寸/轴心。
        show_obj(eye_obj_[i][1 - eye_style], false);
        show_obj(eye, draw_eyes);
        if (draw_eyes && eye != nullptr) {
            const uint16_t* pv = eye_pivot_[i][eye_style];
            lv_obj_set_pos(eye, (lv_coord_t)lroundf(cx - pv[0]),
                           (lv_coord_t)lroundf(cy - pv[1]));
            // 精灵已经按 v9 的 0.8 缩放烘焙，运行时 (sx, sy) 即 v9 的 part() 参数。
            set_zoom(eye, &eye_zoom_[i][eye_style][0], &eye_zoom_[i][eye_style][1],
                     image_zoom(sx), image_zoom(sy));
        }

        // 闭眼弧线：睡觉时一直显示；开机/关机时在眼睛快合上时淡入。
        float lid = 0.0f;
        if (p.closed) {
            lid = 1.0f;
        } else if (opening) {
            lid = 1.0f - smoothstep(0.0f, 0.22f, open);
        }
        if (lid > 0.01f && closed_obj_[i] != nullptr) {
            const float lx = (i == 0) ? 89.0f : 271.0f;
            const float ly = 237.0f + p.eyeShiftY;
            lv_obj_set_pos(closed_obj_[i], (lv_coord_t)lroundf(lx - kClosedW * 0.5f),
                           (lv_coord_t)lroundf(ly - 10.0f));
            lv_obj_set_style_image_opa(closed_obj_[i],
                                       (lv_opa_t)lroundf((float)face_opa * lid), 0);
            show_obj(closed_obj_[i], true);
        } else {
            show_obj(closed_obj_[i], false);
        }
    }

    // 鼻梁上的两撇皱纹只在静态生气脸里出现；v9 在眨眼时走 faceParts，不画它们。
    show_obj(wrinkle_obj_, current_ == FaceExpression::Angry &&
                               std::fmax(blink_l, blink_r) < 0.001f);

    // ---- 顶部图标：配网 / 升级 / AI 等待 / 警告 ----
    const bool show_wifi = (current_ == FaceExpression::Wifi);
    if (wifi_obj_ != nullptr) {
        if (show_wifi) {
            lv_obj_set_pos(wifi_obj_, 180 - kWifiW / 2, 30);
            show_obj(wifi_obj_, true);
        } else {
            show_obj(wifi_obj_, false);
        }
    }

    const bool show_spinner = (current_ == FaceExpression::Upgrade ||
                               current_ == FaceExpression::AiLoad);
    if (arc_obj_ != nullptr) {
        if (show_spinner) {
            const int a = (int)std::fmod(t * 145.0f, 360.0f);
            lv_arc_set_angles(arc_obj_, a, (a + 110) % 360);
            show_obj(arc_obj_, true);
        } else {
            show_obj(arc_obj_, false);
        }
    }
    if (star_obj_[0] != nullptr) {
        if (current_ == FaceExpression::AiLoad) {
            lv_obj_set_pos(star_obj_[0], 180 - kStarW / 2, 69 - kStarH / 2);
            lv_image_set_scale(star_obj_[0], image_zoom(6.0f / 9.0f));
            lv_image_set_rotation(star_obj_[0], (int32_t)std::fmod(t * 572.9578f, 3600.0f));
            show_obj(star_obj_[0], true);
        } else {
            show_obj(star_obj_[0], false);
        }
    }
    if (mark_obj_ != nullptr) {
        if (current_ == FaceExpression::Alert) {
            lv_obj_set_pos(mark_obj_, 180 - kMarkW / 2, 38);
            show_obj(mark_obj_, true);
        } else {
            show_obj(mark_obj_, false);
        }
    }

    // ---- 装饰：爱心 / 星星 / 眼泪 / 音符 / Z / 思考点 / 声波 ----
    for (int i = 0; i < 2; ++i) {
        show_obj(heart_obj_[i], false);
        show_obj(star_obj_[i + 1], false);
        show_obj(drop_obj_[i], false);
        show_obj(note_obj_[i], false);
    }
    for (int i = 0; i < 3; ++i) {
        show_obj(think_obj_[i], false);
    }
    for (int i = 0; i < 4; ++i) {
        show_obj(wave_obj_[i], false);
    }
    show_obj(z_obj_, false);

    auto place_star = [](lv_obj_t* obj, float x, float y, float scale, uint8_t alpha) {
        if (obj == nullptr) {
            return;
        }
        lv_obj_set_pos(obj, (lv_coord_t)lroundf(x - kStarW * 0.5f),
                       (lv_coord_t)lroundf(y - kStarH * 0.5f));
        lv_image_set_scale(obj, image_zoom(scale));
        lv_obj_set_style_image_opa(obj, (lv_opa_t)alpha, 0);
        show_obj(obj, true);
    };
    auto place_heart = [](lv_obj_t* obj, float x, float y, float scale, uint8_t alpha) {
        if (obj == nullptr) {
            return;
        }
        lv_obj_set_pos(obj, (lv_coord_t)lroundf(x - kHeartW * 0.5f),
                       (lv_coord_t)lroundf(y - kHeartH * 0.5f));
        lv_image_set_scale(obj, image_zoom(scale));
        lv_obj_set_style_image_opa(obj, (lv_opa_t)alpha, 0);
        show_obj(obj, true);
    };
    auto place_drop = [](lv_obj_t* obj, float x, float y, float scale, uint8_t alpha) {
        if (obj == nullptr) {
            return;
        }
        lv_obj_set_pos(obj, (lv_coord_t)lroundf(x - kDropW * 0.5f),
                       (lv_coord_t)lroundf(y - kDropH * 0.5f));
        lv_image_set_scale(obj, image_zoom(scale));
        lv_obj_set_style_image_opa(obj, (lv_opa_t)alpha, 0);
        show_obj(obj, true);
    };

    switch (current_) {
        case FaceExpression::Happy: {
            const float bob = 3.0f * std::sin(t * 1.8f);
            place_heart(heart_obj_[0], 37.0f, 198.0f - bob, 1.0f, 220);
            place_heart(heart_obj_[1], 323.0f, 198.0f + bob, 1.0f, 220);
            place_star(star_obj_[1], 34.0f, 143.0f, 8.0f / 9.0f, 255);
            place_star(star_obj_[2], 326.0f, 143.0f, 8.0f / 9.0f, 255);
            break;
        }
        case FaceExpression::Sad: {
            place_drop(drop_obj_[0], 30.0f, 273.0f + std::fmod(t * 25.0f, 42.0f), 0.75f,
                       230);
            place_drop(drop_obj_[1], 330.0f,
                       278.0f + std::fmod(t * 25.0f + 20.0f, 42.0f), 0.65f, 215);
            break;
        }
        case FaceExpression::Fear: {
            place_drop(drop_obj_[0], 25.0f, 187.0f + std::fmod(t * 22.0f, 42.0f), 0.70f,
                       230);
            place_drop(drop_obj_[1], 335.0f,
                       191.0f + std::fmod(t * 22.0f + 22.0f, 42.0f), 0.60f, 215);
            break;
        }
        case FaceExpression::Surprise:
            place_star(star_obj_[1], 27.0f, 172.0f, 8.0f / 9.0f, 255);
            place_star(star_obj_[2], 333.0f, 172.0f, 8.0f / 9.0f, 255);
            break;
        case FaceExpression::Sleep:
            if (z_obj_ != nullptr) {
                const float zy = 185.0f + 2.0f * std::sin(t);
                lv_obj_set_pos(z_obj_, (lv_coord_t)lroundf(312.0f - kZW * 0.5f),
                               (lv_coord_t)lroundf(zy - kZH * 0.5f));
                show_obj(z_obj_, true);
            }
            break;
        case FaceExpression::Cute:
            place_heart(heart_obj_[0], 29.0f, 193.0f, 17.0f / 18.0f, 190);
            place_heart(heart_obj_[1], 331.0f, 193.0f, 17.0f / 18.0f, 190);
            break;
        case FaceExpression::Think: {
            const float d = 2.0f * std::sin(t);
            const float px[3] = {283.0f, 298.0f, 318.0f};
            const float py[3] = {155.0f, 137.0f, 114.0f};
            const int size[3] = {8, 12, 17};
            for (int i = 0; i < 3; ++i) {
                if (think_obj_[i] == nullptr) {
                    continue;
                }
                lv_obj_set_size(think_obj_[i], size[i], size[i]);
                lv_obj_set_pos(think_obj_[i], (lv_coord_t)lroundf(px[i]),
                               (lv_coord_t)lroundf(py[i] + d));
                show_obj(think_obj_[i], true);
            }
            break;
        }
        case FaceExpression::Listen:
            for (int i = 0; i < 4; ++i) {
                show_obj(wave_obj_[i], true);
            }
            break;
        case FaceExpression::Music: {
            const float a = std::fmod(t * 22.0f, 55.0f);
            const float b = std::fmod(t * 22.0f + 27.0f, 55.0f);
            const float alpha_a = 220.0f * (1.0f - std::fabs(a - 27.5f) / 27.5f);
            const float alpha_b = 220.0f * (1.0f - std::fabs(b - 27.5f) / 27.5f);
            if (note_obj_[0] != nullptr) {
                lv_obj_set_pos(note_obj_[0], (lv_coord_t)lroundf(35.0f - 10.0f),
                               (lv_coord_t)lroundf(112.0f - a * 0.55f - 37.0f));
                lv_image_set_scale(note_obj_[0], image_zoom(0.88f));
                lv_obj_set_style_image_opa(note_obj_[0],
                                           (lv_opa_t)lroundf(std::fmax(0.0f, alpha_a)), 0);
                show_obj(note_obj_[0], true);
            }
            if (note_obj_[1] != nullptr) {
                lv_obj_set_pos(note_obj_[1], (lv_coord_t)lroundf(301.0f - 10.0f),
                               (lv_coord_t)lroundf(114.0f - b * 0.55f - 37.0f));
                lv_image_set_scale(note_obj_[1], image_zoom(0.82f));
                lv_obj_set_style_image_opa(note_obj_[1],
                                           (lv_opa_t)lroundf(std::fmax(0.0f, alpha_b)), 0);
                show_obj(note_obj_[1], true);
            }
            break;
        }
        default:
            break;
    }
}
