#include "timer_app.h"
#include "../../shell/app_list_screen.h"
#include "../../framework/app_manager.h"
#include "../../platform/xiaowei_ui_port.h"
#include "../../assets/xiaowei_assets.h"
#include "../../fonts.h"

static lv_obj_t *s_screen, *s_info, *s_arc, *s_minutes, *s_seconds, *s_start, *s_start_label;
static lv_timer_t *s_timer;
static int s_remaining;
static const char *s_options = "0\n1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n12\n13\n14\n15\n16\n17\n18\n19\n20\n21\n22\n23\n24\n25\n26\n27\n28\n29\n30\n31\n32\n33\n34\n35\n36\n37\n38\n39\n40\n41\n42\n43\n44\n45\n46\n47\n48\n49\n50\n51\n52\n53\n54\n55\n56\n57\n58\n59";

static void update_view(void) {
    lv_label_set_text_fmt(s_info, "%02d:%02d", s_remaining / 60, s_remaining % 60);
    if (s_arc) lv_arc_set_value(s_arc, s_remaining);
}
static void timer_tick(lv_timer_t *t) {
    (void)t;
    if (s_remaining > 0) --s_remaining;
    update_view();
    if (s_remaining <= 0 && s_timer) {
        lv_timer_delete(s_timer); s_timer = NULL;
        lv_label_set_text(s_start_label, "开始");
        xiaowei_ui_show_notification("计时器", "计时结束");
    }
}
static void start_event(lv_event_t *e) {
    (void)e;
    if (s_timer) {
        lv_timer_delete(s_timer); s_timer = NULL;
        lv_label_set_text(s_start_label, "开始");
        return;
    }
    int total = lv_roller_get_selected(s_minutes) * 60 + lv_roller_get_selected(s_seconds);
    if (total <= 0) total = 60;
    s_remaining = total;
    lv_arc_set_range(s_arc, 0, total);
    update_view();
    /* s_start is the button; only the child label may be passed to the
       label API. Passing the button here corrupts LVGL's object memory and
       was the cause of the load-access fault when starting a timer. */
    lv_label_set_text(s_start_label, "停止");
    s_timer = lv_timer_create(timer_tick, 1000, NULL);
}
static void screen_event(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_GESTURE &&
        lv_indev_get_gesture_dir(lv_indev_active()) == LV_DIR_RIGHT) xiaowei_app_back();
}
static lv_obj_t *create_screen(void) {
    s_screen = lv_obj_create(NULL);
    lv_obj_set_size(s_screen, 466, 466);
    lv_obj_set_style_bg_color(s_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(s_screen, screen_event, LV_EVENT_GESTURE, NULL);
    s_minutes = lv_roller_create(s_screen); lv_roller_set_options(s_minutes, s_options, LV_ROLLER_MODE_INFINITE);
    lv_obj_set_width(s_minutes, 90); lv_obj_set_height(s_minutes, 160); lv_obj_align(s_minutes, LV_ALIGN_CENTER, -65, -20);
    s_seconds = lv_roller_create(s_screen); lv_roller_set_options(s_seconds, s_options, LV_ROLLER_MODE_INFINITE);
    lv_obj_set_width(s_seconds, 90); lv_obj_set_height(s_seconds, 160); lv_obj_align(s_seconds, LV_ALIGN_CENTER, 65, -20);
    s_arc = lv_arc_create(s_screen); lv_obj_set_size(s_arc, 330, 330); lv_obj_center(s_arc);
    lv_obj_move_to_index(s_arc, 0);
    lv_obj_clear_flag(s_arc, LV_OBJ_FLAG_CLICKABLE);
    s_info = lv_label_create(s_screen); lv_obj_set_style_text_color(s_info, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_info, &lv_font_montserrat_30, 0); lv_obj_align(s_info, LV_ALIGN_CENTER, 0, 95);
    s_start = lv_button_create(s_screen); lv_obj_set_size(s_start, 130, 48); lv_obj_align(s_start, LV_ALIGN_BOTTOM_MID, 0, -24);
    lv_obj_set_style_text_font(s_start, &ui_font_font_30_menu_cn, 0);
    lv_obj_t *label = lv_label_create(s_start);
    lv_obj_set_style_text_font(label, &ui_font_font_30_menu_cn, 0);
    lv_label_set_text(label, "开始"); lv_obj_center(label); s_start_label = label;
    lv_obj_add_event_cb(s_start, start_event, LV_EVENT_CLICKED, NULL);
    s_remaining = 60; lv_arc_set_range(s_arc, 0, 60); update_view();
    return s_screen;
}
static void on_open(void) { if (!s_timer) { s_remaining = 60; update_view(); } }
static void on_close(void) { if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; } }
static const xiaowei_app_t s_descriptor = {
    .id = "timer", .title = "计时器", .icon = &xiaowei_asset_icon_timer,
    .create_screen = create_screen, .on_open = on_open, .on_close = on_close,
    .destroy_screen = NULL
};
const xiaowei_app_t *timer_app_descriptor(void) { return &s_descriptor; }
