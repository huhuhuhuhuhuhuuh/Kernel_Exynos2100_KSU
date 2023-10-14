/*
 * Samsung Exynos SoC series Sensor driver
 *
 *
 * Copyright (c) 2016 Samsung Electronics Co., Ltd
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/version.h>
#include <linux/gpio.h>
#include <linux/clk.h>
#include <linux/regulator/consumer.h>
#include <linux/videodev2.h>
#include <linux/videodev2_exynos_camera.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/platform_device.h>
#include <linux/of_gpio.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-subdev.h>

#include <exynos-is-sensor.h>
#include "is-hw.h"
#include "is-core.h"
#include "is-param.h"
#include "is-device-sensor.h"
#include "is-device-sensor-peri.h"
#include "is-resourcemgr.h"
#include "is-dt.h"
#include "is-cis-gh1.h"
#include "is-vender.h"
#include "is-cis-gh1-setA.h"
#include "is-cis-gh1-setB.h"
#include "is-helper-i2c.h"
#include "is-sec-define.h"

#define SENSOR_NAME "S5KGH1"
//#define GLOBAL_TIME_CAL_WRITE
//#define PDXTC_CAL_DISABLE
//#define XTC_CAL_DISABLE		/* !!!! check A1 0x42C0 setting(XTC bypass) */

static const struct v4l2_subdev_ops subdev_ops;

static bool xtc_cal_first = true;
static bool xtc_skip = false;
static const u32 *sensor_gh1_global;
static u32 sensor_gh1_global_size;
static const u32 *sensor_gh1_global_secure;
static u32 sensor_gh1_global_secure_size;
static const u32 **sensor_gh1_setfiles;
static const u32 *sensor_gh1_setfile_sizes;
static const struct sensor_pll_info_compact **sensor_gh1_pllinfos;
static u32 sensor_gh1_max_setfile_num;
/* settings for initial setfile */
static u32 sensor_gh1_post_init_setting_list[SENSOR_GH1_POST_INIT_SETTING_MAX];
static u32 sensor_gh1_post_init_setting_list_len = 0;

int sensor_gh1_cis_set_frame_rate(struct v4l2_subdev *subdev, u32 min_fps);

static bool sensor_gh1_cis_is_wdr_mode_on(cis_shared_data *cis_data)
{
//ToDo : WDR always off for bring-up
#if 1
	unsigned int mode = cis_data->sens_config_index_cur;

	if (is_vender_wdr_mode_on(cis_data) &&
		(mode == SENSOR_GH1_3648X2736_30FPS
		|| mode == SENSOR_GH1_3968X2232_30FPS
		|| mode == SENSOR_GH1_1824X1368_30FPS
		|| mode == SENSOR_GH1_1984X1116_30FPS
		|| mode == SENSOR_GH1_2944x2208_30FPS
		|| mode == SENSOR_GH1_3216x1808_30FPS))
		return true;
#endif
	return false;
}

static void sensor_gh1_cis_data_calculation(const struct sensor_pll_info_compact *pll_info_compact, cis_shared_data *cis_data)
{
	u64 vt_pix_clk_hz;
	u32 frame_rate, max_fps, frame_valid_us;

	FIMC_BUG_VOID(!pll_info_compact);

	/* 1. get pclk value from pll info */
	vt_pix_clk_hz = pll_info_compact->pclk;

	dbg_sensor(1, "ext_clock(%d), mipi_datarate(%llu), pclk(%llu)\n",
			pll_info_compact->ext_clk, pll_info_compact->mipi_datarate, pll_info_compact->pclk);

	/* 2. the time of processing one frame calculation (us) */
	cis_data->min_frame_us_time = (((u64)pll_info_compact->frame_length_lines) * pll_info_compact->line_length_pck * 1000
					/ (vt_pix_clk_hz / 1000));
	cis_data->cur_frame_us_time = cis_data->min_frame_us_time;

#ifdef CAMERA_REAR2
	cis_data->min_sync_frame_us_time = cis_data->min_frame_us_time;
#endif

	/* 3. FPS calculation */
	frame_rate = vt_pix_clk_hz / (pll_info_compact->frame_length_lines * pll_info_compact->line_length_pck);
	dbg_sensor(1, "frame_rate (%d) = vt_pix_clk_hz(%llu) / "
		KERN_CONT "(pll_info_compact->frame_length_lines(%d) * pll_info_compact->line_length_pck(%d))\n",
		frame_rate, vt_pix_clk_hz, pll_info_compact->frame_length_lines, pll_info_compact->line_length_pck);

	/* calculate max fps */
	max_fps = (vt_pix_clk_hz * 10) / (pll_info_compact->frame_length_lines * pll_info_compact->line_length_pck);
	max_fps = (max_fps % 10 >= 5 ? frame_rate + 1 : frame_rate);

	cis_data->pclk = vt_pix_clk_hz;
	cis_data->max_fps = max_fps;
	cis_data->frame_length_lines = pll_info_compact->frame_length_lines;
	cis_data->line_length_pck = pll_info_compact->line_length_pck;
	cis_data->line_readOut_time = (u64)cis_data->line_length_pck * 1000
					* 1000 * 1000 / cis_data->pclk;
	cis_data->rolling_shutter_skew = (cis_data->cur_height - 1) * cis_data->line_readOut_time;
	cis_data->stream_on = false;

	/* Frame valid time calcuration */
	frame_valid_us = (u64)cis_data->cur_height * cis_data->line_length_pck
				* 1000 * 1000 / cis_data->pclk;
	cis_data->frame_valid_us_time = (unsigned int)frame_valid_us;

	dbg_sensor(1, "%s\n", __func__);
	dbg_sensor(1, "Sensor size(%d x %d) setting: SUCCESS!\n",
					cis_data->cur_width, cis_data->cur_height);
	dbg_sensor(1, "Frame Valid(us): %d\n", frame_valid_us);
	dbg_sensor(1, "rolling_shutter_skew: %lld\n", cis_data->rolling_shutter_skew);

	dbg_sensor(1, "Fps: %d, max fps(%d)\n", frame_rate, cis_data->max_fps);
	dbg_sensor(1, "min_frame_time(%d us)\n", cis_data->min_frame_us_time);
	dbg_sensor(1, "Pixel rate(Kbps): %d\n", cis_data->pclk / 1000);

	/* Frame period calculation */
	cis_data->frame_time = (cis_data->line_readOut_time * cis_data->cur_height / 1000);
	cis_data->rolling_shutter_skew = (cis_data->cur_height - 1) * cis_data->line_readOut_time;

	dbg_sensor(1, "[%s] frame_time(%d), rolling_shutter_skew(%lld)\n", __func__,
		cis_data->frame_time, cis_data->rolling_shutter_skew);

	/* Constant values */
	cis_data->min_fine_integration_time = SENSOR_GH1_FINE_INTEGRATION_TIME_MIN;
	cis_data->max_fine_integration_time = SENSOR_GH1_FINE_INTEGRATION_TIME_MAX;
	cis_data->min_coarse_integration_time = SENSOR_GH1_COARSE_INTEGRATION_TIME_MIN;
	cis_data->max_margin_coarse_integration_time = SENSOR_GH1_COARSE_INTEGRATION_TIME_MAX_MARGIN;
}

static int sensor_gh1_wait_stream_off_status(cis_shared_data *cis_data)
{
	int ret = 0;
	u32 timeout = 0;

	FIMC_BUG(!cis_data);

#define STREAM_OFF_WAIT_TIME 250
	while (timeout < STREAM_OFF_WAIT_TIME) {
		if (cis_data->is_active_area == false &&
				cis_data->stream_on == false) {
	}
		timeout++;
	}

	if (timeout == STREAM_OFF_WAIT_TIME) {
		pr_err("actual stream off wait timeout\n");
		ret = -1;
	}

	return ret;
}

/* CIS OPS */
int sensor_gh1_cis_init(struct v4l2_subdev *subdev)
{
	int ret = 0;
	struct is_cis *cis;
	u32 setfile_index = 0;
	cis_setting_info setinfo;
	setinfo.param = NULL;
	setinfo.return_value = 0;

	FIMC_BUG(!subdev);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);
	if (!cis) {
		err("cis is NULL");
		ret = -EINVAL;
		goto p_err;
	}

	FIMC_BUG(!cis->cis_data);
#if !defined(CONFIG_CAMERA_VENDER_MCD)
	memset(cis->cis_data, 0, sizeof(cis_shared_data));

	ret = sensor_cis_check_rev(cis);
	if (ret < 0) {
		warn("sensor_gh1_check_rev is fail when cis init, ret(%d)", ret);
		ret = -EINVAL;
		goto p_err;
	}
#endif

	cis->cis_data->product_name = cis->id;
	cis->cis_data->cur_width = SENSOR_GH1_MAX_WIDTH;
	cis->cis_data->cur_height = SENSOR_GH1_MAX_HEIGHT;
	cis->cis_data->low_expo_start = 33000;
	cis->need_mode_change = false;
	cis->mipi_clock_index_cur = CAM_MIPI_NOT_INITIALIZED;
	cis->mipi_clock_index_new = CAM_MIPI_NOT_INITIALIZED;
	cis->cis_data->sens_config_index_pre = SENSOR_GH1_MODE_MAX;
	cis->cis_data->cur_pattern_mode = SENSOR_TEST_PATTERN_MODE_OFF;
	cis->long_term_mode.sen_strm_off_on_step = 0;
	cis->long_term_mode.sen_strm_off_on_enable = false;

	sensor_gh1_cis_data_calculation(sensor_gh1_pllinfos[setfile_index], cis->cis_data);

	setinfo.return_value = 0;
	CALL_CISOPS(cis, cis_get_min_exposure_time, subdev, &setinfo.return_value);
	dbg_sensor(1, "[%s] min exposure time : %d\n", __func__, setinfo.return_value);
	setinfo.return_value = 0;
	CALL_CISOPS(cis, cis_get_max_exposure_time, subdev, &setinfo.return_value);
	dbg_sensor(1, "[%s] max exposure time : %d\n", __func__, setinfo.return_value);
	setinfo.return_value = 0;
	CALL_CISOPS(cis, cis_get_min_analog_gain, subdev, &setinfo.return_value);
	dbg_sensor(1, "[%s] min again : %d\n", __func__, setinfo.return_value);
	setinfo.return_value = 0;
	CALL_CISOPS(cis, cis_get_max_analog_gain, subdev, &setinfo.return_value);
	dbg_sensor(1, "[%s] max again : %d\n", __func__, setinfo.return_value);
	setinfo.return_value = 0;
	CALL_CISOPS(cis, cis_get_min_digital_gain, subdev, &setinfo.return_value);
	dbg_sensor(1, "[%s] min dgain : %d\n", __func__, setinfo.return_value);
	setinfo.return_value = 0;
	CALL_CISOPS(cis, cis_get_max_digital_gain, subdev, &setinfo.return_value);
	dbg_sensor(1, "[%s] max dgain : %d\n", __func__, setinfo.return_value);

	xtc_cal_first = true;
	xtc_skip = false;
#ifdef DEBUG_SENSOR_TIME
	do_gettimeofday(&end);
	dbg_sensor(1, "[%s] time %lu us\n", __func__, (end.tv_sec - st.tv_sec)*1000000 + (end.tv_usec - st.tv_usec));
#endif

p_err:
	return ret;
}

int sensor_gh1_cis_log_status(struct v4l2_subdev *subdev)
{
	int ret = 0;
	struct is_cis *cis;
	struct i2c_client *client = NULL;
	u8 data8 = 0;
	u16 data16 = 0;

	FIMC_BUG(!subdev);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);
	if (!cis) {
		err("cis is NULL");
		ret = -EINVAL;
		goto p_err;
	}

	client = cis->client;
	if (unlikely(!client)) {
		err("client is NULL");
		ret = -EINVAL;
		goto p_err;
	}

	I2C_MUTEX_LOCK(cis->i2c_lock);

	pr_info("[%s] *******************************\n", __func__);
	ret = is_sensor_read16(client, 0x0000, &data16);
	if (unlikely(!ret)) pr_info("model_id(0x%x)\n", data16);
	else goto i2c_err;
	ret = is_sensor_read16(client, 0x00DC, &data16);
	if (unlikely(!ret)) pr_info("0x00DC(0x%x)\n", data16);
	else goto i2c_err;
	ret = is_sensor_read16(client, 0x00EA, &data16);
	if (unlikely(!ret)) pr_info("0x00EA(0x%x)\n", data16);
	else goto i2c_err;
	ret = is_sensor_read8(client, 0x0002, &data8);
	if (unlikely(!ret)) pr_info("revision_number(0x%x)\n", data8);
	else goto i2c_err;
	ret = is_sensor_read8(client, 0x0005, &data8);
	if (unlikely(!ret)) pr_info("frame_count(0x%x)\n", data8);
	else goto i2c_err;
	ret = is_sensor_read8(client, 0x0100, &data8);
	if (unlikely(!ret)) pr_info("0x0100(0x%x)\n", data8);
	else goto i2c_err;
	ret = is_sensor_read16(client, 0x0202, &data16);
	if (unlikely(!ret)) pr_info("0x0202(0x%x)\n", data16);
	else goto i2c_err;
	ret = is_sensor_read16(client, 0x0204, &data16);
	if (unlikely(!ret)) pr_info("0x0204(0x%x)\n", data16);
	else goto i2c_err;
	ret = is_sensor_read16(client, 0x0340, &data16);
	if (unlikely(!ret)) pr_info("0x0340(0x%x)\n", data16);
	else goto i2c_err;
	pr_info("[%s] *******************************\n", __func__);

i2c_err:
	I2C_MUTEX_UNLOCK(cis->i2c_lock);
p_err:
	return ret;
}

#if USE_GROUP_PARAM_HOLD
static int sensor_gh1_cis_group_param_hold_func(struct v4l2_subdev *subdev, unsigned int hold)
{
	int ret = 0;
	struct is_cis *cis = NULL;
	struct i2c_client *client = NULL;

	FIMC_BUG(!subdev);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	FIMC_BUG(!cis);
	FIMC_BUG(!cis->cis_data);

	client = cis->client;
	if (unlikely(!client)) {
		err("client is NULL");
		ret = -EINVAL;
		goto p_err;
	}

	if (hold == cis->cis_data->group_param_hold) {
		pr_debug("already group_param_hold (%d)\n", cis->cis_data->group_param_hold);
		goto p_err;
	}

	ret = is_sensor_write8(client, 0x0104, hold);
	if (ret < 0)
		goto p_err;

	cis->cis_data->group_param_hold = hold;
	ret = 1;
p_err:
	return ret;
}
#else
static inline int sensor_gh1_cis_group_param_hold_func(struct v4l2_subdev *subdev, unsigned int hold)
{ return 0; }
#endif

/* Input
 *	hold : true - hold, flase - no hold
 * Output
 *      return: 0 - no effect(already hold or no hold)
 *		positive - setted by request
 *		negative - ERROR value
 */
int sensor_gh1_cis_group_param_hold(struct v4l2_subdev *subdev, bool hold)
{
	int ret = 0;
	struct is_cis *cis = NULL;

	FIMC_BUG(!subdev);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	FIMC_BUG(!cis);
	FIMC_BUG(!cis->cis_data);

	I2C_MUTEX_LOCK(cis->i2c_lock);
	ret = sensor_gh1_cis_group_param_hold_func(subdev, hold);
	if (ret < 0)
		goto p_err;

p_err:
	I2C_MUTEX_UNLOCK(cis->i2c_lock);
	return ret;
}

int sensor_gh1_write16_burst(struct i2c_client *client,
	u16 addr, u8 *val, u32 num, bool endian)
{
	int ret = 0;
	struct i2c_msg msg[1];
	int i = 0;
	u8 *wbuf;

	if (val == NULL) {
		pr_err("val array is null\n");
		ret = -ENODEV;
		goto p_err;
	}

	if (!client->adapter) {
		pr_err("Could not find adapter!\n");
		ret = -ENODEV;
		goto p_err;
	}

	wbuf = kmalloc((2 + (num * 2)), GFP_KERNEL);
	if (!wbuf) {
		pr_err("failed to alloc buffer for burst i2c\n");
		ret = -ENODEV;
		goto p_err;
	}

	msg->addr = client->addr;
	msg->flags = 0;
	msg->len = 2 + (num * 2);
	msg->buf = wbuf;
	wbuf[0] = (addr & 0xFF00) >> 8;
	wbuf[1] = (addr & 0xFF);
	if (endian == GH1_BIG_ENDIAN) {
		for (i = 0; i < num; i++) {
			wbuf[(i * 2) + 2] = (val[(i * 2)] & 0xFF);
			wbuf[(i * 2) + 3] = (val[(i * 2) + 1] & 0xFF);
		}
	} else {
		for (i = 0; i < num; i++) {
			wbuf[(i * 2) + 2] = (val[(i * 2) + 1] & 0xFF);
			wbuf[(i * 2) + 3] = (val[(i * 2)] & 0xFF);
		}
	}

	ret = is_i2c_transfer(client->adapter, msg, 1);
	if (ret < 0) {
		pr_err("i2c transfer fail(%d)", ret);
		goto p_err_free;
	}

	kfree(wbuf);
	return 0;

p_err_free:
	kfree(wbuf);
p_err:
	return ret;
}


int sensor_gh1_cis_set_pdxtc_calibration(struct is_cis *cis)
{
	int ret = 0, i = 0, cal_size[2] = { 0 };
	char *cal_buf;
	u8* cal_data;
	struct is_rom_info *finfo = NULL;

	is_sec_get_cal_buf(&cal_buf, ROM_ID_FRONT);
	is_sec_get_sysfs_finfo(&finfo, ROM_ID_FRONT);
#ifdef CAMERA_GH1_CAL_MODULE_VERSION
	info("[%s] cur_header : %s", __func__, finfo->header_ver);
	if (finfo->header_ver[10] < CAMERA_GH1_CAL_MODULE_VERSION)
#endif
	{
		info("[%s] skip calibration, cal value not valid", __func__);
		return 0;
	}
	if (finfo->rom_pdxtc_cal_data_start_addr == -1) {
		err("[%s] cal addr empty");
		goto p_err;
	} else {
		cal_data = &cal_buf[finfo->rom_pdxtc_cal_data_start_addr];
	}

	cal_size[0] = finfo->rom_pdxtc_cal_data_0_size;
	if (cal_size[0] == -1) {
		err("[%s] cal_size0 empty");
		goto p_err;
	}
	cal_size[1] = finfo->rom_pdxtc_cal_data_1_size;
	if (cal_size[1] == -1) {
		err("[%s] cal_size1 empty");
		goto p_err;
	}

	ret = is_sensor_write16(cis->client, 0xFCFC, 0x4000);		/* default page setting */
/* PDXTC */
	ret = is_sensor_write16(cis->client, 0x6028, 0x2001);
	for (i = 0; i < cal_size[0]; i += 2) {
		u16 val = (cal_data[i] & 0xFF) << 8;
		ret |= is_sensor_write16(cis->client, 0x602A, 0x585A + i);
		if (i + 1 < cal_size[0]) {
			val |= (cal_data[i + 1] & 0xFF);
		}
		ret |= is_sensor_write16(cis->client, 0x6F12, val);
	}
#ifdef GH1_NON_BURST
	ret |= is_sensor_write16(cis->client, 0x602A, 0x5878);
	for (i = 0; i < cal_size[1]; i += 2) {
		u16 val = (cal_data[i + cal_size[0]] & 0xFF) << 8;
		val |= (cal_data[i + cal_size[0] + 1] & 0xFF);
		ret |= is_sensor_write16(cis->client, 0x6F12, val);
	}
#else
	ret |= is_sensor_write16(cis->client, 0x6004, 0x0001); /* burst enable */
	ret |= is_sensor_write16(cis->client, 0x602A, 0x5878);
	ret |= sensor_gh1_write16_burst(cis->client, 0x6F12, (&cal_data[cal_size[0]]), cal_size[1] / 2 , GH1_BIG_ENDIAN);
	ret |= is_sensor_write16(cis->client, 0x6004, 0x0000); /* burst disable */
#endif
	if (ret < 0) {
		err("pdxtc_calibration fail!");
		goto p_err;
	}
	info("[%s] cal done", __func__);
p_err:
	return ret;
}

int sensor_gh1_cis_set_xtc_calibration(struct is_cis *cis)
{
	int ret = 0, cal_size = 0;
#ifdef GH1_NON_BURST
	int i = 0;
#endif
	char *cal_buf;
	u8* cal_data;
	struct is_rom_info *finfo = NULL;

	is_sec_get_cal_buf(&cal_buf, ROM_ID_FRONT);
	is_sec_get_sysfs_finfo(&finfo, ROM_ID_FRONT);

#ifdef CAMERA_GH1_CAL_MODULE_VERSION
	info("[%s] cur_header : %s", __func__, finfo->header_ver);
	if (finfo->header_ver[10] < CAMERA_GH1_CAL_MODULE_VERSION)
#endif
	{
		info("[%s] skip calibration, cal value not valid", __func__);
		xtc_skip = true;
		return 0;
	}

	if (finfo->rom_xtc_cal_data_start_addr == -1) {
		err("[%s] cal_addr empty");
		goto p_err;
	} else {
		cal_data = &cal_buf[finfo->rom_xtc_cal_data_start_addr];
	}

	cal_size = finfo->rom_xtc_cal_data_size;
	if (cal_size == -1) {
		err("[%s] cal_size empty");
		goto p_err;
	}

	ret = is_sensor_write16(cis->client, 0xFCFC, 0x4000);		/* default page setting */
/* Tetra XTC */
	ret |= sensor_cis_set_registers(cis->subdev, sensor_gh1_pre_xtc, ARRAY_SIZE(sensor_gh1_pre_xtc));
	if (ret < 0) {
		err("tetra xtc_calibration fail!");
		goto p_err;
	}

#ifdef GH1_NON_BURST
	ret |= is_sensor_write16(cis->client, 0x6028, 0x2001); /* cal page */
	for (i = 0; i < cal_size; i+=2) {
		u16 val = (cal_data[i] & 0xFF) << 8;
		ret |= is_sensor_write16(cis->client, 0x602A, 0x5D90 + i); /* cal addr */
		val |= cal_data[i + 1] & 0xFF;
		ret |= is_sensor_write16(cis->client, 0x6F12, val);
	}
#else
	ret |= is_sensor_write16(cis->client, 0x6004, 0x0001); /* burst enable */
	ret |= is_sensor_write16(cis->client, 0x6028, 0x2001); /* cal page */
	ret |= is_sensor_write16(cis->client, 0x602A, 0x5D90); /* cal addr */
	ret |= sensor_gh1_write16_burst(cis->client, 0x6F12, cal_data, cal_size / 2, GH1_BIG_ENDIAN);
	ret |= is_sensor_write16(cis->client, 0x6004, 0x0000); /* burst disable */
#endif
	if (ret < 0) {
		err("tetra xtc_calibration fail!");
		goto p_err;
	}
	info("[%s] cal done", __func__);
p_err:
	return ret;
}

int sensor_gh1_cis_set_global_setting(struct v4l2_subdev *subdev)
{
	int ret = 0;
	int i;
	struct is_cis *cis = NULL;
	struct i2c_client *client;
	struct is_core *core;
	u32 post_set_address;
	u32 post_set_value;

	FIMC_BUG(!subdev);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);
	FIMC_BUG(!cis);

	I2C_MUTEX_LOCK(cis->i2c_lock);

	client = cis->client;
	if (unlikely(!client)) {
		err("client is NULL");
		ret = -EINVAL;
		goto p_err;
	}

	core = (struct is_core *)dev_get_drvdata(is_dev);
	if (!core) {
		err("core is NULL");
		return -EINVAL;
	}

	/* setfile global setting is at camera entrance */
	info("[%s] global setting enter\n", __func__);
	if (core->scenario == IS_SCENARIO_SECURE){
		info("[%s] sensor_gh1 FD mode setting\n", __func__);
		ret = sensor_cis_set_registers(subdev, sensor_gh1_global_secure, sensor_gh1_global_secure_size);
	} else {
		ret = sensor_cis_set_registers(subdev, sensor_gh1_global, sensor_gh1_global_size);
#ifndef PDXTC_CAL_DISABLE
		ret |= sensor_gh1_cis_set_pdxtc_calibration(cis);
#endif
#if defined(GLOBAL_TIME_CAL_WRITE) && !defined(XTC_CAL_DISABLE)
		ret |= sensor_gh1_cis_set_xtc_calibration(cis);
#endif
	}

	if (ret < 0) {
		err("sensor_gh1_set_registers fail!!");
		goto p_err;
	}
	info("[%s] global setting done\n", __func__);

	for (i = 0; i < sensor_gh1_post_init_setting_list_len; i += 2) {
		post_set_address = sensor_gh1_post_init_setting_list[i];
		post_set_value = sensor_gh1_post_init_setting_list[i+1];

		ret = is_sensor_write16(client, post_set_address, post_set_value);
		if (ret < 0)
			goto p_err;
	}

p_err:
	I2C_MUTEX_UNLOCK(cis->i2c_lock);

	return ret;
}

static void sensor_gh1_cis_set_paf_stat_enable(u32 mode, cis_shared_data *cis_data)
{
	WARN_ON(!cis_data);

	switch (mode) {
	case SENSOR_GH1_3648X2736_30FPS:
	case SENSOR_GH1_3968X2232_30FPS:
	case SENSOR_GH1_1824X1368_30FPS:
	case SENSOR_GH1_1984X1116_30FPS:
	case SENSOR_GH1_2944x2208_30FPS:
	case SENSOR_GH1_3216x1808_30FPS:
		cis_data->is_data.paf_stat_enable = true;
		break;
	default:
		cis_data->is_data.paf_stat_enable = false;
		break;
	}
}

int sensor_gh1_cis_mode_change(struct v4l2_subdev *subdev, u32 mode)
{
	int ret = 0;
	struct is_cis *cis = NULL;
	struct is_device_sensor *device;
	struct is_device_sensor_peri *sensor_peri;
	struct is_core *core;
	u32 ex_mode;

	FIMC_BUG(!subdev);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);
	FIMC_BUG(!cis);
	FIMC_BUG(!cis->cis_data);

	device = (struct is_device_sensor *)v4l2_get_subdev_hostdata(subdev);
	if (unlikely(!device)) {
		err("device sensor is null");
		return -EINVAL;
	}

	core = (struct is_core *)dev_get_drvdata(is_dev);
	if (!core) {
		err("core is NULL");
		return -EINVAL;
	}

	sensor_peri = find_peri_by_cis_id(device, cis->id);
	if (unlikely(!sensor_peri)) {
		err("sensor peri is NULL");
		return -EINVAL;
	}

	if (mode >= sensor_gh1_max_setfile_num) {
		err("invalid mode(%d)!!", mode);
		ret = -EINVAL;
		goto p_err;
	}

	sensor_gh1_cis_data_calculation(sensor_gh1_pllinfos[mode], cis->cis_data);

	cis->mipi_clock_index_cur = CAM_MIPI_NOT_INITIALIZED;

	sensor_gh1_cis_set_paf_stat_enable(mode, cis->cis_data);

	I2C_MUTEX_LOCK(cis->i2c_lock);

	ex_mode = is_sensor_g_ex_mode(device);

	if (core->scenario != IS_SCENARIO_SECURE){
		if (cis->cis_data->sens_config_index_pre == SENSOR_GH1_912X684_120FPS
			&& ex_mode != EX_REMOSAIC_CAL) {
			switch (mode) {
			case SENSOR_GH1_3648X2736_30FPS:
				info("[%s] SENSOR_GH1_3648X2736_30FPS_SHORT\n", __func__);
				mode = SENSOR_GH1_3648X2736_30FPS_SHORT;
				break;
			case SENSOR_GH1_3968X2232_30FPS:
				info("[%s] SENSOR_GH1_3968X2232_30FPS_SHORT\n", __func__);
				mode = SENSOR_GH1_3968X2232_30FPS_SHORT;
				break;
			}
		}
		ret = sensor_cis_set_registers(subdev, sensor_gh1_setfiles[mode], sensor_gh1_setfile_sizes[mode]);
		if (ret < 0) {
			err("sensor_gh1_set_registers fail!!");
			goto p_err_i2c_unlock;
		}
		cis->cis_data->sens_config_index_pre = mode;

#if !defined(GLOBAL_TIME_CAL_WRITE)
#if !defined(XTC_CAL_DISABLE)
		if (xtc_cal_first && is_sensor_g_ex_mode(device) == EX_REMOSAIC_CAL) {
			ret = sensor_gh1_cis_set_xtc_calibration(cis);
			if (ret < 0) {
				err("set xtc calibration failed!!");
				goto p_err_i2c_unlock;
			}
			if (!xtc_skip) {
				info("[%s] decomp cal!", __func__);
				ret = is_sensor_write16(cis->client, 0x6028, 0x2000); /* cal page */
				ret |= is_sensor_write16(cis->client, 0x602A, 0x31B0); /* cal addr */
				ret |= is_sensor_write16(cis->client, 0x6F12, 0x4000);
			}
			xtc_cal_first = false;
		}
		if (xtc_skip && (mode == SENSOR_GH1_7296X5472_15FPS || mode == SENSOR_GH1_7936x4464_15FPS)) {
			info("[%s] bypass xtc calibration",__func__);
			/* XTC disable register : \x 2000 42C0 */
			ret = is_sensor_write16(cis->client, 0x6028, 0x2000);
			ret |= is_sensor_write16(cis->client, 0x602A, 0x42C0);
			ret |= is_sensor_write16(cis->client, 0x6F12, 0x0001); /* bypass enable */
		}
#else
		ret = is_sensor_write16(cis->client, 0x6028, 0x2000);
		ret |= is_sensor_write16(cis->client, 0x602A, 0x42C0);
		ret |= is_sensor_write16(cis->client, 0x6F12, 0x0001); /* bypass enable */
#endif
#endif
	}

#if 1 /* TEMP_OLYMPUS */
	/* EMB Header off */
	ret = is_sensor_write8(cis->client, 0x0118, 0x00);
	if (ret < 0){
		err("EMB header off fail");
	}
#endif

	info("[%s] mode changed(%d)\n", __func__, mode);

p_err_i2c_unlock:
	I2C_MUTEX_UNLOCK(cis->i2c_lock);

p_err:
	/* make first frame to 24fps for live focus sync issue */
	if (device->image.framerate == 24) {
		sensor_gh1_cis_set_frame_rate(subdev, 24);
	}

	return ret;
}

/* TODO: Sensor set size sequence(sensor done, sensor stop, 3AA done in FW case */
int sensor_gh1_cis_set_size(struct v4l2_subdev *subdev, cis_shared_data *cis_data)
{
	int ret = 0;
	bool binning = false;
	u32 ratio_w = 0, ratio_h = 0, start_x = 0, start_y = 0, end_x = 0, end_y = 0;
	u32 even_x= 0, odd_x = 0, even_y = 0, odd_y = 0;
	struct i2c_client *client = NULL;
	struct is_cis *cis = NULL;
#ifdef DEBUG_SENSOR_TIME
	struct timeval st, end;
	do_gettimeofday(&st);
#endif
	FIMC_BUG(!subdev);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);
	FIMC_BUG(!cis);

	dbg_sensor(1, "[MOD:D:%d] %s\n", cis->id, __func__);

	if (unlikely(!cis_data)) {
		err("cis data is NULL");
		if (unlikely(!cis->cis_data)) {
			ret = -EINVAL;
			goto p_err;
		} else {
			cis_data = cis->cis_data;
		}
	}

	client = cis->client;
	if (unlikely(!client)) {
		err("client is NULL");
		ret = -EINVAL;
		goto p_err;
	}

	/* Wait actual stream off */
	ret = sensor_gh1_wait_stream_off_status(cis_data);
	if (ret) {
		err("Must stream off\n");
		ret = -EINVAL;
		goto p_err;
	}

	binning = cis_data->binning;
	if (binning) {
		ratio_w = (SENSOR_GH1_MAX_WIDTH / cis_data->cur_width);
		ratio_h = (SENSOR_GH1_MAX_HEIGHT / cis_data->cur_height);
	} else {
		ratio_w = 1;
		ratio_h = 1;
	}

	if (((cis_data->cur_width * ratio_w) > SENSOR_GH1_MAX_WIDTH) ||
		((cis_data->cur_height * ratio_h) > SENSOR_GH1_MAX_HEIGHT)) {
		err("Config max sensor size over~!!\n");
		ret = -EINVAL;
		goto p_err;
	}

	I2C_MUTEX_LOCK(cis->i2c_lock);
	/* 1. page_select */
	ret = is_sensor_write16(client, 0x6028, 0x2000);
	if (ret < 0)
		 goto p_err_i2c_unlock;

	/* 2. pixel address region setting */
	start_x = ((SENSOR_GH1_MAX_WIDTH - cis_data->cur_width * ratio_w) / 2) & (~0x1);
	start_y = ((SENSOR_GH1_MAX_HEIGHT - cis_data->cur_height * ratio_h) / 2) & (~0x1);
	end_x = start_x + (cis_data->cur_width * ratio_w - 1);
	end_y = start_y + (cis_data->cur_height * ratio_h - 1);

	if (!(end_x & (0x1)) || !(end_y & (0x1))) {
		err("Sensor pixel end address must odd\n");
		ret = -EINVAL;
		goto p_err_i2c_unlock;
	}

	ret = is_sensor_write16(client, 0x0344, start_x);
	if (ret < 0)
		 goto p_err_i2c_unlock;
	ret = is_sensor_write16(client, 0x0346, start_y);
	if (ret < 0)
		 goto p_err_i2c_unlock;
	ret = is_sensor_write16(client, 0x0348, end_x);
	if (ret < 0)
		 goto p_err_i2c_unlock;
	ret = is_sensor_write16(client, 0x034A, end_y);
	if (ret < 0)
		 goto p_err_i2c_unlock;

	/* 3. output address setting */
	ret = is_sensor_write16(client, 0x034C, cis_data->cur_width);
	if (ret < 0)
		 goto p_err_i2c_unlock;
	ret = is_sensor_write16(client, 0x034E, cis_data->cur_height);
	if (ret < 0)
		 goto p_err_i2c_unlock;

	/* If not use to binning, sensor image should set only crop */
	if (!binning) {
		dbg_sensor(1, "Sensor size set is not binning\n");
		goto p_err_i2c_unlock;
	}

	/* 4. sub sampling setting */
	even_x = 1;	/* 1: not use to even sampling */
	even_y = 1;
	odd_x = (ratio_w * 2) - even_x;
	odd_y = (ratio_h * 2) - even_y;

	ret = is_sensor_write16(client, 0x0380, even_x);
	if (ret < 0)
		 goto p_err_i2c_unlock;
	ret = is_sensor_write16(client, 0x0382, odd_x);
	if (ret < 0)
		 goto p_err_i2c_unlock;
	ret = is_sensor_write16(client, 0x0384, even_y);
	if (ret < 0)
		 goto p_err_i2c_unlock;
	ret = is_sensor_write16(client, 0x0386, odd_y);
	if (ret < 0)
		 goto p_err_i2c_unlock;

	/* 5. binnig setting */
	ret = is_sensor_write8(client, 0x0900, binning);	/* 1:  binning enable, 0: disable */
	if (ret < 0)
		goto p_err_i2c_unlock;
	ret = is_sensor_write8(client, 0x0901, (ratio_w << 4) | ratio_h);
	if (ret < 0)
		goto p_err_i2c_unlock;

	/* 6. scaling setting: but not use */
	/* scaling_mode (0: No scaling, 1: Horizontal, 2: Full) */
	ret = is_sensor_write16(client, 0x0400, 0x0000);
	if (ret < 0)
		goto p_err_i2c_unlock;
	/* down_scale_m: 1 to 16 upwards (scale_n: 16(fixed))
	down scale factor = down_scale_m / down_scale_n */
	ret = is_sensor_write16(client, 0x0404, 0x0010);
	if (ret < 0)
		goto p_err_i2c_unlock;

	cis_data->frame_time = (cis_data->line_readOut_time * cis_data->cur_height / 1000);
	cis->cis_data->rolling_shutter_skew = (cis->cis_data->cur_height - 1) * cis->cis_data->line_readOut_time;
	dbg_sensor(1, "[%s] frame_time(%d), rolling_shutter_skew(%lld)\n", __func__,
		cis->cis_data->frame_time, cis->cis_data->rolling_shutter_skew);

#ifdef DEBUG_SENSOR_TIME
	do_gettimeofday(&end);
	dbg_sensor(1, "[%s] time %lu us\n", __func__, (end.tv_sec - st.tv_sec) * 1000000 + (end.tv_usec - st.tv_usec));
#endif

p_err_i2c_unlock:
	I2C_MUTEX_UNLOCK(cis->i2c_lock);

p_err:
	return ret;
}

int sensor_gh1_cis_long_term_exposure(struct v4l2_subdev *subdev)
{
	int ret = 0;
	struct is_cis *cis;
	struct is_long_term_expo_mode *lte_mode;
	unsigned char shift_count = 0;

	FIMC_BUG(!subdev);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);
	lte_mode = &cis->long_term_mode;

	I2C_MUTEX_LOCK(cis->i2c_lock);
	/* LTE mode or normal mode set */
	if (lte_mode->sen_strm_off_on_enable) {
		shift_count = sensor_cis_get_duration_shifter(cis, lte_mode->expo[0]);
		if (shift_count > 0) {
			lte_mode->expo[0] >>= shift_count;

			ret |= is_sensor_write16(cis->client, 0xFCFC, 0x4000);
			ret |= is_sensor_write8(cis->client, 0x0702, shift_count);
			ret |= is_sensor_write8(cis->client, 0x0704, shift_count);
		}
	} else {
		ret |= is_sensor_write16(cis->client, 0xFCFC, 0x4000);
		ret |= is_sensor_write8(cis->client, 0x0702, 0x00);
		ret |= is_sensor_write8(cis->client, 0x0704, 0x00);
	}

	I2C_MUTEX_UNLOCK(cis->i2c_lock);

	info("%s enable(%d) shift_count(%d) exp(%d)",
		__func__, lte_mode->sen_strm_off_on_enable, shift_count, lte_mode->expo[0]);

	if (ret < 0) {
		pr_err("ERR[%s]: LTE register setting fail\n", __func__);
		return ret;
	}

	return ret;
}

int sensor_gh1_cis_stream_on(struct v4l2_subdev *subdev)
{
	int ret = 0;
	struct is_cis *cis;
	struct i2c_client *client;
	struct is_device_sensor *device;
	cis_shared_data *cis_data;

#ifdef DEBUG_SENSOR_TIME
	struct timeval st, end;
	do_gettimeofday(&st);
#endif

	FIMC_BUG(!subdev);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	FIMC_BUG(!cis);
	FIMC_BUG(!cis->cis_data);

	client = cis->client;
	if (unlikely(!client)) {
		err("client is NULL");
		ret = -EINVAL;
		goto p_err;
	}

	device = (struct is_device_sensor *)v4l2_get_subdev_hostdata(subdev);
	if (unlikely(!device)) {
		err("device sensor is null");
		return -EINVAL;
	}

	cis_data = cis->cis_data;

	dbg_sensor(1, "[MOD:D:%d] %s\n", cis->id, __func__);

	is_vendor_set_mipi_clock(device);

	I2C_MUTEX_LOCK(cis->i2c_lock);

	ret = sensor_gh1_cis_group_param_hold_func(subdev, 0x00);
	if (ret < 0)
		err("group_param_hold_func failed at stream on");

	usleep_range(8000, 8100);

	/* Sensor stream on */
	is_sensor_write8(client, 0x0100, 0x01);

	if (sensor_gh1_cis_is_wdr_mode_on(cis_data))
		is_sensor_write8(client, 0x021E, 0x01);
	else
		is_sensor_write8(client, 0x021E, 0x00);

	info("%s\n", __func__);

	cis_data->stream_on = true;

#ifdef DEBUG_SENSOR_TIME
	do_gettimeofday(&end);
	dbg_sensor(1, "[%s] time %lu us\n", __func__, (end.tv_sec - st.tv_sec)*1000000 + (end.tv_usec - st.tv_usec));
#endif

	I2C_MUTEX_UNLOCK(cis->i2c_lock);

p_err:
	return ret;
}

int sensor_gh1_cis_stream_off(struct v4l2_subdev *subdev)
{
	int ret = 0;
	struct is_cis *cis;
	struct i2c_client *client;
	cis_shared_data *cis_data;

#ifdef DEBUG_SENSOR_TIME
	struct timeval st, end;
	do_gettimeofday(&st);
#endif

	FIMC_BUG(!subdev);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	FIMC_BUG(!cis);
	FIMC_BUG(!cis->cis_data);

	client = cis->client;
	if (unlikely(!client)) {
		err("client is NULL");
		ret = -EINVAL;
		goto p_err;
	}

	cis_data = cis->cis_data;

	dbg_sensor(1, "[MOD:D:%d] %s\n", cis->id, __func__);

	I2C_MUTEX_LOCK(cis->i2c_lock);
	ret = sensor_gh1_cis_group_param_hold_func(subdev, 0x00);
	if (ret < 0)
		err("group_param_hold_func failed at stream off");

	/* Sensor stream off */
	is_sensor_write8(client, 0x0100, 0x00);

	info("%s\n", __func__);

	cis_data->stream_on = false;

#ifdef DEBUG_SENSOR_TIME
	do_gettimeofday(&end);
	dbg_sensor(1, "[%s] time %lu us\n", __func__, (end.tv_sec - st.tv_sec)*1000000 + (end.tv_usec - st.tv_usec));
#endif

	I2C_MUTEX_UNLOCK(cis->i2c_lock);

p_err:
	return ret;
}

int sensor_gh1_cis_set_exposure_time(struct v4l2_subdev *subdev, struct ae_param *target_exposure)
{
	int ret = 0;
	int hold = 0;
	struct is_cis *cis;
	struct i2c_client *client;
	cis_shared_data *cis_data;

	u64 vt_pic_clk_freq_khz = 0;
	u16 long_coarse_int = 0;
	u16 short_coarse_int = 0;
	u32 line_length_pck = 0;
	u32 min_fine_int = 0;
	u32 input_duration;
	u8 cit_shifter = 0;

#ifdef DEBUG_SENSOR_TIME
	struct timeval st, end;
	do_gettimeofday(&st);
#endif

	FIMC_BUG(!subdev);
	FIMC_BUG(!target_exposure);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	FIMC_BUG(!cis);
	FIMC_BUG(!cis->cis_data);

	client = cis->client;
	if (unlikely(!client)) {
		err("client is NULL");
		ret = -EINVAL;
		goto p_err;
	}

	if ((target_exposure->long_val <= 0) || (target_exposure->short_val <= 0)) {
		err("[%s] invalid target exposure(%d, %d)\n", __func__,
				target_exposure->long_val, target_exposure->short_val);
		ret = -EINVAL;
		goto p_err;
	}

	cis_data = cis->cis_data;

	if (cis->long_term_mode.sen_strm_off_on_enable == false) {
		input_duration = MAX(target_exposure->long_val, target_exposure->short_val);
		cit_shifter = sensor_cis_get_duration_shifter(cis, input_duration);

		cit_shifter = MAX(cit_shifter, cis_data->frame_length_lines_shifter);
		target_exposure->long_val >>= cit_shifter;
		target_exposure->short_val >>= cit_shifter;
	}

	dbg_sensor(1, "[MOD:D:%d] %s, vsync_cnt(%d), target long(%d), short(%d)\n", cis->id, __func__,
			cis_data->sen_vsync_count, target_exposure->long_val, target_exposure->short_val);

	vt_pic_clk_freq_khz = cis_data->pclk / 1000;
	line_length_pck = cis_data->line_length_pck;
	min_fine_int = cis_data->min_fine_integration_time;

	long_coarse_int = ((target_exposure->long_val * vt_pic_clk_freq_khz) / 1000 - min_fine_int) / line_length_pck;
	short_coarse_int = ((target_exposure->short_val * vt_pic_clk_freq_khz) / 1000 - min_fine_int) / line_length_pck;

	if (long_coarse_int > cis_data->max_coarse_integration_time) {
		long_coarse_int = cis_data->max_coarse_integration_time;
		dbg_sensor(1, "[MOD:D:%d] %s, vsync_cnt(%d), long coarse(%d) max\n", cis->id, __func__,
			cis_data->sen_vsync_count, long_coarse_int);
	}

	if (short_coarse_int > cis_data->max_coarse_integration_time) {
		short_coarse_int = cis_data->max_coarse_integration_time;
		dbg_sensor(1, "[MOD:D:%d] %s, vsync_cnt(%d), short coarse(%d) max\n", cis->id, __func__,
			cis_data->sen_vsync_count, short_coarse_int);
	}

	if (long_coarse_int < cis_data->min_coarse_integration_time) {
		long_coarse_int = cis_data->min_coarse_integration_time;
		dbg_sensor(1, "[MOD:D:%d] %s, vsync_cnt(%d), long coarse(%d) min\n", cis->id, __func__,
			cis_data->sen_vsync_count, long_coarse_int);
	}

	if (short_coarse_int < cis_data->min_coarse_integration_time) {
		short_coarse_int = cis_data->min_coarse_integration_time;
		dbg_sensor(1, "[MOD:D:%d] %s, vsync_cnt(%d), short coarse(%d) min\n", cis->id, __func__,
			cis_data->sen_vsync_count, short_coarse_int);
	}

	if (short_coarse_int > long_coarse_int) {
		dbg_sensor(1, "[MOD:D:%d] %s, long coarse(%d), short coarse(%d) max\n", cis->id, __func__,
			long_coarse_int, short_coarse_int);
		long_coarse_int = short_coarse_int;
	}

	cis_data->cur_long_exposure_coarse = long_coarse_int;
	cis_data->cur_short_exposure_coarse = short_coarse_int;

	I2C_MUTEX_LOCK(cis->i2c_lock);
	hold = sensor_gh1_cis_group_param_hold_func(subdev, 0x01);
	if (hold < 0) {
		ret = hold;
		goto p_err_i2c_unlock;
	}

	/* WDR mode */
	if (sensor_gh1_cis_is_wdr_mode_on(cis_data)) {
		is_sensor_write8(client, 0x021E, 0x01);
	} else {
		is_sensor_write8(client, 0x021E, 0x00);
	}

	/* Short exposure */
	ret = is_sensor_write16(client, 0x0202, short_coarse_int);
	if (ret < 0)
		goto p_err_i2c_unlock;

	/* Long exposure */
	if (sensor_gh1_cis_is_wdr_mode_on(cis_data)) {
		ret = is_sensor_write16(client, 0x0226, long_coarse_int);
		if (ret < 0)
			goto p_err_i2c_unlock;
	}

	/* CIT shifter */
	if (cis->long_term_mode.sen_strm_off_on_enable == false) {
		ret = is_sensor_write8(client, 0x0704, cit_shifter);
		if (ret < 0)
			goto p_err_i2c_unlock;
	}

	dbg_sensor(1, "[MOD:D:%d] %s, vsync_cnt(%d), vt_pic_clk_freq_khz (%llu),"
		KERN_CONT "line_length_pck(%d), min_fine_int (%d)\n", cis->id, __func__,
		cis_data->sen_vsync_count, vt_pic_clk_freq_khz, line_length_pck, min_fine_int);
	dbg_sensor(1, "[MOD:D:%d] %s, vsync_cnt(%d), frame_length_lines(%#x),"
		KERN_CONT "long_coarse_int %#x, short_coarse_int %#x cit_shifter %#x\n",
		cis->id, __func__, cis_data->sen_vsync_count, cis_data->frame_length_lines,
		long_coarse_int, short_coarse_int, cit_shifter);

#ifdef DEBUG_SENSOR_TIME
	do_gettimeofday(&end);
	dbg_sensor(1, "[%s] time %lu us\n", __func__, (end.tv_sec - st.tv_sec)*1000000 + (end.tv_usec - st.tv_usec));
#endif

p_err_i2c_unlock:
	if (hold > 0) {
		hold = sensor_gh1_cis_group_param_hold_func(subdev, 0x00);
		if (hold < 0)
			ret = hold;
	}
	I2C_MUTEX_UNLOCK(cis->i2c_lock);

p_err:
	return ret;
}

int sensor_gh1_cis_get_min_exposure_time(struct v4l2_subdev *subdev, u32 *min_expo)
{
	int ret = 0;
	struct is_cis *cis = NULL;
	cis_shared_data *cis_data = NULL;
	u32 min_integration_time = 0;
	u32 min_coarse = 0;
	u32 min_fine = 0;
	u64 vt_pic_clk_freq_khz = 0;
	u32 line_length_pck = 0;

#ifdef DEBUG_SENSOR_TIME
	struct timeval st, end;
	do_gettimeofday(&st);
#endif

	FIMC_BUG(!subdev);
	FIMC_BUG(!min_expo);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	FIMC_BUG(!cis);
	FIMC_BUG(!cis->cis_data);

	cis_data = cis->cis_data;

	vt_pic_clk_freq_khz = cis_data->pclk / 1000;
	if (vt_pic_clk_freq_khz == 0) {
		pr_err("[MOD:D:%d] %s, Invalid vt_pic_clk_freq_khz(%d)\n", cis->id, __func__, vt_pic_clk_freq_khz);
		goto p_err;
	}
	line_length_pck = cis_data->line_length_pck;
	min_coarse = cis_data->min_coarse_integration_time;
	min_fine = cis_data->min_fine_integration_time;

	min_integration_time = (u32)((u64)((line_length_pck * min_coarse) + min_fine) * 1000 / vt_pic_clk_freq_khz);
	*min_expo = min_integration_time;

	dbg_sensor(1, "[%s] min integration time %d\n", __func__, min_integration_time);

#ifdef DEBUG_SENSOR_TIME
	do_gettimeofday(&end);
	dbg_sensor(1, "[%s] time %lu us\n", __func__, (end.tv_sec - st.tv_sec)*1000000 + (end.tv_usec - st.tv_usec));
#endif

p_err:
	return ret;
}

int sensor_gh1_cis_get_max_exposure_time(struct v4l2_subdev *subdev, u32 *max_expo)
{
	int ret = 0;
	struct is_cis *cis;
	cis_shared_data *cis_data;
	u32 max_integration_time = 0;
	u32 max_coarse_margin = 0;
	u32 max_fine_margin = 0;
	u32 max_coarse = 0;
	u32 max_fine = 0;
	u64 vt_pic_clk_freq_khz = 0;
	u32 line_length_pck = 0;
	u32 frame_length_lines = 0;

#ifdef DEBUG_SENSOR_TIME
	struct timeval st, end;
	do_gettimeofday(&st);
#endif

	FIMC_BUG(!subdev);
	FIMC_BUG(!max_expo);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	FIMC_BUG(!cis);
	FIMC_BUG(!cis->cis_data);

	cis_data = cis->cis_data;

	vt_pic_clk_freq_khz = cis_data->pclk / 1000;
	if (vt_pic_clk_freq_khz == 0) {
		pr_err("[MOD:D:%d] %s, Invalid vt_pic_clk_freq_khz(%d)\n", cis->id, __func__, vt_pic_clk_freq_khz);
		goto p_err;
	}
	line_length_pck = cis_data->line_length_pck;
	frame_length_lines = cis_data->frame_length_lines;

	max_coarse_margin = cis_data->max_margin_coarse_integration_time;
	max_fine_margin = line_length_pck - cis_data->min_fine_integration_time;
	max_coarse = frame_length_lines - max_coarse_margin;
	max_fine = cis_data->max_fine_integration_time;

	max_integration_time = (u32)((u64)((line_length_pck * max_coarse) + max_fine) * 1000 / vt_pic_clk_freq_khz);

	*max_expo = max_integration_time;

	/* TODO: Is this values update hear? */
	cis_data->max_margin_fine_integration_time = max_fine_margin;
	cis_data->max_coarse_integration_time = max_coarse;

	dbg_sensor(1, "[%s] max integration time %d, max margin fine integration %d, max coarse integration %d\n",
			__func__, max_integration_time, cis_data->max_margin_fine_integration_time, cis_data->max_coarse_integration_time);

#ifdef DEBUG_SENSOR_TIME
	do_gettimeofday(&end);
	dbg_sensor(1, "[%s] time %lu us\n", __func__, (end.tv_sec - st.tv_sec)*1000000 + (end.tv_usec - st.tv_usec));
#endif

p_err:
	return ret;
}

int sensor_gh1_cis_adjust_frame_duration(struct v4l2_subdev *subdev,
						u32 input_exposure_time,
						u32 *target_duration)
{
	int ret = 0;
	struct is_cis *cis;
	cis_shared_data *cis_data;

	u64 vt_pic_clk_freq_khz = 0;
	u32 line_length_pck = 0;
	u32 frame_length_lines = 0;
	u32 frame_duration = 0;

#ifdef DEBUG_SENSOR_TIME
	struct timeval st, end;
	do_gettimeofday(&st);
#endif

	FIMC_BUG(!subdev);
	FIMC_BUG(!target_duration);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	FIMC_BUG(!cis);
	FIMC_BUG(!cis->cis_data);

	cis_data = cis->cis_data;

	if (input_exposure_time == 0) {
		input_exposure_time  = cis_data->min_frame_us_time;
		info("[%s] Not proper exposure time(0), so apply min frame duration to exposure time forcely!!!(%d)\n",
			__func__, cis_data->min_frame_us_time);
	}

	vt_pic_clk_freq_khz = cis_data->pclk / 1000;
	line_length_pck = cis_data->line_length_pck;
	frame_length_lines = (((vt_pic_clk_freq_khz * input_exposure_time) / 1000) / line_length_pck);
	frame_length_lines += cis_data->max_margin_coarse_integration_time;

	frame_duration = (u32)(((u64)frame_length_lines * line_length_pck) * 1000 / vt_pic_clk_freq_khz);

	dbg_sensor(1, "[%s] input exp(%d), adj duration, frame duraion(%d), min_frame_us(%d)\n",
			__func__, input_exposure_time, frame_duration, cis_data->min_frame_us_time);
	dbg_sensor(1, "[%s] adj duration, frame duraion(%d), min_frame_us(%d)\n",
			__func__, frame_duration, cis_data->min_frame_us_time);

	*target_duration = MAX(frame_duration, cis_data->min_frame_us_time);

#ifdef DEBUG_SENSOR_TIME
	do_gettimeofday(&end);
	dbg_sensor(1, "[%s] time %lu us\n", __func__, (end.tv_sec - st.tv_sec)*1000000 + (end.tv_usec - st.tv_usec));
#endif

	return ret;
}

int sensor_gh1_cis_set_frame_duration(struct v4l2_subdev *subdev, u32 frame_duration)
{
	int ret = 0;
	int hold = 0;
	struct is_cis *cis;
	struct i2c_client *client;
	cis_shared_data *cis_data;

	u64 vt_pic_clk_freq_khz = 0;
	u32 line_length_pck = 0;
	u16 frame_length_lines = 0;
	u8 fll_shifter = 0;

#ifdef DEBUG_SENSOR_TIME
	struct timeval st, end;
	do_gettimeofday(&st);
#endif

	FIMC_BUG(!subdev);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	FIMC_BUG(!cis);
	FIMC_BUG(!cis->cis_data);

	client = cis->client;
	if (unlikely(!client)) {
		err("client is NULL");
		ret = -EINVAL;
		goto p_err;
	}

	cis_data = cis->cis_data;

	if (frame_duration < cis_data->min_frame_us_time) {
		dbg_sensor(1, "frame duration is less than min(%d)\n", frame_duration);
		frame_duration = cis_data->min_frame_us_time;
	}

	if (cis->long_term_mode.sen_strm_off_on_enable == false) {
		fll_shifter = sensor_cis_get_duration_shifter(cis, frame_duration);
		frame_duration >>= fll_shifter;
	}
	vt_pic_clk_freq_khz = cis_data->pclk / 1000;
	line_length_pck = cis_data->line_length_pck;

	frame_length_lines = (u16)((vt_pic_clk_freq_khz * frame_duration) / (line_length_pck * 1000));

	dbg_sensor(1, "[MOD:D:%d] %s, vt_pic_clk_freq_khz(%llu)) frame_duration = %d us,"
			KERN_CONT "(line_length_pck%#x), frame_length_lines(%#x), fll_shifter(%#x)\n",
			cis->id, __func__, vt_pic_clk_freq_khz/1000, frame_duration,
			line_length_pck, frame_length_lines, fll_shifter);

	I2C_MUTEX_LOCK(cis->i2c_lock);
	hold = sensor_gh1_cis_group_param_hold_func(subdev, 0x01);
	if (hold < 0) {
		ret = hold;
		goto p_err_i2c_unlock;
	}

	ret = is_sensor_write16(client, 0x0340, frame_length_lines);
	if (ret < 0)
		goto p_err_i2c_unlock;

	cis_data->cur_frame_us_time = frame_duration;

	/* frame duration shifter */
	if (cis->long_term_mode.sen_strm_off_on_enable == false) {
		ret = is_sensor_write8(client, 0x0702, fll_shifter);
		if (ret < 0)
			goto p_err_i2c_unlock;
	}

	cis_data->frame_length_lines = frame_length_lines;
	cis_data->max_coarse_integration_time = cis_data->frame_length_lines - cis_data->max_margin_coarse_integration_time;
	cis_data->frame_length_lines_shifter = fll_shifter;

#ifdef DEBUG_SENSOR_TIME
	do_gettimeofday(&end);
	dbg_sensor(1, "[%s] time %lu us\n", __func__, (end.tv_sec - st.tv_sec)*1000000 + (end.tv_usec - st.tv_usec));
#endif

p_err_i2c_unlock:
	if (hold > 0) {
		hold = sensor_gh1_cis_group_param_hold_func(subdev, 0x00);
		if (hold < 0)
			ret = hold;
	}
	I2C_MUTEX_UNLOCK(cis->i2c_lock);

p_err:
	return ret;
}

int sensor_gh1_cis_set_frame_rate(struct v4l2_subdev *subdev, u32 min_fps)
{
	int ret = 0;
	struct is_cis *cis;
	cis_shared_data *cis_data;

	u32 frame_duration = 0;

#ifdef DEBUG_SENSOR_TIME
	struct timeval st, end;
	do_gettimeofday(&st);
#endif

	FIMC_BUG(!subdev);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	FIMC_BUG(!cis);
	FIMC_BUG(!cis->cis_data);

	cis_data = cis->cis_data;

	if (min_fps > cis_data->max_fps) {
		err("[MOD:D:%d] %s, request FPS is too high(%d), set to max(%d)\n",
			cis->id, __func__, min_fps, cis_data->max_fps);
		min_fps = cis_data->max_fps;
	}

	if (min_fps == 0) {
		err("[MOD:D:%d] %s, request FPS is 0, set to min FPS(1)\n",
			cis->id, __func__);
		min_fps = 1;
	}

	frame_duration = (1 * 1000 * 1000) / min_fps;

	dbg_sensor(1, "[MOD:D:%d] %s, set FPS(%d), frame duration(%d)\n",
			cis->id, __func__, min_fps, frame_duration);

	ret = sensor_gh1_cis_set_frame_duration(subdev, frame_duration);
	if (ret < 0) {
		err("[MOD:D:%d] %s, set frame duration is fail(%d)\n",
			cis->id, __func__, ret);
		goto p_err;
	}

#ifdef CAMERA_REAR2
	cis_data->min_frame_us_time = MAX(frame_duration, cis_data->min_sync_frame_us_time);
#else
	cis_data->min_frame_us_time = frame_duration;
#endif

#ifdef DEBUG_SENSOR_TIME
	do_gettimeofday(&end);
	dbg_sensor(1, "[%s] time %lu us\n", __func__, (end.tv_sec - st.tv_sec)*1000000 + (end.tv_usec - st.tv_usec));
#endif

p_err:

	return ret;
}

int sensor_gh1_cis_adjust_analog_gain(struct v4l2_subdev *subdev, u32 input_again, u32 *target_permile)
{
	int ret = 0;
	struct is_cis *cis;
	cis_shared_data *cis_data;

	u32 again_code = 0;
	u32 again_permile = 0;

#ifdef DEBUG_SENSOR_TIME
	struct timeval st, end;
	do_gettimeofday(&st);
#endif

	FIMC_BUG(!subdev);
	FIMC_BUG(!target_permile);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	FIMC_BUG(!cis);
	FIMC_BUG(!cis->cis_data);

	cis_data = cis->cis_data;

	again_code = sensor_cis_calc_again_code(input_again);

	if (again_code > cis_data->max_analog_gain[0]) {
		again_code = cis_data->max_analog_gain[0];
	} else if (again_code < cis_data->min_analog_gain[0]) {
		again_code = cis_data->min_analog_gain[0];
	}

	again_permile = sensor_cis_calc_again_permile(again_code);

	dbg_sensor(1, "[%s] min again(%d), max(%d), input_again(%d), code(%d), permile(%d)\n", __func__,
			cis_data->max_analog_gain[0],
			cis_data->min_analog_gain[0],
			input_again,
			again_code,
			again_permile);

	*target_permile = again_permile;

	return ret;
}

int sensor_gh1_cis_set_analog_gain(struct v4l2_subdev *subdev, struct ae_param *again)
{
	int ret = 0;
	int hold = 0;
	struct is_cis *cis;
	struct i2c_client *client;
	cis_shared_data *cis_data;

	u16 short_gain = 0;
	u16 long_gain = 0;

#ifdef DEBUG_SENSOR_TIME
	struct timeval st, end;
	do_gettimeofday(&st);
#endif

	FIMC_BUG(!subdev);
	FIMC_BUG(!again);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	FIMC_BUG(!cis);
	FIMC_BUG(!cis->cis_data);

	client = cis->client;
	if (unlikely(!client)) {
		err("client is NULL");
		ret = -EINVAL;
		goto p_err;
	}

	cis_data = cis->cis_data;

	short_gain = (u16)sensor_cis_calc_again_code(again->short_val);
	long_gain = (u16)sensor_cis_calc_again_code(again->long_val);

	if (long_gain < cis->cis_data->min_analog_gain[0]) {
		info("[%s] not proper long_gain value, reset to min_analog_gain\n", __func__);
		long_gain = cis->cis_data->min_analog_gain[0];
	}
	if (long_gain > cis->cis_data->max_analog_gain[0]) {
		info("[%s] not proper long_gain value, reset to max_analog_gain\n", __func__);
		long_gain = cis->cis_data->max_analog_gain[0];
	}

	if (short_gain < cis->cis_data->min_analog_gain[0]) {
		info("[%s] not proper short_gain value, reset to min_analog_gain\n", __func__);
		short_gain = cis->cis_data->min_analog_gain[0];
	}
	if (short_gain > cis->cis_data->max_analog_gain[0]) {
		info("[%s] not proper short_gain value, reset to max_analog_gain\n", __func__);
		short_gain = cis->cis_data->max_analog_gain[0];
	}

	dbg_sensor(1, "[MOD:D:%d] %s(vsync cnt = %d),"
		KERN_CONT "input short gain = %d us, input long gain = %d us,"
		KERN_CONT "analog short gain(%#x), analog short gain(%#x)\n",
		cis->id, __func__, cis->cis_data->sen_vsync_count,
		again->short_val, again->long_val,
		short_gain, long_gain);

	I2C_MUTEX_LOCK(cis->i2c_lock);
	hold = sensor_gh1_cis_group_param_hold_func(subdev, 0x01);
	if (hold < 0) {
		ret = hold;
		goto p_err_i2c_unlock;
	}

	/* Short analog gain */
	ret = is_sensor_write16(client, 0x0204, short_gain);
	if (ret < 0)
		goto p_err_i2c_unlock;

#if 1
	/*
	 * gh1 sensor have to setting a long analog gain
	 * This function check a sensor guide by sensor development team
	 */
	/* Long analog gain */
	if (sensor_gh1_cis_is_wdr_mode_on(cis_data)) {
		if (long_gain != short_gain)	/* Request it by IQ team*/
			long_gain = short_gain;
		ret = is_sensor_write16(client, 0x0206, long_gain);
		if (ret < 0)
			goto p_err_i2c_unlock;
	}
#endif

#ifdef DEBUG_SENSOR_TIME
	do_gettimeofday(&end);
	dbg_sensor(1, "[%s] time %lu us\n", __func__, (end.tv_sec - st.tv_sec)*1000000 + (end.tv_usec - st.tv_usec));
#endif

p_err_i2c_unlock:
	if (hold > 0) {
		hold = sensor_gh1_cis_group_param_hold_func(subdev, 0x00);
		if (hold < 0)
			ret = hold;
	}
	I2C_MUTEX_UNLOCK(cis->i2c_lock);

p_err:
	return ret;
}

int sensor_gh1_cis_get_analog_gain(struct v4l2_subdev *subdev, u32 *again)
{
	int ret = 0;
	int hold = 0;
	struct is_cis *cis;
	struct i2c_client *client;

	u16 analog_gain = 0;

#ifdef DEBUG_SENSOR_TIME
	struct timeval st, end;
	do_gettimeofday(&st);
#endif

	FIMC_BUG(!subdev);
	FIMC_BUG(!again);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	FIMC_BUG(!cis);

	client = cis->client;
	if (unlikely(!client)) {
		err("client is NULL");
		ret = -EINVAL;
		goto p_err;
	}

	I2C_MUTEX_LOCK(cis->i2c_lock);
	hold = sensor_gh1_cis_group_param_hold_func(subdev, 0x01);
	if (hold < 0) {
		ret = hold;
		goto p_err_i2c_unlock;
	}

	ret = is_sensor_read16(client, 0x0204, &analog_gain);
	if (ret < 0)
		goto p_err_i2c_unlock;

	*again = sensor_cis_calc_again_permile(analog_gain);

	dbg_sensor(1, "[MOD:D:%d] %s, cur_again = %d us, analog_gain(%#x)\n",
			cis->id, __func__, *again, analog_gain);

#ifdef DEBUG_SENSOR_TIME
	do_gettimeofday(&end);
	dbg_sensor(1, "[%s] time %lu us\n", __func__, (end.tv_sec - st.tv_sec)*1000000 + (end.tv_usec - st.tv_usec));
#endif

p_err_i2c_unlock:
	if (hold > 0) {
		hold = sensor_gh1_cis_group_param_hold_func(subdev, 0x00);
		if (hold < 0)
			ret = hold;
	}
	I2C_MUTEX_UNLOCK(cis->i2c_lock);

p_err:
	return ret;
}

int sensor_gh1_cis_get_min_analog_gain(struct v4l2_subdev *subdev, u32 *min_again)
{
	int ret = 0;
	struct is_cis *cis;
	cis_shared_data *cis_data;

#ifdef DEBUG_SENSOR_TIME
	struct timeval st, end;

	do_gettimeofday(&st);
#endif

	WARN_ON(!subdev);
	WARN_ON(!min_again);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	WARN_ON(!cis);
	WARN_ON(!cis->cis_data);

	cis_data = cis->cis_data;
	cis_data->min_analog_gain[0] = 0x20; /* x1, gain=x/0x20 */
	cis_data->min_analog_gain[1] = sensor_cis_calc_again_permile(cis_data->min_analog_gain[0]);

	*min_again = cis_data->min_analog_gain[1];

	dbg_sensor(1, "[%s] code %d, permile %d\n", __func__,
		cis_data->min_analog_gain[0], cis_data->min_analog_gain[1]);

#ifdef DEBUG_SENSOR_TIME
	do_gettimeofday(&end);
	dbg_sensor(1, "[%s] time %lu us\n", __func__, (end.tv_sec - st.tv_sec)*1000000 + (end.tv_usec - st.tv_usec));
#endif

	return ret;
}

int sensor_gh1_cis_get_max_analog_gain(struct v4l2_subdev *subdev, u32 *max_again)
{
	int ret = 0;
	struct is_cis *cis;
	cis_shared_data *cis_data;

#ifdef DEBUG_SENSOR_TIME
	struct timeval st, end;
	do_gettimeofday(&st);
#endif

	WARN_ON(!subdev);
	WARN_ON(!max_again);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	WARN_ON(!cis);
	WARN_ON(!cis->cis_data);

	cis_data = cis->cis_data;
	cis_data->max_analog_gain[0] = 0x200; /* x16, gain=x/0x20 */
	cis_data->max_analog_gain[1] = sensor_cis_calc_again_permile(cis_data->max_analog_gain[0]);

	*max_again = cis_data->max_analog_gain[1];

	dbg_sensor(1, "[%s] code %d, permile %d\n", __func__,
		cis_data->max_analog_gain[0], cis_data->max_analog_gain[1]);

#ifdef DEBUG_SENSOR_TIME
	do_gettimeofday(&end);
	dbg_sensor(1, "[%s] time %lu us\n", __func__, (end.tv_sec - st.tv_sec)*1000000 + (end.tv_usec - st.tv_usec));
#endif

	return ret;
}

int sensor_gh1_cis_set_digital_gain(struct v4l2_subdev *subdev, struct ae_param *dgain)
{
	int ret = 0;
	int hold = 0;
	struct is_cis *cis;
	struct i2c_client *client;
	cis_shared_data *cis_data;

	u16 long_gain = 0;
	u16 short_gain = 0;

#ifdef DEBUG_SENSOR_TIME
	struct timeval st, end;
	do_gettimeofday(&st);
#endif

	FIMC_BUG(!subdev);
	FIMC_BUG(!dgain);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	FIMC_BUG(!cis);
	FIMC_BUG(!cis->cis_data);

	client = cis->client;
	if (unlikely(!client)) {
		err("client is NULL");
		ret = -EINVAL;
		goto p_err;
	}

	cis_data = cis->cis_data;

	long_gain = (u16)sensor_cis_calc_dgain_code(dgain->long_val);
	short_gain = (u16)sensor_cis_calc_dgain_code(dgain->short_val);

	if (long_gain < cis->cis_data->min_digital_gain[0]) {
		info("[%s] not proper long_gain value, reset to min_digital_gain\n", __func__);
		long_gain = cis->cis_data->min_digital_gain[0];
	}
	if (long_gain > cis->cis_data->max_digital_gain[0]) {
		info("[%s] not proper long_gain value, reset to max_digital_gain\n", __func__);
		long_gain = cis->cis_data->max_digital_gain[0];
	}

	if (short_gain < cis->cis_data->min_digital_gain[0]) {
		info("[%s] not proper short_gain value, reset to min_digital_gain\n", __func__);
		short_gain = cis->cis_data->min_digital_gain[0];
	}
	if (short_gain > cis->cis_data->max_digital_gain[0]) {
		info("[%s] not proper short_gain value, reset to max_digital_gain\n", __func__);
		short_gain = cis->cis_data->max_digital_gain[0];
	}

	dbg_sensor(1, "[MOD:D:%d] %s, input_dgain = %d/%d us, long_gain(%#x), short_gain(%#x)\n",
			cis->id, __func__, dgain->long_val, dgain->short_val, long_gain, short_gain);

	I2C_MUTEX_LOCK(cis->i2c_lock);
	hold = sensor_gh1_cis_group_param_hold_func(subdev, 0x01);
	if (hold < 0) {
		ret = hold;
		goto p_err_i2c_unlock;
	}

#if 0	/* Block unused register in GH1*/
	/* DGain WDR Mode */
	if (sensor_gh1_cis_is_wdr_mode_on(cis_data)) {
		is_sensor_write16(client, 0x31E8, 0x0100);
	} else {
		is_sensor_write16(client, 0x31E8, 0x0000);
	}
#endif
	/* Short digital gain */
	ret = is_sensor_write16(client, 0x020E, short_gain);
	if (ret < 0)
		goto p_err_i2c_unlock;

	/* Long digital gain */
	if (sensor_gh1_cis_is_wdr_mode_on(cis_data)) {
		ret = is_sensor_write16(client, 0x0230, long_gain);
		if (ret < 0)
			goto p_err_i2c_unlock;
	}
#ifdef DEBUG_SENSOR_TIME
	do_gettimeofday(&end);
	dbg_sensor(1, "[%s] time %lu us\n", __func__, (end.tv_sec - st.tv_sec)*1000000 + (end.tv_usec - st.tv_usec));
#endif

p_err_i2c_unlock:
	if (hold > 0) {
		hold = sensor_gh1_cis_group_param_hold_func(subdev, 0x00);
		if (hold < 0)
			ret = hold;
	}
	I2C_MUTEX_UNLOCK(cis->i2c_lock);

p_err:
	return ret;
}

int sensor_gh1_cis_get_digital_gain(struct v4l2_subdev *subdev, u32 *dgain)
{
	int ret = 0;
	int hold = 0;
	struct is_cis *cis;
	struct i2c_client *client;

	u16 digital_gain = 0;

#ifdef DEBUG_SENSOR_TIME
	struct timeval st, end;
	do_gettimeofday(&st);
#endif

	FIMC_BUG(!subdev);
	FIMC_BUG(!dgain);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	FIMC_BUG(!cis);

	client = cis->client;
	if (unlikely(!client)) {
		err("client is NULL");
		ret = -EINVAL;
		goto p_err;
	}

	I2C_MUTEX_LOCK(cis->i2c_lock);
	hold = sensor_gh1_cis_group_param_hold_func(subdev, 0x01);
	if (hold < 0) {
		ret = hold;
		goto p_err_i2c_unlock;
	}

	ret = is_sensor_read16(client, 0x020E, &digital_gain);
	if (ret < 0)
		goto p_err_i2c_unlock;

	*dgain = sensor_cis_calc_dgain_permile(digital_gain);

	dbg_sensor(1, "[MOD:D:%d] %s, cur_dgain = %d us, digital_gain(%#x)\n",
			cis->id, __func__, *dgain, digital_gain);

#ifdef DEBUG_SENSOR_TIME
	do_gettimeofday(&end);
	dbg_sensor(1, "[%s] time %lu us\n", __func__, (end.tv_sec - st.tv_sec)*1000000 + (end.tv_usec - st.tv_usec));
#endif

p_err_i2c_unlock:
	if (hold > 0) {
		hold = sensor_gh1_cis_group_param_hold_func(subdev, 0x00);
		if (hold < 0)
			ret = hold;
	}
	I2C_MUTEX_UNLOCK(cis->i2c_lock);

p_err:
	return ret;
}

int sensor_gh1_cis_get_min_digital_gain(struct v4l2_subdev *subdev, u32 *min_dgain)
{
	int ret = 0;
	struct is_cis *cis;
	cis_shared_data *cis_data;

#ifdef DEBUG_SENSOR_TIME
	struct timeval st, end;
	do_gettimeofday(&st);
#endif

	WARN_ON(!subdev);
	WARN_ON(!min_dgain);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	WARN_ON(!cis);
	WARN_ON(!cis->cis_data);

	cis_data = cis->cis_data;
	cis_data->min_digital_gain[0] = 0x100;
	cis_data->min_digital_gain[1] = sensor_cis_calc_dgain_permile(cis_data->min_digital_gain[0]);

	*min_dgain = cis_data->min_digital_gain[1];

	dbg_sensor(1, "[%s] code %d, permile %d\n", __func__,
		cis_data->min_digital_gain[0], cis_data->min_digital_gain[1]);

#ifdef DEBUG_SENSOR_TIME
	do_gettimeofday(&end);
	dbg_sensor(1, "[%s] time %lu us\n", __func__, (end.tv_sec - st.tv_sec)*1000000 + (end.tv_usec - st.tv_usec));
#endif

	return ret;
}

int sensor_gh1_cis_get_max_digital_gain(struct v4l2_subdev *subdev, u32 *max_dgain)
{
	int ret = 0;
	struct is_cis *cis;
	cis_shared_data *cis_data;

#ifdef DEBUG_SENSOR_TIME
	struct timeval st, end;
	do_gettimeofday(&st);
#endif

	WARN_ON(!subdev);
	WARN_ON(!max_dgain);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	WARN_ON(!cis);
	WARN_ON(!cis->cis_data);

	cis_data = cis->cis_data;
	cis_data->max_digital_gain[0] = 0x1000;
	cis_data->max_digital_gain[1] = sensor_cis_calc_dgain_permile(cis_data->max_digital_gain[0]);

	*max_dgain = cis_data->max_digital_gain[1];

	dbg_sensor(1, "[%s] code %d, permile %d\n", __func__,
		cis_data->max_digital_gain[0], cis_data->max_digital_gain[1]);

#ifdef DEBUG_SENSOR_TIME
	do_gettimeofday(&end);
	dbg_sensor(1, "[%s] time %lu us\n", __func__, (end.tv_sec - st.tv_sec)*1000000 + (end.tv_usec - st.tv_usec));
#endif

	return ret;
}

int sensor_gh1_cis_set_wb_gain(struct v4l2_subdev *subdev, struct wb_gains wb_gains)
{
	int ret = 0;
	int hold = 0;
	struct is_cis *cis;
	struct i2c_client *client;
	int mode = 0;
	u16 abs_gains[3] = {0, }; /* R, G, B */
	u32 avg_g = 0, div = 0;

#ifdef DEBUG_SENSOR_TIME
	struct timeval st, end;
	do_gettimeofday(&st);
#endif

	FIMC_BUG(!subdev);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	FIMC_BUG(!cis);
	FIMC_BUG(!cis->cis_data);

	client = cis->client;
	if (unlikely(!client)) {
		err("client is NULL");
		ret = -EINVAL;
		goto p_err;
	}

	if (!cis->use_wb_gain)
		return ret;

	mode = cis->cis_data->sens_config_index_cur;
	if (!sensor_gh1_support_wb_gain[mode]) {
		return 0;
	}

	if (wb_gains.gr != wb_gains.gb) {
		err("gr, gb not euqal"); /* check DDK layer */
		return -EINVAL;
	}

	if (wb_gains.gr == 1024)
		div = 4;
	else if (wb_gains.gr == 2048)
		div = 8;
	else {
		err("invalid gr,gb %d", wb_gains.gr); /* check DDK layer */
		return -EINVAL;
	}

	dbg_sensor(1, "[SEN:%d]%s:DDK vlaue: wb_gain_gr(%d), wb_gain_r(%d), wb_gain_b(%d), wb_gain_gb(%d)\n",
		cis->id, __func__, wb_gains.gr, wb_gains.r, wb_gains.b, wb_gains.gb);

	avg_g = (wb_gains.gr + wb_gains.gb) / 2;
	abs_gains[0] = (u16)((wb_gains.r / div) & 0xFFFF);
	abs_gains[1] = (u16)((avg_g / div) & 0xFFFF);
	abs_gains[2] = (u16)((wb_gains.b / div) & 0xFFFF);

	dbg_sensor(1, "[SEN:%d]%s, abs_gain_r(0x%4X), abs_gain_g(0x%4X), abs_gain_b(0x%4X)\n",
		cis->id, __func__, abs_gains[0], abs_gains[1], abs_gains[2]);

	hold = sensor_gh1_cis_group_param_hold(subdev, 0x01);
	if (hold < 0) {
		ret = hold;
		goto p_err;
	}

	I2C_MUTEX_LOCK(cis->i2c_lock);
	ret |= is_sensor_write16(client, 0xFCFC, 0x4000);
	ret |= is_sensor_write16(client, 0x0D82, abs_gains[0]);
	ret |= is_sensor_write16(client, 0x0D84, abs_gains[1]);
	ret |= is_sensor_write16(client, 0x0D86, abs_gains[2]);
	if (ret < 0) {
		err("sensor_gh1_set_registers fail!!");
		goto p_i2c_err;
	}

#ifdef DEBUG_SENSOR_TIME
	do_gettimeofday(&end);
	dbg_sensor(1, "[%s] time %lu us\n", __func__, (end.tv_sec - st.tv_sec)*1000000 + (end.tv_usec - st.tv_usec));
#endif

p_i2c_err:
	I2C_MUTEX_UNLOCK(cis->i2c_lock);

p_err:
	if (hold > 0) {
		hold = sensor_gh1_cis_group_param_hold(subdev, 0x00);
		if (hold < 0)
			ret = hold;
	}

	return ret;
}

static struct is_cis_ops cis_ops = {
	.cis_init = sensor_gh1_cis_init,
	.cis_log_status = sensor_gh1_cis_log_status,
	.cis_group_param_hold = sensor_gh1_cis_group_param_hold,
	.cis_set_global_setting = sensor_gh1_cis_set_global_setting,
	.cis_mode_change = sensor_gh1_cis_mode_change,
	.cis_set_size = sensor_gh1_cis_set_size,
	.cis_stream_on = sensor_gh1_cis_stream_on,
	.cis_stream_off = sensor_gh1_cis_stream_off,
	.cis_set_exposure_time = sensor_gh1_cis_set_exposure_time,
	.cis_get_min_exposure_time = sensor_gh1_cis_get_min_exposure_time,
	.cis_get_max_exposure_time = sensor_gh1_cis_get_max_exposure_time,
	.cis_adjust_frame_duration = sensor_gh1_cis_adjust_frame_duration,
	.cis_set_frame_duration = sensor_gh1_cis_set_frame_duration,
	.cis_set_frame_rate = sensor_gh1_cis_set_frame_rate,
	.cis_adjust_analog_gain = sensor_gh1_cis_adjust_analog_gain,
	.cis_set_analog_gain = sensor_gh1_cis_set_analog_gain,
	.cis_get_analog_gain = sensor_gh1_cis_get_analog_gain,
	.cis_get_min_analog_gain = sensor_gh1_cis_get_min_analog_gain,
	.cis_get_max_analog_gain = sensor_gh1_cis_get_max_analog_gain,
	.cis_set_digital_gain = sensor_gh1_cis_set_digital_gain,
	.cis_get_digital_gain = sensor_gh1_cis_get_digital_gain,
	.cis_get_min_digital_gain = sensor_gh1_cis_get_min_digital_gain,
	.cis_get_max_digital_gain = sensor_gh1_cis_get_max_digital_gain,
	.cis_compensate_gain_for_extremely_br = sensor_cis_compensate_gain_for_extremely_br,
	.cis_set_wb_gains = sensor_gh1_cis_set_wb_gain,
	.cis_wait_streamoff = sensor_cis_wait_streamoff,
	.cis_wait_streamon = sensor_cis_wait_streamon,
	.cis_check_rev_on_init = sensor_cis_check_rev_on_init,
	.cis_set_initial_exposure = sensor_cis_set_initial_exposure,
	.cis_set_test_pattern = sensor_cis_set_test_pattern,
	.cis_set_long_term_exposure = sensor_gh1_cis_long_term_exposure,
};

static int cis_gh1_probe(struct i2c_client *client,
	const struct i2c_device_id *id)
{
	int ret = 0;
	struct is_core *core = NULL;
	struct v4l2_subdev *subdev_cis = NULL;
	struct is_cis *cis = NULL;
	struct is_device_sensor *device = NULL;
	struct is_device_sensor_peri *sensor_peri = NULL;
	const u32 *post_init_setting_list_spec;
	u32 sensor_id[IS_STREAM_COUNT] = {0, };
	u32 sensor_id_len;
	const u32 *sensor_id_spec;
	char const *setfile;
	struct device *dev;
	struct device_node *dnode;
	int i;
	int index;
	const int *verify_sensor_mode = NULL;
	int verify_sensor_mode_size = 0;

	FIMC_BUG(!client);
	FIMC_BUG(!is_dev);

	core = (struct is_core *)dev_get_drvdata(is_dev);
	if (!core) {
		probe_info("core device is not yet probed");
		return -EPROBE_DEFER;
	}

	dev = &client->dev;
	dnode = dev->of_node;

	sensor_id_spec = of_get_property(dnode, "id", &sensor_id_len);
	if (!sensor_id_spec) {
		err("sensor_id num read is fail(%d)", ret);
		goto p_err;
	}

	sensor_id_len /= (unsigned int)sizeof(*sensor_id_spec);

	probe_info("%s sensor_id_spec %d, sensor_id_len %d\n", __func__,
			*sensor_id_spec, sensor_id_len);

	ret = of_property_read_u32_array(dnode, "id", sensor_id, sensor_id_len);
	if (ret) {
		err("sensor_id read is fail(%d)", ret);
		goto p_err;
	}

	for (i = 0; i < sensor_id_len; i++) {
		probe_info("%s sensor_id %d\n", __func__, sensor_id[i]);
		device = &core->sensor[sensor_id[i]];

		sensor_peri = find_peri_by_cis_id(device, SENSOR_NAME_S5KGH1);
		if (!sensor_peri) {
			probe_info("GH1 : sensor peri is net yet probed");
			return -EPROBE_DEFER;
		}
	}

	for (i = 0; i < sensor_id_len; i++) {
		device = &core->sensor[sensor_id[i]];
		sensor_peri = find_peri_by_cis_id(device, SENSOR_NAME_S5KGH1);

		cis = &sensor_peri->cis;
		subdev_cis = kzalloc(sizeof(struct v4l2_subdev), GFP_KERNEL);
		if (!subdev_cis) {
			probe_err("subdev_cis is NULL");
			ret = -ENOMEM;
			goto p_err;
		}

		sensor_peri->subdev_cis = subdev_cis;

		cis->id = SENSOR_NAME_S5KGH1;
		cis->subdev = subdev_cis;
		cis->device = sensor_id[i];
		cis->client = client;
		sensor_peri->module->client = cis->client;
		cis->i2c_lock = NULL;
		cis->ctrl_delay = N_PLUS_TWO_FRAME;
		cis->mipi_clock_index_cur = CAM_MIPI_NOT_INITIALIZED;
		cis->mipi_clock_index_new = CAM_MIPI_NOT_INITIALIZED;
		cis->cis_data = kzalloc(sizeof(cis_shared_data), GFP_KERNEL);
		if (!cis->cis_data) {
			err("cis_data is NULL");
			ret = -ENOMEM;
			goto p_err;
		}
		cis->cis_ops = &cis_ops;

		/* belows are depend on sensor cis. MUST check sensor spec */
		cis->bayer_order = OTF_INPUT_ORDER_BAYER_GR_BG;

		if (of_property_read_bool(dnode, "sensor_f_number")) {
			if (of_property_read_u32(dnode, "sensor_f_number", &cis->aperture_num))
				warn("f-number read is fail(%d)",ret);
		} else {
			cis->aperture_num = F1_9;
		}

		probe_info("%s f-number %d\n", __func__, cis->aperture_num);

		cis->use_dgain = true;
		cis->use_wb_gain = true;
		cis->hdr_ctrl_by_again = false;

		v4l2_set_subdevdata(subdev_cis, cis);
		v4l2_set_subdev_hostdata(subdev_cis, device);
		snprintf(subdev_cis->name, V4L2_SUBDEV_NAME_SIZE, "cis-subdev.%d", cis->id);

		sensor_cis_parse_dt(dev, cis->subdev);
	}

	cis->use_initial_ae = of_property_read_bool(dnode, "use_initial_ae");
	probe_info("%s use initial_ae(%d)\n", __func__, cis->use_initial_ae);

	cis->vendor_use_adaptive_mipi = of_property_read_bool(dnode, "vendor_use_adaptive_mipi");
	probe_info("%s vendor_use_adaptive_mipi(%d)\n", __func__, cis->vendor_use_adaptive_mipi);

	if (of_property_read_string(dnode, "setfile", &setfile)) {
		err("setfile index read fail(%d), take default setfile!!", ret);
		setfile = "default";
	}

	post_init_setting_list_spec = of_get_property(dnode, "post_init_setting_list",
		&sensor_gh1_post_init_setting_list_len);
	if (post_init_setting_list_spec) {
		sensor_gh1_post_init_setting_list_len /= (unsigned int)sizeof(*post_init_setting_list_spec);

		BUG_ON(sensor_gh1_post_init_setting_list_len > SENSOR_GH1_POST_INIT_SETTING_MAX);

		ret = of_property_read_u32_array(dnode, "post_init_setting_list",
			sensor_gh1_post_init_setting_list, sensor_gh1_post_init_setting_list_len);
		if (ret)
			err("sensor_gh1_post_init_setting_list read is fail(%d)", ret);
	}

	if (strcmp(setfile, "default") == 0 ||
			strcmp(setfile, "setA") == 0) {
		probe_info("%s setfile_A\n", __func__);
		sensor_gh1_global = sensor_gh1_setfile_A_Global;
		sensor_gh1_global_size = ARRAY_SIZE(sensor_gh1_setfile_A_Global);
		sensor_gh1_global_secure = sensor_gh1_setfile_A_Global_Secure;
		sensor_gh1_global_secure_size = ARRAY_SIZE(sensor_gh1_setfile_A_Global_Secure);
		sensor_gh1_setfiles = sensor_gh1_setfiles_A;
		sensor_gh1_setfile_sizes = sensor_gh1_setfile_A_sizes;
		sensor_gh1_pllinfos = sensor_gh1_pllinfos_A;
		sensor_gh1_max_setfile_num = ARRAY_SIZE(sensor_gh1_setfiles_A);
		cis->mipi_sensor_mode = sensor_gh1_setfile_A_mipi_sensor_mode;
		cis->mipi_sensor_mode_size = ARRAY_SIZE(sensor_gh1_setfile_A_mipi_sensor_mode);
		verify_sensor_mode = sensor_gh1_setfile_A_verify_sensor_mode;
		verify_sensor_mode_size = ARRAY_SIZE(sensor_gh1_setfile_A_verify_sensor_mode);
	}
#if 0
	else if (strcmp(setfile, "setB") == 0) {
		probe_info("%s setfile_B\n", __func__);
		sensor_gh1_global = sensor_gh1_setfile_B_Global;
		sensor_gh1_global_size = ARRAY_SIZE(sensor_gh1_setfile_B_Global);
		sensor_gh1_setfiles = sensor_gh1_setfiles_B;
		sensor_gh1_setfile_sizes = sensor_gh1_setfile_B_sizes;
		sensor_gh1_pllinfos = sensor_gh1_pllinfos_B;
		sensor_gh1_max_setfile_num = ARRAY_SIZE(sensor_gh1_setfiles_B);
	}
#endif
	else {
		err("%s setfile index out of bound, take default (setfile_A)", __func__);
		sensor_gh1_global = sensor_gh1_setfile_A_Global;
		sensor_gh1_global_size = ARRAY_SIZE(sensor_gh1_setfile_A_Global);
		sensor_gh1_global_secure = sensor_gh1_setfile_A_Global_Secure;
		sensor_gh1_global_secure_size = ARRAY_SIZE(sensor_gh1_setfile_A_Global_Secure);
		sensor_gh1_setfiles = sensor_gh1_setfiles_A;
		sensor_gh1_setfile_sizes = sensor_gh1_setfile_A_sizes;
		sensor_gh1_pllinfos = sensor_gh1_pllinfos_A;
		sensor_gh1_max_setfile_num = ARRAY_SIZE(sensor_gh1_setfiles_A);
		cis->mipi_sensor_mode = sensor_gh1_setfile_A_mipi_sensor_mode;
		cis->mipi_sensor_mode_size = ARRAY_SIZE(sensor_gh1_setfile_A_mipi_sensor_mode);
		verify_sensor_mode = sensor_gh1_setfile_A_verify_sensor_mode;
		verify_sensor_mode_size = ARRAY_SIZE(sensor_gh1_setfile_A_verify_sensor_mode);
	}

	if (cis->vendor_use_adaptive_mipi) {
		for (i = 0; i < verify_sensor_mode_size; i++) {
			index = verify_sensor_mode[i];

			if (index >= cis->mipi_sensor_mode_size || index < 0) {
				panic("wrong mipi_sensor_mode index");
				break;
			}

			if (is_vendor_verify_mipi_channel(cis->mipi_sensor_mode[index].mipi_channel,
						cis->mipi_sensor_mode[index].mipi_channel_size)) {
				panic("wrong mipi channel");
				break;
			}
		}
	}

	probe_info("%s done\n", __func__);

p_err:
	return ret;
}

static const struct of_device_id sensor_cis_gh1_match[] = {
	{
		.compatible = "samsung,exynos-is-cis-gh1",
	},
	{},
};
MODULE_DEVICE_TABLE(of, sensor_cis_gh1_match);

static const struct i2c_device_id sensor_cis_gh1_idt[] = {
	{ SENSOR_NAME, 0 },
	{},
};

static struct i2c_driver sensor_cis_gh1_driver = {
	.probe	= cis_gh1_probe,
	.driver = {
		.name	= SENSOR_NAME,
		.owner	= THIS_MODULE,
		.of_match_table = sensor_cis_gh1_match,
		.suppress_bind_attrs = true,
	},
	.id_table = sensor_cis_gh1_idt
};

#ifdef MODULE
builtin_i2c_driver(sensor_cis_gh1_driver);
#else
static int __init sensor_cis_gh1_init(void)
{
	int ret;

	ret = i2c_add_driver(&sensor_cis_gh1_driver);
	if (ret)
		err("failed to add %s driver: %d\n",
			sensor_cis_gh1_driver.driver.name, ret);

	return ret;
}
late_initcall_sync(sensor_cis_gh1_init);
#endif

MODULE_LICENSE("GPL");
