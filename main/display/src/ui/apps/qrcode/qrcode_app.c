#include "qrcode_app.h"
#include "../../framework/app_manager.h"
#include <stdio.h>
#include <string.h>

static lv_obj_t *s_screen, *s_code, *s_title;
static const char *s_default_url = "http://47.108.216.163:8002/?#/home";
static char s_url[513];
static bool s_download;
static const lv_font_t *s_font;

static void refresh(void) {
    if (!s_screen) return;
    lv_qrcode_set_data(s_code, s_download ? s_url : s_default_url);
    lv_label_set_text(s_title, s_download ? "扫码下载" : "");
    if (s_font) lv_obj_set_style_text_font(s_title, s_font, 0);
}
static void qrcode_event(lv_event_t *event) {
    if (lv_event_get_code(event) == LV_EVENT_GESTURE &&
        lv_indev_get_gesture_dir(lv_indev_active()) == LV_DIR_RIGHT) xiaowei_app_back();
}
static lv_obj_t *qrcode_create(void) {
    if (s_screen) { refresh(); return s_screen; }
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_screen, qrcode_event, LV_EVENT_GESTURE, NULL);
    int width = lv_display_get_horizontal_resolution(NULL);
    int height = lv_display_get_vertical_resolution(NULL);
    int side = (width < height ? width : height) * 60 / 100;
    s_code = lv_qrcode_create(s_screen);
    lv_qrcode_set_size(s_code, side);
    lv_qrcode_set_dark_color(s_code, lv_color_black());
    lv_qrcode_set_light_color(s_code, lv_color_white());
    lv_qrcode_set_quiet_zone(s_code, true);
    lv_obj_align(s_code, LV_ALIGN_CENTER, 0, 0);
    s_title = lv_label_create(s_screen);
    lv_obj_set_style_text_color(s_title, lv_color_white(), 0);
    lv_obj_align(s_title, LV_ALIGN_CENTER, 0, -side / 2 - 28);
    refresh();
    return s_screen;
}
static void qrcode_close(void) { s_download = false; }
bool qrcode_app_show_download(const char *url, const lv_font_t *font) {
    if (!url || strlen(url) > 512 ||
        (strncmp(url, "https://", 8) && strncmp(url, "http://", 7))) return false;
    snprintf(s_url, sizeof(s_url), "%s", url);
    s_font = font;
    /* Opening the same app would call on_close, resetting download mode. */
    if (lv_screen_active() == s_screen) {
        s_download = true;
        refresh();
        return true;
    }
    s_download = true;
    return xiaowei_app_open("qrcode");
}
bool qrcode_app_download_visible(void) {
    return s_download && s_screen && lv_screen_active() == s_screen;
}
static const xiaowei_app_t s_descriptor = {
    .id = "qrcode", .title = "二维码", .icon = NULL,
    .create_screen = qrcode_create, .on_open = NULL,
    .on_close = qrcode_close, .destroy_screen = NULL,
};
const xiaowei_app_t *qrcode_app_descriptor(void) { return &s_descriptor; }
