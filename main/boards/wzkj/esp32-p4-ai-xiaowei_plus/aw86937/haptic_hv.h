/*
 * haptic_hv.h —— AW86937 触觉驱动的唯一头文件。
 *
 * 本文件由三份头合并而成（2026-10-14），原来的分工是：
 *   haptic_hv_port.h      取代官方 "main.h"：ESP-IDF 头、日志宏、平台接口声明
 *   haptic_hv.h           引擎自身的宏 / 枚举 / 结构体 / 函数原型
 *   haptic_hv_waveforms.h 8 个波形的 extern 声明
 * 三者总是一起被 include，拆成三个文件只是徒增文件数。
 *
 * 寄存器地址表仍独立成 haptic_hv_reg.h —— 160 KB 纯宏，没必要塞进来。
 *
 * 所有 .c 文件只 include 本文件 + haptic_hv_reg.h 即可。
 */
#ifndef _HAPTIC_HV_H_
#define _HAPTIC_HV_H_

/* ================= 平台层（原 haptic_hv_port.h） ================= */

#include <stdint.h>
#include <stdio.h>

#include <stdint.h>

#include <esp_err.h>
#include <esp_log.h>
#include <esp_rom_sys.h>
#include <esp_timer.h>
#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "config.h" /* 板级引脚：HAPTIC_RST_PIN / HAPTIC_TRIG1..3_PIN */

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 官方 haptic_hv.c 里唯一的 STM32 宏 ---- */
#define AW_RST_Pin HAPTIC_RST_PIN

/* ---- 日志 ----
 *
 * 官方用 AWINIC_ERR_LOG / AWINIC_INFO_LOG / AWINIC_DEBUG_LOG 三个宏在编译期
 * 开关日志，这里改成无条件映射到 ESP_LOGx：ESP-IDF 自带运行期分级，
 * 想要安静就 esp_log_level_set("haptic_hv", ESP_LOG_WARN)，比编译期开关灵活。
 * （haptic_hv.h 里那三个 AWINIC_*_LOG 宏因此变成纯记录性的，不再影响输出。）
 */
#define HAPTIC_HV_LOG_TAG "haptic_hv"

#define AW_LOGE(format, ...) ESP_LOGE(HAPTIC_HV_LOG_TAG, "%s: " format, __func__, ##__VA_ARGS__)
#define AW_LOGI(format, ...) ESP_LOGI(HAPTIC_HV_LOG_TAG, "%s: " format, __func__, ##__VA_ARGS__)
#define AW_LOGD(format, ...) ESP_LOGD(HAPTIC_HV_LOG_TAG, "%s: " format, __func__, ##__VA_ARGS__)

/*
 * 播放任务的通知值。
 *
 * 长振动到点这件事由 esp_timer 回调通知播放任务，而播放任务同时也接收
 * "开始播放"的通知，两者必须区分开，所以用带值的通知而不是 xTaskNotifyGive。
 */
typedef enum {
	AW_VIB_NOTIFY_PLAY = 1,		/* 开始播放 */
	AW_VIB_NOTIFY_TIMEOUT = 2,	/* 长振动到时，请停止并归还负载锁 */
	AW_VIB_NOTIFY_F0 = 3,		/* 重新探测一次共振频率（只读不写） */
	AW_VIB_NOTIFY_F0_CALI = 4,	/* 重跑一次官方 F0 校准（读 + 写 TRIMCFG5） */
} aw_vib_notify;

/* 长振动时长上限（毫秒）。官方 long_vib_work 用 loop-15 近似无限循环，
 * 万一传进来一个离谱的 duration，马达会一直转到掉电，这里兜个底。 */
#define AW_VIB_DURATION_MAX_MS (10000)

/* ---- 平台接口 ---- */

/* 绑定 I2C 设备句柄（由 Aw86937 在 Init() 里传入 i2c_device_） */
void haptic_hv_port_bind_i2c(i2c_master_dev_handle_t dev);

/* 解绑并停掉定时器。析构时调用，避免定时器回调继续往一条已经不属于
 * 本对象的 I2C 总线上发 play_stop()。 */
void haptic_hv_port_deinit(void);

/* 创建 esp_timer。必须在 haptic_hv_boot_init() 之前调用。 */
esp_err_t haptic_hv_port_init(void);

/* 注册播放任务句柄，长振动到点时定时器回调要通知它 */
void haptic_hv_port_set_play_task(TaskHandle_t task);

/* 长振动定时器当前是否在跑。
 *
 * haptic_hv_start_hrtimer() 是 void，官方 long_vib_work() 无法知道定时器起没起来，
 * 起播成功照样返回 AW_SUCCESS。万一 esp_timer_start_once 失败，就永远等不到
 * TIMEOUT 通知 —— 而 long_vib_work 设的是 loop-15 近似无限循环，
 * 结果是马达一直转到掉电。所以起播后必须回头确认一次。 */
bool haptic_hv_port_timer_running(void);


/* ================= 引擎（原 haptic_hv.h） ================= */

/*
 * 本板只有 AW86937 一颗芯片，其余型号的分支已经全部删掉（2026-10-14），
 * 所以这里不再需要 AW869X_DRIVER / AW869XX_DRIVER / AW8671X_DRIVER /
 * AW8692X_DRIVER / AW8672X_DRIVER / AW8693XS_DRIVER 这六个宏 ——
 * 连它们包着的代码一起没了。AW_DOUBLE（双马达）和 AW_TIKTAP_X 同理。
 *
 * 剩下这几个是仍然有代码在里面、但保持关闭的功能开关，留着以备启用。
 */
#define AW8693X_DRIVER
#define AW_LRA_F0_DEFAULT
#define AW_F0_CALI_DURING_STARTUP
#define AWINIC_ERR_LOG
#define AWINIC_INFO_LOG
/* #define AWINIC_DEBUG_LOG */
/* #define AW_RAM_GET_F0 */
/* #define AW_CHECK_RAM_DATA */
/* #define AW_USE_MAXIMUM_F0_CALI_DATA */
/* #define AW_RTP_AUTO_SIN_ENABLE */
/* #define AW_ADD_BRAKE_WAVE */

/*
 * 波形数据来源只有一个：haptic_hv_waveforms.c（本项目自己那份，见其文件头
 * 注释）。它定义 7 个 GUI 波形 aw86937_wave_* 和 RAM 启动表
 * haptic_hv_ram_data[] / haptic_hv_ram_data_len，由 main/CMakeLists.txt
 * 显式列进 BOARD_SOURCES。
 *
 * 这里原本还列了两个"二选一"的备选表 —— 官方 haptic_hv_params.c 的默认表，
 * 以及由 vib_ramlib_heartbeat.bin 生成的心跳表 haptic_hv_ram_heartbeat.c。
 * 两者**都没有参与编译**，是纯占位源码，2026-10-14 连同那两个 .bin 一起删了。
 * 要翻回来从 lcd 工程取：
 *   ui/lcd/main/boards/WZKJ/esp32-p4-ai-xiaowei_plus/aw86937/
 */

#define AW_SOFT_RESET								(0xaa)
#define AW_REG_RSTCFG								(0x00)
#define AW_I2C_ADDR_A								(0x5a << 1)
#define AW_I2C_ADDR_B								(0x5b << 1)

#define AW_REG_CHIPID								(0x00) /* AW869X AW869XX */
#define AW_REG_CHIPIDH								(0x57) /* AW8692X AW8671X AW8693X AW8672X */
#define AW_REG_CHIPIDL								(0x58) /* AW8692X AW8671X AW8693X AW8672X */
#define AW8695_CHIPID								(0x95)
#define AW8697_CHIPID								(0x97)
#define AW86905_CHIPID								(0x05)
#define AW86907_CHIPID								(0x04)
#define AW86915_CHIPID								(0x07)
#define AW86917_CHIPID								(0x06)
#define AW86715_CHIPID								(0x7150)
#define AW86716_CHIPID								(0x7170)
#define AW86717_CHIPID								(0x7171)
#define AW86715FGR_CHIPID							(0x7175)
#define AW86718LGR_CHIPID							(0x7178)
#define AW86718_CHIPID								(0x7180)
#define AW86925_CHIPID								(0x9250)
#define AW86926_CHIPID								(0x9260)
#define AW86927_CHIPID								(0x9270)
#define AW86928_CHIPID								(0x9280)
#define AW86727_CHIPID								(0x7270)
#define AW86728_CHIPID								(0x7280)
#define AW86936_CHIPID								(0x9360)
#define AW86937_CHIPID								(0x9370)
#define AW86938_CHIPID								(0x9380)
#define AW86937S_CHIPID								(0x9371)
#define AW86938S_CHIPID								(0x9381)

#define AW_I2C_BYTE_ONE								(1)
#define AW_I2C_BYTE_TWO								(2)
#define AW_I2C_BYTE_THREE							(3)
#define AW_I2C_BYTE_FOUR							(4)
#define AW_I2C_BYTE_FIVE							(5)
#define AW_I2C_BYTE_SIX								(6)
#define AW_I2C_BYTE_SEVEN							(7)
#define AW_I2C_BYTE_EIGHT							(8)
#define AW_I2C_RETRIES								(5)
#define AW_READ_CHIPID_RETRIES						(5)
#define AW_RAM_GET_F0_SEQ							(8)
#define AW_OSC_CALI_LRA_ACCURACY					(24)
#define AW_RTP_FRAME_LEN							(512)
#define AW_RAMDATA_RD_BUFFER_SIZE					(1)
/*
 * 官方原值是 1（每次 I2C 只发 1 字节）。STM32 上无所谓，但心跳波形库有
 * 7685 字节，逐字节发就是 7685 次事务、开机要 1 秒以上。ESP-IDF 的
 * i2c_master_transmit 单次能发 256 字节，这里提到 256 -> 约 31 次事务。
 * 旧的手写驱动用的就是这个值（RAM_CHUNK=256），已在同款硬件上验证可用。
 * 注意：haptic_hv_port.c 里的发送缓冲要相应开到 1+256 字节。
 */
#define AW_RAMDATA_WR_BUFFER_SIZE					(256)
#define AW_VBAT_REFER								(4200)
#define AW_VBAT_MIN									(3000)
#define AW_VBAT_MAX									(4500)
#define AW_DRV2_LVL_MAX								(0x7F)

#define AW869X_BST_VOL_MIN							(6000)
#define AW869X_BST_VOL_MAX							(10500)
#define AW869X_BST_VOL_FORMULA(bst_vol)				((((bst_vol) - 6000) / 142) << 1)
#define AW869X_F0_FORMULA(f0_reg, coeff)			(1000000000 / ((f0_reg) * (coeff)))
#define AW869X_LRA_FORMULA(lra_code)				(298 * (lra_code))
#define AW869X_VBAT_FORMULA(vbat_code)				(6100 * (vbat_code) / 256)
#define AW869X_FIFO_AE_ADDR_H(base_addr)			(((base_addr) >> 1) >> 8)
#define AW869X_FIFO_AE_ADDR_L(base_addr)			(((base_addr) >> 1) & 0x00ff)
#define AW869X_FIFO_AF_ADDR_H(base_addr)			(((base_addr) - ((base_addr) >> 2)) >> 8)
#define AW869X_FIFO_AF_ADDR_L(base_addr)			(((base_addr) - ((base_addr) >> 2)) & 0x00ff)

#define AW869XX_BST_VOL_MIN							(6000)
#define AW869XX_BST_VOL_MAX							(10971)
#define AW869XX_DRV2_LVL_FORMULA(f0, vrms)			((((f0) < 1800) ? 1809920 : 1990912) / 1000 * (vrms) / 30500)
#define AW869XX_BST_VOL_FORMULA(bst_vol)			(((bst_vol) - 6000) * 1000 / 78893)
#define AW869XX_F0_FORMULA(f0_reg)					(384000 * 10 / (f0_reg))
#define AW869XX_LRA_FORMULA(lra_code, d2s_gain)		(((lra_code) * 678 * 1000) / (1024 * d2s_gain))
#define AW869XX_VBAT_FORMULA(vbat_code)				(6100 * (vbat_code) / 1024)
#define AW869XX_OS_FORMULA(os_code, d2s_gain)		(2440 * ((os_code) - 512) / (1024 * ((d2s_gain) + 1)))
#define AW869XX_FIFO_AE_ADDR_H(base_addr)			((((base_addr) >> 1) >> 4) & 0xF0)
#define AW869XX_FIFO_AE_ADDR_L(base_addr)			(((base_addr) >> 1) & 0x00ff)
#define AW869XX_FIFO_AF_ADDR_H(base_addr)			((((base_addr) - ((base_addr) >> 2)) >> 8) & 0x0F)
#define AW869XX_FIFO_AF_ADDR_L(base_addr)			(((base_addr) - ((base_addr) >> 2)) & 0x00ff)

#define AW8671X_DRV2_LVL_FORMULA(f0, vrms)			((((f0) < 1800) ? 1809920 : 1990912) / 1000 * (vrms) / 40000)
#define AW8671X_F0_FORMULA(f0_reg)					(384000 * 10 / (f0_reg))
#define AW8671X_VBAT_FORMULA(vbat)					(6050 * (vbat) / 4095)
#define AW8671X_LRA_FORMULA(lra, d2s_gain)			((1210 * 1000 * 10 * (lra)) / (2047 * 36 * (d2s_gain)))
#define AW8671X_FIFO_AE_ADDR_H(base_addr)			((((base_addr) >> 1) >> 4) & 0xF0)
#define AW8671X_FIFO_AE_ADDR_L(base_addr)			(((base_addr) >> 1) & 0x00ff)
#define AW8671X_FIFO_AF_ADDR_H(base_addr)			(((base_addr) - ((base_addr) >> 2)) >> 8 & 0x0F)
#define AW8671X_FIFO_AF_ADDR_L(base_addr)			(((base_addr) - ((base_addr) >> 2)) & 0x00ff)

#define AW8672X_DRV2_LVL_FORMULA(f0, vrms)			((((f0) < 1800) ? 1809920 : 1990912) / 1000 * (vrms) / 40000)
#define AW8672X_F0_FORMULA(f0_reg)					(384000 * 10 / (f0_reg))
#define AW8672X_VBAT_FORMULA(vbat_code)				(6100 * (vbat_code) / 1023)
#define AW8672X_LRA_FORMULA(lra, d2s_gain)			((610000 * (lra)) / (1023 * (d2s_gain)))
#define AW8672X_OS_FORMULA(os_code, d2s_gain)		(2440 * ((os_code) - 512) / (1023 * ((d2s_gain) + 1)))
#define AW8672X_FIFO_AE_ADDR_H(base_addr)			((((base_addr) >> 1) >> 4) & 0xF0)
#define AW8672X_FIFO_AE_ADDR_L(base_addr)			(((base_addr) >> 1) & 0x00ff)
#define AW8672X_FIFO_AF_ADDR_H(base_addr)			(((base_addr) - ((base_addr) >> 2)) >> 8 & 0x0F)
#define AW8672X_FIFO_AF_ADDR_L(base_addr)			(((base_addr) - ((base_addr) >> 2)) & 0x00ff)

#define AW8692X_BST_VOL_MIN							(6000)
#define AW8692X_BST_VOL_MAX							(11437)
#define AW8692X_BOOT_DETECTION						(5)
#define AW8692X_DRV2_LVL_FORMULA(f0, vrms)			((((f0) < 1800) ? 1809920 : 1990912) / 1000 * (vrms) / 40000)
#define AW8692X_BST_VOL_FORMULA(bst_vol)			(((bst_vol) - 3500) * 10 / 625)
#define AW8692X_F0_FORMULA(f0_reg)					(384000 * 10 / (f0_reg))
#define AW8692X_LRA_FORMULA(lra, d2s_gain)			((6075 * 100 * (lra)) / (1024 * (d2s_gain)))
#define AW8692X_VBAT_FORMULA(vbat)					(5 * 1215 * (vbat) / 1024)
#define AW8692X_FIFO_AE_ADDR_H(base_addr)			((((base_addr) >> 1) >> 4) & 0xF0)
#define AW8692X_FIFO_AE_ADDR_L(base_addr)			(((base_addr) >> 1) & 0x00ff)
#define AW8692X_FIFO_AF_ADDR_H(base_addr)			((((base_addr) - ((base_addr) >> 2)) >> 8) & 0x0F)
#define AW8692X_FIFO_AF_ADDR_L(base_addr)			(((base_addr) - ((base_addr) >> 2)) & 0x00ff)

#define AW8693X_BST_VOL_MIN							(6000)
#define AW8693X_BST_VOL_MAX							(11000)
#define AW8693X_DRV2_LVL_FORMULA(f0, vrms)			((((f0) < 1800) ? 1809920 : 1990912) / 1000 * (vrms) / 40000)
#define AW8693X_BST_VOL_FORMULA(bst_vol)			(((bst_vol) - 6000) / 250 + 5)
#define AW8693X_F0_FORMULA(f0_reg)					(384000 * 10 / (f0_reg))
#define AW8693X_VBAT_FORMULA(vbat)					(6100 * (vbat) / 1023)
#define AW8693X_OS_FORMULA(os_code, d2s_gain)		(2440 * ((os_code) - 512) / (1023 * ((d2s_gain) + 1)))
#define AW8693X_LRA_FORMULA(lra, d2s_gain)			((610000 * (lra)) / (1023 * (d2s_gain)))
#define AW8693X_F_PRE_FORMULA(f0_pre)				(240000 / (f0_pre))
#define AW8693X_FIFO_AE_ADDR_H(base_addr)			((((base_addr) >> 1) >> 4) & 0xF0)
#define AW8693X_FIFO_AE_ADDR_L(base_addr)			(((base_addr) >> 1) & 0x00ff)
#define AW8693X_FIFO_AF_ADDR_H(base_addr)			((((base_addr) - ((base_addr) >> 2)) >> 8) & 0x0F)
#define AW8693X_FIFO_AF_ADDR_L(base_addr)			(((base_addr) - ((base_addr) >> 2)) & 0x00ff)

#define AW_DRV_WIDTH_FORMULA(f0_pre, brk_gain, track_margin) (240000 / \
							 (f0_pre) - 8 - (brk_gain) - (track_margin))

#define AW8693XS_OSC_CALI_ACCURACY					(22)
#define AW8693XS_BST_VOL_MIN						(6000)
#define AW8693XS_BST_VOL_MAX						(11000)
#define AW8693XS_PRO_BSTMAX_MIN						(5000)
#define AW8693XS_PRO_BSTMAX_MAX						(12500)
#define AW8693XS_PRO_IPEAK_MIN						(1500)
#define AW8693XS_PRO_IPEAK_MAX						(4750)
#define AW8693XS_VBAT_PRO1_BST_DEFAULT				(6000)
#define AW8693XS_VBAT_PRO1_BST_IPEAK_DEFAULT		(2500)
#define AW8693XS_VBAT_PRO2_BST_DEFAULT				(5000)
#define AW8693XS_VBAT_PRO2_BST_IPEAK_DEFAULT		(1500)
#define AW8693XS_DRV2_LVL_FORMULA(f0, vrms)			((((f0) < 1800) ? 1809920 : 1990912) / 1000 * (vrms) / 40000)
#define AW8693XS_BST_VOL_FORMULA(bst_vol)			(((bst_vol) - 6000) / 250 + 5)
#define AW8693XS_BST_MAX_FORMULA(bst_max)			(((bst_max) - 5000) / 500)
#define AW8693XS_IPEAK_FORMULA(ipeak)				(((ipeak) - 1500) / 250)
#define AW8693XS_F0_FORMULA(f0_reg)					(384000 * 10 / (f0_reg))
#define AW8693XS_VBAT_FORMULA(vbat)					(6100 * (vbat) / 1023)
#define AW8693XS_OS_FORMULA(os_code, d2s_gain)		(2440 * ((os_code) - 512) / (1023 * ((d2s_gain) + 1)))
#define AW8693XS_LRA_FORMULA(lra, d2s_gain)			((2440 * 1000 * (lra)) / (4 * 1023 * (d2s_gain)) * 2000 / 1933)
#define AW8693XS_F_PRE_FORMULA(f0_pre)				(240000 / (f0_pre))
#define AW8693XS_FIFO_AE_ADDR_H(base_addr)			((((base_addr) >> 1) >> 4) & 0xF0)
#define AW8693XS_FIFO_AE_ADDR_L(base_addr)			(((base_addr) >> 1) & 0x00ff)
#define AW8693XS_FIFO_AF_ADDR_H(base_addr)			((((base_addr) - ((base_addr) >> 2)) >> 8) & 0x0F)
#define AW8693XS_FIFO_AF_ADDR_L(base_addr)			(((base_addr) - ((base_addr) >> 2)) & 0x00ff)
#define AW8693XS_CALI_DATA_FORMULA(f0, f0_pre, s) \
													(10 * ((int)f0_pre * (10000 + s * AW8693XS_OSC_CALI_ACCURACY) - \
														10000 * (int)f0) / ((int)f0 * AW8693XS_OSC_CALI_ACCURACY))

/*
 * AW_LOGE / AW_LOGI / AW_LOGD 定义在本文件开头的"平台层"一节，转接到 ESP_LOGx。
 * 上表的 AWINIC_ERR_LOG / AWINIC_INFO_LOG / AWINIC_DEBUG_LOG 只是记录性宏
 * （从原 haptic_hv_port.h 沿袭），已经不影响输出 —— 想要安静用运行期分级：
 * esp_log_level_set("haptic_hv", ESP_LOG_WARN)。
 */

typedef enum
{
	AW_SUCCESS = 0,
	AW_ERROR = 1,
} aw_err;

typedef enum
{
	AW_FALSE = 0,
	AW_TRUE = 1,
} aw_bool;

enum aw_trim_config {
	AW_TRIM_EFUSE = 0x00,
	AW_TRIM_REGISTER = 0x20,
};
typedef enum
{
	AW_TRIM_LRA_BOUNDARY = 0x20,
	AW8672X_TRIM_LRA_BOUNDARY = 0x40,
	AW8693X_TRIM_LRA_BOUNDARY = 0x40,
	AW8693XS_TRIM_LRA_BOUNDARY = 0x80,
} aw_trim_lra;

typedef enum
{
	AW_REG_VALUE_MIN = 0,
	AW_REG_VALUE_MAX = 0xFF,
} aw_reg_value;

typedef enum
{
	AW_STANDBY_MODE = 0,
	AW_RAM_MODE =  1,
	AW_RTP_MODE =  2,
	AW_TRIG_MODE = 3,
	AW_CONT_MODE = 4,
	AW_RAM_LOOP_MODE = 5,
} aw_mode;

typedef enum
{
	AW_RAM_VBAT_COMP_DISABLE = 0,
	AW_RAM_VBAT_COMP_ENABLE = 1,
} aw_vbat_comp;

typedef enum
{
	AW_MS_UINT = 1,
	AW_TEN_MS_UINT = 10,
	AW_S_UINT  = 1000,
} aw_time;

typedef enum
{
	AW_PWM_12K = 0,
	AW_PWM_24K = 1,
	AW_PWM_48K = 2,
	AW_PWM_8K = 3,
} aw_pwm;

typedef enum
{
	AW_BST_PC_L1 = 0,
	AW_BST_PC_L2 = 1,
} aw_bst_pc;

typedef enum
{
	AW_VBAT_PRO1 = 0,
	AW_VBAT_PRO2 = 1,
} aw_pro_pc;

typedef enum
{
	AW_BST_BYPASS_MODE = 0,
	AW_BST_BOOST_MODE = 1,
} aw_bst_mode;

typedef enum
{
	AW_SW_VBAT_COMP_MODE = 0,
	AW_HW_VBAT_COMP_MODE = 1,
} aw_vbat_mode;

typedef enum
{
	AW_VBAT_REF_3P3V = 3300,
	AW_VBAT_REF_3P6V = 3600,
	AW_VBAT_REF_4P0V = 4000,
	AW_VBAT_REF_4P2V = 4200,
	AW_VBAT_REF_4P5V = 4500,
	AW_VBAT_REF_4P8V = 4800,
	AW_VBAT_REF_5P0V = 5000,
	AW_VBAT_REF_5P5V = 5500,
} aw_vbat_ref;

typedef enum
{
	AW_WRITE_ZERO = 0,
	AW_F0_CALI_LRA = 1,
	AW_OSC_CALI_LRA = 2,
} aw_cali_lra;

typedef enum
{
	AW_GAIN_LEVEL_1 = 128,	/* max 0x80 */
	AW_GAIN_LEVEL_2 = 112,
	AW_GAIN_LEVEL_3 = 96,
	AW_GAIN_LEVEL_4 = 80,
	AW_GAIN_LEVEL_5 = 64,
	AW_GAIN_LEVEL_6 = 48,
	AW_GAIN_LEVEL_7 = 32,
	AW_GAIN_LEVEL_8=  16,
	AW_GAIN_LEVEL_NUM_MAX = 8,
} aw_gain;

typedef enum
{
	AW_LOOP1 = 1,
	AW_LOOP2 = 2,
	AW_LOOP3 = 3,
	AW_LOOP4 = 4,
	AW_LOOP5 = 5,
	AW_LOOP6 = 6,
	AW_LOOP7 = 7,
	AW_LOOP8 = 8,
	AW_LOOP9 = 9,
	AW_LOOP10 = 10,
	AW_LOOP11 = 11,
	AW_LOOP12 = 12,
	AW_LOOP13 = 13,
	AW_LOOP14 = 14,
	AW_LOOP15 = 15,
	AW_LOOP_NUM_MAX = AW_LOOP15,
} aw_loop;

typedef enum
{
	AW_INDEX1 = 1,
	AW_INDEX2 = 2,
	AW_INDEX3 = 3,
	AW_INDEX4 = 4,
	AW_INDEX5 = 5,
	AW_INDEX6 = 6,
	AW_INDEX7 = 7,
	AW_INDEX8 = 8,
	AW_INDEX9 = 9,
	AW_INDEX10 = 10,
	AW_INDEX11 = 11,
	AW_INDEX12 = 12,
	AW_INDEX13 = 13,
	AW_INDEX14 = 14,
	AW_INDEX15 = 15,
	AW_INDEX16 = 16,
	AW_INDEX_MAX = AW_INDEX16,
} aw_index;

typedef enum
{
	AW_IRQ_ALMOST_EMPTY = 1,
	AW_IRQ_ALMOST_FULL = 2,
	AW_IRQ_BST_SCP = 3,
	AW_IRQ_BST_OVP = 4,
	AW_IRQ_UVLO = 5,
	AW_IRQ_OCD = 6,
	AW_IRQ_OT = 7,
	AW_IRQ_LOW_VBAT = 8,
	AW_IRQ_OV = 9,
	AW_IRQ_DONE = 10,
	AW_IRQ_CP_OVP = 11,
} aw_haptic_irq_status;

typedef enum
{
	AW8671X_CHARGEPUMP_CODE0  = 4950,
	AW8671X_CHARGEPUMP_CODE1  = 5300,
	AW8671X_CHARGEPUMP_CODE2  = 5600,
	AW8671X_CHARGEPUMP_CODE3  = 5800,
	AW8671X_CHARGEPUMP_CODE4  = 6300,
} aw8671x_haptic_chargepump;

typedef enum
{
	AW8672X_CHARGEPUMP_6V     = 6000,
	AW8672X_CHARGEPUMP_6P25V  = 6250,
	AW8672X_CHARGEPUMP_6P5V   = 6500,
	AW8672X_CHARGEPUMP_6P75V  = 6750,
	AW8672X_CHARGEPUMP_7V     = 7000,
	AW8672X_CHARGEPUMP_7P25V  = 7250,
	AW8672X_CHARGEPUMP_7P5V   = 7500,
	AW8672X_CHARGEPUMP_7P75V  = 7750,
	AW8672X_CHARGEPUMP_8V     = 8000,
	AW8672X_CHARGEPUMP_8P25V  = 8250,
	AW8672X_CHARGEPUMP_8P5V   = 8500,
	AW8672X_CHARGEPUMP_8P75V  = 8750,
	AW8672X_CHARGEPUMP_9V     = 9000,
	AW8672X_CHARGEPUMP_9P25V  = 9250,
	AW8672X_CHARGEPUMP_9P5V   = 9500,
} aw8672x_haptic_chargepump;

/*********************************************************
 *
 * Enum aw8693xs
 *
 *********************************************************/
enum d2s_gain_sel {
	AW8693XS_D2S_GAIN,
	AW8693XS_BEMF_D2S_GAIN,
};

enum aw8693xs_vbat_pro3_gain {
	AW8693XS_PRO3_GAIN_1, // 1
	AW8693XS_PRO3_GAIN_3_4,// 3/4
	AW8693XS_PRO3_GAIN_1_2,// 1/2
	AW8693XS_PRO3_GAIN_0,// 0
};

/*********************************************************
 *
 * Struct Define
 *
 *********************************************************/
/* 原有的 struct aw_haptic_trig 连同 aw8693x_trig1/2/3 三张参数表一起删了
 * （2026-10-14）：它们唯一的用户是 trig*_param_config()，而那三个函数从来
 * 没有被函数表调用过。TRIG1~3 引脚目前没接（见 config.h）。 */

struct aw_haptic_dts {
	/* common */
	uint8_t f0_cali_percent;
	uint16_t f0_pre;
	uint16_t vbat_pro_params[5];/* 0:pro1 bstmax 1: pro1 boost ipeak 2:pro2 bstmax 3: pro2 boost ipeak 4: pro3 gain level*/

	/* aw869x aw8693xs */
	uint8_t f0_pre_num;
	uint8_t f0_wait_num;
	uint8_t f0_repeat_num;
	uint8_t f0_trace_num;
	uint8_t bemf_config[4];
	uint8_t bemf_d2s_gain;
	uint8_t uvlo_vol_default;
	uint8_t bstdbg[3];
	uint8_t cont_drv_lvl;
	uint8_t cont_drv_lvl_ov;
	uint8_t cont_num_break;
	uint8_t tset;
	uint8_t r_spare;
	uint16_t cont_td;
	uint16_t cont_zc_thr;
	uint16_t f0_coeff;

	/* aw869xx aw8692x aw8671x aw8693x aw8693xs aw8672X */
	uint8_t cont_drv1_lvl;
	uint8_t cont_drv2_lvl;
	uint8_t cont_brk_time;
	uint8_t cont_brk_gain;
	uint8_t cont_drv1_time;
	uint8_t cont_drv2_time;
	uint8_t cont_smart_loop;
	uint8_t cont_track_margin;
	uint8_t d2s_gain;
	uint8_t gain_bypass;
	uint8_t bstcfg[5];
	uint8_t prctmode[3];
	uint32_t bst_vol;
	uint32_t cont_lra_vrms;
	aw_bool enabled_one_wire;
	aw_bool is_enabled_inter_brake;
	aw_bool is_enabled_low_power;
	aw_bool	is_enabled_vbat_pro;
};

struct haptic_hv {
	uint8_t f0_cali_data;
	uint8_t osc_cali_data;
	uint8_t gain;
	uint8_t ram_num;
	uint8_t i2c_addr;
	uint8_t max_pos_beme;
	uint8_t max_neg_beme;
	uint8_t trim_lra_boundary;
	uint16_t f0;
	uint16_t rst_pin;
	uint16_t base_addr;
	uint32_t chipid;
	uint32_t timer_ms_cnt;
	uint32_t duration;
	uint32_t vbat;
	uint32_t lra;
	int osc_trim_s;

	aw_mode play_mode;
	aw_vbat_comp vbat_comp;
	aw_bool auto_bst;
	aw_bst_pc bst_pc;
};

struct aw_haptic_func
{
	int  (*get_f0)(void);
	int  (*offset_cali)(void);
	int  (*check_qualify)(void);
	void (*play_go)(aw_bool);
	void (*set_pwm)(aw_pwm);
	void (*get_vbat)(void);
	void (*set_gain)(uint8_t);
	void (*play_stop)(void);
	void (*play_mode)(aw_mode);
	void (*upload_lra)(aw_cali_lra);
	void (*set_bst_vol)(uint32_t);
	void (*set_wav_seq)(uint8_t, uint8_t);
	void (*set_wav_loop)(uint8_t, uint8_t);
	void (*set_main_loop)(uint8_t);
	/* 只读：波形库放在 flash 里，见下方 haptic_hv_ram_data 的说明 */
	void (*set_ram_data)(const uint8_t *, uint32_t);
	void (*get_ram_data)(uint8_t *, uint32_t);
	void (*set_ram_addr)(void);
	void (*set_base_addr)(void);
	void (*raminit_enable)(aw_bool);
	void (*misc_para_init)(void);
	void (*protect_config)(uint8_t, uint8_t);
	void (*interrupt_setup)(void);
	void (*auto_bst_enable)(aw_bool);
	void (*auto_brake_mode)(aw_bool);
	void (*bst_mode_config)(aw_bst_mode);
	void (*vbat_mode_config)(aw_vbat_mode);
	uint8_t (*get_glb_state)(void);

	int (*f0_cali)(void);
	int (*long_vib_work)(aw_index, aw_gain, uint32_t);
	int (*short_vib_work)(aw_index, aw_gain, aw_loop);
};

extern int haptic_hv_i2c_reads(uint8_t, uint8_t *, uint16_t);
extern int haptic_hv_i2c_writes(uint8_t, const uint8_t *, uint16_t);
extern void haptic_hv_i2c_write_bits(uint8_t, uint32_t, uint8_t);
extern void haptic_hv_mdelay(uint32_t);
extern void haptic_hv_udelay(uint32_t);
extern void haptic_hv_stop_hrtimer(void);
extern void haptic_hv_start_hrtimer(void);
extern void haptic_hv_disable_irq(void);
extern void haptic_hv_enable_irq(void);
extern void haptic_hv_set_reset_gpio(uint16_t, uint8_t);
extern void haptic_hv_set_cali_to_flash(void);
extern void haptic_hv_get_cali_from_flash(void);

/*
 * waveform data and its len
 *
 * 加 const：库有 7685 字节，不加 const 要占 7685 字节内部 RAM；
 * 加了之后编译器把它放进 flash（.rodata），驱动只读它。
 */
extern const uint8_t haptic_hv_ram_data[];
extern uint32_t haptic_hv_ram_data_len;

/* 7 个 GUI 波形（定义在 haptic_hv_waveforms.c） */
extern const uint8_t aw86937_wave_alarm[];
extern const uint32_t aw86937_wave_alarm_len;
extern const uint8_t aw86937_wave_error_part1[];
extern const uint32_t aw86937_wave_error_part1_len;
extern const uint8_t aw86937_wave_error_part2[];
extern const uint32_t aw86937_wave_error_part2_len;
extern const uint8_t aw86937_wave_normal[];
extern const uint32_t aw86937_wave_normal_len;
extern const uint8_t aw86937_wave_soft[];
extern const uint32_t aw86937_wave_soft_len;
extern const uint8_t aw86937_wave_strong[];
extern const uint32_t aw86937_wave_strong_len;
extern const uint8_t aw86937_wave_success[];
extern const uint32_t aw86937_wave_success_len;
/* Load one complete AWINIC RAM table (header + payload) at runtime. */
extern int haptic_hv_load_ram_table(const uint8_t *data, uint32_t len);

extern struct aw_haptic_dts aw8693x_dts;
extern struct aw_haptic_func aw8693x_func_list;

extern struct haptic_hv *g_haptic_hv;

/*****************************************************
 * haptic_hv_boot_init
 * It is initialization function,
 * please call it to complete initialization first,
 * and then pass g_func_haptic_hv pointer calling function.
 *****************************************************/
extern void haptic_hv_sw_reset(void);
extern int haptic_hv_boot_init(void);
extern struct aw_haptic_func *g_func_haptic_hv;

#ifdef __cplusplus
}
#endif

#endif
