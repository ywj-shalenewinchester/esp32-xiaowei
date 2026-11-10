#include "artifact_app.h"
#include "../../framework/app_manager.h"
#include <stdio.h>

static lv_obj_t *s_screen, *s_title, *s_status, *s_spinner;
static char s_title_text[192], s_status_text[256];
static bool s_busy;
static const lv_font_t *s_font;

static void back_event(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_GESTURE &&
        lv_indev_get_gesture_dir(lv_indev_active()) == LV_DIR_RIGHT) xiaowei_app_back();
}
static void refresh(void) {
    if (!s_screen) return;
    lv_label_set_text(s_title, s_title_text);
    lv_label_set_text(s_status, s_status_text);
    if (s_font) lv_obj_set_style_text_font(s_screen, s_font, 0);
    if (s_busy) lv_obj_remove_flag(s_spinner, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_spinner, LV_OBJ_FLAG_HIDDEN);
}
static lv_obj_t *create(void) {
    if (s_screen) { refresh(); return s_screen; }
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(0x101827), 0);
    lv_obj_set_style_text_color(s_screen, lv_color_white(), 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_screen, back_event, LV_EVENT_GESTURE, NULL);
    int width = lv_display_get_horizontal_resolution(NULL);
    s_title = lv_label_create(s_screen);
    lv_obj_set_width(s_title, width * 65 / 100);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_title, LV_ALIGN_CENTER, 0, -100);
    s_spinner = lv_spinner_create(s_screen);
    lv_obj_set_size(s_spinner, 64, 64);
    lv_spinner_set_anim_params(s_spinner, 1200, 120);
    lv_obj_align(s_spinner, LV_ALIGN_CENTER, 0, -10);
    s_status = lv_label_create(s_screen);
    lv_obj_set_width(s_status, width * 70 / 100);
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_status, LV_ALIGN_CENTER, 0, 80);
    refresh();
    return s_screen;
}
static void close_page(void) {
    if (s_spinner) lv_obj_add_flag(s_spinner, LV_OBJ_FLAG_HIDDEN);
}
void artifact_app_update(const char *title, const char *text, bool busy, const lv_font_t *font) {
    snprintf(s_title_text, sizeof(s_title_text), "%s", title ? title : "");
    snprintf(s_status_text, sizeof(s_status_text), "%s", text ? text : "");
    s_busy = busy;
    s_font = font;
    refresh();
}
static const xiaowei_app_t descriptor = {
    .id = "artifact", .title = "研究与绘画", .icon = NULL,
    .create_screen = create, .on_open = NULL, .on_close = close_page, .destroy_screen = NULL,
};
const xiaowei_app_t *artifact_app_descriptor(void) { return &descriptor; }
