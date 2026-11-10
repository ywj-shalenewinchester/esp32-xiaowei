#include "legacy_app.h"

#include "../../assets/xiaowei_assets.h"
#include "../../framework/app_manager.h"
#include "../../fonts.h"
#include "../../platform/xiaowei_ui_port.h"
#include "../../platform/xiaowei_ui_hw.h"
#include "../../shell/submenu.h"
#include "../../ported/weather_screen.h"
#include "../../ported/simon_game.h"
#include "../camera/camera_app.h"
#include "../audio/audio_app.h"


#include <stddef.h>
#include <stdbool.h>
#include <string.h>

/* 手写迁移的完整菜单层级：只复用旧工程的资源，不依赖 EEZ 页面对象。 */
typedef enum {
    ACTION_INFO = 0,
    ACTION_FLASHLIGHT,
    ACTION_TIMER,
    ACTION_CALENDAR,
    ACTION_CAMERA,
    ACTION_IMU,
    ACTION_AUDIO,
    ACTION_MOTOR,
    ACTION_ROBOT,
    ACTION_ALBUM,
} action_t;

typedef struct {
    const char *title;
    const lv_image_dsc_t *icon;
    action_t action;
} menu_item_t;

typedef struct {
    const char *title;
    const char *const *items;
} menu_group_t;

static lv_obj_t *s_root;
static lv_obj_t *s_child;
/* 音乐页常驻复用（和 s_root/s_child 一样不销毁），见 music_player_create()。 */
static lv_obj_t *s_music_screen;
static lv_obj_t *s_control_value;
static bool s_control_is_volume;
static lv_obj_t *s_haptic_value;
static lv_obj_t *s_control_slider;
static lv_obj_t *s_haptic_slider;
static lv_obj_t *s_haptic_buttons[3];
static uint32_t s_haptic_last_play_ms;
/* 震动设置的状态归 UI 自己（板级只负责“播第几号波形 / 设强度”），
   离开页面再进来这些值仍然在，和原来的服务行为一致。 */
static int s_haptic_mode = 1;        /* 0=轻柔 1=标准 2=强烈 */
static int s_haptic_intensity = 80;  /* 0~100，原服务默认 80 */
/* 0/1/2 -> AW86937 RAM 波形号（映射见 aw86937.cc 的 LoadWaveform）。 */
static const uint8_t kModeToWave[3] = {5, 4, 6};
static bool s_slider_dragging;
/* 列表页"这一下是拖拽还是点按"的判定状态，见 list_tap_guard()。 */
static lv_point_t s_press_point;
static bool s_press_dragged;
/* Selection state consumed by the registered secondary-page descriptors. */
static const char *s_pending_info_title;
static size_t s_pending_group_index;

static void style_screen(lv_obj_t *screen)
{
    lv_obj_set_size(screen, 466, 466);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x101418), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
}

static void style_label(lv_obj_t *label, bool large)
{
    lv_obj_set_style_text_font(label,
        large ? &ui_font_font_50_menu_cn : &ui_font_font_30_menu_cn, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0xf9f5f5), 0);
}

static void page_gesture(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_GESTURE) return;
    if (lv_indev_get_gesture_dir(lv_indev_active()) != LV_DIR_RIGHT) return;
    if (s_slider_dragging) return;
    lv_obj_t *target = lv_event_get_target(event);
    for (lv_obj_t *obj = target; obj != NULL; obj = lv_obj_get_parent(obj)) {
        if (obj == s_control_slider || obj == s_haptic_slider) return;
    }
    /* Every page, including legacy secondary pages, returns through the
     * central manager.  This keeps the stack and lifecycle callbacks in sync. */
    xiaowei_app_back();
}

static void slider_guard_event(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING ||
        code == LV_EVENT_PRESS_LOST) {
        s_slider_dragging = true;
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_CANCEL) {
        s_slider_dragging = false;
    }
}

/* ----------------------------------------------------------------------------
 *  这一下是"拖拽列表"还是"点按某一项"
 *
 *  二级/三级页面用的是真正可滚动的 lv_list，而 LVGL 在**抬手那一刻**只凭一件事
 *  决定发什么事件（indev/lv_indev.c:1525-1537）：
 *
 *      scroll_obj == NULL ? CLICKED : SCROLL_THROW_BEGIN
 *
 *  scroll_obj 要等位移累计超过 indev->scroll_limit（默认 10px，lv_indev.c:32）
 *  才会被设上（lv_indev_scroll.c:391-403）。于是：
 *
 *    • 手指只挪了几 px 就抬起来 —— LVGL 认定这是"点按"，照样补一个 CLICKED，
 *      哪怕在 466 的屏上、在 90px 高的列表项上那已经是明确的一划；
 *    • 手指按着不动（正准备开滑）时，只要有一帧被当成"抬手"（触摸偶发抖动、
 *      I2C 读失败、马达波形期间芯片给出的假采样），走的同样是 CLICKED 分支。
 *
 *  两种都会直接 xiaowei_app_open() 开出下一级页面，而换屏是整屏位移动效
 *  （app_manager.c 的 SCREEN_LOAD_ANIM_MS=220），它还叠在一直按着的手指上 ——
 *  用户看到的就是"翻列表时画面闪烁、撕裂、非常不稳定"。
 *
 *  主菜单 app_list_screen.c 早就用 s_menu_dragged 兜住了这件事（它的 item_event
 *  是 `code != CLICKED || s_menu_dragged`，PRESSING 里 dy 一动就算拖拽），所以
 *  主菜单不炸；本文件原来的 item_event/group_event/group_item_event/
 *  motor_wave_event/haptic_mode_click 只判 `code != LV_EVENT_CLICKED`，
 *  同一个毛病就原样留在二级三级页上。
 *
 *  规则刻意和主菜单保持一致：记下按下时的位置，之后偏离超过 LIST_TAP_SLOP_PX
 *  即算拖拽；收到 PRESS_LOST（手指划出了这一项，也包括 LVGL 中途换了 act_obj）
 *  同样算。返回值 true = 这次事件不该按"点按"处理，调用者直接 return。
 * -------------------------------------------------------------------------- */
#define LIST_TAP_SLOP_PX 8

static bool list_tap_guard(lv_event_t *event)
{
    switch (lv_event_get_code(event)) {
    case LV_EVENT_PRESSED:
        lv_indev_get_point(lv_indev_active(), &s_press_point);
        s_press_dragged = false;
        return true;

    case LV_EVENT_PRESSING: {
        lv_point_t point;
        lv_indev_get_point(lv_indev_active(), &point);
        if (point.x - s_press_point.x > LIST_TAP_SLOP_PX ||
            s_press_point.x - point.x > LIST_TAP_SLOP_PX ||
            point.y - s_press_point.y > LIST_TAP_SLOP_PX ||
            s_press_point.y - point.y > LIST_TAP_SLOP_PX) {
            s_press_dragged = true;
        }
        return true;
    }

    case LV_EVENT_PRESS_LOST:
        /* 手指还在屏上，却从这一项上划走了 —— 这是拖拽，不是点按。 */
        s_press_dragged = true;
        return true;

    case LV_EVENT_CANCEL:
        /* 换屏时 lv_indev_wait_release() 打的那一下，这一按本来就作废。 */
        s_press_dragged = false;
        return true;

    case LV_EVENT_CLICKED:
        return s_press_dragged;

    default:
        return true;
    }
}

static void add_title(lv_obj_t *screen, const char *title)
{
    lv_obj_t *label = lv_label_create(screen);
    lv_label_set_text(label, title);
    style_label(label, true);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 24);
}

static void add_back_hint(lv_obj_t *screen)
{
    lv_obj_t *hint = lv_label_create(screen);
    lv_label_set_text(hint, "右滑返回");
    style_label(hint, false);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x9aa4ad), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -8);
}

static lv_obj_t *create_info_screen(const char *title)
{
    s_child = lv_obj_create(NULL);
    style_screen(s_child);
    lv_obj_add_event_cb(s_child, page_gesture, LV_EVENT_GESTURE, NULL);
    add_title(s_child, title);
    add_back_hint(s_child);
    lv_obj_t *label = lv_label_create(s_child);
    lv_label_set_text(label, "页面已接入新的 UI 生命周期");
    style_label(label, false);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 20);
    return s_child;
}

static void control_changed(lv_event_t *event)
{
    lv_obj_t *slider = lv_event_get_target(event);
    int value = (int)lv_slider_get_value(slider);
    if (s_control_is_volume) xiaowei_ui_set_volume(value);
    else xiaowei_ui_set_brightness(value);
    if (s_control_value) lv_label_set_text_fmt(s_control_value, "%d%%", value);
}

static lv_obj_t *create_control_screen(const char *title, bool volume)
{
    s_child = lv_obj_create(NULL);
    style_screen(s_child);
    lv_obj_add_event_cb(s_child, page_gesture, LV_EVENT_GESTURE, NULL);
    add_title(s_child, title);
    s_control_is_volume = volume;
    lv_obj_t *list = lv_list_create(s_child);
    lv_obj_set_pos(list, 73, 110); lv_obj_set_size(list, 320, 340);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_t *slider_title = lv_label_create(list);
    lv_label_set_text(slider_title, title);
    lv_obj_set_style_text_font(slider_title, &ui_font_font_32_cn, 0);
    lv_obj_set_style_text_color(slider_title, lv_color_hex(0xf9f5f5), 0);
    lv_obj_set_width(slider_title, 300);
    lv_obj_set_style_pad_top(slider_title, 24, 0);
    lv_obj_set_style_pad_bottom(slider_title, 12, 0);
    lv_obj_set_style_text_align(slider_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *slider = lv_slider_create(list);
    s_control_slider = slider;
    lv_obj_set_width(slider, 270); lv_obj_set_height(slider, 24);
    int value = volume ? xiaowei_ui_get_volume() : xiaowei_ui_get_brightness();
    lv_slider_set_range(slider, 0, 100);
    lv_slider_set_value(slider, value, LV_ANIM_OFF);
    lv_obj_set_ext_click_area(slider, 20);
    lv_obj_clear_flag(slider, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(slider, slider_guard_event, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(slider, control_changed, LV_EVENT_VALUE_CHANGED, NULL);
    s_control_value = lv_label_create(list);
    lv_obj_set_style_text_font(s_control_value, &ui_font_font_32_cn, 0);
    lv_obj_set_style_text_color(s_control_value, lv_color_hex(0xf9f5f5), 0);
    lv_label_set_text_fmt(s_control_value, "%d%%", value);
    lv_obj_set_width(s_control_value, 300);
    lv_obj_set_style_text_align(s_control_value, LV_TEXT_ALIGN_CENTER, 0);
    return s_child;
}

/* 测试页 6 个按钮 -> 波形序列（1..7；0 = 结束）。
   error 拆成 part1 + part2 两段，其余一段。 */
static const uint8_t kMotorTestSeq[6][2] = {
    {1, 0},  /* ALARM */
    {2, 3},  /* ERROR: part1 -> part2 */
    {4, 0},  /* NORMAL */
    {5, 0},  /* SOFT */
    {6, 0},  /* STRONG */
    {7, 0},  /* SUCCESS */
};

/* 播放序列完全在 UI 侧用 LVGL 定时器推进：底层只给 play_wave / is_playing，
   板级不知道“测试页”这回事。节奏照搬原服务的时序（30 ms/3000 ms/120 ms），
   避免刚触发就查 IsPlaying 被芯片 STANDBY 误判成“已播完”。 */
enum {
    MOTOR_PHASE_IDLE = 0,  /* 开播前等上一段彻底停下（最多 500 ms） */
    MOTOR_PHASE_PLAYING,   /* 等这一段播完（最多 3000 ms） */
    MOTOR_PHASE_GAP,       /* 两段之间留 120 ms，听感边界清晰 */
};
static lv_timer_t *s_motor_test_timer;
static bool s_motor_test_running;
static int s_motor_test_id;
static int s_motor_test_part;
static int s_motor_test_phase;
static uint32_t s_motor_test_phase_ms;

static void motor_test_stop(void)
{
    s_motor_test_running = false;
    if (s_motor_test_timer) lv_timer_pause(s_motor_test_timer);
}

static void motor_test_start_part(void)
{
    uint8_t wave = kMotorTestSeq[s_motor_test_id][s_motor_test_part];
    if (wave == 0 || !xiaowei_haptic_play_wave((int)wave)) {
        motor_test_stop();
        return;
    }
    s_motor_test_phase = MOTOR_PHASE_PLAYING;
    s_motor_test_phase_ms = lv_tick_get();
}

static void motor_test_tick(lv_timer_t *timer)
{
    (void)timer;
    uint32_t now = lv_tick_get();
    uint32_t elapsed = now - s_motor_test_phase_ms;

    switch (s_motor_test_phase) {
    case MOTOR_PHASE_IDLE:
        /* 上一段（可能是别的按钮触发的）还在播时先别 load，否则会覆盖 RAM。 */
        if (!xiaowei_haptic_is_playing() || elapsed >= 500) {
            motor_test_start_part();
        }
        break;

    case MOTOR_PHASE_PLAYING:
        if (elapsed >= 30 && !xiaowei_haptic_is_playing()) {
            /* 这一段的最后一帧：推进到下一段或结束。 */
            s_motor_test_part++;
            if (kMotorTestSeq[s_motor_test_id][s_motor_test_part] == 0) {
                motor_test_stop();
            } else {
                s_motor_test_phase = MOTOR_PHASE_GAP;
                s_motor_test_phase_ms = now;
            }
        } else if (elapsed >= 3000) {
            /* 状态一直读不回来时不要卡死这一页，超时也往下走。 */
            s_motor_test_part++;
            if (kMotorTestSeq[s_motor_test_id][s_motor_test_part] == 0) {
                motor_test_stop();
            } else {
                s_motor_test_phase = MOTOR_PHASE_GAP;
                s_motor_test_phase_ms = now;
            }
        }
        break;

    case MOTOR_PHASE_GAP:
        if (elapsed >= 120) motor_test_start_part();
        break;

    default:
        motor_test_stop();
        break;
    }
}

static void motor_wave_event(lv_event_t *event)
{
    if (list_tap_guard(event)) return;
    if (s_motor_test_running) return;  /* 上一段还没播完，忽略连点 */

    /* 定时器懒创建一次、跑完只暂停，不从回调里 delete（避免 LVGL 版本差异）。 */
    if (s_motor_test_timer == NULL) {
        s_motor_test_timer = lv_timer_create(motor_test_tick, 20, NULL);
        if (s_motor_test_timer == NULL) return;
    }

    s_motor_test_id = (int)(intptr_t)lv_event_get_user_data(event);
    s_motor_test_part = 0;
    s_motor_test_phase = MOTOR_PHASE_IDLE;
    s_motor_test_phase_ms = lv_tick_get();
    s_motor_test_running = true;
    lv_timer_reset(s_motor_test_timer);
    lv_timer_resume(s_motor_test_timer);
}

static lv_obj_t *create_motor_screen(void)
{
    s_child = lv_obj_create(NULL);
    style_screen(s_child);
    lv_obj_add_event_cb(s_child, page_gesture, LV_EVENT_GESTURE, NULL);
    lv_obj_t *title = lv_label_create(s_child);
    lv_label_set_text(title, "MOTOR WAVE");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 28);
    lv_obj_t *column = lv_obj_create(s_child);
    lv_obj_set_pos(column, 93, 92); lv_obj_set_size(column, 280, 356);
    lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(column, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(column, 8, 0); lv_obj_set_style_pad_row(column, 12, 0);
    lv_obj_set_style_bg_opa(column, LV_OPA_TRANSP, 0); lv_obj_set_style_border_width(column, 0, 0);
    lv_obj_set_scrollbar_mode(column, LV_SCROLLBAR_MODE_OFF);
    static const char *const names[] = { "ALARM", "ERROR", "NORMAL", "SOFT", "STRONG", "SUCCESS" };
    for (int i = 0; i < 6; ++i) {
        lv_obj_t *button = lv_button_create(column);
        lv_obj_set_width(button, LV_PCT(100)); lv_obj_set_height(button, 54);
        lv_obj_set_style_bg_color(button, lv_color_hex(0x2E7DFF), 0);
        lv_obj_set_style_radius(button, 14, 0); lv_obj_set_style_shadow_width(button, 0, 0);
        lv_obj_add_event_cb(button, motor_wave_event, LV_EVENT_ALL, (void *)(intptr_t)i);
        lv_obj_t *label = lv_label_create(button);
        lv_label_set_text(label, names[i]);
        lv_obj_set_style_text_color(label, lv_color_white(), 0);
        lv_obj_center(label);
    }
    return s_child;
}

/* 播当前模式对应的波形。模式 0/1/2 -> 波形 5/4/6（轻柔/标准/强烈）。 */
static void haptic_play_mode_wave(void)
{
    if (s_haptic_mode < 0 || s_haptic_mode > 2) s_haptic_mode = 1;
    xiaowei_haptic_play_wave((int)kModeToWave[s_haptic_mode]);
}

static void haptic_apply_style(void)
{
    int selected = s_haptic_mode;
    for (int i = 0; i < 3; ++i) {
        if (!s_haptic_buttons[i]) continue;
        bool active = i == selected;
        lv_obj_set_style_bg_color(s_haptic_buttons[i],
                                  lv_color_hex(active ? 0x2F6FED : 0x232A31), 0);
        lv_obj_set_style_bg_opa(s_haptic_buttons[i], LV_OPA_COVER, 0);
        lv_obj_set_style_outline_width(s_haptic_buttons[i], active ? 2 : 0, 0);
        lv_obj_set_style_outline_color(s_haptic_buttons[i], lv_color_hex(0x2F6FED), 0);
    }
}

static void haptic_mode_click(lv_event_t *event)
{
    if (list_tap_guard(event)) return;
    s_haptic_mode = (int)(intptr_t)lv_event_get_user_data(event);
    haptic_apply_style();
    haptic_play_mode_wave();
}

static void haptic_slider_changed(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) return;
    int value = (int)lv_slider_get_value(lv_event_get_target(event));
    s_haptic_intensity = value;
    xiaowei_haptic_set_intensity(value);
    if (s_haptic_value) lv_label_set_text_fmt(s_haptic_value, "%d%%", value);
    /* 拖动时节流触发：波形号没变，底层只写增益寄存器，震感实时跟着滑条走。 */
    uint32_t now = lv_tick_get();
    if ((uint32_t)(now - s_haptic_last_play_ms) >= 90) {
        s_haptic_last_play_ms = now;
        haptic_play_mode_wave();
    }
}

static lv_obj_t *create_haptic_screen(void)
{
    s_child = lv_obj_create(NULL);
    style_screen(s_child);
    lv_obj_add_event_cb(s_child, page_gesture, LV_EVENT_GESTURE, NULL);
    add_title(s_child, "震动");
    lv_obj_t *list = lv_obj_create(s_child);
    lv_obj_set_size(list, 320, 340); lv_obj_set_pos(list, 73, 110);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 8, 0);
    lv_obj_t *strength = lv_label_create(list);
    lv_label_set_text(strength, "震动强度");
    lv_obj_set_style_text_font(strength, &ui_font_font_32_cn, 0);
    lv_obj_set_style_text_color(strength, lv_color_hex(0xf9f5f5), 0);
    lv_obj_t *row = lv_obj_create(list);
    lv_obj_remove_style_all(row); lv_obj_set_size(row, 300, 60);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *slider = lv_slider_create(row);
    s_haptic_slider = slider;
    lv_obj_set_size(slider, 200, 20); lv_obj_align(slider, LV_ALIGN_LEFT_MID, 12, 0);
    lv_slider_set_range(slider, 0, 100);
    lv_slider_set_value(slider, s_haptic_intensity, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x3A424B), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x2F6FED), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0xF2F4F7), LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 8, LV_PART_KNOB);
    lv_obj_set_ext_click_area(slider, 25);
    lv_obj_clear_flag(slider, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_clear_flag(slider, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(slider, slider_guard_event, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(slider, haptic_slider_changed, LV_EVENT_VALUE_CHANGED, NULL);
    s_haptic_value = lv_label_create(row);
    lv_obj_align(s_haptic_value, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_text_font(s_haptic_value, &ui_font_font_28_cn, 0);
    lv_obj_set_style_text_color(s_haptic_value, lv_color_hex(0xf9f5f5), 0);
    lv_label_set_text_fmt(s_haptic_value, "%d%%", s_haptic_intensity);
    lv_obj_t *mode_title = lv_label_create(list);
    lv_label_set_text(mode_title, "震动模式");
    lv_obj_set_style_text_font(mode_title, &ui_font_font_32_cn, 0);
    lv_obj_set_style_text_color(mode_title, lv_color_hex(0xf9f5f5), 0);
    static const char *const names[] = { "轻柔", "标准", "强烈" };
    for (int i = 0; i < 3; ++i) {
        s_haptic_buttons[i] = lv_button_create(list);
        lv_obj_set_width(s_haptic_buttons[i], LV_PCT(100));
        lv_obj_set_height(s_haptic_buttons[i], 60);
        lv_obj_set_style_radius(s_haptic_buttons[i], 16, 0);
        lv_obj_add_event_cb(s_haptic_buttons[i], haptic_mode_click, LV_EVENT_ALL, (void *)(intptr_t)i);
        lv_obj_t *label = lv_label_create(s_haptic_buttons[i]);
        lv_label_set_text(label, names[i]);
        lv_obj_set_style_text_font(label, &ui_font_font_28_cn, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(0xf9f5f5), 0);
        lv_obj_center(label);
    }
    haptic_apply_style();
    return s_child;
}

static void item_event(lv_event_t *event)
{
    if (list_tap_guard(event)) return;
    const menu_item_t *item = (const menu_item_t *)lv_event_get_user_data(event);
    if (!item) return;
    switch (item->action) {
    case ACTION_FLASHLIGHT: xiaowei_app_open("flashlight"); break;
    case ACTION_TIMER:      xiaowei_app_open("timer"); break;
    case ACTION_CALENDAR:   xiaowei_app_open("calendar"); break;
    case ACTION_CAMERA:     xiaowei_app_open("camera_live"); break;
    case ACTION_IMU:        xiaowei_app_open("imu_test"); break;
    case ACTION_AUDIO:      xiaowei_app_open("audio_test"); break;
    case ACTION_MOTOR:      xiaowei_app_open("motor_test"); break;
    case ACTION_ROBOT:      xiaowei_app_open("robot"); break;
    case ACTION_ALBUM:      xiaowei_app_open("camera_album"); break;
    default:
        s_pending_info_title = item->title;
        xiaowei_app_open("info");
        break;
    }
}

/* 二级页的列表本体搬到了 shell/submenu.{h,c}（相机、游戏也用同一份，见那里），
 * 这里只留「菜单表 -> 统一条目表」的转换：user_data 仍指向**静态的** menu_item_t
 * 数组元素（item_event 靠它分派），千万别改成指向下面这个局部数组 ——
 * 它出了函数就没了，而按钮的回调是活到页面销毁才走的。 */
static lv_obj_t *build_list(const char *title, const menu_item_t *items, size_t count)
{
    if (s_child) lv_obj_del(s_child);
    s_child = lv_obj_create(NULL);
    style_screen(s_child);
    lv_obj_add_event_cb(s_child, page_gesture, LV_EVENT_GESTURE, NULL);

    /* 目前最长的一张表是 s_tool_items 的 5 条；上限只是防手滑写超。 */
    enum { MAX_ROWS = 8 };
    xiaowei_submenu_item_t rows[MAX_ROWS];
    if (count > MAX_ROWS) count = MAX_ROWS;
    for (size_t i = 0; i < count; ++i) {
        rows[i].icon = items[i].icon;
        rows[i].label = items[i].title;
        rows[i].on_event = item_event;
        rows[i].user_data = (void *)&items[i];
    }
    xiaowei_submenu_build(s_child, title, rows, count);
    return s_child;
}

/* These arrays are iterated until NULL by create_settings_group_screen().
 * The missing sentinels previously made a group continue into adjacent static
 * data, which is why entries such as Robot leaked into the appearance page. */
static const char *const s_appearance[] = { "菜单模式", "主界面模式", "待机表情", NULL };
static const char *const s_interaction[] = { "唤醒方式", "摇晃互动", NULL };
static const char *const s_display[] = { "亮度", "息屏时间", "夜间模式", NULL };
static const char *const s_sound[] = { "音量", "静音", "提示音", NULL };
static const char *const s_haptic[] = { "震动强度", "震动模式", NULL };
static const char *const s_network[] = { "WiFi状态", "重新配网", NULL };
static const char *const s_language[] = { "English", "中文", NULL };
static const char *const s_system[] = { "OTA升级", "重启设备", "恢复出厂", NULL };
static const char *const s_about[] = { "AI Desktop Companion", "Model: ESP32-P4", "Firmware: V1.0.0", "Hardware: Rev A", NULL };

/* 事件 user_data 必须指向静态存储，不能指向 create_screen() 的局部数组。 */
static const menu_item_t s_ai_items[] = {
    { "AI人格", &xiaowei_asset_icon_robot_outline, ACTION_INFO },
    { "AI功能", &xiaowei_asset_icon_robot, ACTION_INFO },
    { "AI音色", &xiaowei_asset_icon_ai_voice, ACTION_INFO },
    { "AI绘画", &xiaowei_asset_icon_ai_draw, ACTION_INFO },
};
/* 音乐原来是一张 lv_list（"播放列表"/"录音" 两条，带图标）。现在整条砍掉 ——
   歌单以后由服务器下发，设备端不再自己维护，所以没有条目表了。
   见下面的 music_player_create()。 */
static const menu_item_t s_test_items[] = {
    { "IMU", &xiaowei_asset_icon_imu, ACTION_IMU },
    { "audio", &xiaowei_asset_icon_audio, ACTION_AUDIO },
    { "motor", &xiaowei_asset_icon_motor, ACTION_MOTOR },
    { "camera", &xiaowei_asset_icon_camera, ACTION_CAMERA },
    { "Robot", &xiaowei_asset_icon_robot_outline, ACTION_ROBOT },
};
static const menu_item_t s_tool_items[] = {
    { "手电筒", &xiaowei_asset_icon_flashlight, ACTION_FLASHLIGHT },
    { "计时器", &xiaowei_asset_icon_timer, ACTION_TIMER },
    { "日历", &xiaowei_asset_icon_calendar, ACTION_CALENDAR },
    { "闹钟", &xiaowei_asset_icon_alarm, ACTION_INFO },
    { "录音", &xiaowei_asset_icon_audio, ACTION_INFO },
};
static const menu_group_t s_groups[] = {
    { "外观", s_appearance }, { "交互", s_interaction }, { "显示", s_display },
    { "声音", s_sound }, { "震动", s_haptic }, { "网络", s_network },
    { "语言", s_language }, { "系统", s_system }, { "关于", s_about },
};

static __attribute__((unused)) lv_obj_t *build_hardware_menu(void)
{
    s_root = lv_obj_create(NULL);
    style_screen(s_root);
    lv_obj_add_event_cb(s_root, page_gesture, LV_EVENT_GESTURE, NULL);
    for (size_t i = 0; i < sizeof(s_test_items) / sizeof(s_test_items[0]); ++i) {
        lv_obj_t *button = lv_button_create(s_root);
        lv_obj_set_size(button, 266, 90);
        lv_obj_set_pos(button, 100, 53 + (lv_coord_t)i * 90);
        lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(button, 0, 0);
        lv_obj_set_style_shadow_width(button, 0, 0);
        lv_obj_add_event_cb(button, item_event, LV_EVENT_ALL, (void *)&s_test_items[i]);
        if (s_test_items[i].icon) {
            lv_obj_t *icon = lv_image_create(button);
            lv_image_set_src(icon, s_test_items[i].icon);
            lv_obj_align(icon, LV_ALIGN_LEFT_MID, 0, 0);
        }
        lv_obj_t *label = lv_label_create(button);
        lv_label_set_text(label, s_test_items[i].title);
        style_label(label, true);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 94, 0);
    }
    return s_root;
}

static void group_item_event(lv_event_t *event)
{
    if (list_tap_guard(event)) return;

    lv_obj_t *button = lv_event_get_target(event);
    lv_obj_t *label = lv_obj_get_child(button, 0);
    if (!label) return;
    const char *text = lv_label_get_text(label);
    if (strcmp(text, "亮度") == 0) {
        s_control_is_volume = false;
        xiaowei_app_open("settings_brightness");
    } else if (strcmp(text, "音量") == 0) {
        s_control_is_volume = true;
        xiaowei_app_open("settings_volume");
    } else {
        s_pending_info_title = text;
        xiaowei_app_open("info");
    }
}

static lv_obj_t *create_settings_group_screen(void)
{
    const menu_group_t *group = &s_groups[s_pending_group_index];
    s_child = lv_obj_create(NULL);
    style_screen(s_child);
    lv_obj_add_event_cb(s_child, page_gesture, LV_EVENT_GESTURE, NULL);
    add_title(s_child, group->title);
    lv_obj_t *list = lv_list_create(s_child);
    lv_obj_set_size(list, 320, 350);
    lv_obj_set_pos(list, 73, 100);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 8, 0);
    lv_obj_set_style_pad_row(list, 6, 0);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    for (size_t i = 0; group->items[i]; ++i) {
        lv_obj_t *button = lv_list_add_button(list, NULL, group->items[i]);
        lv_obj_set_width(button, 304);
        lv_obj_set_height(button, 78);
        lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(button, 0, 0);
        lv_obj_set_style_shadow_width(button, 0, 0);
        lv_obj_add_event_cb(button, group_item_event, LV_EVENT_ALL, NULL);
        lv_obj_t *label = lv_obj_get_child(button, 0);
        if (label) {
            style_label(label, true);
            lv_obj_set_style_text_color(label, lv_color_hex(0xf9f5f5), 0);
        }
    }
    return s_child;
}

static void group_event(lv_event_t *event)
{
    if (list_tap_guard(event)) return;
    const menu_group_t *group = (const menu_group_t *)lv_event_get_user_data(event);
    if (!group) return;
    s_pending_group_index = (size_t)(group - s_groups);
    if (s_pending_group_index == 4) xiaowei_app_open("settings_haptic");
    else xiaowei_app_open("settings_group");
}

static lv_obj_t *ai_create(void)
{
    s_root = build_list("AI助手", s_ai_items, sizeof(s_ai_items) / sizeof(s_ai_items[0])); s_child = NULL; return s_root;
}
static lv_obj_t *camera_create(void)
{
    s_root = camera_menu_create_for_app(); s_child = NULL; return s_root;
}
static lv_obj_t *weather_create(void) { s_root = weather_screen_create_for_app(); s_child = NULL; return s_root; }
/* ----------------------------------------------------------------------------
 *  音乐页：**只有播放器 UI，没有列表、没有图标素材**
 *
 *  曲目列表以后由服务器下发（和天气同一路数：服务端拉好、设备连上就推），
 *  设备端不再自己维护一份歌单。所以这一页先摆占位：封面框、曲名/歌手、
 *  进度条、三个控制键 —— 形状照着播放器摆，等歌单来了往里填就行。
 *
 *  两条"不能用"记在这里，免得以后有人顺手改回去：
 *    • 不用 assets 里的图标素材 —— 需求就是去掉图标；
 *    • 不用 LV_SYMBOL_PREV / PLAY / NEXT —— 本 build 的 montserrat 字形表里
 *      没有 0xF048 / 0xF04B / 0xF051 这三个码位（在 lv_font_montserrat_20.c
 *      里逐个查过），塞进 lv_label 只会渲染成空白，而不是画不出来报错。
 *  所以控制键是拿普通控件画的圆：中间实心 = 播放，两边描边 = 上一首/下一首。
 * ------------------------------------------------------------------------- */

#define MUSIC_KEY_D   56   /* 控制键直径 */
#define MUSIC_KEY_GAP 44   /* 两个键边缘之间的空档 */

static void music_add_key(lv_obj_t *screen, int x, bool filled)
{
    lv_obj_t *key = lv_obj_create(screen);
    lv_obj_remove_style_all(key);
    lv_obj_clear_flag(key, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(key, MUSIC_KEY_D, MUSIC_KEY_D);
    lv_obj_set_style_radius(key, LV_RADIUS_CIRCLE, 0);
    lv_obj_align(key, LV_ALIGN_CENTER, x, 160);
    if (filled) {
        lv_obj_set_style_bg_color(key, lv_color_hex(0xF9F5F5), 0);
        lv_obj_set_style_bg_opa(key, LV_OPA_COVER, 0);
    } else {
        lv_obj_set_style_border_width(key, 2, 0);
        lv_obj_set_style_border_color(key, lv_color_hex(0x8A949E), 0);
    }
}

static void music_add_caption(lv_obj_t *screen, const char *text, int x, int y)
{
    lv_obj_t *label = lv_label_create(screen);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0x8A949E), 0);
    lv_obj_align(label, LV_ALIGN_CENTER, x, y);
}

static lv_obj_t *music_player_create(void)
{
    if (s_music_screen) return s_music_screen;

    s_music_screen = lv_obj_create(NULL);
    style_screen(s_music_screen);
    lv_obj_add_event_cb(s_music_screen, page_gesture, LV_EVENT_GESTURE, NULL);

    lv_obj_t *title = lv_label_create(s_music_screen);
    lv_label_set_text(title, "MUSIC");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 60);

    /* 封面占位：一个空框，等有曲目了再往里放图 */
    lv_obj_t *cover = lv_obj_create(s_music_screen);
    lv_obj_clear_flag(cover, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(cover, 160, 160);
    lv_obj_align(cover, LV_ALIGN_CENTER, 0, -78);
    lv_obj_set_style_radius(cover, 24, 0);
    lv_obj_set_style_bg_color(cover, lv_color_hex(0x1C2228), 0);
    lv_obj_set_style_bg_opa(cover, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(cover, 1, 0);
    lv_obj_set_style_border_color(cover, lv_color_hex(0x39434D), 0);

    lv_obj_t *song = lv_label_create(s_music_screen);
    lv_label_set_text(song, "No Music");
    lv_obj_set_style_text_font(song, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(song, lv_color_hex(0xF9F5F5), 0);
    lv_obj_align(song, LV_ALIGN_CENTER, 0, 30);

    music_add_caption(s_music_screen, "Waiting for playlist", 0, 58);

    /* 进度条：停在 0%，两头是时间占位 */
    lv_obj_t *bar = lv_bar_create(s_music_screen);
    lv_obj_set_size(bar, 300, 6);
    lv_obj_align(bar, LV_ALIGN_CENTER, 0, 92);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_radius(bar, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x39434D), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 3, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0xF9F5F5), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);

    music_add_caption(s_music_screen, "00:00", -118, 118);
    music_add_caption(s_music_screen, "00:00", 118, 118);

    const int step = MUSIC_KEY_D + MUSIC_KEY_GAP;
    music_add_key(s_music_screen, -step, false);
    music_add_key(s_music_screen, 0, true);
    music_add_key(s_music_screen, step, false);

    return s_music_screen;
}

static lv_obj_t *music_create(void)
{
    s_root = music_player_create();
    s_child = NULL;
    return s_root;
}
static lv_obj_t *game_create(void) { s_root = simon_game_create_for_app(); s_child = NULL; return s_root; }
static lv_obj_t *test_create(void)
{
    /* 标题字号降到 30 号后，"测/试"两个字不在 ui_font_font_30_menu_cn 里
       （见 shell/submenu.c 里 TITLE_FONT 那段），所以这一页改用英文标题。 */
    s_root = build_list("TEST", s_test_items, sizeof(s_test_items) / sizeof(s_test_items[0]));
    s_child = NULL;
    return s_root;
}
static lv_obj_t *tools_create(void)
{
    s_root = build_list("工具", s_tool_items, sizeof(s_tool_items) / sizeof(s_tool_items[0])); s_child = NULL; return s_root;
}

static lv_obj_t *settings_create(void)
{
    s_root = lv_obj_create(NULL); style_screen(s_root); lv_obj_add_event_cb(s_root, page_gesture, LV_EVENT_GESTURE, NULL);
    /* 旧 interaction 初始化后会把生成的设置列表移到 y=100，并加顶部标题。 */
    add_title(s_root, "设置");
    lv_obj_t *list = lv_list_create(s_root); lv_obj_set_size(list, 320, 350); lv_obj_set_pos(list, 73, 100);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0); lv_obj_set_style_border_width(list, 0, 0); lv_obj_set_style_pad_all(list, 8, 0); lv_obj_set_style_pad_row(list, 6, 0); lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    static const lv_image_dsc_t *const icons[] = {
        &xiaowei_asset_icon_appearance, &xiaowei_asset_icon_interaction,
        &xiaowei_asset_icon_display, &xiaowei_asset_icon_sound,
        &xiaowei_asset_icon_haptic, &xiaowei_asset_icon_network,
        &xiaowei_asset_icon_language, &xiaowei_asset_icon_system,
        &xiaowei_asset_icon_about,
    };
    for (size_t i = 0; i < sizeof(s_groups) / sizeof(s_groups[0]); ++i) {
        lv_obj_t *button = lv_list_add_button(list, icons[i], s_groups[i].title);
        lv_obj_set_height(button, 90); lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0); lv_obj_set_style_border_width(button, 0, 0); lv_obj_set_style_shadow_width(button, 0, 0); lv_obj_set_style_pad_left(button, 16, 0); lv_obj_add_event_cb(button, group_event, LV_EVENT_ALL, (void *)&s_groups[i]);
        lv_obj_t *label = lv_obj_get_child(button, 1); if (label) { style_label(label, true); lv_obj_set_width(label, LV_SIZE_CONTENT); }
    }
    s_child = NULL;
    return s_root;
}

/* Registered secondary pages.  The small amount of pending state above is
 * intentional: it lets one descriptor represent all of the legacy
 * information/settings sub-pages without duplicating page implementations. */
static lv_obj_t *info_create(void)
{
    return create_info_screen(s_pending_info_title ? s_pending_info_title : "信息");
}

static lv_obj_t *motor_create(void)
{
    return create_motor_screen();
}

static lv_obj_t *brightness_create(void)
{
    return create_control_screen("亮度", false);
}

static lv_obj_t *volume_create(void)
{
    return create_control_screen("音量", true);
}

static lv_obj_t *haptic_create(void)
{
    return create_haptic_screen();
}

static lv_obj_t *settings_group_create(void)
{
    return create_settings_group_screen();
}

#define DESC(fn, id_, title_, icon_, create_) \
    static const xiaowei_app_t fn##_desc = { .id = id_, .title = title_, .icon = icon_, .create_screen = create_, .on_open = NULL, .on_close = NULL, .destroy_screen = NULL }; \
    const xiaowei_app_t *fn##_descriptor(void) { return &fn##_desc; }

DESC(legacy_ai, "ai", "AI助手", &xiaowei_asset_icon_ai, ai_create)
DESC(legacy_camera, "camera", "相机", &xiaowei_asset_icon_camera, camera_create)
DESC(legacy_weather, "weather", "天气", &xiaowei_asset_icon_weather, weather_create)
DESC(legacy_music, "music", "音乐", &xiaowei_asset_icon_music, music_create)
DESC(legacy_game, "game", "游戏", &xiaowei_asset_icon_game, game_create)
DESC(legacy_tools, "tools", "工具", &xiaowei_asset_icon_tools, tools_create)
DESC(legacy_test, "test", "测试", &xiaowei_asset_icon_test, test_create)
DESC(legacy_settings, "settings", "设置", &xiaowei_asset_icon_settings, settings_create)
DESC(legacy_info, "info", "信息", NULL, info_create)
DESC(legacy_motor, "motor_test", "电机测试", &xiaowei_asset_icon_motor, motor_create)
DESC(legacy_brightness, "settings_brightness", "亮度", NULL, brightness_create)
DESC(legacy_volume, "settings_volume", "音量", NULL, volume_create)
DESC(legacy_haptic, "settings_haptic", "震动", &xiaowei_asset_icon_haptic, haptic_create)
DESC(legacy_settings_group, "settings_group", "设置项", NULL, settings_group_create)
