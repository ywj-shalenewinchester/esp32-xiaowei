#include "xiaowei_plus_audio.h"

#include <cmath>
#include <cstring>
#include <vector>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/i2s_pdm.h>
#include <driver/i2s_std.h>

static const char* TAG = "XiaoweiPlusAudio";

// 临时热源隔离实验：1=完全禁止 AW88082 PA/PLL 上电，只保留麦克风和音频服务；
// 0=恢复正常播放。用于确认发热是否来自功放工作态。
static constexpr bool DISABLE_AW88082_FOR_THERMAL_TEST = false;

// ============================================================================
//  调参入口 —— 麦克风信号链只有这两个数字
// ============================================================================

// 固定输入增益。这颗 PDM 麦（LMD2718B261）灵敏度很低：厂商参考实现实测语音 RMS
// 只有一两百、底噪 RMS≈50，SNR 仅约 10dB，所以它上了动态增益（目标 RMS 6000，
// 上限 64×，见参考工程 audio_test.c:70-72 的 VOICE_GAIN_TARGET_RMS /
// VOICE_GAIN_MAX_Q8）。这里退一步用固定增益 —— 常驻的动态增益会在静音段把底噪
// 一起拉满，对唤醒词模型是净负面的（厂商自己也在注释里写明"不能加噪声门"，
// 信噪比太窄）。
//
// 为什么是 64×（= 参考工程给这颗麦留的上限）而不是更小的 8×：
//   PDM 配置和参考工程逐字相同（clk/slot/毛刺门限/去直流都一样），唯一差的就是这一项。
//   本板实测**说话时**原始 RMS 只有 40~50，而参考工程判定"拾到声音"的门限就是
//   RMS > 64（audio_test.c:373）—— 也就是说，不放大到参考工程那一档，连它自己的
//   前端都会把这一路判成"没录到声音"。取它的上限值，等价于把参考工程在静音段
//   会做的事，用固定增益一次性做满。
//
// ⚠ 电平条画的是**加增益之后**的值（audio_app.c 的 AUDIO_LEVEL_FULL_SCALE=6000），
//   所以调这个常数时条子会动；而旁边那行 RAW 数字是加增益之前的原始值，调它是不会变的。
static constexpr int MIC_GAIN = 64;

// 毛刺剔除阈值。数据线偶发 glitch 会让 PDM 解调器输出到固定饱和值（厂商实测
// ~27377），单点就能把整段 RMS 抬高几十、放大后是爆音。
//
// ⚠ 必须在**加增益之前**剔除：6000 乘上 MIC_GAIN 早已越过 32767，加完再判什么都
// 救不回来（毛刺一旦被放大就饱和成一段满幅方波，听感是爆音）。
// 这也是本类不走 AudioCodec::input_gain_ 的原因 —— 基类 Read() 是先加增益再返回，
// 顺序反了（见构造里把 input_gain_ 置 0 的说明）。
//
// 6000 同时是"真实话音"和"数据线毛刺"的分界：本板实测说话时原始峰值只有几十，
// 离 6000 还有两个数量级余量，所以抬 MIC_GAIN 不需要动这个门限（它判的是增益前）。
static constexpr int MIC_GLITCH_ABS = 6000;

// 每次使能输入后，前多少块打一行原始统计（10ms 一块 → 5 块 ≈ 50ms）。够看清直流
// 偏置和信号幅度，又不会刷屏。
static constexpr int kRawLogBlocks = 5;

// ============================================================================
//  ⚠ 诊断开关 —— 正常保持 0
//
//  置 1 时 Read() 把基类读到的原始 PDM 数据**原样**交出去：去直流、去毛刺、加增益
//  三步全部跳过。
//
//  它存在的唯一理由是给"唤醒词引擎崩溃"做二分。改这一个字符重新编译，就能把
//  「崩是我们处理过的数据引起的」和「跟我们的数据处理无关」彻底分开：
//    · 置 1 后不再崩 → 问题在上面那三步里，逐个放开就能定位到具体哪一步；
//    · 置 1 后照崩  → 数据和这里无关，去查 WakeNet 模型 / AFE / 内存，别再调这些参数。
//
//  在把这条音频链路调通、并且确认唤醒词稳定之前不要删掉它 —— 这是本板唯一一个能把
//  "音频数据"和"语音模型"两个变量拆开的开关。
// ============================================================================
#define MIC_BYPASS_PROCESSING 0

// ============================================================================
//  AW88082QNR 功放寄存器（8bit 地址 + 16bit 数据，大端）
// ============================================================================

static constexpr uint8_t AMP_REG_ID       = 0x00;
static constexpr uint8_t AMP_REG_SYSST    = 0x01;
static constexpr uint8_t AMP_REG_SYSCTRL  = 0x04;
static constexpr uint8_t AMP_REG_SYSCTRL2 = 0x05;
static constexpr uint8_t AMP_REG_I2SCTRL1 = 0x06;
static constexpr uint8_t AMP_REG_I2SCTRL3 = 0x08;

static constexpr uint16_t AMP_EN_PA    = 1u << 1;
static constexpr uint16_t AMP_EN_PLL   = 1u << 2;
static constexpr uint16_t AMP_IPLL_BCK = 0u << 3;  // PLL 参考取 BCK（不是 WCK）
static constexpr uint16_t AMP_I2SEN    = 1u << 6;
static constexpr uint16_t AMP_HMUTE    = 1u << 8;  // 硬静音
static constexpr uint16_t AMP_HDCCE    = 1u << 9;
static constexpr uint16_t AMP_GAIN_6P8 = 2u << 12;
static constexpr uint16_t AMP_PWDN     = 1u << 0;

// 正常工作态（仍带硬静音）：EN_PA|EN_PLL|I2SEN|HDCCE|GAIN_6P8 = 0x2246
static constexpr uint16_t AMP_WORK =
    AMP_EN_PA | AMP_EN_PLL | AMP_IPLL_BCK | AMP_I2SEN | AMP_HDCCE | AMP_GAIN_6P8;

static constexpr uint16_t AMP_I2SCTRL3_DEFAULT = 0x0056;
static constexpr uint16_t AMP_I2SCTRL1_BASE    = 0x0400;

// 功放自带的衰减器（SYSCTRL2）：0 = 0dB，每 LSB 约 -0.094dB，0x95 ≈ -14dB。
// 用户音量走 codec 的软件增益（AudioCodec::output_volume_，平方律），所以这里
// 固定一个适中衰减留余量 —— 两级都放到最大会削顶，两级都衰减又偏轻。
static constexpr uint16_t AMP_ATTENUATION = 0x0095;  // ≈ -14dB，与正常根工程一致

static constexpr int AMP_I2C_TIMEOUT_MS = 100;

// 采样率 → AW88082 的 SSR 码（audio_test.c 的表，逐项照抄）
static uint8_t AmpSsrCode(uint32_t rate)
{
    switch (rate) {
    case 8000:  return 0x0;
    case 11025: return 0x1;
    case 12000: return 0x2;
    case 16000: return 0x3;
    case 22050: return 0x4;
    case 24000: return 0x5;
    case 32000: return 0x6;
    case 44100: return 0x7;
    case 48000: return 0x8;
    case 96000: return 0x9;
    default:    return 0x8;
    }
}

static uint16_t AmpI2sCtrl1(uint32_t rate)
{
    // 正常根工程使用完整的 0x0400 格式位；不能截断成 uint8_t，否则 bit10 丢失，
    // AW88082 会在错误的 I2S 帧格式下工作。24kHz 对应 0x0405。
    return AMP_I2SCTRL1_BASE | AmpSsrCode(rate);
}

// ============================================================================

XiaoweiPlusAudioCodec::XiaoweiPlusAudioCodec(
    int input_sample_rate, int output_sample_rate,
    gpio_num_t spk_bclk, gpio_num_t spk_ws, gpio_num_t spk_dout,
    gpio_num_t mic_sck, gpio_num_t mic_din,
    i2c_master_dev_handle_t amp_dev)
    : NoAudioCodecSimplexPdm(input_sample_rate, output_sample_rate,
                             spk_bclk, spk_ws, spk_dout,
                             I2S_STD_SLOT_LEFT, mic_sck, mic_din),
      amp_dev_(amp_dev)
{
    // 本类的 Read() 自己按"先去毛刺、再加增益"的顺序处理，所以必须让基类的
    // 那一步增益失效 —— no_audio_codec.cc:398 只在 input_gain_ > 0 时才乘，
    // 置 0 正好跳过。增益量在 MIC_GAIN 里，是唯一入口。
    input_gain_ = 0.0f;

    DetectMicChannel();
    AlignTxToStereo();

    ESP_LOGI(TAG, "音频底层就绪：功放 I2S1 %dHz/16bit/stereo，麦克风 I2S0 PDM %dHz/mono(%s)，"
                  "输入增益 %d×，毛刺阈值 %d",
             output_sample_rate_, input_sample_rate_,
             mic_use_right_ ? "右" : "左", MIC_GAIN, MIC_GLITCH_ABS);
}

// ---------------------------------------------------------------------------
//  麦克风声道探测
// ---------------------------------------------------------------------------

void XiaoweiPlusAudioCodec::DetectMicChannel()
{
    // 10ms 一块（48kHz 时 480 帧）。
    const int chunk_frames = input_sample_rate_ / 100;
    if (chunk_frames <= 0) return;

    // 先用 STEREO 读一次：PDM 时钟的两个相位各对应一个"声道"，麦克风的 LR 选择脚
    // 决定它驱动哪个相位。读错相位只能拿到微弱串扰，灵敏度暴跌 —— 表现就是
    // "完全没声音"。测完再按选中的那一侧重建 MONO 通道。
    //
    // 注意这里复用基类建好的 rx_handle_ 而不是像参考实现那样另建一个探测通道：
    // IDF 每个控制器只允许一个 RX 通道，基类构造已经占了 I2S0，再建必然失败。
    i2s_pdm_rx_slot_config_t stereo_cfg =
        I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    if (i2s_channel_reconfig_pdm_rx_slot(rx_handle_, &stereo_cfg) != ESP_OK) {
        ESP_LOGW(TAG, "PDM 声道探测：切换立体声失败，保持基类的左声道配置");
        return;
    }

    int l_rms = 0, r_rms = 0;
    if (i2s_channel_enable(rx_handle_) != ESP_OK) {
        ESP_LOGW(TAG, "PDM 声道探测：通道使能失败，保持基类的左声道配置");
        return;
    }

    std::vector<int16_t> buf((size_t)chunk_frames * 2);
    int64_t l_acc = 0, r_acc = 0;
    size_t frames = 0;

    for (int i = 0; i < 10; i++) {  // 10 × 10ms = 100ms
        size_t rd = 0;
        if (i2s_channel_read(rx_handle_, buf.data(), buf.size() * sizeof(int16_t),
                             &rd, 200) != ESP_OK) {
            continue;
        }
        // 前 40ms 丢掉：LMD2718B261 手册 Wake-up Time ≤35ms，时钟刚起来的
        // 头一小段数据不可信。
        if (i < 4) continue;

        const size_t n = rd / sizeof(int16_t);
        for (size_t k = 0; k + 1 < n; k += 2) {
            l_acc += (int64_t)buf[k] * buf[k];
            r_acc += (int64_t)buf[k + 1] * buf[k + 1];
            frames++;
        }
    }
    // reconfig 要求通道处于停止态，测完必须关掉再改 slot。
    i2s_channel_disable(rx_handle_);

    if (frames > 0) {
        l_rms = (int)std::sqrt((double)(l_acc / (int64_t)frames));
        r_rms = (int)std::sqrt((double)(r_acc / (int64_t)frames));
    }

    // 平局取左，与参考实现一致。
    mic_use_right_ = (r_rms > l_rms);

    i2s_pdm_rx_slot_config_t mono_cfg =
        I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
    mono_cfg.slot_mask = mic_use_right_ ? I2S_PDM_SLOT_RIGHT : I2S_PDM_SLOT_LEFT;
    if (i2s_channel_reconfig_pdm_rx_slot(rx_handle_, &mono_cfg) != ESP_OK) {
        ESP_LOGW(TAG, "PDM 声道探测：按选中声道重建失败，保持左声道");
        mic_use_right_ = false;
        return;
    }

    ESP_LOGI(TAG, "PDM 声道探测：左 RMS=%d 右 RMS=%d → 采用%s声道", l_rms, r_rms,
             mic_use_right_ ? "右" : "左");
    if (l_rms < MIC_GLITCH_ABS && r_rms < MIC_GLITCH_ABS) {
        // 两侧都很安静有两种可能，日志里点一下，省得误判成"声道选错了"：
        // 只是环境静音（正常），或者麦克风根本没出数据（查供电/时钟/焊接）。
        ESP_LOGI(TAG, "（两侧 RMS 都很小：若对着麦克风说话时自检的交流峰值也不动，"
                      "那是麦克风没出数据，不是声道问题）");
    }
}

// ---------------------------------------------------------------------------
//  喇叭发送对齐到立体声
// ---------------------------------------------------------------------------

void XiaoweiPlusAudioCodec::AlignTxToStereo()
{
    // 基类建的是 MONO + LEFT slot。参考实现用的是 STEREO（同一采样复制到左右），
    // 对"功放到底取哪个声道"这件事更不敏感。功放此时还没上电，通道也没使能。
    i2s_std_slot_config_t slot_cfg =
        I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    const esp_err_t err = i2s_channel_reconfig_std_slot(tx_handle_, &slot_cfg);
    if (err == ESP_OK) {
        tx_stereo_ = true;
    } else {
        // 退回去用基类的单声道配置是对的：这时 Write() 必须按 mono 送，
        // 否则交错后的数据会被当成两倍速的单声道流，听起来是快放 + 噪声。
        ESP_LOGW(TAG, "TX 改立体声失败 (%s)，保持单声道发送", esp_err_to_name(err));
    }
}

// ---------------------------------------------------------------------------
//  发送
// ---------------------------------------------------------------------------

int XiaoweiPlusAudioCodec::Write(const int16_t* data, int samples)
{
    if (samples <= 0) return 0;

    if (!tx_stereo_) {
        return NoAudioCodecSimplexPdm::Write(data, samples);
    }

    // mono → stereo：同一采样复制到左右两个 slot。
    // 音量缩放由基类 Write() 逐元素施加，所以这里只做交错，不碰幅度。
    std::vector<int16_t> stereo((size_t)samples * 2);
    for (int i = 0; i < samples; ++i) {
        stereo[(size_t)i * 2]     = data[i];
        stereo[(size_t)i * 2 + 1] = data[i];
    }
    const int written = NoAudioCodecSimplexPdm::Write(stereo.data(), samples * 2);
    return written / 2;  // 调用方要的是 mono 帧数
}

// ---------------------------------------------------------------------------
//  接收：去毛刺 → 测电平 → 加增益
// ---------------------------------------------------------------------------

int XiaoweiPlusAudioCodec::Read(int16_t* dest, int samples)
{
    // 基类这一步只做 i2s_channel_read（input_gain_ 已被置 0，不会加增益）
    const int got = NoAudioCodecSimplexPdm::Read(dest, samples);
    if (got <= 0) return got;

#if MIC_BYPASS_PROCESSING
    // 诊断用：原样交出去，下面三步全跳过。见文件顶部那个开关的说明。
    return got;
#endif

    // ---- 1) 去直流。**必须放在最前面** ----------------------------------------
    // 这颗 PDM 麦静止时是 ~50% 占空比，解调出来的波形不是零中心的，而是整体叠在
    // 一个直流偏置上（偏多少看日志里那个"直流="）。顺序反了，下面两步都会坏：
    //   · 去毛刺若拿含直流的原始值去判，偏置自己就超过阈值，整块会被判成"全是毛刺"；
    //     而毛刺分支里 prev 是刻意不更新的，于是一路被顶成 last_sample_（初值 0）——
    //     结果是喂给 AFE 的整块变成 0，等于把麦克风静音了。
    //   · 加增益会把偏置一起放大，信号被推到饱和。
    // 顺带说一句为什么减直流对唤醒词尤其要紧：WakeNet 是逐帧按数据本身算量化指数的
    // 定点模型（崩溃栈里那个 qacc_i16o16k8b16 就是），指数一旦被大直流顶满，交流
    // 部分会被量化到接近 0 —— 模型等于收到静音，表现就是"有声音也唤不醒"。
    int64_t sum = 0;
    for (int i = 0; i < got; ++i) sum += dest[i];
    const int dc = (int)(sum / got);
    level_dc_.store(dc, std::memory_order_relaxed);

    // 减直流 + 测交流电平，一趟做完。这一对存的是**加增益之前**的值 —— 它反映的是
    // 麦克风本身有多灵敏，跟 MIC_GAIN 无关。要判断"送给 AFE 的够不够响"看下面第 3)
    // 步里那一对（level_gain_*）。
    int64_t acc = 0;
    int peak = 0;
    for (int i = 0; i < got; ++i) {
        int v = (int)dest[i] - dc;
        // 偏置可能和采样值同量级，差值会超出 int16 范围，必须钳住
        if (v > INT16_MAX) v = INT16_MAX;
        else if (v < -INT16_MAX) v = -INT16_MAX;
        dest[i] = (int16_t)v;

        const int a = (v < 0) ? -v : v;
        if (a > peak) peak = a;
        acc += (int64_t)v * v;
    }
    level_peak_.store(peak, std::memory_order_relaxed);
    level_rms_.store((int)std::sqrt((double)(acc / got)), std::memory_order_relaxed);

    // ---- 2) 去毛刺（判的是上面刚减掉直流之后的交流幅度）-----------------------
    // 用前一个采样顶替而不是清零 —— 清零会在波形上留一个台阶，听起来是咔哒声；
    // 顶替则几乎听不出来。prev 在毛刺分支里刻意不更新：连着几个毛刺应当统一由
    // "毛刺出现前的那一个正常采样"顶替，而不是互相顶替出一段跟着跑的假波形。
    int16_t prev = last_sample_;
    for (int i = 0; i < got; ++i) {
        const int16_t v = dest[i];
        if (v > MIC_GLITCH_ABS || v < -MIC_GLITCH_ABS) {
            dest[i] = prev;
        } else {
            prev = v;
        }
    }
    last_sample_ = prev;

    // ---- 3) 增益（饱和钳位）--------------------------------------------------
    // 顺手量一下加完增益的电平。这才是真正喂给 AFE 的数，也是判断"增益够不够"
    // 唯一有意义的数 —— 1) 里那个原始电平只说明麦克风本身有多灵敏，跟增益无关。
    if (MIC_GAIN > 1) {
        int64_t gacc = 0;
        int gpeak = 0;
        for (int i = 0; i < got; ++i) {
            int32_t v = (int32_t)dest[i] * MIC_GAIN;
            if (v > INT16_MAX) v = INT16_MAX;
            else if (v < -INT16_MAX) v = -INT16_MAX;
            dest[i] = (int16_t)v;

            const int a = (v < 0) ? -(int)v : (int)v;
            if (a > gpeak) gpeak = a;
            gacc += (int64_t)v * v;
        }
        level_gain_peak_.store(gpeak, std::memory_order_relaxed);
        level_gain_rms_.store((int)std::sqrt((double)(gacc / got)), std::memory_order_relaxed);
    } else {
        // 增益为 1 时两级等价，直接沿用 1) 的结果，省一趟循环。
        level_gain_peak_.store(peak, std::memory_order_relaxed);
        level_gain_rms_.store(level_rms_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }

    // 每次使能输入后的头几块打一行。这是"麦克风到底出没出数据、直流偏多少、
    // 放大后送到 AFE 的是多响"的第一手证据，而且只在开头出现（约 50ms），不会刷屏。
    // 放在增益之后打，两个电平才是同一块的 —— 摆在中间会打印上一块的加增益值。
    if (raw_log_budget_ > 0) {
        raw_log_budget_--;
        ESP_LOGI(TAG, "PDM 原始块：直流=%d 交流 RMS=%d 峰值=%d | 加增益后(%d×) RMS=%d 峰值=%d",
                 dc, level_rms_.load(std::memory_order_relaxed), peak, MIC_GAIN,
                 level_gain_rms_.load(std::memory_order_relaxed),
                 level_gain_peak_.load(std::memory_order_relaxed));
    }
    return got;
}

// ---------------------------------------------------------------------------

void XiaoweiPlusAudioCodec::EnableInput(bool enable)
{
    NoAudioCodec::EnableInput(enable);
    if (enable) {
        // 每次开输入都重打一遍那几行原始统计。开机自检和 AudioService 各自会开一次
        // 输入，两次的现场都值得看 —— 尤其是"崩之前那一刻的数据长什么样"。
        raw_log_budget_ = kRawLogBlocks;
    } else {
        // 输入都关了，电平就地归零 —— 否则 audio 页会一直显示最后一帧的残留值，
        // 看起来像"还在拾音"。
        level_rms_.store(0, std::memory_order_relaxed);
        level_peak_.store(0, std::memory_order_relaxed);
        level_gain_rms_.store(0, std::memory_order_relaxed);
        level_gain_peak_.store(0, std::memory_order_relaxed);
    }
}

// ---------------------------------------------------------------------------
//  功放时序
//
//  ⚠ 这里覆写的是 NoAudioCodec::EnableOutput（protected virtual）。它原本会
//  i2s_channel_disable(tx_handle_)，而我们要**故意**不停 I2S：AW88082 的 PLL 拿
//  BCK 当参考时钟，BCK 一停 PLL 就丢锁，下次出声又要等最多 200ms 重新锁定。
//  所以空闲时只置硬静音，时钟一直跑。
// ---------------------------------------------------------------------------

void XiaoweiPlusAudioCodec::EnableOutput(bool enable)
{
    std::lock_guard<std::mutex> lock(data_if_mutex_);

    if (DISABLE_AW88082_FOR_THERMAL_TEST) {
        if (amp_dev_ != nullptr) {
            AmpWriteReg(AMP_REG_SYSCTRL, AMP_PWDN | AMP_HMUTE);
        }
        amp_ready_ = false;
        ESP_LOGW(TAG, "AW88082 热源隔离测试：禁止 PA/PLL 上电（enable=%d）", enable);
        AudioCodec::EnableOutput(enable);
        return;
    }

    if (enable == output_enabled_) return;

    if (enable) {
        if (!tx_running_) {
            // 首次出声才做这套一次性初始化。放在这里而不是板级构造函数里，
            // 是因为整套握手都依赖 BCK 已经在跑，而板级构造期 GetAudioCodec()
            // 还没被调过、TX 通道根本不存在。
            if (i2s_channel_enable(tx_handle_) != ESP_OK) {
                ESP_LOGE(TAG, "使能 I2S TX 失败，功放无法初始化");
                return;
            }
            tx_running_ = true;
            PrimeAmpClock();
            amp_ready_ = AmpConfigure() && AmpPowerUp();
        } else if (!amp_ready_) {
            // 空闲期间已将 AW88082 置 PWDN；重新播放时恢复完整上电时序。
            amp_ready_ = AmpConfigure() && AmpPowerUp();
        }
        AmpSetMute(false);
        uint16_t sysctrl = 0;
        if (AmpReadReg(AMP_REG_SYSCTRL, &sysctrl)) {
            ESP_LOGI(TAG, "AW88082 播放态 SYSCTRL=0x%04X", sysctrl);
        }
    } else {
        AmpSetMute(true);
        // 空闲时关闭 PA/PLL，并停止 TX 时钟，确保 AW88082 只在实际播放期间工作。
        if (amp_dev_ != nullptr && tx_running_) {
            AmpWriteReg(AMP_REG_SYSCTRL, AMP_PWDN | AMP_HMUTE);
            amp_ready_ = false;
            uint16_t sysctrl = 0;
            if (AmpReadReg(AMP_REG_SYSCTRL, &sysctrl)) {
                ESP_LOGI(TAG, "AW88082 空闲关断 SYSCTRL=0x%04X（期望 0x0101）", sysctrl);
            }
            if (i2s_channel_disable(tx_handle_) == ESP_OK) {
                tx_running_ = false;
                ESP_LOGI(TAG, "AW88082 空闲：I2S TX 已停止");
            } else {
                ESP_LOGW(TAG, "AW88082 空闲：停止 I2S TX 失败");
            }
        }
    }

    // 只调 AudioCodec 这一层：它只翻 output_enabled_ 标志，不碰 I2S 硬件。
    // （调 NoAudioCodec::EnableOutput 就会 disable 通道，正是我们要避免的。）
    AudioCodec::EnableOutput(enable);
}

// 先推一段静音把 BCK/WCK 跑起来 —— 功放 PLL 要靠这两根线取参考时钟，
// 时钟不动它永远锁不上，后面等 SYSST 必然超时。
void XiaoweiPlusAudioCodec::PrimeAmpClock()
{
    // 全零数据，mono/stereo 都无所谓（只是把 DMA 灌满、让 BCK/WCK 真的动起来）。
    std::vector<int16_t> silence(512, 0);
    size_t written = 0;
    // 写失败不影响主流程：通道使能后 auto_clear 本来也会持续输出静音时钟，
    // 这一步只是让"时钟已经在跑"这件事更确定。
    i2s_channel_write(tx_handle_, silence.data(), silence.size() * sizeof(int16_t),
                      &written, 100);
}

bool XiaoweiPlusAudioCodec::AmpWriteReg(uint8_t reg, uint16_t value)
{
    if (amp_dev_ == nullptr) return false;
    const uint8_t buf[3] = {reg, (uint8_t)(value >> 8), (uint8_t)(value & 0xFF)};
    return i2c_master_transmit(amp_dev_, buf, sizeof(buf), AMP_I2C_TIMEOUT_MS) == ESP_OK;
}

bool XiaoweiPlusAudioCodec::AmpReadReg(uint8_t reg, uint16_t* value)
{
    if (amp_dev_ == nullptr || value == nullptr) return false;

    uint8_t rx[2] = {0};
    esp_err_t err;

    /* 写法上先把 err 接出来再判、不要写成 `if (i2c_master_transmit_receive(...)
     * == ESP_OK)` —— 后者会触发 GCC -Wdangling-pointer 的误报（把"栈上数组的
     * 地址交给了不透明函数、之后又读回来"判成读了失效对象，而 rx 整个作用域内
     * 都活着），本工程是 -Werror=all，直接编不过。touch.cc:107 是同样的事，
     * 之所以没事就是因为它也是先把 err 接出来的。
     *
     * 上面这个 pragma 是保险：GCC 这个 pass 的触发条件没有稳定规律可循，
     * 而一次构建失败的成本远高于这一段局部抑制。作用域只有这几行。 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdangling-pointer"

    err = i2c_master_transmit_receive(amp_dev_, &reg, 1, rx, sizeof(rx),
                                      AMP_I2C_TIMEOUT_MS);
    if (err == ESP_OK) {
        // AW88082 是 MSB 先出
        *value = ((uint16_t)rx[0] << 8) | rx[1];
    }

#pragma GCC diagnostic pop

    return err == ESP_OK;
}

// 轮询 SYSST 的某一位。参考实现里超时**不算致命**（只警告），因为有些板子上
// 位翻转很快、轮询窗口可能刚好错过；这里保持一致，只把它当诊断信号。
bool XiaoweiPlusAudioCodec::AmpWaitStatusBit(uint8_t bit, int timeout_ms, const char* what)
{
    for (int waited = 0; waited < timeout_ms; waited += 2) {
        uint16_t st = 0;
        if (AmpReadReg(AMP_REG_SYSST, &st) && ((st >> bit) & 1)) {
            ESP_LOGI(TAG, "%s OK（%dms）", what, waited);
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    ESP_LOGW(TAG, "%s 超时（%dms）—— 若反复出现，说明 BCK/WCK 没跑起来", what, timeout_ms);
    return false;
}

bool XiaoweiPlusAudioCodec::AmpConfigure()
{
    if (amp_dev_ == nullptr) {
        ESP_LOGW(TAG, "功放不可用（I2C 设备句柄为空），跳过寄存器配置");
        return false;
    }

    uint16_t id = 0;
    if (AmpReadReg(AMP_REG_ID, &id)) {
        // 手册读回 0x2116
        ESP_LOGI(TAG, "AW88082 ID=0x%04X%s", (unsigned)id,
                 (id == 0x2116) ? "（AW88082QNR 确认）" : "（与手册的 0x2116 不符）");
    }

    if (!AmpWriteReg(AMP_REG_I2SCTRL1, AmpI2sCtrl1((uint32_t)output_sample_rate_)) ||
        !AmpWriteReg(AMP_REG_I2SCTRL3, AMP_I2SCTRL3_DEFAULT) ||
        !AmpWriteReg(AMP_REG_SYSCTRL2, AMP_ATTENUATION)) {
        ESP_LOGW(TAG, "功放寄存器配置失败");
        return false;
    }
    uint16_t i2sctrl1 = 0, sysctrl2 = 0;
    if (AmpReadReg(AMP_REG_I2SCTRL1, &i2sctrl1) &&
        AmpReadReg(AMP_REG_SYSCTRL2, &sysctrl2)) {
        ESP_LOGI(TAG, "AW88082 配置确认：I2SCTRL1=0x%04X SYSCTRL2=0x%04X rate=%dHz",
                 i2sctrl1, sysctrl2, output_sample_rate_);
    }
    return true;
}

bool XiaoweiPlusAudioCodec::AmpPowerUp()
{
    // 两步上电，顺序不能合并：
    //   1) 先进 Config 模式（PLL 开、PA 关、硬静音），等 PLL 锁定
    //   2) 再使能 PA，等功放开关就位
    // 直接写最终值（原来的做法）会让 PA 在 PLL 还没锁的时候就被打开。
    if (!AmpWriteReg(AMP_REG_SYSCTRL,
                     (AMP_WORK & ~AMP_EN_PA) | AMP_HMUTE)) {  // 0x2344
        ESP_LOGW(TAG, "功放进入 Config 模式失败");
        return false;
    }
    AmpWaitStatusBit(0, 200, "等待功放 PLL 锁定");

    if (!AmpWriteReg(AMP_REG_SYSCTRL, AMP_WORK | AMP_HMUTE)) {  // 0x2346
        ESP_LOGW(TAG, "功放使能 PA 失败");
        return false;
    }
    AmpWaitStatusBit(8, 100, "等待功放开关就位");
    return true;
}

void XiaoweiPlusAudioCodec::AmpSetMute(bool mute)
{
    if (amp_dev_ == nullptr) return;
    // 解除静音 = 写完整 WORK 且清 HMUTE（0x2246）。
    // 功放没配置成功过就只做静音，避免在 PLL 没锁的情况下把 PA 打开。
    const uint16_t value = (mute || !amp_ready_) ? (AMP_WORK | AMP_HMUTE)
                                                 : (AMP_WORK & ~AMP_HMUTE);
    AmpWriteReg(AMP_REG_SYSCTRL, value);
}
