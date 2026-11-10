#include "robot_app.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "emote_gen_player.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "../../framework/app_manager.h"
#include "../../framework/quick_panel.h"
#include "../../shell/standby_ring.h"
#include "../../fonts.h"

#define ROBOT_W 466
#define ROBOT_H 466

/* 编辑层配色。刻意沿用壁纸页时间/日期那支玫瑰色（home_screen.c:171），
 * 让"这个按钮是把表情送到主页"这件事在视觉上有个线索 —— 按钮的颜色和主页是一套。 */
#define ROBOT_ACCENT 0xd68e8e

/* 把表情帧交给 LVGL 的轮询周期（refresh_timer）。
 *
 * 这里是**采样器**，周期必须**远快于**生产者，否则两个周期会打拍频 —— 这个值上
 * 前后踩过两次，都记下来：
 *
 *  ① 40ms（慢于生产者的 33.3ms）：每 5 帧里必有 1 帧（33.3×5 vs 40×4 那个错位）
 *     还没被采样就被下一帧覆盖，永远画不出来。实测把页面压在 23fps ——
 *     比生产者自己的 30 还低，而且低得没规律。
 *  ② 16ms（快于生产者，但只有 2:1）：33.3/16 ≈ 2.08，采样器每 2~3 个 tick 才撞上
 *     一帧，于是"同一帧被画 1 次还是 2 次"完全取决于两个周期的相位 ——
 *     呈现间隔在 17/33/50ms 之间跳（实测 gap min/max 19~29/66ms 就是这个）。
 *     这就是「帧率不固定」。**2:1 附近的采样周期一定会产生这种拍频**，
 *     只有把采样周期压到远小于生产周期才能消掉。
 *
 * 取 1ms：发布到失效的延迟 ≤1ms，呈现节奏就等于生产节奏，拍频消失。
 * 代价为零 —— LVGL 任务本来就被 refr timer 按 1ms 唤醒（lcd_display.cc:417），
 * 多这一个 1ms 定时器不改变任务实际唤醒频率；而且生产者没出帧时
 * s_dirty 为 false，这个回调第一句就返回，不会去碰 466×466 的画布。
 *
 * ⚠ 这一页的上限就是生产者的 30fps（动画本身按 30fps 灌帧）。想再往上不是改这个
 * 数，而是改动画资产的灌帧速率 —— 那等于让表情播放变快，是内容决定，不是帧率问题。 */
#define ROBOT_REFRESH_MS 1

/* ==========================================================================
 *  同一块屏，两种身份
 *
 *  这个页面被两条完全不同的路径使用，且用户明确要求二者**互相独立**
 *  （原话：「一个是测试->robot页面，一个主页是robot表情！这两个是独立的」，
 *   类比是「壁纸商店」和「锁屏壁纸」）：
 *
 *    ROBOT_CTX_APP      测试 → Robot。普通 app 上下文。
 *                       手势**保持原样**：左滑换下一张表情、右滑返回。
 *                       这是"商店"：在这里翻看、并单击唤出编辑层把某张**放置到主页**。
 *
 *    ROBOT_CTX_STANDBY  主页的表情模式。待机环上下文。
 *                       手势属于环：右滑下一个模式、左滑进主菜单。
 *                       这是"锁屏"：只负责**显示**已经放置好的那一张，
 *                       不出编辑层、也不换表情。
 *
 *  ⚠ 「切换模式」是**主页**的定义，跟测试→Robot 无关。所以下面 screen_event()
 *  里两种上下文的手势是分流的，app 那一支必须和改动前逐字一致。
 *
 *  两者共用同一个 lv_obj（以及同一个 434 KB 帧缓冲、同一个 emote 播放器），
 *  但任何时刻只有一个可见，用户看不出共用；省掉一整套重复的表情渲染资源。
 * ========================================================================== */
typedef enum {
    ROBOT_CTX_APP = 0,
    ROBOT_CTX_STANDBY = 1,
} robot_ctx_t;

static const char *TAG = "xiaowei_robot";
static lv_obj_t *s_screen;
static lv_obj_t *s_canvas;
static lv_obj_t *s_edit_layer;
static lv_obj_t *s_place_btn;
static lv_timer_t *s_refresh_timer;
static uint16_t *s_framebuffer;
static emote_gen_player_handle_t s_player;
static size_t s_emote_index;
static volatile bool s_dirty;
static robot_ctx_t s_ctx = ROBOT_CTX_APP;

/* 诊断开关：数"一帧被切成几块""帧与帧的间隔是多少"。定位完改成 0，下面的计数
   和日志一起消失，其余代码一行不用动。 */
#define ROBOT_FLUSH_DEBUG 1

#if ROBOT_FLUSH_DEBUG
static uint32_t s_dbg_chunks;          /* 本帧已收到的分块数 */
static uint32_t s_dbg_chunks_last;     /* 上一整帧的分块数 */
static uint32_t s_dbg_frames;          /* 本窗口结算过的帧数 */
static uint32_t s_dbg_win_start_us;    /* 本窗口起点 */
static uint32_t s_dbg_prev_us;         /* 上一帧的结算时刻 */
static uint32_t s_dbg_min_gap_us;
static uint32_t s_dbg_max_gap_us;
#endif

/* 引擎的 flush 是**按块**回调的，而且只有"整帧最后一块"才代表像素齐了：
 *
 *   gfx_render.c:171 gfx_render_part_area() 里 row_h = buf_pixels / area_w。
 *   本页 cfg.buffers.buf_pixels = 466*16（见 ensure_player），于是整屏脏区被切成
 *   466/16 ≈ 30 块，**每块一次回调**；块与块之间引擎还要等我们的
 *   notify_flush_finished 才继续（gfx_render.c:250）。
 *   gfx_render.c:242 在调回调**之前**把 flushing_last 置好（= 本区最后一块 &&
 *   最后一个脏区），所以回调里查 gfx_disp_is_flushing_last() 是准的。
 *
 * 原来的写法是"第一块拷完就 s_dirty = true"，于是 LVGL 去画的是一张
 * **1/30 是新帧、29/30 还停在上一帧**的混合画面 —— 表情一动就是横条状的闪，
 * 这就是「表情经常闪烁」。而且一帧里被置 30 次脏，1ms 的采样器每次只抓 1 次，
 * 多出来的那些是纯粹的重复整屏重绘，白吃渲染预算。
 *
 * 改成只在整帧最后一块置脏：一帧一呈，画出去的必然是一张完整帧。
 *
 * ⚠ 回调里**绝不能阻塞**（更别去拿 LVGL 锁）：渲染任务是在**持着 render_mutex**
 *   的情况下调我们的（gfx_core.c:117 拿锁 → :142 gfx_render_handler → 本回调），
 *   而 gfx_emote_lock 就是那把锁（gfx_core.c:300）。一旦在这里等，
 *   用户此刻左滑换表情（LVGL 任务 → play_emote → anim_now → gfx_emote_lock）
 *   就是 ABBA 死锁。所以这里只做拷贝 + 置标志，一个字都不能等。 */
static void robot_flush_cb(int x1, int y1, int x2, int y2, const void *data,
                           emote_gen_player_handle_t manager)
{
    bool frame_complete = false;

    if (s_framebuffer && data && x1 >= 0 && y1 >= 0 && x2 <= ROBOT_W &&
        y2 <= ROBOT_H && x2 > x1 && y2 > y1) {
        const uint8_t *src = (const uint8_t *)data;
        const int width = x2 - x1;
        for (int y = y1; y < y2; ++y) {
            memcpy(&s_framebuffer[(size_t)y * ROBOT_W + x1], src,
                   (size_t)width * sizeof(uint16_t));
            src += (size_t)width * sizeof(uint16_t);
        }
        /* manager 一定是非空的（emote_gen_player.c:59 的 wrapper 已判过），
           而 get_disp 理论上可能给 NULL —— gfx_disp_is_flushing_last() 对 NULL
           返回 false，所以这条链子是安全的，不需要额外判空。 */
        frame_complete = gfx_disp_is_flushing_last(emote_gen_player_get_disp(manager));
    }

    if (frame_complete) {
        s_dirty = true;
#if ROBOT_FLUSH_DEBUG
        const uint32_t now = (uint32_t)esp_timer_get_time();
        s_dbg_chunks_last = s_dbg_chunks;
        s_dbg_chunks = 0;
        if (s_dbg_prev_us != 0) {
            const uint32_t gap = now - s_dbg_prev_us;
            if (s_dbg_min_gap_us == 0 || gap < s_dbg_min_gap_us) s_dbg_min_gap_us = gap;
            if (gap > s_dbg_max_gap_us) s_dbg_max_gap_us = gap;
        }
        s_dbg_prev_us = now;
        if (s_dbg_win_start_us == 0) s_dbg_win_start_us = now;
        if (++s_dbg_frames >= 30) {   /* 30 帧 ≈ 1 秒，一行/秒 */
            /* 这个回调是在**引擎的渲染任务**上跑的（gfx_render.c:243 同步调用），
               所以 uxTaskGetStackHighWaterMark(NULL) 量的正是 gfx_render 那条栈的
               历史最深用量 —— 单位是 StackType_t 字（×4 = 字节）。
               它是判断"5KB 栈溢出"那条推断的唯一直接证据：余量贴近 0 就是溢出过
               （PSRAM 栈溢出不 panic，只会静默写坏邻居）。 */
            ESP_LOGI(TAG, "emote: %u 块/帧, %u 帧用 %ums, 帧间隔 min/max %u/%u ms, 渲染栈余 %u 字",
                     (unsigned)s_dbg_chunks_last, (unsigned)s_dbg_frames,
                     (unsigned)((now - s_dbg_win_start_us) / 1000),
                     (unsigned)(s_dbg_min_gap_us / 1000), (unsigned)(s_dbg_max_gap_us / 1000),
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
            s_dbg_frames = 0;
            s_dbg_win_start_us = now;
            s_dbg_min_gap_us = 0;
            s_dbg_max_gap_us = 0;
        }
#endif
    }

#if ROBOT_FLUSH_DEBUG
    s_dbg_chunks++;
#endif

    /* The emote worker waits for this even when the destination is invalid. */
    if (manager) emote_gen_player_notify_flush_finished(manager);
}

/* s_dirty 的语义是"s_framebuffer 里躺着一张**完整**的帧"（由 robot_flush_cb 在
   整帧最后一块置起，见那边）。这里只负责把它翻译成一次 LVGL 失效 —— 必须在
   LVGL 任务里做，LVGL 不是线程安全的，不能从引擎的渲染任务直接 lv_obj_invalidate。 */
static void refresh_timer(lv_timer_t *timer)
{
    (void)timer;
    if (s_dirty && s_canvas) {
        s_dirty = false;
        lv_obj_invalidate(s_canvas);
    }
}

static bool ensure_player(void)
{
    if (s_player) return true;
    if (!s_framebuffer) {
        s_framebuffer = (uint16_t *)heap_caps_malloc((size_t)ROBOT_W * ROBOT_H * 2,
                                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_framebuffer) {
            s_framebuffer = (uint16_t *)heap_caps_malloc((size_t)ROBOT_W * ROBOT_H * 2,
                                                         MALLOC_CAP_8BIT);
        }
    }
    if (!s_framebuffer) {
        ESP_LOGE(TAG, "robot framebuffer allocation failed");
        return false;
    }

    emote_gen_player_config_t cfg = {0};
    cfg.flags.swap = false;
    cfg.flags.double_buffer = true;
    cfg.flags.buff_dma = false;
    cfg.flags.buff_spiram = true;
    cfg.gfx_emote.h_res = ROBOT_W;
    cfg.gfx_emote.v_res = ROBOT_H;
    cfg.gfx_emote.fps = 30;
    cfg.buffers.buf_pixels = ROBOT_W * 16;
    cfg.task.task_priority = 7;
    /* 栈大小：原来 5KB，**不够**。实测崩溃在引擎自己的绘制路径里 ——
     * gfx_render.c:94 遍历 disp->child_list 时 child_node 变成 0x4d8 这种垃圾值，
     * 是典型的"栈溢出把邻居写坏"。
     *
     * 为什么这条推断成立、以及为什么以前没暴露：
     *   - PSRAM 栈**拿不到**栈溢出检测。IDF 的 CONFIG_FREERTOS_CHECK_STACKOVERFLOW_*
     *     靠内部 RAM 的观察点/canary，外部内存的栈它不看 —— 所以溢出不 panic、
     *     不报 "Stack canary watchpoint"，只是**静默**写穿到栈下方的 PSRAM。
     *   - 捐赠工程（main/display/emote_display.cc:97）用 6KB 且 task_stack_in_ext=false
     *     （栈在内部 RAM，真溢出会当场面崩溃报出来）。我们是 **5KB + PSRAM**：
     *     比参照更小，且失去了全部检测手段 —— 最坏的一种组合。
     *   - 5KB 要扛的是"图片解码 + 混合 + 遍历对象树"这条链路（gfx_render_part_area
     *     → gfx_render_draw_child_objects → obj->vfunc.draw → 解码/搬运），
     *     本来就很紧。
     * ⚠ 顺带说明：栈变大**不会**掩盖证据 —— uxTaskGetStackHighWaterMark() 量的是
     *   "历史最深用量"，与栈开多大无关。所以下面的 emote: 日志里那个余量数字能
     *   直接回答"真实峰值到底有没有超过 5KB"，从而证实或否证这条推断。 */
    cfg.task.task_stack = 16 * 1024;
    cfg.task.task_affinity = 0;
    /* 渲染任务的栈必须放 PSRAM。置 false 时 emote_gen_player.c:216 会要
     * MALLOC_CAP_INTERNAL，而本工程（小智 WiFi/协议栈/音频/唤醒词 + LVGL UI）
     * 内部 DRAM 只剩 ~54 KB 且高度碎片化，连续 5 KB 拿不到，直接表现是：
     *   E core: gfx_emote_init(200): Failed to create render task
     *   E emote_gen_player: emote_gen_player_init(224): gfx_emote_init failed
     * 于是 ensure_player() 恒失败、表情页一片黑。donor 的 lcd 工程只有 UI，
     * 内部 RAM 宽裕，那边用 false 也能跑 —— 这里不能照抄。
     * 前提已满足：CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y、
     * CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM=y。 */
    cfg.task.task_stack_in_ext = true;
    cfg.flush_cb = robot_flush_cb;
    s_player = emote_gen_player_init(&cfg);
    if (!s_player) return false;

    gfx_disp_t *disp = emote_gen_player_get_disp(s_player);
    if (disp) gfx_disp_set_bg_color(disp, GFX_COLOR_HEX(0x000000));

    emote_gen_player_data_t data = {0};
    data.type = EMOTE_GEN_PLAYER_SOURCE_PARTITION;
    /* 与 lcd 工程一致：表情包放在独立的 emote 分区，不跟 xiaozhi 的 assets 混。
     *
     * 曾经指到 "assets" 是因为分区表里没有 emote 这一项，而那时的表（16m_ota7.csv）
     * 又把 16 MB 铺满了、塞不下 1.73 MB 的表情包。现在换用 partitions/16m_uiimg.csv，
     * 把 8 张全屏大图外置到 uiimg 分区腾出空间后，emote 有了自己的 0x1C0000。
     *
     * ⚠ 单独分区的意义不只是"放得下"：xiaozhi 的 assets 分区在 OTA 下发新资源包时
     *   会被 main/assets.cc 的 Download()/Apply() 整块重写，表情包混在里面会被抹掉。 */
    data.source.partition_label = "emote";
    data.flags.mmap_enable = 1;
    if (emote_gen_player_mount_assets(s_player, &data) != ESP_OK) {
        ESP_LOGE(TAG, "emote pack is unavailable in the \"emote\" partition "
                      "(did the flash step write main/display/Expression/emote-assets.bin?)");
        emote_gen_player_deinit(s_player);
        s_player = NULL;
        return false;
    }
    return true;
}

/* --------------------------------------------------------------------------
 *  编辑层（只在 app 上下文出现）
 * -------------------------------------------------------------------------- */

static void set_edit_layer_visible(bool visible)
{
    if (!s_edit_layer) return;
    /* 待机（锁屏）上下文不出编辑层 —— 这条是双保险，正常路径下
       robot_app_set_standby(true) 已经先把它收起来并置好了 s_ctx。 */
    if (visible && s_ctx != ROBOT_CTX_APP) return;
    if (visible) lv_obj_clear_flag(s_edit_layer, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_edit_layer, LV_OBJ_FLAG_HIDDEN);
}

static bool edit_layer_visible(void)
{
    return s_edit_layer && !lv_obj_has_flag(s_edit_layer, LV_OBJ_FLAG_HIDDEN);
}

/* 按钮没有文字（用户要求），所以用"实心/描边"表示当前这张是不是已经放置好的那张。
 * 重开编辑层或换了一张表情后都要调一次，否则状态会对不上。 */
static void sync_place_btn(void)
{
    if (!s_place_btn) return;
    const bool placed = (standby_ring_emote_index() == s_emote_index);
    lv_obj_set_style_bg_opa(s_place_btn, placed ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_place_btn, placed ? 0 : 3, 0);
    lv_obj_set_style_border_color(s_place_btn, lv_color_hex(ROBOT_ACCENT), 0);
}

static void place_btn_event(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    /* 放置 = 记下"主页表情模式该显示哪一张"，存 NVS，重启保留。
       收起编辑层既是确认也是收尾。 */
    standby_ring_set_emote_index(s_emote_index);
    set_edit_layer_visible(false);
}

static void play_emote(size_t index)
{
    if (!ensure_player()) return;
    size_t count = emote_gen_player_get_index_count(s_player);
    if (!count) return;
    s_emote_index = index % count;
    if (emote_gen_player_anim_now(s_player, s_emote_index, true) == ESP_OK)
        emote_gen_player_set_tip_text(s_player, "");
    sync_place_btn();
}

static void screen_event(lv_event_t *event)
{
    const lv_event_code_t code = lv_event_get_code(event);

    if (code == LV_EVENT_GESTURE) {
        const lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());

        if (s_ctx == ROBOT_CTX_STANDBY) {
            /* 锁屏：手势归待机环。**左滑是"下一个模式"**（环内部自己知道下一站是谁），
               右滑是应用主菜单而不是返回。 */
            if (dir == LV_DIR_LEFT) standby_ring_next();
            else if (dir == LV_DIR_RIGHT) standby_ring_show_menu();
            else if (dir == LV_DIR_BOTTOM) xiaowei_quick_panel_open();
            /* 上滑收起不在这里：面板一打开就盖满整屏、成为 act_obj，
               手势由面板自己接（framework/quick_panel.c）。 */
            return;
        }

        /* 商店（测试→Robot）：**与改动前逐字一致** —— 左滑换下一张、右滑返回。
           用户明确要求"和原来的一致"，这里不是笔误。 */
        if (dir == LV_DIR_LEFT) {
            if (s_player) {
                size_t count = emote_gen_player_get_index_count(s_player);
                play_emote(s_emote_index + 1u < count ? s_emote_index + 1u : 0u);
            }
        } else if (dir == LV_DIR_RIGHT) {
            xiaowei_app_back();
        }
        return;
    }

    if (code == LV_EVENT_CLICKED) {
        /* 单击表情出边框，再点一下收起。只有 app 上下文有这一层。 */
        set_edit_layer_visible(!edit_layer_visible());
    }
}

static lv_obj_t *create_screen(void)
{
    if (!s_screen) {
        s_screen = lv_obj_create(NULL);
        lv_obj_set_size(s_screen, ROBOT_W, ROBOT_H);
        lv_obj_set_style_bg_color(s_screen, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(s_screen, 0, 0);
        lv_obj_set_style_border_width(s_screen, 0, 0);
        lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
        /* 用 ALL 而不是 GESTURE：这一层现在还要接单击（唤出编辑层）。 */
        lv_obj_add_event_cb(s_screen, screen_event, LV_EVENT_ALL, NULL);

        /* 切走时收起下拉快捷面板（面板挂在顶层，切屏带不走它）。
           这块屏 standby / app 两种身份共用，登记是按屏的、与 s_ctx 无关；
           多这一条不会给 app 上下文添行为 —— 面板在 app 上下文里根本打不开
           （screen_event 的 BOTTOM 分支只写在 standby 那一支里），
           而 close() 对已经收着的面板是空操作。 */
        xiaowei_quick_panel_watch_screen(s_screen);

        s_canvas = lv_canvas_create(s_screen);
        lv_obj_set_pos(s_canvas, 0, 0);
        /* 不可点击，单击才会落到 s_screen 上（否则 canvas 吃掉点击，
           screen_event 永远收不到 CLICKED）。 */
        lv_obj_clear_flag(s_canvas, LV_OBJ_FLAG_CLICKABLE);

        /* 编辑层建在 canvas 之后，保证盖在表情上面。
           440×440 居中、圆角拉满，跟 466 的圆屏同心，看起来是一圈内嵌的边框。 */
        s_edit_layer = lv_obj_create(s_screen);
        lv_obj_set_size(s_edit_layer, 440, 440);
        lv_obj_center(s_edit_layer);
        lv_obj_set_style_bg_opa(s_edit_layer, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_edit_layer, 3, 0);
        lv_obj_set_style_border_color(s_edit_layer, lv_color_hex(ROBOT_ACCENT), 0);
        lv_obj_set_style_radius(s_edit_layer, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_pad_all(s_edit_layer, 0, 0);
        lv_obj_clear_flag(s_edit_layer, LV_OBJ_FLAG_SCROLLABLE);
        /* LVGL 默认**不**冒泡；必须显式加，边框空白处的单击/手势才能到 s_screen。
           本工程其它地方同理：home_screen.c:167,193、app_list_screen.c:154。 */
        lv_obj_add_flag(s_edit_layer, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_flag(s_edit_layer, LV_OBJ_FLAG_HIDDEN);

        /* 层里**只放一个按钮**（用户要求），且按钮**没有文字**（用户要求）。
           按钮**不加** EVENT_BUBBLE —— 它的单击必须被自己吃掉，
           否则会冒泡到 s_screen 触发"再点一下收起"，按下去边框会立刻消失。 */
        s_place_btn = lv_button_create(s_edit_layer);
        lv_obj_set_size(s_place_btn, 104, 104);
        lv_obj_align(s_place_btn, LV_ALIGN_BOTTOM_MID, 0, -46);
        lv_obj_set_style_radius(s_place_btn, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(s_place_btn, lv_color_hex(ROBOT_ACCENT), 0);
        lv_obj_set_style_bg_opa(s_place_btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_place_btn, 0, 0);
        lv_obj_set_style_shadow_width(s_place_btn, 0, 0);
        lv_obj_add_event_cb(s_place_btn, place_btn_event, LV_EVENT_CLICKED, NULL);

        s_refresh_timer = lv_timer_create(refresh_timer, ROBOT_REFRESH_MS, NULL);
    }
    if (!s_framebuffer) {
        s_framebuffer = (uint16_t *)heap_caps_malloc((size_t)ROBOT_W * ROBOT_H * 2,
                                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_framebuffer)
            s_framebuffer = (uint16_t *)heap_caps_malloc((size_t)ROBOT_W * ROBOT_H * 2,
                                                         MALLOC_CAP_8BIT);
    }
    if (s_framebuffer) {
        lv_canvas_set_buffer(s_canvas, s_framebuffer, ROBOT_W, ROBOT_H,
                             LV_COLOR_FORMAT_RGB565);
        memset(s_framebuffer, 0, (size_t)ROBOT_W * ROBOT_H * 2);
    }
    s_dirty = false;
    play_emote(s_ctx == ROBOT_CTX_STANDBY ? standby_ring_emote_index() : 0);
    return s_screen;
}

static void on_close(void)
{
    if (s_player) {
        emote_gen_player_deinit(s_player);
        s_player = NULL;
    }
    s_dirty = false;
}

static void on_open(void)
{
    /* 进 app 就把身份摆正：待机环退出时会把上下文交还给 APP，但用户也可能
       从主菜单右滑回壁纸页、再左滑进菜单、再点测试→Robot，那条路径没经过环。
       在 on_open 里兜一次底，成本为零，省掉一整类"手势还停在待机语义"的错乱。 */
    s_ctx = ROBOT_CTX_APP;
    set_edit_layer_visible(false);
}

static const xiaowei_app_t s_descriptor = {
    .id = "robot",
    .title = "Robot",
    .icon = NULL,
    .create_screen = create_screen,
    .on_open = on_open,
    .on_close = on_close,
    .destroy_screen = NULL,
};

const xiaowei_app_t *robot_app_descriptor(void) { return &s_descriptor; }

lv_obj_t *robot_app_screen(void) { return s_screen; }

lv_obj_t *robot_app_create_screen(void) { return create_screen(); }

void robot_app_set_standby(bool on)
{
    if (on) {
        s_ctx = ROBOT_CTX_STANDBY;
        set_edit_layer_visible(false); /* 锁屏不摆编辑层 */
        if (s_screen) play_emote(standby_ring_emote_index());
        ESP_LOGI(TAG, "context -> STANDBY (emote %u)", (unsigned)standby_ring_emote_index());
        return;
    }

    s_ctx = ROBOT_CTX_APP;
    set_edit_layer_visible(false);
    /* 归还 emote 播放器（mmap 资源 + 任务 + 双缓冲）。
       ⚠ 这里**不**释放 s_framebuffer：调用方紧接着就会 lv_screen_load_anim()，
       那 220 ms 的滑出动画仍然要绘制这块屏，canvas 还指着这个缓冲区 ——
       提前 free 就是 use-after-free。434 KB PSRAM 换来这条路径的确定性，值得。
       （原先 create_screen() 也从不释放它，行为一致。） */
    on_close();
    ESP_LOGI(TAG, "context -> APP (player released)");
}
