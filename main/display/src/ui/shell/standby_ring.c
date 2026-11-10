#include "standby_ring.h"

#include <nvs.h>
#include <nvs_flash.h>

#include <esp_timer.h>

#include "esp_log.h"

#include "app_list_screen.h"
#include "home_screen.h"
#include "../apps/chat/chat_app.h"
#include "../apps/robot/robot_app.h"
#include "../framework/app_manager.h"

static const char *TAG = "standby_ring";
static standby_mode_t s_mode_before_menu = STANDBY_MODE_WALLPAPER;

#define STANDBY_NVS_NAMESPACE "standby"
#define STANDBY_NVS_KEY_EMOTE "emote"

/* ==========================================================================
 *  屏幕解析
 *
 *  screen_of() 允许**创建**（切过去时该建就建）；
 *  standby_ring_current() 一律走只读取值函数，绝不因为"问一句当前在哪页"
 *  就把 robot 页连带 434 KB 帧缓冲建出来。
 * ========================================================================== */

static lv_obj_t *screen_of(standby_mode_t mode)
{
    switch (mode) {
    case STANDBY_MODE_WALLPAPER: return home_screen_create();
    case STANDBY_MODE_CHAT:      return chat_app_screen();
    case STANDBY_MODE_EMOTE:     return robot_app_create_screen();
    default:                     return NULL;
    }
}

standby_mode_t standby_ring_current(void)
{
    lv_obj_t *active = lv_screen_active();
    if (!active) return STANDBY_MODE_COUNT;

    /* 三个取值函数都可能在屏还没建时返回 NULL —— 非 NULL 的 active 不会等于
       NULL，所以没建过的屏自然匹配不上。 */
    if (active == home_screen_get())  return STANDBY_MODE_WALLPAPER;
    if (active == chat_app_screen())  return STANDBY_MODE_CHAT;
    if (active == robot_app_screen()) return STANDBY_MODE_EMOTE;
    return STANDBY_MODE_COUNT;
}

bool standby_ring_on_ring(void)
{
    return standby_ring_current() < STANDBY_MODE_COUNT;
}

/* 交出表情模式的上下文：把 emote 播放器还回去，并让 robot 页回到 app 语义
 * （左滑换表情、右滑返回）。帧缓冲不在这里释放，原因见 robot_app_set_standby()。
 *
 * 必须在**离开环**的两条路径上都调用：
 *   - goto() 切到别的模式
 *   - show_menu() 进应用菜单
 * 漏掉后者会留下一个隐蔽的错乱：用户从表情模式左滑进菜单 → 点 测试→Robot，
 * 那时 robot 页的 s_ctx 还停在 STANDBY，于是"测试→Robot"竟然在切模式而不是
 * 换表情。on_open() 里兜了一次底，但这里该退的还是要退干净（顺便把播放器还掉）。 */
static void leave_emote_mode(void)
{
    if (standby_ring_current() == STANDBY_MODE_EMOTE) robot_app_set_standby(false);
}

/* goto 的实现体。anim 由调用者按**手势方向**给，别在这里写死：
       LV_SCREEN_LOAD_ANIM_MOVE_LEFT  —— 新页从屏幕右边进 = 左滑前进
       LV_SCREEN_LOAD_ANIM_MOVE_RIGHT —— 新页从屏幕左边进 = 右滑返回
   （几何依据：display/lv_display.c:848-883，MOVE_LEFT 新页起点 +H、MOVE_RIGHT 起点 -H。）
   原来两条路都传 MOVE_LEFT，于是"右滑从菜单返回"变成新页从**右边**覆盖进来，
   和左滑前进同向、方向是反的。 */
static void ring_goto(standby_mode_t mode, lv_screen_load_anim_t anim)
{
    if (mode >= STANDBY_MODE_COUNT) return;

    /* 换屏前的这一段（还播放器、起 emote 播放器）是同步跑的，界面冻住；
       进表情模式时它会重建 gfx_emote 渲染任务 + mmap 分区 + 几百 KB 缓冲。
       计一笔，和 app_manager 的 "build=" 一起看，就能分清"切换慢"是慢在这里
       还是慢在整屏重绘。 */
    const int64_t t_build = esp_timer_get_time();

    lv_obj_t *target = screen_of(mode);
    if (!target) {
        /* 聊天页由 XiaoweiUiDisplay::SetupUI() 登记，理论上开机就绪；
           真拿不到时保持原地，不要切到一块空屏。 */
        ESP_LOGW(TAG, "standby mode %d has no screen; staying put", (int)mode);
        return;
    }
    if (lv_screen_active() == target) return;

    /* 离开表情模式：**先**把播放器还回去，再切屏。
       set_standby(false) 内部是幂等的（robot_app.c 里有 s_player 判空保护），
       所以即使它已经被 on_close() 关过一次也不会二次释放。 */
    leave_emote_mode();

    /* 进入表情模式：切上下文 + 按 NVS 里记着的那张开播。
       必须排在 robot_app_create_screen() 之后 —— 建屏时那一下会先 play_emote(0)，
       这里再覆盖成用户选中的序号。 */
    if (mode == STANDBY_MODE_EMOTE) robot_app_set_standby(true);

    /* 用 set_shell_screen() 而**不是** xiaowei_app_open()，这是本模块的关键：
     *
     *   xiaowei_app_open() 会把当前屏 push 进 8 深的返回栈
     *   （framework/screen_stack.c:3，溢出后静默丢弃）。环上三页左滑是"进应用
     *   菜单"而不是"返回"，栈本来就该一直是空的；一旦 push，xiaowei_app_back()
     *   就会从栈里弹出上一页，和"左滑=主菜单"的语义直接打架。循环滑几圈还会把
     *   栈塞满。
     *
     *   set_shell_screen() 正好做我们想要的：关掉上一个 app 的 on_close、
     *   清空返回栈、把当前屏记为该 shell。
     *
     * 副作用：它会把 app_manager 的 s_current 置 NULL，于是 robot 的
     * on_close **不会**被它调到 —— robot 的资源释放由上面那句
     * robot_app_set_standby(false) 显式承担，两条路都幂等。 */
    xiaowei_app_manager_set_shell_screen(target);
    /* 换屏走 app_manager 的统一入口：环上这三页的左滑和"进应用/返回"是同一条整屏
       位移路径，动效时长（app_manager.c 的 SCREEN_LOAD_ANIM_MS）必须共用一个数，
       否则调完一处另一处照旧抖。方向由调用者给，见 ring_goto 上面的说明。 */
    xiaowei_screen_switch(target, anim);

    ESP_LOGI(TAG, "standby mode -> %d (build %ums)", (int)mode,
             (unsigned)((esp_timer_get_time() - t_build) / 1000));
}

void standby_ring_goto(standby_mode_t mode)
{
    /* 对外没有"手势方向"这一说（目前的调用方只有语音自动进聊天页），按"前进"处理。 */
    ring_goto(mode, LV_SCREEN_LOAD_ANIM_MOVE_LEFT);
}

void standby_ring_next(void)
{
    const standby_mode_t current = standby_ring_current();
    /* 不在环上就不接管：这个函数挂在三页的手势里，但当前活动屏未必是这三页
       （例如被语音切进 app 的中间态），那种情况下什么都不做最安全。 */
    if (current >= STANDBY_MODE_COUNT) return;
    /* 左滑：往环上的下一个模式，新页从屏幕右边进。 */
    ring_goto((standby_mode_t)((current + 1) % STANDBY_MODE_COUNT), LV_SCREEN_LOAD_ANIM_MOVE_LEFT);
}

void standby_ring_show_menu(void)
{
    standby_mode_t current = standby_ring_current();
    if (current < STANDBY_MODE_COUNT) s_mode_before_menu = current;
    /* 先退干净表情模式的上下文（还播放器 + 恢复 robot 的 app 手势语义），
       再切走。顺序不能反 —— leave_emote_mode() 要靠 lv_screen_active()
       认出"当前在表情页"，切屏之后就认不出来了。 */
    leave_emote_mode();

    /* 应用菜单不在环上。它自己右滑回壁纸页（app_list_screen.c 的 LV_DIR_RIGHT
       分支），不需要本模块介入。app_list_screen_show() 内部同样会
       set_shell_screen()，所以返回栈一样被清干净。 */
    app_list_screen_show();
}

void standby_ring_return_from_menu(void)
{
    /* 右滑返回：新页必须从**屏幕左边**滑进来（MOVE_RIGHT），和 standby_ring_next()
       的 MOVE_LEFT 正好相反 —— 手指往右走，内容就该从左边跟进，两者同向才顺。 */
    ring_goto(s_mode_before_menu, LV_SCREEN_LOAD_ANIM_MOVE_RIGHT);
}

/* ==========================================================================
 *  选中的表情序号（NVS）
 *
 *  照抄 ported/simon_game.c:87-107 的裸 NVS 写法。nvs_flash_init 已由主流程做过，
 *  这里直接 open 即可；namespace 尚不存在时 READONLY 打开会失败，按 0 处理。
 * ========================================================================== */

static size_t s_emote_index;
static bool s_emote_loaded;

static void emote_index_load(void)
{
    if (s_emote_loaded) return;
    s_emote_loaded = true; /* 无论成败都只读一次 */
    s_emote_index = 0;

    nvs_handle_t handle;
    if (nvs_open(STANDBY_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return;
    int32_t stored = 0;
    if (nvs_get_i32(handle, STANDBY_NVS_KEY_EMOTE, &stored) == ESP_OK && stored >= 0)
        s_emote_index = (size_t)stored;
    nvs_close(handle);
}

size_t standby_ring_emote_index(void)
{
    emote_index_load();
    return s_emote_index;
}

void standby_ring_set_emote_index(size_t index)
{
    emote_index_load();
    s_emote_index = index; /* 先落内存：NVS 写失败也不该让这次点击"没生效" */

    nvs_handle_t handle;
    if (nvs_open(STANDBY_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGW(TAG, "NVS namespace '%s' unavailable; emote %u kept in RAM only",
                 STANDBY_NVS_NAMESPACE, (unsigned)index);
        return;
    }
    nvs_set_i32(handle, STANDBY_NVS_KEY_EMOTE, (int32_t)index);
    nvs_commit(handle);
    nvs_close(handle);
    ESP_LOGI(TAG, "standby emote -> %u (persisted)", (unsigned)index);
}
