#include "aw86937.h"

#include <climits>
#include <cstring>
#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_rom_sys.h>
#include <esp_timer.h>

#include "config.h"

/* 官方 AWINIC 驱动（aw86937/ 目录，全部是 .c）。
 *
 * 分工：aw86937/ 里只放芯片引擎（haptic_hv.c）和波形数据
 * （haptic_hv_waveforms.c），外加纯寄存器表 haptic_hv_reg.h。
 * 平台的 I2C 收发、延时、长振动定时器、复位脚都在本文件里实现 ——
 * 也就是官方 haptic_hv_config.c 那份 STM32 HAL 代码的对应物，见下面
 * "官方驱动的平台层" 一节。
 *
 * 原来 haptic_hv_port.h / haptic_hv_waveforms.h 是两个独立小头文件，
 * 2026-10-14 并进了 haptic_hv.h。 */
#include "aw86937/haptic_hv.h"
#include "aw86937/haptic_hv_reg.h"

// 平台负载锁（本板无电源域仲裁，main.cpp 里是空实现）
extern "C" {
    extern void board_load_release(int load_type);
    extern bool board_load_acquire(int load_type, int timeout);
}
#define BOARD_LOAD_VIB 1

static const char* TAG = "Aw86937";

/* 短振动的兜底等待上限。波形播完后芯片会自己回 standby，这里只是防止
 * 万一回不去时任务永远卡在这个循环里（那会让后续所有播放请求被静默忽略）。 */
static const int VIB_SHORT_WAIT_MAX_MS = 3000;

/* 起振等待：play_go 之后芯片需要一段时间才离开 STANDBY，
 * 立刻去查 GLBRD5 会读到 STANDBY 而误判成"已经播完"。 */
static const int VIB_STARTUP_SETTLE_MS = 30;

/* 单次 I2C 超时。官方用 1000ms，乘以重试次数会放大成秒级卡死，
 * 这里收紧到 50ms：正常一次寄存器写只要几十微秒。 */
#define HAPTIC_I2C_TIMEOUT_MS (50)

/* 单次 I2C 写能带的最大数据长度。
 * RAM 分块用的是 AW_RAMDATA_WR_BUFFER_SIZE(256)，RTP 帧是 AW_RTP_FRAME_LEN(512)，
 * 取大的那个。RTP 当前没启用（需要 INT 脚），但缓冲按它开，免得以后接通了踩坑。 */
#define HAPTIC_I2C_WR_MAX_LEN AW_RTP_FRAME_LEN

// ===========================================================================
//  官方驱动的平台层（对应官方 haptic_hv_config.c，STM32 HAL -> ESP-IDF）
//
//  对照关系：
//    HAL_I2C_Mem_Read/Write  -> i2c_master_transmit_receive / i2c_master_transmit
//    HAL_Delay               -> vTaskDelay + esp_rom_delay_us 混合（见 haptic_hv_mdelay）
//    HAL_Delay_us            -> esp_rom_delay_us
//    HAL_TIM_Base_Start_IT   -> esp_timer 一次性定时器
//    HAL_GPIO_WritePin       -> gpio_set_level
//    HAL_NVIC_xxxIRQ         -> 空实现（本板 INT 脚没接）
//
//  这些函数由 haptic_hv.h 以 C 链接声明，引擎（C 文件）调用。
// ===========================================================================
extern "C" {

static i2c_master_dev_handle_t s_port_i2c_dev = NULL;
static esp_timer_handle_t s_vib_timer = NULL;
static TaskHandle_t s_play_task = NULL;
static bool s_timer_ready = false;

/* ---------------------------- I2C ---------------------------- */

void haptic_hv_port_bind_i2c(i2c_master_dev_handle_t dev)
{
	s_port_i2c_dev = dev;
}

int haptic_hv_i2c_reads(uint8_t reg_addr, uint8_t *reg_data, uint16_t len)
{
	int cnt = 0;

	if (s_port_i2c_dev == NULL || reg_data == NULL || len == 0)
		return AW_ERROR;

	while (cnt < AW_I2C_RETRIES) {
		if (i2c_master_transmit_receive(s_port_i2c_dev, &reg_addr, 1, reg_data,
										(size_t)len, HAPTIC_I2C_TIMEOUT_MS) == ESP_OK) {
			return AW_SUCCESS;
		}
		cnt++;
	}
	AW_LOGE("i2c read 0x%02X err!", reg_addr);
	return AW_ERROR;
}

int haptic_hv_i2c_writes(uint8_t reg_addr, const uint8_t *reg_data, uint16_t len)
{
	/* 栈上拼 [寄存器地址][数据...]。
	 * i2c_master_transmit 会阻塞到发完，且 I2C ISR 直接读这块内存，
	 * 所以不能图省事把 .rodata 里的波形数据直接甩给驱动。 */
	uint8_t buf[1 + HAPTIC_I2C_WR_MAX_LEN];
	int cnt = 0;

	if (s_port_i2c_dev == NULL || reg_data == NULL || len == 0)
		return AW_ERROR;

	if (len > HAPTIC_I2C_WR_MAX_LEN) {
		AW_LOGE("i2c write 0x%02X len %u exceeds buffer %d",
				reg_addr, len, (int)HAPTIC_I2C_WR_MAX_LEN);
		return AW_ERROR;
	}

	buf[0] = reg_addr;
	memcpy(&buf[1], reg_data, len);

	while (cnt < AW_I2C_RETRIES) {
		if (i2c_master_transmit(s_port_i2c_dev, buf, (size_t)len + 1,
								HAPTIC_I2C_TIMEOUT_MS) == ESP_OK) {
			return AW_SUCCESS;
		}
		cnt++;
	}
	AW_LOGE("i2c write 0x%02X err!", reg_addr);
	return AW_ERROR;
}

void haptic_hv_i2c_write_bits(uint8_t reg_addr, uint32_t mask, uint8_t reg_data)
{
	uint8_t reg_val = 0;

	haptic_hv_i2c_reads(reg_addr, &reg_val, AW_I2C_BYTE_ONE);
	reg_val &= (uint8_t)mask;
	reg_val |= (reg_data & (~mask));
	haptic_hv_i2c_writes(reg_addr, &reg_val, AW_I2C_BYTE_ONE);
}

/* ---------------------------- 延时 ---------------------------- */

/*
 * 官方驱动里 mdelay 用得极其密集：hw_reset 的 mdelay(8)、offset_cali 的
 * mdelay(3)、wait_enter_standby 的 100 × mdelay(2)……全都是"几毫秒"级别。
 *
 * 坑：ESP-IDF 默认 CONFIG_FREERTOS_HZ=100，pdMS_TO_TICKS(2) 算出来是 0，
 * vTaskDelay(0) 立即返回 —— 上面那些延时全部变成空操作，后果是：
 *   - haptic_hv_hw_reset() 里拉高 RSTN 后的 8ms 建立时间没了，
 *     紧接着就发 I2C 读 chip id，芯片还没醒 -> boot_init 直接失败
 *   - aw8693x_offset_cali() 的 3ms 没了，DETRD1 读回来是垃圾
 *
 * 所以 10ms 以下一律忙等（这些只在开机和停止时出现，次数有限），
 * 10ms 以上才真正让出 CPU。
 */
void haptic_hv_mdelay(uint32_t ms)
{
	if (ms == 0)
		return;

	if (ms < 10)
		esp_rom_delay_us(ms * 1000U);
	else
		vTaskDelay(pdMS_TO_TICKS(ms));
}

void haptic_hv_udelay(uint32_t us)
{
	esp_rom_delay_us(us);
}

/* ---------------------- 长振动定时器 ---------------------- */

static void vib_timer_cb(void *arg)
{
	(void)arg;

	/*
	 * 这里绝对不能碰 I2C。aw8693x_play_stop() 是个多事务序列，最坏要在
	 * aw8693x_wait_enter_standby() 里轮询 100 次 × 2ms ≈ 200ms；把它放在
	 * esp_timer 回调里会阻塞整个 esp_timer 任务，拖垮系统里所有定时器用户。
	 * 只发通知，真正的停止交给播放任务做。
	 */
	/* 定时器已经落地，清掉"正在计时"的标志。
	 * haptic_hv_vib_work_cancel() 靠 timer_ms_cnt != 0 判断要不要停定时器。 */
	g_haptic_hv->timer_ms_cnt = 0;

	if (s_play_task != NULL)
		xTaskNotify(s_play_task, AW_VIB_NOTIFY_TIMEOUT, eSetValueWithOverwrite);
}

void haptic_hv_start_hrtimer(void)
{
	uint32_t duration_ms;

	if (!s_timer_ready)
		return;

	duration_ms = g_haptic_hv->duration;
	if (duration_ms == 0)
		return;

	if (duration_ms > AW_VIB_DURATION_MAX_MS) {
		AW_LOGE("duration %u exceeds max, clamp to %d ms",
				(unsigned)duration_ms, AW_VIB_DURATION_MAX_MS);
		duration_ms = AW_VIB_DURATION_MAX_MS;
	}

	/*
	 * 用一次性定时器，不移植官方那套 1ms 周期 + timer_ms_cnt 计数器。
	 *
	 * 原因：官方判据是 timer_ms_cnt == duration 的精确相等，而计数器靠
	 * 定时器回调唤醒任务来递增。ulTaskNotifyTake(pdTRUE, ...) 会把积压的
	 * 计数信号量一次性清空，只要任务被抢占过一次（比如正卡在 play_stop 里），
	 * 计数值就会跳过目标值，判据永远不成立 —— 而 long_vib_work 用的是
	 * loop-15 近似无限循环，结果就是马达一直转到掉电。
	 * 一次性定时器没有累加，天然没有这个问题，代码也更短。
	 */
	esp_timer_stop(s_vib_timer); /* 未启动时返回 ESP_ERR_INVALID_STATE，忽略 */
	if (esp_timer_start_once(s_vib_timer, (uint64_t)duration_ms * 1000ULL) != ESP_OK) {
		AW_LOGE("start vib timer failed");
		return;
	}

	/* 这里只是借用 timer_ms_cnt 当"定时器正在跑"的布尔标志。
	 * haptic_hv_vib_work_cancel() 判的是 timer_ms_cnt != 0，
	 * 官方原版由 1ms 回调去累加它，我们没移植那个计数器，只能自己维护，
	 * 否则 cancel 永远不会调 stop_hrtimer()，上一次的长振动定时器会活到
	 * 下一次播放时才炸。具体数值无意义，非 0 即可。 */
	g_haptic_hv->timer_ms_cnt = 1;
}

void haptic_hv_stop_hrtimer(void)
{
	/* 标志先清，且不受 s_timer_ready 影响：
	 * cancel() 只看 timer_ms_cnt，看漏了它就会走进"以为定时器还在"的分支。 */
	if (g_haptic_hv != NULL)
		g_haptic_hv->timer_ms_cnt = 0;

	if (!s_timer_ready)
		return;

	/* 无条件停。官方那份只在 timer_ms_cnt != 0 时才停，会让上一次遗留的
	 * 定时器活到下一次播放，然后把新播放停掉。 */
	esp_timer_stop(s_vib_timer); /* 未启动时返回 ESP_ERR_INVALID_STATE，忽略 */
}

/* -------------------- 中断与复位脚 -------------------- */

/*
 * 本板 AW86937 的 INT 脚没有接到 ESP32-P4（config.h 里只有 RST 和 TRIG1/2/3），
 * 所以这两个是空实现。中断只服务于 RTP 流式播放，RAM 模式的
 * short_vib_work / long_vib_work 完全不需要它。
 *
 * 将来要接 RTP：在 gpio 上装 ISR，回调里
 * g_haptic_hv->irq_handle_flag = AW_IRQ_ON，然后由一个任务调
 * g_func_haptic_hv->irq_handle()。这里补上 gpio_intr_disable/enable 即可。
 */
void haptic_hv_disable_irq(void)
{
}

void haptic_hv_enable_irq(void)
{
}

void haptic_hv_set_reset_gpio(uint16_t GPIO_Pin, uint8_t state)
{
	gpio_set_level((gpio_num_t)GPIO_Pin, state ? 1 : 0);
}

/* ---------------------- 校准值存储 ---------------------- */

/*
 * 官方把 F0 校准值存进外部 flash，避免每次开机重算。
 * 本工程打开了 AW_F0_CALI_DURING_STARTUP，开机每次都会重新校准，
 * 所以这两个函数目前不会被调用，只打日志。
 *
 * 要启用持久化（把 haptic_hv.h 里的 AW_F0_CALI_DURING_STARTUP 注释掉）时，
 * 在这里接 NVS 即可：nvs_open("aw86937") / nvs_get_u8("f0_cali", ...)。
 * 注意需要先确保 nvs_flash_init() 已经跑过。
 */
void haptic_hv_set_cali_to_flash(void)
{
	AW_LOGI("f0 cali data is 0x%02x (not persisted)", g_haptic_hv->f0_cali_data);
}

void haptic_hv_get_cali_from_flash(void)
{
	AW_LOGI("f0 cali data is 0x%02x (from default)", g_haptic_hv->f0_cali_data);
}

/* ------------------------ 生命周期 ------------------------ */

esp_err_t haptic_hv_port_init(void)
{
	const esp_timer_create_args_t args = {
		.callback = vib_timer_cb,
		.arg = NULL,
		/* ESP_TIMER_TASK 是默认值，写出来是为了明确：回调跑在 esp_timer 任务里，
		 * 可以安全调用 xTaskNotify。如果将来有人打开
		 * CONFIG_ESP_TIMER_SUPPORTS_ISR_DISPATCH_METHOD，回调会跑在中断里，
		 * xTaskNotify 就不合法了，必须换 FromISR 版本。 */
		.dispatch_method = ESP_TIMER_TASK,
		.name = "aw86937_vib",
		.skip_unhandled_events = false,
	};

	if (s_timer_ready)
		return ESP_OK;

	if (esp_timer_create(&args, &s_vib_timer) != ESP_OK) {
		AW_LOGE("create vib timer failed");
		s_vib_timer = NULL;
		return ESP_FAIL;
	}

	s_timer_ready = true;
	return ESP_OK;
}

void haptic_hv_port_set_play_task(TaskHandle_t task)
{
	s_play_task = task;
}

bool haptic_hv_port_timer_running(void)
{
	return s_timer_ready && esp_timer_is_active(s_vib_timer);
}

void haptic_hv_port_deinit(void)
{
	/* 先摘掉任务句柄，防止回调在删除定时器的空档里再去通知一个已死的任务 */
	s_play_task = NULL;

	if (s_timer_ready && s_vib_timer != NULL) {
		esp_timer_stop(s_vib_timer);
		esp_timer_delete(s_vib_timer);
		s_vib_timer = NULL;
		s_timer_ready = false;
	}

	s_port_i2c_dev = NULL;
}

} /* extern "C" */

// ===========================================================================
//                              对外的马达类
// ===========================================================================

Aw86937::Aw86937(i2c_master_bus_handle_t i2c_bus, uint8_t addr)
    // AW86937 RTP at 48 kHz requires at least 1 MHz I2C (datasheet Table 3).
    : I2cDevice(i2c_bus, addr),
      is_playing_(false),
      play_task_handle_(nullptr),
      initialized_(false),
      gain_(0x80),
      intensity_percent_(100),
      mode_(Mode::Standard),
      pending_index_(1),
      pending_loop_(1),
      pending_duration_ms_(0),
      f0_result_hz_(0),
      f0_result_raw_(0),
      f0_result_cali_ok_(0) {
}

Aw86937::~Aw86937() {
    // 先停掉长振动定时器，并摘掉端口层登记的播放任务句柄 ——
    // 否则定时器回调可能在我们删掉任务之后，再去通知一个已经失效的句柄。
    haptic_hv_port_deinit();

    if (play_task_handle_ != nullptr) {
        // 通知它走一遍收尾（停芯片 + 归还负载锁）。若它正卡在 WaitPlayDone 里，
        // 这条通知会挂起，等它回到 xTaskNotifyWait 时被吃掉，是个空操作。
        xTaskNotify(play_task_handle_, AW_VIB_NOTIFY_TIMEOUT, eSetValueWithOverwrite);
        vTaskDelay(pdMS_TO_TICKS(20));
        vTaskDelete(play_task_handle_);
        play_task_handle_ = nullptr;
    }

    // 兜底：任务没来得及归还的负载锁在这里补上，绝不允许泄漏。
    // exchange 保证和 ReleaseLoadLock() 之间只会真正释放一次。
    if (is_playing_.exchange(false)) {
        if (g_func_haptic_hv != nullptr)
            g_func_haptic_hv->play_stop();  // 别把马达丢在转
        board_load_release(BOARD_LOAD_VIB);
    }

    initialized_ = false;
}

esp_err_t Aw86937::Init() {
    // ---- 1. 先配好 RSTN / TRIGx ----
    // 顺序不能反：haptic_hv_boot_init() 第一件事就是 hw_reset()，里面直接
    // gpio_set_level(RSTN)。引脚没配好的话那次电平操作是空操作，芯片停在
    // Power-Down，随后读 chip id 必然失败（旧驱动也踩过这个坑）。
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = (1ULL << HAPTIC_RST_PIN) |
                           (1ULL << HAPTIC_TRIG1_PIN) |
                           (1ULL << HAPTIC_TRIG2_PIN) |
                           (1ULL << HAPTIC_TRIG3_PIN);
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.intr_type = GPIO_INTR_DISABLE;

    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "配置 RSTN/TRIG 引脚失败: %s", esp_err_to_name(err));
        return err;
    }

    // TRIG1/2/3 不用（硬件触发通道），保持低电平
    gpio_set_level(HAPTIC_TRIG1_PIN, 0);
    gpio_set_level(HAPTIC_TRIG2_PIN, 0);
    gpio_set_level(HAPTIC_TRIG3_PIN, 0);

    // ---- 2. 释放 RSTN，把芯片叫醒 ----
    // 必须放在探测之前：gpio_config() 只是把引脚配成输出，电平还停在默认低，
    // 而 RSTN=0 时 AW86937 处于 Power-Down，I2C 根本不响应 —— 直接 probe 必然
    // 返回 ESP_ERR_NOT_FOUND，看起来像"芯片不在总线上"，其实是没唤醒。
    // 时序照抄官方 haptic_hv_hw_reset()：0 -> 2ms -> 1 -> 8ms。
    // 之后 haptic_hv_boot_init() 还会再复位一次，重复一次无害。
    haptic_hv_set_reset_gpio(HAPTIC_RST_PIN, 0);
    haptic_hv_mdelay(2);
    haptic_hv_set_reset_gpio(HAPTIC_RST_PIN, 1);
    haptic_hv_mdelay(8);

    // ---- 3. 前面挡一道探测 ----
    // 芯片不在线时，官方驱动的 I2C 重试（AW_I2C_RETRIES × AW_READ_CHIPID_RETRIES）
    // 会耗掉十几秒，日志里只有一堆 i2c 错误，看不出根因。先探一下，失败就快速返回。
    esp_err_t probe = ESP_ERR_NOT_FOUND;
    for (int i = 0; i < 3; i++) {
        probe = i2c_master_probe(i2c_bus_, device_address_, 100);
        if (probe == ESP_OK) break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (probe != ESP_OK) {
        ESP_LOGE(TAG, "I2C 上找不到 AW86937 (0x%02X): %s（检查 RSTN / VBAT 与 I2C 接线）",
                 device_address_, esp_err_to_name(probe));
        return probe;
    }

    // ---- 4. 交给官方驱动引擎 ----
    // 从这里开始，芯片检测 / offset 校准 / RAM 库下载 / F0 校准 / boost 配置
    // 全部由 haptic_hv_boot_init() 完成，本类不再自己写任何寄存器。
    haptic_hv_port_bind_i2c(i2c_device_);

    if (haptic_hv_port_init() != ESP_OK) {
        ESP_LOGE(TAG, "长振动定时器创建失败");
        return ESP_FAIL;
    }

    if (haptic_hv_boot_init() != AW_SUCCESS) {
        ESP_LOGE(TAG, "haptic_hv_boot_init 失败，详见上面 haptic_hv 的日志");
        haptic_hv_port_deinit();
        return ESP_FAIL;
    }

    gain_ = g_haptic_hv->gain;

    // ---- 5. 播放任务 ----
    // 必须在允许任何播放之前建好：长振动的定时器回调要往这个句柄发通知，
    // 句柄为空的话定时器到点了没人停芯片。
    if (xTaskCreate(PlayTaskWrapper, "vib_play", 4096, this, 6, &play_task_handle_) != pdPASS) {
        ESP_LOGE(TAG, "创建播放任务失败");
        play_task_handle_ = nullptr;
        haptic_hv_port_deinit();
        return ESP_ERR_NO_MEM;
    }
    haptic_hv_port_set_play_task(play_task_handle_);

    initialized_ = true;
    ESP_LOGI(TAG, "AW86937 就绪: chipid=0x%06X, F0=%u Hz, ram_num=%u, gain=0x%02X",
             (unsigned)g_haptic_hv->chipid, (unsigned)g_haptic_hv->f0,
             (unsigned)g_haptic_hv->ram_num, gain_);

    // 开机日志（chipid / offset 校准 / RAM 下载 / F0 校准）到这里已经打完，
    // 之后官方引擎每播一次都要打 play_go / play_stop / play_mode 一堆 INFO，
    // 正常跑起来就是刷屏。降到 WARN：错误照常输出，噪音没了。
    // 要重新看细节，把这一行注释掉，或者运行期再调回 ESP_LOG_INFO。
    esp_log_level_set(HAPTIC_HV_LOG_TAG, ESP_LOG_WARN);

    return ESP_OK;
}

// ============================ 播放接口 ============================

esp_err_t Aw86937::Submit(uint8_t index, uint8_t loop, uint32_t duration_ms) {
    if (!initialized_ || play_task_handle_ == nullptr) {
        ESP_LOGW(TAG, "驱动未初始化，忽略播放请求");
        return ESP_ERR_INVALID_STATE;
    }

    if (index == 0 || index > g_haptic_hv->ram_num) {
        ESP_LOGE(TAG, "波形序号 %u 超出范围 (1~%u)", index, (unsigned)g_haptic_hv->ram_num);
        return ESP_ERR_INVALID_ARG;
    }

    // 用 CAS 抢占"正在播放"这个位置。普通读改写在这里挡不住并发调用，
    // 一旦两个线程同时通过检查，负载锁会被 acquire 两次而只 release 一次。
    bool expected = false;
    if (!is_playing_.compare_exchange_strong(expected, true))
        return ESP_OK;  // 已经在播，忽略（不排队）

    if (!board_load_acquire(BOARD_LOAD_VIB, 0)) {
        is_playing_.store(false);
        ESP_LOGW(TAG, "负载锁获取失败，忽略播放请求");
        return ESP_ERR_INVALID_STATE;
    }

    pending_index_ = index;
    pending_loop_ = loop;
    pending_duration_ms_ = duration_ms;

    xTaskNotify(play_task_handle_, AW_VIB_NOTIFY_PLAY, eSetValueWithOverwrite);
    return ESP_OK;
}

esp_err_t Aw86937::PlayWave(uint8_t index, uint8_t loop) {
    if (loop == 0) loop = 1;
    if (loop > 15) loop = 15;  // 官方 aw_loop 上限是 AW_LOOP15
    return Submit(index, loop, 0);
}

esp_err_t Aw86937::LoadWaveform(uint8_t index) {
    const uint8_t *data = nullptr;
    uint32_t len = 0;
    switch (index) {
    case 1: data = aw86937_wave_alarm; len = aw86937_wave_alarm_len; break;
    case 2: data = aw86937_wave_error_part1; len = aw86937_wave_error_part1_len; break;
    case 3: data = aw86937_wave_error_part2; len = aw86937_wave_error_part2_len; break;
    case 4: data = aw86937_wave_normal; len = aw86937_wave_normal_len; break;
    case 5: data = aw86937_wave_soft; len = aw86937_wave_soft_len; break;
    case 6: data = aw86937_wave_strong; len = aw86937_wave_strong_len; break;
    case 7: data = aw86937_wave_success; len = aw86937_wave_success_len; break;
    default: return ESP_ERR_INVALID_ARG;
    }
    return haptic_hv_load_ram_table(data, len) == AW_SUCCESS ? ESP_OK : ESP_FAIL;
}

esp_err_t Aw86937::PlayLong(uint8_t index, uint32_t duration_ms) {
    if (duration_ms == 0) {
        ESP_LOGE(TAG, "时长不能为 0");
        return ESP_ERR_INVALID_ARG;
    }
    // 官方 long_vib_work 用 loop-15 近似无限循环，全靠这个定时器停。
    // 定时器上限在平台层也是 10s，这里提前拦下来，别让调用方以为设成了 60s。
    if (duration_ms > AW_VIB_DURATION_MAX_MS)
        duration_ms = AW_VIB_DURATION_MAX_MS;
    return Submit(index, 1, duration_ms);
}

esp_err_t Aw86937::PlayHeartbeat() {
    // 心跳波形库只有 1 段（base_addr=0, ram_num=1），所以固定 index=1
    return PlayWave(1, 1);
}

esp_err_t Aw86937::Stop() {
    if (play_task_handle_ == nullptr || !is_playing_.load())
        return ESP_OK;

    // 复用 TIMEOUT 那条路径做"停止"：它做的事就是 play_stop + 归还负载锁，
    // 与"长振动到点"完全一样。若任务正卡在 WaitPlayDone 里，这条通知会挂起，
    // 等它回到 xTaskNotifyWait 时被吃掉 —— 那时它自己已经停过了，
    // ReleaseLoadLock() 是幂等的，不会重复停或重复归还。
    xTaskNotify(play_task_handle_, AW_VIB_NOTIFY_TIMEOUT, eSetValueWithOverwrite);
    return ESP_OK;
}

esp_err_t Aw86937::SetGain(uint8_t gain) {
    // 官方增益档位 0x80(最大) ~ 0x10(最小)
    if (gain < 0x10 || gain > 0x80) {
        ESP_LOGE(TAG, "增益 0x%02X 超出范围 (0x10~0x80)", gain);
        return ESP_ERR_INVALID_ARG;
    }
    gain_ = gain;
    g_haptic_hv->gain = gain;
    return ESP_OK;
}

// 强度按百分比折算成官方增益档。aw_gain 只有 8 个离散档位(0x10~0x80)，
// 必须量化落档：g_haptic_hv->gain 这个全局值会被长振动的电池补偿
// (haptic_hv_ram_vbat_comp) 放大复用，非档位值补偿后可能越界，
// 触发底层 "gain value out of range" 导致强度异常。
// 与 SetGain 一样只记参数，下一次播放才写寄存器。
esp_err_t Aw86937::SetIntensity(uint8_t percent) {
    if (percent > 100) return ESP_ERR_INVALID_ARG;
    intensity_percent_ = percent;
    static const uint8_t kLevels[AW_GAIN_LEVEL_NUM_MAX] = {
        AW_GAIN_LEVEL_8, AW_GAIN_LEVEL_7, AW_GAIN_LEVEL_6, AW_GAIN_LEVEL_5,
        AW_GAIN_LEVEL_4, AW_GAIN_LEVEL_3, AW_GAIN_LEVEL_2, AW_GAIN_LEVEL_1,
    };
    const int idx = ((uint32_t)percent * (AW_GAIN_LEVEL_NUM_MAX - 1) + 50) / 100;
    return SetGain(kLevels[idx]);
}

// 模式本身不写寄存器：轻柔/标准/强弱的区别由 RAM 波形库的波形 1/2/3 承担，
// 这里只是把模式记下来并按当前强度刷新增益，供界面层组合使用。
esp_err_t Aw86937::SetMode(Mode mode) {
    mode_ = mode;
    return SetIntensity(intensity_percent_);
}

// ========================== 共振频率探测 ==========================

Aw86937::F0Result Aw86937::MeasureF0(int timeout_ms) {
    // get_f0()：只测不写，TRIMCFG5 和 f0_cali_data 都不动
    return RunF0Job(AW_VIB_NOTIFY_F0, timeout_ms);
}

Aw86937::F0Result Aw86937::RecalibrateF0(int timeout_ms) {
    // f0_cali()：测完还会按偏差改 TRIMCFG5，等于把开机校准重跑一遍
    return RunF0Job(AW_VIB_NOTIFY_F0_CALI, timeout_ms);
}

// 两个接口的公共骨架：把活儿丢给播放任务，然后等结果回来。
// 区别只有"让播放任务干什么"，停播放 / 拿负载锁 / 超时兜底完全一样。
Aw86937::F0Result Aw86937::RunF0Job(uint32_t notify_value, int timeout_ms) {
    F0Result r = {};
    r.hz        = -1;
    r.raw       = 0;
    r.cali_ok   = false;
    r.pre_hz    = aw8693x_dts.f0_pre;
    r.pct       = aw8693x_dts.f0_cali_percent;
    r.lo_hz     = aw8693x_dts.f0_pre * (100 - aw8693x_dts.f0_cali_percent) / 100;
    r.hi_hz     = aw8693x_dts.f0_pre * (100 + aw8693x_dts.f0_cali_percent) / 100;
    r.cali_data = (g_haptic_hv != nullptr) ? g_haptic_hv->f0_cali_data : 0;

    if (!initialized_ || play_task_handle_ == nullptr) {
        ESP_LOGE(TAG, "驱动未初始化，无法探测 F0");
        return r;
    }

    // 先停掉正在进行的振动，并等播放任务真正收尾。
    // get_f0() 自己开头会 play_stop()，但要是播放任务此刻正卡在
    // short_vib_work() 的多事务序列中间，两边 I2C 交错会把芯片写坏 ——
    // 这正是本类所有 I2C 都收在播放任务里跑的原因。
    Stop();
    const int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (is_playing_.load() && esp_timer_get_time() < deadline)
        vTaskDelay(1);

    f0_result_hz_ = 0;  // 0 = 尚未回来，播放任务靠写非 0 表示完成
    xTaskNotify(play_task_handle_, notify_value, eSetValueWithOverwrite);

    while (f0_result_hz_ == 0 && esp_timer_get_time() < deadline)
        vTaskDelay(1);

    if (f0_result_hz_ == 0) {
        ESP_LOGE(TAG, "F0 探测超时 (%d ms)", timeout_ms);
        f0_result_hz_ = -1;  // 复位，免得下一次调用一进来就以为已经有结果
        return r;
    }

    r.hz  = f0_result_hz_;
    r.raw = f0_result_raw_;
    r.cali_ok = (f0_result_cali_ok_ != 0);
    // 重校准会改 f0_cali_data，所以要重新读一次，不能用开头那份快照
    if (g_haptic_hv != nullptr)
        r.cali_data = g_haptic_hv->f0_cali_data;
    return r;
}

// ============================== 状态 ==============================

int Aw86937::GetF0Hz() const {
    return (g_haptic_hv != nullptr) ? (int)g_haptic_hv->f0 : 0;
}

uint32_t Aw86937::GetVbat() const {
    return (g_haptic_hv != nullptr) ? g_haptic_hv->vbat : 0;
}

uint8_t Aw86937::GetRamNum() const {
    return (g_haptic_hv != nullptr) ? g_haptic_hv->ram_num : 0;
}

// ============================ 播放任务 ============================

void Aw86937::PlayTaskWrapper(void* arg) {
    static_cast<Aw86937*>(arg)->PlayTask();
}

void Aw86937::PlayTask() {
    uint32_t notify_value = 0;

    while (1) {
        // 带值通知：PLAY 和 TIMEOUT 必须区分开，否则长振动到点会被当成一次新的播放
        if (xTaskNotifyWait(0, ULONG_MAX, &notify_value, portMAX_DELAY) != pdTRUE)
            continue;

        switch ((aw_vib_notify)notify_value) {
        case AW_VIB_NOTIFY_PLAY: {
            const uint8_t index = pending_index_;
            const uint32_t duration_ms = pending_duration_ms_;

            if (duration_ms > 0) {
                // 长振动：芯片进入 RAM_LOOP_MODE 自循环，由定时器回调发 TIMEOUT 收尾。
                // 所以这条分支成功时【不能】归还负载锁，否则振动还在继续锁就没了。
                if (g_func_haptic_hv->long_vib_work((aw_index)index, (aw_gain)gain_, duration_ms) != AW_SUCCESS) {
                    // 起播失败 -> 定时器没起来 -> 永远等不到 TIMEOUT，必须自己收尾
                    ESP_LOGE(TAG, "长振动启动失败 (index=%u, duration=%u ms)",
                             index, (unsigned)duration_ms);
                    ReleaseLoadLock();
                    break;
                }

                // 起播成功也还要回头确认一次定时器真的在跑。
                // long_vib_work 内部把波形设成 loop-15 近似无限循环，全靠这个定时器停；
                // esp_timer_start_once 万一失败它是察觉不到的（start_hrtimer 返回 void），
                // 那样就永远等不到 TIMEOUT，马达会一直转到掉电。
                if (!haptic_hv_port_timer_running()) {
                    ESP_LOGE(TAG, "长振动定时器未启动，立即停止以免马达失控");
                    g_func_haptic_hv->play_stop();
                    ReleaseLoadLock();
                }
                break;
            }

            // 短振动：芯片自己播 loop 次后回 standby，没有定时器，得有人等它播完
            const uint8_t loop = pending_loop_;
            if (g_func_haptic_hv->short_vib_work((aw_index)index, (aw_gain)gain_, (aw_loop)loop) != AW_SUCCESS) {
                ESP_LOGE(TAG, "短振动启动失败 (index=%u, loop=%u)", index, loop);
                ReleaseLoadLock();
                break;
            }

            WaitPlayDone();
            g_func_haptic_hv->play_stop();
            ReleaseLoadLock();
            break;
        }

        case AW_VIB_NOTIFY_TIMEOUT:
            // 长振动到点，或外部调 Stop()
            g_func_haptic_hv->play_stop();
            ReleaseLoadLock();
            break;

        case AW_VIB_NOTIFY_F0: {
            // 官方 get_f0()：把芯片切到 CONT 模式驱动 LRA，靠内置的谐振跟踪
            // （CONTCFG6 的 TRACK_ENABLE）锁定到实际共振点，结果落在 CONTRD14。
            // 这一步也在实际驱动马达，所以要拿负载锁，跑完把状态收干净。
            int hz = -1;
            uint16_t raw = 0;

            if (!board_load_acquire(BOARD_LOAD_VIB, 0)) {
                ESP_LOGW(TAG, "F0 探测：负载锁获取失败");
            } else {
                if (g_func_haptic_hv->get_f0() == AW_SUCCESS) {
                    hz = (int)g_haptic_hv->f0;

                    // 顺手把原始寄存器读回来。官方 read_f0() 只留下换算后的
                    // f0 = 3840000 / raw，raw 本身丢掉了；有它才能算出当前
                    // 测量点的分辨率（1 LSB 对应多少 Hz），看读数有多可信。
                    uint8_t reg[2] = {0};
                    if (haptic_hv_i2c_reads(AW8693X_REG_CONTRD14, reg, AW_I2C_BYTE_TWO) == AW_SUCCESS)
                        raw = (uint16_t)((reg[0] << 8) | reg[1]);
                } else {
                    ESP_LOGE(TAG, "官方 get_f0() 报错，本次读数作废");
                }

                // 探测把 play_mode 改成了 CONT 模式，这里强制回 standby，
                // 别让下一次 short_vib_work 从一个残留状态起步。
                g_func_haptic_hv->play_stop();
                board_load_release(BOARD_LOAD_VIB);
            }

            // raw / cali_ok 先写、hz 后写：hz 非 0 才是"结果就绪"的标志（见头文件注释）
            f0_result_raw_ = raw;
            f0_result_cali_ok_ = 0;  // 这条路径不做校准
            f0_result_hz_ = hz;
            break;
        }

        case AW_VIB_NOTIFY_F0_CALI: {
            // 官方 f0_cali()：先 upload_lra(AW_WRITE_ZERO) 把 TRIMCFG5 清掉，
            // 再 get_f0() 测一次，落在窗口内就按偏差算 trim 写回去。
            // 和开机那次启动校准（AW_F0_CALI_DURING_STARTUP）走的是同一条路。
            //
            // 注意：读数超窗口时它直接 return AW_ERROR，且【不】写 TRIMCFG5，
            // 所以返回失败 != 没测到 —— g_haptic_hv->f0 里照样是本次读数。
            int hz = -1;
            uint16_t raw = 0;
            int cali_ok = 0;

            if (!board_load_acquire(BOARD_LOAD_VIB, 0)) {
                ESP_LOGW(TAG, "F0 重校准：负载锁获取失败");
            } else {
                const int ret = g_func_haptic_hv->f0_cali();
                hz = (int)g_haptic_hv->f0;
                cali_ok = (ret == AW_SUCCESS) ? 1 : 0;

                uint8_t reg[2] = {0};
                if (haptic_hv_i2c_reads(AW8693X_REG_CONTRD14, reg, AW_I2C_BYTE_TWO) == AW_SUCCESS)
                    raw = (uint16_t)((reg[0] << 8) | reg[1]);

                g_func_haptic_hv->play_stop();
                board_load_release(BOARD_LOAD_VIB);
            }

            f0_result_raw_ = raw;
            f0_result_cali_ok_ = cali_ok;
            f0_result_hz_ = hz;
            break;
        }

        default:
            break;
        }
    }
}

void Aw86937::WaitPlayDone() {
    vTaskDelay(pdMS_TO_TICKS(VIB_STARTUP_SETTLE_MS));

    const int64_t deadline = esp_timer_get_time() + (int64_t)VIB_SHORT_WAIT_MAX_MS * 1000;
    while (esp_timer_get_time() < deadline) {
        // 顺便看一眼有没有人喊停。Stop() 发的就是 TIMEOUT 通知，在这里吃掉它，
        // 就不必等波形自己播完（可能要到 3 秒兜底上限）。
        // 吃掉之后外层不会走 TIMEOUT 分支，但外层本来就要做
        // play_stop + 归还负载锁，收尾完全一样，不会漏掉什么。
        uint32_t pending = 0;
        if (xTaskNotifyWait(0, ULONG_MAX, &pending, 0) == pdTRUE)
            return;

        if (g_func_haptic_hv->get_glb_state() == AW8693X_BIT_GLBRD5_STATE_STANDBY)
            return;

        // 这里用 tick 数而不是 pdMS_TO_TICKS(5)：CONFIG_FREERTOS_HZ=100 时
        // pdMS_TO_TICKS(5) == 0，vTaskDelay(0) 立即返回，循环退化成全速轮询 I2C
        // （每秒上万次读），把总线和同一条总线上的 PMIC 喂狗任务全挤掉。
        // 1 tick = 10ms，作为轮询间隔正合适。
        vTaskDelay(1);
    }

    // 到点了还没回 standby：多半是波形库设了很长的循环，或者 I2C 出问题了。
    // 不在这里强行停 —— 后面的 play_stop() 会处理。
    // 把 GLBRD5 原值打出来：0x07=RAM_GO（还在播）、0x0B=BRAKE（在刹车）、
    // 0x0C=END（波形播完但没回待机），比只说"超时"有用得多。
    ESP_LOGW(TAG, "等待短振动结束超时 (%d ms), GLBRD5 = 0x%02X",
             VIB_SHORT_WAIT_MAX_MS, g_func_haptic_hv->get_glb_state());
}

void Aw86937::ReleaseLoadLock() {
    // 幂等：只有真正持有锁的那一次会走下去。
    // Stop() 和"短振动自然结束"可能同时走到这里，不幂等就会重复释放。
    if (is_playing_.exchange(false))
        board_load_release(BOARD_LOAD_VIB);
}
