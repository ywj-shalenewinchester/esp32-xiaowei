#include "home_screen.h"
#include "standby_ring.h"
#include "../assets/xiaowei_assets.h"
#include "../framework/app_manager.h"
#include "../framework/quick_panel.h"
#include "../platform/xiaowei_ui_port.h"
#include "../fonts.h"
#include <time.h>

static lv_obj_t *s_home;
static lv_obj_t *s_wallpaper;
static lv_obj_t *s_info;
static lv_obj_t *s_date_label, *s_weekday_label, *s_time_label;
static unsigned s_wallpaper_index;
static const lv_image_dsc_t *const s_wallpapers[] = {
    &xiaowei_asset_wallpaper_1, &xiaowei_asset_wallpaper_2, &xiaowei_asset_wallpaper_3
};

/* 快捷面板已经搬到 framework/quick_panel.c 并挂到 lv_layer_top() 上 ——
   它原来是本屏的私有子对象，所以只有壁纸页拉得出来。现在待机环上的
   壁纸/聊天/表情三页共用同一个面板，这里的角色只剩下"转发开合手势"。

   注意本函数**只挂在本屏和壁纸图**上（见 home_screen_create），而手势投递
   规则是"发给最上层那个没设 LV_OBJ_FLAG_GESTURE_BUBBLE 的祖先"
   （indev/lv_indev.c:1773-1815）：本屏没有父对象，所以它就是终点，
   壁纸图、s_info 上的手势一律冒泡到这里。 */
static void home_event(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    /* 左右滑交给待机环（壁纸/聊天/表情 三页同级循环）。
       方向刻意与改动前保持同向：**右滑进应用主菜单、左滑切下一个模式**
       （改动前是右滑 app_list_screen_show()、左滑进聊天页 = 环的第一步）。
       下滑（快捷面板）现在三页都有，不是壁纸页独有的了；
       长按换壁纸则仍然只有壁纸页有，它是本页自己的行为。 */
    if (code == LV_EVENT_GESTURE && lv_indev_get_gesture_dir(lv_indev_active()) == LV_DIR_LEFT) {
        standby_ring_next();
    } else if (code == LV_EVENT_GESTURE && lv_indev_get_gesture_dir(lv_indev_active()) == LV_DIR_BOTTOM) {
        xiaowei_quick_panel_open();
        /* 上滑（收起）的分支不在这里：面板一打开就盖满整屏、成为 act_obj，
           LVGL 的输入搜索（indev/lv_indev.c:1654，sys -> top -> active screen
           -> bottom）到不了本屏，手势由面板自己接。 */
    } else if (code == LV_EVENT_GESTURE && lv_indev_get_gesture_dir(lv_indev_active()) == LV_DIR_RIGHT) {
        standby_ring_show_menu();
    } else if (code == LV_EVENT_LONG_PRESSED) {
        home_screen_next_wallpaper();
    }
}

static void clock_update(lv_timer_t *timer) {
    (void)timer;
    int year, month, day, hour, minute;
    xiaowei_ui_get_date(&year, &month, &day);
    xiaowei_ui_get_time(&hour, &minute);
    static const char *const weekdays[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
    time_t now = time(NULL);
    struct tm local = {0};
    localtime_r(&now, &local);
    lv_label_set_text_fmt(s_date_label, "%02d-%02d", month, day);
    lv_label_set_text(s_weekday_label, weekdays[local.tm_wday]);
    lv_label_set_text_fmt(s_time_label, "%02d:%02d", hour, minute);
}

lv_obj_t *home_screen_create(void) {
    if (s_home) return s_home;
    s_home = lv_obj_create(NULL);
    lv_obj_set_size(s_home, 466, 466);
    lv_obj_set_style_bg_color(s_home, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_home, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_home, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_home, home_event, LV_EVENT_ALL, NULL);

    s_wallpaper = lv_image_create(s_home);
    lv_obj_set_size(s_wallpaper, 466, 466);
    lv_obj_center(s_wallpaper);
    lv_image_set_src(s_wallpaper, s_wallpapers[s_wallpaper_index]);
    lv_obj_add_flag(s_wallpaper, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_add_event_cb(s_wallpaper, home_event, LV_EVENT_ALL, NULL);

    /* 备份首页的日期/时间是一个位于 (113,103) 的组合容器，内部坐标不能
       直接当成屏幕坐标使用，否则会在圆屏上整体偏移。 */
    s_info = lv_obj_create(s_home);
    lv_obj_set_size(s_info, 250, 105);
    lv_obj_set_pos(s_info, 113, 103);
    lv_obj_set_style_bg_opa(s_info, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_info, 0, 0);
    lv_obj_set_style_pad_all(s_info, 0, 0);
    lv_obj_clear_flag(s_info, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_info, LV_OBJ_FLAG_EVENT_BUBBLE);

    s_date_label = lv_label_create(s_info);
    lv_obj_set_style_text_font(s_date_label, &ui_font_font_40_cn, 0);
    lv_obj_set_style_text_color(s_date_label, lv_color_hex(0xd68e8e), 0);
    lv_obj_set_pos(s_date_label, 0, 0);
    s_weekday_label = lv_label_create(s_info);
    lv_obj_set_style_text_font(s_weekday_label, &ui_font_font_40_cn, 0);
    lv_obj_set_style_text_color(s_weekday_label, lv_color_hex(0xd68e8e), 0);
    lv_obj_set_pos(s_weekday_label, 134, 0);
    s_time_label = lv_label_create(s_info);
    lv_obj_set_style_text_font(s_time_label, &lv_font_montserrat_46, 0);
    lv_obj_set_style_text_color(s_time_label, lv_color_hex(0xd68e8e), 0);
    lv_obj_set_pos(s_time_label, 0, 50);
    clock_update(NULL);
    lv_timer_create(clock_update, 1000, NULL);

    /* 让面板在本屏被切走时自动收起。面板挂在顶层，切屏带不走它；
       而语音唤起会自动切到聊天页，那时面板若还开着就会赖在聊天页上。

       挂在屏幕对象**自己**身上，不能像 fps_overlay 那样挂 display 的
       LV_EVENT_SCREEN_UNLOAD_START：引擎只在**立即切换**路径发屏幕级事件
       （core/lv_display.c:1445-1474）。本工程所有换屏都走
       xiaowei_screen_switch()，而它的两条分支取决于 app_manager.c 的
       SCREEN_LOAD_ANIM_MS —— 哪天把它设回 0，display 级钩子就会静默失效，
       而且不会有任何报错。挂在屏幕对象上则两条路径都会发（动画路径
       core/lv_display.c:924，在起动画之前；立即路径 :1448），
       不必跟着那个旋钮一起改。 */
    xiaowei_quick_panel_watch_screen(s_home);
    return s_home;
}

void home_screen_show(void) {
    lv_obj_t *screen = home_screen_create();
    xiaowei_app_manager_set_shell_screen(screen);
    /* 开机第一屏，没有"从哪来"的方向可言，沿用进/出菜单的 MOVE_RIGHT。
       时长走 app_manager 的统一旋钮（原来这里写死 220ms，和那个旋钮打架）。 */
    xiaowei_screen_switch(screen, LV_SCREEN_LOAD_ANIM_MOVE_RIGHT);
}

lv_obj_t *home_screen_get(void) { return s_home; }

void home_screen_next_wallpaper(void) {
    s_wallpaper_index = (s_wallpaper_index + 1) % 3;
    if (s_wallpaper) lv_image_set_src(s_wallpaper, s_wallpapers[s_wallpaper_index]);
}
