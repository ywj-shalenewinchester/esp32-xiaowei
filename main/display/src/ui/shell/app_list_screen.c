#include "app_list_screen.h"
#include "home_screen.h"
#include "standby_ring.h"
#include "../framework/app_registry.h"
#include "../framework/app_manager.h"
#include "../fonts.h"
#include <math.h>
#include <string.h>

#define MENU_MAX_ITEMS 8
#define MENU_STEP 108
#define MENU_ITEM_H 92
#define MENU_CENTER_Y 233
#define MENU_CENTER_X 205
#define MENU_ARC_R 250.0f
#define MENU_HIDE_AT 272.0f
#define MENU_MAX_OFF 0.45f

static lv_obj_t *s_menu_screen;
static lv_obj_t *s_menu_items[MENU_MAX_ITEMS];
static unsigned s_menu_count;
static float s_menu_pos;
static float s_menu_vel;
static lv_coord_t s_last_touch_y;
static uint32_t s_last_move_ms;
static bool s_menu_pressed;
static bool s_menu_dragged;
/* Keep enough width for the 50 px Chinese labels.  The narrower legacy
 * widths clipped the last glyphs of 工具/测试/设置. */
static const lv_coord_t s_legacy_w[] = {246, 246, 246, 246, 246, 246, 246, 246};
static const lv_coord_t s_legacy_label_x[] = {94, 99, 90, 90, 88, 92, 90, 90};

static float clampf_local(float value, float low, float high) {
    return value < low ? low : value > high ? high : value;
}

static void arrange_menu(void) {
    for (unsigned i = 0; i < s_menu_count; ++i) {
        lv_obj_t *item = s_menu_items[i];
        float dy = ((float)i - s_menu_pos) * MENU_STEP;
        if (dy < -MENU_HIDE_AT || dy > MENU_HIDE_AT) {
            lv_obj_add_flag(item, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(item, LV_OBJ_FLAG_HIDDEN);
        float curve = clampf_local(dy, -MENU_ARC_R, MENU_ARC_R);
        float arc = sqrtf(MENU_ARC_R * MENU_ARC_R - curve * curve);
        int x = MENU_CENTER_X - (int)lv_obj_get_width(item) / 2 +
                (int)((1.0f - arc / MENU_ARC_R) * 80.0f + 0.5f);
        int y = (int)(MENU_CENTER_Y - MENU_ITEM_H / 2.0f + dy + 0.5f);
        lv_obj_set_pos(item, x, y);
        lv_obj_set_style_opa(item,
                             (lv_opa_t)(255.0f - fabsf(dy) / MENU_HIDE_AT * 150.0f),
                             LV_PART_MAIN);
    }
}

static void menu_anim_cb(void *obj, int32_t value) {
    (void)obj;
    s_menu_pos = value / 1000.0f;
    arrange_menu();
}

static void menu_snap(void) {
    float target = s_menu_pos;
    if (s_menu_vel > 0.6f || s_menu_vel < -0.6f) target += s_menu_vel * 0.30f;
    target = roundf(target);
    target = clampf_local(target, 0.0f, (float)(s_menu_count ? s_menu_count - 1 : 0));
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, s_menu_screen);
    lv_anim_set_exec_cb(&anim, menu_anim_cb);
    lv_anim_set_values(&anim, (int32_t)(s_menu_pos * 1000.0f), (int32_t)(target * 1000.0f));
    lv_anim_set_duration(&anim, 240);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    lv_anim_start(&anim);
}

static void menu_event(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_SCREEN_LOAD_START || code == LV_EVENT_SCREEN_LOADED) {
        /* 首次进入和再次返回都走同一套弧形布局，避免首次显示沿用默认坐标。 */
        arrange_menu();
    } else if (code == LV_EVENT_PRESSED) {
        s_menu_pressed = true;
        s_menu_dragged = false;
        s_menu_vel = 0.0f;
        lv_anim_delete(s_menu_screen, menu_anim_cb);
        lv_point_t point;
        lv_indev_get_point(lv_indev_active(), &point);
        s_last_touch_y = point.y;
        s_last_move_ms = lv_tick_get();
    } else if (code == LV_EVENT_PRESSING && s_menu_pressed) {
        lv_point_t point;
        lv_indev_get_point(lv_indev_active(), &point);
        lv_coord_t dy = point.y - s_last_touch_y;
        if (!dy) return;
        s_menu_dragged = true;
        uint32_t now = lv_tick_get();
        uint32_t elapsed = now - s_last_move_ms;
        if (elapsed >= 8) {
            s_menu_vel = -(float)dy / MENU_STEP * 1000.0f / elapsed;
            s_last_move_ms = now;
        }
        s_last_touch_y = point.y;
        s_menu_pos = clampf_local(s_menu_pos - (float)dy / MENU_STEP,
                                  -MENU_MAX_OFF, (float)(s_menu_count - 1) + MENU_MAX_OFF);
        arrange_menu();
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (s_menu_pressed) {
            s_menu_pressed = false;
            menu_snap();
        }
    } else if (code == LV_EVENT_GESTURE &&
               lv_indev_get_gesture_dir(lv_indev_active()) == LV_DIR_RIGHT) {
        standby_ring_return_from_menu();
    }
}

static void item_event(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || s_menu_dragged) return;
    const xiaowei_app_t *app = (const xiaowei_app_t *)lv_event_get_user_data(e);
    if (app) xiaowei_app_open(app->id);
}

static bool is_root_menu_app(const xiaowei_app_t *app) {
    /* The registry also contains secondary pages (camera capture, settings
     * sliders, info pages, ...).  Only the eight legacy top-level entries are
     * rendered here; all other registered apps remain reachable by ID from
     * their parent app. */
    if (!app || !app->id) return false;
    static const char *const root_ids[] = {
        "ai", "camera", "weather", "music", "game", "tools", "test", "settings"
    };
    for (size_t i = 0; i < sizeof(root_ids) / sizeof(root_ids[0]); ++i)
        if (strcmp(app->id, root_ids[i]) == 0) return true;
    return false;
}

lv_obj_t *app_list_screen_create(void) {
    if (s_menu_screen) return s_menu_screen;
    s_menu_screen = lv_obj_create(NULL);
    lv_obj_set_size(s_menu_screen, 466, 466);
    lv_obj_set_style_bg_color(s_menu_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_menu_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_menu_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_menu_screen, menu_event, LV_EVENT_ALL, NULL);

    for (size_t i = 0; i < xiaowei_app_count() && s_menu_count < MENU_MAX_ITEMS; ++i) {
        const xiaowei_app_t *app = xiaowei_app_at(i);
        if (!is_root_menu_app(app)) continue;
        lv_obj_t *item = lv_button_create(s_menu_screen);
        lv_obj_set_size(item, s_menu_count < 8 ? s_legacy_w[s_menu_count] : 246, MENU_ITEM_H);
        lv_obj_set_style_bg_opa(item, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(item, 0, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(item, 0, LV_PART_MAIN);
        lv_obj_add_flag(item, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_event_cb(item, item_event, LV_EVENT_CLICKED, (void *)app);
        lv_obj_t *icon = lv_image_create(item);
        lv_image_set_src(icon, app->icon);
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_t *label = lv_label_create(item);
        lv_label_set_text(label, app->title);
        lv_obj_set_style_text_font(label, &ui_font_font_50_menu_cn, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(0xf9f5f5), 0);
        /* The old EEZ labels were LV_SIZE_CONTENT. A fixed width clipped the
           last glyph of "AI助手" (and made some Chinese labels look corrupt)
           once the backup font was restored. */
        lv_obj_set_width(label, LV_SIZE_CONTENT);
        lv_obj_set_height(label, LV_SIZE_CONTENT);
        lv_obj_set_style_text_font(label, &ui_font_font_50_menu_cn, 0);
        /* 与旧工程每个菜单项的图标-文字间距一致。 */
        lv_obj_align(label, LV_ALIGN_LEFT_MID,
                     s_menu_count < 8 ? s_legacy_label_x[s_menu_count] : 90, 0);
        s_menu_items[s_menu_count++] = item;
    }
    arrange_menu();
    return s_menu_screen;
}

void app_list_screen_show(void) {
    lv_obj_t *screen = app_list_screen_create();
    xiaowei_app_manager_set_shell_screen(screen);
    arrange_menu();
    /* 进主菜单一律来自**右滑**（standby_ring_show_menu / 应用返回栈为空时的
       xiaowei_app_back），所以方向固定 MOVE_RIGHT —— 新页从屏幕左边滑进来。
       时长必须走 app_manager 的统一旋钮：原来这里写死 220ms，和那个旋钮的 0
       打架，表现就是"进菜单有滑入动画、出菜单/别的切页是硬切"。 */
    xiaowei_screen_switch(screen, LV_SCREEN_LOAD_ANIM_MOVE_RIGHT);
}
