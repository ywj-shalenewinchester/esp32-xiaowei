#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* ==========================================================================
 *  相册：最近 10 张照片
 *
 *  三路来源共用一个相册，都可以往里塞：
 *    · 相机页/拍照页上手动按快门            -> XIAOWEI_PHOTO_SHUTTER
 *    · AI 透过 MCP 工具 self.camera.take_photo -> XIAOWEI_PHOTO_AI_CAM
 *    · AI 绘画（服务端 type=="display" 下发的整屏图）-> XIAOWEI_PHOTO_AI_DRAW
 *  超过 10 张自动挤掉最早的那张。
 *
 *  照片一律**拷进 PSRAM**（466x350x2 = 326 KB，10 张约 3.1 MB），调用方的
 *  缓冲之后怎么被改写/释放都无所谓 —— 相机那边是两块轮转的公共缓冲，
 *  不拷就没法留着。
 * ========================================================================== */

typedef enum {
    XIAOWEI_PHOTO_SHUTTER = 0,  /* 手动按快门 */
    XIAOWEI_PHOTO_AI_CAM,       /* AI 调 MCP 工具拍的 */
    XIAOWEI_PHOTO_AI_DRAW,      /* AI 绘画下发的整屏图 */
} xiaowei_photo_kind_t;

typedef struct {
    uint8_t *pixels;            /* PSRAM，RGB565（字节序同相机驱动，GC2145 已转成 LVGL 要的那种） */
    int w;
    int h;
    int stride;                 /* 字节 */
    xiaowei_photo_kind_t kind;
} xiaowei_photo_t;

#define XIAOWEI_PHOTO_CAPACITY 10

/* 存一张。pixels 立刻被拷走，w/h/stride 按源图给（相机是 466x350，
   AI 画图通常是 466x466 —— 相册允许混着放）。成功返回 true。
   PSRAM 分配失败会先腾掉最早那张再试一次；再失败就放弃这张并记日志。 */
bool xiaowei_photo_store_add(const uint8_t *pixels, int w, int h, int stride,
                             xiaowei_photo_kind_t kind);

/* 拍一张：确保相机在出流 -> 等一帧新的落地 -> 暂停防撕裂 -> 取帧存进相册
   -> 把相机恢复成调用前的开关流状态。会阻塞 70~400ms，别在音频/协议回调里调。 */
bool xiaowei_photo_capture(xiaowei_photo_kind_t kind);

int xiaowei_photo_count(void);

/* 取第 index 张（0 = 最新），**拷一份出来**给 out。越界或没有返回 false。
 *
 * 不返回槽位指针：另一边（AI 拍照 / AI 绘画）随时可能塞新照片进来把最早那张腾掉，
 * 腾的动作里那个函数会把槽位清零 —— 指针式的接口就可能在调用方读 w/h 的那一瞬间
 * 变成 0，相册页拿 0 去除就是除零。拷出来就锁一致了。
 *
 * 注意 out->pixels 指向的那块内存仍然可能被下一次腾空释放 —— 只保证调用这一刻
 * 描述符自洽。相册页在显示期间正好被腾掉时最坏是画花一屏，不会崩
 * （PSRAM 是常映射的，读一块已释放的内存不触发异常）。 */
bool xiaowei_photo_get(int index, xiaowei_photo_t *out);

#ifdef __cplusplus
}
#endif
