// ============================================================================
//  XiaoweiPlusAudioCodec —— 本板音频底层（AW88082 数字功放 + PDM 数字麦）
//
//  为什么要有这个子类：框架的 NoAudioCodecSimplexPdm 在三处和这块板子的实际硬件
//  对不上，而它又是 S3 板共享的文件（S3 走 Es8311AudioCodec）。把差异收进板级
//  子类，`main/audio/` 下一行都不用动，改动静默地只影响 P4 板。
//
//  参照实现是 lcd_demo 工程的 `main/boards/WZKJ/esp32-p4-ai-xiaowei_plus/audio_test.c`
//  （同板同引脚，实测能收音能放音）。注意：那个工程里 `xiaowei_plus.cc` 的
//  Es8311AudioCodec 版本**根本没编进固件**，真正跑的就是 audio_test.c 这套。
//
//  三处差异：
//
//  1) 麦克风声道。PDM 麦的 LR 选择脚决定数据落在哪个 slot，而 IDF 的
//     I2S_PDM_RX_SLOT_DEFAULT_CONFIG(..., MONO) 把 slot_mask 写死成 LEFT
//     （i2s_pdm.h:36-37），我们没有任何选择权。读错 slot 只能拿到恒定直流，
//     表现就是"麦克风完全没声音、唤不醒 AI"。这里在构造时探测一次左右声道，
//     取信号强的那侧（audio_test.c:471-515 的做法）。
//
//  2) 功放上电时序。AW88082 的 PLL 拿 BCK 当参考时钟，必须先让 I2S 时钟跑起来、
//     再分两步上电并轮询 SYSST 等 PLL 锁定，最后才解除静音。原来那句一次性写
//     0x2246 跳过了整个握手（audio_test.c:116-147）。
//
//  3) 输入增益。这颗麦灵敏度很低（厂商实测语音 RMS 只有一两百、底噪 RMS≈50，
//     SNR 仅约 10dB）。框架的 input_gain_ 默认 0.0，而 no_audio_codec.cc:398
//     的 `if (input_gain_ > 0)` 会**直接跳过**，等于喂给 AFE 的是原始信号。
// ============================================================================

#pragma once

#include <atomic>
#include <cstdint>

#include <driver/gpio.h>
#include <driver/i2c_master.h>

#include "audio/codecs/no_audio_codec.h"

class XiaoweiPlusAudioCodec : public NoAudioCodecSimplexPdm {
public:
    // amp_dev：板上已经建好的 AW88082 I2C 设备句柄（地址 0x34，挂外设 I2C 总线）。
    // 由板级传进来而不是自己 add_device —— 同一条总线上同一个地址建两个句柄没有意义。
    XiaoweiPlusAudioCodec(int input_sample_rate, int output_sample_rate,
                          gpio_num_t spk_bclk, gpio_num_t spk_ws, gpio_num_t spk_dout,
                          gpio_num_t mic_sck, gpio_num_t mic_din,
                          i2c_master_dev_handle_t amp_dev);
    ~XiaoweiPlusAudioCodec() override = default;

    // 探测结果。给板级自检打印用 —— "采的是哪一侧"是排查麦克风问题的第一手信息。
    bool mic_uses_right_channel() const { return mic_use_right_; }

    // 实时输入电平：**去直流之后、去毛刺与加增益之前**的交流值。
    //
    // 为什么由 codec 提供而不是 AudioService：框架的录音测试队列里存的是 **opus 包**，
    // 不是 PCM，AudioService 也没有暴露任何输入电平。codec 的 Read() 是唯一能看到
    // 原始 PCM 的地方。audio 页的电平表就是从这里取数的。
    int InputLevelRms() const { return level_rms_.load(std::memory_order_relaxed); }
    int InputLevelPeak() const { return level_peak_.load(std::memory_order_relaxed); }

    // 加增益之后的电平，也就是**真正送进 AFE 的那个数**。
    //
    // 和上面那对的分工：InputLevel* 回答"这颗麦克风本身有多灵敏"（不随 MIC_GAIN 变），
    // 这一对回答"送到唤醒词/ASR 面前的信号有多响"（随 MIC_GAIN 变）。判断"增益还
    // 该不该往上抬"，只能看这一对 —— 参考工程把这颗麦拉到 RMS 6000 当作"响亮"
    // （audio_test.c:70 的 VOICE_GAIN_TARGET_RMS），可以拿它当参照。
    int InputGainLevelRms() const { return level_gain_rms_.load(std::memory_order_relaxed); }
    int InputGainLevelPeak() const { return level_gain_peak_.load(std::memory_order_relaxed); }

    // 上一块里被减掉的那个直流偏置。单独暴露出来是因为它是排查麦克风时最难猜、又
    // 一眼能定性的一个数：PDM 静止时理论上是 50% 占空比（直流=0），实际偏多少看这里。
    int InputDcOffset() const { return level_dc_.load(std::memory_order_relaxed); }

protected:
    int Write(const int16_t* data, int samples) override;
    int Read(int16_t* dest, int samples) override;
    void EnableInput(bool enable) override;
    void EnableOutput(bool enable) override;

private:
    // ---- 初始化（构造期一次性）----
    void DetectMicChannel();
    void AlignTxToStereo();

    // ---- 功放 ----
    bool AmpWriteReg(uint8_t reg, uint16_t value);
    bool AmpReadReg(uint8_t reg, uint16_t* value);
    bool AmpWaitStatusBit(uint8_t bit, int timeout_ms, const char* what);
    bool AmpConfigure();
    bool AmpPowerUp();
    void AmpSetMute(bool mute);

    // 往 TX 里推一段静音把 BCK/WCK 跑起来 —— 功放的 PLL 拿 BCK 当参考时钟，
    // 时钟不动它永远锁不上，后面等 SYSST 必然超时。只在首次使能 TX 后调一次。
    void PrimeAmpClock();

    i2c_master_dev_handle_t amp_dev_ = nullptr;

    bool mic_use_right_ = false;

    // TX 是否已经改成立体声。改失败时 Write() 必须按单声道送，
    // 否则交错后的数据会被当成两倍速的单声道流（听起来是快放 + 噪声）。
    bool tx_stereo_ = false;

    // TX 通道是否已经使能过。我们**不**在 EnableOutput(false) 时关它 —— 见 .cc 里的说明。
    bool tx_running_ = false;
    bool amp_ready_ = false;

    // 去毛刺用：跨 Read 调用保持上一个采样，毛刺用前值顶替而不是清零
    // （清零会在波形上留一个台阶，听起来是咔哒声）。
    int16_t last_sample_ = 0;

    // 每次使能输入后的头几块打一行原始统计，打完自动停（见 .cc 里的 kRawLogBlocks）。
    // Read() 与 EnableInput() 可能在不同任务上跑，这里是个无所谓的竞态：最多多打一行
    // 或少打一行日志，不值得为它上原子操作。
    int raw_log_budget_ = 0;

    std::atomic<int> level_rms_{0};
    std::atomic<int> level_peak_{0};
    std::atomic<int> level_dc_{0};
    std::atomic<int> level_gain_rms_{0};
    std::atomic<int> level_gain_peak_{0};
};
