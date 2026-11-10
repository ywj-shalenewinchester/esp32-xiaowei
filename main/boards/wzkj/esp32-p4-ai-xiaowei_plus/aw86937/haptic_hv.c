/*
 * File: haptic_hv.c
 *
 * Author: Ethan <renzhiqiang@awinic.com>
 *
 * Copyright (c) 2026 AWINIC Technology CO., LTD
 *
 * 本文件由两份源文件合并而成（2026-10-14）：
 *   haptic_hv.c  引擎核心：上电序列、F0 校准、RAM 库下载、播放状态机
 *   aw8693x.c    AW8693X 芯片层：寄存器读写助手 + aw8693x_func_list 函数表
 * 本板只有这一颗芯片，两层永远一起编译，拆成两个翻译单元没有意义。
 */

#include "haptic_hv_reg.h"
#include "haptic_hv.h"


#define HAPTIC_HV_DRIVER_VERSION			"v1.8.0"

struct aw_haptic_dts *g_dts = NULL;
struct aw_haptic_func *g_func_haptic_hv = NULL;

struct haptic_hv haptic_hv = {
	.i2c_addr = AW_I2C_ADDR_A,
	.rst_pin = AW_RST_Pin,
	.f0 = 0,
	.chipid = 0,
	.ram_num = 0,
	.gain = 0x80,
	.duration = 0,
	.timer_ms_cnt = 0,
	/* 预置成"零偏移"的 TRIM 编码，而不是 0。
	 *
	 * haptic_hv_f0_cali() 在 get_f0() 失败时不会调用 aw_get_cali_data()，
	 * f0_cali_data 保持原值；但它结尾是无条件 upload_lra(AW_F0_CALI_LRA)。
	 * 于是初值 0 会被当成"最大负向偏移"（0 = boundary - 0x40）写进 TRIMCFG5，
	 * 把 LRA 共振点拽偏，而日志上只会看到一句 "get f0 error"，很难查。
	 * AW8693X_TRIM_LRA_BOUNDARY(0x40) 才是零偏移，等同于官方自己
	 * 在校准越界时回退的 f0_cali_data = g_haptic_hv->trim_lra_boundary。 */
	.f0_cali_data = AW8693X_TRIM_LRA_BOUNDARY,
	.osc_cali_data = 0,
	.auto_bst = AW_FALSE,
	.play_mode = AW_RAM_MODE,
	.vbat_comp = AW_RAM_VBAT_COMP_ENABLE,
	.bst_pc = AW_BST_PC_L1,
};
struct haptic_hv *g_haptic_hv = &haptic_hv;


static int haptic_hv_get_base_addr(void)
{
	uint16_t last_end = 0;
	uint16_t next_start = 0;
	int i = 0;
	int ram_num = 1;

	/* i+3 才是每轮真正读到的最高下标。
	 * 官方原版只判 i < len，当 len % 4 == 1 时（心跳库 7685、官方默认表 1061
	 * 都是这个余数）最后一轮会读到数组尾后 2 字节。这里改判 i+3 < len。 */
	for (i = 3; i + 3 < haptic_hv_ram_data_len; i = i + 4) {
		last_end = haptic_hv_ram_data[i] << 8 | haptic_hv_ram_data[i + 1];
		next_start = haptic_hv_ram_data[i + 2] << 8 | haptic_hv_ram_data[i + 3];
		if (next_start - last_end == 1)
			ram_num++;
		else
			break;
	}

	for (i = ram_num * 4; i >= 4; i = i - 4) {
		last_end = haptic_hv_ram_data[i - 1] << 8 | haptic_hv_ram_data[i];
		g_haptic_hv->base_addr = (int)(haptic_hv_ram_data[1] << 8 | haptic_hv_ram_data[2]) - ram_num * 4 - 1;
		if (last_end - g_haptic_hv->base_addr + 1 == haptic_hv_ram_data_len) {
			AW_LOGI("base_addr = 0x%04x", g_haptic_hv->base_addr);
			return AW_SUCCESS;
		} else {
			ram_num--;
		}
	}
	return AW_ERROR;
}

static int haptic_hv_ram_init(void)
{
	int i = 0;
	int len = 0;
#ifdef AW_CHECK_RAM_DATA
	int j = 0;
	uint8_t ram_data[AW_RAMDATA_RD_BUFFER_SIZE] = {0};
#endif

	if (haptic_hv_get_base_addr() != AW_SUCCESS) {
		AW_LOGE("base addr error, please check your ram data");
		return AW_ERROR;
	}
	/* RAMINIT Enable */
	g_func_haptic_hv->raminit_enable(AW_TRUE);
	g_func_haptic_hv->set_base_addr();
	g_func_haptic_hv->set_ram_addr();

	AW_LOGI("ram data len is %d", haptic_hv_ram_data_len);
	while (i < haptic_hv_ram_data_len) {
		if ((haptic_hv_ram_data_len - i) < AW_RAMDATA_WR_BUFFER_SIZE)
			len = haptic_hv_ram_data_len - i;
		else
			len = AW_RAMDATA_WR_BUFFER_SIZE;
		g_func_haptic_hv->set_ram_data(&haptic_hv_ram_data[i], len);
		i += len;
	}

#ifdef AW_CHECK_RAM_DATA
	i = 0;
	g_func_haptic_hv->set_ram_addr();
	while (i < haptic_hv_ram_data_len) {
		if ((haptic_hv_ram_data_len - i) < AW_RAMDATA_RD_BUFFER_SIZE)
			len = haptic_hv_ram_data_len - i;
		else
			len = AW_RAMDATA_RD_BUFFER_SIZE;
		g_func_haptic_hv->get_ram_data(ram_data, len);
		for (j = 0; j < len; j++)
			if (ram_data[j] != haptic_hv_ram_data[i + j]) {
				AW_LOGE("check ramdata error, addr=0x%04x, ram_data=0x%02x, file_data=0x%02x",
						(i + j), ram_data[j], haptic_hv_ram_data[i + j]);
				return AW_ERROR;
			}
		i += len;
	}
	AW_LOGI("check ram data pass");
#endif

	/* RAMINIT Disable */
	g_func_haptic_hv->raminit_enable(AW_FALSE);

	return AW_SUCCESS;
}

int haptic_hv_load_ram_table(const uint8_t *data, uint32_t len)
{
	uint32_t first_wave_addr;
	uint32_t i = 0;
	int chunk;

	if (data == NULL || len < 5 || data[0] != 0x01)
		return AW_ERROR;
	first_wave_addr = ((uint32_t)data[1] << 8) | data[2];
	if (first_wave_addr < 5)
		return AW_ERROR;

	/* Every GUI table is a one-wave table with its own 0x0800 base. */
	g_haptic_hv->base_addr = (int)first_wave_addr - 5;
	g_haptic_hv->ram_num = 1;
	g_func_haptic_hv->play_stop();
	g_func_haptic_hv->raminit_enable(AW_TRUE);
	g_func_haptic_hv->set_base_addr();
	g_func_haptic_hv->set_ram_addr();
	while (i < len) {
		chunk = (len - i < AW_RAMDATA_WR_BUFFER_SIZE) ? (int)(len - i) : AW_RAMDATA_WR_BUFFER_SIZE;
		g_func_haptic_hv->set_ram_data(&data[i], chunk);
		i += chunk;
	}
	g_func_haptic_hv->raminit_enable(AW_FALSE);
	return AW_SUCCESS;
}

static void haptic_hv_get_ram_num(void)
{
	uint32_t first_wave_addr = 0;

	first_wave_addr = (haptic_hv_ram_data[1] << 8) | haptic_hv_ram_data[2];
	g_haptic_hv->ram_num = (first_wave_addr - g_haptic_hv->base_addr - 1) / 4;
	AW_LOGI("ram num = %d", g_haptic_hv->ram_num);
}

static void aw_get_cali_data(int f0_cali_step)
{
	char f0_cali_lra = 0;

	AW_LOGI("f0_cali_step = %d", f0_cali_step);
	if (f0_cali_step >= 0) {	/*f0_cali_step >= 0 */
		if (f0_cali_step % 10 >= 5)
			f0_cali_step = g_haptic_hv->trim_lra_boundary + (f0_cali_step / 10 + 1);
		else
			f0_cali_step = g_haptic_hv->trim_lra_boundary + f0_cali_step / 10;
	} else {	/* f0_cali_step < 0 */
		if (f0_cali_step % 10 <= -5)
			f0_cali_step = g_haptic_hv->trim_lra_boundary + (f0_cali_step / 10 - 1);
		else
			f0_cali_step = g_haptic_hv->trim_lra_boundary + f0_cali_step / 10;
	}
	if (f0_cali_step >= g_haptic_hv->trim_lra_boundary)
		f0_cali_lra = (char)f0_cali_step - g_haptic_hv->trim_lra_boundary;
	else
		f0_cali_lra = (char)f0_cali_step + g_haptic_hv->trim_lra_boundary;
	/* update cali step */
	g_haptic_hv->f0_cali_data = (int)f0_cali_lra;
	AW_LOGI("f0_cali_data = 0x%02X", g_haptic_hv->f0_cali_data);
}

static int haptic_hv_f0_cali(void)
{
	uint32_t f0_limit = 0;
	uint32_t f0_cali_min = g_dts->f0_pre * (100 - g_dts->f0_cali_percent) / 100;
	uint32_t f0_cali_max = g_dts->f0_pre * (100 + g_dts->f0_cali_percent) / 100;
	int f0_cali_step = 0;

	g_func_haptic_hv->upload_lra(AW_WRITE_ZERO);
	if (g_func_haptic_hv->get_f0() == AW_ERROR) {
		AW_LOGE("get f0 error, user defafult f0");
	} else {
		/* max and min limit */
		f0_limit = g_haptic_hv->f0;
		AW_LOGI("f0_pre = %d, f0_cali_min = %d, f0_cali_max = %d, f0 = %d",
				g_dts->f0_pre, f0_cali_min, f0_cali_max, g_haptic_hv->f0);

		if (g_haptic_hv->f0 < f0_cali_min) {
			AW_LOGE("f0 is too small, f0 = %d!", g_haptic_hv->f0);
#ifdef AW_USE_MAXIMUM_F0_CALI_DATA
			if (g_haptic_hv->chipid == AW86937S_CHIPID || g_haptic_hv->chipid == AW86938S_CHIPID) {
				f0_cali_step = AW8693XS_CALI_DATA_FORMULA(f0_cali_min, g_dts->f0_pre, g_haptic_hv->osc_trim_s);
				aw_get_cali_data(f0_cali_step);
			} else {
				g_haptic_hv->f0_cali_data = g_haptic_hv->trim_lra_boundary;
			}
			g_func_haptic_hv->upload_lra(AW_F0_CALI_LRA);
#endif
			return AW_ERROR;
		}

		if (g_haptic_hv->f0 > f0_cali_max) {
			AW_LOGE("f0 is too large, f0 = %d!", g_haptic_hv->f0);
#ifdef AW_USE_MAXIMUM_F0_CALI_DATA
			if (g_haptic_hv->chipid == AW86937S_CHIPID || g_haptic_hv->chipid == AW86938S_CHIPID) {
				f0_cali_step = AW8693XS_CALI_DATA_FORMULA(f0_cali_max, g_dts->f0_pre, g_haptic_hv->osc_trim_s);
				aw_get_cali_data(f0_cali_step);
			} else {
				g_haptic_hv->f0_cali_data = g_haptic_hv->trim_lra_boundary - 1;
			}
			g_func_haptic_hv->upload_lra(AW_F0_CALI_LRA);
#endif
			return AW_ERROR;
		}

		/* calculate cali step */
		if (g_haptic_hv->chipid == AW86937S_CHIPID || g_haptic_hv->chipid == AW86938S_CHIPID) {
			f0_cali_step = AW8693XS_CALI_DATA_FORMULA(f0_limit, g_dts->f0_pre, g_haptic_hv->osc_trim_s);
		} else {
			f0_cali_step = 100000 * ((int)f0_limit - (int)g_dts->f0_pre) /
						((int)g_dts->f0_pre * AW_OSC_CALI_LRA_ACCURACY);
		}
		aw_get_cali_data(f0_cali_step);
	}
	g_func_haptic_hv->upload_lra(AW_F0_CALI_LRA);
	/* restore standby work mode */
	g_func_haptic_hv->play_stop();
#ifndef AW_F0_CALI_DURING_STARTUP
	haptic_hv_set_cali_to_flash();
#endif
	return AW_SUCCESS;
}

static void haptic_hv_ram_vbat_comp(aw_bool flag)
{
	int temp_gain = 0;

	if (flag) {
		if (g_haptic_hv->vbat_comp == AW_RAM_VBAT_COMP_ENABLE) {
			AW_LOGI("open sw vbat comp");
			g_func_haptic_hv->get_vbat();
			temp_gain = g_haptic_hv->gain * AW_VBAT_REFER / g_haptic_hv->vbat;
			if (temp_gain > (128 * AW_VBAT_REFER/AW_VBAT_MIN)) {
				temp_gain = 128 * AW_VBAT_REFER/AW_VBAT_MIN;
				AW_LOGI("gain limit=%d", temp_gain);
			}
		g_func_haptic_hv->set_gain(temp_gain);
		} else {
			AW_LOGI("close sw vbat comp");
			g_func_haptic_hv->set_gain(g_haptic_hv->gain);
		}
	} else {
		g_func_haptic_hv->set_gain(g_haptic_hv->gain);
		AW_LOGI("close sw vbat comp");
	}
}

static void haptic_hv_vib_work_cancel(void)
{
	/* canel befefore timer handler */
	if (g_haptic_hv->timer_ms_cnt != 0) {
		haptic_hv_stop_hrtimer();
		g_haptic_hv->timer_ms_cnt = 0;
	}
	g_func_haptic_hv->play_stop();
}

static int haptic_hv_short_vib_work(aw_index index, aw_gain gain, aw_loop loop)
{
	if ((loop >= AW_LOOP_NUM_MAX) || (index == 0) || (index > g_haptic_hv->ram_num)) {
		AW_LOGE("loop = %d, index = %d, err", loop, index);
		return AW_ERROR;
	}
	AW_LOGI("start loop = %d, index = %d", loop, index);
	haptic_hv_vib_work_cancel();
	g_func_haptic_hv->play_mode(AW_RAM_MODE);
	g_func_haptic_hv->auto_brake_mode(AW_FALSE);
	g_func_haptic_hv->upload_lra(AW_F0_CALI_LRA);
	g_func_haptic_hv->set_wav_seq(0x00, index);
	g_func_haptic_hv->set_wav_seq(0x01, 0x00);
	g_func_haptic_hv->set_wav_loop(0x00, loop - 1);
	g_func_haptic_hv->set_main_loop(0x00);
	g_func_haptic_hv->set_gain(gain);
	g_func_haptic_hv->play_go(AW_TRUE);
	return AW_SUCCESS;
};

static int haptic_hv_long_vib_work(aw_index index, aw_gain gain, uint32_t duration)
{
	if ((duration == 0) || (index == 0) || (index > g_haptic_hv->ram_num)) {
		AW_LOGE("duration = %d, index = %d, err", duration, index);
		return AW_ERROR;
	}
	AW_LOGI("start duration = %d, index = %d", duration, index);
	haptic_hv_vib_work_cancel();
	g_haptic_hv->gain = gain;
	g_haptic_hv->duration = duration;
	g_func_haptic_hv->upload_lra(AW_F0_CALI_LRA);
	haptic_hv_ram_vbat_comp(AW_TRUE);
	g_func_haptic_hv->set_wav_seq(0x00, index);
	g_func_haptic_hv->set_wav_seq(0x01, 0x00);
	g_func_haptic_hv->set_wav_loop(0x00, 0x0f);
	g_func_haptic_hv->set_main_loop(0x00);
	g_func_haptic_hv->play_mode(AW_RAM_LOOP_MODE);
	g_func_haptic_hv->auto_brake_mode(AW_TRUE);
	g_func_haptic_hv->play_go(AW_TRUE);
	haptic_hv_start_hrtimer();
	return AW_SUCCESS;
};


void haptic_hv_sw_reset(void)
{
	uint8_t reset = AW_SOFT_RESET;

	AW_LOGI("enter!");
	haptic_hv_i2c_writes(AW_REG_RSTCFG, &reset, AW_I2C_BYTE_ONE);
}

static void haptic_hv_hw_reset(void)
{
	AW_LOGI("enter!");
	haptic_hv_set_reset_gpio(g_haptic_hv->rst_pin, 0);
	haptic_hv_mdelay(2);
	haptic_hv_set_reset_gpio(g_haptic_hv->rst_pin, 1);
	haptic_hv_mdelay(8);
}

static int haptic_hv_judge_value(uint8_t reg_val)
{
	if (!reg_val)
		return AW_ERROR;
	switch (reg_val) {
	case AW86925_BIT_RSTCFG_PRE_VAL:
	case AW86926_BIT_RSTCFG_PRE_VAL:
	case AW86927_BIT_RSTCFG_PRE_VAL:
	case AW86928_BIT_RSTCFG_PRE_VAL:
	case AW86925_BIT_RSTCFG_VAL:
	case AW86926_BIT_RSTCFG_VAL:
	case AW86927_BIT_RSTCFG_VAL:
	case AW86928_BIT_RSTCFG_VAL:
		return AW_ERROR;
	default:
		break;
	}
	return AW_SUCCESS;
}

static int haptic_hv_read_chipid(uint32_t *reg_val)
{
	uint8_t value[2] = {0};
	int ret = AW_SUCCESS;

	/* try the old way of read chip id */
	ret = haptic_hv_i2c_reads(AW_REG_CHIPID, &value[0], AW_I2C_BYTE_ONE);
	if (ret != AW_SUCCESS)
		return ret;

	ret = haptic_hv_judge_value(value[0]);
	if (!ret) {
		*reg_val = value[0];
		return ret;
	}
	/* try the new way of read chip id */
	ret = haptic_hv_i2c_reads(AW_REG_CHIPIDH, value, AW_I2C_BYTE_TWO);
	if (ret != AW_SUCCESS)
		return ret;
	*reg_val = value[0] << 8 | value[1];
	return ret;
}

static int haptic_hv_ctrl_init(void)
{
	uint32_t reg = 0;
	uint8_t cnt = 0;

	AW_LOGI("enter");
	for (cnt = 0; cnt < AW_READ_CHIPID_RETRIES; cnt++) {
		if (haptic_hv_read_chipid(&reg) != AW_SUCCESS)
			AW_LOGE("read chip id fail");
		switch (reg) {
		case AW86936_CHIPID:
		case AW86937_CHIPID:
		case AW86938_CHIPID:
			g_dts = &aw8693x_dts;
			g_func_haptic_hv = &aw8693x_func_list;
			g_haptic_hv->vbat_comp = AW_RAM_VBAT_COMP_DISABLE;
			g_haptic_hv->trim_lra_boundary = AW8693X_TRIM_LRA_BOUNDARY;
			return AW_SUCCESS;
		default:
			AW_LOGI("unexpected chipid!");
			break;
		}
		haptic_hv_mdelay(2);
	}
	return AW_ERROR;
}

static void haptic_hv_api_init(void)
{
	g_func_haptic_hv->f0_cali = haptic_hv_f0_cali;
	g_func_haptic_hv->long_vib_work = haptic_hv_long_vib_work;
	g_func_haptic_hv->short_vib_work = haptic_hv_short_vib_work;
}

static int haptic_hv_parse_chipid(void)
{
	uint8_t cnt = 0;
	uint32_t reg = 0;
	int ret = AW_SUCCESS;

	for (cnt = 0; cnt < AW_READ_CHIPID_RETRIES; cnt++) {
		ret = haptic_hv_read_chipid(&reg);
		if (ret != AW_SUCCESS)
			AW_LOGE("failed to read AW_REG_ID: %d", ret);
		switch (reg) {
		case AW86936_CHIPID:
			g_haptic_hv->chipid = AW86936_CHIPID;
			AW_LOGI("reg_val = 0x%02X, detected aw86936.", reg);
			return ret;
		case AW86937_CHIPID:
			g_haptic_hv->chipid = AW86937_CHIPID;
			AW_LOGI("reg_val = 0x%02X, detected aw86937.", reg);
			return ret;
		case AW86938_CHIPID:
			g_haptic_hv->chipid = AW86938_CHIPID;
			AW_LOGI("reg_val = 0x%02X, detected aw86938.", reg);
			return ret;
		default:
			AW_LOGI("unsupport device revision (0x%02X)", reg);
			break;
		}
		haptic_hv_mdelay(2);
	}
	return AW_ERROR;
}

int haptic_hv_boot_init(void)
{
	/* uint8_t reg_val = 0; */
	int ret = AW_SUCCESS;

	AW_LOGI("haptic_hv driver version %s", HAPTIC_HV_DRIVER_VERSION);
	haptic_hv_hw_reset();
	ret = haptic_hv_ctrl_init();
	if (ret != AW_SUCCESS) {
		AW_LOGE("please check hardward ad_pin!");
		return ret;
	}
	ret = g_func_haptic_hv->check_qualify();
	if (ret != AW_SUCCESS) {
		AW_LOGE("check qualify failed!");
		return ret;
	}
	ret = haptic_hv_parse_chipid();
	if (ret != AW_SUCCESS) {
		AW_LOGE("please check hardward ad_pin!");
		return ret;
	}
	AW_LOGI("I2C target confirmed: 7-bit addr=0x%02x, chipid=0x%06x",
			g_haptic_hv->i2c_addr >> 1, g_haptic_hv->chipid);

	haptic_hv_api_init();
	haptic_hv_sw_reset();

	ret = g_func_haptic_hv->offset_cali();
	if (ret != AW_SUCCESS)
		haptic_hv_sw_reset();

	ret = haptic_hv_ram_init();
	if (ret != AW_SUCCESS) {
		AW_LOGE("ram init err!!!");
		return ret;
	}
	haptic_hv_get_ram_num();
	g_func_haptic_hv->interrupt_setup();
	g_func_haptic_hv->play_mode(AW_STANDBY_MODE);
	g_func_haptic_hv->misc_para_init();
	g_func_haptic_hv->auto_bst_enable(g_haptic_hv->auto_bst);
	g_func_haptic_hv->set_bst_vol(g_dts->bst_vol);

	/* GUI/bx.txt waveform timing: WAVDAT_MODE = 48 kHz. */
	g_func_haptic_hv->set_pwm(AW_PWM_48K);
#ifdef AW_F0_CALI_DURING_STARTUP
	/* f0 calibration */
	haptic_hv_f0_cali();
#else
	haptic_hv_get_cali_from_flash();
#endif

	AW_LOGI("end");
	return ret;
}

/*********************************************************
 * 以下为 AW8693X 芯片层（原 aw8693x.c）
 *********************************************************/

struct aw_haptic_dts aw8693x_dts = {
	.f0_pre = 1700,
	.d2s_gain = 0x04,
	.gain_bypass = 0x01,
	.f0_cali_percent = 15,
	.cont_drv1_lvl = 0x7f,
	.cont_lra_vrms = 1000,
	.cont_brk_time = 0x08,
	.cont_brk_gain = 0x08,
	.cont_drv1_time = 0x04,
	.cont_drv2_time = 0x14,
	.cont_smart_loop = 0x00,
	.cont_track_margin = 0x0f,
	.enabled_one_wire = AW_FALSE,
	.bst_vol = 8500,
};

/* aw8693x_trig1/2/3 三张参数表已删（2026-10-14）：唯一的用户
 * trig*_param_config() 从没被函数表调用过，表和 struct 一起走了。 */

static void aw8693x_reg_unlock(aw_bool flag)
{
	uint8_t reg_val = 0;

	if (flag) {
		/* Unlock register */
		reg_val = AW8693X_BIT_TMCFG_TM_UNLOCK;
		haptic_hv_i2c_writes(AW8693X_REG_TMCFG, &reg_val, AW_I2C_BYTE_ONE);
	} else {
		/* Lock register */
		reg_val = AW8693X_BIT_TMCFG_TM_LOCK;
		haptic_hv_i2c_writes(AW8693X_REG_TMCFG, &reg_val, AW_I2C_BYTE_ONE);
	}
}

static int aw8693x_check_qualify(void)
{
	uint8_t reg_val = 0;

	haptic_hv_i2c_reads(AW8693X_REG_EFCFG6, &reg_val, AW_I2C_BYTE_ONE);
	if (!(reg_val & 0x80)) {
		AW_LOGE("unqualified chip");
		return AW_ERROR;
	}
	AW_LOGI("qualified chip");

	return AW_SUCCESS;
}

static void aw8693x_raminit_enable(aw_bool flag)
{
	if (flag) {
		haptic_hv_i2c_write_bits(AW8693X_REG_SYSCTRL3, AW8693X_BIT_SYSCTRL3_EN_RAMINIT_MASK,
								 AW8693X_BIT_SYSCTRL3_EN_RAMINIT_ON);
		haptic_hv_udelay(500);
	} else {
		haptic_hv_i2c_write_bits(AW8693X_REG_SYSCTRL3, AW8693X_BIT_SYSCTRL3_EN_RAMINIT_MASK,
								 AW8693X_BIT_SYSCTRL3_EN_RAMINIT_OFF);
	}
}

static void aw8693x_set_pwm(aw_pwm pwm_type)
{
	switch (pwm_type) {
	case AW_PWM_48K:
		haptic_hv_i2c_write_bits(AW8693X_REG_SYSCTRL4, AW8693X_BIT_SYSCTRL4_WAVDAT_MODE_MASK,
								 AW8693X_BIT_SYSCTRL4_WAVDAT_48K);
		break;
	case AW_PWM_24K:
		haptic_hv_i2c_write_bits(AW8693X_REG_SYSCTRL4, AW8693X_BIT_SYSCTRL4_WAVDAT_MODE_MASK,
								 AW8693X_BIT_SYSCTRL4_WAVDAT_24K);
		break;
	case AW_PWM_12K:
		haptic_hv_i2c_write_bits(AW8693X_REG_SYSCTRL4, AW8693X_BIT_SYSCTRL4_WAVDAT_MODE_MASK,
								 AW8693X_BIT_SYSCTRL4_WAVDAT_12K);
		break;
	case AW_PWM_8K:
		haptic_hv_i2c_write_bits(AW8693X_REG_SYSCTRL4, AW8693X_BIT_SYSCTRL4_WAVDAT_MODE_MASK,
								 AW8693X_BIT_SYSCTRL4_WAVDAT_8K);
		break;
	default:
		AW_LOGE("error param");
		break;
	}
}

static void aw8693x_set_gain(uint8_t gain)
{
	haptic_hv_i2c_writes(AW8693X_REG_PLAYCFG2, &gain, AW_I2C_BYTE_ONE);
}

static void aw8693x_set_bst_vol(uint32_t bst_vol)
{
	uint8_t reg_val = 0;

	bst_vol = (bst_vol < AW8693X_BST_VOL_MIN) ? AW8693X_BST_VOL_MIN : bst_vol;
	bst_vol = (bst_vol > AW8693X_BST_VOL_MAX) ? AW8693X_BST_VOL_MAX : bst_vol;
	reg_val = AW8693X_BST_VOL_FORMULA(bst_vol);
	AW_LOGI("set bst vol = %u mV, reg_val = 0x%02x", bst_vol, reg_val);

	haptic_hv_i2c_write_bits(AW8693X_REG_PLAYCFG1, AW8693X_BIT_PLAYCFG1_BST_VOUT_VREFSET_MASK, reg_val);
}

static void aw8693x_set_wav_seq(uint8_t wav, uint8_t seq)
{
	haptic_hv_i2c_writes(AW8693X_REG_WAVCFG1 + wav, &seq, AW_I2C_BYTE_ONE);
}

static void aw8693x_set_main_loop(uint8_t loop)
{
	haptic_hv_i2c_write_bits(AW8693X_REG_WAVCFG13, AW8693X_BIT_WAVCFG13_MAINLOOP_MASK, loop);
}

static void aw8693x_set_wav_loop(uint8_t wav, uint8_t loop)
{
	if (wav % 2) {
		loop <<= 0;
		haptic_hv_i2c_write_bits(AW8693X_REG_WAVCFG9 + (wav / 2),
								 AW8693X_BIT_WAVLOOP_SEQ_EVEN_MASK, loop);
	} else {
		loop <<= 4;
		haptic_hv_i2c_write_bits(AW8693X_REG_WAVCFG9 + (wav / 2),
								 AW8693X_BIT_WAVLOOP_SEQ_ODD_MASK, loop);
	}
}

static void aw8693x_set_ram_addr(void)
{
	uint8_t ram_addr[2] = {0};

	ram_addr[0] = g_haptic_hv->base_addr >> 8;
	ram_addr[1] = g_haptic_hv->base_addr & 0xFF;
	haptic_hv_i2c_writes(AW8693X_REG_RAMADDRH, ram_addr, AW_I2C_BYTE_TWO);
}

static void aw8693x_set_base_addr(void)
{
	uint8_t reg_val[5] = {0};
	uint32_t temp = 0;

	/* RAM Base Addr H */
	reg_val[0] = (uint8_t)(g_haptic_hv->base_addr >> 8);
	/* RAM Base Addr L */
	reg_val[1] = (uint8_t)(g_haptic_hv->base_addr & 0xFF);
	/* FIFO AEH & AFH */
	reg_val[2] = (uint8_t)(AW8693X_FIFO_AE_ADDR_H(g_haptic_hv->base_addr) | AW8693X_FIFO_AF_ADDR_H(g_haptic_hv->base_addr));
	/* FIFO AEL */
	reg_val[3] = (uint8_t)(AW8693X_FIFO_AE_ADDR_L(g_haptic_hv->base_addr));
	/* FIFO AFL */
	reg_val[4] = (uint8_t)(AW8693X_FIFO_AF_ADDR_L(g_haptic_hv->base_addr));
	haptic_hv_i2c_writes(AW8693X_REG_RTPCFG1, reg_val, AW_I2C_BYTE_FIVE);

	haptic_hv_i2c_reads(AW8693X_REG_RTPCFG3, reg_val, AW_I2C_BYTE_THREE);
	temp = ((reg_val[0] & 0x0f) << 24) | ((reg_val[0] & 0xf0) << 4);
	temp = temp | reg_val[1];
	AW_LOGI("almost_empty_threshold = %d", (temp & 0x0000ffff));
	temp = temp | (reg_val[2] << 16);
	AW_LOGI("almost_full_threshold = %d", temp >> 16);
}

static void aw8693x_auto_brake_mode(aw_bool flag)
{
	if (flag) {
		haptic_hv_i2c_write_bits(AW8693X_REG_PLAYCFG3, AW8693X_BIT_PLAYCFG3_BRK_EN_MASK,
								 AW8693X_BIT_PLAYCFG3_BRK_ENABLE);
	} else {
		haptic_hv_i2c_write_bits(AW8693X_REG_PLAYCFG3, AW8693X_BIT_PLAYCFG3_BRK_EN_MASK,
								 AW8693X_BIT_PLAYCFG3_BRK_DISABLE);
	}
}

static void aw8693x_f0_detect(aw_bool flag)
{
	if (flag) {
		haptic_hv_i2c_write_bits(AW8693X_REG_CONTCFG1, AW8693X_BIT_CONTCFG1_EN_F0_DET_MASK,
								 AW8693X_BIT_CONTCFG1_F0_DET_ENABLE);
	} else {
		haptic_hv_i2c_write_bits(AW8693X_REG_CONTCFG1, AW8693X_BIT_CONTCFG1_EN_F0_DET_MASK,
								 AW8693X_BIT_CONTCFG1_F0_DET_DISABLE);
	}
}

static uint8_t aw8693x_get_glb_state(void)
{
	uint8_t state = 0;

	haptic_hv_i2c_reads(AW8693X_REG_GLBRD5, &state, AW_I2C_BYTE_ONE);

	return state;
}

static void aw8693x_play_go(aw_bool flag)
{
	uint8_t reg_val = 0;

	AW_LOGI("enter, flag = %d", flag);

	if (flag) {
		reg_val = AW8693X_BIT_PLAYCFG4_GO_ON;
		haptic_hv_i2c_writes(AW8693X_REG_PLAYCFG4, &reg_val, AW_I2C_BYTE_ONE);
	} else {
		reg_val = AW8693X_BIT_PLAYCFG4_STOP_ON;
		haptic_hv_i2c_writes(AW8693X_REG_PLAYCFG4, &reg_val, AW_I2C_BYTE_ONE);
	}
}

static int aw8693x_wait_enter_standby(void)
{
	uint8_t reg_val = 0;
	/*
	 * 官方原版是 100 次 × 2ms = 200ms。
	 * 实测：正常起停（开机那几次 play_stop）几毫秒就进 standby 了；但从
	 * RAM_LOOP_MODE 的长振动里强制 Stop() 时，芯片会先进入 BRAKE(0x0b) 把
	 * 还在高速转的 LRA 刹住，200ms 不够，于是每次都报
	 * "do not enter standby automatically" 再走兜底。
	 * 放宽到 300 次 = 600ms：成功时是看到 standby 就立即返回，不会变慢，
	 * 只有失败路径会多等 400ms。若 600ms 仍然报错，说明是真卡在 BRAKE，
	 * 那就不是等待时长的问题了。
	 */
	int count = 300;

	while (count--) {
		reg_val = aw8693x_get_glb_state();
		if (reg_val == AW8693X_BIT_GLBRD5_STATE_STANDBY) {
			AW_LOGI("entered standby!");
			return AW_SUCCESS;
		}
		haptic_hv_mdelay(2);
	}
	AW_LOGE("do not enter standby automatically, glb_state = 0x%02x", reg_val);

	return AW_ERROR;
}

static void aw8693x_bst_mode_config(aw_bst_mode mode)
{
	switch (mode) {
	case AW_BST_BOOST_MODE:
		AW_LOGI("haptic bst mode = bst");
		haptic_hv_i2c_write_bits(AW8693X_REG_PLAYCFG1, AW8693X_BIT_PLAYCFG1_BST_MODE_MASK,
								 AW8693X_BIT_PLAYCFG1_BST_MODE);
		break;
	case AW_BST_BYPASS_MODE:
		AW_LOGI("haptic bst mode = bypass");
		haptic_hv_i2c_write_bits(AW8693X_REG_PLAYCFG1, AW8693X_BIT_PLAYCFG1_BST_MODE_MASK,
								 AW8693X_BIT_PLAYCFG1_BST_MODE_BYPASS);
		break;
	default:
		AW_LOGE("mode = %d error", mode);
		break;
	}
}

static void aw8693x_set_vbat_ref(int vbat_ref)
{
	uint8_t reg_val = 0;

	if (vbat_ref >= AW_VBAT_REF_5P5V)
		reg_val = AW8693X_BIT_DETCFG1_VBAT_REF_5P5V;
	else if (vbat_ref >= AW_VBAT_REF_5P0V)
		reg_val = AW8693X_BIT_DETCFG1_VBAT_REF_5P0V;
	else if (vbat_ref >= AW_VBAT_REF_4P8V)
		reg_val = AW8693X_BIT_DETCFG1_VBAT_REF_4P8V;
	else if (vbat_ref >= AW_VBAT_REF_4P5V)
		reg_val = AW8693X_BIT_DETCFG1_VBAT_REF_4P5V;
	else if (vbat_ref >= AW_VBAT_REF_4P2V)
		reg_val = AW8693X_BIT_DETCFG1_VBAT_REF_4P2V;
	else if (vbat_ref >= AW_VBAT_REF_4P0V)
		reg_val = AW8693X_BIT_DETCFG1_VBAT_REF_4P0V;
	else if (vbat_ref >= AW_VBAT_REF_3P6V)
		reg_val = AW8693X_BIT_DETCFG1_VBAT_REF_3P6V;
	else if (vbat_ref >= AW_VBAT_REF_3P3V)
		reg_val = AW8693X_BIT_DETCFG1_VBAT_REF_3P3V;
	else
		reg_val = AW8693X_BIT_DETCFG1_VBAT_REF_3P3V;

	haptic_hv_i2c_write_bits(AW8693X_REG_DETCFG1, AW8693X_BIT_DETCFG1_VBAT_REF_MASK, reg_val);
}

static void aw8693x_vbat_mode_config(aw_vbat_mode flag)
{
	if (flag == AW_HW_VBAT_COMP_MODE) {
		haptic_hv_i2c_write_bits(AW8693X_REG_VBATCTRL, AW8693X_BIT_VBATCTRL_VBAT_MODE_MASK,
								 AW8693X_BIT_VBATCTRL_VBAT_MODE_HW);
	} else {
		haptic_hv_i2c_write_bits(AW8693X_REG_VBATCTRL, AW8693X_BIT_VBATCTRL_VBAT_MODE_MASK,
								 AW8693X_BIT_VBATCTRL_VBAT_MODE_SW);
	}
}

static void aw8693x_play_mode(aw_mode play_mode)
{
	switch (play_mode) {
	case AW_STANDBY_MODE:
		AW_LOGI("enter standby mode");
		g_haptic_hv->play_mode = AW_STANDBY_MODE;
		haptic_hv_i2c_write_bits(AW8693X_REG_SYSCTRL3, AW8693X_BIT_SYSCTRL3_STANDBY_MASK,
								 AW8693X_BIT_SYSCTRL3_STANDBY_ON);
		haptic_hv_i2c_write_bits(AW8693X_REG_SYSCTRL3, AW8693X_BIT_SYSCTRL3_STANDBY_MASK,
								 AW8693X_BIT_SYSCTRL3_STANDBY_OFF);
		break;
	case AW_RAM_MODE:
		AW_LOGI("enter ram mode");
		g_haptic_hv->play_mode = AW_RAM_MODE;
		haptic_hv_i2c_write_bits(AW8693X_REG_PLAYCFG3, AW8693X_BIT_PLAYCFG3_PLAY_MODE_MASK,
								 AW8693X_BIT_PLAYCFG3_PLAY_MODE_RAM);
		aw8693x_auto_brake_mode(AW_FALSE);
		aw8693x_bst_mode_config(AW_BST_BOOST_MODE);
		aw8693x_vbat_mode_config(AW_SW_VBAT_COMP_MODE);
		break;
	case AW_RAM_LOOP_MODE:
		AW_LOGI("enter ram loop mode");
		g_haptic_hv->play_mode = AW_RAM_LOOP_MODE;
		haptic_hv_i2c_write_bits(AW8693X_REG_PLAYCFG3, AW8693X_BIT_PLAYCFG3_PLAY_MODE_MASK,
								 AW8693X_BIT_PLAYCFG3_PLAY_MODE_RAM);
		aw8693x_auto_brake_mode(AW_TRUE);
		aw8693x_bst_mode_config(AW_BST_BYPASS_MODE);
		aw8693x_vbat_mode_config(AW_HW_VBAT_COMP_MODE);
		break;
	case AW_RTP_MODE:
		AW_LOGI("enter rtp mode");
		g_haptic_hv->play_mode = AW_RTP_MODE;
		haptic_hv_i2c_write_bits(AW8693X_REG_PLAYCFG3, AW8693X_BIT_PLAYCFG3_PLAY_MODE_MASK,
								 AW8693X_BIT_PLAYCFG3_PLAY_MODE_RTP);
		aw8693x_auto_brake_mode(AW_TRUE);
		aw8693x_bst_mode_config(AW_BST_BOOST_MODE);
		aw8693x_vbat_mode_config(AW_SW_VBAT_COMP_MODE);
		break;
	case AW_TRIG_MODE:
		AW_LOGI("enter trig mode");
		g_haptic_hv->play_mode = AW_TRIG_MODE;
		haptic_hv_i2c_write_bits(AW8693X_REG_PLAYCFG3, AW8693X_BIT_PLAYCFG3_PLAY_MODE_MASK,
								 AW8693X_BIT_PLAYCFG3_PLAY_MODE_RAM);
		aw8693x_vbat_mode_config(AW_SW_VBAT_COMP_MODE);
		break;
	case AW_CONT_MODE:
		AW_LOGI("enter cont mode");
		g_haptic_hv->play_mode = AW_CONT_MODE;
		haptic_hv_i2c_write_bits(AW8693X_REG_PLAYCFG3, AW8693X_BIT_PLAYCFG3_PLAY_MODE_MASK,
								 AW8693X_BIT_PLAYCFG3_PLAY_MODE_CONT);
		aw8693x_auto_brake_mode(AW_TRUE);
		aw8693x_bst_mode_config(AW_BST_BYPASS_MODE);
		aw8693x_vbat_mode_config(AW_HW_VBAT_COMP_MODE);
		break;
	default:
		AW_LOGE("play mode %d error", play_mode);
		break;
	}
}

static void aw8693x_play_stop(void)
{
	uint8_t reg_val = 0;
	int ret = 0;

	AW_LOGI("enter");
	g_haptic_hv->play_mode = AW_STANDBY_MODE;
	reg_val = AW8693X_BIT_PLAYCFG4_GO_ON;
	aw8693x_raminit_enable(AW_TRUE);
	haptic_hv_i2c_write_bits(AW8693X_REG_PLAYCFG3, AW8693X_BIT_PLAYCFG3_PLAY_MODE_MASK,
					AW8693X_BIT_PLAYCFG3_PLAY_MODE_STOP);
	haptic_hv_i2c_writes(AW8693X_REG_PLAYCFG4, &reg_val, AW_I2C_BYTE_ONE);
	aw8693x_raminit_enable(AW_FALSE);
	ret = aw8693x_wait_enter_standby();
	if (ret == AW_ERROR) {
		AW_LOGE("force to enter standby mode!");
		aw8693x_play_mode(AW_STANDBY_MODE);
	}
}

static int aw8693x_read_f0(void)
{
	uint8_t reg_val[2] = {0};
	uint32_t f0_reg = 0;

#ifdef AW_LRA_F0_DEFAULT
	/* lra_f0 */
	haptic_hv_i2c_reads(AW8693X_REG_CONTRD14, reg_val, AW_I2C_BYTE_TWO);
	f0_reg = (reg_val[0] << 8) | reg_val[1];
	if (!f0_reg) {
		g_haptic_hv->f0 = 0;
		AW_LOGE("lra_f0 is error, f0_reg = 0");
		return AW_ERROR;
	}
	g_haptic_hv->f0 = AW8693X_F0_FORMULA(f0_reg);
	AW_LOGI("lra_f0 = %d", g_haptic_hv->f0);
#else
	/* cont_f0 */
	haptic_hv_i2c_reads(AW8693X_REG_CONTRD16, reg_val, AW_I2C_BYTE_TWO);
	f0_reg = (reg_val[0] << 8) | reg_val[1];
	if (!f0_reg) {
		g_haptic_hv->f0 = 0;
		AW_LOGE("cont_f0 is error, f0_reg = 0");
		return AW_ERROR;
	}
	g_haptic_hv->f0 = AW8693X_F0_FORMULA(f0_reg);
	AW_LOGI("cont_f0 = %d", g_haptic_hv->f0);
#endif
	return AW_SUCCESS;
}

static void aw8693x_upload_lra(aw_cali_lra cali_type)
{
	uint8_t cali_data = 0;

	switch (cali_type) {
	case AW_WRITE_ZERO:
		AW_LOGI("write zero to trim_lra!");
		break;
	case AW_F0_CALI_LRA:
		AW_LOGI("write f0_cali_data to trim_lra = 0x%02X", g_haptic_hv->f0_cali_data);
		cali_data = g_haptic_hv->f0_cali_data & (~AW8693X_BIT_TRIMCFG5_TRIM_LRA_MASK);
		break;
	case AW_OSC_CALI_LRA:
		AW_LOGI("write osc_cali_data to trim_lra = 0x%02X", g_haptic_hv->osc_cali_data);
		cali_data = g_haptic_hv->osc_cali_data & (~AW8693X_BIT_TRIMCFG5_TRIM_LRA_MASK);
		break;
	default:
		AW_LOGE("error param, write f0_cali_data to trim_lra!");
		cali_data = g_haptic_hv->f0_cali_data & (~AW8693X_BIT_TRIMCFG5_TRIM_LRA_MASK);
		break;
	}

	haptic_hv_i2c_writes(AW8693X_REG_TRIMCFG5, &cali_data, AW_I2C_BYTE_ONE);
}

static void aw8693x_auto_bst_enable(aw_bool flag)
{
	g_haptic_hv->auto_bst = flag;

	if (flag) {
		haptic_hv_i2c_write_bits(AW8693X_REG_PLAYCFG3, AW8693X_BIT_PLAYCFG3_AUTO_BST_MASK,
								 AW8693X_BIT_PLAYCFG3_AUTO_BST_ENABLE);
	} else {
		haptic_hv_i2c_write_bits(AW8693X_REG_PLAYCFG3, AW8693X_BIT_PLAYCFG3_AUTO_BST_MASK,
								 AW8693X_BIT_PLAYCFG3_AUTO_BST_DISABLE);
	}
}

static void aw8693x_get_vbat(void)
{
	uint8_t reg_array[2] = {0};
	uint32_t vbat_code = 0;

	aw8693x_play_stop();
	aw8693x_raminit_enable(AW_TRUE);
	haptic_hv_i2c_write_bits(AW8693X_REG_DETCFG2, AW8693X_BIT_DETCFG2_DET_SEQ0_MASK,
							 AW8693X_BIT_DETCFG2_DET_SEQ0_VBAT);
	haptic_hv_i2c_write_bits(AW8693X_REG_DETCFG1, AW8693X_BIT_DETCFG1_DET_GO_MASK,
							 AW8693X_BIT_DETCFG1_DET_GO_ON);
	haptic_hv_mdelay(3);
	haptic_hv_i2c_write_bits(AW8693X_REG_DETCFG1, AW8693X_BIT_DETCFG1_DET_GO_MASK,
							 AW8693X_BIT_DETCFG1_DET_GO_OFF);
	haptic_hv_i2c_reads(AW8693X_REG_DETRD1, reg_array, AW_I2C_BYTE_TWO);
	aw8693x_raminit_enable(AW_FALSE);

	vbat_code = ((reg_array[0] & (~AW8693X_BIT_DETRD1_AVG_DATA_H_MASK)) << 8) + reg_array[1];
	g_haptic_hv->vbat = AW8693X_VBAT_FORMULA(vbat_code);

	if (g_haptic_hv->vbat > AW_VBAT_MAX) {
		g_haptic_hv->vbat = AW_VBAT_MAX;
		AW_LOGI("vbat max limit = %d", g_haptic_hv->vbat);
	}
	if (g_haptic_hv->vbat < AW_VBAT_MIN) {
		g_haptic_hv->vbat = AW_VBAT_MIN;
		AW_LOGI("vbat min limit = %d", g_haptic_hv->vbat);
	}
	AW_LOGI("awinic->vbat=%dmV, vbat_code=0x%02X", g_haptic_hv->vbat, vbat_code);
}

static int aw8693x_select_d2s_gain(uint8_t reg)
{
	int d2s_gain = 0;

	switch (reg) {
	case AW8693X_BIT_DETCFG2_D2S_GAIN_1:
		d2s_gain = 1;
		break;
	case AW8693X_BIT_DETCFG2_D2S_GAIN_2:
		d2s_gain = 2;
		break;
	case AW8693X_BIT_DETCFG2_D2S_GAIN_4:
		d2s_gain = 4;
		break;
	case AW8693X_BIT_DETCFG2_D2S_GAIN_8:
		d2s_gain = 8;
		break;
	case AW8693X_BIT_DETCFG2_D2S_GAIN_10:
		d2s_gain = 10;
		break;
	case AW8693X_BIT_DETCFG2_D2S_GAIN_16:
		d2s_gain = 16;
		break;
	case AW8693X_BIT_DETCFG2_D2S_GAIN_20:
		d2s_gain = 20;
		break;
	case AW8693X_BIT_DETCFG2_D2S_GAIN_40:
		d2s_gain = 40;
		break;
	default:
		d2s_gain = -1;
		break;
	}

	return d2s_gain;
}

/* const：波形库来自 .rodata（haptic_hv_waveforms.c），
 * 这里只读不写，加 const 才能把 7685 字节留在 flash 而不是复制进 RAM。 */
static void aw8693x_set_ram_data(const uint8_t *data, uint32_t len)
{
	haptic_hv_i2c_writes(AW8693X_REG_RAMDATA, data, len);
}

static void aw8693x_get_ram_data(uint8_t *data, uint32_t len)
{
	haptic_hv_i2c_reads(AW8693X_REG_RAMDATA, data, len);
}

static void aw8693x_interrupt_setup(void)
{
	uint8_t reg_val = 0;

	haptic_hv_i2c_reads(AW8693X_REG_SYSINT, &reg_val, AW_I2C_BYTE_ONE);

	/* edge int mode */
	haptic_hv_i2c_write_bits(AW8693X_REG_SYSCTRL4, AW8693X_BIT_SYSCTRL4_INT_MODE_MASK,
							 AW8693X_BIT_SYSCTRL4_INT_MODE_POSEDGE);
	/* int enable */
	haptic_hv_i2c_write_bits(AW8693X_REG_SYSINTM, (AW8693X_BIT_SYSINTM_BST_SCPM_MASK &
							 AW8693X_BIT_SYSINTM_BST_OVPM_MASK & AW8693X_BIT_SYSINTM_UVLM_MASK &
							 AW8693X_BIT_SYSINTM_OCDM_MASK & AW8693X_BIT_SYSINTM_OTM_MASK),
							 (AW8693X_BIT_SYSINTM_BST_SCPM_ON | AW8693X_BIT_SYSINTM_BST_OVPM_OFF |
							 AW8693X_BIT_SYSINTM_UVLM_ON | AW8693X_BIT_SYSINTM_OCDM_ON |
							 AW8693X_BIT_SYSINTM_OTM_ON));
}

static void aw8693x_protect_config(uint8_t prtime, uint8_t prlvl)
{
	uint8_t reg_val = 0;

	haptic_hv_i2c_write_bits(AW8693X_REG_PWMCFG1, AW8693X_BIT_PWMCFG1_PRC_EN_MASK,
							 AW8693X_BIT_PWMCFG1_PRC_DISABLE);
	if (prlvl != 0) {
		/* Enable protection mode */
		AW_LOGI("enable protection mode");
		reg_val = AW8693X_BIT_PWMCFG3_PR_ENABLE | (prlvl & (~AW8693X_BIT_PWMCFG3_PRLVL_MASK));
		haptic_hv_i2c_writes(AW8693X_REG_PWMCFG3, &reg_val, AW_I2C_BYTE_ONE);
		haptic_hv_i2c_writes(AW8693X_REG_PWMCFG4, &prtime, AW_I2C_BYTE_ONE);
	} else {
		/* Disable */
		AW_LOGI("disable protection mode");
		haptic_hv_i2c_write_bits(AW8693X_REG_PWMCFG3, AW8693X_BIT_PWMCFG3_PR_EN_MASK,
								 AW8693X_BIT_PWMCFG3_PR_DISABLE);
	}
}

static int aw8693x_offset_cali(void)
{
	int os_code = 0;
	int d2s_gain = 0;
	uint8_t reg_val[3] = { 0 };

	if (aw8693x_dts.cont_smart_loop)
		return AW_SUCCESS;

	if (!aw8693x_dts.d2s_gain)
		AW_LOGE("aw8693x_dts.d2s_gain = 0!");
	haptic_hv_i2c_write_bits(AW8693X_REG_DETCFG2, AW8693X_BIT_DETCFG2_D2S_GAIN_MASK,
				 aw8693x_dts.d2s_gain);
	aw8693x_raminit_enable(AW_TRUE);
	haptic_hv_i2c_write_bits(AW8693X_REG_DETCFG2, AW8693X_BIT_DETCFG2_DET_SEQ0_MASK,
							 AW8693X_BIT_DETCFG2_DET_SEQ0_OS);
	haptic_hv_i2c_write_bits(AW8693X_REG_DETCFG1, AW8693X_BIT_DETCFG1_DET_GO_MASK,
							 AW8693X_BIT_DETCFG1_DET_GO_ON);
	haptic_hv_mdelay(3);
	haptic_hv_i2c_write_bits(AW8693X_REG_DETCFG1, AW8693X_BIT_DETCFG1_DET_GO_MASK,
							 AW8693X_BIT_DETCFG1_DET_GO_OFF);
	haptic_hv_i2c_reads(AW8693X_REG_DETRD1, reg_val, AW_I2C_BYTE_TWO);
	aw8693x_raminit_enable(AW_FALSE);
	d2s_gain = aw8693x_select_d2s_gain(aw8693x_dts.d2s_gain);
	if (d2s_gain <= 0) {
		AW_LOGE("d2s_gain is error");
		return AW_ERROR;
	}
	os_code = ((reg_val[0] & (~AW8693X_BIT_DETRD1_AVG_DATA_H_MASK)) << 8) | reg_val[1];
	os_code = AW8693X_OS_FORMULA(os_code, d2s_gain);
	AW_LOGI("os_code = %d", os_code);
	if (os_code > 15 || os_code < -15) {
		AW_LOGE("offset calibration out of range");
		return AW_ERROR;
	}

	return AW_SUCCESS;
}

static int aw8693x_get_f0(void)
{
	uint8_t reg_val = 0;
	uint8_t brk_en_default = 0;
	uint8_t cont_config[4] = {0};
	int drv_width = 0;
	int f_pre = 0;
	int ret = 0;

	g_haptic_hv->f0 = aw8693x_dts.f0_pre;
	/* enter standby mode */
	aw8693x_play_stop();
	haptic_hv_i2c_write_bits(AW8693X_REG_DETCFG2, AW8693X_BIT_DETCFG2_D2S_GAIN_MASK,
							 aw8693x_dts.d2s_gain);
	aw8693x_set_vbat_ref(AW_VBAT_REF_4P0V);
	/* enable f0 detect */
	aw8693x_f0_detect(AW_TRUE);
	/* enable auto break */
	haptic_hv_i2c_reads(AW8693X_REG_PLAYCFG3, &reg_val, AW_I2C_BYTE_ONE);
	brk_en_default = reg_val & (~AW8693X_BIT_PLAYCFG3_BRK_EN_MASK);
	aw8693x_auto_brake_mode(AW_TRUE);
#ifdef AW_RAM_GET_F0
	if (g_haptic_hv->ram_num < AW_RAM_GET_F0_SEQ) {
		AW_LOGE("miss ram get f0 waveform!");
		return AW_ERROR;
	}
	/* f0 calibrate work mode */
	aw8693x_play_mode(AW_RAM_MODE);
	aw8693x_set_bst_vol(8000);
	aw8693x_set_wav_seq(0x00, AW_RAM_GET_F0_SEQ);
	aw8693x_set_wav_seq(0x01, 0x00);
	aw8693x_set_gain(AW_GAIN_LEVEL_1);
	aw8693x_set_wav_loop(0x00, AW_LOOP2);
#else
	/* f0 calibrate work mode */
	aw8693x_play_mode(AW_CONT_MODE);
	/* f0 driver level & time */
	cont_config[0] = aw8693x_dts.cont_drv1_lvl | AW8693X_BIT_CONTCFG6_TRACK_ENABLE;
	cont_config[1] = aw8693x_dts.cont_drv2_lvl;
	cont_config[2] = aw8693x_dts.cont_drv1_time;
	cont_config[3] = aw8693x_dts.cont_drv2_time;
	haptic_hv_i2c_writes(AW8693X_REG_CONTCFG6, cont_config, AW_I2C_BYTE_FOUR);
	/* TRACK_MARGIN */
	if (!aw8693x_dts.cont_track_margin)
		AW_LOGE("aw8693x_dts.cont_track_margin = 0");
	else
		haptic_hv_i2c_writes(AW8693X_REG_CONTCFG11, &aw8693x_dts.cont_track_margin, AW_I2C_BYTE_ONE);
	/* DRV_WIDTH */
	if (!aw8693x_dts.f0_pre)
		return AW_ERROR;
	drv_width = AW_DRV_WIDTH_FORMULA(aw8693x_dts.f0_pre, aw8693x_dts.cont_brk_gain,
									 aw8693x_dts.cont_track_margin);
	if (drv_width < AW_REG_VALUE_MIN)
		drv_width = AW_REG_VALUE_MIN;
	if (drv_width > AW_REG_VALUE_MAX)
		drv_width = AW_REG_VALUE_MAX;
	cont_config[1] = (uint8_t)drv_width;

	f_pre = AW8693X_F_PRE_FORMULA(aw8693x_dts.f0_pre);
	cont_config[0] = (f_pre > AW_REG_VALUE_MAX) ? AW_REG_VALUE_MAX : f_pre;
	haptic_hv_i2c_writes(AW8693X_REG_CONTCFG2, cont_config, AW_I2C_BYTE_TWO);
#endif
	/* play go */
	aw8693x_play_go(AW_TRUE);
	haptic_hv_mdelay(2);
	aw8693x_wait_enter_standby();
	ret = aw8693x_read_f0();
	aw8693x_f0_detect(AW_FALSE);
	aw8693x_set_vbat_ref(AW_VBAT_REFER);
	/* recover auto break config */
	if (brk_en_default)
		aw8693x_auto_brake_mode(AW_TRUE);
	else
		aw8693x_auto_brake_mode(AW_FALSE);
#ifdef AW_RAM_GET_F0
	aw8693x_set_bst_vol(aw8693x_dts.bst_vol);
#endif

	return ret;
}

static void aw8693x_misc_para_init(void)
{
	uint8_t reg_val[5] = {0};
	uint32_t drv2_lvl = 0;

	/* Set I2C broadcast addr */
	reg_val[0] = g_haptic_hv->i2c_addr >> 1;
	haptic_hv_i2c_writes(AW8693X_REG_SYSCTRL5, reg_val, AW_I2C_BYTE_ONE);
	/* Cont drv2 lvl */
	drv2_lvl = AW8693X_DRV2_LVL_FORMULA(aw8693x_dts.f0_pre, aw8693x_dts.cont_lra_vrms);
	aw8693x_dts.cont_drv2_lvl = (drv2_lvl > AW_DRV2_LVL_MAX) ? AW_DRV2_LVL_MAX : drv2_lvl;

	/* Set gain_bypass */
	haptic_hv_i2c_write_bits(AW8693X_REG_SYSCTRL4, AW8693X_BIT_SYSCTRL4_GAIN_BYPASS_MASK,
							 aw8693x_dts.gain_bypass);

	haptic_hv_i2c_write_bits(AW8693X_REG_CONTCFG1, AW8693X_BIT_CONTCFG1_SMART_LOOP_MASK,
							 aw8693x_dts.cont_smart_loop << 7);
	haptic_hv_i2c_write_bits(AW8693X_REG_CONTCFG5, AW8693X_BIT_CONTCFG5_BRK_GAIN_MASK,
							 aw8693x_dts.cont_brk_gain);
	haptic_hv_i2c_writes(AW8693X_REG_CONTCFG10, &aw8693x_dts.cont_brk_time, AW_I2C_BYTE_ONE);
	aw8693x_protect_config(AW8693X_BIT_PWMCFG4_PRTIME_DEFAULT_VALUE,
						   AW8693X_BIT_PWMCFG3_PRLVL_DEFAULT_VALUE);
	aw8693x_set_vbat_ref(AW_VBAT_REFER);
	aw8693x_vbat_mode_config(AW_HW_VBAT_COMP_MODE);

#ifdef AW_RTP_AUTO_SIN_ENABLE
	haptic_hv_i2c_write_bits(AW8693X_REG_RTPCFG1, AW8693X_BIT_RTPCFG1_RTP_AUTO_SIN_MASK,
							 AW8693X_BIT_RTPCFG1_RTP_AUTO_SIN_ENABLE);
#endif
	reg_val[0] = AW8693X_BIT_ANACFG7_INIT_VAL;
	reg_val[1] = AW8693X_BIT_ANACFG8_INIT_VAL;
	reg_val[2] = AW8693X_BIT_ANACFG10_INIT_VAL;
	reg_val[3] = AW8693X_BIT_ANACFG15_INIT_VAL;
	reg_val[4] = AW8693X_BIT_ANACFG21_INIT_VAL;
	aw8693x_reg_unlock(AW_TRUE);
	haptic_hv_i2c_write_bits(AW8693X_REG_SYSCTRL6, AW8693X_BIT_SYSCTRL6_UVLO_ADJ_MASK,
							 AW8693X_BIT_SYSCTRL6_UVLO_ADJ_2P4V);
	haptic_hv_i2c_writes(AW8693X_REG_ANACFG7, reg_val, AW_I2C_BYTE_TWO);
	haptic_hv_i2c_writes(AW8693X_REG_ANACFG10, &reg_val[2], AW_I2C_BYTE_ONE);
	haptic_hv_i2c_writes(AW8693X_REG_ANACFG15, &reg_val[3], AW_I2C_BYTE_ONE);
	haptic_hv_i2c_writes(AW8693X_REG_ANACFG21, &reg_val[4], AW_I2C_BYTE_ONE);
	aw8693x_reg_unlock(AW_FALSE);
}

struct aw_haptic_func aw8693x_func_list = {
	.get_f0 = aw8693x_get_f0,
	.offset_cali = aw8693x_offset_cali,
	.check_qualify = aw8693x_check_qualify,
	.play_go = aw8693x_play_go,
	.set_pwm = aw8693x_set_pwm,
	.get_vbat = aw8693x_get_vbat,
	.set_gain = aw8693x_set_gain,
	.play_stop = aw8693x_play_stop,
	.play_mode = aw8693x_play_mode,
	.upload_lra = aw8693x_upload_lra,
	.set_bst_vol = aw8693x_set_bst_vol,
	.set_wav_seq = aw8693x_set_wav_seq,
	.set_wav_loop = aw8693x_set_wav_loop,
	.set_main_loop = aw8693x_set_main_loop,
	.set_ram_data = aw8693x_set_ram_data,
	.get_ram_data = aw8693x_get_ram_data,
	.set_ram_addr = aw8693x_set_ram_addr,
	.set_base_addr = aw8693x_set_base_addr,
	.raminit_enable = aw8693x_raminit_enable,
	.misc_para_init = aw8693x_misc_para_init,
	.protect_config = aw8693x_protect_config,
	.interrupt_setup = aw8693x_interrupt_setup,
	.auto_bst_enable = aw8693x_auto_bst_enable,
	.auto_brake_mode = aw8693x_auto_brake_mode,
	.bst_mode_config = aw8693x_bst_mode_config,
	.vbat_mode_config = aw8693x_vbat_mode_config,
	.get_glb_state = aw8693x_get_glb_state,
};
