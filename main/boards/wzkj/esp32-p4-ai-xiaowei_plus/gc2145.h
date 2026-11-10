#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "i2c_device.h"

/* GC2145 传感器驱动。*/
class Gc2145 final : public I2cDevice {
public:
    static constexpr uint8_t kAddress = 0x3C;

    Gc2145(i2c_master_bus_handle_t bus, uint8_t address = kAddress);

    /* 读 ID 校验 0x2145，然后配成 VGA RGB565。重复调用是空操作。 */
    esp_err_t Init();
    esp_err_t ReadId(uint16_t* id);
    esp_err_t ConfigureVgaRgb565();

private:
    static constexpr uint8_t kIdHigh = 0xF0;
    static constexpr uint8_t kIdLow  = 0xF1;

    bool init_done_ = false;  // 防止重复初始化
};

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- 采集管线 */

/* 开机一次性初始化：复位 GC2145 + LEDC 临时供 XCLK 配 SCCB + 建 DVP 控制器 +
   分配 ping-pong PSRAM 缓冲和 466×350 输出缓冲 + 建采集 task。
   会复用触摸初始化好的 I2C 总线（xiaowei_camera_i2c_bus()）挂 GC2145，
   所以必须在 InitializeLcd() 之后调用。失败时自动清理并返回 false。 */
bool xiaowei_camera_init(void);

/* 开流：只是把 task 的标志位打开，让它开始把帧缩放到输出缓冲。
   **不动 DVP 控制器状态机** —— esp_cam_ctlr_dvp 在 stop 之后无法靠
   enable+start 恢复连续 DMA，二次进页面会只出一帧就停住。 */
bool xiaowei_camera_start_stream(void);

/* 关流：只切标志位。输出缓冲保留最后一帧画面，DVP 继续 ping-pong，
   下次 start_stream 直接恢复刷新。 */
void xiaowei_camera_stop_stream(void);

/* 现在开着流吗。用来让"临时拉起来拍一张"的调用方（MCP 拍照）拍完能恢复原状，
   而不是把别人正在用的流一起关掉。 */
bool xiaowei_camera_is_streaming(void);

/* 当前 466×350 RGB565 缓冲指针（连续紧凑，466*350*2 字节）。
   未初始化或初始化失败时返回 NULL —— 调用方必须做 NULL 保护。

   采集侧是**多块轮转**的（见 gc2145.cc 里 s_lcd_frame 的说明），所以：
   - 连续两次调用可能返回**不同**的指针，每次都重新取，不要缓存。
   - 拿到的那块在下次发布之前不会被改写，可以放心交给 LVGL 慢慢画 ——
     这正是之前单缓冲时撕裂的来源。 */
const uint8_t* xiaowei_camera_get_frame(void);

/* 暂停 = 停止刷新输出缓冲（保持最后一帧，用来模拟"拍照"）。 */
void xiaowei_camera_set_paused(bool paused);

/* 最近一秒**实际写进输出缓冲**的帧数（1 秒滑动窗口，在采集 task 里统计）。
   未开流时为 0。无锁：单写单读的 32 位值。 */
uint32_t xiaowei_camera_get_fps(void);

/* 每写出一帧 +1 的单调计数器。

   UI 侧靠它判断"手上这帧是不是新的"：lv_image_set_src() 第一句就是无条件
   lv_obj_invalidate()（lvgl/src/widgets/image/lv_image.c:152），所以哪怕数据
   一个字节没变，只要调了就会让 LVGL 把那 466×350 整个重绘一遍。 */
uint32_t xiaowei_camera_get_frame_seq(void);

#ifdef __cplusplus
}
#endif
