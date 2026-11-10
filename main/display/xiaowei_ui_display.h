#ifndef XIAOWEI_UI_DISPLAY_H
#define XIAOWEI_UI_DISPLAY_H

#include "lcd_display.h"

#include <esp_timer.h>

/* 服务端下发的整屏图片（AI 画图）停留时长，单位毫秒。
 * 到点自动收起，并回到"看图之前那一屏"（见 .cc 的 OnScreenImageHidden）。 */
#define SCREEN_IMAGE_DURATION_MS 10000

/*
 * 小未 LCD UI 主显示类（仅 P4 板）
 *
 * 把 lcd 工程那套 UI（main/display/src/ui/）接成 P4 板的**主界面**，取代 xiaozhi
 * 原本那套「表情脸 + 状态 + 字幕」布局。
 *
 * 为什么要单开一个子类而不是改 MipiLcdDisplay：
 *   原布局是**产品默认**，S3 板（WzkjLcdDisplay，见 xiaowei_low.cc）和以后可能
 *   新增的板子都还走它。把这套 UI 挂成一个 P4 专属子类，stock 路径一行不动。
 *
 * 复用基类构造：MipiLcdDisplay 的 ctor 已经做完整套 LVGL 起机 —— 白屏填充、
 * lv_init()、图像缓存、lvgl_port_init()、lvgl_port_add_disp_dsi()。而移植过来的
 * UI 自己**完全不碰** LVGL 生命周期（实测 ui/ 里没有任何 lv_init / lv_timer_handler
 * / lv_display_create 调用），天生就是能挂上来的纯控件层，所以不需要任何粘合代码。
 *
 * ══ 两块屏幕，不是一块 ══
 * 本类要同时养活两套界面，所以 lv_screen_active() 有两种可能：
 *
 *   chat_screen_  ——  xiaozhi 原来的「表情脸 + 状态 + 字幕」，接入成 "chat" app，
 *                     主屏左滑进（shell/home_screen.c 的 LV_DIR_LEFT）。
 *   LCD UI 自己的屏 —— ui/ 那套（home_screen_create / app_list_screen_create /
 *                     各个 app 的 create_screen 都是 lv_obj_create(NULL) 自建屏）。
 *
 * ── chat_screen_ 为什么由本类建，而不是 chat_app.c ──
 * 那一页要的控件整套都在 LcdDisplay::SetupUI() 里，而它是用 lv_screen_active() 取
 * 屏幕的。所以本类先把 chat_screen_ 设成活动屏幕、再调基类 SetupUI()，基类就把
 * 控件建到了这块屏上。于是基类那批 setter（SetEmotion / SetStatus / SetChatMessage
 * / ClearChatMessages / SetPreviewImage / ShowNotification / UpdateStatusBar）
 * **全部可以直接复用，本类一个都不用 override** —— 它们操作的成员控件现在都真实存在。
 *
 * 这也是本类只 override 这几个方法的原因：
 *   SetupUI()             —— 整套接线的唯一入口（见 .cc）
 *   SetTheme()            —— 基类实现会把主题糊到 lv_screen_active()，必须接管（见 .cc）
 *   DisplayImage()        —— 图片落在聊天页那块整屏控件上，得先切过去（见 .cc）
 *   OnScreenImageHidden() —— 图看完了要回到原来那一屏（见 .cc）
 *
 * 反过来说：**不要**把 chat_screen_ 交给 app_registry 去 destroy（见 chat_app.c
 * 里 destroy_screen = NULL 的说明），销毁它 = 基类 setter 全体 use-after-free。
 */
class XiaoweiUiDisplay : public MipiLcdDisplay {
public:
    // 复用 MipiLcdDisplay 的 9 参构造
    using MipiLcdDisplay::MipiLcdDisplay;
    ~XiaoweiUiDisplay() override;

    // 建 LCD UI 外壳 + 聊天页屏（取代 xiaozhi 原来的布局）
    void SetupUI() override;

    // 必须 override：基类实现会把主题写到「当前活动屏幕」上（见 .cc）
    void SetTheme(Theme* theme) override;

    // 服务端下发的图片（AI 画图）必须先切回聊天页 —— 见 .cc
    void DisplayImage(const uint8_t* data, size_t len) override;

    // 服务端下发的天气数据（连接后 type=="weather"）落到天气页上 —— 见 .cc
    void DisplayWeather(const char* json, size_t len) override;

    // 深度研究/绘画任务状态、扫码下载二维码 —— 见 .cc
    void DisplayArtifactEvent(const char* json, size_t len) override;
    void DisplayArtifactImage(const uint8_t* data, size_t len, const char* id) override;

    // 图片收起（点击 / SCREEN_IMAGE_DURATION_MS 超时）后回到看图之前那一屏 —— 见 .cc
    void OnScreenImageHidden() override;

private:
    // 当前研究与绘画任务的 id，用来过滤过期的图片/状态
    std::string artifact_task_id_;
    // 启动/重置"看完自动回去"的计时器（重复调用会把上一次的计时顶掉）
    void StartScreenImageTimer();
    // 计时器到点（跑在 esp_timer 任务上，不是 LVGL 任务）
    void OnScreenImageTimeout();
    // 聊天页的屏幕。由 SetupUI() 创建，之后一直复用（不销毁）。
    lv_obj_t* chat_screen_ = nullptr;

    // 基类 SetupUI() 是否已经跑过（即聊天页那批成员控件是否已存在）。
    // SetTheme() 用它判断能不能安全地把主题应用下去。
    bool chat_ui_built_ = false;

    // 轮询设备状态、在语音活动时自动切到聊天页的 LVGL 定时器。
    lv_timer_t* voice_watch_timer_ = nullptr;

    // 整屏图片的"看完自动回去"计时器。懒创建（第一次看图时建），
    // nullptr 表示还没建或已随析构销毁。
    esp_timer_handle_t screen_image_timer_ = nullptr;

    // DisplayImage() 切到聊天页之前的那一屏，超时/点击收起图片后回到它。
    // nullptr = 本来就在聊天页（那就不用回，收起即可）。
    lv_obj_t* screen_image_prev_screen_ = nullptr;
};

#endif  // XIAOWEI_UI_DISPLAY_H
