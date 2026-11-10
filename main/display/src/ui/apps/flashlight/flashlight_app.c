#include "flashlight_app.h"
#include "../../shell/app_list_screen.h"
#include "../../framework/app_manager.h"
#include "../../platform/xiaowei_ui_port.h"
#include "../../assets/xiaowei_assets.h"

static lv_obj_t *s_screen;
static int s_previous_brightness;

static void screen_event(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_GESTURE) return;
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    if (dir == LV_DIR_RIGHT) {
        xiaowei_app_back();
    } else if (dir == LV_DIR_TOP) {
        xiaowei_ui_set_brightness(xiaowei_ui_get_brightness() + 10);
    } else if (dir == LV_DIR_BOTTOM) {
        xiaowei_ui_set_brightness(xiaowei_ui_get_brightness() - 10);
    }
}

static lv_obj_t *create_screen(void) {
    s_screen = lv_obj_create(NULL);
    lv_obj_set_size(s_screen, 466, 466);
    lv_obj_set_style_bg_color(s_screen, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_screen, screen_event, LV_EVENT_GESTURE, NULL);
    lv_obj_t *slider = lv_slider_create(s_screen);
    lv_obj_set_size(slider, 466, 466);
    lv_obj_center(slider);
    lv_slider_set_range(slider, 0, 100);
    lv_obj_set_style_bg_opa(slider, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(slider, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_add_event_cb(slider, screen_event, LV_EVENT_GESTURE, NULL);
    return s_screen;
}

static void on_open(void) {
    s_previous_brightness = xiaowei_ui_get_brightness();
    xiaowei_ui_set_brightness(100);
}
static void on_close(void) { xiaowei_ui_set_brightness(s_previous_brightness); }
static const xiaowei_app_t s_descriptor = {
    .id = "flashlight", .title = "手电筒", .icon = &xiaowei_asset_icon_flashlight,
    .create_screen = create_screen, .on_open = on_open, .on_close = on_close,
    .destroy_screen = NULL
};
const xiaowei_app_t *flashlight_app_descriptor(void) { return &s_descriptor; }
