#include "calendar_app.h"
#include "../../framework/app_manager.h"
#include "../../platform/xiaowei_ui_port.h"
#include "../../assets/xiaowei_assets.h"

static lv_obj_t *s_screen;
static lv_obj_t *s_calendar;

static void screen_event(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_GESTURE &&
        lv_indev_get_gesture_dir(lv_indev_active()) == LV_DIR_RIGHT) {
        xiaowei_app_back();
    }
}

static lv_obj_t *create_screen(void) {
    s_screen = lv_obj_create(NULL);
    lv_obj_set_size(s_screen, 466, 466);
    lv_obj_set_style_bg_color(s_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(s_screen, screen_event, LV_EVENT_GESTURE, NULL);
    s_calendar = lv_calendar_create(s_screen);
    lv_obj_set_size(s_calendar, 400, 360);
    lv_obj_center(s_calendar);
    lv_calendar_header_arrow_create(s_calendar);
    lv_obj_set_style_bg_color(s_calendar, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_calendar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_calendar, 0, LV_PART_MAIN);
    return s_screen;
}

static void on_open(void) {
    int y, m, d;
    xiaowei_ui_get_date(&y, &m, &d);
    lv_calendar_set_today_date(s_calendar, y, m, d);
    lv_calendar_set_showed_date(s_calendar, y, m);
}

static void on_close(void) { (void)s_calendar; }
static const xiaowei_app_t s_descriptor = {
    .id = "calendar", .title = "日历", .icon = &xiaowei_asset_icon_calendar,
    .create_screen = create_screen, .on_open = on_open, .on_close = on_close,
    .destroy_screen = NULL
};
const xiaowei_app_t *calendar_app_descriptor(void) { return &s_descriptor; }
