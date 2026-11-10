#include "quick_panel.h"

#include "../assets/xiaowei_assets.h"
#include "app_manager.h"

/* 面板本体和三个按钮的几何全部沿用 home_screen.c 里的原值，没有改动，
   只是宿主从 s_home 换成了 lv_layer_top()。 */
#define QUICK_PANEL_W 466
#define QUICK_PANEL_H 466
#define QUICK_PANEL_MS 160

static lv_obj_t *s_panel;

/* --------------------------------------------------------------------------
 *  开合
 *
 *  关掉时必须**显式隐藏**，不能只把 y 挪回 -466：
 *    - 挪走的面板仍在绘制链表里，底边正好压在 y=0 那条线上参与命中测试；
 *    - lv_screen_load_anim() 只清屏幕自己的动画（display/lv_display.c:809-817），
 *      **不会**取消面板的动画，切页时若还挂着一段进行中的位移，
 *      下一帧就会把已经滑走的面板又画回来。
 *  LV_OBJ_FLAG_HIDDEN 同时把它从绘制和命中测试里摘掉，比"挪到屏幕外"干净。
 * -------------------------------------------------------------------------- */

void xiaowei_quick_panel_close(void)
{
    if (!s_panel) return;   /* watch_screen 的回调可能在建面板之前就跑到 */
    lv_anim_delete(s_panel, NULL);
    lv_obj_set_y(s_panel, -QUICK_PANEL_H);
    lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
}

void xiaowei_quick_panel_open(void)
{
    if (!s_panel) return;
    if (!lv_obj_has_flag(s_panel, LV_OBJ_FLAG_HIDDEN)) return;   /* 已经开着，别重启动画 */

    lv_anim_delete(s_panel, NULL);
    lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_y(s_panel, -QUICK_PANEL_H);   /* 起点恒定，动画从屏外滑入 */

    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, s_panel);
    lv_anim_set_exec_cb(&anim, (lv_anim_exec_xcb_t)lv_obj_set_y);
    lv_anim_set_values(&anim, -QUICK_PANEL_H, 0);
    lv_anim_set_duration(&anim, QUICK_PANEL_MS);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    lv_anim_start(&anim);
}

/* --------------------------------------------------------------------------
 *  切屏时自动收起
 * -------------------------------------------------------------------------- */

static void screen_unload_cb(lv_event_t *event)
{
    (void)event;
    xiaowei_quick_panel_close();
}

void xiaowei_quick_panel_watch_screen(lv_obj_t *screen)
{
    if (!screen) return;
    /* 先摘再挂，重复登记同一块屏就只留一份。 */
    lv_obj_remove_event_cb(screen, screen_unload_cb);
    lv_obj_add_event_cb(screen, screen_unload_cb, LV_EVENT_SCREEN_UNLOAD_START, NULL);
}

/* --------------------------------------------------------------------------
 *  面板自己的手势
 * -------------------------------------------------------------------------- */

/* 面板打开时是**模态**的：盖满屏幕的那一层就是 act_obj，LVGL 的输入搜索
   （indev/lv_indev.c:1654，顺序 sys -> top -> active screen -> bottom）根本
   到不了下面那页，所以底下三个待机页的 LEFT/RIGHT 分支收不到手势 —— 模态是
   结构性成立的，不靠在别处设标志位。

   这里只认上滑（收起）；LEFT/RIGHT/BOTTOM 一律什么都不做。 */
static void panel_gesture_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_GESTURE) return;
    if (lv_indev_get_gesture_dir(lv_indev_active()) == LV_DIR_TOP)
        xiaowei_quick_panel_close();
}

/* --------------------------------------------------------------------------
 *  三个按钮
 * -------------------------------------------------------------------------- */

static void quick_panel_action(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;

    /* 手势发完之后 LVGL **仍会补发一次 CLICKED** —— indev/lv_indev.c:1525-1532
       只看"按下和抬起是否落在同一个对象里"，不看 gesture_sent。而手势阈值只有
       50px（lv_indev.c:46），按钮有 150px，所以"在按钮上左右划一下"会同时
       发出手势 **并** 打开音量页，模态当场失效。

       gesture_dir 每次重新按压都归零（lv_indev.c:1325），只在真的发出手势时才
       置位（:1804-1812），所以拿它当判据是准的。
       不能用 lv_indev_get_press_moved()：它只在单帧位移超过 10px 时置位，
       慢速拖过 50px 时它还是 0。 */
    lv_indev_t *indev = lv_event_get_indev(event);
    if (indev && lv_indev_get_gesture_dir(indev) != LV_DIR_NONE) return;

    const char *id = (const char *)lv_event_get_user_data(event);
    xiaowei_quick_panel_close();
    if (id) xiaowei_app_open(id);
}

static lv_obj_t *quick_action_button(const lv_image_dsc_t *icon,
                                     const char *id, int x, int y)
{
    lv_obj_t *button = lv_button_create(s_panel);
    lv_obj_set_size(button, 150, 150);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x20262E), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(button, 26, 0);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_add_event_cb(button, quick_panel_action, LV_EVENT_CLICKED, (void *)id);

    if (icon) {
        lv_obj_t *image = lv_image_create(button);
        lv_image_set_src(image, icon);
        lv_obj_set_size(image, 96, 96);
        lv_obj_center(image);
        lv_obj_set_y(image, -2);
    }
    return button;
}

static void quick_qr_button(void)
{
    lv_obj_t *button = lv_button_create(s_panel);
    lv_obj_set_size(button, 150, 150);
    lv_obj_set_pos(button, 54, 270);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x20262E), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(button, 26, 0);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_add_event_cb(button, quick_panel_action, LV_EVENT_CLICKED, (void *)"qrcode");

    lv_obj_t *mark = lv_obj_create(button);
    lv_obj_remove_style_all(mark);
    /* The QR artwork is a child of the button. Bubble touches from every
     * pixel of the artwork to the full-size button hit target. */
    lv_obj_add_flag(mark, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_size(mark, 96, 96);
    lv_obj_set_style_bg_color(mark, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(mark, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(mark, 8, 0);
    lv_obj_center(mark);
    static const lv_coord_t points[][2] = {
        {39, 9}, {57, 15}, {72, 33}, {39, 48}, {60, 57}, {78, 72},
        {30, 69}, {51, 78}, {75, 51}
    };
    for (size_t i = 0; i < sizeof(points) / sizeof(points[0]); ++i) {
        lv_obj_t *dot = lv_obj_create(mark);
        lv_obj_remove_style_all(dot);
        lv_obj_add_flag(dot, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_set_size(dot, 12, 12);
        lv_obj_set_pos(dot, points[i][0], points[i][1]);
        lv_obj_set_style_bg_color(dot, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    }
    /* Three finder patterns make the action visually identifiable as QR. */
    static const lv_coord_t finder[][2] = {{6, 6}, {60, 6}, {6, 60}};
    for (size_t i = 0; i < 3; ++i) {
        lv_obj_t *outer = lv_obj_create(mark);
        lv_obj_add_flag(outer, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_remove_style_all(outer);
        lv_obj_set_size(outer, 30, 30);
        lv_obj_set_pos(outer, finder[i][0], finder[i][1]);
        lv_obj_set_style_border_width(outer, 7, 0);
        lv_obj_set_style_border_color(outer, lv_color_black(), 0);
        lv_obj_set_style_bg_color(outer, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(outer, LV_OPA_COVER, 0);
    }
}

/* --------------------------------------------------------------------------
 *  建面板
 * -------------------------------------------------------------------------- */

void xiaowei_quick_panel_init(void)
{
    if (s_panel) return;   /* 只建一次 */

    /* 挂在顶层：不属于任何 screen，所以待机环上三页共用同一个面板。
       层级也要留意 —— 顶层同层的兄弟是**后建的画在上面**
       （core/lv_refr.c:1116-1129），面板是不透明的全屏对象，所以
       xiaowei_ui.c 里必须**先** init 面板、**后**建帧率标签，
       否则帧率标签会被面板整个盖住。 */
    s_panel = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_panel, QUICK_PANEL_W, QUICK_PANEL_H);
    lv_obj_set_pos(s_panel, 0, -QUICK_PANEL_H);
    lv_obj_set_style_bg_color(s_panel, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_panel, 0, 0);
    lv_obj_set_style_border_width(s_panel, 0, 0);
    lv_obj_set_style_pad_all(s_panel, 0, 0);
    lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);

    /* 关键的一句。LV_EVENT_GESTURE **不是**发给手指按住的那个对象的：
       indev_gesture()（indev/lv_indev.c:1773-1815）从 act_obj 往上爬，直到遇到
       一个**没设** LV_OBJ_FLAG_GESTURE_BUBBLE 的祖先，把事件发给它 —— 而这个
       flag 对任何有父对象的控件都默认打开（core/lv_obj.c:593）。

       面板挂到 lv_layer_top() 之后就有了父对象，不清掉这个 flag 的话，
       在按钮上起手的手势会一路冒泡到 top layer 就没了，panel_gesture_cb
       永远不会被调用（"在按钮上上滑收不起面板"）。

       清掉之后，在按钮上起手的手势会**停在面板这一层**，所以面板一个回调就够：
       按钮既不需要 LV_OBJ_FLAG_EVENT_BUBBLE（那是另一套机制，管的是"已经收到
       事件的对象继续往上传"，对手势投递没有影响），也不需要各挂一份回调。 */
    lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(s_panel, panel_gesture_cb, LV_EVENT_GESTURE, NULL);

    quick_action_button(&xiaowei_asset_icon_sound, "settings_volume", 54, 70);
    quick_action_button(&xiaowei_asset_icon_display, "settings_brightness", 262, 70);
    quick_qr_button();

    /* 建出来就是收起状态。 */
    lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
}
