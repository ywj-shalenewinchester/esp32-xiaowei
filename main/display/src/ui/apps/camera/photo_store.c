#include "photo_store.h"

#include "../../platform/xiaowei_ui_hw.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <string.h>

static const char *TAG = "PHOTO";

/* 相机出图尺寸。和 gc2145.cc 里的 DST_W/DST_H 是同一对数字 —— 那边是文件内的
   constexpr 没有导出，而 UI 这侧本来也一直是照着写死的（camera_app.c 的
   lv_image_dsc_t、相册描述符都是）。 */
#define CAM_FRAME_W 466
#define CAM_FRAME_H 350

/* 冷启动时等相机出第一帧的上限（10ms x 30 = 300ms）。30fps 下正常两三帧就到了。 */
#define CAM_WAIT_TRIES 30
#define CAM_WAIT_MS    10

/* 暂停之后再等这么久才取帧：让采集任务手上那一帧写完，否则正在写的和要拷的
   可能是同一块，拷出来半新半旧。和原来拍照键里的 60ms 是同一个道理。 */
#define CAM_SETTLE_MS  60

static xiaowei_photo_t s_photos[XIAOWEI_PHOTO_CAPACITY];
static int s_count;                  /* 有效张数，0..CAPACITY */
static SemaphoreHandle_t s_lock;

/* 写入方分属不同任务：快门在 LVGL 任务、AI 拍照在 Application 主任务、
   AI 绘画也在主任务（DisplayImage）。这是一把叶子锁 —— 里面不调任何会
   反过来取 LVGL 锁 / DisplayLockGuard 的接口，不会成环。 */
static SemaphoreHandle_t photo_lock(void)
{
    if (s_lock == NULL) {
        static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
        portENTER_CRITICAL(&mux);
        if (s_lock == NULL) s_lock = xSemaphoreCreateMutex();
        portEXIT_CRITICAL(&mux);
    }
    return s_lock;
}

static void photo_release(xiaowei_photo_t *photo)
{
    if (photo->pixels) heap_caps_free(photo->pixels);
    memset(photo, 0, sizeof(*photo));
}

/* 只在拿着锁的时候调。 */
static uint8_t *photo_alloc(size_t bytes)
{
    return (uint8_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

bool xiaowei_photo_store_add(const uint8_t *pixels, int w, int h, int stride,
                             xiaowei_photo_kind_t kind)
{
    if (!pixels || w <= 0 || h <= 0 || stride < w * 2) return false;

    const size_t bytes = (size_t)stride * (size_t)h;
    SemaphoreHandle_t lock = photo_lock();
    if (lock) xSemaphoreTake(lock, portMAX_DELAY);

    uint8_t *copy = photo_alloc(bytes);
    if (!copy && s_count == XIAOWEI_PHOTO_CAPACITY) {
        /* 满了且这一次分配没成：腾掉最早那张再试。腾的动作放在锁里做，
           免得两个任务同时腾掉两张。 */
        photo_release(&s_photos[XIAOWEI_PHOTO_CAPACITY - 1]);
        s_count--;
        copy = photo_alloc(bytes);
    }
    if (!copy) {
        if (lock) xSemaphoreGive(lock);
        ESP_LOGW(TAG, "存不下这张 %dx%d（%u 字节）：PSRAM 空闲 %u",
                 w, h, (unsigned)bytes,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        return false;
    }
    memcpy(copy, pixels, bytes);

    /* 还没腾过（说明第一次就分配成功、但相册已经满了）：现在补上。 */
    if (s_count == XIAOWEI_PHOTO_CAPACITY) {
        photo_release(&s_photos[XIAOWEI_PHOTO_CAPACITY - 1]);
        s_count--;
    }
    for (int i = s_count; i > 0; --i) s_photos[i] = s_photos[i - 1];
    s_photos[0].pixels = copy;
    s_photos[0].w = w;
    s_photos[0].h = h;
    s_photos[0].stride = stride;
    s_photos[0].kind = kind;
    s_count++;
    const int count = s_count;

    if (lock) xSemaphoreGive(lock);

    ESP_LOGI(TAG, "相册 +1（来源 %d，%dx%d）：现有 %d/%d 张",
             (int)kind, w, h, count, XIAOWEI_PHOTO_CAPACITY);
    return true;
}

bool xiaowei_photo_capture(xiaowei_photo_kind_t kind)
{
    /* 相机页开着时流本来就在跑；AI 隔着 MCP 叫的时候可能压根没人开过相机页，
       这时 get_frame() 拿到的是从没写过的缓冲，所以先确保流是开的。 */
    const bool was_streaming = xiaowei_camera_is_streaming();
    if (!was_streaming) xiaowei_camera_start_stream();
    xiaowei_camera_set_paused(false);

    /* 等一帧**新的**落地再拍。等不到（比如已经暂停）就用当前这帧，不报错。 */
    const uint32_t before = xiaowei_camera_get_frame_seq();
    for (int i = 0; i < CAM_WAIT_TRIES && xiaowei_camera_get_frame_seq() == before; ++i)
        vTaskDelay(pdMS_TO_TICKS(CAM_WAIT_MS));

    xiaowei_camera_set_paused(true);
    vTaskDelay(pdMS_TO_TICKS(CAM_SETTLE_MS));

    const uint8_t *frame = xiaowei_camera_get_frame();
    bool ok = false;
    if (frame) {
        ok = xiaowei_photo_store_add(frame, CAM_FRAME_W, CAM_FRAME_H, CAM_FRAME_W * 2, kind);
    } else {
        ESP_LOGW(TAG, "拍照失败：相机没有出帧（DVP 初始化失败？）");
    }

    xiaowei_camera_set_paused(false);
    /* 自己拉起来的流自己收掉，免得 AI 拍一张之后采集任务一直空转
       （传感器 + DVP + 缩放 task 全都在耗电）。 */
    if (!was_streaming) xiaowei_camera_stop_stream();
    return ok;
}

int xiaowei_photo_count(void)
{
    SemaphoreHandle_t lock = photo_lock();
    if (lock) xSemaphoreTake(lock, portMAX_DELAY);
    const int count = s_count;
    if (lock) xSemaphoreGive(lock);
    return count;
}

bool xiaowei_photo_get(int index, xiaowei_photo_t *out)
{
    if (!out) return false;
    if (index < 0 || index >= XIAOWEI_PHOTO_CAPACITY) return false;
    SemaphoreHandle_t lock = photo_lock();
    if (lock) xSemaphoreTake(lock, portMAX_DELAY);
    const bool ok = (index < s_count) && s_photos[index].pixels && s_photos[index].w > 0;
    if (ok) *out = s_photos[index];   /* 结构体浅拷：pixels 还是指向相册自己那份 */
    if (lock) xSemaphoreGive(lock);
    return ok;
}
