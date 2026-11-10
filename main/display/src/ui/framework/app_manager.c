#include "app_manager.h"
#include "screen_stack.h"
#include "../shell/home_screen.h"
#include "../shell/app_list_screen.h"

#include <esp_log.h>
#include <esp_timer.h>

static const char *TAG = "AppManager";

static const xiaowei_app_t *s_current;
static lv_obj_t *s_current_screen;

/* 切页动效时长（ms）。0 = 不做位移动画，直接换屏 —— 这是**唯一的旋钮**。
 *
 * 所有换屏都必须走下面的 xiaowei_screen_switch()，别再各自调 lv_screen_load_anim()：
 * 时长散在多处就一定会打架。原来 app_list_screen.c 和 home_screen.c 各自写死了 220，
 * 而这里是 0 —— 结果"右滑进菜单有滑入动画、从菜单返回是硬切"，看着极不协调。
 *
 * 方向约定。别按名字猜，几何在 display/lv_display.c:848-883：
 *     MOVE_LEFT  —— 新页从**屏幕右边**进来（旧页往左退）
 *     MOVE_RIGHT —— 新页从**屏幕左边**进来（旧页往右退）
 * 所以：左滑前进（环上换模式、进应用）= MOVE_LEFT；右滑返回 = MOVE_RIGHT。
 *
 * 代价先说清楚：这个动效是**整屏位移**，动画期间每一帧都要重绘整屏，而本工程整屏
 * 重绘实测 22~41ms（见 fps_overlay 的 render 读数）。220ms 按 ~30ms/帧算能画出
 * 6~7 帧，方向看得清但不算丝滑。想利落些就调小（120 大约 4 帧）；设 0 就是**硬切**，
 * 只付一次整屏重绘（~30ms），主观上最快，代价是没有方向感 —— 用户明确要求
 * "右滑从左边进、左滑从右边进"，所以这里保持有动画。 */
#define SCREEN_LOAD_ANIM_MS 220

/* 换屏后把当前这一"按"作废：只要手指还按着，就让 LVGL 直到抬手为止都不再处理输入
   （lv_indev_wait_release，indev/lv_indev.c:573；indev_proc_press 第一句 :1257 直接
   return，抬手时在 release 路径 :1476-1487 清掉，并给 act_obj 补一个 PRESS_LOST）。

   ⚠ 不加这一句，**同一次拖拽会反复切页** —— 这正是"左滑一下却翻了两页 / 按着滑动
   屏幕一直闪"的根因：
     1. 手势目标 indev->pointer.act_obj 是**按下那一刻**记住的对象（lv_indev.c:1314），
        而每个按压帧只要没在滚动、也没设 PRESS_LOCK，就会**重新搜索**手指底下的对象
        （:1268-1272）；
     2. 换屏之后手指底下换成了新页面的对象 → act_obj 变了 → 走 :1287 那个分支 →
        gesture_sent 被清 0（:1326）、gesture_sum 归零（:1327-1328）；
     3. 于是手指每再走 50px（gesture_min_distance，:1797）就**再发一次
        LV_EVENT_GESTURE**。466px 宽的一划能连发好几次，页面就一张张闪过去。

   LVGL 自己在**带动画**换屏时本来有这层保护（lv_indev.c:244-247：prev_scr != NULL
   就直接 return，动画期间不收输入），所以原来走 220ms 动画时没暴露这个 bug；
   SCREEN_LOAD_ANIM_MS 一度改成 0 之后保护没了，才变成现在这样。这里补的这一句对
   "硬切"和"动画"两条路都成立，而且覆盖到动画结束之后剩下的那截拖拽。

   只在**手指还按着**时才设：lv_indev_get_state() 读的就是本次采样状态
   （lv_indev.c:263）。点按触发的换屏（CLICKED 是在 release 路径里发的）此时已经是
   RELEASED，不会误伤下一次点击。 */
static void cancel_current_press(void) {
    for (lv_indev_t *indev = lv_indev_get_next(NULL); indev != NULL;
         indev = lv_indev_get_next(indev)) {
        if (lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER) continue;
        if (lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED) {
            lv_indev_wait_release(indev);
        }
    }
}

/* 切页。anim 只在 SCREEN_LOAD_ANIM_MS > 0 时起作用。
   auto_del 一律 false：页面的生命周期由本文件 + screen_stack 管，不让 LVGL 删。

   导出给 standby_ring.c 一起用 —— 环上那三页（壁纸/聊天/表情）的左右滑也是同一条
   整屏位移路径，两边必须共用一个旋钮，否则调完一处另一处照旧抖。 */
void xiaowei_screen_switch(lv_obj_t *screen, lv_screen_load_anim_t anim) {
    if (SCREEN_LOAD_ANIM_MS > 0) {
        lv_screen_load_anim(screen, anim, SCREEN_LOAD_ANIM_MS, 0, false);
    } else {
        lv_screen_load(screen);
    }
    cancel_current_press();
}

void xiaowei_app_manager_init(void) {
    s_current = NULL;
    s_current_screen = NULL;
    xiaowei_screen_stack_init();
}

void xiaowei_app_manager_set_shell_screen(lv_obj_t *screen) {
    if (s_current && s_current->on_close) s_current->on_close();
    s_current = NULL;
    s_current_screen = screen;
    xiaowei_screen_stack_init();
}

bool xiaowei_app_open(const char *id) {
    const xiaowei_app_t *app = xiaowei_app_find(id);
    if (!app) return false;
    if (s_current && s_current->on_close) s_current->on_close();
    if (s_current_screen) xiaowei_screen_stack_push(s_current_screen);
    else if (lv_screen_active()) xiaowei_screen_stack_push(lv_screen_active());
    s_current = app;
    /* 建屏是**同步**的、而且在换屏之前，界面上这段时间是冻住的。
       create_screen()/on_open() 里可能有重活（robot 页每进一次都要重建 emote 播放器：
       建 gfx_emote 渲染任务 + mmap 分区 + 几百 KB 缓冲），用户在这期间的滑动会被
       推迟处理 —— 那看上去也是"滑了没反应"，但和"动画/重绘慢"是两回事，修法也不同。
       所以把这一段单独计一笔，grep "build=" 就能看到。 */
    const int64_t t_build = esp_timer_get_time();
    s_current_screen = app->create_screen();
    if (!s_current_screen) return false;
    if (app->on_open) app->on_open();
    /* 用 %u 不用 %lld：本工程开着 CONFIG_NEWLIB_NANO_FORMAT，64 位格式串不被支持，
       会原样把 "lldms" 打到串口上（日志里那行 "build ldms" 就是这么来的）。
       这里的数最大也就几百 ms，转成 unsigned 足够。 */
    ESP_LOGI(TAG, "open %s: build=%ums", id,
             (unsigned)((esp_timer_get_time() - t_build) / 1000));
    xiaowei_screen_switch(s_current_screen, LV_SCREEN_LOAD_ANIM_MOVE_LEFT);
    return true;
}

void xiaowei_app_back(void) {
    if (s_current) {
        if (s_current->on_close) s_current->on_close();
        if (s_current->destroy_screen && s_current_screen) s_current->destroy_screen();
        s_current = NULL;
        s_current_screen = NULL;
    }
    lv_obj_t *previous = xiaowei_screen_stack_pop();
    if (previous) {
        s_current_screen = previous;
        xiaowei_screen_switch(previous, LV_SCREEN_LOAD_ANIM_MOVE_RIGHT);
    } else {
        app_list_screen_show();
    }
}

void xiaowei_app_close(void) { xiaowei_app_back(); }
const xiaowei_app_t *xiaowei_app_current(void) { return s_current; }
lv_obj_t *xiaowei_current_screen(void) { return s_current_screen; }
