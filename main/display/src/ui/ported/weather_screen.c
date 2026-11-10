/*
 * weather_screen.c
 * 天气界面（移植自 Chronos esp32-c3-mini 的 ui.c 天气屏，MIT License）
 *
 * 原版设计分辨率即 466x466（Waveshare S3 1.43 默认环境），此处按 466 圆屏
 * 重新编排了间距与列表项尺寸。
 *
 * 数据不是内置 mock，而是服务端连接后下发的真实天气 —— 见 .h 顶部说明。
 * 页面只显示英文和数字，也不放天气图标（原因见 .h）。
 *
 * 手势：实时天气 左滑=7 天预报，上滑=逐时预报；
 *       两个列表下滑/右滑回实时天气；实时天气下滑/右滑返回上一屏。
 *
 * 与 lcd 工程的唯一差异：LVGL v8 的 lv_obj_set_hidden() / lv_obj_is_hidden()
 * 在 v9 里已经删掉（本工程是 9.5.0，头文件里没有兼容别名，用就是 implicit
 * declaration 报错），换成 v9 的 flag 接口，语义一一对应：
 *     lv_obj_set_hidden(o, true)  -> lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN)
 *     lv_obj_set_hidden(o, false) -> lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN)
 *     lv_obj_is_hidden(o)         -> lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)
 * 全文件共 14 处（simon_game.c 另 10 处同理）。
 */
#include "weather_screen.h"

#include "../framework/app_manager.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include <string.h>

static const char *TAG = "WEATHER";

static bool weather_obj_ok(const lv_obj_t *obj, const char *what)
{
    if (obj != NULL) return true;
    ESP_LOGE(TAG, "LVGL allocation failed while creating %s (free=%u, largest=%u)",
             what,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    return false;
}

/* ---------------- 服务端下发的数据 ---------------- */

/* 屏幕是**单例**（legacy_app.c 里这个 app 的 destroy_screen = NULL，建一次用到关机），
   所以数据也存一份静态的：服务端多半在用户点开天气之前就发过来了，那时屏幕还没建，
   只能先记下来，建屏时补填。 */
static weather_data_t s_data;
static bool s_has_data = false;

/* ---------------- 视图对象 ---------------- */
static lv_obj_t *s_screen;
static lv_obj_t *s_panel;        /* 实时天气 */
static lv_obj_t *s_city_label;
static lv_obj_t *s_cur_cond;     /* 英文天气词（替代了原来的大图标） */
static lv_obj_t *s_cur_temp;
static lv_obj_t *s_detail_label; /* Feels xxC   Humidity xx% */
static lv_obj_t *s_wind_label;   /* Wind xx km/h */
static lv_obj_t *s_update_label;
static lv_obj_t *s_forecast_list;
static lv_obj_t *s_hourly_list;

/* ---------------- 小工具 ---------------- */

/* 缺值一律渲染成 "--"：0 度和"服务端没给"是两回事，不能混。 */
static void fmt_value(char *buf, size_t size, const char *fmt, int value)
{
    if (value == WEATHER_NO_VALUE) {
        lv_snprintf(buf, size, "--");
    } else {
        lv_snprintf(buf, size, fmt, value);
    }
}

/* 城市名是中文时本页渲染不出来（只有 Montserrat，没有中文字形），
   这种情况退回 "LOCAL" —— 总比显示一排方框强。 */
static bool is_ascii(const char *s)
{
    for (; *s != '\0'; s++) {
        if ((unsigned char)*s >= 0x80) return false;
    }
    return true;
}

/* ---------------- 列表项 ---------------- */

/* 预报行：340x76。左侧上"星期"下"天气词"，右侧"最低/最高"。
   天气词放第二行而不是跟星期挤一行：像 "Moderate to Heavy Rain" 这种长度，
   和右边的温度挤一行必然重叠（LVGL 的 label 不会给兄弟节点让位）。 */
static void add_forecast_item(const weather_daily_t *d)
{
    lv_obj_t *item = lv_obj_create(s_forecast_list);
    if (!weather_obj_ok(item, "forecast item")) return;
    lv_obj_set_size(item, 340, 76);
    lv_obj_clear_flag(item, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(item, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_style_radius(item, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(item, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(item, 1, LV_PART_MAIN);
    lv_obj_set_style_border_side(item, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_border_color(item, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_pad_all(item, 0, LV_PART_MAIN);

    lv_obj_t *day_lab = lv_label_create(item);
    if (!weather_obj_ok(day_lab, "forecast day label")) return;
    lv_obj_align(day_lab, LV_ALIGN_TOP_LEFT, 14, 10);
    lv_label_set_text(day_lab, d->day[0] != '\0' ? d->day : "---");
    lv_obj_set_style_text_font(day_lab, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(day_lab, lv_color_white(), LV_PART_MAIN);

    lv_obj_t *cond_lab = lv_label_create(item);
    if (!weather_obj_ok(cond_lab, "forecast condition label")) return;
    lv_obj_align(cond_lab, LV_ALIGN_BOTTOM_LEFT, 14, -10);
    lv_label_set_text(cond_lab, d->cond[0] != '\0' ? d->cond : "--");
    lv_obj_set_style_text_font(cond_lab, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(cond_lab, lv_color_hex(0xB0B0B0), LV_PART_MAIN);

    char tmin[16], tmax[16], range[40];
    fmt_value(tmin, sizeof(tmin), "%d", d->tmin);
    fmt_value(tmax, sizeof(tmax), "%d", d->tmax);
    lv_snprintf(range, sizeof(range), "%s/%sC", tmin, tmax);

    lv_obj_t *temp_lab = lv_label_create(item);
    if (!weather_obj_ok(temp_lab, "forecast temperature label")) return;
    lv_obj_align(temp_lab, LV_ALIGN_RIGHT_MID, -14, 0);
    lv_label_set_text(temp_lab, range);
    lv_obj_set_style_text_font(temp_lab, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(temp_lab, lv_color_white(), LV_PART_MAIN);
}

/* 逐时卡片：170x300，竖排 时间 / 天气词 / 温度 */
static void add_hourly_item(const weather_hourly_t *h)
{
    lv_obj_t *item = lv_obj_create(s_hourly_list);
    if (!weather_obj_ok(item, "hourly item")) return;
    lv_obj_set_size(item, 170, 300);
    lv_obj_set_flex_flow(item, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(item, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(item, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(item, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_style_bg_color(item, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(item, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(item, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(item, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(item, 18, LV_PART_MAIN);

    lv_obj_t *time_lab = lv_label_create(item);
    if (!weather_obj_ok(time_lab, "hourly time label")) return;
    lv_label_set_text(time_lab, h->time[0] != '\0' ? h->time : "--:--");
    lv_obj_set_style_text_font(time_lab, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(time_lab, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_align(time_lab, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_border_width(time_lab, 1, LV_PART_MAIN);
    lv_obj_set_style_border_side(time_lab, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_border_color(time_lab, lv_color_white(), LV_PART_MAIN);

    lv_obj_t *cond_lab = lv_label_create(item);
    if (!weather_obj_ok(cond_lab, "hourly condition label")) return;
    /* 天气词最长的有 20 来个字符，比卡片窄，必须允许折行 —— 否则会被父对象裁掉半截 */
    lv_obj_set_width(cond_lab, 150);
    lv_label_set_long_mode(cond_lab, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(cond_lab, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(cond_lab, h->cond[0] != '\0' ? h->cond : "--");
    lv_obj_set_style_text_font(cond_lab, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(cond_lab, lv_color_hex(0xD0D0D0), LV_PART_MAIN);

    char temp[16];
    fmt_value(temp, sizeof(temp), "%dC", h->temp);
    lv_obj_t *temp_lab = lv_label_create(item);
    if (!weather_obj_ok(temp_lab, "hourly temperature label")) return;
    lv_label_set_text(temp_lab, temp);
    lv_obj_set_style_text_font(temp_lab, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_set_style_text_color(temp_lab, lv_color_white(), LV_PART_MAIN);
}

/* 一格占位的提示行（服务端还没发天气时用） */
static void add_placeholder(lv_obj_t *list, const char *text)
{
    lv_obj_t *lab = lv_label_create(list);
    if (!weather_obj_ok(lab, "placeholder label")) return;
    lv_label_set_text(lab, text);
    lv_obj_set_style_text_font(lab, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(lab, lv_color_hex(0x808080), LV_PART_MAIN);
    lv_obj_set_style_pad_top(lab, 30, LV_PART_MAIN);
}

/* ---------------- 数据填充 ---------------- */

/* 两个列表的内容会随数据条数变化（服务端给几天就几行），所以整段重建：
   lv_obj_clean() 连标题一起删掉，标题在这里重新建。 */
static void rebuild_forecast(void)
{
    if (s_forecast_list == NULL) return;
    lv_obj_clean(s_forecast_list);

    const int count = s_has_data ? s_data.daily_count : 0;
    char title[24];
    if (count > 0) {
        lv_snprintf(title, sizeof(title), "%d-DAY FORECAST", count);
    } else {
        lv_snprintf(title, sizeof(title), "FORECAST");
    }

    lv_obj_t *fc_title = lv_label_create(s_forecast_list);
    if (!weather_obj_ok(fc_title, "forecast title")) return;
    lv_label_set_text(fc_title, title);
    lv_obj_set_style_text_font(fc_title, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(fc_title, lv_color_hex(0x90C0FF), LV_PART_MAIN);

    if (count <= 0) {
        add_placeholder(s_forecast_list, "No forecast data");
        return;
    }
    for (int i = 0; i < count; i++) {
        add_forecast_item(&s_data.daily[i]);
    }
}

static void rebuild_hourly(void)
{
    if (s_hourly_list == NULL) return;
    lv_obj_clean(s_hourly_list);

    const int count = s_has_data ? s_data.hourly_count : 0;
    if (count <= 0) {
        add_placeholder(s_hourly_list, "No hourly data");
        return;
    }
    for (int i = 0; i < count; i++) {
        add_hourly_item(&s_data.hourly[i]);
    }
}

static void weather_fill(void)
{
    char buf[48];
    const weather_now_t *now = s_has_data ? &s_data.now : NULL;

    /* 城市：中文渲染不出来，退回 LOCAL */
    if (s_has_data && s_data.city[0] != '\0' && is_ascii(s_data.city)) {
        lv_label_set_text(s_city_label, s_data.city);
    } else {
        lv_label_set_text(s_city_label, "LOCAL");
    }

    lv_label_set_text(s_cur_cond,
                      (now != NULL && now->cond[0] != '\0') ? now->cond : "--");

    fmt_value(buf, sizeof(buf), "%dC", now != NULL ? now->temp : WEATHER_NO_VALUE);
    lv_label_set_text(s_cur_temp, buf);

    char feels[16], humidity[16];
    fmt_value(feels, sizeof(feels), "%dC", now != NULL ? now->feels : WEATHER_NO_VALUE);
    fmt_value(humidity, sizeof(humidity), "%d%%",
              now != NULL ? now->humidity : WEATHER_NO_VALUE);
    lv_label_set_text_fmt(s_detail_label, "Feels %s   Humidity %s", feels, humidity);

    fmt_value(buf, sizeof(buf), "%d km/h", now != NULL ? now->wind : WEATHER_NO_VALUE);
    lv_label_set_text_fmt(s_wind_label, "Wind %s", buf);

    if (s_has_data && s_data.update[0] != '\0') {
        lv_label_set_text_fmt(s_update_label, "Updated %s", s_data.update);
    } else {
        lv_label_set_text(s_update_label, s_has_data ? "Updated --" : "Waiting for data");
    }

    rebuild_forecast();
    rebuild_hourly();
}

void weather_screen_apply(const weather_data_t *data)
{
    if (data == NULL) return;

    s_data = *data;
    /* 条数兜一下：报文再怎么不对也不能让下面 for 循环越界读 */
    if (s_data.daily_count < 0) s_data.daily_count = 0;
    if (s_data.daily_count > WEATHER_DAILY_MAX) s_data.daily_count = WEATHER_DAILY_MAX;
    if (s_data.hourly_count < 0) s_data.hourly_count = 0;
    if (s_data.hourly_count > WEATHER_HOURLY_MAX) s_data.hourly_count = WEATHER_HOURLY_MAX;
    s_has_data = true;

    ESP_LOGI(TAG, "weather data applied: %s %s %dC (%d days / %d hours)",
             s_data.city, s_data.now.cond, s_data.now.temp, s_data.daily_count,
             s_data.hourly_count);

    /* 屏幕还没建（用户还没点开过天气）就只存着，建屏时会补填 */
    if (s_screen != NULL) {
        weather_fill();
    }
}

/* ---------------- 三视图切换 ---------------- */
static void show_current(void)
{
    lv_obj_remove_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_forecast_list, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_hourly_list, LV_OBJ_FLAG_HIDDEN);
}

static void show_forecast(void)
{
    lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_forecast_list, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_hourly_list, LV_OBJ_FLAG_HIDDEN);
}

static void show_hourly(void)
{
    lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_forecast_list, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_hourly_list, LV_OBJ_FLAG_HIDDEN);
}

static void screen_event(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_GESTURE) return;
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());

    bool hourly_visible = !lv_obj_has_flag(s_hourly_list, LV_OBJ_FLAG_HIDDEN);
    bool forecast_visible = !lv_obj_has_flag(s_forecast_list, LV_OBJ_FLAG_HIDDEN);

    /* 右滑和下滑都是"返回"：在列表页回到实时页，在实时页退出到上一屏。
       两个方向都认 —— 圆屏上单手操作时下滑比右滑顺手。
       （列表页里的下滑会被列表自己的滚动吃掉，收不到这个事件，属正常。） */
    if (dir == LV_DIR_RIGHT || dir == LV_DIR_BOTTOM) {
        if (hourly_visible || forecast_visible) {
            show_current();
        } else {
            xiaowei_app_back();
        }
    } else if (dir == LV_DIR_LEFT && !hourly_visible && !forecast_visible) {
        show_forecast();
    } else if (dir == LV_DIR_TOP && !hourly_visible && !forecast_visible) {
        show_hourly();
    }
}

/* ---------------- 屏幕构建 ---------------- */
void weather_screen_open(lv_event_t *e)
{
    (void)e;
    if (!s_screen) {
        s_screen = lv_obj_create(NULL);
        if (!weather_obj_ok(s_screen, "weather screen")) return;
        lv_obj_set_size(s_screen, 466, 466);
        lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(s_screen, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_add_event_cb(s_screen, screen_event, LV_EVENT_GESTURE, NULL);

        /* ---- 实时天气面板 ----
         * 注意：remove_style_all 会清掉尺寸等 local style，
         * 所以尺寸/对齐必须在它之后设置，否则面板塌缩成内容大小。 */
        s_panel = lv_obj_create(s_screen);
        if (!weather_obj_ok(s_panel, "current weather panel")) return;
        lv_obj_remove_style_all(s_panel);
        lv_obj_set_size(s_panel, 466, 466);
        lv_obj_center(s_panel);
        lv_obj_add_flag(s_panel, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_opa(s_panel, LV_OPA_TRANSP, LV_PART_MAIN);

        s_city_label = lv_label_create(s_panel);
        if (!weather_obj_ok(s_city_label, "city label")) return;
        lv_obj_align(s_city_label, LV_ALIGN_CENTER, 0, -170);
        lv_label_set_text(s_city_label, "LOCAL");
        lv_obj_set_style_text_font(s_city_label, &lv_font_montserrat_20, LV_PART_MAIN);
        lv_obj_set_style_text_color(s_city_label, lv_color_white(), LV_PART_MAIN);

        /* 天气词占着原版大图标的位置 */
        s_cur_cond = lv_label_create(s_panel);
        if (!weather_obj_ok(s_cur_cond, "current condition label")) return;
        lv_obj_align(s_cur_cond, LV_ALIGN_CENTER, 0, -80);
        lv_label_set_text(s_cur_cond, "--");
        lv_obj_set_style_text_font(s_cur_cond, &lv_font_montserrat_24, LV_PART_MAIN);
        lv_obj_set_style_text_color(s_cur_cond, lv_color_white(), LV_PART_MAIN);

        s_cur_temp = lv_label_create(s_panel);
        if (!weather_obj_ok(s_cur_temp, "current temperature label")) return;
        lv_obj_align(s_cur_temp, LV_ALIGN_CENTER, 0, 5);
        lv_label_set_text(s_cur_temp, "--C");
        lv_obj_set_style_text_font(s_cur_temp, &lv_font_montserrat_46, LV_PART_MAIN);
        lv_obj_set_style_text_color(s_cur_temp, lv_color_white(), LV_PART_MAIN);

        s_detail_label = lv_label_create(s_panel);
        if (!weather_obj_ok(s_detail_label, "detail label")) return;
        lv_obj_align(s_detail_label, LV_ALIGN_CENTER, 0, 78);
        lv_label_set_text(s_detail_label, "Feels --   Humidity --");
        lv_obj_set_style_text_font(s_detail_label, &lv_font_montserrat_16, LV_PART_MAIN);
        lv_obj_set_style_text_color(s_detail_label, lv_color_hex(0xD0D0D0), LV_PART_MAIN);

        s_wind_label = lv_label_create(s_panel);
        if (!weather_obj_ok(s_wind_label, "wind label")) return;
        lv_obj_align(s_wind_label, LV_ALIGN_CENTER, 0, 108);
        lv_label_set_text(s_wind_label, "Wind --");
        lv_obj_set_style_text_font(s_wind_label, &lv_font_montserrat_16, LV_PART_MAIN);
        lv_obj_set_style_text_color(s_wind_label, lv_color_hex(0xD0D0D0), LV_PART_MAIN);

        s_update_label = lv_label_create(s_panel);
        if (!weather_obj_ok(s_update_label, "update label")) return;
        lv_obj_align(s_update_label, LV_ALIGN_CENTER, 0, 155);
        lv_label_set_text(s_update_label, "Waiting for data");
        lv_obj_set_style_text_align(s_update_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_style_text_font(s_update_label, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(s_update_label, lv_color_hex(0xA0A0A0), LV_PART_MAIN);

        lv_obj_t *hint = lv_label_create(s_panel);
        if (!weather_obj_ok(hint, "gesture hint label")) return;
        lv_obj_align(hint, LV_ALIGN_CENTER, 0, 200);
        lv_label_set_text(hint, "< 7-Day      Hourly ^");
        lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(hint, lv_color_hex(0x707070), LV_PART_MAIN);

        /* ---- 7 天预报（垂直滚动） ---- */
        s_forecast_list = lv_obj_create(s_screen);
        if (!weather_obj_ok(s_forecast_list, "forecast list")) return;
        /* remove_style_all 必须在 flex/scroll 等样式设置之前，否则会被清空 */
        lv_obj_remove_style_all(s_forecast_list);
        lv_obj_set_size(s_forecast_list, lv_pct(100), lv_pct(100));
        lv_obj_align(s_forecast_list, LV_ALIGN_TOP_MID, 0, 0);
        lv_obj_set_flex_flow(s_forecast_list, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(s_forecast_list, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_flag(s_forecast_list, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_forecast_list, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_set_scrollbar_mode(s_forecast_list, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_scroll_dir(s_forecast_list, LV_DIR_VER);
        lv_obj_set_style_pad_top(s_forecast_list, 70, LV_PART_MAIN);
        lv_obj_set_style_pad_bottom(s_forecast_list, 140, LV_PART_MAIN);

        /* ---- 逐时预报（水平滚动） ---- */
        s_hourly_list = lv_obj_create(s_screen);
        if (!weather_obj_ok(s_hourly_list, "hourly list")) return;
        /* remove_style_all 必须在 flex/scroll 等样式设置之前，否则会被清空 */
        lv_obj_remove_style_all(s_hourly_list);
        lv_obj_set_size(s_hourly_list, lv_pct(100), lv_pct(100));
        lv_obj_center(s_hourly_list);
        lv_obj_set_flex_flow(s_hourly_list, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(s_hourly_list, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_flag(s_hourly_list, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_hourly_list, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_set_scrollbar_mode(s_hourly_list, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_scroll_dir(s_hourly_list, LV_DIR_HOR);
        lv_obj_set_style_bg_opa(s_hourly_list, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_pad_left(s_hourly_list, 90, LV_PART_MAIN);
        lv_obj_set_style_pad_right(s_hourly_list, 120, LV_PART_MAIN);

        ESP_LOGI(TAG, "weather screen created");
    }

    /* 有服务端数据就填真实的，没有就留给占位文案（建屏后数据到了会再填一次） */
    weather_fill();

    show_current();
    lv_scr_load_anim(s_screen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 220, 0, false);
}

lv_obj_t *weather_screen_create_for_app(void)
{
    weather_screen_open(NULL);
    return s_screen;
}
