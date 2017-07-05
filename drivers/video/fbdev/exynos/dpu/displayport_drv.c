/*
 * Copyright (c) 2016 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com
 *
 * Samsung SoC DisplayPort driver.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/platform_device.h>
#include <linux/phy/phy.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/kthread.h>
#include <linux/pm_runtime.h>
#include <linux/of_gpio.h>
#include <linux/device.h>
#include <linux/module.h>
#include <video/mipi_display.h>
#include <linux/regulator/consumer.h>
#include <media/v4l2-dv-timings.h>

#include "displayport.h"
#include "decon.h"

#define PIXELCLK_2160P30HZ 297000000 /* UHD 30hz */
#define PIXELCLK_1080P60HZ 148500000 /* FHD 60Hz */
#define PIXELCLK_1080P30HZ 74250000 /* FHD 30Hz */

#define HDCP_SUPPORT
#define HDCP_2_2

int displayport_log_level = 6;
videoformat g_displayport_videoformat = v640x480p_60Hz;
static u8 max_lane_cnt;
static u8 max_link_rate;
static u64 reduced_resolution;
struct displayport_debug_param g_displayport_debug_param;

struct displayport_device *displayport_drvdata;
EXPORT_SYMBOL(displayport_drvdata);

static int displayport_runtime_suspend(struct device *dev);
static int displayport_runtime_resume(struct device *dev);

void displayport_hdcp22_enable(u32 en);

static void displayport_dump_registers(struct displayport_device *displayport)
{
	displayport_info("=== DisplayPort SFR DUMP ===\n");

	print_hex_dump(KERN_INFO, "", DUMP_PREFIX_ADDRESS, 32, 4,
			displayport->res.regs, 0xC0, false);
}

static int displayport_remove(struct platform_device *pdev)
{
	struct displayport_device *displayport = platform_get_drvdata(pdev);

	pm_runtime_disable(&pdev->dev);
	mutex_destroy(&displayport->cmd_lock);
	mutex_destroy(&displayport->hpd_lock);
	mutex_destroy(&displayport->aux_lock);
	mutex_destroy(&displayport->training_lock);
	destroy_workqueue(displayport->dp_wq);
	destroy_workqueue(displayport->hdcp2_wq);
	displayport_info("displayport driver removed\n");

	return 0;
}

static int displayport_get_gpios(struct displayport_device *displayport)
{
	struct device *dev = displayport->dev;
	struct displayport_resources *res = &displayport->res;

	displayport_info("%s +\n", __func__);

	if (of_get_property(dev->of_node, "gpios", NULL) != NULL)  {
		/* panel reset */
		res->aux_ch_mux_gpio = of_get_gpio(dev->of_node, 0);
		if (res->aux_ch_mux_gpio < 0) {
			displayport_err("failed to get aux ch mux GPIO");
			return -ENODEV;
		}
	}

	displayport_info("%s -\n", __func__);
	return 0;
}

static u64 displayport_find_edid_max_pixelclock(void)
{
	int i;

	for (i = displayport_pre_cnt - 1;i > 0; i--) {
		if (displayport_supported_presets[i].edid_support_match) {
			displayport_info("max resolution: %s\n",
				displayport_supported_presets[i].name);
			break;
		}
	}

	return displayport_supported_presets[i].dv_timings.bt.pixelclock;
}

static int displayport_get_min_link_rate(u8 rx_link_rate, u8 lane_cnt)
{
	int i;
	u64 pc1lane[] = {54000000, 90000000, 180000000};
	int link_rate[] = {LINK_RATE_1_62Gbps, LINK_RATE_2_7Gbps, LINK_RATE_5_4Gbps};
	u64 max_pclk;
	u8 min_link_rate;

	if (rx_link_rate == LINK_RATE_1_62Gbps)
		return rx_link_rate;

	if (lane_cnt > 4)
		return LINK_RATE_5_4Gbps;

	max_pclk = displayport_find_edid_max_pixelclock();
	for (i = 0; i < 2; i++) {
		if ((u64)lane_cnt * pc1lane[i] >= max_pclk)
			break;
	}

	min_link_rate = link_rate[i] > rx_link_rate ? rx_link_rate : link_rate[i];
	displayport_info("set link late: 0x%x, lane cnt:%d\n", min_link_rate, lane_cnt);

	return min_link_rate;
}

void displayport_get_voltage_and_pre_emphasis_max_reach(u8 *drive_current, u8 *pre_emphasis, u8 *max_reach_value)
{
	int i;

	displayport_reg_get_voltage_and_pre_emphasis_max_reach((u8 *)max_reach_value);

	for (i = 0; i < 4; i++) {
		if (drive_current[i] + pre_emphasis[i] >= MAX_REACHED_CNT) {
			if (!((max_reach_value[i] >> MAX_SWING_REACHED_BIT_POS) & 0x01))
				max_reach_value[i] |= (1 << MAX_SWING_REACHED_BIT_POS);

			if (!((max_reach_value[i] >> MAX_PRE_EMPHASIS_REACHED_BIT_POS) & 0x01))
				max_reach_value[i] |= (1 << MAX_PRE_EMPHASIS_REACHED_BIT_POS);
		} else if (pre_emphasis[i] >= MAX_PRE_EMPHASIS_REACHED_CNT) {
			if (!((max_reach_value[i] >> MAX_PRE_EMPHASIS_REACHED_BIT_POS) & 0x01))
				max_reach_value[i] |= (1 << MAX_PRE_EMPHASIS_REACHED_BIT_POS);
		}
	}
}

static int displayport_full_link_training(void)
{
	u8 link_rate;
	u8 lane_cnt;
	u8 training_aux_rd_interval;
	u8 pre_emphasis[MAX_LANE_CNT];
	u8 drive_current[MAX_LANE_CNT];
	u8 voltage_swing_lane[MAX_LANE_CNT];
	u8 pre_emphasis_lane[MAX_LANE_CNT];
	u8 max_reach_value[MAX_LANE_CNT];
	int training_retry_no, eq_training_retry_no, i;
	u8 val[DPCD_BUF_SIZE] = {0,};
	u8 eq_val[DPCD_BUF_SIZE] = {0,};
	u8 lane_cr_done;
	u8 lane_channel_eq_done;
	u8 lane_symbol_locked_done;
	u8 interlane_align_done;
	u8 enhanced_frame_cap;
	int ret = 0;
	int tps3_supported = 0;
	struct displayport_device *displayport = get_displayport_drvdata();

	displayport_reg_dpcd_read_burst(DPCD_ADD_REVISION_NUMBER, DPCD_BUF_SIZE, val);
	displayport_info("Full Link Training Start + : %02x %02x\n", val[1], val[2]);

	link_rate = val[1];
	lane_cnt = val[2] & MAX_LANE_COUNT;
	max_lane_cnt = lane_cnt;
	tps3_supported = val[2] & TPS3_SUPPORTED;
	enhanced_frame_cap = val[2] & ENHANCED_FRAME_CAP;

	if (!displayport->auto_test_mode) {
		link_rate = displayport_get_min_link_rate(link_rate, lane_cnt);
		displayport->auto_test_mode = 0;
	}

	if (g_displayport_debug_param.param_used) {
		link_rate = g_displayport_debug_param.link_rate;
		lane_cnt = g_displayport_debug_param.lane_cnt;
	}

	if (enhanced_frame_cap)
		displayport_write_mask(DP_System_Control_4, 1, Enhanced_Framing_Mode);

	displayport_reg_dpcd_read(DPCD_ADD_TRAINING_AUX_RD_INTERVAL, 1, val);
	training_aux_rd_interval = val[0];

Reduce_Link_Rate_Retry:
	displayport_info("Reduce_Link_Rate_Retry\n");

	for (i = 0; i < 4; i++) {
		pre_emphasis[i] = 0;
		drive_current[i] = 0;
	}

	training_retry_no = 0;

	displayport_reg_phy_reset(1);

	displayport_reg_set_link_bw(link_rate);

	if (displayport_reg_get_cmn_ctl_sfr_ctl_mode()) {
		displayport_reg_set_phy_clk_bw(link_rate);
		displayport_dbg("displayport_reg_set_phy_clk_bw\n");
	}

	displayport_reg_set_lane_count(lane_cnt);

	displayport_info("link_rate = %x\n", link_rate);
	displayport_info("lane_cnt = %x\n", lane_cnt);

	displayport_reg_phy_reset(0);

	val[0] = link_rate;
	val[1] = lane_cnt;

	if (enhanced_frame_cap)
		val[1] |= ENHANCED_FRAME_CAP;

	displayport_reg_dpcd_write_burst(DPCD_ADD_LINK_BW_SET, 2, val);

	displayport_reg_wait_phy_pll_lock();

	displayport_reg_set_training_pattern(TRAINING_PATTERN_1);

	val[0] = 0x21;	/* SCRAMBLING_DISABLE, TRAINING_PATTERN_1 */
	displayport_reg_dpcd_write(DPCD_ADD_TRANING_PATTERN_SET, 1, val);

Voltage_Swing_Retry:
	displayport_dbg("Voltage_Swing_Retry\n");

	displayport_reg_set_voltage_and_pre_emphasis((u8 *)drive_current, (u8 *)pre_emphasis);
	displayport_get_voltage_and_pre_emphasis_max_reach((u8 *)drive_current,
			(u8 *)pre_emphasis, (u8 *)max_reach_value);

	val[0] = (pre_emphasis[0]<<3) | drive_current[0] | max_reach_value[0];
	val[1] = (pre_emphasis[1]<<3) | drive_current[1] | max_reach_value[1];
	val[2] = (pre_emphasis[2]<<3) | drive_current[2] | max_reach_value[2];
	val[3] = (pre_emphasis[3]<<3) | drive_current[3] | max_reach_value[3];
	displayport_info("Voltage_Swing_Retry %02x %02x %02x %02x\n", val[0], val[1], val[2], val[3]);
	displayport_reg_dpcd_write_burst(DPCD_ADD_TRANING_LANE0_SET, 4, val);

	udelay((training_aux_rd_interval*4000)+400);

	lane_cr_done = 0;

	displayport_reg_dpcd_read(DPCD_ADD_LANE0_1_STATUS, 2, val);
	lane_cr_done |= ((val[0] & LANE0_CR_DONE) >> 0);
	lane_cr_done |= ((val[0] & LANE1_CR_DONE) >> 3);
	lane_cr_done |= ((val[1] & LANE2_CR_DONE) << 2);
	lane_cr_done |= ((val[1] & LANE3_CR_DONE) >> 1);

	displayport_dbg("lane_cr_done = %x\n", lane_cr_done);

	if (lane_cnt == 0x04) {
		if (lane_cr_done == 0x0F) {
			displayport_dbg("lane_cr_done\n");
			goto EQ_Training_Start;
		}
	} else if (lane_cnt == 0x02) {
		if (lane_cr_done == 0x03) {
			displayport_dbg("lane_cr_done\n");
			goto EQ_Training_Start;
		}
	} else if (lane_cnt == 0x01) {
		if (lane_cr_done == 0x01) {
			displayport_dbg("lane_cr_done\n");
			goto EQ_Training_Start;
		}
	} else {
		val[0] = 0x00;	/* SCRAMBLING_ENABLE, NORMAL_DATA */
		displayport_reg_dpcd_write(DPCD_ADD_TRANING_PATTERN_SET, 1, val);
		displayport_err("Full Link Training Fail : Link Rate %02x, lane Count %02x -",
				link_rate, lane_cnt);
		return -EINVAL;
	}

	if (!(drive_current[0] == 3 && drive_current[1] == 3
				&& drive_current[2] == 3 && drive_current[3] == 3)) {
		displayport_reg_dpcd_read_burst(DPCD_ADD_ADJUST_REQUEST_LANE0_1, 2, val);
		voltage_swing_lane[0] = (val[0] & VOLTAGE_SWING_LANE0);
		pre_emphasis_lane[0] = (val[0] & PRE_EMPHASIS_LANE0) >> 2;
		voltage_swing_lane[1] = (val[0] & VOLTAGE_SWING_LANE1) >> 4;
		pre_emphasis_lane[1] = (val[0] & PRE_EMPHASIS_LANE1) >> 6;

		voltage_swing_lane[2] = (val[1] & VOLTAGE_SWING_LANE2);
		pre_emphasis_lane[2] = (val[1] & PRE_EMPHASIS_LANE2) >> 2;
		voltage_swing_lane[3] = (val[1] & VOLTAGE_SWING_LANE3) >> 4;
		pre_emphasis_lane[3] = (val[1] & PRE_EMPHASIS_LANE3) >> 6;

		if (drive_current[0] == voltage_swing_lane[0] &&
				drive_current[1] == voltage_swing_lane[1] &&
				drive_current[2] == voltage_swing_lane[2] &&
				drive_current[3] == voltage_swing_lane[3]) {
			if (training_retry_no == 4)
				goto Check_Link_rate;
			else
				training_retry_no++;
		} else
			training_retry_no = 1;

		for (i = 0; i < 4; i++) {
			drive_current[i] = voltage_swing_lane[i];
			pre_emphasis[i] = pre_emphasis_lane[i];
			displayport_dbg("v drive_current[%d] = %x\n",
					i, drive_current[i]);
			displayport_dbg("v pre_emphasis[%d] = %x\n",
					i, pre_emphasis[i]);
		}

		goto Voltage_Swing_Retry;
	}

Check_Link_rate:
	displayport_info("Check_Link_rate\n");

	if (link_rate == LINK_RATE_5_4Gbps) {
		link_rate = LINK_RATE_2_7Gbps;
		goto Reduce_Link_Rate_Retry;
	} else if (link_rate == LINK_RATE_2_7Gbps) {
		link_rate = LINK_RATE_1_62Gbps;
		goto Reduce_Link_Rate_Retry;
	} else if (link_rate == LINK_RATE_1_62Gbps) {
		val[0] = 0x00;	/* SCRAMBLING_ENABLE, NORMAL_DATA */
		displayport_reg_dpcd_write(DPCD_ADD_TRANING_PATTERN_SET, 1, val);
		displayport_err("Full Link Training Fail : Link_Rate Retry -");
		return -EINVAL;
	}

EQ_Training_Start:
	displayport_info("EQ_Training_Start\n");

	eq_training_retry_no = 0;
	for (i = 0; i < DPCD_BUF_SIZE; i++)
		eq_val[i] = 0;

	if (tps3_supported) {
		displayport_reg_set_training_pattern(TRAINING_PATTERN_3);

		val[0] = 0x23;	/* SCRAMBLING_DISABLE, TRAINING_PATTERN_3 */
		displayport_reg_dpcd_write(DPCD_ADD_TRANING_PATTERN_SET, 1, val);
	} else {
		displayport_reg_set_training_pattern(TRAINING_PATTERN_2);

		val[0] = 0x22;	/* SCRAMBLING_DISABLE, TRAINING_PATTERN_2 */
		displayport_reg_dpcd_write(DPCD_ADD_TRANING_PATTERN_SET, 1, val);
	}

EQ_Training_Retry:
	displayport_dbg("EQ_Training_Retry\n");

	displayport_reg_set_voltage_and_pre_emphasis((u8 *)drive_current, (u8 *)pre_emphasis);
	displayport_get_voltage_and_pre_emphasis_max_reach((u8 *)drive_current,
			(u8 *)pre_emphasis, (u8 *)max_reach_value);

	val[0] = (pre_emphasis[0]<<3) | drive_current[0] | max_reach_value[0];
	val[1] = (pre_emphasis[1]<<3) | drive_current[1] | max_reach_value[1];
	val[2] = (pre_emphasis[2]<<3) | drive_current[2] | max_reach_value[2];
	val[3] = (pre_emphasis[3]<<3) | drive_current[3] | max_reach_value[3];
	displayport_info("EQ_Training_Retry %02x %02x %02x %02x\n", val[0], val[1], val[2], val[3]);
	displayport_reg_dpcd_write_burst(DPCD_ADD_TRANING_LANE0_SET, 4, val);

	for (i = 0; i < 4; i++)
		eq_val[i] = val[i];

	lane_cr_done = 0;
	lane_channel_eq_done = 0;
	lane_symbol_locked_done = 0;
	interlane_align_done = 0;

	udelay((training_aux_rd_interval*4000)+400);

	displayport_reg_dpcd_read_burst(DPCD_ADD_LANE0_1_STATUS, 3, val);
	lane_cr_done |= ((val[0] & LANE0_CR_DONE) >> 0);
	lane_cr_done |= ((val[0] & LANE1_CR_DONE) >> 3);
	lane_channel_eq_done |= ((val[0] & LANE0_CHANNEL_EQ_DONE) >> 1);
	lane_channel_eq_done |= ((val[0] & LANE1_CHANNEL_EQ_DONE) >> 4);
	lane_symbol_locked_done |= ((val[0] & LANE0_SYMBOL_LOCKED) >> 2);
	lane_symbol_locked_done |= ((val[0] & LANE1_SYMBOL_LOCKED) >> 5);

	lane_cr_done |= ((val[1] & LANE2_CR_DONE) << 2);
	lane_cr_done |= ((val[1] & LANE3_CR_DONE) >> 1);
	lane_channel_eq_done |= ((val[1] & LANE2_CHANNEL_EQ_DONE) << 1);
	lane_channel_eq_done |= ((val[1] & LANE3_CHANNEL_EQ_DONE) >> 2);
	lane_symbol_locked_done |= ((val[1] & LANE2_SYMBOL_LOCKED) >> 0);
	lane_symbol_locked_done |= ((val[1] & LANE3_SYMBOL_LOCKED) >> 3);

	interlane_align_done |= (val[2] & INTERLANE_ALIGN_DONE);

	if (lane_cnt == 0x04) {
		if (lane_cr_done != 0x0F)
			goto Check_Link_rate;
	} else if (lane_cnt == 0x02) {
		if (lane_cr_done != 0x03)
			goto Check_Link_rate;
	} else {
		if (lane_cr_done != 0x01)
			goto Check_Link_rate;
	}

	displayport_info("lane_cr_done = %x\n", lane_cr_done);
	displayport_info("lane_channel_eq_done = %x\n", lane_channel_eq_done);
	displayport_info("lane_symbol_locked_done = %x\n", lane_symbol_locked_done);
	displayport_info("interlane_align_done = %x\n", interlane_align_done);

	max_link_rate = link_rate;

	if (lane_cnt == 0x04) {
		if ((lane_channel_eq_done == 0x0F) && (lane_symbol_locked_done == 0x0F)
				&& (interlane_align_done == 1)) {
			displayport_reg_set_training_pattern(NORAMAL_DATA);

			val[0] = 0x00;	/* SCRAMBLING_ENABLE, NORMAL_DATA */
			displayport_reg_dpcd_write(DPCD_ADD_TRANING_PATTERN_SET, 1, val);

			displayport_info("Full Link Training Finish - : %02x %02x\n", link_rate, lane_cnt);
			displayport_info("LANE_SET [%d] : %02x %02x %02x %02x\n",
					eq_training_retry_no, eq_val[0], eq_val[1], eq_val[2], eq_val[3]);
			return ret;
		}
	} else if (lane_cnt == 0x02) {
		if ((lane_channel_eq_done == 0x03) && (lane_symbol_locked_done == 0x03)
				&& (interlane_align_done == 1)) {
			displayport_reg_set_training_pattern(NORAMAL_DATA);

			val[0] = 0x00;	/* SCRAMBLING_ENABLE, NORMAL_DATA */
			displayport_reg_dpcd_write(DPCD_ADD_TRANING_PATTERN_SET, 1, val);

			displayport_info("Full Link Training Finish - : %02x %02x\n", link_rate, lane_cnt);
			displayport_info("LANE_SET [%d] : %02x %02x %02x %02x\n",
					eq_training_retry_no, eq_val[0], eq_val[1], eq_val[2], eq_val[3]);
			return ret;
		}
	} else {
		if ((lane_channel_eq_done == 0x01) && (lane_symbol_locked_done == 0x01)
				&& (interlane_align_done == 1)) {
			displayport_reg_set_training_pattern(NORAMAL_DATA);

			val[0] = 0x00;	/* SCRAMBLING_ENABLE, NORMAL_DATA */
			displayport_reg_dpcd_write(DPCD_ADD_TRANING_PATTERN_SET, 1, val);

			displayport_info("Full Link Training Finish - : %02x %02x\n", link_rate, lane_cnt);
			displayport_info("LANE_SET [%d] : %02x %02x %02x %02x\n",
					eq_training_retry_no, eq_val[0], eq_val[1], eq_val[2], eq_val[3]);
			return ret;
		}
	}

	if (training_retry_no == 4)
		goto Check_Link_rate;

	if (eq_training_retry_no >= 5) {
		val[0] = 0x00;	/* SCRAMBLING_ENABLE, NORMAL_DATA */
		displayport_reg_dpcd_write(DPCD_ADD_TRANING_PATTERN_SET, 1, val);
		displayport_err("Full Link Training Fail : EQ_training Retry -");
		return -EINVAL;
	}

	displayport_reg_dpcd_read_burst(DPCD_ADD_ADJUST_REQUEST_LANE0_1, 2, val);
	voltage_swing_lane[0] = (val[0] & VOLTAGE_SWING_LANE0);
	pre_emphasis_lane[0] = (val[0] & PRE_EMPHASIS_LANE0) >> 2;
	voltage_swing_lane[1] = (val[0] & VOLTAGE_SWING_LANE1) >> 4;
	pre_emphasis_lane[1] = (val[0] & PRE_EMPHASIS_LANE1) >> 6;

	voltage_swing_lane[2] = (val[1] & VOLTAGE_SWING_LANE2);
	pre_emphasis_lane[2] = (val[1] & PRE_EMPHASIS_LANE2) >> 2;
	voltage_swing_lane[3] = (val[1] & VOLTAGE_SWING_LANE3) >> 4;
	pre_emphasis_lane[3] = (val[1] & PRE_EMPHASIS_LANE3) >> 6;

	for (i = 0; i < 4; i++) {
		drive_current[i] = voltage_swing_lane[i];
		pre_emphasis[i] = pre_emphasis_lane[i];

		displayport_dbg("eq drive_current[%d] = %x\n", i, drive_current[i]);
		displayport_dbg("eq pre_emphasis[%d] = %x\n", i, pre_emphasis[i]);
	}

	eq_training_retry_no++;
	goto EQ_Training_Retry;
}

static int displayport_fast_link_training(void)
{
	u8 link_rate;
	u8 lane_cnt;
	u8 pre_emphasis[4];
	u8 drive_current[4];
	u8 max_reach_value[4];
	int i;
	u8 val;
	u8 lane_cr_done;
	u8 lane_channel_eq_done;
	u8 lane_symbol_locked_done;
	u8 interlane_align_done;
	int ret = 0;

	displayport_info("Fast Link Training start +\n");

	displayport_reg_dpcd_read(DPCD_ADD_MAX_LINK_RATE, 1, &val);
	link_rate = val;

	displayport_reg_dpcd_read(DPCD_ADD_MAX_LANE_COUNT, 1, &val);
	lane_cnt = val & MAX_LANE_COUNT;
	max_lane_cnt = lane_cnt;

	if (g_displayport_debug_param.param_used) {
		link_rate = g_displayport_debug_param.link_rate;
		lane_cnt = g_displayport_debug_param.lane_cnt;
	}

	for (i = 0; i < 4; i++) {
		pre_emphasis[i] = 1;
		drive_current[i] = 2;
	}

	displayport_reg_phy_reset(1);

	displayport_reg_set_link_bw(link_rate);

	if (displayport_reg_get_cmn_ctl_sfr_ctl_mode()) {
		displayport_reg_set_phy_clk_bw(link_rate);
		displayport_dbg("displayport_reg_set_phy_clk_bw\n");
	}

	displayport_reg_set_lane_count(lane_cnt);

	displayport_info("link_rate = %x\n", link_rate);
	displayport_info("lane_cnt = %x\n", lane_cnt);

	displayport_reg_phy_reset(0);

	displayport_reg_dpcd_write(DPCD_ADD_LINK_BW_SET, 1, &link_rate);
	displayport_reg_dpcd_write(DPCD_ADD_LANE_COUNT_SET, 1, &lane_cnt);

	displayport_reg_lane_reset(1);

	displayport_reg_set_voltage_and_pre_emphasis((u8 *)drive_current, (u8 *)pre_emphasis);
	displayport_get_voltage_and_pre_emphasis_max_reach((u8 *)drive_current,
			(u8 *)pre_emphasis, (u8 *)max_reach_value);

	displayport_reg_lane_reset(0);

	val = (pre_emphasis[0]<<3) | drive_current[0] | max_reach_value[0];
	displayport_reg_dpcd_write(DPCD_ADD_TRANING_LANE0_SET, 1, &val);
	val = (pre_emphasis[1]<<3) | drive_current[1] | max_reach_value[1];
	displayport_reg_dpcd_write(DPCD_ADD_TRANING_LANE1_SET, 1, &val);
	val = (pre_emphasis[2]<<3) | drive_current[2] | max_reach_value[2];
	displayport_reg_dpcd_write(DPCD_ADD_TRANING_LANE2_SET, 1, &val);
	val = (pre_emphasis[3]<<3) | drive_current[3] | max_reach_value[3];
	displayport_reg_dpcd_write(DPCD_ADD_TRANING_LANE3_SET, 1, &val);

	displayport_reg_wait_phy_pll_lock();

	displayport_reg_set_training_pattern(TRAINING_PATTERN_1);

	udelay(500);

	lane_cr_done = 0;

	displayport_reg_dpcd_read(DPCD_ADD_LANE0_1_STATUS, 1, &val);
	lane_cr_done |= ((val & LANE0_CR_DONE) >> 0);
	lane_cr_done |= ((val & LANE1_CR_DONE) >> 3);

	displayport_reg_dpcd_read(DPCD_ADD_LANE2_3_STATUS, 1, &val);
	lane_cr_done |= ((val & LANE2_CR_DONE) << 2);
	lane_cr_done |= ((val & LANE3_CR_DONE) >> 1);

	displayport_dbg("lane_cr_done = %x\n", lane_cr_done);

	if (lane_cnt == 0x04) {
		if (lane_cr_done != 0x0F) {
			displayport_err("Fast Link Training Fail : lane_cnt %d -", lane_cnt);
			return -EINVAL;
		}
	} else if (lane_cnt == 0x02) {
		if (lane_cr_done != 0x03) {
			displayport_err("Fast Link Training Fail : lane_cnt %d -", lane_cnt);
			return -EINVAL;
		}
	} else {
		if (lane_cr_done != 0x01) {
			displayport_err("Fast Link Training Fail : lane_cnt %d -", lane_cnt);
			return -EINVAL;
		}
	}

	displayport_reg_set_training_pattern(TRAINING_PATTERN_2);

	udelay(500);

	lane_cr_done = 0;
	lane_channel_eq_done = 0;
	lane_symbol_locked_done = 0;
	interlane_align_done = 0;

	displayport_reg_dpcd_read(DPCD_ADD_LANE0_1_STATUS, 1, &val);
	lane_cr_done |= ((val & LANE0_CR_DONE) >> 0);
	lane_cr_done |= ((val & LANE1_CR_DONE) >> 3);
	lane_channel_eq_done |= ((val & LANE0_CHANNEL_EQ_DONE) >> 1);
	lane_channel_eq_done |= ((val & LANE1_CHANNEL_EQ_DONE) >> 4);
	lane_symbol_locked_done |= ((val & LANE0_SYMBOL_LOCKED) >> 2);
	lane_symbol_locked_done |= ((val & LANE1_SYMBOL_LOCKED) >> 5);

	displayport_reg_dpcd_read(DPCD_ADD_LANE2_3_STATUS, 1, &val);
	lane_cr_done |= ((val & LANE2_CR_DONE) << 2);
	lane_cr_done |= ((val & LANE3_CR_DONE) >> 1);
	lane_channel_eq_done |= ((val & LANE2_CHANNEL_EQ_DONE) << 1);
	lane_channel_eq_done |= ((val & LANE3_CHANNEL_EQ_DONE) >> 2);
	lane_symbol_locked_done |= ((val & LANE2_SYMBOL_LOCKED) >> 0);
	lane_symbol_locked_done |= ((val & LANE3_SYMBOL_LOCKED) >> 3);

	displayport_reg_dpcd_read(DPCD_ADD_LANE_ALIGN_STATUS_UPDATE, 1, &val);
	interlane_align_done |= (val & INTERLANE_ALIGN_DONE);

	if (lane_cnt == 0x04) {
		if (lane_cr_done != 0x0F) {
			displayport_err("Fast Link Training Fail : lane_cnt %d -", lane_cnt);
			return -EINVAL;
		}
	} else if (lane_cnt == 0x02) {
		if (lane_cr_done != 0x03) {
			displayport_err("Fast Link Training Fail : lane_cnt %d -", lane_cnt);
			return -EINVAL;
		}
	} else {
		if (lane_cr_done != 0x01)
			displayport_err("Fast Link Training Fail : lane_cnt %d -", lane_cnt);
		return -EINVAL;
	}

	displayport_info("lane_cr_done = %x\n", lane_cr_done);
	displayport_info("lane_channel_eq_done = %x\n", lane_channel_eq_done);
	displayport_info("lane_symbol_locked_done = %x\n", lane_symbol_locked_done);
	displayport_info("interlane_align_done = %x\n", interlane_align_done);

	max_link_rate = link_rate;

	if (lane_cnt == 0x04) {
		if ((lane_channel_eq_done == 0x0F) && (lane_symbol_locked_done == 0x0F)
				&& (interlane_align_done == 1)) {
			displayport_reg_set_training_pattern(NORAMAL_DATA);

			val = 0x00;	/* SCRAMBLING_ENABLE, NORMAL_DATA */
			displayport_reg_dpcd_write(DPCD_ADD_TRANING_PATTERN_SET, 1, &val);

			displayport_info("Fast Link Training Finish -\n");
			return ret;
		}
	} else if (lane_cnt == 0x02) {
		if ((lane_channel_eq_done == 0x03) && (lane_symbol_locked_done == 0x03)
				&& (interlane_align_done == 1)) {
			displayport_reg_set_training_pattern(NORAMAL_DATA);

			val = 0x00;	/* SCRAMBLING_ENABLE, NORMAL_DATA */
			displayport_reg_dpcd_write(DPCD_ADD_TRANING_PATTERN_SET, 1, &val);

			displayport_info("Fast Link Training Finish -\n");
			return ret;
		}
	} else {
		if ((lane_channel_eq_done == 0x01) && (lane_symbol_locked_done == 0x01)
				&& (interlane_align_done == 1)) {
			displayport_reg_set_training_pattern(NORAMAL_DATA);

			val = 0x00;	/* SCRAMBLING_ENABLE, NORMAL_DATA */
			displayport_reg_dpcd_write(DPCD_ADD_TRANING_PATTERN_SET, 1, &val);

			displayport_info("Fast Link Training Finish -\n");
			return ret;
		}
	}

	displayport_reg_set_training_pattern(NORAMAL_DATA);

	val = 0x00;	/* SCRAMBLING_ENABLE, NORMAL_DATA */
	displayport_reg_dpcd_write(DPCD_ADD_TRANING_PATTERN_SET, 1, &val);

	displayport_err("Fast Link Training Fail -");
	return -EINVAL;
}

static int displayport_check_dfp_type(void)
{
	u8 val = 0;
	int port_type = 0;
	char *dfp[] = {"Displayport", "VGA", "HDMI", "Others"};

	displayport_reg_dpcd_read(DPCD_ADD_DOWN_STREAM_PORT_PRESENT, 1, &val);
	port_type = (val & BIT_DFP_TYPE) >> 1;
	displayport_info("DFP type: %s(0x%X)\n", dfp[port_type], val);

	return port_type;
}

static void displayport_link_sink_status_read(void)
{
	u8 val[DPCP_LINK_SINK_STATUS_FIELD_LENGTH] = {0, };

	displayport_reg_dpcd_read_burst(DPCD_ADD_SINK_COUNT,
			DPCP_LINK_SINK_STATUS_FIELD_LENGTH, val);
	displayport_info("Read link status %02x %02x %02x %02x %02x %02x\n",
			val[0], val[1], val[2], val[3], val[4], val[5]);

	displayport_reg_dpcd_read_burst(DPCD_ADD_LINK_BW_SET, 2, val);
	displayport_info("Read link rate %02x, count %02x\n", val[0], val[1]);
}

static int displayport_read_branch_revision(struct displayport_device *displayport)
{
	int ret = 0;
	u8 val[4] = {0, };

	ret = displayport_reg_dpcd_read_burst(DPCD_BRANCH_HW_REVISION, 3, val);
	if (!ret) {
		displayport_info("Branch revision: HW(0x%X), SW(0x%X, 0x%X)\n",
			val[0], val[1], val[2]);
		displayport->dex_ver[0] = val[1];
		displayport->dex_ver[1] = val[2];
	}

	return ret;
}

static int displayport_link_status_read(void)
{
	u8 val[DPCP_LINK_SINK_STATUS_FIELD_LENGTH] = {0, };

	displayport_reg_dpcd_read_burst(DPCD_ADD_SINK_COUNT,
			DPCP_LINK_SINK_STATUS_FIELD_LENGTH, val);

	displayport_info("Read link status %02x %02x %02x %02x %02x %02x\n",
			val[0], val[1], val[2], val[3], val[4], val[5]);

	if ((val[0] & val[1] & val[2] & val[3] & val[4] & val[5]) == 0xff)
		return -EINVAL;

	if (val[1] == AUTOMATED_TEST_REQUEST) {
		u8 data = 0;
		struct displayport_device *displayport = get_displayport_drvdata();

		displayport_reg_dpcd_read(DPCD_TEST_REQUEST, 1, &data);

		if ((data & TEST_EDID_READ) == TEST_EDID_READ) {
			val[0] = edid_read_checksum();
			displayport_info("TEST_EDID_CHECKSUM %02x\n", val[0]);

			displayport_reg_dpcd_write(DPCD_TEST_EDID_CHECKSUM, 1, val);

			val[0] = 0x04; /*TEST_EDID_CHECKSUM_WRITE*/
			displayport_reg_dpcd_write(DPCD_TEST_RESPONSE, 1, val);

			displayport->bist_used = 0;
		}
	}

	return 0;
}

static int displayport_link_training(void)
{
	u8 val;
	struct displayport_device *displayport = get_displayport_drvdata();
	int ret = 0;

	mutex_lock(&displayport->training_lock);

	ret = edid_update(displayport);
	if (ret < 0)
		displayport_err("failed to update edid\n");

	displayport_reg_dpcd_read(DPCD_ADD_MAX_DOWNSPREAD, 1, &val);
	displayport_dbg("DPCD_ADD_MAX_DOWNSPREAD = %x\n", val);

	if (val & NO_AUX_HANDSHAKE_LINK_TRANING)
		ret = displayport_fast_link_training();
	else
		ret = displayport_full_link_training();

	mutex_unlock(&displayport->training_lock);

	displayport_link_sink_status_read(); /* for Link CTS : (4.2.2.7) Branch Device Detection*/

	return ret;
}

static void displayport_set_switch_state(struct displayport_device *displayport, int state)
{
	if (state) {
		switch_set_state(&displayport->hpd_switch, 1);
		switch_set_state(&displayport->audio_switch, edid_audio_informs());
	} else {
		switch_set_state(&displayport->audio_switch, -1);
		switch_set_state(&displayport->hpd_switch, 0);
	}
	displayport_info("HPD status = %d\n", state);
}

void displayport_hpd_changed(int state)
{
	int ret;
	int timeout = 0;
	struct displayport_device *displayport = get_displayport_drvdata();

	mutex_lock(&displayport->hpd_lock);
	if (displayport->hpd_current_state == state) {
		displayport_info("hpd same state skip %x\n", state);
		mutex_unlock(&displayport->hpd_lock);
		return;
	}

	displayport_info("displayport hpd changed %d\n", state);
	displayport->hpd_current_state = state;

	if (state) {
		if (!wake_lock_active(&displayport->dp_wake_lock))
			wake_lock(&displayport->dp_wake_lock);

		displayport->bpc = BPC_8;	/*default setting*/
		displayport->bist_used = 0;
		displayport->bist_type = COLOR_BAR;
		displayport->dyn_range = CEA_RANGE;
		displayport->hpd_state = HPD_PLUG;
		displayport->auto_test_mode = 0;
		/* PHY power on */
		phy_power_on(displayport->phy);
		displayport_reg_init(); /* for AUX ch read/write. */
		msleep(10);

		/* for Link CTS : (4.2.2.3) EDID Read */
		if (displayport_link_status_read()) {
			displayport_err("link_status_read fail\n");
			goto HPD_FAIL;
		}
		
		if (displayport_read_branch_revision(displayport))
			displayport_err("branch_revision_read fail\n");

		displayport_info("link training in hpd_changed\n");
		ret = displayport_link_training();
		if (ret < 0) {
			displayport_dbg("link training fail\n");
			goto HPD_FAIL;
		}
		displayport->dfp_type = displayport_check_dfp_type();
		if (displayport->dfp_type != DFP_TYPE_DP &&
				displayport->dfp_type != DFP_TYPE_HDMI) {
			displayport_err("not supported DFT type\n");
			goto HPD_FAIL;
		}
		displayport_set_switch_state(displayport, 1);
		timeout = wait_event_interruptible_timeout(displayport->dp_wait,
			(displayport->state == DISPLAYPORT_STATE_ON), msecs_to_jiffies(1000));
		if (!timeout)
			displayport_err("enable timeout\n");
	} else {
		if (displayport->hdcp_ver == HDCP_VERSION_2_2) {
			hdcp_dplink_set_integrity_fail();
			displayport_hdcp22_enable(0);
		}

		cancel_delayed_work_sync(&displayport->hpd_plug_work);
		cancel_delayed_work_sync(&displayport->hpd_unplug_work);
		cancel_delayed_work_sync(&displayport->hpd_irq_work);
		cancel_delayed_work_sync(&displayport->hdcp13_work);
		cancel_delayed_work_sync(&displayport->hdcp22_work);
		cancel_delayed_work_sync(&displayport->hdcp13_integrity_check_work);
		displayport->hpd_state = HPD_UNPLUG;
		g_displayport_videoformat = v640x480p_60Hz;
		if (wake_lock_active(&displayport->dp_wake_lock))
			wake_unlock(&displayport->dp_wake_lock);
		displayport_set_switch_state(displayport, 0);
		timeout = wait_event_interruptible_timeout(displayport->dp_wait,
			(displayport->state == DISPLAYPORT_STATE_INIT), msecs_to_jiffies(1000));
		if (!timeout)
			displayport_err("disable timeout\n");
		displayport->hdcp_ver = 0;
	}
	mutex_unlock(&displayport->hpd_lock);

	return;
HPD_FAIL:
	displayport_reg_phy_off();
	phy_power_off(displayport->phy);
	if (wake_lock_active(&displayport->dp_wake_lock))
			wake_unlock(&displayport->dp_wake_lock);
	mutex_unlock(&displayport->hpd_lock);

	return;
}

void displayport_set_reconnection(void)
{
	int ret;
	struct displayport_device *displayport = get_displayport_drvdata();

	/* PHY power on */
	phy_power_on(displayport->phy);
	displayport_reg_init(); /* for AUX ch read/write. */

	displayport_info("link training in reconnection\n");
	ret = displayport_link_training();
	if (ret < 0) {
		displayport_dbg("link training fail\n");
		return;
	}
}

int displayport_get_hpd_state(void)
{
	struct displayport_device *displayport = get_displayport_drvdata();

	return displayport->hpd_current_state;
}

static void displayport_hpd_plug_work(struct work_struct *work)
{
	displayport_hpd_changed(1);
}

static void displayport_hpd_unplug_work(struct work_struct *work)
{
	displayport_hpd_changed(0);
}

static int displayport_check_dpcd_lane_status(u8 lane0_1_status,
		u8 lane2_3_status, u8 lane_align_status)
{
	u8 val[2] = {0,};
	u32 link_rate = displayport_reg_get_link_bw();
	u32 lane_cnt = displayport_reg_get_lane_count();

	displayport_reg_dpcd_read(DPCD_ADD_LINK_BW_SET, 2, val);

	displayport_info("check lane %02x %02x %02x %02x\n", link_rate, lane_cnt,
			val[0], (val[1] & MAX_LANE_COUNT));

	if ((link_rate != val[0]) || (lane_cnt != (val[1] & MAX_LANE_COUNT))) {
		displayport_err("%s(%d)\n", __func__, __LINE__);
		return -EINVAL;
	}

	if ((lane_align_status & INTERLANE_ALIGN_DONE) != INTERLANE_ALIGN_DONE) {
		displayport_err("%s(%d)\n", __func__, __LINE__);
		return -EINVAL;
	}

	if ((lane_align_status & LINK_STATUS_UPDATE) == LINK_STATUS_UPDATE) {
		displayport_err("%s(%d)\n", __func__, __LINE__);
		return -EINVAL;
	}

	if (lane_cnt >= 1) {
		if ((lane0_1_status & (LANE0_CR_DONE | LANE0_CHANNEL_EQ_DONE | LANE0_SYMBOL_LOCKED))
				!= (LANE0_CR_DONE | LANE0_CHANNEL_EQ_DONE | LANE0_SYMBOL_LOCKED)) {
			displayport_err("%s(%d)\n", __func__, __LINE__);
			return -EINVAL;
		}
	}

	if (lane_cnt >= 2) {
		if ((lane0_1_status & (LANE1_CR_DONE | LANE1_CHANNEL_EQ_DONE | LANE1_SYMBOL_LOCKED))
				!= (LANE1_CR_DONE | LANE1_CHANNEL_EQ_DONE | LANE1_SYMBOL_LOCKED)) {
			displayport_err("%s(%d)\n", __func__, __LINE__);
			return -EINVAL;
		}
	}

	if (lane_cnt == 4) {
		if ((lane2_3_status & (LANE2_CR_DONE | LANE2_CHANNEL_EQ_DONE | LANE2_SYMBOL_LOCKED))
				!= (LANE2_CR_DONE | LANE2_CHANNEL_EQ_DONE | LANE2_SYMBOL_LOCKED)) {
			displayport_err("%s(%d)\n", __func__, __LINE__);
			return -EINVAL;
		}

		if ((lane2_3_status & (LANE3_CR_DONE | LANE3_CHANNEL_EQ_DONE | LANE3_SYMBOL_LOCKED))
				!= (LANE3_CR_DONE | LANE3_CHANNEL_EQ_DONE | LANE3_SYMBOL_LOCKED)) {
			displayport_err("%s(%d)\n", __func__, __LINE__);
			return -EINVAL;
		}
	}

	return 0;
}

static int displayport_Automated_Test_Request(void)
{
	u8 data = 0;
	u8 val[DPCP_LINK_SINK_STATUS_FIELD_LENGTH] = {0, };
	struct displayport_device *displayport = get_displayport_drvdata();

	displayport_reg_dpcd_read(DPCD_TEST_REQUEST, 1, &data);
	displayport_info("TEST_REQUEST %02x\n", data);

	displayport->auto_test_mode = 1;
	val[0] = 0x01; /*TEST_ACK*/
	displayport_reg_dpcd_write(DPCD_TEST_RESPONSE, 1, val);

	if ((data & TEST_LINK_TRAINING) == TEST_LINK_TRAINING) {
		displayport_reg_dpcd_read(DPCD_TEST_LINK_RATE, 1, val);
		displayport_info("TEST_LINK_RATE %02x\n", val[0]);
		g_displayport_debug_param.link_rate = (val[0]&TEST_LINK_RATE);

		displayport_reg_dpcd_read(DPCD_TEST_LANE_COUNT, 1, val);
		displayport_info("TEST_LANE_COUNT %02x\n", val[0]);
		g_displayport_debug_param.lane_cnt = (val[0]&TEST_LANE_COUNT);

		g_displayport_debug_param.param_used = 1;

		displayport_reg_init();
		displayport_link_training();

		g_displayport_debug_param.param_used = 0;
	} else if ((data & TEST_VIDEO_PATTERN) == TEST_VIDEO_PATTERN) {
		u16 hactive, vactive, fps, vmode;
		int edid_preset;

		displayport_set_switch_state(displayport, 0);

		msleep(300);

		/* PHY power on */
		phy_power_on(displayport->phy);
		displayport_reg_init(); /* for AUX ch read/write. */

		g_displayport_debug_param.param_used = 1;

		displayport_link_training();

		g_displayport_debug_param.param_used = 0;

		displayport_reg_dpcd_read(DPCD_TEST_PATTERN, 1, val);
		displayport_info("TEST_PATTERN %02x\n", val[0]);

		displayport_reg_dpcd_read(DPCD_TEST_H_WIDTH_1, 2, val);
		hactive = val[0];
		hactive = (hactive<<8)|val[1];
		displayport_info("TEST_H_WIDTH %d\n", hactive);

		displayport_reg_dpcd_read(DPCD_TEST_V_HEIGHT_1, 2, val);
		vactive = val[0];
		vactive = (vactive<<8)|val[1];
		displayport_info("TEST_V_HEIGHT %d\n", vactive);

		displayport_reg_dpcd_read(DPCD_TEST_MISC_1, 1, val);
		displayport_info("TEST_MISC_1 %02x", val[0]);
		displayport->dyn_range = (val[0] & TEST_DYNAMIC_RANGE)?CEA_RANGE:VESA_RANGE;
		displayport->bpc = (val[0] & TEST_BIT_DEPTH)?BPC_8:BPC_6;

		displayport_reg_dpcd_read(DPCD_TEST_MISC_2, 1, val);
		displayport_info("TEST_MISC_2 %02x\n", val[0]);
		vmode = (val[0]&TEST_INTERLACED);

		displayport_reg_dpcd_read(DPCD_TEST_REFRESH_RATE_NUMERATOR, 1, val);
		fps = val[0];
		displayport_info("TEST_REFRESH_RATE_NUMERATOR %02x", fps);
		displayport_info("%d %d %d %d dyn:%d %dbpc\n", hactive, vactive, fps, vmode,
				displayport->dyn_range, (displayport->bpc == BPC_6)?6:8);

		displayport->bist_used = 1;
		displayport->bist_type = COLOR_BAR;

		edid_preset = edid_find_resolution(hactive, vactive, fps, vmode);
		edid_set_preferred_preset(edid_preset);

		displayport_set_switch_state(displayport, 1);

	} else if ((data & TEST_PHY_TEST_PATTERN) == TEST_PHY_TEST_PATTERN) {
		displayport_reg_stop();
		msleep(120);
		displayport_reg_dpcd_read(DPCD_ADD_ADJUST_REQUEST_LANE0_1, 2, val);
		displayport_info("ADJUST_REQUEST_LANE0_1 %02x %02x\n", val[0], val[1]);

		displayport_reg_dpcd_read(DCDP_ADD_PHY_TEST_PATTERN, 4, val);
		displayport_info("PHY_TEST_PATTERN %02x %02x %02x %02x\n", val[0], val[1], val[2], val[3]);

		switch (val[0]) {
		case DISABLE_PATTEN:
			displayport_reg_set_qual_pattern(DISABLE_PATTEN, ENABLE_SCRAM);
			break;
		case D10_2_PATTERN:
			displayport_reg_set_qual_pattern(D10_2_PATTERN, DISABLE_SCRAM);
			break;
		case SERP_PATTERN:
			displayport_reg_set_qual_pattern(SERP_PATTERN, ENABLE_SCRAM);
			break;
		case PRBS7:
			displayport_reg_set_qual_pattern(PRBS7, DISABLE_SCRAM);
			break;
		case CUSTOM_80BIT:
			displayport_reg_set_pattern_PLTPAT();
			displayport_reg_set_qual_pattern(CUSTOM_80BIT, DISABLE_SCRAM);
			break;
		case HBR2_COMPLIANCE:
			/*option 0*/
			/*displayport_reg_set_hbr2_scrambler_reset(252);*/

			/*option 1*/
			displayport_reg_set_hbr2_scrambler_reset(252*2);

			/*option 2*/
			/*displayport_reg_set_hbr2_scrambler_reset(252);*/
			/*displayport_reg_set_PN_Inverse_PHY_Lane(1);*/

			/*option 3*/
			/*displayport_reg_set_hbr2_scrambler_reset(252*2);*/
			/*displayport_reg_set_PN_Inverse_PHY_Lane(1);*/

			displayport_reg_set_qual_pattern(HBR2_COMPLIANCE, ENABLE_SCRAM);
			break;
		default:
			displayport_err("not supported link qual pattern");
			break;
		}
	} else if ((data & TEST_AUDIO_PATTERN) == TEST_AUDIO_PATTERN) {
		struct displayport_audio_config_data audio_config_data;

		displayport_reg_dpcd_read(DPCD_TEST_AUDIO_MODE, 1, val);
		displayport_info("TEST_AUDIO_MODE %02x %02x\n",
				(val[0] & TEST_AUDIO_SAMPLING_RATE),
				(val[0] & TEST_AUDIO_CHANNEL_COUNT) >> 4);

		displayport_reg_dpcd_read(DPCD_TEST_AUDIO_PATTERN_TYPE, 1, val);
		displayport_info("TEST_AUDIO_PATTERN_TYPE %02x\n", val[0]);

		msleep(300);

		audio_config_data.audio_enable = 1;
		audio_config_data.audio_fs =  (val[0] & TEST_AUDIO_SAMPLING_RATE);
		audio_config_data.audio_channel_cnt = (val[0] & TEST_AUDIO_CHANNEL_COUNT) >> 4;
		audio_config_data.audio_channel_cnt++;
		displayport_audio_bist_enable(audio_config_data);

	} else {

		displayport_err("Not Supported AUTOMATED_TEST_REQUEST\n");
		return -EINVAL;
	}
	return 0;
}

static int displayport_hdcp22_irq_handler(void)
{
	struct displayport_device *displayport = get_displayport_drvdata();
	uint8_t rxstatus = 0;
	int ret = 0;

	hdcp_dplink_get_rxstatus(&rxstatus);

	if (rxstatus & DPCD_HDCP22_RXSTATUS_LINK_INTEGRITY_FAIL) {
		/* hdcp22 disable while re-authentication */
		ret = hdcp_dplink_set_integrity_fail();
		displayport_hdcp22_enable(0);

		queue_delayed_work(displayport->hdcp2_wq, &displayport->hdcp22_work,
				msecs_to_jiffies(2000));
	} else if (rxstatus & DPCD_HDCP22_RXSTATUS_REAUTH_REQ) {
		/* hdcp22 disable while re-authentication */
		ret = hdcp_dplink_set_reauth();
		displayport_hdcp22_enable(0);

		queue_delayed_work(displayport->hdcp2_wq, &displayport->hdcp22_work,
				msecs_to_jiffies(1000));
	} else if (rxstatus & DPCD_HDCP22_RXSTATUS_PAIRING_AVAILABLE) {
		/* set pairing avaible flag */
		ret = hdcp_dplink_set_paring_available();
	} else if (rxstatus & DPCD_HDCP22_RXSTATUS_HPRIME_AVAILABLE) {
		/* set hprime avaible flag */
		ret = hdcp_dplink_set_hprime_available();
	} else if (rxstatus & DPCD_HDCP22_RXSTATUS_READY) {
		/* set ready avaible flag */
		/* todo update stream Management */
		ret = hdcp_dplink_set_rp_ready();
	} else {
		displayport_info("undefined RxStatus(0x%x). ignore\n", rxstatus);
		ret = 0;
	}

	return ret;
}

static void displayport_hpd_irq_work(struct work_struct *work)
{
	u8 val[DPCP_LINK_SINK_STATUS_FIELD_LENGTH] = {0, };
	struct displayport_device *displayport = get_displayport_drvdata();

	if (!displayport->hpd_current_state) {
		displayport_info("HPD IRQ work: hpd is low\n");
		return;
	}

	displayport->hpd_state = HPD_IRQ;
	displayport_dbg("detect HPD_IRQ\n");

	if (displayport->hdcp_ver == HDCP_VERSION_2_2) {
		int ret = displayport_reg_dpcd_read_burst(DPCD_ADD_SINK_COUNT,
				DPCP_LINK_SINK_STATUS_FIELD_LENGTH, val);

		displayport_info("HPD IRQ work %02x %02x %02x %02x %02x %02x\n",
				val[0], val[1], val[2], val[3], val[4], val[5]);
		if (ret < 0 || (val[0] & val[1] & val[2] & val[3] & val[4] & val[5]) == 0xff) {
			displayport_info("dpcd_read error in HPD IRQ work\n");
			return;
		}

		if ((val[1] & CP_IRQ) == CP_IRQ) {
			displayport_info("hdcp22: detect CP_IRQ\n");
			displayport_hdcp22_irq_handler();
		} else {
			displayport_info("hdcp22: detect hpd_irq!!!!\n");

			if ((val[1] & AUTOMATED_TEST_REQUEST) == AUTOMATED_TEST_REQUEST) {
				if (displayport_Automated_Test_Request() == 0)
					return;
			}

			if (displayport_check_dpcd_lane_status(val[2], val[3], val[4]) != 0) {
				displayport_info("link training in HPD IRQ work\n");
				displayport_link_training();
			}
		}
		return;
	}

	if (hdcp13_info.auth_state != HDCP13_STATE_AUTH_PROCESS) {
		int ret = displayport_reg_dpcd_read_burst(DPCD_ADD_SINK_COUNT,
				DPCP_LINK_SINK_STATUS_FIELD_LENGTH, val);

		displayport_info("HPD IRQ work %02x %02x %02x %02x %02x %02x\n",
				val[0], val[1], val[2], val[3], val[4], val[5]);
		if (ret < 0 || (val[0] & val[1] & val[2] & val[3] & val[4] & val[5]) == 0xff) {
			displayport_info("dpcd_read error in HPD IRQ work\n");
			return;
		}

		if ((val[1] & CP_IRQ) == CP_IRQ && displayport->hdcp_ver == HDCP_VERSION_1_3) {
			displayport_info("detect CP_IRQ\n");
			hdcp13_info.cp_irq_flag = 1;
			hdcp13_info.link_check = LINK_CHECK_NEED;
			HDCP13_Link_integrity_check();

			if (hdcp13_info.auth_state == HDCP13_STATE_FAIL) {
				queue_delayed_work(displayport->dp_wq,
					&displayport->hdcp13_work, msecs_to_jiffies(2000));
			}
			return;
		}

		if ((val[1] & AUTOMATED_TEST_REQUEST) == AUTOMATED_TEST_REQUEST) {
			if (displayport_Automated_Test_Request() == 0)
				return;
		}

		if (displayport_check_dpcd_lane_status(val[2], val[3], val[4]) != 0) {
			displayport_info("link training in HPD IRQ work\n");
			displayport_link_training();
		}
	} else {
		displayport_reg_dpcd_read(DPCD_ADD_DEVICE_SERVICE_IRQ_VECTOR, 1, val);

		if ((val[0] & CP_IRQ) == CP_IRQ) {
			displayport_info("detect CP_IRQ\n");
			hdcp13_info.cp_irq_flag = 1;
			displayport_reg_dpcd_read(ADDR_HDCP13_BSTATUS, 1, HDCP13_DPCD.HDCP13_BSTATUS);
		}
	}
}

static void displayport_hdcp13_integrity_check_work(struct work_struct *work)
{
	struct displayport_device *displayport = get_displayport_drvdata();
	
	if (displayport->hdcp_ver == HDCP_VERSION_1_3) {
		hdcp13_info.link_check = LINK_CHECK_NEED;
		HDCP13_Link_integrity_check();
	}
}

static irqreturn_t displayport_irq_handler(int irq, void *dev_data)
{
	struct displayport_device *displayport = dev_data;
	u32 irq_status_reg;

	spin_lock(&displayport->slock);

	if (displayport->state != DISPLAYPORT_STATE_ON)
		goto irq_end;

	irq_status_reg = displayport_reg_get_interrupt_and_clear(DP_Interrupt_Status);

	if (irq_status_reg & HOT_PLUG_DET) {
		displayport->hpd_state = HPD_IRQ;
		/*queue_delayed_work(displayport->dp_wq, &displayport->hpd_irq_work, 0);*/
		displayport_info("HPD IRQ detect\n");
	}

	irq_status_reg = displayport_reg_get_interrupt_and_clear(Common_Interrupt_Status_2);

	if (irq_status_reg & HDCP_LINK_CHK_FAIL) {
		queue_delayed_work(displayport->dp_wq, &displayport->hdcp13_integrity_check_work, 0);
		displayport_info("HDCP_LINK_CHK detect\n");
	}

	if (irq_status_reg & R0_CHECK_FLAG) {
		hdcp13_info.r0_read_flag = 1;
		displayport_info("R0_CHECK_FLAG detect\n");
	}

	irq_status_reg = displayport_reg_get_interrupt_and_clear(Common_Interrupt_Status_4);

	if (irq_status_reg & HPD_CHG)
		displayport_info("HPD_CHG detect\n");

	if (irq_status_reg & HPD_LOST) {
		/*queue_delayed_work(displayport->dp_wq, &displayport->hpd_unplug_work, 0);*/
		displayport_info("HPD_LOST detect\n");
	}

	if (irq_status_reg & PLUG) {
		/*queue_delayed_work(displayport->dp_wq, &displayport->hpd_plug_work, 0);*/
		displayport_info("HPD_PLUG detect\n");
	}

irq_end:
	spin_unlock(&displayport->slock);

	return IRQ_HANDLED;
}

static u8 displayport_get_vic(void)
{
	return videoformat_parameters[g_displayport_videoformat].vic;
}

static int displayport_make_avi_infoframe_data(struct infoframe *avi_infoframe)
{
	int i;

	avi_infoframe->type_code = INFOFRAME_PACKET_TYPE_AVI;
	avi_infoframe->version_number = AVI_INFOFRAME_VERSION;
	avi_infoframe->length = AVI_INFOFRAME_LENGTH;

	for (i = 0; i < AVI_INFOFRAME_LENGTH; i++)
		avi_infoframe->data[i] = 0x00;

	avi_infoframe->data[0] |= ACTIVE_FORMAT_INFOMATION_PRESENT;
	avi_infoframe->data[1] |= ACITVE_PORTION_ASPECT_RATIO;
	avi_infoframe->data[3] = displayport_get_vic();

	return 0;
}

static int displayport_make_audio_infoframe_data(struct infoframe *audio_infoframe,
		struct displayport_audio_config_data *audio_config_data)
{
	int i;

	audio_infoframe->type_code = INFOFRAME_PACKET_TYPE_AUDIO;
	audio_infoframe->version_number = AUDIO_INFOFRAME_VERSION;
	audio_infoframe->length = AUDIO_INFOFRAME_LENGTH;

	for (i = 0; i < AUDIO_INFOFRAME_LENGTH; i++)
		audio_infoframe->data[i] = 0x00;

	/* Data Byte 1, PCM type and audio channel count */
	audio_infoframe->data[0] = ((u8)audio_config_data->audio_channel_cnt - 1);

	/* Data Byte 4, how various speaker locations are allocated */
	audio_infoframe->data[3] = 0;

	displayport_info("audio_infoframe: type and ch_cnt %02x, SF and bit size %02x, ch_allocation %02x\n",
			audio_infoframe->data[0], audio_infoframe->data[1], audio_infoframe->data[3]);

	return 0;
}

static int displayport_set_avi_infoframe(void)
{
	struct infoframe avi_infoframe;

	displayport_make_avi_infoframe_data(&avi_infoframe);
	displayport_reg_set_avi_infoframe(avi_infoframe);

	return 0;
}

static int displayport_set_audio_infoframe(struct displayport_audio_config_data *audio_config_data)
{
	struct infoframe audio_infoframe;

	displayport_make_audio_infoframe_data(&audio_infoframe, audio_config_data);
	displayport_reg_set_audio_infoframe(audio_infoframe, audio_config_data->audio_enable);

	return 0;
}

static int displayport_set_audio_ch_status(struct displayport_audio_config_data *audio_config_data)
{
	displayport_reg_set_ch_status_sw_audio_coding(1);
	displayport_reg_set_ch_status_ch_cnt(audio_config_data->audio_channel_cnt);
	displayport_reg_set_ch_status_word_length(audio_config_data->audio_bit);
	displayport_reg_set_ch_status_sampling_frequency(audio_config_data->audio_fs);
	displayport_reg_set_ch_status_clock_accuracy(NOT_MATCH);

	return 0;
}

int displayport_audio_config(struct displayport_audio_config_data *audio_config_data)
{
	int ret = 0;

	displayport_info("audio en:%d, ch:%d, fs:%d, bit:%d, packed:%d, word_len:%d\n",
			audio_config_data->audio_enable, audio_config_data->audio_channel_cnt,
			audio_config_data->audio_fs, audio_config_data->audio_bit,
			audio_config_data->audio_packed_mode, audio_config_data->audio_word_length);

	/* channel mapping: FL, FR, C, SW, RL, RR */
	displayport_write(Audio_Packet_Data_Re_arrangement_Register, 0x87653421);
	displayport_dbg("audio channel mapping = 0x%X\n",
			displayport_read(Audio_Packet_Data_Re_arrangement_Register));

	displayport_reg_set_audio_m_n(SYNC_MODE, audio_config_data->audio_fs);
	displayport_reg_set_audio_function_enable(audio_config_data->audio_enable);
	displayport_reg_set_audio_master_mode();
	displayport_reg_set_dma_burst_size(audio_config_data->audio_word_length);
	displayport_reg_set_dma_pack_mode(audio_config_data->audio_packed_mode);
	displayport_reg_set_pcm_size(audio_config_data->audio_bit);
	displayport_reg_set_audio_ch(audio_config_data->audio_channel_cnt);
	displayport_reg_set_audio_fifo_function_enable(audio_config_data->audio_enable);
	displayport_reg_set_audio_ch_status_same(1);
	displayport_reg_set_audio_sampling_frequency(audio_config_data->audio_fs);
	displayport_reg_set_dp_audio_enable(audio_config_data->audio_enable);
	displayport_set_audio_infoframe(audio_config_data);
	displayport_set_audio_ch_status(audio_config_data);
	displayport_reg_set_audio_master_mode_enable(audio_config_data->audio_enable);

	return ret;
}
EXPORT_SYMBOL(displayport_audio_config);

int displayport_audio_bist_enable(struct displayport_audio_config_data audio_config_data)
{
	int ret = 0;

	displayport_info("displayport_audio_bist\n");
	displayport_info("audio_enable = %d\n", audio_config_data.audio_enable);
	displayport_info("audio_channel_cnt = %d\n", audio_config_data.audio_channel_cnt);
	displayport_info("audio_fs = %d\n", audio_config_data.audio_fs);

	displayport_reg_set_audio_m_n(SYNC_MODE, audio_config_data.audio_fs);
	displayport_reg_set_audio_function_enable(audio_config_data.audio_enable);
	displayport_reg_set_audio_master_mode();

	displayport_reg_set_audio_ch(audio_config_data.audio_channel_cnt);
	displayport_reg_set_audio_fifo_function_enable(audio_config_data.audio_enable);
	displayport_reg_set_audio_ch_status_same(1);
	displayport_reg_set_audio_sampling_frequency(audio_config_data.audio_fs);
	displayport_reg_set_dp_audio_enable(audio_config_data.audio_enable);
	displayport_reg_set_audio_bist_mode(1);
	displayport_set_audio_infoframe(&audio_config_data);
	displayport_set_audio_ch_status(&audio_config_data);
	displayport_reg_set_audio_master_mode_enable(audio_config_data.audio_enable);

	return ret;
}

int displayport_dpcd_read_for_hdcp22(u32 address, u32 length, u8 *data)
{
	int ret;

	ret = displayport_reg_dpcd_read_burst(address, length, data);

	if (ret != 0)
		displayport_err("dpcd_read_for_hdcp22 fail: 0x%Xn", address);

	return ret;
}

int displayport_dpcd_write_for_hdcp22(u32 address, u32 length, u8 *data)
{
	int ret;

	ret = displayport_reg_dpcd_write_burst(address, length, data);

	if (ret != 0)
		displayport_err("dpcd_write_for_hdcp22 fail: 0x%X\n", address);

	return ret;
}

void displayport_hdcp22_enable(u32 en)
{
	if (en) {
		displayport_reg_set_hdcp22_lane_count();
		displayport_reg_set_hdcp22_system_enable(1);
		displayport_reg_set_hdcp22_mode(1);
		displayport_reg_set_hdcp22_encryption_enable(1);
	} else {
		displayport_reg_set_hdcp22_system_enable(0);
		displayport_reg_set_hdcp22_mode(0);
		displayport_reg_set_hdcp22_encryption_enable(0);
	}
}

static void displayport_hdcp13_run(struct work_struct *work)
{
	struct displayport_device *displayport = get_displayport_drvdata();

	if (displayport->hdcp_ver != HDCP_VERSION_1_3 ||
			!displayport->hpd_current_state)
		return;

	displayport_dbg("[HDCP 1.3] run\n");
	HDCP13_run();
	if (hdcp13_info.auth_state == HDCP13_STATE_FAIL) {
		queue_delayed_work(displayport->dp_wq, &displayport->hdcp13_work,
				msecs_to_jiffies(2000));
	}
}

static void displayport_hdcp22_run(struct work_struct *work)
{
	u8 val[2] = {0, };

	if (hdcp_dplink_authenticate() != 0)
		displayport_reg_video_mute(1);
	else
		displayport_reg_video_mute(0);

	displayport_dpcd_read_for_hdcp22(DPCD_HDCP22_RX_INFO, 2, val);
	displayport_info("HDCP2.2 rx_info: 0:0x%X, 8:0x%X\n", val[1], val[0]);
}

static int displayport_check_hdcp_version(void)
{
	int ret = 0;
	u8 val[DPCD_HDCP22_RX_CAPS_LENGTH];
	u32 rx_caps = 0;
	int i;

	HDCP13_DPCD_BUFFER();

	if (HDCP13_Read_Bcap() != 0)
		displayport_dbg("[HDCP 1.3] NONE HDCP CAPABLE\n");
#if defined(HDCP_SUPPORT)
	else
		ret = HDCP_VERSION_1_3;
#endif
	displayport_dpcd_read_for_hdcp22(DPCD_HDCP22_RX_CAPS, DPCD_HDCP22_RX_CAPS_LENGTH, val);

	for (i = 0; i < DPCD_HDCP22_RX_CAPS_LENGTH; i++)
		rx_caps |= (u32)val[i] << ((DPCD_HDCP22_RX_CAPS_LENGTH - (i + 1)) * 8);

	displayport_info("HDCP2.2 rx_caps = 0x%x\n", rx_caps);

	if ((((rx_caps & VERSION) >> DPCD_HDCP_VERSION_BIT_POSITION) == (HDCP_VERSION_2_2))
			&& ((rx_caps & HDCP_CAPABLE) != 0)) {
#if defined(HDCP_2_2)
		ret = HDCP_VERSION_2_2;
#endif
		displayport_dbg("displayport_rx supports hdcp2.2\n");
	}

	return ret;
}

static void hdcp_start(struct displayport_device *displayport)
{
	displayport->hdcp_ver = displayport_check_hdcp_version();
#if defined(HDCP_SUPPORT)
	if (displayport->hdcp_ver == HDCP_VERSION_2_2)
		queue_delayed_work(displayport->hdcp2_wq, &displayport->hdcp22_work,
				msecs_to_jiffies(3500));
	else if (displayport->hdcp_ver == HDCP_VERSION_1_3)
		queue_delayed_work(displayport->dp_wq, &displayport->hdcp13_work,
						msecs_to_jiffies(4500));
	else
		displayport_info("HDCP is not supported\n");
#endif
}

static int displayport_enable(struct displayport_device *displayport)
{
	int ret = 0;
	u8 bpc = (u8)displayport->bpc;
	u8 bist_type = (u8)displayport->bist_type;
	u8 dyn_range = (u8)displayport->dyn_range;

	if (displayport->state == DISPLAYPORT_STATE_ON)
		return 0;

	displayport_info("displayport_enable\n");

	displayport_reg_set_pixel_clock(g_displayport_videoformat);
#if defined(CONFIG_PM_RUNTIME)
	pm_runtime_get_sync(displayport->dev);
#else
	displayport_runtime_resume(displayport->dev);
#endif
	/*enable_irq(displayport->res.irq);*/

	if (displayport->bist_used)
		displayport_reg_set_bist_video_configuration(g_displayport_videoformat,
				bpc, bist_type, dyn_range);
	else {
		if (displayport->dfp_type != DFP_TYPE_DP)
			bpc = BPC_8;

		displayport_reg_set_video_configuration(bpc);
	}

	displayport_set_avi_infoframe();
#ifdef HDCP_SUPPORT
	displayport_reg_video_mute(0);
#endif
	displayport_reg_start();

	displayport->state = DISPLAYPORT_STATE_ON;
	wake_up_interruptible(&displayport->dp_wait);
	hdcp_start(displayport);

	return ret;
}

static int displayport_disable(struct displayport_device *displayport)
{
	if (displayport->state != DISPLAYPORT_STATE_ON)
		return 0;

	/* Wait for current read & write CMDs. */
	mutex_lock(&displayport->cmd_lock);
	displayport->state = DISPLAYPORT_STATE_OFF;
	hdcp13_info.auth_state = HDCP13_STATE_NOT_AUTHENTICATED;
	mutex_unlock(&displayport->cmd_lock);

	displayport_reg_set_video_bist_mode(0);
	displayport_reg_stop();
	disable_irq(displayport->res.irq);

	displayport_reg_phy_off();
	phy_power_off(displayport->phy);

#if defined(CONFIG_PM_RUNTIME)
	pm_runtime_put_sync(displayport->dev);
#else
	displayport_runtime_suspend(displayport->dev);
#endif
	displayport->state = DISPLAYPORT_STATE_INIT;
	wake_up_interruptible(&displayport->dp_wait);
	displayport_info("displayport_disable\n");
	
	return 0;
}

bool displayport_match_timings(const struct v4l2_dv_timings *t1,
		const struct v4l2_dv_timings *t2,
		unsigned pclock_delta)
{
	if (t1->type != t2->type)
		return false;

	if (t1->bt.width == t2->bt.width &&
			t1->bt.height == t2->bt.height &&
			t1->bt.interlaced == t2->bt.interlaced &&
			t1->bt.polarities == t2->bt.polarities &&
			t1->bt.pixelclock >= t2->bt.pixelclock - pclock_delta &&
			t1->bt.pixelclock <= t2->bt.pixelclock + pclock_delta &&
			t1->bt.hfrontporch == t2->bt.hfrontporch &&
			t1->bt.vfrontporch == t2->bt.vfrontporch &&
			t1->bt.vsync == t2->bt.vsync &&
			t1->bt.vbackporch == t2->bt.vbackporch &&
			(!t1->bt.interlaced ||
			 (t1->bt.il_vfrontporch == t2->bt.il_vfrontporch &&
			  t1->bt.il_vsync == t2->bt.il_vsync &&
			  t1->bt.il_vbackporch == t2->bt.il_vbackporch)))
		return true;

	return false;
}

bool displayport_match_video_params(u32 i, u32 j)
{
	if (displayport_supported_presets[i].xres == videoformat_parameters[j].active_pixel &&
			displayport_supported_presets[i].yres == videoformat_parameters[j].active_line &&
			displayport_supported_presets[i].refresh == videoformat_parameters[j].fps)
		return true;

	return false;
}

static int displayport_timing2conf(struct v4l2_dv_timings *timings)
{
	int i, j;

	for (i = 0; i < displayport_pre_cnt; i++) {
		if (displayport_match_timings(&displayport_supported_presets[i].dv_timings,
					timings, 0)) {
			for (j = 0; j < videoformat_parameters_cnt; j++) {
				if (displayport_match_video_params(i, j))
					return j; /* eum matching between displayport_conf and videoformat */
			}
		}
	}

	return -EINVAL;
}

static int displayport_s_dv_timings(struct v4l2_subdev *sd,
		struct v4l2_dv_timings *timings)
{
	struct displayport_device *displayport = container_of(sd, struct displayport_device, sd);
	struct decon_device *decon = get_decon_drvdata(2);
	videoformat displayport_setting_videoformat;
	int ret = 0;

	v4l2_print_dv_timings("Displayport:", " set ", timings, false);
	ret = displayport_timing2conf(timings);
	if (ret < 0) {
		displayport_err("displayport timings not supported\n");
		return -EINVAL;
	}
	displayport_setting_videoformat = ret;

	if (displayport->bist_used == 0) {
		displayport->bpc = BPC_8;
		/*fail safe mode (640x480) with 6 bpc*/
		if (displayport_setting_videoformat == v640x480p_60Hz)
			displayport->bpc = BPC_6;
	}

	g_displayport_videoformat = displayport_setting_videoformat;
	displayport->cur_timings = *timings;

	decon_displayport_get_out_sd(decon);

	displayport_dbg("New g_displayport_videoformat = %d\n", g_displayport_videoformat);

	return 0;
}

static int displayport_g_dv_timings(struct v4l2_subdev *sd,
		struct v4l2_dv_timings *timings)
{
	struct displayport_device *displayport = container_of(sd, struct displayport_device, sd);

	*timings = displayport->cur_timings;

	displayport_dbg("displayport_g_dv_timings\n");

	return 0;
}

static u64 displayport_get_max_pixelclock(void)
{
	u64 pc;
	u64 pc1lane;

	if (max_link_rate == LINK_RATE_5_4Gbps) {
		pc1lane = 180000000;
	} else if (max_link_rate == LINK_RATE_2_7Gbps) {
		pc1lane = 90000000;
	} else {/* LINK_RATE_1_62Gbps */
		pc1lane = 54000000;
	}

	pc = max_lane_cnt * pc1lane;

	return pc;
}

static int displayport_enum_dv_timings(struct v4l2_subdev *sd,
		struct v4l2_enum_dv_timings *timings)
{
	struct displayport_device *displayport = container_of(sd, struct displayport_device, sd);

	if (timings->index >= displayport_pre_cnt) {
		displayport_dbg("displayport_enum_dv_timings -EOVERFLOW\n");
		return -E2BIG;
	}

	/* reduce the timing by lane count and link rate */
	if (displayport_supported_presets[timings->index].dv_timings.bt.pixelclock >
			displayport_get_max_pixelclock()) {
		displayport_info("Max pixelclock = %llu, lane:%d, rate:0x%x\n",
				displayport_get_max_pixelclock(), max_lane_cnt, max_link_rate);
		return -E2BIG;
	}

	if ((check_dex_support(displayport)) && reduced_resolution && reduced_resolution < 
			displayport_supported_presets[timings->index].dv_timings.bt.pixelclock) {
		displayport_info("reduced_resolution: %llu\n", reduced_resolution);
		return -E2BIG;
	}

	if (displayport_supported_presets[timings->index].edid_support_match) {
		displayport_info("matched %d(%s)\n", timings->index,
				displayport_supported_presets[timings->index].name);
		timings->timings = displayport_supported_presets[timings->index].dv_timings;
	} else {
		return -EINVAL;
	}

	return 0;
}

static int displayport_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct displayport_device *displayport = container_of(sd, struct displayport_device, sd);

	if (enable)
		return displayport_enable(displayport);
	else
		return displayport_disable(displayport);
}

static long displayport_ioctl(struct v4l2_subdev *sd, unsigned int cmd, void *arg)
{
	struct displayport_device *displayport = container_of(sd, struct displayport_device, sd);
	int ret = 0;
	struct v4l2_enum_dv_timings *enum_timings;

	switch (cmd) {
	case DISPLAYPORT_IOC_DUMP:
		displayport_dump_registers(displayport);
		break;

	case DISPLAYPORT_IOC_GET_ENUM_DV_TIMINGS:
		enum_timings = (struct v4l2_enum_dv_timings *)arg;

		ret = displayport_enum_dv_timings(sd, enum_timings);
		break;

	case DISPLAYPORT_IOC_SET_RECONNECTION:	/* for restart without hpd change */
		displayport_set_reconnection();
		displayport_info("DISPLAYPORT_IOC_SET_RECONNECTION ioctl\n");
		break;

	default:
		displayport_err("unsupported ioctl");
		ret = -EINVAL;
		break;
	}

	return ret;
}

static const struct v4l2_subdev_core_ops displayport_sd_core_ops = {
	.ioctl = displayport_ioctl,
};

static const struct v4l2_subdev_video_ops displayport_sd_video_ops = {
	.s_dv_timings = displayport_s_dv_timings,
	.g_dv_timings = displayport_g_dv_timings,
	.s_stream = displayport_s_stream,
};

static const struct v4l2_subdev_ops displayport_subdev_ops = {
	.core = &displayport_sd_core_ops,
	.video = &displayport_sd_video_ops,
};

static void displayport_init_subdev(struct displayport_device *displayport)
{
	struct v4l2_subdev *sd = &displayport->sd;

	v4l2_subdev_init(sd, &displayport_subdev_ops);
	sd->owner = THIS_MODULE;
	snprintf(sd->name, sizeof(sd->name), "%s", "displayport-sd");
	v4l2_set_subdevdata(sd, displayport);
}

static int displayport_parse_dt(struct displayport_device *displayport, struct device *dev)
{
	struct device_node *np = dev->of_node;
	int ret;

	if (IS_ERR_OR_NULL(dev->of_node)) {
		displayport_err("no device tree information\n");
		return -EINVAL;
	}

	displayport->phy = devm_phy_get(dev, "displayport_phy");
	if (IS_ERR_OR_NULL(displayport->phy)) {
		displayport_err("failed to get displayport phy\n");
		return PTR_ERR(displayport->phy);
	}

	displayport->dev = dev;
	displayport_get_gpios(displayport);

	if (of_property_read_string(np, "dp,aux_vdd",
				&displayport->aux_vdd) < 0) {
		displayport_err("failed to get displayport aux vdd\n");
		displayport->aux_vdd = NULL;
	}

	displayport->gpio_sw_oe = of_get_named_gpio(np, "dp,aux_sw_oe", 0);
	ret = gpio_request(displayport->gpio_sw_oe, "dp_aux_sw_oe");
	if (ret)
		displayport_err("failed to get gpio dp_aux_sw_oe\n");
	gpio_direction_output(displayport->gpio_sw_oe, 1);

	displayport->gpio_sw_sel = of_get_named_gpio(np, "dp,sbu_sw_sel", 0);
	if (gpio_is_valid(displayport->gpio_sw_sel)) {
		ret = gpio_request(displayport->gpio_sw_sel, "dp_sbu_sw_sel");
		if (ret)
			displayport_err("failed to get gpio dp_sbu_sw_sel\n");
	}

	displayport->gpio_usb_dir = of_get_named_gpio(np, "dp,usb_con_sel", 0);
	displayport_info("%s done\n", __func__);

	return 0;
}

static int displayport_init_resources(struct displayport_device *displayport, struct platform_device *pdev)
{
	struct resource *res;
	int ret = 0;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		displayport_err("failed to get mem resource\n");
		return -ENOENT;
	}

	displayport_info("res: start(0x%x), end(0x%x)\n", (u32)res->start, (u32)res->end);

	displayport->res.regs = devm_ioremap_resource(displayport->dev, res);
	if (!displayport->res.regs) {
		displayport_err("failed to remap DisplayPort SFR region\n");
		return -EINVAL;
	}

	res = platform_get_resource(pdev, IORESOURCE_IRQ, 0);
	if (!res) {
		displayport_err("failed to get irq resource\n");
		return -ENOENT;
	}

	displayport->res.irq = res->start;
	ret = devm_request_irq(displayport->dev, res->start,
			displayport_irq_handler, 0, pdev->name, displayport);
	if (ret) {
		displayport_err("failed to install DisplayPort irq\n");
		return -EINVAL;
	}
	disable_irq(displayport->res.irq);

	return 0;
}

static int displayport_aux_onoff(struct displayport_device *displayport, int
		onoff)
{
	int rc = 0;
	struct regulator *regulator_aux_vdd;
	static int onoff_last;

	if (onoff_last == onoff)
		return 0;

	onoff_last = onoff;

	if (displayport->aux_vdd == NULL) {
		gpio_direction_output(displayport->gpio_sw_oe, !onoff);
		displayport_info("aux_vdd is not used rev (onoff = %d)\n", onoff);
		return rc;
	}
	
	regulator_aux_vdd = regulator_get(NULL, displayport->aux_vdd);
	if (IS_ERR_OR_NULL(regulator_aux_vdd)) {
		displayport_err("aux_vdd regulator get fail\n");
		return -ENODEV;
	}

	displayport_info("aux vdd onoff = %d\n", onoff);

	if (onoff == 1) {
		rc = regulator_enable(regulator_aux_vdd);
		if (rc) {
			displayport_err("aux vdd enable failed(%d).\n", rc);
			goto done;
		}
		gpio_direction_output(displayport->gpio_sw_oe, 0);
	} else {
		rc = regulator_disable(regulator_aux_vdd);
		if (rc) {
			displayport_err("aux vdd disable failed(%d).\n", rc);
			goto done;
		}
		gpio_direction_output(displayport->gpio_sw_oe, 1);
	}

done:
	regulator_put(regulator_aux_vdd);

	return rc;
}

static void displayport_aux_sel(struct displayport_device *displayport)
{
	if (gpio_is_valid(displayport->gpio_usb_dir) &&
			gpio_is_valid(displayport->gpio_sw_sel)) {
		displayport->dp_sw_sel = gpio_get_value(displayport->gpio_usb_dir);
		gpio_direction_output(displayport->gpio_sw_sel, !(displayport->dp_sw_sel));
		displayport_info("Get direction from ccic %d\n", displayport->dp_sw_sel);
	} else if (gpio_is_valid(displayport->gpio_usb_dir)) {
		/* for old H/W - AUX switch is controlled by CCIC */
		displayport->dp_sw_sel = !gpio_get_value(displayport->gpio_usb_dir);
		displayport_info("Get Direction From CCIC %d\n", !displayport->dp_sw_sel);
	}
}

bool check_dex_support(struct displayport_device *displayport)
{
	if (displayport->ven_id == SAMSUNG_VENDOR_ID
			&& displayport->prod_id == DEXDOCK_PRODUCT_ID)
		return true;

#ifdef CONFIG_DISPLAYPORT_ENG 
	return true;
#else
	return false;
#endif
}

#if defined(CONFIG_USB_TYPEC_MANAGER_NOTIFIER)
static int usb_typec_displayport_notification(struct notifier_block *nb,
		unsigned long action, void *data)
{
	struct displayport_device *displayport = container_of(nb,
			struct displayport_device, dp_typec_nb);
	CC_NOTI_TYPEDEF usb_typec_info = *(CC_NOTI_TYPEDEF *)data;
	
	if (usb_typec_info.dest != CCIC_NOTIFY_DEV_DP)
		return 0;

	displayport_dbg("%s: action (%ld) dump(0x%01x, 0x%01x, 0x%02x, 0x%04x, 0x%04x, 0x%04x)\n",
			__func__, action, usb_typec_info.src, usb_typec_info.dest, usb_typec_info.id,
			usb_typec_info.sub1, usb_typec_info.sub2, usb_typec_info.sub3);

	switch (usb_typec_info.id) {
	case CCIC_NOTIFY_ID_DP_CONNECT:
		displayport_info("CCIC_NOTIFY_ID_DP_CONNECT, %x\n", usb_typec_info.sub1);
		switch (usb_typec_info.sub1) {
		case CCIC_NOTIFY_DETACH:
			displayport->dex_state = DEX_OFF;
			displayport->dex_ver[0] = 0;
			displayport->dex_ver[1] = 0;
			displayport->ccic_notify_dp_conf = CCIC_NOTIFY_DP_PIN_UNKNOWN;
			displayport->ccic_link_conf = false;
			displayport->ccic_hpd = false;
			displayport_hpd_changed(0);
			displayport_aux_onoff(displayport, 0);
			break;
		case CCIC_NOTIFY_ATTACH:
			displayport->ven_id = usb_typec_info.sub2;
			displayport->prod_id = usb_typec_info.sub3;

			if(check_dex_support(displayport))
				displayport_info("Dex mode supported product connected\n");

			displayport_aux_onoff(displayport, 1);
			break;
		default:
			break;
		}
		break;

	case CCIC_NOTIFY_ID_DP_LINK_CONF:
		displayport_info("CCIC_NOTIFY_ID_DP_LINK_CONF %x\n",
				usb_typec_info.sub1);
		displayport_aux_sel(displayport);
		switch (usb_typec_info.sub1) {
		case CCIC_NOTIFY_DP_PIN_UNKNOWN:
			displayport->ccic_notify_dp_conf = CCIC_NOTIFY_DP_PIN_UNKNOWN;
			break;
		case CCIC_NOTIFY_DP_PIN_A:
			displayport->ccic_notify_dp_conf = CCIC_NOTIFY_DP_PIN_A;
			break;
		case CCIC_NOTIFY_DP_PIN_B:
			displayport->dp_sw_sel = !displayport->dp_sw_sel;
			displayport->ccic_notify_dp_conf = CCIC_NOTIFY_DP_PIN_B;
			break;
		case CCIC_NOTIFY_DP_PIN_C:
			displayport->ccic_notify_dp_conf = CCIC_NOTIFY_DP_PIN_C;
			break;
		case CCIC_NOTIFY_DP_PIN_D:
			displayport->ccic_notify_dp_conf = CCIC_NOTIFY_DP_PIN_D;
			break;
		case CCIC_NOTIFY_DP_PIN_E:
			displayport->ccic_notify_dp_conf = CCIC_NOTIFY_DP_PIN_E;
			break;
		case CCIC_NOTIFY_DP_PIN_F:
			displayport->ccic_notify_dp_conf = CCIC_NOTIFY_DP_PIN_F;
			break;
		default:
			displayport->ccic_notify_dp_conf = CCIC_NOTIFY_DP_PIN_UNKNOWN;
			break;
		}
		displayport->ccic_link_conf = true;
		if(displayport->ccic_hpd) {
			msleep(10);
			displayport_hpd_changed(1);
		}
		break;

	case CCIC_NOTIFY_ID_DP_HPD:
		displayport_info("CCIC_NOTIFY_ID_DP_HPD, %x, %x\n",
				usb_typec_info.sub1, usb_typec_info.sub2);
		switch (usb_typec_info.sub1) {
		case CCIC_NOTIFY_IRQ:
			break;
		case CCIC_NOTIFY_LOW:
			displayport->dex_state = DEX_OFF;
			displayport->ccic_hpd = false;
			displayport_hpd_changed(0);
			break;
		case CCIC_NOTIFY_HIGH:
			if (displayport->hpd_current_state &&
					usb_typec_info.sub2 == CCIC_NOTIFY_IRQ) {
				queue_delayed_work(displayport->dp_wq, &displayport->hpd_irq_work, 0);
				return 0;
			} else {
				displayport->ccic_hpd = true;
				if(displayport->ccic_link_conf) {
					msleep(10);
					displayport_hpd_changed(1);
				}
			}
			break;
		default:
			break;
		}

		break;

	default:
		break;
	}

	return 0;
}
#endif

static ssize_t displayport_link_show(struct class *class,
		struct class_attribute *attr,
		char *buf)
{
	/*struct displayport_device *displayport = get_displayport_drvdata();*/

	return snprintf(buf, PAGE_SIZE, "%s\n", __func__);
}

static ssize_t displayport_link_store(struct class *dev,
		struct class_attribute *attr, const char *buf, size_t size)
{
	int mode = 0;
	struct displayport_device *displayport = get_displayport_drvdata();

	if (kstrtoint(buf, 10, &mode))
		return size;
	pr_info("%s mode=%d\n", __func__, mode);

	if (mode == 0) {
		displayport_hpd_changed(0);
		displayport_link_sink_status_read();
	} else if (mode == 8) {
		queue_delayed_work(displayport->dp_wq, &displayport->hpd_irq_work, 0);
	} else if (mode == 9) {
		displayport_hpd_changed(1);
	} else {
		u8 link_rate = mode/10;
		u8 lane_cnt = mode%10;

		if ((link_rate >= 1 && link_rate <= 3) &&
				(lane_cnt == 1 || lane_cnt == 2 || lane_cnt == 4)) {
			if (link_rate == 3)
				link_rate = LINK_RATE_5_4Gbps;
			else if (link_rate == 2)
				link_rate = LINK_RATE_2_7Gbps;
			else
				link_rate = LINK_RATE_1_62Gbps;

			pr_info("%s: %02x %02x\n", __func__, link_rate, lane_cnt);
			phy_power_on(displayport->phy);
			displayport_reg_init(); /* for AUX ch read/write. */

			displayport_link_status_read();

			g_displayport_debug_param.param_used = 1;
			g_displayport_debug_param.link_rate = link_rate;
			g_displayport_debug_param.lane_cnt = lane_cnt;

			displayport_full_link_training();

			g_displayport_debug_param.param_used = 0;

			switch_set_state(&displayport->hpd_switch, 1);
			displayport_info("HPD status = %d\n", 1);
		} else {
			pr_err("%s: Not support command[%d]\n",
					__func__, mode);
		}
	}

	return size;
}

static CLASS_ATTR(link, 0664, displayport_link_show, displayport_link_store);

static ssize_t displayport_test_bpc_show(struct class *class,
		struct class_attribute *attr,
		char *buf)
{
	struct displayport_device *displayport = get_displayport_drvdata();

	return snprintf(buf, PAGE_SIZE, "displayport bpc %d\n", (displayport->bpc == BPC_6)?6:8);
}
static ssize_t displayport_test_bpc_store(struct class *dev,
		struct class_attribute *attr,
		const char *buf, size_t size)
{
	int mode = 0;
	struct displayport_device *displayport = get_displayport_drvdata();

	if (kstrtoint(buf, 10, &mode))
		return size;
	pr_info("%s mode=%d\n", __func__, mode);

	switch (mode) {
	case 6:
		displayport->bpc = BPC_6;
		break;
	case 8:
		displayport->bpc = BPC_8;
		break;
	default:
		pr_err("%s: Not support command[%d]\n",
				__func__, mode);
		break;
	}

	return size;
}

static CLASS_ATTR(bpc, 0664, displayport_test_bpc_show, displayport_test_bpc_store);

static ssize_t displayport_test_range_show(struct class *class,
		struct class_attribute *attr,
		char *buf)
{
	struct displayport_device *displayport = get_displayport_drvdata();

	return sprintf(buf, "displayport range %s\n",
		(displayport->dyn_range == VESA_RANGE)?"VESA_RANGE":"CEA_RANGE");
}
static ssize_t displayport_test_range_store(struct class *dev,
		struct class_attribute *attr,
		const char *buf, size_t size)
{
	int mode = 0;
	struct displayport_device *displayport = get_displayport_drvdata();

	if (kstrtoint(buf, 10, &mode))
		return size;
	pr_info("%s mode=%d\n", __func__, mode);

	switch (mode) {
	case 0:
		displayport->dyn_range = VESA_RANGE;
		break;
	case 1:
		displayport->dyn_range = CEA_RANGE;
		break;
	default:
		pr_err("%s: Not support command[%d]\n",
				__func__, mode);
		break;
	}

	return size;
}

static CLASS_ATTR(range, 0664, displayport_test_range_show, displayport_test_range_store);

static ssize_t displayport_test_edid_show(struct class *class,
		struct class_attribute *attr,
		char *buf)
{
	struct v4l2_dv_timings edid_preset;
	int i;
	/*struct displayport_device *displayport = get_displayport_drvdata();*/

	edid_preset = edid_preferred_preset();

	i = displayport_timing2conf(&edid_preset);
	if (i < 0) {
		i = g_displayport_videoformat;
		pr_err("displayport timings not supported\n");
	}

	return snprintf(buf, PAGE_SIZE, "displayport preferred_preset = %d %d %d\n",
			videoformat_parameters[i].active_pixel,
			videoformat_parameters[i].active_line,
			videoformat_parameters[i].fps);
}

static ssize_t displayport_test_edid_store(struct class *dev,
		struct class_attribute *attr,
		const char *buf, size_t size)
{
	struct v4l2_dv_timings edid_preset;
	int i;
	int mode = 0;
	/*struct displayport_device *displayport = get_displayport_drvdata(); */

	if (kstrtoint(buf, 10, &mode))
		return size;
	pr_info("%s mode=%d\n", __func__, mode);

	edid_set_preferred_preset(mode);
	edid_preset = edid_preferred_preset();
	i = displayport_timing2conf(&edid_preset);
	if (i < 0) {
		i = g_displayport_videoformat;
		pr_err("displayport timings not supported\n");
	}

	pr_info("displayport preferred_preset = %d %d %d\n",
			videoformat_parameters[i].active_pixel,
			videoformat_parameters[i].active_line,
			videoformat_parameters[i].fps);

	return size;
}
static CLASS_ATTR(edid, 0664, displayport_test_edid_show, displayport_test_edid_store);

static ssize_t displayport_test_bist_show(struct class *class,
		struct class_attribute *attr,
		char *buf)
{
	struct displayport_device *displayport = get_displayport_drvdata();

	return snprintf(buf, PAGE_SIZE, "displayport bist used %d type %d\n",
			displayport->bist_used,
			displayport->bist_type);
}

static ssize_t displayport_test_bist_store(struct class *dev,
		struct class_attribute *attr,
		const char *buf, size_t size)
{
	int mode = 0;
	struct displayport_audio_config_data audio_config_data;
	struct displayport_device *displayport = get_displayport_drvdata();

	if (kstrtoint(buf, 10, &mode))
		return size;
	pr_info("%s mode=%d\n", __func__, mode);

	switch (mode) {
	case 0:
		displayport->bist_used = 0;
		displayport->bist_type = COLOR_BAR;
		break;
	case 1:
		displayport->bist_used = 1;
		displayport->bist_type = COLOR_BAR;
		break;
	case 2:
		displayport->bist_used = 1;
		displayport->bist_type = WGB_BAR;
		break;
	case 3:
		displayport->bist_used = 1;
		displayport->bist_type = MW_BAR;
		break;
	case 11:
	case 12:
		audio_config_data.audio_enable = 1;
		audio_config_data.audio_fs = FS_192KHZ;
		audio_config_data.audio_channel_cnt = mode-10;
		audio_config_data.audio_bit = 0;
		audio_config_data.audio_packed_mode = 0;
		audio_config_data.audio_word_length = 0;
		displayport_audio_bist_enable(audio_config_data);
		break;
	default:
		pr_err("%s: Not support command[%d]\n",
				__func__, mode);
		break;
	}

	return size;
}
static CLASS_ATTR(bist, 0664, displayport_test_bist_show, displayport_test_bist_store);

static ssize_t displayport_test_show(struct class *class,
		struct class_attribute *attr,
		char *buf)
{
	struct displayport_device *displayport = get_displayport_drvdata();

	return snprintf(buf, PAGE_SIZE, "displayport gpio oe %d, sel %d, direction %d\n",
			gpio_get_value(displayport->gpio_sw_oe),
			gpio_get_value(displayport->gpio_sw_sel),
			gpio_get_value(displayport->gpio_usb_dir));
}
static ssize_t displayport_test_store(struct class *dev,
		struct class_attribute *attr,
		const char *buf, size_t size)
{
	/*	struct displayport_device *displayport = get_displayport_drvdata(); */

	return size;
}

static CLASS_ATTR(dp, 0664, displayport_test_show, displayport_test_store);

extern int forced_resolution;
static ssize_t displayport_forced_resolution_show(struct class *class,
		struct class_attribute *attr, char *buf)
{
	int ret = 0;
	int i;

	for (i = 0; i < displayport_pre_cnt; i++) {
		ret += scnprintf(buf + ret, PAGE_SIZE - ret, "%c %2d : %s\n",
				forced_resolution == i ? '*':' ', i,
				displayport_supported_presets[i].name);
	}

	return ret;
}

static ssize_t displayport_forced_resolution_store(struct class *dev,
		struct class_attribute *attr, const char *buf, size_t size)
{
	int val[4] = {0,};

	get_options(buf, 4, val);

	reduced_resolution = 0;

	if (val[1] < 0 || val[1] >= displayport_pre_cnt || val[0] < 1)
		forced_resolution = -1;
	else {
		struct displayport_device *displayport = get_displayport_drvdata();
		int hpd_stat = displayport->hpd_current_state;

		forced_resolution = val[1];
		if (hpd_stat) {
			displayport_hpd_changed(0);
			msleep(100);
			displayport_hpd_changed(1);
		}
	}

	return size;
}
static CLASS_ATTR(forced_resolution, 0664, displayport_forced_resolution_show,
		displayport_forced_resolution_store);

static ssize_t displayport_reduced_resolution_show(struct class *class,
		struct class_attribute *attr, char *buf)
{
	int ret = 0;

	ret = scnprintf(buf, PAGE_SIZE, "%llu\n", reduced_resolution);

	return ret;
}

static ssize_t displayport_reduced_resolution_store(struct class *dev,
		struct class_attribute *attr, const char *buf, size_t size)
{
	int val[4] = {0,};

	get_options(buf, 4, val);

	forced_resolution = -1;

	if (val[1] < 0 || val[1] >= displayport_pre_cnt || val[0] < 1)
		reduced_resolution = 0;
	else {
		switch (val[1]) {
		case 1:
			reduced_resolution = PIXELCLK_2160P30HZ;
			break;
		case 2:
			reduced_resolution = PIXELCLK_1080P60HZ;
			break;
		case 3:
			reduced_resolution = PIXELCLK_1080P30HZ;
			break;
		default:
			reduced_resolution = 0;
		};
	}

	return size;
}
static CLASS_ATTR(reduced_resolution, 0664, displayport_reduced_resolution_show,
		displayport_reduced_resolution_store);

static ssize_t displayport_dex_show(struct class *class,
		struct class_attribute *attr, char *buf)
{
	struct displayport_device *displayport = get_displayport_drvdata();
	int ret = 0;

	displayport_info("dex state: %d\n", displayport->dex_state);
	ret = scnprintf(buf, PAGE_SIZE, "%d\n", displayport->dex_state);
	if (displayport->dex_state == DEX_RECONNECTING)
		displayport->dex_state = DEX_ON;

	return ret;
}

static ssize_t displayport_dex_store(struct class *dev,
		struct class_attribute *attr, const char *buf, size_t size)
{
	struct displayport_device *displayport = get_displayport_drvdata();
	int val[4] = {0,};
	int dex_run;
	int need_reconnect = 0;

	get_options(buf, 4, val);

	displayport->dex_setting = (val[1] & 0xF0) >> 4;
	dex_run = (val[1] & 0x0F);

	displayport_info("dex set:0x%X, hpd:%d, state:%d\n", val[1],
			displayport->hpd_current_state, displayport->dex_state);

#if defined(CONFIG_USB_TYPEC_MANAGER_NOTIFIER)
	if (!displayport->notifier_registered) {
		displayport->notifier_registered = 1;
		displayport_info("notifier registered in dex\n");
		cancel_delayed_work_sync(&displayport->notifier_register_work);
		if (displayport->dex_setting == 1)
			reduced_resolution = PIXELCLK_1080P60HZ;
		manager_notifier_register(&displayport->dp_typec_nb,
			usb_typec_displayport_notification, MANAGER_NOTIFY_CCIC_DP);
		goto dex_exit;
	}
#endif
	if (displayport->dex_state != dex_run && displayport->dex_state != DEX_RECONNECTING &&
			check_dex_support(displayport))
		need_reconnect = 1;
	displayport->dex_state = dex_run;

	if (displayport->dex_setting == 1) {
		reduced_resolution = PIXELCLK_1080P60HZ;
		if (g_displayport_videoformat <= v1920x1080p_60Hz) {
			displayport_info("not need to reconnect\n");
			goto dex_exit;
		}
	} else
		reduced_resolution = 0;

	/* reconnect if setting was mirroring(0) and dex is running(1), */
	if (displayport->hpd_current_state && need_reconnect) {
		displayport->dex_state = DEX_RECONNECTING;
		displayport_hpd_changed(0);
		msleep(1000);
		if (displayport->dex_state != DEX_OFF)
			displayport_hpd_changed(1);
	}

dex_exit:
	displayport_info("dex reduced_resolution %llu\n", reduced_resolution);
	return size;
}
static CLASS_ATTR(dex, 0664, displayport_dex_show, displayport_dex_store);

static ssize_t displayport_dex_ver_show(struct class *class,
		struct class_attribute *attr, char *buf)
{
	struct displayport_device *displayport = get_displayport_drvdata();
	int ret = 0;

	ret = scnprintf(buf, PAGE_SIZE, "%02X%02X\n", displayport->dex_ver[0],
			displayport->dex_ver[1]);

	return ret;
}

static CLASS_ATTR(dex_ver, 0444, displayport_dex_ver_show, NULL);

static ssize_t displayport_aux_sw_sel_store(struct class *dev,
		struct class_attribute *attr, const char *buf, size_t size)
{
	struct displayport_device *displayport = get_displayport_drvdata();
	int val[10] = {0,};
	int aux_sw_sel, aux_sw_oe;

	get_options(buf, 10, val);

	aux_sw_sel = val[1];
	aux_sw_oe = val[2];
	displayport_info("sbu_sw_sel(%d), sbu_sw_oe(%d)\n", aux_sw_sel, aux_sw_oe);

	if ((aux_sw_sel == 0 || aux_sw_sel == 1) && (aux_sw_oe == 0 || aux_sw_oe == 1)) {
		if (gpio_is_valid(displayport->gpio_sw_sel))
			gpio_direction_output(displayport->gpio_sw_sel, aux_sw_sel);
		displayport_aux_onoff(displayport, !aux_sw_oe);
	} else
		displayport_err("invalid aux switch parameter\n");

	return size;
}
static CLASS_ATTR(dp_sbu_sw_sel, 0664, NULL, displayport_aux_sw_sel_store);

static ssize_t displayport_log_level_show(struct class *class,
		struct class_attribute *attr,
		char *buf)
{
	return snprintf(buf, PAGE_SIZE, "displayport log level %1d\n", displayport_log_level);
}
static ssize_t displayport_log_level_store(struct class *dev,
		struct class_attribute *attr,
		const char *buf, size_t size)
{
	int mode = 0;

	if (kstrtoint(buf, 10, &mode))
		return size;
	displayport_log_level = mode;
	displayport_err("log level = %d\n", displayport_log_level);

	return size;
}

static CLASS_ATTR(log_level, 0664, displayport_log_level_show, displayport_log_level_store);

#if defined(CONFIG_USB_TYPEC_MANAGER_NOTIFIER)
static void displayport_notifier_register_work(struct work_struct *work)
{
	struct displayport_device *displayport = get_displayport_drvdata();

	displayport_info("notifier register %d\n", displayport->dex_setting);
	if (!displayport->notifier_registered) {
		displayport->notifier_registered = 1;
		manager_notifier_register(&displayport->dp_typec_nb,
			usb_typec_displayport_notification, MANAGER_NOTIFY_CCIC_DP);
	}
}
#endif

static int displayport_probe(struct platform_device *pdev)
{
	int ret = 0;
	struct device *dev = &pdev->dev;
	struct displayport_device *displayport = NULL;
	struct class *dp_class;

	displayport = devm_kzalloc(dev, sizeof(struct displayport_device), GFP_KERNEL);
	if (!displayport) {
		displayport_err("failed to allocate displayport device.\n");
		ret = -ENOMEM;
		goto err;
	}

	ret = displayport_parse_dt(displayport, dev);
	if (ret)
		goto err_dt;

	displayport_drvdata = displayport;

	spin_lock_init(&displayport->slock);
	mutex_init(&displayport->cmd_lock);
	mutex_init(&displayport->hpd_lock);
	mutex_init(&displayport->aux_lock);
	mutex_init(&displayport->training_lock);
	init_waitqueue_head(&displayport->dp_wait);

	ret = displayport_init_resources(displayport, pdev);
	if (ret)
		goto err_dt;

	displayport_init_subdev(displayport);
	platform_set_drvdata(pdev, displayport);

	displayport->dp_wq = create_singlethread_workqueue(dev_name(&pdev->dev));
	if (!displayport->dp_wq) {
		displayport_err("create wq failed.\n");
		goto err_dt;
	}

	displayport->hdcp2_wq = create_singlethread_workqueue(dev_name(&pdev->dev));
	if (!displayport->hdcp2_wq) {
		displayport_err("create hdcp2_wq failed.\n");
		goto err_dt;
	}

	INIT_DELAYED_WORK(&displayport->hpd_plug_work, displayport_hpd_plug_work);
	INIT_DELAYED_WORK(&displayport->hpd_unplug_work, displayport_hpd_unplug_work);
	INIT_DELAYED_WORK(&displayport->hpd_irq_work, displayport_hpd_irq_work);
	INIT_DELAYED_WORK(&displayport->hdcp13_work, displayport_hdcp13_run);
	INIT_DELAYED_WORK(&displayport->hdcp22_work, displayport_hdcp22_run);
	INIT_DELAYED_WORK(&displayport->hdcp13_integrity_check_work, displayport_hdcp13_integrity_check_work);

#if defined(CONFIG_USB_TYPEC_MANAGER_NOTIFIER)
	INIT_DELAYED_WORK(&displayport->notifier_register_work,
			displayport_notifier_register_work);
	queue_delayed_work(displayport->dp_wq, &displayport->notifier_register_work,
			msecs_to_jiffies(30000));
#endif
	/* register the switch device for HPD */
	displayport->hpd_switch.name = "hdmi";
	ret = switch_dev_register(&displayport->hpd_switch);
	if (ret) {
		displayport_err("hdmi switch register failed.\n");
		goto err_dt;
	}
	displayport->audio_switch.name = "ch_hdmi_audio";
	ret = switch_dev_register(&displayport->audio_switch);
	if (ret) {
		displayport_err("audio switch register failed.\n");
		goto err_dt;
	}
	displayport_info("success register switch device\n");

	displayport->hpd_state = HPD_UNPLUG;

	pm_runtime_enable(dev);

	phy_init(displayport->phy);

	displayport->state = DISPLAYPORT_STATE_INIT;

	wake_lock_init(&displayport->dp_wake_lock, WAKE_LOCK_SUSPEND, "dp_wake_lock");

	dp_class = class_create(THIS_MODULE, "dp_sec");
	if (IS_ERR(dp_class))
		displayport_err("failed to creat dp_class\n");
	else {
		ret = class_create_file(dp_class, &class_attr_link);
		if (ret)
			displayport_err("failed to create attr_link\n");
		ret = class_create_file(dp_class, &class_attr_bpc);
		if (ret)
			displayport_err("failed to create attr_bpc\n");
		ret = class_create_file(dp_class, &class_attr_range);
		if (ret)
			displayport_err("failed to create attr_range\n");
		ret = class_create_file(dp_class, &class_attr_edid);
		if (ret)
			displayport_err("failed to create attr_edid\n");
		ret = class_create_file(dp_class, &class_attr_bist);
		if (ret)
			displayport_err("failed to create attr_bist\n");
		ret = class_create_file(dp_class, &class_attr_dp);
		if (ret)
			displayport_err("failed to create attr_test\n");
		ret = class_create_file(dp_class, &class_attr_forced_resolution);
		if (ret)
			displayport_err("failed to create attr_dp_forced_resolution\n");
		ret = class_create_file(dp_class, &class_attr_reduced_resolution);
		if (ret)
			displayport_err("failed to create attr_dp_reduced_resolution\n");
		ret = class_create_file(dp_class, &class_attr_dex);
		if (ret)
			displayport_err("failed to create attr_dp_dex\n");
		ret = class_create_file(dp_class, &class_attr_dex_ver);
		if (ret)
			displayport_err("failed to create attr_dp_dex_ver\n");
		ret = class_create_file(dp_class, &class_attr_dp_sbu_sw_sel);
		if (ret)
			displayport_err("failed to create class_attr_dp_sbu_sw_sel\n");
		ret = class_create_file(dp_class, &class_attr_log_level);
		if (ret)
			displayport_err("failed to create class_attr_log_level\n");
	}

	g_displayport_debug_param.param_used = 0;
	g_displayport_debug_param.link_rate = LINK_RATE_2_7Gbps;
	g_displayport_debug_param.lane_cnt = 0x04;

	displayport->bpc = BPC_8;
	displayport->bist_used = 0;
	displayport->bist_type = COLOR_BAR;
	displayport->dyn_range = CEA_RANGE;
	displayport_info("displayport driver has been probed.\n");
	return 0;

err_dt:
	kfree(displayport);
err:
	return ret;
}

static void displayport_shutdown(struct platform_device *pdev)
{
	struct displayport_device *displayport = platform_get_drvdata(pdev);

	/* DPU_EVENT_LOG(DPU_EVT_DP_SHUTDOWN, &displayport->sd, ktime_set(0, 0)); */
	displayport_info("%s + state:%d\n", __func__, displayport->state);

	displayport_disable(displayport);

	displayport_info("%s -\n", __func__);
}

static int displayport_runtime_suspend(struct device *dev)
{
	/* struct displayport_device *displayport = dev_get_drvdata(dev); */

	/* DPU_EVENT_LOG(DPU_EVT_DP_SUSPEND, &displayport->sd, ktime_set(0, 0)); */
	displayport_dbg("%s +\n", __func__);

	displayport_dbg("%s -\n", __func__);
	return 0;
}

static int displayport_runtime_resume(struct device *dev)
{
	/* struct displayport_device *displayport = dev_get_drvdata(dev); */

	/* DPU_EVENT_LOG(DPU_EVT_DP_RESUME, &displayport->sd, ktime_set(0, 0)); */
	displayport_dbg("%s: +\n", __func__);

	displayport_dbg("%s -\n", __func__);
	return 0;
}

static const struct of_device_id displayport_of_match[] = {
	{ .compatible = "samsung,exynos-displayport" },
	{},
};
MODULE_DEVICE_TABLE(of, displayport_of_match);

static const struct dev_pm_ops displayport_pm_ops = {
	.runtime_suspend	= displayport_runtime_suspend,
	.runtime_resume		= displayport_runtime_resume,
};

static struct platform_driver displayport_driver __refdata = {
	.probe			= displayport_probe,
	.remove			= displayport_remove,
	.shutdown		= displayport_shutdown,
	.driver = {
		.name		= DISPLAYPORT_MODULE_NAME,
		.owner		= THIS_MODULE,
		.pm		= &displayport_pm_ops,
		.of_match_table	= of_match_ptr(displayport_of_match),
		.suppress_bind_attrs = true,
	}
};

static int __init displayport_init(void)
{
	int ret = platform_driver_register(&displayport_driver);

	if (ret)
		pr_err("displayport driver register failed\n");

	return ret;
}
late_initcall(displayport_init);

static void __exit displayport_exit(void)
{
	platform_driver_unregister(&displayport_driver);
}

module_exit(displayport_exit);
MODULE_AUTHOR("Kwangje Kim <kj1.kim@samsung.com>");
MODULE_DESCRIPTION("Samusung EXYNOS DisplayPort driver");
MODULE_LICENSE("GPL");
