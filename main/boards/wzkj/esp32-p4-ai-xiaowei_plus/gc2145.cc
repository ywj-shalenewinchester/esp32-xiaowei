/* GC2145 摄像头驱动 —— 两层都在这里，分工见 gc2145.h 的分节注释头。
 *
 *   上半：class Gc2145 的 SCCB 传感器驱动（读 ID + 灌官方初始化表 + 配 VGA）。
 *   下半：DVP 采集管线（LEDC 临时 XCLK、DVP 控制器、ping-pong、缩放、task）
 *         以 xiaowei_camera_* 一族 C 符号导出。
 *
 * 引脚全部取自 config.h 第 4 节，本文件不出现裸 GPIO 号。
 * 错误处理：非致命（*Safe 系列 + ESP_LOGE + 返回错误码），不用 ESP_ERROR_CHECK
 * —— 摄像头起不来只应该是"相机页不可用"，不该把整机 abort 掉。 */

#include "gc2145.h"

#include <string.h>

#include "config.h"
#include "gc2145_cfg.h"
#include "touch.h"          /* xiaowei_camera_i2c_bus()：复用触摸那路 I2C */

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_cache.h"
#include "esp_cam_ctlr.h"
#include "esp_cam_ctlr_dvp.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "Gc2145";

/* ==========================================================================
 *  一、传感器驱动（SCCB / I2C）
 * ========================================================================== */

Gc2145::Gc2145(i2c_master_bus_handle_t bus, uint8_t address) : I2cDevice(bus, address) {}

esp_err_t Gc2145::ReadId(uint16_t* id) {
    if (!id) return ESP_ERR_INVALID_ARG;
    uint8_t hi = 0, lo = 0;
    esp_err_t err = ReadRegSafe(kIdHigh, &hi);
    if (err != ESP_OK) return err;
    err = ReadRegSafe(kIdLow, &lo);
    if (err != ESP_OK) return err;
    *id = (static_cast<uint16_t>(hi) << 8) | lo;
    return ESP_OK;
}

esp_err_t Gc2145::Init() {
    if (init_done_) {
        ESP_LOGW(TAG, "Init() called again — skipping");
        return ESP_OK;
    }
    uint16_t id = 0;
    esp_err_t err = ReadId(&id);
    if (err != ESP_OK) return err;
    if (id != 0x2145) {
        ESP_LOGW(TAG, "unexpected sensor ID 0x%04X", id);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "GC2145 online, ID=0x%04X", id);
    err = ConfigureVgaRgb565();
    if (err == ESP_OK) init_done_ = true;
    return err;
}

// 100% 照搬 esp32-camera 官方 gc2145.c 流程：
//   reset() -> 灌 gc2145_default_init_regs -> set_pixformat(RGB565) -> set_framesize(VGA)
// 官方表即包含 LSC/AWB/CC/AEC/gamma，无需任何额外寄存器修补。
esp_err_t Gc2145::ConfigureVgaRgb565() {
    /* 写一个寄存器，失败就带寄存器号报错并返回。
       这里**故意不用** ESP_ERROR_CHECK：板级约定是外设操作一律非致命
       （对齐 I2cDevice 的 *Safe 系列，见 sy6206.cc 的分节注释头），
       否则"GC2145 没插好"会升级成 abort，整机起不来。 */
    auto wr = [this](uint8_t reg, uint8_t value) -> esp_err_t {
        esp_err_t err = WriteRegSafe(reg, value);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "write 0x%02X = 0x%02X failed: %s", reg, value, esp_err_to_name(err));
        }
        return err;
    };

    // 1. reset()：软件复位
    esp_err_t err = wr(0xfe, 0xe0);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(100));

    // 2. 灌入官方 init 表（gc2145_default_init_regs）
    for (size_t i = 0; i < sizeof(gc2145_init_reg_tbl) / sizeof(gc2145_init_reg_tbl[0]); ++i) {
        err = wr(gc2145_init_reg_tbl[i][0], gc2145_init_reg_tbl[i][1]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "init table entry %u (0x%02X) failed",
                     (unsigned)i, gc2145_init_reg_tbl[i][0]);
            return err;
        }
    }
    vTaskDelay(pdMS_TO_TICKS(100));

    // 3. set_pixformat(PIXFORMAT_RGB565)：0x84 低 5 位 = 6，高位保持
    err = wr(0xfe, 0x00);
    if (err != ESP_OK) return err;
    uint8_t v84 = 0;
    err = ReadRegSafe(0x84, &v84);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "read 0x84 failed: %s", esp_err_to_name(err));
        return err;
    }
    v84 = (v84 & 0xE0) | 0x06;
    err = wr(0x84, v84);
    if (err != ESP_OK) return err;

    // 4. set_framesize(FRAMESIZE_VGA)：官方子采样模式。
    //    UXGA 1600x1200 -> 1/2 子采样窗口 1280x960 -> VGA 640x480。
    //    官方 subsample_cfgs 中 1/2 项：0x99=0x22，0x9b-0xa2 全 0。
    //    窗口居中：row_s=(1200-960)/2=120，col_s=(1600-1280)/2=160。
    //    （顺序照官方，别重排：裁剪窗口要在子采样使能之前写好。）
    static const uint8_t kVgaCrop[][2] = {
        {0x90, 0x01},   // P0_CROP_ENABLE
        {0x09, 0x00},   // row_s H8
        {0x0a, 0x78},   // row_s L8 = 120
        {0x0b, 0x00},   // col_s H8
        {0x0c, 0xa0},   // col_s L8 = 160
        {0x0d, 0x03},   // win_h+8 H8
        {0x0e, 0xc8},   // win_h+8 L8 = 968
        {0x0f, 0x05},   // win_w+16 H8
        {0x10, 0x10},   // win_w+16 L8 = 1296
        {0x99, 0x22},   // 1/2 子采样
        {0x9b, 0x00}, {0x9c, 0x00}, {0x9d, 0x00}, {0x9e, 0x00},
        {0x9f, 0x00}, {0xa0, 0x00}, {0xa1, 0x00}, {0xa2, 0x00},
        {0x95, 0x01}, {0x96, 0xe0},   // out H = 480
        {0x97, 0x02}, {0x98, 0x80},   // out W = 640
    };
    for (size_t i = 0; i < sizeof(kVgaCrop) / sizeof(kVgaCrop[0]); ++i) {
        err = wr(kVgaCrop[i][0], kVgaCrop[i][1]);
        if (err != ESP_OK) return err;
    }
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "GC2145 configured (esp32-camera official): VGA RGB565 640x480, 1/2 subsample");
    return ESP_OK;
}

/* ==========================================================================
 *  二、DVP 采集管线
 *
 *  给 GC2145 的 MCLK。传感器的 PLL 以它为基准，所以出帧率随它成正比；
 *  原来的 10MHz 是这条链路上除软件节流外的第二道人为上限。
 *
 *  为什么是 20MHz 而不是更高（别随手往上调）：
 *   - P4 的 DVP 时钟源是 PLL_F160M = 160MHz（soc/esp32p4/include/soc/clk_tree_defs.h:398
 *     的 CAM_CLK_SRC_DEFAULT），而驱动只做**整数分频**：
 *     esp_cam_ctlr_dvp_output_clock() 里 `if ((src_clk_hz % xclk_freq) == 0)`，
 *     除不尽就只打一条 ESP_LOGE 并返回 ESP_ERR_INVALID_ARG（esp_cam_ctlr_dvp_cam.c:387-396），
 *     而调用方 esp_cam_new_dvp_ctlr 是 ESP_GOTO_ON_ERROR 接的（同文件 :898）
 *     → 错误一路抛上来，摄像头整个起不来。**24MHz 就是这么被拒的**（160%24=16MHz≠0）。
 *     160MHz ÷ N 的合法值：80 / 40 / 32 / 20 / 16 / 10 / 8 / 5 / 4 …
 *   - 合法值里比 20MHz 大的只剩 32 / 40MHz。GC2145 常见配置的 MCLK 上限在 27MHz 量级
 *     （esp32-camera 的参考配置也都在 24MHz 附近），32MHz 已经出界，不试。
 *   所以 20MHz（160÷8）是既过驱动整数分频、又留在 MCLK 合理范围内的最大值。
 *
 *  ⚠️ 这个值还受**第二道**约束：xiaowei_camera_init() 里那个 LEDC 临时时钟也必须能
 *  配出同一个频率，否则 ledc_timer_config 失败 → init 返回 false → 摄像头整个起不来
 *  （症状是相机页全黑，日志里刷 start_stream: DVP not initialized）。
 *  两道约束的算式和推导写在 init 里 LEDC 那一段，改这个常量前一起看。
 *
 *  注：这一改只解开 MCLK 这道限制。GC2145 真正的出帧率还受帧长寄存器约束 ——
 *  本工程的 VGA 是"UXGA 表 + 裁剪 1280×960 + 1/2 子采样"（见上面的
 *  ConfigureVgaRgb565），子采样只减少输出像素、不缩短帧时间，所以能不能到 60fps
 *  要看传感器本身。实测数字看每秒那行 `CAM fps=` 日志。 */
static constexpr uint32_t CAM_XCLK_HZ = 20000000;

constexpr int CAM_W = 640, CAM_H = 480;
constexpr int DST_W = 466, DST_H = 350;          // 466*3/4 ≈ 350
constexpr size_t DST_FRAME_SIZE = DST_W * DST_H * 2;
constexpr int kLcdBuffers = 2;                   // 输出缓冲块数，见 s_lcd_frame 声明处

/* ---- DVP 引脚 ----
 *
 * 全部来自 config.h 第 4 节的 CAM_*_PIN —— 本文件不出现裸 GPIO 号。
 *
 * 那段宏曾经按原理图写成另一套（XCLK 和 HSYNC 正好对调），实机上相机页打不开；
 * 已按实测网表校正，config.h 那一节开头有记录。以后要动引脚只改 config.h，
 * 不要在这里另抄一份 —— 两处不一致时，LEDC 会按 A 脚配传感器、DVP 按 B 脚跑时钟。
 *
 * 唯一要留神的是 XCLK：xiaowei_camera_init() 里 LEDC 先临时驱动它给 GC2145 供
 * MCLK 以便配 SCCB，配完释放、由 DVP 控制器接管同一个脚。两处都用 CAM_XCLK_PIN。 */

/* 双缓冲 ping-pong：DMA 交替写 buf[0]/buf[1]，task 读另一块。 */
static uint8_t *s_frame_buf[2] = {nullptr, nullptr};
static size_t s_frame_buf_size = 0;
static volatile uint8_t s_dma_idx = 0;
static volatile int8_t s_ready_idx = -1;
static volatile size_t s_frame_size = 0;

/* 缩放后的 466×350 RGB565 PSRAM，**多块轮转**，不是一块。

   原来只有一块：采集 task 每帧自上而下整块重写它，而 LVGL 同时也在自上而下
   整块读它（466×350 按 draw buffer 切成 7 段绘制）。两边都在跑、扫描方向还一样，
   撞上是常态而不是意外 —— 撞在哪一行，那一行以上是新帧、以下是旧帧。手一晃就是
   一条横带，静置时前后两帧内容相同所以看不见。

   现在：task 只写 back，整块写完才把 front 换成它、并抬 s_frame_seq；
   UI 侧 xiaowei_camera_get_frame() 返回 front。task 此后绝不再碰 front 那块，
   两边从此不同时持有同一块内存 —— 是"交接"，不是"加锁"。
   （加锁不行：采集写一整块要好几毫秒，锁住等于把 LVGL 按住同样久，更糟。）

   为什么 2 块够：同一块要被重写，得先隔 kLcdBuffers-1 次发布。按当前节奏
   （UI 每 16ms 取一次、LVGL 单次绘制约 10ms、采集每帧 16~33ms）余量在 20ms 以上。
   若以后 UI 单次绘制变长，把 kLcdBuffers 改成 3 即可 —— 下面全是按它轮转的，
   没有写死的 0/1。 */
static uint8_t *s_lcd_frame[kLcdBuffers] = {};
static uint8_t s_lcd_back = 0;                    // task 正在写这块
static volatile uint8_t s_lcd_front = 0;          // 最近发布、UI 该读的那块
static esp_cam_ctlr_handle_t s_cam = nullptr;
static TaskHandle_t s_task = nullptr;
static volatile bool s_paused = false;
static volatile bool s_running = false;       // init 成功后恒为 true（没有 deinit）
static volatile bool s_streaming = false;     // 视频流是否开启；false 时 task 只睡眠不处理帧

static volatile uint32_t s_frame_seq = 0;     // 每写出一帧 +1（UI 判新帧用）
static volatile uint32_t s_fps = 0;           // 最近 1 秒写出的帧数

/* ISR 里数"传感器到底产出了几帧"，与 task 消费掉几帧分开计。

   这两个数**必须分开**，否则"相机只有 11fps"这句话没法归因：
     dvp ≈ 30 而 CAM fps ≈ 11  ⇒ 传感器没问题，是采集 task 这条链路跟不上；
     dvp ≈ 11                  ⇒ 传感器自己在 11fps（帧长寄存器/XCLK 那一路），
                                  task 再快也没用。
   32 位对齐量的单写单读不需要临界区。 */
static volatile uint32_t s_dvp_frames = 0;

/* 水平最近邻映射表：dst 列号 -> src 列号。只由分辨率决定、与帧无关，
   初始化时算一次即可 —— 放进内循环每像素省一次乘法+除法。 */
static uint16_t s_xmap[DST_W];

/* ---- ISR 回调（管线移植自 aw86937 main_camera.cpp 的对应两段）---- */

static bool IRAM_ATTR on_get_frame(esp_cam_ctlr_handle_t, esp_cam_ctlr_trans_t *trans, void *)
{
    uint8_t next = s_dma_idx ^ 1;
    s_dma_idx = next;
    trans->buffer = s_frame_buf[next];
    trans->buflen = s_frame_buf_size;
    return false;
}

static bool IRAM_ATTR on_frame_done(esp_cam_ctlr_handle_t, esp_cam_ctlr_trans_t *trans, void *)
{
    /* 哪一块缓冲刚写满，用 trans->buffer **本身**判断，不要用 `1 - s_dma_idx` 反推。

       `1 - s_dma_idx` 只在"on_get_frame 还没为下一帧换过缓冲"时成立，而这个先后
       顺序驱动并没有承诺。推错就是**永远晚一帧**：task 拿到的是上一帧那块，
       而 DMA 紧接着要写的恰好是 task 正在读的这块 —— ping-pong 只有两块，
       读写撞在同一块上，横带撕裂（静置时前后帧内容相同，看不出来；手一晃就现）。

       trans 是**刚完成**的那一笔传输（s_frame_size 也是从它身上取的），
       所以 trans->buffer 就是刚写满的那块，与任何回调顺序假设无关。

       两个缓冲都比对、都不匹配就**什么都不发布**：宁可这一帧不要（task 那边
       s_ready_idx 保持 -1，它会继续等），也不要拿一个猜出来的下标去读。
       先写 s_frame_size、再写 s_ready_idx：task 看到 >= 0 时尺寸一定已就位。 */
    s_dvp_frames = s_dvp_frames + 1;
    s_frame_size = trans->received_size;
    if (trans->buffer == s_frame_buf[0]) {
        s_ready_idx = 0;
    } else if (trans->buffer == s_frame_buf[1]) {
        s_ready_idx = 1;
    }
    return false;
}

/* ---- 采集 task：循环取帧 → 缓存同步 → 缩放+字节交换到 s_lcd_frame ---- */

static void camera_task(void *)
{
    uint32_t frame_no = 0;
    uint32_t wait_ticks = 0;
    /* 1 秒滑动窗口的出帧率统计 */
    uint32_t fps_frames = 0;
    uint32_t drop_frames = 0;      // 本窗口里因收帧不完整而丢掉的帧
    uint32_t dvp_last = 0;         // 上一次结算时 ISR 那边的累计帧数
    int64_t fps_window_start = esp_timer_get_time();

    while (s_running) {
        if (!s_streaming) {
            /* 视频流关闭（用户不在摄像头页）：安静睡眠，等 start_stream 唤醒。
               帧率归零 —— 否则页面上会一直挂着上一次的读数。 */
            fps_frames = 0;
            drop_frames = 0;
            /* dvp_last 也要跟着贴住当前累计值：s_dvp_frames 是 ISR 那边一直累加的，
               停在 0 的话，重新开流后的**第一行**日志会把上一轮遗留的总数当成这
               一秒的产出（实测出现过 "dvp=881" 这种假数），要等下一秒才恢复。
               不是提前贴住而是贴住此刻的值，开流后的第一行就是干净的一秒增量。 */
            dvp_last = s_dvp_frames;
            fps_window_start = esp_timer_get_time();
            s_fps = 0;
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        int8_t ready = s_ready_idx;
        if (ready < 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
            if ((++wait_ticks % 500) == 0) {
                ESP_LOGW(TAG, "waiting for DVP frame (%u ms)",
                         (unsigned)(wait_ticks * 10));
            }
            continue;
        }
        wait_ticks = 0;
        s_ready_idx = -1;
        if (s_frame_size != s_frame_buf_size) {
            ++drop_frames;
            /* 注意这个 if 只在 frame_no 上取模：连续丢帧时 frame_no 不涨，
               所以它一开始会每次必打（0 % 30 == 0）。真正的丢帧率看下面
               每秒那行的 drop= 字段，别靠这条日志数。 */
            if ((frame_no % 30) == 0) {
                ESP_LOGW(TAG, "discard incomplete frame: %u/%u",
                         (unsigned)s_frame_size, (unsigned)s_frame_buf_size);
            }
            continue;
        }
        const uint8_t *p = s_frame_buf[ready];
        esp_cache_msync((void *)p, s_frame_buf_size,
                        ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_INVALIDATE);

        if (!s_paused && s_lcd_frame[0]) {
            /* 最近邻缩小 640×480 → 466×350，并做 RGB565 字节交换
               （DVP 给的是高字节在前，LVGL RGB565 期望低字节在前）。

               按 16 位（一个像素）而不是 4 次字节读写来做，源列号查 s_xmap 表 ——
               每帧 466×350 = 163k 个像素，60fps 下就是 9.8M 像素/s，这里省下的
               每像素一次乘法+除法 + 两次字节访问是能不能站住 60fps 的关键。
               32 位读写在这里不安全：dst 行首 466*2 字节是 4 的倍数、src 行首
               640*2 也是，但 dst 宽度为奇数像素会让"两像素一组"跨行错位，
               收益不值这个风险。 */
            uint8_t *dst = s_lcd_frame[s_lcd_back];   /* 只写 back，绝不碰 front */
            for (int dy = 0; dy < DST_H; ++dy) {
                int sy = dy * CAM_H / DST_H;
                const uint16_t *src_row =
                    reinterpret_cast<const uint16_t *>(p + (size_t)sy * CAM_W * 2);
                uint16_t *dst_row =
                    reinterpret_cast<uint16_t *>(dst + (size_t)dy * DST_W * 2);
                for (int dx = 0; dx < DST_W; ++dx) {
                    uint16_t v = src_row[s_xmap[dx]];
                    dst_row[dx] = (uint16_t)((v >> 8) | (v << 8));
                }
            }
            /* 整块写完了才发布，而且顺序不能反 —— 先换 front，再抬 seq。
               UI 是**先读 seq、再取指针**的（camera_app.c 的 camera_tick /
               capture_tick），seq 没变它就早退、什么都不画，所以
               "新指针 + 旧 seq" 完全无害；反过来 "旧指针 + 新 seq" 会让它
               拿着旧帧却记下新帧号，下一 tick 直接跳过 —— 白丢一帧。
               front 是单字节写，单核上原子。 */
            s_lcd_front = s_lcd_back;
            s_lcd_back = (uint8_t)((s_lcd_back + 1) % kLcdBuffers);
            /* 不用 ++：C++20 起对 volatile 的自增是废弃写法 */
            s_frame_seq = s_frame_seq + 1;
            ++fps_frames;
        }

        /* 出帧率：每秒结算一次，顺手当"传感器还活着"的串口探针。

           dvp 是 ISR 数的**传感器产出**，CAM fps 是 task **消化掉**的，drop 是被
           判为收帧不完整的。三者一比就能定位瓶颈在哪一段（见 s_dvp_frames 处）。 */
        int64_t now = esp_timer_get_time();
        int64_t elapsed = now - fps_window_start;
        if (elapsed >= 1000000) {
            const uint32_t dvp_now = s_dvp_frames;
            const uint32_t dvp_fps =
                (uint32_t)((uint64_t)(dvp_now - dvp_last) * 1000000 / elapsed);  // 无符号相减，回绕安全
            s_fps = (uint32_t)((uint64_t)fps_frames * 1000000 / elapsed);
            ESP_LOGI(TAG, "CAM fps=%u dvp=%u drop=%u (frame #%u)",
                     (unsigned)s_fps, (unsigned)dvp_fps, (unsigned)drop_frames, (unsigned)frame_no);
            fps_frames = 0;
            drop_frames = 0;
            dvp_last = dvp_now;
            fps_window_start = now;
        }

        /* 传感器体检：整帧扫一遍数非零字节，能区分"全黑"和"没数据"。
           原来每 60 帧扫一次，614KB/次 —— 10~30fps 时无所谓，到 60fps 就是
           每秒白扫 614KB PSRAM，改成 300 帧（60fps 下 5 秒一次）。 */
        if ((frame_no % 300) == 0) {
            size_t nonzero = 0;
            for (size_t i = 0; i < s_frame_buf_size; ++i) nonzero += (p[i] != 0);
            ESP_LOGI(TAG, "frame #%u: nonzero=%u/%u first=%02X%02X%02X%02X",
                     (unsigned)frame_no, (unsigned)nonzero,
                     (unsigned)s_frame_buf_size,
                     p[0], p[1], p[2], p[3]);
        }
        frame_no++;
    }
    vTaskDelete(nullptr);
}

/* ==========================================================================
 *  三、对外的 C 接口
 * ========================================================================== */

extern "C" bool xiaowei_camera_init(void)
{
    if (s_running) return true;

    /* 1. RESET/PWDN GPIO */
    gpio_config_t ctrl = {};
    ctrl.pin_bit_mask = (1ULL << CAM_RST_PIN) | (1ULL << CAM_PWDN_PIN);
    ctrl.mode = GPIO_MODE_OUTPUT;
    ctrl.pull_up_en = GPIO_PULLUP_DISABLE;
    ctrl.pull_down_en = GPIO_PULLDOWN_DISABLE;
    if (gpio_config(&ctrl) != ESP_OK) {
        ESP_LOGE(TAG, "GPIO config failed");
        return false;
    }
    gpio_set_level(CAM_PWDN_PIN, 0);
    gpio_set_level(CAM_RST_PIN, 0);

    /* 2. 临时输出 XCLK（LEDC）唤醒 GC2145 SCCB，DVP 初始化后释放。
       频率必须和下面 DVP 的 xclk_freq 一致：传感器是在这个时钟下被配置的，
       两处不一致等于"按 A 时钟配、按 B 时钟跑"。用同一个常量。
       引脚同理 —— 这里和 dvp_cfg.pin->xclk_io 必须是同一个宏，
       LEDC 释放后由 DVP 控制器接管同一个脚继续供 MCLK。

       ⚠️ duty_resolution 和 clk_cfg 不是随便填的，它们共同决定了这个频率能不能配出来。
       LEDC 的可达频率上限是 `freq < src_clk / 2^N`（N = duty_resolution 位数），
       因为驱动只接受 div = (src<<8)/(freq * 2^N) 落在 (255, MAX] 区间内的解
       （ledc.c:115 的 LEDC_IS_DIV_INVALID 把 div <= 255 判为非法，<=255 等价于
       真实分频比 <= 1，即"要求输出比源时钟还快"）。

       P4 的 LEDC 源只有 40MHz XTAL / 80MHz PLL_F80M / ~17.5MHz RC_FAST
       （clk_tree_defs.h:703 的 SOC_LEDC_CLKS），最高也就是 80MHz。于是：
         N=3（原来）→ 上限 80/8 = 10MHz，10MHz 恰好压线（80MHz 源，div=256，
                       只比非法阈值 255 大 1）—— 所以原来能跑，但也只能跑 10MHz。
         N=3 配 20MHz → XTAL 64 / PLL 128 / RC_FAST 28，**三个源全非法**，
                       ledc_timer_config 返回 ESP_ERR_INVALID_ARG，
                       摄像头整个起不来（就是"进相机页全黑 + 日志刷
                       start_stream: DVP not initialized"的那次事故）。
         N=1（现在）+ PLL_F80M，20MHz → div=(80e6<<8)/(20e6*2)=512，合法且余量充足。

       改这里之前先按上面的式子算一遍，别再撞一次。 */
    ledc_timer_config_t xclk_timer = {};
    xclk_timer.speed_mode = LEDC_LOW_SPEED_MODE;
    xclk_timer.timer_num = LEDC_TIMER_0;
    xclk_timer.duty_resolution = LEDC_TIMER_1_BIT;   /* N=1 → 可达上限 80/2 = 40MHz */
    xclk_timer.freq_hz = CAM_XCLK_HZ;
    /* 不用 LEDC_AUTO_CLK：它会从 XTAL 开始试，XTAL 下 20MHz 的 div 正好是 256，
       只比非法阈值大 1，没有余量。显式指定 80MHz 源，div=512。 */
    xclk_timer.clk_cfg = LEDC_USE_PLL_DIV_CLK;
    if (ledc_timer_config(&xclk_timer) != ESP_OK) {
        ESP_LOGE(TAG, "LEDC timer config failed: can't produce %u Hz at %d-bit "
                      "resolution from an LEDC source clock",
                 (unsigned)CAM_XCLK_HZ, (int)LEDC_TIMER_1_BIT + 1);
        return false;
    }
    ledc_channel_config_t xclk_channel = {};
    xclk_channel.gpio_num = CAM_XCLK_PIN;
    xclk_channel.speed_mode = LEDC_LOW_SPEED_MODE;
    xclk_channel.channel = LEDC_CHANNEL_0;
    xclk_channel.intr_type = LEDC_INTR_DISABLE;
    xclk_channel.timer_sel = LEDC_TIMER_0;
    xclk_channel.duty = 1;   /* 2^1 的一半 = 50% 占空比，与原来 3bit/duty=4 等价 */
    if (ledc_channel_config(&xclk_channel) != ESP_OK) {
        ESP_LOGE(TAG, "LEDC channel config failed");
        return false;
    }
    gpio_set_level(CAM_RST_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(20));

    /* 3. 复用触摸初始化好的 I2C 总线，挂 GC2145 + 配置 VGA RGB565 */
    i2c_master_bus_handle_t bus = xiaowei_camera_i2c_bus();
    if (!bus) {
        ESP_LOGE(TAG, "CAM_TOUCH_I2C bus not ready; touch init must run first");
        return false;
    }
    Gc2145 sensor(bus, Gc2145::kAddress);
    esp_err_t err = sensor.Init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GC2145 init failed: %s", esp_err_to_name(err));
        ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
        return false;
    }

    /* 4. 释放 LEDC，DVP 控制器会接管同一个脚作 XCLK */
    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);

    /* 5. DVP 640×480 RGB565 ping-pong。引脚全部来自 config.h 第 4 节。
       data_io[0..7] 对应 D0..D7，顺序不能乱 —— 换序等于把通道位接错，
       画面会出现规律性的色偏/条纹而不是"出不来图"，很容易被误判成别的问题。 */
    esp_cam_ctlr_dvp_pin_config_t pins = {
        .data_width = CAM_CTLR_DATA_WIDTH_8,
        .data_io = {CAM_D0_PIN, CAM_D1_PIN, CAM_D2_PIN, CAM_D3_PIN,
                    CAM_D4_PIN, CAM_D5_PIN, CAM_D6_PIN, CAM_D7_PIN},
        .vsync_io = CAM_VSYNC_PIN,
        .de_io = CAM_HSYNC_PIN,
        .pclk_io = CAM_PCLK_PIN,
        .xclk_io = CAM_XCLK_PIN,
    };
    esp_cam_ctlr_dvp_config_t dvp_cfg = {
        .ctlr_id = 0, .clk_src = CAM_CLK_SRC_DEFAULT,
        .h_res = CAM_W, .v_res = CAM_H,
        .input_data_color_type = CAM_CTLR_COLOR_RGB565,
        .cam_data_width = 8,
        .bit_swap_en = false, .byte_swap_en = false,
        .bk_buffer_dis = 1, .pin_dont_init = false,
        .pic_format_jpeg = false, .external_xtal = false,
        .dma_burst_size = 64, .xclk_freq = CAM_XCLK_HZ, .pin = &pins,
    };
    err = esp_cam_new_dvp_ctlr(&dvp_cfg, &s_cam);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "DVP controller init failed: %s", esp_err_to_name(err));
        return false;
    }
    err = esp_cam_ctlr_get_frame_buffer_len(s_cam, &s_frame_buf_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "get frame buffer len failed: %s", esp_err_to_name(err));
        esp_cam_ctlr_del(s_cam); s_cam = nullptr;
        return false;
    }
    s_frame_buf[0] = static_cast<uint8_t *>(
        heap_caps_malloc(s_frame_buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA));
    s_frame_buf[1] = static_cast<uint8_t *>(
        heap_caps_malloc(s_frame_buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA));
    if (!s_frame_buf[0] || !s_frame_buf[1]) {
        ESP_LOGE(TAG, "frame buffer alloc failed (2 x %u)", (unsigned)s_frame_buf_size);
        free(s_frame_buf[0]); s_frame_buf[0] = nullptr;
        free(s_frame_buf[1]); s_frame_buf[1] = nullptr;
        esp_cam_ctlr_del(s_cam); s_cam = nullptr;
        s_frame_buf_size = 0;
        return false;
    }
    ESP_LOGI(TAG, "frame buffers: buf[0]=%p buf[1]=%p size=%u",
             s_frame_buf[0], s_frame_buf[1], (unsigned)s_frame_buf_size);

    esp_cam_ctlr_evt_cbs_t stream_cbs = {
        .on_get_new_trans = on_get_frame,
        .on_trans_finished = on_frame_done,
    };
    err = esp_cam_ctlr_register_event_callbacks(s_cam, &stream_cbs, nullptr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register DVP cbs failed: %s", esp_err_to_name(err));
        goto fail_buf;
    }
    /* 开机即 enable + start DVP，并一直保持运行：DMA ping-pong 持续运转，
       进出摄像头页只靠 s_streaming 标志控制 task 是否把帧缩放到 s_lcd_frame。
       不在 start/stop_stream 里调 stop/disable —— esp_cam_ctlr_dvp 在 stop 后
       无法通过 enable+start 恢复连续 DMA 传输，会导致二次进入只出一帧就停住。 */
    err = esp_cam_ctlr_enable(s_cam);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "DVP enable: %s", esp_err_to_name(err));
        goto fail_buf;
    }
    err = esp_cam_ctlr_start(s_cam);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "DVP start: %s", esp_err_to_name(err));
        esp_cam_ctlr_disable(s_cam);
        goto fail_buf;
    }

    /* 输出缓冲 kLcdBuffers 块（为什么多块见 s_lcd_frame 的声明处）。
       合计 kLcdBuffers × 466×350×2 ≈ 652KB PSRAM。 */
    for (int i = 0; i < kLcdBuffers; ++i) {
        s_lcd_frame[i] = static_cast<uint8_t *>(
            heap_caps_malloc(DST_FRAME_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA));
        if (!s_lcd_frame[i]) {
            ESP_LOGE(TAG, "lcd frame alloc failed (buffer %d/%d, %u each)",
                     i, kLcdBuffers, (unsigned)DST_FRAME_SIZE);
            goto fail_buf;
        }
        memset(s_lcd_frame[i], 0, DST_FRAME_SIZE);
        ESP_LOGI(TAG, "lcd frame[%d] = %p (%u bytes)", i, s_lcd_frame[i],
                 (unsigned)DST_FRAME_SIZE);
    }
    /* 起手 front 和 back 都指向 0：UI 先看到的是全 0 的黑帧，
       第一次发布之后两者才会分开。 */
    s_lcd_back = 0;
    s_lcd_front = 0;

    /* 水平最近邻映射表：算出每个目标列对应的源列，之后每帧直接查 */
    for (int dx = 0; dx < DST_W; ++dx) s_xmap[dx] = (uint16_t)(dx * CAM_W / DST_W);

    s_paused = false;
    s_streaming = false;
    s_running = true;
    s_dma_idx = 0;
    s_ready_idx = -1;
    s_frame_size = 0;
    s_frame_seq = 0;
    s_fps = 0;

    /* 绑 core 0、优先级 5。注意 LVGL 那边是**故意不绑核**的
       （见 display/lcd_display.cc 的 MipiLcdDisplay 构造函数：绑 core 1 会踩
       esp_emote_gfx 那把没加锁的 gfx_timer_handler，Load access fault），
       优先级 4 —— 所以两者都可能落在 core 0 上，采集任务会抢占渲染。
       画面帧率上去了、显示帧率反而掉下来。这是已知的待修项。 */
    if (xTaskCreatePinnedToCore(camera_task, "cam_stream", 8192, nullptr, 5, &s_task, 0) != pdPASS) {
        ESP_LOGE(TAG, "camera task create failed");
        goto fail_buf;
    }
    ESP_LOGI(TAG, "camera initialized (stream idle), 640x480 -> %dx%d RGB565", DST_W, DST_H);
    return true;

fail_buf:
    /* 也负责回收输出缓冲：走到这里的路径不一定分配过它（s_lcd_frame 是静态
       零初始化的），free(nullptr) 是空操作，所以统一在这里收。 */
    for (int i = 0; i < kLcdBuffers; ++i) { free(s_lcd_frame[i]); s_lcd_frame[i] = nullptr; }
    free(s_frame_buf[0]); s_frame_buf[0] = nullptr;
    free(s_frame_buf[1]); s_frame_buf[1] = nullptr;
    s_frame_buf_size = 0;
    esp_cam_ctlr_del(s_cam); s_cam = nullptr;
    return false;
}

extern "C" bool xiaowei_camera_start_stream(void)
{
    if (!s_cam) {
        ESP_LOGE(TAG, "start_stream: DVP not initialized (init at boot failed?)");
        return false;
    }
    /* DVP 一直 enable+start（开机即开），这里只切标志位让 task 开始把帧
       缩放到 s_lcd_frame；不碰控制器状态机，避免 stop 后无法重启 DMA。 */
    s_dma_idx = 0;
    s_ready_idx = -1;
    s_frame_size = 0;
    s_paused = false;
    s_streaming = true;
    ESP_LOGI(TAG, "camera stream started");
    return true;
}

extern "C" void xiaowei_camera_stop_stream(void)
{
    if (!s_cam) return;
    /* 只切标志：task 不再把帧写到 s_lcd_frame（保留最后一帧画面）。
       DVP 控制器保持 enable+start，DMA 继续 ping-pong，下次进页面
       直接 s_streaming=true 即可恢复刷新。 */
    s_streaming = false;
    ESP_LOGI(TAG, "camera stream stopped");
}

extern "C" bool xiaowei_camera_is_streaming(void)
{
    /* 给"临时把流拉起来拍一张、拍完收回去"用：调用方要先知道自己是不是
       把别人的流给关了。 */
    return s_streaming;
}

extern "C" const uint8_t *xiaowei_camera_get_frame(void)
{
    /* 返回**当前已发布的那一块**，不是"刚写完的那一块"。采集 task 只写 back，
       所以拿到的这块在下次发布之前不会被动过 —— UI 尽管交给 LVGL 慢慢画。
       连续两次调用返回的指针可能不同（轮转），调用方每次都重新取。 */
    return s_lcd_frame[s_lcd_front];
}

extern "C" void xiaowei_camera_set_paused(bool paused)
{
    s_paused = paused;
}

extern "C" uint32_t xiaowei_camera_get_fps(void)
{
    return s_fps;
}

extern "C" uint32_t xiaowei_camera_get_frame_seq(void)
{
    return s_frame_seq;
}
