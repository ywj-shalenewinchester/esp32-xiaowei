// 全屏机器人脸（S3 低配版，360x360 圆屏）。
//
// 移植自 ui/robot_expression_win_source/robot_expression_studio_v9.cpp：
// 19 种状态/表情、同样的姿态参数与动画曲线，用 LVGL 图片变换重写。
// 主界面只显示这张脸，所有文字控件由 WzkjLcdDisplay 隐藏。
//
// v9 的几何形状来自 expression_geometry.h（Q2 定点贝塞尔轮廓），启动时按
// v9 的 part() 变换（-矩形中心、×0.8）一次性烘焙成精灵，运行时只做位移/
// 旋转/缩放，几何与 Windows 版一致。

#pragma once

#include <lvgl.h>

#include <cstdint>

#include "face_sprite.h"

enum class FaceExpression : uint8_t {
    Boot = 0,     // 开机 / 苏醒
    Off,          // 关机
    Wifi,         // 配网 / 连接网络
    Upgrade,      // 升级
    AiLoad,       // AI 生成 / 绘画等待
    Alert,        // 错误 / 警告
    Happy,        // 开心
    Angry,        // 愤怒
    Sad,          // 悲伤
    Fear,         // 恐惧
    Disgust,      // 厌恶
    Surprise,     // 惊讶
    Idle,         // 自然待机
    Glance,       // 东张西望
    Sleep,        // 困倦 / 睡觉
    Cute,         // 卖萌
    Think,        // 思考
    Listen,       // 聆听
    Music,        // 听音乐
    Count,
};

class RobotFace {
public:
    RobotFace() = default;
    ~RobotFace();

    RobotFace(const RobotFace&) = delete;
    RobotFace& operator=(const RobotFace&) = delete;

    // 在 LVGL 锁内调用。parent 一般是 lv_screen_active()。
    void Create(lv_obj_t* parent, int width, int height);

    // 设备状态（SetStatus 的文字）驱动的基础表情。
    void SetBaseExpression(FaceExpression expression);
    void SetEmotion(const char* emotion);
    void SetStatus(const char* status);
    // 功能表情：放歌（Music）、等待回答（Think）这类没有状态文字可依据的
    // 临时表情。优先于服务端情绪；传 Count 清除，状态一变就清。
    void SetFunctionExpression(FaceExpression expression);
    void SetPowerSave(bool on);
    void SetVisible(bool visible);

    bool ready() const { return root_ != nullptr; }
    bool visible() const { return visible_; }
    FaceExpression current() const { return current_; }
    FaceExpression function() const { return function_; }

    // 由 33 ms 的 lv_timer 调用（回调本来就在 LVGL 锁内）。
    void Tick();

private:
    // v9 的 pose 表：所有状态都是在这几个参数上做位移/缩放/旋转。
    struct Pose {
        float eyeShiftX = 0.0f;
        float eyeShiftY = 0.0f;
        float eyeScale = 1.0f;
        float leftEyeScale = 1.0f;
        float rightEyeScale = 1.0f;
        float browShiftY = 0.0f;
        float leftBrowLift = 0.0f;
        float rightBrowLift = 0.0f;
        float leftBrowTurn = 0.0f;
        float rightBrowTurn = 0.0f;
        bool angryEyes = false;
        bool angryBrows = false;
        bool closed = false;
    };

    lv_obj_t* root_ = nullptr;
    lv_obj_t* logo_obj_ = nullptr;
    lv_obj_t* mark_obj_ = nullptr;   // 错误 / 警告三角
    lv_obj_t* arc_obj_ = nullptr;    // 升级 / AI 生成的转圈
    lv_obj_t* wifi_obj_ = nullptr;   // 配网 Wi-Fi 图标
    lv_obj_t* star_obj_[3] = {};     // 0: 转圈中心；1/2: 开心/惊讶的星星
    lv_obj_t* heart_obj_[2] = {};
    lv_obj_t* brow_obj_[2] = {};
    lv_obj_t* eye_obj_[2][2] = {};   // [左右][0 普通 / 1 生气]
    lv_obj_t* closed_obj_[2] = {};
    lv_obj_t* wrinkle_obj_ = nullptr;  // 生气时鼻梁上的两撇
    lv_obj_t* drop_obj_[2] = {};
    lv_obj_t* note_obj_[2] = {};
    lv_obj_t* think_obj_[3] = {};
    lv_obj_t* wave_obj_[4] = {};
    lv_obj_t* z_obj_ = nullptr;
    lv_timer_t* timer_ = nullptr;

    face::Sprite logo_sprite_;
    face::Sprite eye_sprite_[2][2];   // [左右][0 普通 / 1 生气]，0.8 烘焙
    face::Sprite closed_sprite_;
    face::Sprite wrinkle_sprite_;
    face::Sprite mark_sprite_;
    face::Sprite wifi_sprite_;
    face::Sprite heart_sprite_;
    face::Sprite star_sprite_;
    face::Sprite note_sprite_[2];
    face::Sprite drop_sad_sprite_;
    face::Sprite drop_fear_sprite_;
    face::Sprite z_sprite_;
    face::Sprite brow_sprite_[2][2];  // [左右][0 普通 / 1 生气]
    const face::Sprite* brow_src_[2] = {};

    bool visible_ = true;
    FaceExpression base_ = FaceExpression::Boot;
    FaceExpression emotion_ = FaceExpression::Idle;
    FaceExpression function_ = FaceExpression::Count;
    bool has_emotion_ = false;
    // 开机动画只放一次：初始化流程里 INITIALIZING → LOADING_PROTOCOL 可能
    // 隔了十几秒，动画早早结束，再收到 Boot 不能再重播。
    bool boot_done_ = false;
    // 最后一次状态/情绪/交互的时间，用于长时间待机自动进入 Sleep。
    uint32_t last_active_ms_ = 0;
    bool power_save_ = false;
    FaceExpression base_before_power_save_ = FaceExpression::Idle;
    FaceExpression current_ = FaceExpression::Boot;
    FaceExpression pending_base_ = FaceExpression::Count;
    uint32_t state_start_ms_ = 0;
    uint32_t next_glance_ms_ = 0;

    uint8_t face_alpha_ = 255;
    uint8_t logo_alpha_ = 0;
    uint16_t logo_zoom_ = 0;

    lv_point_precise_t wave_points_[4][10] = {};

    // 精灵内部的旋转/缩放轴：v9 的 part() 是绕眼睛/眉毛矩形的中心变换的，
    // 这里记录该中心在精灵里的像素位置。
    uint16_t eye_pivot_[2][2][2] = {};   // [左右][样式][x/y]
    uint16_t brow_pivot_[2][2][2] = {};  // [左右][样式][x/y]
    uint16_t logo_pivot_[2] = {};
    int wrinkle_x_ = 0;
    int wrinkle_y_ = 0;

    // LVGL 的 lv_image_set_scale_* 每次调用都会让控件失效，即使数值没变。
    // 缓存上一帧的缩放值，静止状态（待机、睡觉、配网……）就不用整块重绘。
    uint32_t eye_zoom_[2][2][2] = {};  // [左右][样式][x/y]

    void BakeBaseSprites();
    void SetBrowSource(int index, const face::Sprite* sprite, int style);
    void EnterExpression(FaceExpression expression);
    void RefreshActive();
    void ApplyFaceAlpha(uint8_t alpha);
    void ApplyLogo(uint8_t alpha, uint16_t zoom);
};
