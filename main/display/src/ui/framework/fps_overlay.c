#include "fps_overlay.h"

#include "lvgl/lvgl.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>

#include <string.h>

#include "../platform/xiaowei_ui_hw.h"

/* 统计窗口。500ms 一次：够快能看清掉帧，又不至于让标签自己成为画面负担
   （每刷一次标签就要重绘一次底边那一条）。 */
#define FPS_OVERLAY_PERIOD_MS 500

/* 串口 FPS 日志总开关：默认关闭。
   屏幕上的浮层照常刷新，只是不再每秒往控制台打一行 —— 正常运行不该刷屏。
   要抓帧率数据时把它改成 1。 */
#define FPS_OVERLAY_SERIAL_LOG 0

/* 串口日志的节流：每 N 个窗口打一行（2 × 500ms = 1 秒一行）。
   控制台走 USB-Serial-JTAG（见 sdkconfig 的 CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG），
   打日志不占 UART 也不进 LVGL 任务的关键路径，但仍不值得每 500ms 刷一行。 */
#define FPS_OVERLAY_LOG_EVERY 2

/* 周期内存探针：默认关闭（每 30 秒会在控制台打 2~3 行）。
   开机那一次仍然保留，启动诊断不受影响；要长期观察内存通路时改成 1。 */
#define FPS_OVERLAY_PERIODIC_PROBE 0

static const char *TAG = "FpsOverlay";

static lv_obj_t *s_label;

/* 本窗口的读数。四个数字回答的是**不同**的问题，别混：
     ui_frames —— 真的画了几次（内容驱动的重绘率，不是面板能力）
     render    —— 每次重绘里**CPU 在画**的时长（FLUSH_START - RENDER_START）
     flush     —— 每次重绘里**等硬件**的时长（FLUSH_FINISH - FLUSH_START）
     gap       —— 相邻两次重绘的间隔（=1/fps）

   ★ 读 flush 之前必须先知道它内含一道 vsync 闸门：lvgl_port 的 flush 回调在
     DIRECT 模式下最后是
         xSemaphoreTake(trans_sem, 0);              // 倒掉旧令牌
         xSemaphoreTake(trans_sem, portMAX_DELAY);  // 等**下一个** vsync
     （esp_lvgl_port_disp.c:746-752，令牌由 DPI 的 on_refresh_done 每个 vsync 给）
     所以每次呈现都被重新对齐到 vsync 上，**帧间隔只能落在 16.68ms 的整数倍上**：

         render + flush < 16.68ms  ⇒  gap ≈ 16.7ms  ⇒  59.9fps（面板满速）
         render + flush > 16.68ms  ⇒  gap ≈ 33.4ms  ⇒  30fps（整整错过一个 vsync）

     也就是说帧率不是"慢慢降"的，而是在 16.68ms 这条线上**断崖**。看到 30 就意味着
     每帧多做了一点点、正好越过线；目标很明确：把 render 压到 16.68 以下。
     实测 50 这种非整倍数，是两种间隔混在一秒里的平均值（面板 59.94、量化到 16.7/33.4）。

   gap min 是最有决策价值的一个数：它就是"这一页最快能到的节奏"，
   min ≈ 17 ⇒ 已经在满速，慢的是内容；min ≈ 33 ⇒ 每帧都超线，是画得慢。 */
static uint32_t s_ui_frames;          /* 本窗口内 LVGL 真实重绘次数 */
static uint32_t s_window_start_ms;
static uint32_t s_win_render_us;
static uint32_t s_win_flush_us;
static uint32_t s_win_max_render_us;
static uint32_t s_win_max_flush_us;
static uint32_t s_win_max_gap_us;
static uint32_t s_win_min_gap_us;     /* 0 = 本窗口还没有样本 */
static uint32_t s_render_start_us;
static uint32_t s_flush_start_us;
static uint32_t s_last_ready_us;
static uint32_t s_log_windows;

/* 单次重绘（一个 refr）内的累计量。为什么不能直接往窗口里累加：一个 refr 可能有
   多段 flush（多个互不相邻的脏区各一次并排画），而窗口里的 avg 是按**重绘次数**
   摊的。把分段时长直接累加进窗口、却拿重绘次数当分母，平均值就会被撑到 max 之上
   —— 实测出现过 "render avg/max 36/23 ms" 这种自相矛盾的数字，就是这里来的。
   改成：refr 内累加，RENDER_READY 时一次性结算进窗口，avg 与 max 就同口径了。 */
static uint32_t s_refr_render_us;
static uint32_t s_refr_flush_us;

/* 重绘一次的完整时序（lv_refr.c 里的事件顺序，已核对）：
     RENDER_START(765) → 画各区 → FLUSH_START(1431) → flush_cb → FLUSH_FINISH(1439)
     → RENDER_READY(833)
   所以 FLUSH_START 这个时刻就是"CPU 画完了"的分界线，不用自己插桩。 */

static inline uint32_t now_us(void)
{
    return (uint32_t)esp_timer_get_time();
}

/* 内存通路探针（定义在文件后半段），定期重跑 —— 理由见 mem_probe 上面那段：
   它是判定"内存通路慢"还是"只有 LVGL 绘制路径慢"的唯一依据，而开机那一次很容易
   被 monitor 缓冲区错过。重跑的好处是**不需要 reset 板子**也能等到这两行。 */
static void mem_probe(lv_display_t *disp);
#if FPS_OVERLAY_PERIODIC_PROBE
static lv_display_t *s_probe_disp;
#endif

/* 探针重跑间隔（按统计窗口数）：60 × 500ms = 30 秒。 */
#define FPS_OVERLAY_PROBE_EVERY 60

/* LVGL 每完成一次重绘发一次这个事件（core/lv_refr.c:833）。

   注意它统计的是"真的画了"而不是"刷新定时器跑了一圈"：事件上游的
   refr_invalid_areas() 第一句就是 `if(disp_refr->inv_p == 0) return;`
   （lv_refr.c:761），没东西要画时根本走不到发事件那行。

   所以静止页面上 UI 读数会贴着 0 —— 这是对的，LVGL 没内容可画就是不画。
   唯一的下限是标签自己：它每 500ms 变一次文字，等于每秒至少制造 2 次重绘，
   所以实测底噪约 2，不是 0。 */
static void render_ready_cb(lv_event_t *event)
{
    (void)event;
    const uint32_t t = now_us();
    s_ui_frames = s_ui_frames + 1;
    if (s_last_ready_us != 0) {
        const uint32_t gap = t - s_last_ready_us;
        if (gap > s_win_max_gap_us) s_win_max_gap_us = gap;
        /* 最小间隔才是这一页**最好**的呈现节奏（原因见本文件开头 s_ui_frames
           上面那段：呈现被 vsync 量化，所以它有 16.7ms 这个地板）。内容静止时的
           空档会把 max 拉得很大、没有解释价值，min 不会。 */
        if (s_win_min_gap_us == 0 || gap < s_win_min_gap_us) s_win_min_gap_us = gap;
    }

    /* 这一次重绘的 render / flush 在这里结算（口径见上面 s_refr_render_us 那段）。 */
    s_win_render_us += s_refr_render_us;
    s_win_flush_us += s_refr_flush_us;
    if (s_refr_render_us > s_win_max_render_us) s_win_max_render_us = s_refr_render_us;
    if (s_refr_flush_us > s_win_max_flush_us) s_win_max_flush_us = s_refr_flush_us;
    s_refr_render_us = 0;
    s_refr_flush_us = 0;

    s_last_ready_us = t;
}

static void render_start_cb(lv_event_t *event)
{
    (void)event;
    s_render_start_us = now_us();
    s_refr_render_us = 0;   /* 新的一次重绘开始，清掉上一次的分段累计 */
    s_refr_flush_us = 0;
}

static void flush_start_cb(lv_event_t *event)
{
    (void)event;
    const uint32_t t = now_us();
    /* 这一句标志"这一段画完了"，把渲染时长结算掉。 */
    const uint32_t d = t - s_render_start_us;
    s_refr_render_us += d;
    s_render_start_us = t;   /* 多段 flush 时，下一段的"画"从这一刻起算 */
    s_flush_start_us = t;
}

static void flush_finish_cb(lv_event_t *event)
{
    (void)event;
    /* flush_cb 已经返回（在 DIRECT/avoid_tearing 下它内含等 vsync）。 */
    s_refr_flush_us += now_us() - s_flush_start_us;
}

static void fps_tick(lv_timer_t *timer)
{
    (void)timer;
    if (!s_label) return;

    const uint32_t now = lv_tick_get();
    const uint32_t elapsed = now - s_window_start_ms;   /* 无符号相减，回绕安全 */
    if (elapsed == 0) return;

    const uint32_t frames = s_ui_frames;
    const uint32_t ui_fps = (uint32_t)((uint64_t)frames * 1000u / elapsed);
    /* 平均时长按"重绘次数"摊；一串静态帧里夹一次重绘时，avg 会被那一次独立撑起来，
       所以同时留 max —— 参照系是 vsync 间隔 16.7ms。 */
    const uint32_t avg_render = frames ? s_win_render_us / frames / 1000u : 0;
    const uint32_t avg_flush = frames ? s_win_flush_us / frames / 1000u : 0;

    lv_label_set_text_fmt(s_label, "CAM %u  UI %u  R%u F%u",
                          (unsigned)xiaowei_camera_get_fps(), (unsigned)ui_fps,
                          (unsigned)avg_render, (unsigned)avg_flush);

#if FPS_OVERLAY_SERIAL_LOG
    if ((++s_log_windows % FPS_OVERLAY_LOG_EVERY) == 0) {
        ESP_LOGI(TAG,
                 "UI %u/s (%u 次/%ums)  render avg/max %u/%u ms  flush avg/max %u/%u ms  gap min/max %u/%u ms  CAM %u",
                 (unsigned)ui_fps, (unsigned)frames, (unsigned)elapsed, (unsigned)avg_render,
                 (unsigned)(s_win_max_render_us / 1000u), (unsigned)avg_flush,
                 (unsigned)(s_win_max_flush_us / 1000u), (unsigned)(s_win_min_gap_us / 1000u),
                 (unsigned)(s_win_max_gap_us / 1000u), (unsigned)xiaowei_camera_get_fps());
    }
#else
    ++s_log_windows;   /* 计数器仍要推进：下面的周期探针靠它计时 */
#endif

    s_ui_frames = 0;
    s_win_render_us = 0;
    s_win_flush_us = 0;
    s_win_max_render_us = 0;
    s_win_max_flush_us = 0;
    s_win_max_gap_us = 0;
    s_win_min_gap_us = 0;
    s_window_start_ms = now;

    /* 定期重跑内存探针：这样不用 reset 板子，随便什么时候开 monitor 都能等到那两行。
       代价是每 30 秒占用 LVGL 任务 ~20ms（丢一帧，看不出来），探针本身不改变帧缓冲
       内容，所以也不会闪。 */
#if FPS_OVERLAY_PERIODIC_PROBE
    if (s_probe_disp && (s_log_windows % FPS_OVERLAY_PROBE_EVERY) == 0) {
        mem_probe(s_probe_disp);
    }
#endif
}

/* ============================ 一次性内存通路探针 ============================
 *
 * 为什么要有这一段（诊断用，定位完就该删）：
 * 实测"整屏重绘"这一类操作的 render 是 13~24ms（相机预览 466×350 ≈ 326KB、机器人
 * 画布 466×466 ≈ 434KB；机器人页那一屏还要先铺一遍底色，等于又过一遍 434KB）。
 * 按像素摊下来是 55~80ns/像素 —— 400MHz 下 20~45 个周期/像素。
 *
 * 一次纯内存搬运不该是这个量级：PSRAM 200MHz 八线（CONFIG_SPIRAM_SPEED_200M）的
 * 理论带宽约 200MB/s，L2 又开着 128KB，434KB 的整块拷贝应该落在 3~5ms 量级。
 * 实测差着 5~10 倍，而这只可能来自两类完全不同的原因，修法也完全相反：
 *
 *   A) 内存通路本身有问题 —— 例如 PSRAM 实际没跑在 200MHz（降频回退），或者面板
 *      帧缓冲那段地址被映射成**非缓存**，于是每写两个字节就是一次独立的 PSRAM
 *      事务。→ 这种情况下怎么调 LVGL 都没用，得从内存/缓存配置下手，或者干脆把
 *      整屏搬运交给硬件（DMA2D/PPA 不经过 CPU 缓存）。
 *   B) 只有 LVGL 的绘制路径慢（逐像素混合、或每帧对目标区做缓存失效，把写变成
 *      "读-改-写"）；单纯的内存搬运仍然是快的。→ 这种情况下才值得去动绘制路径。
 *
 * 所以开机量一次就够分开：同样大小的 memcpy，分别打在 PSRAM 堆和**面板帧缓冲**上，
 * 拿"堆→堆"当基准。三组数一摆，A 还是 B 一目了然。
 *
 * 副作用：会往帧缓冲写一遍**内容完全相同**的数据（先把 fb 读进临时缓冲，再原样
 * 写回），所以屏幕不会花。临时缓冲用完即放（各 434KB，都在 PSRAM）。
 * ======================================================================== */
static void mem_probe(lv_display_t *disp)
{
    const size_t bytes = 466u * 466u * 2u;   /* 和机器人画布同尺寸，方便直接对比 */

    uint8_t *src = (uint8_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    uint8_t *tmp = (uint8_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (!src || !tmp) {
        ESP_LOGW(TAG, "memprobe: 临时缓冲分配失败（各需 %uKB），跳过", (unsigned)(bytes / 1024u));
        if (src) heap_caps_free(src);
        if (tmp) heap_caps_free(tmp);
        return;
    }

    int64_t t0 = esp_timer_get_time();
    memset(src, 0xA5, bytes);
    int64_t t1 = esp_timer_get_time();
    memcpy(tmp, src, bytes);                     /* 堆→堆，冷读 + 热写 */
    int64_t t2 = esp_timer_get_time();
    memcpy(tmp, src, bytes);                     /* 堆→堆，热读 + 热写（接近带宽上限） */
    int64_t t3 = esp_timer_get_time();

    /* 这里 B/µs 正好就是 MB/s，不用再换算。
       全部走 %u：本工程开着 CONFIG_NEWLIB_NANO_FORMAT，64 位格式串不被支持，
       %lld 会把 "lld" 原样打到串口上（用户的日志里 "build ldms" 就是这么来的）。
       这些耗时最多几万 µs，unsigned 绰绰有余。 */
    ESP_LOGI(TAG, "memprobe: %uKB  memset(堆) %uus | memcpy 堆→堆 冷 %uus 热 %uus (%u MB/s)",
             (unsigned)(bytes / 1024u), (unsigned)(t1 - t0), (unsigned)(t2 - t1),
             (unsigned)(t3 - t2), (unsigned)(bytes / (size_t)(t3 - t2 ? (t3 - t2) : 1)));

    lv_draw_buf_t *buf = lv_display_get_buf_active(disp);
    uint8_t *fb = buf ? (uint8_t *)buf->data : NULL;
    /* ⚠ 活动绘制缓冲**不等于**整屏缓冲，长度必须按缓冲自己的 data_size 取。

       这里原来是硬编码 bytes（466×466×2 = 434KB）的：在 avoid_tearing 那条配置下
       活动缓冲就是面板的整屏帧缓冲（434KB），长度正好对上，所以一直没暴露。改成
       LVGL 自己分块绘制（avoid_tearing = false，见 display/lcd_display.cc）之后，
       活动缓冲变成 466×50×2 = 46KB，再按 434KB 去读就是越界 —— 实机直接
       Load access fault（memcpy 源地址越过 0x4ffc0000 的内部 RAM 边界）重启。

       还要清楚这个数现在量的是**谁**：关掉 avoid_tearing 后它是 LVGL 的绘制缓冲
       （buff_dma = true → 内部 RAM），不再是面板帧缓冲（PSRAM）。所以下面两组数
       只说明"绘制缓冲所在那块内存"的搬运速度，不能再当帧缓冲带宽看了。 */
    const size_t fb_bytes = buf ? (size_t)buf->data_size : 0;
    if (fb && fb_bytes) {
        const size_t probe = fb_bytes < bytes ? fb_bytes : bytes;
        int64_t f0 = esp_timer_get_time();
        memcpy(tmp, fb, probe);                  /* 读绘制缓冲 */
        int64_t f1 = esp_timer_get_time();
        memcpy(fb, tmp, probe);                  /* 写绘制缓冲（内容不变） */
        int64_t f2 = esp_timer_get_time();

        ESP_LOGI(TAG, "memprobe: 绘制缓冲 %uKB@%p 读 %uus (%u MB/s) | 写 %uus (%u MB/s)",
                 (unsigned)(probe / 1024u), (void *)fb,
                 (unsigned)(f1 - f0), (unsigned)(probe / (size_t)(f1 - f0 ? (f1 - f0) : 1)),
                 (unsigned)(f2 - f1), (unsigned)(probe / (size_t)(f2 - f1 ? (f2 - f1) : 1)));
    } else {
        ESP_LOGW(TAG, "memprobe: 拿不到活动绘制缓冲，跳过 fb 那两组");
    }

    heap_caps_free(src);
    heap_caps_free(tmp);
}

void xiaowei_fps_overlay_init(void)
{
    if (s_label) return;   /* 只建一次 */

    /* 顶层是 display 的子对象，不属于任何 screen，切页不会碰它。
       LVGL 建它时就摘掉了 LV_OBJ_FLAG_CLICKABLE 并 lv_obj_remove_style_all()
       （core/lv_display.c:159-162），label 本身默认也不可点击 —— 所以压在
       这上面的触摸/滑动照常落到下面的页面上，不会被吞掉。 */
    lv_obj_t *top = lv_layer_top();

    s_label = lv_label_create(top);
    lv_label_set_text(s_label, "CAM --  UI --");
    lv_obj_set_style_text_font(s_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_label, lv_color_white(), 0);
    /* 半透明深底 + 圆角：压在壁纸、照片、表情脸上都读得清，又不至于太抢眼。 */
    lv_obj_set_style_bg_color(s_label, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_label, LV_OPA_40, 0);
    lv_obj_set_style_radius(s_label, 8, 0);
    lv_obj_set_style_pad_hor(s_label, 8, 0);
    lv_obj_set_style_pad_ver(s_label, 2, 0);
    lv_obj_align(s_label, LV_ALIGN_BOTTOM_MID, 0, -8);

    /* 用 top 自己的 display，而不是 lv_display_get_default()：两者现在指向同一个，
       但这样写不依赖"默认 display 就是这一个"这个前提。 */
    lv_display_t *disp = lv_obj_get_display(top);
    lv_display_add_event_cb(disp, render_ready_cb, LV_EVENT_RENDER_READY, NULL);
    lv_display_add_event_cb(disp, render_start_cb, LV_EVENT_RENDER_START, NULL);
    lv_display_add_event_cb(disp, flush_start_cb, LV_EVENT_FLUSH_START, NULL);
    lv_display_add_event_cb(disp, flush_finish_cb, LV_EVENT_FLUSH_FINISH, NULL);

    s_ui_frames = 0;
    s_window_start_ms = lv_tick_get();
    lv_timer_create(fps_tick, FPS_OVERLAY_PERIOD_MS, NULL);

    /* 内存通路基准：只开机打这一次（启动诊断保留）。
       之后是否每 30 秒重打，由 FPS_OVERLAY_PERIODIC_PROBE 决定，默认关闭。
       探针长注释在 mem_probe 上面那一段。 */
#if FPS_OVERLAY_PERIODIC_PROBE
    s_probe_disp = disp;
#endif
    mem_probe(disp);
}
