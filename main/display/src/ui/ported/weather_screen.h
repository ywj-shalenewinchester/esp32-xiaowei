/*
 * weather_screen.h
 * 天气界面（移植自 Chronos esp32-c3-mini 的 ui.c 天气屏，MIT License）
 *
 * 三个视图（手势切换）：
 *   实时天气（默认）  城市 / 天气词 / 当前温度 / 体感·湿度·风速 / 更新时间
 *   7 天预报（左滑进入，垂直滚动）
 *   逐时预报（上滑进入，水平滚动）
 * 实时页下滑或右滑返回上一屏；两个列表里下滑/右滑回到实时页。
 *
 * ── 全页只有英文和数字，也不放天气图标 ──
 * 本页只装了 Montserrat，**没有中文字形**；内置天气图标只有 8 张，覆盖不了和风几十个
 * icon 码值。所以服务端专门为设备裁了一份报文：icon 码在服务端翻成英文天气词
 * （my-server 的 get_weather.build_device_payload），设备这边不做任何映射，
 * 拿到什么显示什么。
 *
 * ── 数据从哪来 ──
 * 设备连接建立后，服务端主动下发一条 {"type":"weather", "city":..., "now":{...},
 * "daily":[...], "hourly":[...]}；XiaoweiUiDisplay::DisplayWeather() 解析成下面的
 * weather_data_t，再调 weather_screen_apply()。
 * 报文通常比"用户点开天气"到得早，所以 apply 时屏幕多半还没建 —— 数据先存着，
 * 建屏时补填（也就没有内置 demo 数据了）。
 */
#ifndef PORTED_WEATHER_SCREEN_H
#define PORTED_WEATHER_SCREEN_H

#include "lvgl/lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 天气词/城市名的缓冲长度。"Moderate to Heavy Rain" 这类最长 21 字符 + 结尾。 */
#define WEATHER_TEXT_MAX 24
#define WEATHER_DAILY_MAX 7
#define WEATHER_HOURLY_MAX 24

/* 服务端没给这个字段（报文里是 null）。设备端一律显示 "--" —— 0 度和"没数据"是两回事。 */
#define WEATHER_NO_VALUE (-1000)

typedef struct {
    int temp;                     /* 摄氏度 */
    int feels;                    /* 体感温度 */
    int humidity;                 /* 相对湿度 % */
    int wind;                     /* 风速 km/h */
    char cond[WEATHER_TEXT_MAX];  /* 英文天气词，如 "Partly Cloudy" */
} weather_now_t;

typedef struct {
    char day[4];                  /* "MON"；空串 = 服务端没给 */
    char cond[WEATHER_TEXT_MAX];
    int tmin;
    int tmax;
} weather_daily_t;

typedef struct {
    char time[6];                 /* "14:00" */
    char cond[WEATHER_TEXT_MAX];
    int temp;
} weather_hourly_t;

typedef struct {
    char city[WEATHER_TEXT_MAX];  /* 城市名。含非 ASCII（中文）时页面显示 "LOCAL" */
    char update[6];               /* 观测时间 "14:00" */
    weather_now_t now;
    int daily_count;              /* 实际有效条数，0..WEATHER_DAILY_MAX */
    weather_daily_t daily[WEATHER_DAILY_MAX];
    int hourly_count;             /* 实际有效条数，0..WEATHER_HOURLY_MAX */
    weather_hourly_t hourly[WEATHER_HOURLY_MAX];
} weather_data_t;

/* 打开天气屏幕（主菜单"天气"项点击调用） */
void weather_screen_open(lv_event_t *e);
lv_obj_t *weather_screen_create_for_app(void);

/* 用一份真实天气刷新页面。屏幕还没建就先存着，建屏时补填。
 * 调用方必须**持有 LVGL 锁**（和 ui/ 下其它屏幕的约定一致）。 */
void weather_screen_apply(const weather_data_t *data);

#ifdef __cplusplus
}
#endif

#endif /* PORTED_WEATHER_SCREEN_H */
