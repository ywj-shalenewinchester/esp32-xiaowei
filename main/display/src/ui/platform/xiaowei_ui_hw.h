#pragma once

/*
 * 小未 LCD UI —— 硬件接口声明（目标工程侧）
 *
 * ui/ 这套代码原来在 lcd 工程里，各 app 直接 #include 板级/根目录的头文件：
 *
 *   apps/audio/audio_app.c   -> ../../../../../boards/WZKJ/.../audio_test.h
 *   apps/legacy/legacy_app.c -> ../../../../../boards/WZKJ/.../haptic_test.h
 *   apps/camera/camera_app.c -> ../../../../../camera_test.h
 *
 * 那三个头文件在本工程里都不存在（音频底层换成了 AW88082 那套、马达驱动
 * 是另一套、摄像头驱动搬进了板级目录）。与其让三个 app 各自去啃一条又深又脆
 * 的相对路径，不如把「UI 需要哪些硬件能力」集中声明在这里一处。
 *
 * 实现分两种落法，两者都在板级目录（main/boards/wzkj/esp32-p4-ai-xiaowei_plus/）：
 *   - 大部分由本目录的 platform/xiaowei_ui_hw.cc 直接实现；
 *   - 需要直接碰板级外设对象的（音频电平、摄像头、IMU、马达）由板级定义、
 *     链接期解析，本文件只给声明，不写转发（见 .cc 末尾的分节注释）。
 *
 * UI 侧因此一行逻辑都不用改，只是把 include 目标换了个位置。
 *
 * 注意 xiaowei_ui_* 那 9 个 port 钩子在 xiaowei_ui_port.h 里，不在这里 ——
 * 那些是 UI 自己的抽象缝，这里是更底层的硬件能力。
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==========================================================================
 *  IMU（IMU 测试页用，apps/imu/imu_app.c）
 *
 *  加速度单位 g，角速度单位 deg/s，温度摄氏度。出参允许传 NULL 表示不关心那一项。
 *  返回 false 表示这次没读到（总线错误 / 器件未就绪 / 板上没这颗料），
 *  调用方应跳过本帧 —— 页面据此显示 OFFLINE，不崩、不假动。
 *
 *  实现在板级目录的 xiaowei_plus.cc（板级持有 Lsm6dsv80x 对象）；本文件只给
 *  声明，不写转发（和下面 camera 那组同一种落法）。
 * ========================================================================== */
bool xiaowei_imu_get(float *ax, float *ay, float *az,
                     float *gx, float *gy, float *gz, float *temp);

/* ==========================================================================
 *  触觉马达（legacy 设置页 / 马达测试页用）
 *
 *  底层只给三个原语：播 RAM 波形库的第 index 号（1..7）、查当前是否在播、
 *  设强度。哪个模式对应哪号波形、测试页 6 个按钮按什么顺序播（error 是两段），
 *  都是 UI 语义，写在 apps/legacy/legacy_app.c，板级不参与。
 *
 *  index 与波形的对应见 aw86937.cc 的 LoadWaveform：
 *    1 alarm / 2 error part1 / 3 error part2 / 4 normal /
 *    5 soft / 6 strong / 7 success
 *
 *  play_wave 返回 false 表示马达未就绪或波形下载失败（调用方当作“这次没播”）。
 *  set_intensity 取 0~100，未就绪时先记住，Init 完自动应用。
 *  实现在板级目录的 xiaowei_plus.cc。
 * ========================================================================== */
bool xiaowei_haptic_play_wave(int index);
bool xiaowei_haptic_is_playing(void);
void xiaowei_haptic_set_intensity(int percent);

/* ==========================================================================
 *  录音回放（audio 测试页用）
 *
 *  本工程没有独立的音频测试链路，这些接口接在目标自己的音频栈
 *  （AudioService / AudioCodec）上，详见 xiaowei_ui_hw.cc。
 *
 *  busy 的语义是"这一轮测试还没走完"：录音中为真，录完转回放的整段时间也为真。
 *  它同时是页面定时器的轮询入口 —— 由它负责在整轮结束后把唤醒词装回去。
 * ========================================================================== */
bool xiaowei_audio_record_start(void);
void xiaowei_audio_record_stop(void);
bool xiaowei_audio_busy(void);

/* 当前是不是还在录音（false + busy 为真 = 已经转入回放）。
 * 框架自己会在录满 10 秒时停录转回放，页面光靠"按过 START"是跟不上这个变化的。 */
bool xiaowei_audio_recording(void);

/* 麦克风实时电平，**加增益之前**的原始值。
 * rms/peak 是出参，允许传 NULL 表示不关心那一项。
 *
 * 由板级音频底层（XiaoweiPlusAudioCodec 的 Read()）在解调后立刻测量 —— 框架的
 * AudioService 给不出这个数：它的录音测试队列里存的是 opus 包，不是 PCM。
 * 用原始值而不是放大后的值，是为了让电平表反映麦克风本身有多灵敏，
 * 调增益时不会被自己的增益绕进去。 */
void xiaowei_audio_input_levels(int *rms, int *peak);

/* 麦克风实时电平，**加增益之后**的值 —— 也就是真正送进 AFE（唤醒词/ASR）的那个数。
 *
 * 和上面那一路成对用：上面回答"麦克风本身有多灵敏"，这一路回答"送到模型面前有多响"。
 * 判断"MIC_GAIN 还该不该往上抬"只能看这一路，因为上面那一路跟 MIC_GAIN 无关，
 * 调增益时它一动不动。 */
void xiaowei_audio_afe_levels(int *rms, int *peak);

/* ==========================================================================
 *  摄像头（camera 页用）
 *
 *  实现在板级目录的 gc2145.cc（GC2145 传感器 + DVP 采集管线，和板级头
 *  gc2145.h 里的声明是同一组符号，在 C 里重复声明无害）。这一组不做转发 ——
 *  板级定义的符号名和这里完全同名，链接期直接解析，再加一层空转发只是噪声。
 *  （音频电平那两路之所以要转发，是因为板级符号叫 xiaowei_plus_audio_*。）
 *
 *  init 不在这里：它只由板级构造函数调用，UI 不需要看见。
 *
 *  注意 get_frame 在没有摄像头/初始化失败时返回 NULL —— camera_app.c 的
 *  每一个取帧点都做了 NULL 保护，所以那种情况下整页自然显示空画面。
 * ========================================================================== */
bool xiaowei_camera_start_stream(void);
void xiaowei_camera_stop_stream(void);
bool xiaowei_camera_is_streaming(void);
const uint8_t *xiaowei_camera_get_frame(void);
void xiaowei_camera_set_paused(bool paused);

/* 最近一秒实际出帧数，与"每写出一帧 +1"的帧序号。
   帧序号用来避免对同一帧反复 set_src 重绘：lv_image_set_src() 是无条件
   lv_obj_invalidate() 的，数据没变也会把 466×350 整个重画一遍。 */
uint32_t xiaowei_camera_get_fps(void);
uint32_t xiaowei_camera_get_frame_seq(void);

#ifdef __cplusplus
}
#endif
