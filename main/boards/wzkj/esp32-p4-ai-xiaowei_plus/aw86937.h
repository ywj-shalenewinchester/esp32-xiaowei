#ifndef __AW86937_H__
#define __AW86937_H__

#include "i2c_device.h"
#include <atomic>
#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

/*
 * AW86937 (U9) 触觉驱动 —— 对外 API。
 *
 * 本类只负责三件事：持有 I2C 设备句柄、管理播放任务与负载锁、暴露对外的播放接口。
 * 真正碰芯片的逻辑全部在 aw86937/ 目录下移植的 AWINIC 官方驱动里
 * （haptic_hv.c = 引擎 + 芯片层，haptic_hv_waveforms.c = 8 个波形数据），
 * 平台层（I2C/延时/定时器/复位脚）在 aw86937.cc。本类只是它的一层壳。
 *
 * 为什么不让外部直接调官方 API：官方那套是全局单例（g_haptic_hv / g_func_haptic_hv），
 * short_vib_work / long_vib_work 是多事务序列且会改全局状态，
 * 必须串行化在同一个任务里跑，否则两条 I2C 序列交错会把芯片写坏。
 * 所以这里所有播放请求都只是"设参数 + 通知任务"，实际 I2C 全部发生在 PlayTask 内。
 */
class Aw86937 : public I2cDevice {
public:
    // AD 接地，默认 7bit 地址为 0x5A
    Aw86937(i2c_master_bus_handle_t i2c_bus, uint8_t addr = 0x5A);

    ~Aw86937();

    // 初始化：配置 RSTN/TRIGx -> 探测器件 -> 启动官方驱动引擎 -> 建播放任务
    esp_err_t Init();

    // ---------------- 播放 ----------------
    // 短振动：波形循环播放 loop 次后芯片自己回 standby（loop 取值 1~15）
    esp_err_t PlayWave(uint8_t index, uint8_t loop = 1);

    // 将指定的完整 .bin.array 单独写入 AW86937 RAM，随后用 index=1 播放。
    esp_err_t LoadWaveform(uint8_t index);

    // 长振动：持续 duration_ms 后由定时器自动停止（duration_ms 上限 10000）
    esp_err_t PlayLong(uint8_t index, uint32_t duration_ms);

    // 单次心跳（= PlayWave(1, 1)）
    esp_err_t PlayHeartbeat();

    // 立即停止当前振动（短振动/长振动都能停）
    esp_err_t Stop();

    // ---------------- 参数 ----------------
    // 增益档位：0x10(最小) ~ 0x80(最大)，见官方 aw_gain 枚举。
    // 只记录，下一次播放生效 —— 不在调用者线程里写寄存器，避免和播放任务抢 I2C。
    esp_err_t SetGain(uint8_t gain);
    uint8_t GetGain() const { return gain_; }

    // 强度（百分比 0~100）与模式（轻柔/标准/强烈）。
    // SetIntensity 折算成增益 0x10~0x80，下一次播放生效；
    // SetMode 只记住当前模式（真正区分三档的是 RAM 波形库里的波形 1/2/3）。
    enum class Mode : uint8_t { Soft, Standard, Strong };
    esp_err_t SetIntensity(uint8_t percent);
    uint8_t GetIntensity() const { return intensity_percent_; }
    esp_err_t SetMode(Mode mode);

    // ---------------- 共振频率（F0）探测 ----------------
    // 一次探测的结果。字段分两类：参考系（pre_hz / lo_hz / hi_hz / cali_data）
    // 是配置值，每次调用都一样；hz / raw 才是这一次实测出来的。
    struct F0Result {
        int      hz;         // 实测共振频率 Hz；< 0 表示这次没测到
        uint16_t raw;        // CONTRD14(LRA_F0) 原始寄存器值；0 = 没读回来
        int      pre_hz;     // 官方标称值 aw8693x_dts.f0_pre
        int      pct;        // 校准允许的偏差百分比 aw8693x_dts.f0_cali_percent
        int      lo_hz;      // 校准允许的下界 = pre * (100 - pct) / 100
        int      hi_hz;      // 校准允许的上界 = pre * (100 + pct) / 100
        uint8_t  cali_data;  // 当前 f0_cali_data（写进 TRIMCFG5 的那个偏移量）
        bool     cali_ok;    // 仅 RecalibrateF0() 有意义：官方 f0_cali() 是否接受这次读数
    };

    // 触发一次 F0 探测并等结果，超时返回 hz = -1。
    //
    // 为什么值得单独提供：开机那次 F0 是校准时顺带测的，值被写进 TRIMCFG5 之后
    // 就固定了，事后想知道"这颗马达的真实共振点是多少、测得稳不稳"，
    // 只能重新触发。多测几次看离散度，才能判断这个读数可不可信。
    //
    // 阻塞调用：内部让芯片按 CONT 模式驱动 LRA 振一下（几十~几百 ms）再读寄存器。
    // 会先停掉正在进行的振动，且整段 I2C 序列都在播放任务里跑，不会和播放交错。
    F0Result MeasureF0(int timeout_ms = 3000);

    // 重跑一次官方 F0 校准（haptic_hv_f0_cali）：测一次，落在窗口内就按偏差
    // 算出新的 trim 写进 TRIMCFG5。和开机那次启动校准走的是同一条路。
    //
    // 为什么要暴露它：开机校准是"一次性"的 —— 万一那次测量赶上芯片还没启动完
    // （见 aw8693x_wait_enter_standby 的竞态），算出来的 trim 就是错的，而且之后
    // 每次 MeasureF0() 都会稳定地复现同一个错误值，光看离散度发现不了。
    // 有了这个接口就能验证"重跑一次能不能恢复正常"。返回的 cali_ok 表示官方
    // 是否接受了这次读数；即使为 false，hz 里仍然是本次实测值（官方超窗口时
    // 直接 return AW_ERROR，但 f0 已经写好了）。
    F0Result RecalibrateF0(int timeout_ms = 3000);

    // ---------------- 状态 ----------------
    bool IsPlaying() const { return is_playing_.load(); }
    bool IsReady() const { return initialized_; }

    // 芯片当前实际使用的共振频率（官方 f0，单位 Hz，约 1445~1955）
    int GetF0Hz() const;

    // 电池电压，单位 mV。取的是上一次长振动做 vbat 补偿时读到的缓存值，
    // 不会为了查询单独发起 I2C。开机到第一次长振动之前是 0。
    uint32_t GetVbat() const;

    // 当前波形库的波形段数（心跳库为 1，官方默认表为 4）
    uint8_t GetRamNum() const;

private:
    std::atomic<bool> is_playing_;
    TaskHandle_t play_task_handle_;
    bool initialized_;
    uint8_t gain_;

    // 强度百分比（0~100）与当前模式。两者都只"记录"，真正生效在
    // SetIntensity / SetMode 里折算：强度 -> 增益档，模式 -> RAM 波形 1/2/3。
    volatile uint8_t intensity_percent_;
    Mode mode_;

    // 播放请求参数。提交方写、播放任务读，写入后紧跟一次 xTaskNotify，
    // 通知本身带内存屏障，不需要额外加锁。
    volatile uint8_t pending_index_;
    volatile uint8_t pending_loop_;
    volatile uint32_t pending_duration_ms_;

    // F0 探测结果。播放任务写、MeasureF0() 轮询读，靠 f0_result_hz_ 非 0
    // 判断"回来了"。所以写入顺序必须是 raw / cali_ok 在前、hz 最后 ——
    // 反过来的话轮询方可能看到 hz 已有值、其余还是上一次的旧值。
    volatile int f0_result_hz_;
    volatile uint16_t f0_result_raw_;
    volatile int f0_result_cali_ok_;

    // MeasureF0() / RecalibrateF0() 的公共骨架，区别只在丢给播放任务的通知值
    F0Result RunF0Job(uint32_t notify_value, int timeout_ms);

    esp_err_t Submit(uint8_t index, uint8_t loop, uint32_t duration_ms);
    void PlayTask();
    static void PlayTaskWrapper(void* arg);
    void WaitPlayDone();
    void ReleaseLoadLock();
};

#endif  // __AW86937_H__
