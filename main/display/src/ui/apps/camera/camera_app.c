#include "camera_app.h"

#include "photo_store.h"
#include "../../platform/xiaowei_ui_hw.h"
#include "../../framework/app_manager.h"
#include "../../fonts.h"
#include "../../assets/xiaowei_assets.h"
#include "../../shell/submenu.h"

static lv_obj_t *s_screen;
static lv_obj_t *s_menu_screen;
static lv_obj_t *s_image;
static lv_obj_t *s_status;
static lv_timer_t *s_timer;
static lv_image_dsc_t s_frame;
/* 相册页单独一份帧描述符。原来三页共用 s_frame，而相册现在要按每张照片自己的
   尺寸（相机的 466x350 / AI 画图的 466x466）改宽高和 stride —— 共用的话逛完
   相册再回预览页，s_frame 还留着上次的尺寸，而 s_screen 是一次性建的、
   不会重设表头，预览就画歪了。 */
static lv_image_dsc_t s_album_frame;
static lv_obj_t *s_album_screen;
static lv_obj_t *s_album_image;
static lv_obj_t *s_album_empty;
static lv_obj_t *s_album_counter_pill;
static lv_obj_t *s_album_counter;
static int s_album_index;              /* 0 = 最新那张 */
static lv_obj_t *s_capture_screen;
static lv_obj_t *s_capture_image;
static lv_timer_t *s_capture_timer;
static lv_timer_t *s_capture_resume_timer;

/* 两个预览刷新定时器的周期。原来 50ms（20fps）是这条链路上最窄的一道人为节流，
   传感器和屏幕都比它快，所以压到 16ms（≈62.5fps）把上限交给硬件。

   ⚠ 现在改成 2ms，因为**显示侧的节奏源已经换了**（见 lcd_display.cc 里
   avoid_tearing + lv_timer_set_period(refr_timer, 1) 那两段）：屏幕现在锁在面板
   vsync 上（59.94Hz），刷新由"有新内容"驱动。下面这个 tick 是**唯一**的失效来源，
   它自己再挂一个 16ms 的周期，就等于在 vsync 之外又串了一道互不同步的节奏 ——
   表现为：传感器那帧已经躺在缓冲里了，却最多要等 16ms 才被交给 LVGL，而且等多久
   是随机的（两个 16ms 量级的周期打拍）。这不影响"画的是哪一帧"，但让呈现时刻抖动。

   压到 2ms 后，新帧落地到上屏的延迟 ≤2ms + 一帧 vsync。代价几乎为零：定时器跑得
   比传感器快，所以绝大多数 tick 在第一句"有新帧吗"就返回了
   —— lv_image_set_src() 是无条件 lv_obj_invalidate() 的（lv_image.c:152），
   不判就会对着同一帧反复重绘 466×350。

   注意这两个 timer 只在相机页活着（create 时建、退出时删），所以这份轮询不会
   常驻。 */
#define CAMERA_PREVIEW_MS 2

/* 各页"上一次已经画到屏上的帧序号"。

   两个页面各用一个，不共用：这两个 timer 由不同的 create 回调建、由不同的退出
   路径删，生命周期彼此独立（camera_live 的 s_timer 由 camera_event 删除、
   camera_capture 的由 capture_event 删除，各自只认得自己的那一个）。
   共用一个变量的话，先跑的那个 tick 会把序号推进，另一个页面就停在旧画面上 ——
   而这两个 timer 会不会同时活着，取决于用户怎么进出这些页面，不该由它决定对错。 */
static uint32_t s_live_shown_seq;
static uint32_t s_capture_shown_seq;

static void camera_menu_event(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_GESTURE &&
        lv_indev_get_gesture_dir(lv_indev_active()) == LV_DIR_RIGHT)
        xiaowei_app_back();
}

/* 二级菜单的两个目的地。条目表里存的是**应用 id 字符串字面量**（静态存储期，
   挂成 user_data 是安全的），由这一个回调统一打开 —— 和 legacy_app.c 那边
   item_event 拿到 user_data 再分发是同一个套路。 */
static void camera_menu_pick(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    const char *app_id = (const char *)lv_event_get_user_data(event);
    if (app_id) xiaowei_app_open(app_id);
}

/* 文字是英文（"测试"页里的 IMU / audio / motor 就是英文，同一支字体同一号）。 */
static const xiaowei_submenu_item_t s_camera_menu_items[] = {
    { &xiaowei_asset_icon_take_photo, "CAMERA", camera_menu_pick, (void *)"camera_capture" },
    { &xiaowei_asset_icon_photo,      "ALBUM",  camera_menu_pick, (void *)"camera_album" },
};

static void capture_tick(lv_timer_t *timer)
{
    (void)timer;
    if (!s_capture_image) return;
    const uint32_t seq = xiaowei_camera_get_frame_seq();
    if (seq == s_capture_shown_seq) return;   /* 没有新帧，别白重绘一遍 */
    const uint8_t *frame = xiaowei_camera_get_frame();
    if (frame) {
        s_frame.data = frame;
        lv_image_set_src(s_capture_image, &s_frame);
        s_capture_shown_seq = seq;
    }
}

static void capture_resume(lv_timer_t *timer)
{
    (void)timer;
    s_capture_resume_timer = NULL;
    xiaowei_camera_set_paused(false);
    if (!s_capture_timer) s_capture_timer = lv_timer_create(capture_tick, CAMERA_PREVIEW_MS, NULL);
}

/* 快门：冻住预览 -> 抓一帧进相册 -> 2 秒后自动恢复预览。
   抓帧本身（含防撕裂的"暂停 + 等 60ms"和拷进 PSRAM）在 photo_store.c 里，
   和 AI 透过 MCP 叫的拍照是同一条路、同一个相册。 */
static void capture_shutter_event(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || s_capture_resume_timer) return;
    if (s_capture_timer) { lv_timer_delete(s_capture_timer); s_capture_timer = NULL; }
    xiaowei_photo_capture(XIAOWEI_PHOTO_SHUTTER);
    xiaowei_camera_set_paused(true);
    s_capture_resume_timer = lv_timer_create(capture_resume, 2000, NULL);
    lv_timer_set_repeat_count(s_capture_resume_timer, 1);
}

static void capture_event(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_GESTURE ||
        lv_indev_get_gesture_dir(lv_indev_active()) != LV_DIR_RIGHT) return;
    if (s_capture_timer) { lv_timer_delete(s_capture_timer); s_capture_timer = NULL; }
    if (s_capture_resume_timer) { lv_timer_delete(s_capture_resume_timer); s_capture_resume_timer = NULL; }
    xiaowei_camera_set_paused(false);
    xiaowei_camera_stop_stream();
    xiaowei_app_back();
}

static lv_obj_t *capture_create(void)
{
    if (!s_capture_screen) {
        s_capture_screen = lv_obj_create(NULL);
        lv_obj_set_size(s_capture_screen, 466, 466);
        lv_obj_set_style_bg_color(s_capture_screen, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(s_capture_screen, LV_OPA_COVER, 0);
        lv_obj_clear_flag(s_capture_screen, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(s_capture_screen, capture_event, LV_EVENT_GESTURE, NULL);
        s_capture_image = lv_image_create(s_capture_screen);
        s_frame.header.magic = LV_IMAGE_HEADER_MAGIC;
        s_frame.header.cf = LV_COLOR_FORMAT_RGB565;
        s_frame.header.flags = 0;
        s_frame.header.w = 466;
        s_frame.header.h = 350;
        s_frame.header.stride = 466 * 2;
        s_frame.header.reserved_2 = 0;
        s_frame.data_size = 466 * 350 * 2;
        s_frame.data = NULL;
        s_frame.reserved = NULL;
        s_frame.reserved_2 = NULL;
        lv_image_set_src(s_capture_image, &s_frame);
        lv_obj_align(s_capture_image, LV_ALIGN_CENTER, 0, 0);
        lv_obj_t *shutter = lv_button_create(s_capture_screen);
        lv_obj_set_size(shutter, 104, 104);
        lv_obj_align(shutter, LV_ALIGN_BOTTOM_MID, 0, -28);
        lv_obj_set_style_radius(shutter, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(shutter, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(shutter, 7, 0);
        lv_obj_set_style_border_color(shutter, lv_color_white(), 0);
        lv_obj_add_event_cb(shutter, capture_shutter_event, LV_EVENT_CLICKED, NULL);
    }
    xiaowei_camera_start_stream();
    xiaowei_camera_set_paused(false);
    if (s_capture_resume_timer) { lv_timer_delete(s_capture_resume_timer); s_capture_resume_timer = NULL; }
    if (!s_capture_timer) s_capture_timer = lv_timer_create(capture_tick, CAMERA_PREVIEW_MS, NULL);
    return s_capture_screen;
}

lv_obj_t *camera_menu_create_for_app(void)
{
    if (!s_menu_screen) {
        s_menu_screen = lv_obj_create(NULL);
        lv_obj_set_size(s_menu_screen, 466, 466);
        /* 底色和「测试/工具/AI助手」那几页一致（legacy_app.c 的 style_screen：
           0x101418），这三页在同一个主菜单里并排，底色不一样很显眼。 */
        lv_obj_set_style_bg_color(s_menu_screen, lv_color_hex(0x101418), 0);
        lv_obj_set_style_bg_opa(s_menu_screen, LV_OPA_COVER, 0);
        lv_obj_clear_flag(s_menu_screen, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(s_menu_screen, camera_menu_event, LV_EVENT_GESTURE, NULL);
        /* 原来这里是两个 120x120 的大圆图标按钮（camera_icon_button），
           一个字都没有。现在标题 + 条目整批交给 shell/submenu，和
           「测试/工具/AI助手」、游戏页用的是同一份实现（lv_list，图标 + 文字）。 */
        xiaowei_submenu_build(s_menu_screen, "相机", s_camera_menu_items,
                              sizeof(s_camera_menu_items) /
                                  sizeof(s_camera_menu_items[0]));
    }
    return s_menu_screen;
}

static void camera_tick(lv_timer_t *timer)
{
    (void)timer;
    if (!s_image) return;
    const uint32_t seq = xiaowei_camera_get_frame_seq();
    if (seq == s_live_shown_seq) return;   /* 没有新帧，别白重绘一遍 */
    const uint8_t *frame = xiaowei_camera_get_frame();
    if (!frame) return;
    s_frame.data = frame;
    lv_image_set_src(s_image, &s_frame);
    s_live_shown_seq = seq;
}

static void camera_event(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_GESTURE &&
        lv_indev_get_gesture_dir(lv_indev_active()) == LV_DIR_RIGHT) {
        if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
        xiaowei_camera_stop_stream();
        xiaowei_app_back();
    }
}

/* 相机页那颗红快门。原来只暂停画面、一张都不存（"已拍照"是句空话，相册里
   那张其实是实时帧的回落到）。现在和拍照页一样真存一张。

   提示语用英文：这个标签挂在 ui_font_font_30_menu_cn 上，那支字体里没有
   "拍""照"（见 shell/submenu.c 的说明），写中文只会显示成两个方块。 */
#define CAMERA_TITLE    "相机"
#define CAMERA_SAVED    "SAVED"

static void shutter_event(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    xiaowei_photo_capture(XIAOWEI_PHOTO_SHUTTER);
    xiaowei_camera_set_paused(true);
    if (s_status) lv_label_set_text(s_status, CAMERA_SAVED);
}

static void restart_event(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED)
        xiaowei_camera_set_paused(false);
}

static lv_obj_t *camera_create(void)
{
    if (!s_screen) {
        s_screen = lv_obj_create(NULL);
        lv_obj_set_size(s_screen, 466, 466);
        lv_obj_set_style_bg_color(s_screen, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
        lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(s_screen, camera_event, LV_EVENT_GESTURE, NULL);

        s_frame.header.magic = LV_IMAGE_HEADER_MAGIC;
        s_frame.header.cf = LV_COLOR_FORMAT_RGB565;
        s_frame.header.flags = 0;
        s_frame.header.w = 466;
        s_frame.header.h = 350;
        s_frame.header.stride = 466 * 2;
        s_frame.header.reserved_2 = 0;
        s_frame.data_size = 466 * 350 * 2;
        s_frame.data = NULL;
        s_frame.reserved = NULL;
        s_frame.reserved_2 = NULL;

        s_image = lv_image_create(s_screen);
        lv_obj_align(s_image, LV_ALIGN_CENTER, 0, 0);
        lv_image_set_src(s_image, &s_frame);

        lv_obj_t *button = lv_button_create(s_screen);
        lv_obj_set_size(button, 80, 80);
        lv_obj_align(button, LV_ALIGN_BOTTOM_MID, -70, -100);
        lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(button, lv_color_hex(0xE53935), 0);
        lv_obj_add_event_cb(button, shutter_event, LV_EVENT_CLICKED, NULL);
        lv_obj_t *mark = lv_obj_create(button);
        lv_obj_remove_style_all(mark); lv_obj_set_size(mark, 40, 40); lv_obj_center(mark);
        lv_obj_set_style_radius(mark, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(mark, lv_color_white(), 0);
        lv_obj_t *restart = lv_button_create(s_screen);
        lv_obj_set_size(restart, 80, 80);
        lv_obj_align(restart, LV_ALIGN_BOTTOM_MID, 70, -100);
        lv_obj_set_style_radius(restart, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(restart, lv_color_hex(0x4CAF50), 0);
        lv_obj_add_event_cb(restart, restart_event, LV_EVENT_CLICKED, NULL);
        lv_obj_t *ring = lv_obj_create(restart);
        lv_obj_remove_style_all(ring); lv_obj_set_size(ring, 40, 40); lv_obj_center(ring);
        lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(ring, 7, 0);
        lv_obj_set_style_border_color(ring, lv_color_white(), 0);

        s_status = lv_label_create(s_screen);
        lv_label_set_text(s_status, CAMERA_TITLE);
        lv_obj_set_style_text_font(s_status, &ui_font_font_30_menu_cn, 0);
        lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, 12);
    }
    /* 页面是一次性建的、退出时不销毁，所以这行标签得在这里复位 ——
       否则拍过一张之后标题就一直是 SAVED，下次进来还是它。 */
    if (s_status) lv_label_set_text(s_status, CAMERA_TITLE);
    xiaowei_camera_start_stream();
    xiaowei_camera_set_paused(false);
    if (!s_timer) s_timer = lv_timer_create(camera_tick, CAMERA_PREVIEW_MS, NULL);
    return s_screen;
}

/* ------------------------------------------------------------------ 相册 --
 *
 *  内容来自 photo_store.c 里那 10 个槽位：手动快门的、AI 透过 MCP 拍的、
 *  AI 绘画下发的整屏图，三路混在一起按时间排，满了挤掉最早那张。
 *
 *  以前这里只有一张 s_saved_photo，而且**没拍过照时回落到实时帧** ——
 *  于是没按过快门也能"看到一张照片"。现在没有照片就是没有照片。
 * ------------------------------------------------------------------------ */

/* 空相册的提示语用英文：几个中文字体是按字形子集烧的（见 shell/submenu.c 的
   说明），"暂无照片"里有字不在里面。 */
#define ALBUM_EMPTY_TEXT "NO PHOTOS"

static void album_show(int index)
{
    const int count = xiaowei_photo_count();

    if (count == 0) {
        s_album_index = 0;
        lv_obj_add_flag(s_album_image, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_album_empty, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_album_counter_pill, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    if (index < 0) index = 0;
    if (index >= count) index = count - 1;
    xiaowei_photo_t photo;
    if (!xiaowei_photo_get(index, &photo)) return;
    s_album_index = index;

    s_album_frame.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_album_frame.header.cf = LV_COLOR_FORMAT_RGB565;
    s_album_frame.header.flags = 0;
    s_album_frame.header.w = photo.w;
    s_album_frame.header.h = photo.h;
    s_album_frame.header.stride = photo.stride;
    s_album_frame.header.reserved_2 = 0;
    s_album_frame.data_size = (uint32_t)photo.stride * (uint32_t)photo.h;
    s_album_frame.data = photo.pixels;
    s_album_frame.reserved = NULL;
    s_album_frame.reserved_2 = NULL;
    lv_image_set_src(s_album_image, &s_album_frame);

    /* 只缩不放：相机照片 466x350 原尺寸铺满屏宽；AI 画的 466x466 缩到看得全。 */
    int scale = 256;
    const int fit_w = 256 * 466 / photo.w;
    const int fit_h = 256 * 466 / photo.h;
    if (fit_w < scale) scale = fit_w;
    if (fit_h < scale) scale = fit_h;
    lv_image_set_scale(s_album_image, (uint32_t)scale);

    lv_obj_remove_flag(s_album_image, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_album_empty, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_album_counter_pill, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text_fmt(s_album_counter, "%d/%d", index + 1, count);
}

static void album_event(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_GESTURE) return;
    switch (lv_indev_get_gesture_dir(lv_indev_active())) {
    case LV_DIR_RIGHT:  xiaowei_app_back(); break;             /* 右滑返回，和其它页一致 */
    case LV_DIR_TOP:    album_show(s_album_index + 1); break;  /* 上滑看更早的 */
    case LV_DIR_BOTTOM: album_show(s_album_index - 1); break;  /* 下滑看更新的 */
    default: break;
    }
}

static lv_obj_t *album_create(void)
{
    if (!s_album_screen) {
        s_album_screen = lv_obj_create(NULL);
        lv_obj_set_size(s_album_screen, 466, 466);
        lv_obj_set_style_bg_color(s_album_screen, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(s_album_screen, LV_OPA_COVER, 0);
        lv_obj_clear_flag(s_album_screen, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(s_album_screen, album_event, LV_EVENT_GESTURE, NULL);

        s_album_image = lv_image_create(s_album_screen);
        lv_obj_align(s_album_image, LV_ALIGN_CENTER, 0, 0);

        lv_obj_t *title = lv_label_create(s_album_screen);
        lv_label_set_text(title, "相册");
        /* 原来是 40 号那支中文字体 —— 它连"相"和"册"都没有（那几个 40/32/28
           号字体是按另一批界面词烧的子集），标题其实一直是空白。50 号菜单
           字体收字最全，这里得用它。 */
        lv_obj_set_style_text_font(title, &ui_font_font_50_menu_cn, 0);
        lv_obj_set_style_text_color(title, lv_color_hex(0xf9f5f5), 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

        s_album_empty = lv_label_create(s_album_screen);
        lv_label_set_text(s_album_empty, ALBUM_EMPTY_TEXT);
        lv_obj_set_style_text_font(s_album_empty, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(s_album_empty, lv_color_hex(0x8A949E), 0);
        lv_obj_align(s_album_empty, LV_ALIGN_CENTER, 0, 0);

        /* 张数指示。**最后建**才画在照片上层 —— 整屏照片会盖住标题，这个
           小胶囊得压在照片上才看得见。 */
        s_album_counter_pill = lv_obj_create(s_album_screen);
        lv_obj_set_size(s_album_counter_pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_radius(s_album_counter_pill, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(s_album_counter_pill, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(s_album_counter_pill, 150, 0);
        lv_obj_set_style_border_width(s_album_counter_pill, 0, 0);
        lv_obj_set_style_shadow_width(s_album_counter_pill, 0, 0);
        lv_obj_set_style_pad_hor(s_album_counter_pill, 18, 0);
        lv_obj_set_style_pad_ver(s_album_counter_pill, 4, 0);
        lv_obj_clear_flag(s_album_counter_pill, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(s_album_counter_pill, LV_ALIGN_BOTTOM_MID, 0, -18);
        s_album_counter = lv_label_create(s_album_counter_pill);
        lv_obj_set_style_text_font(s_album_counter, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(s_album_counter, lv_color_white(), 0);
        lv_obj_center(s_album_counter);
    }

    /* 每次进来都从最新的那张开始看 */
    album_show(0);
    return s_album_screen;
}

static const xiaowei_app_t s_desc = {
    .id = "camera_live",
    .title = "相机",
    .icon = NULL,
    .create_screen = camera_create,
    .on_open = NULL,
    .on_close = NULL,
    .destroy_screen = NULL,
};

const xiaowei_app_t *camera_live_app_descriptor(void) { return &s_desc; }

static const xiaowei_app_t s_capture_desc = {
    .id = "camera_capture",
    .title = "拍照",
    .icon = NULL,
    .create_screen = capture_create,
    .on_open = NULL,
    .on_close = NULL,
    .destroy_screen = NULL,
};

const xiaowei_app_t *camera_capture_app_descriptor(void) { return &s_capture_desc; }

static const xiaowei_app_t s_album_desc = {
    .id = "camera_album",
    .title = "相册",
    .icon = NULL,
    .create_screen = album_create,
    .on_open = NULL,
    .on_close = NULL,
    .destroy_screen = NULL,
};

const xiaowei_app_t *camera_album_app_descriptor(void) { return &s_album_desc; }
