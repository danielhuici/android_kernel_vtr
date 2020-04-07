/*
 * bq25892_charger_aux.c
 *
 * bq25892 driver
 *
 * Copyright (c) 2012-2018 Huawei Technologies Co., Ltd.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/wakelock.h>
#include <linux/usb/otg.h>
#include <linux/io.h>
#include <linux/gpio.h>
#include <linux/power_supply.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_address.h>
#include <linux/of_gpio.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/notifier.h>
#include <linux/mutex.h>
#include <linux/hisi/usb/hisi_usb.h>
#include <huawei_platform/log/hw_log.h>
#ifdef CONFIG_HUAWEI_HW_DEV_DCT
#include <huawei_platform/devdetect/hw_dev_dec.h>
#endif
#include <linux/raid/pq.h>
#include <huawei_platform/power/huawei_charger.h>
#ifdef CONFIG_HISI_BCI_BATTERY
#include <linux/power/hisi/hisi_bci_battery.h>
#endif
#include <bq25892_charger_aux.h>
#include <../dual_charger.h>
#include <linux/hisi/hisi_adc.h>

#ifdef HWLOG_TAG
#undef HWLOG_TAG
#endif

#define HWLOG_TAG bq25892_charger_aux
HWLOG_REGIST();

static struct bq25892_aux_device_info *g_bq25892_aux_dev;

/* configured in dts based on the real value of the iin limit resistance */
static unsigned int rilim = 124;
static u32 is_board_type; /*0:sft 1:udp 2:asic */
/* has ever reach charge done state after connect charger */
static bool first_charging_done = FALSE;
static bool charging_again_after_charging_done = FALSE;

#define MSG_LEN                      (2)
#define BUF_LEN                      (26)

static int bq25892_aux_write_block(struct bq25892_aux_device_info *di,
	u8 *value, u8 reg, unsigned int num_bytes)
{
	struct i2c_msg msg[1];
	int ret = 0;

	if (di == NULL || value == NULL) {
		hwlog_err("di or value is null\n");
		return -EIO;
	}

	*value = reg;

	msg[0].addr = di->client->addr;
	msg[0].flags = 0;
	msg[0].buf = value;
	msg[0].len = num_bytes + 1;

	ret = i2c_transfer(di->client->adapter, msg, 1);

	/* i2c_transfer returns number of messages transferred */
	if (ret != 1) {
		hwlog_err("write_block failed[%x]\n", reg);
		if (ret < 0)
			return ret;
		else
			return -EIO;
	} else {
		return 0;
	}
}

static int bq25892_aux_read_block(struct bq25892_aux_device_info *di,
	u8 *value, u8 reg, unsigned int num_bytes)
{
	struct i2c_msg msg[MSG_LEN];
	u8 buf = 0;
	int ret = 0;

	if (di == NULL || value == NULL) {
		hwlog_err("di or value is null\n");
		return -EIO;
	}

	buf = reg;

	msg[0].addr = di->client->addr;
	msg[0].flags = 0;
	msg[0].buf = &buf;
	msg[0].len = 1;

	msg[1].addr = di->client->addr;
	msg[1].flags = I2C_M_RD;
	msg[1].buf = value;
	msg[1].len = num_bytes;

	ret = i2c_transfer(di->client->adapter, msg, MSG_LEN);

	/* i2c_transfer returns number of messages transferred */
	if (ret != MSG_LEN) {
		hwlog_err("read_block failed[%x]\n", reg);
		if (ret < 0)
			return ret;
		else
			return -EIO;
	} else {
		return 0;
	}
}

static int bq25892_aux_write_byte(u8 reg, u8 value)
{
	struct bq25892_aux_device_info *di = g_bq25892_aux_dev;
	/* 2 bytes offset 1 contains the data offset 0 is used by i2c_write */
	u8 temp_buffer[MSG_LEN] = {0};

	/* offset 1 contains the data */
	temp_buffer[1] = value;
	return bq25892_aux_write_block(di, temp_buffer, reg, 1);
}

static int bq25892_aux_read_byte(u8 reg, u8 *value)
{
	struct bq25892_aux_device_info *di = g_bq25892_aux_dev;

	return bq25892_aux_read_block(di, value, reg, 1);
}

static int bq25892_aux_write_mask(u8 reg, u8 mask, u8 shift, u8 value)
{
	int ret = 0;
	u8 val = 0;

	ret = bq25892_aux_read_byte(reg, &val);
	if (ret < 0)
		return ret;

	val &= ~mask;
	val |= ((value << shift) & mask);

	ret = bq25892_aux_write_byte(reg, val);

	return ret;
}

static int bq25892_aux_read_mask(u8 reg, u8 mask, u8 shift, u8 *value)
{
	int ret = 0;
	u8 val = 0;

	ret = bq25892_aux_read_byte(reg, &val);
	if (ret < 0)
		return ret;

	val &= mask;
	val >>= shift;
	*value = val;

	return 0;
}

int bq25892_aux_get_ichg_reg(void)
{
	int ret = 0;
	u8 ichg_reg = 0;
	int ichg = 0;

	ret = bq25892_aux_read_mask(BQ25892_AUX_REG_04,
			BQ25892_AUX_REG_04_ICHG_MASK,
			BQ25892_AUX_REG_04_ICHG_SHIFT,
			&ichg_reg);
	if (ret)
		return -1;

	ichg = ichg_reg * 64; /* ichg_step is 64 */

	hwlog_info("bq25892 aux get ichg reg = %d\n", ichg);
	return ichg;
}

int bq25892_aux_get_ichg_adc(void)
{
	int ret = 0;
	u8 ichg_adc_reg = 0;
	int ichg_adc = 0;

	ret = bq25892_aux_read_byte(BQ25892_AUX_REG_12, &ichg_adc_reg);
	if (ret)
		return -1;

	ichg_adc = ichg_adc_reg * 50; /* ichg_adc_step is 50 */

	hwlog_info("bq25892 aux get ichg adc = %d\n", ichg_adc);
	return ichg_adc;
}

#ifdef CONFIG_SYSFS
/*
 * There are a numerous options that are configurable on the bq25892_aux
 * that go well beyond what the power_supply properties provide access to.
 * Provide sysfs access to them so they can be examined and possibly modified
 * on the fly.
 */
#define BQ25892_AUX_SYSFS_FIELD(_name, r, f, m, store) \
{ \
	.attr = __ATTR(_name, m, bq25892_aux_sysfs_show, store), \
	.reg = BQ25892_AUX_REG_##r, \
	.mask = BQ25892_AUX_REG_##r##_##f##_MASK, \
	.shift = BQ25892_AUX_REG_##r##_##f##_SHIFT, \
}

#define BQ25892_AUX_SYSFS_FIELD_RW(_name, r, f) \
	BQ25892_AUX_SYSFS_FIELD(_name, r, f, 0644, bq25892_aux_sysfs_store)

#define BQ25892_AUX_SYSFS_FIELD_RO(_name, r, f) \
	BQ25892_AUX_SYSFS_FIELD(_name, r, f, 0444, NULL)

static ssize_t bq25892_aux_sysfs_show(struct device *dev,
	struct device_attribute *attr, char *buf);
static ssize_t bq25892_aux_sysfs_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count);

struct bq25892_aux_sysfs_field_info {
	struct device_attribute attr;
	u8 reg;
	u8 mask;
	u8 shift;
};

static struct bq25892_aux_sysfs_field_info bq25892_aux_sysfs_field_tbl[] = {
	/* sysfs name reg field in reg */
	BQ25892_AUX_SYSFS_FIELD_RW(en_hiz, 00, EN_HIZ),
	BQ25892_AUX_SYSFS_FIELD_RW(en_ilim, 00, EN_ILIM),
	BQ25892_AUX_SYSFS_FIELD_RW(iinlim, 00, IINLIM),
	BQ25892_AUX_SYSFS_FIELD_RW(dpm_os, 01, VINDPM_OS),
	BQ25892_AUX_SYSFS_FIELD_RW(conv_start, 02, CONV_START),
	BQ25892_AUX_SYSFS_FIELD_RW(ico_en, 02, ICO_EN),
	BQ25892_AUX_SYSFS_FIELD_RW(force_dpdm, 02, FORCE_DPDM),
	BQ25892_AUX_SYSFS_FIELD_RW(auto_dpdm_en, 02, AUTO_DPDM_EN),
	BQ25892_AUX_SYSFS_FIELD_RW(chg_config, 03, CHG_CONFIG),
	BQ25892_AUX_SYSFS_FIELD_RW(otg_config, 03, OTG_CONFIG),
	BQ25892_AUX_SYSFS_FIELD_RW(sys_min, 03, SYS_MIN),
	BQ25892_AUX_SYSFS_FIELD_RW(ichg, 04, ICHG),
	BQ25892_AUX_SYSFS_FIELD_RW(iprechg, 05, IPRECHG),
	BQ25892_AUX_SYSFS_FIELD_RW(iterm, 05, ITERM),
	BQ25892_AUX_SYSFS_FIELD_RW(vreg, 06, VREG),
	BQ25892_AUX_SYSFS_FIELD_RW(batlowv, 06, BATLOWV),
	BQ25892_AUX_SYSFS_FIELD_RW(vrechg, 06, VRECHG),
	BQ25892_AUX_SYSFS_FIELD_RW(en_term, 07, EN_TERM),
	BQ25892_AUX_SYSFS_FIELD_RW(watchdog, 07, WATCHDOG),
	BQ25892_AUX_SYSFS_FIELD_RW(en_timer, 07, EN_TIMER),
	BQ25892_AUX_SYSFS_FIELD_RW(chg_timer, 07, CHG_TIMER),
	BQ25892_AUX_SYSFS_FIELD_RW(jeta_iset, 07, JEITA_ISET),
	BQ25892_AUX_SYSFS_FIELD_RW(bat_comp, 08, BAT_COMP),
	BQ25892_AUX_SYSFS_FIELD_RW(vclamp, 08, VCLAMP),
	BQ25892_AUX_SYSFS_FIELD_RW(treg, 08, TREG),
	BQ25892_AUX_SYSFS_FIELD_RW(force_ico, 09, FORCE_ICO),
	BQ25892_AUX_SYSFS_FIELD_RW(batfet_disable, 09, BATFET_DISABLE),
	BQ25892_AUX_SYSFS_FIELD_RW(jeita_vset, 09, JEITA_VSET),
	BQ25892_AUX_SYSFS_FIELD_RW(boost_vol, 0A, BOOSTV),
	BQ25892_AUX_SYSFS_FIELD_RW(boost_lim, 0A, BOOST_LIM),
	BQ25892_AUX_SYSFS_FIELD_RW(vindpm, 0D, VINDPM),
	BQ25892_AUX_SYSFS_FIELD_RW(force_vindpm, 0D, FORCE_VINDPM),
	BQ25892_AUX_SYSFS_FIELD_RW(reg_rst, 14, REG_RST),
	BQ25892_AUX_SYSFS_FIELD_RO(vbus_stat, 0B, VBUS_STAT),
	BQ25892_AUX_SYSFS_FIELD_RO(chrg_stat, 0B, CHRG_STAT),
	BQ25892_AUX_SYSFS_FIELD_RO(pg_stat, 0B, PG_STAT),
	BQ25892_AUX_SYSFS_FIELD_RO(sdp_stat, 0B, SDP_STAT),
	BQ25892_AUX_SYSFS_FIELD_RO(vsys_stat, 0B, VSYS_STAT),
	BQ25892_AUX_SYSFS_FIELD_RO(watchdog_fault, 0C, WATCHDOG_FAULT),
	BQ25892_AUX_SYSFS_FIELD_RO(boost_fault, 0C, BOOST_FAULT),
	BQ25892_AUX_SYSFS_FIELD_RO(chrg_fault, 0C, CHRG_FAULT),
	BQ25892_AUX_SYSFS_FIELD_RO(bat_fault, 0C, BAT_FAULT),
	BQ25892_AUX_SYSFS_FIELD_RO(ntc_fault, 0C, NTC_FAULT),
	BQ25892_AUX_SYSFS_FIELD_RO(therm_stat, 0E, THERM_STAT),
	BQ25892_AUX_SYSFS_FIELD_RO(bat_vol, 0E, BATV),
	BQ25892_AUX_SYSFS_FIELD_RO(sys_volt, 0F, SYSV),
	BQ25892_AUX_SYSFS_FIELD_RO(vbus_volt, 11, VBUSV),
	BQ25892_AUX_SYSFS_FIELD_RO(ichg_adc, 12, ICHGR),
	BQ25892_AUX_SYSFS_FIELD_RO(vdpm_stat, 13, VDPM_STAT),
	BQ25892_AUX_SYSFS_FIELD_RO(idpm_stat, 13, IDPM_STAT),
	BQ25892_AUX_SYSFS_FIELD_RO(idpm_lim, 13, IDPM_LIM),
	BQ25892_AUX_SYSFS_FIELD_RO(ico_optimized, 14, ICO_OPTIMIZED),
	BQ25892_AUX_SYSFS_FIELD_RW(reg_addr, NONE, NONE),
	BQ25892_AUX_SYSFS_FIELD_RW(reg_value, NONE, NONE),
};

static struct attribute *bq25892_aux_sysfs_attrs[
	ARRAY_SIZE(bq25892_aux_sysfs_field_tbl) + 1];

static const struct attribute_group bq25892_aux_sysfs_attr_group = {
	.attrs = bq25892_aux_sysfs_attrs,
};

static void bq25892_aux_sysfs_init_attrs(void)
{
	int i, limit = ARRAY_SIZE(bq25892_aux_sysfs_field_tbl);

	for (i = 0; i < limit; i++) {
		bq25892_aux_sysfs_attrs[i] =
			&bq25892_aux_sysfs_field_tbl[i].attr.attr;
	}

	bq25892_aux_sysfs_attrs[limit] = NULL;
}

static struct bq25892_aux_sysfs_field_info *bq25892_aux_sysfs_field_lookup(
	const char *name)
{
	int i, limit = ARRAY_SIZE(bq25892_aux_sysfs_field_tbl);

	for (i = 0; i < limit; i++) {
		if (!strcmp(name,
			bq25892_aux_sysfs_field_tbl[i].attr.attr.name))
			break;
	}

	if (i >= limit)
		return NULL;

	return &bq25892_aux_sysfs_field_tbl[i];
}

static ssize_t bq25892_aux_sysfs_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct bq25892_aux_sysfs_field_info *info;
	struct bq25892_aux_sysfs_field_info *info2;
	int ret;
	u8 v;

	info = bq25892_aux_sysfs_field_lookup(attr->attr.name);
	if (info == NULL) {
		hwlog_err("get sysfs entries failed\n");
		return -EINVAL;
	}

	if (!strncmp("reg_addr", attr->attr.name, strlen("reg_addr")))
		return scnprintf(buf, PAGE_SIZE, "0x%hhx\n", info->reg);

	if (!strncmp(("reg_value"), attr->attr.name, strlen("reg_value"))) {
		info2 = bq25892_aux_sysfs_field_lookup("reg_addr");
		if (info2 == NULL) {
			hwlog_err("get sysfs entries failed\n");
			return -EINVAL;
		}

		info->reg = info2->reg;
	}

	ret = bq25892_aux_read_mask(info->reg, info->mask, info->shift, &v);
	if (ret)
		return ret;

	return scnprintf(buf, PAGE_SIZE, "%hhx\n", v);
}

static ssize_t bq25892_aux_sysfs_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct bq25892_aux_sysfs_field_info *info;
	struct bq25892_aux_sysfs_field_info *info2;
	int ret;
	u8 v;

	info = bq25892_aux_sysfs_field_lookup(attr->attr.name);
	if (info == NULL) {
		hwlog_err("get sysfs entries failed\n");
		return -EINVAL;
	}

	ret = kstrtou8(buf, 0, &v);
	if (ret < 0) {
		hwlog_err("get kstrtou8 failed\n");
		return ret;
	}

	if (!strncmp(("reg_value"), attr->attr.name, strlen("reg_value"))) {
		info2 = bq25892_aux_sysfs_field_lookup("reg_addr");
		if (info2 == NULL) {
			hwlog_err("get sysfs entries failed\n");
			return -EINVAL;
		}

		info->reg = info2->reg;
	}
	if (!strncmp(("reg_addr"), attr->attr.name, strlen("reg_addr"))) {
		if (v < (u8) BQ25892_AUX_REG_TOTAL_NUM && v >= (u8) 0x00) {
			info->reg = v;
			return count;
		} else {
			return -EINVAL;
		}
	}

	ret = bq25892_aux_write_mask(info->reg, info->mask, info->shift, v);
	if (ret)
		return ret;

	return count;
}

static int bq25892_aux_sysfs_create_group(struct bq25892_aux_device_info *di)
{
	bq25892_aux_sysfs_init_attrs();

	return sysfs_create_group(&di->dev->kobj,
		&bq25892_aux_sysfs_attr_group);
}

static void bq25892_aux_sysfs_remove_group(struct bq25892_aux_device_info *di)
{
	sysfs_remove_group(&di->dev->kobj, &bq25892_aux_sysfs_attr_group);
}

#else

static int bq25892_aux_sysfs_create_group(struct bq25892_aux_device_info *di)
{
	return 0;
}

static inline void bq25892_aux_sysfs_remove_group(
	struct bq25892_aux_device_info *di)
{
}

#endif /* CONFIG_SYSFS */

static int bq25892_aux_device_check(void)
{
	int ret = 0;
	u8 reg = 0xff;

	ret |= bq25892_aux_read_byte(BQ25892_AUX_REG_14, &reg);
	if (ret)
		return CHARGE_IC_BAD;

	hwlog_info("device_check [%x]=0x%x\n", BQ25892_AUX_REG_14, reg);

	if ((BQ25892 == (reg & CHIP_VERSION_MASK)) &&
		(CHIP_REVISION == (reg & CHIP_REVISION_MASK))) {
		hwlog_info("bq25892 aux is good\n");
		return CHARGE_IC_GOOD;
	}

	hwlog_err("bq25892 aux is bad\n");
	return CHARGE_IC_BAD;
}

static int bq25892_aux_set_bat_comp(int value)
{
	int bat_comp = 0;
	u8 bat_comp_reg = 0;

	bat_comp = value;

	if (bat_comp < BAT_COMP_MIN_0) {
		hwlog_err("set bat_comp %d, out of range:%d",
			value, BAT_COMP_MIN_0);
		bat_comp = BAT_COMP_MIN_0;
	} else if (bat_comp > BAT_COMP_MAX_140) {
		hwlog_err("set bat_comp %d, out of range:%d",
			value, BAT_COMP_MAX_140);
		bat_comp = BAT_COMP_MAX_140;
	} else {
		/* do nothing */
	}

	bat_comp_reg = (bat_comp - BAT_COMP_MIN_0) / BAT_COMP_STEP_20;

	hwlog_info("set_bat_comp [%x]=0x%x\n",
		BQ25892_AUX_REG_08, bat_comp_reg);

	return bq25892_aux_write_mask(BQ25892_AUX_REG_08,
			BQ25892_AUX_REG_08_BAT_COMP_MASK,
			BQ25892_AUX_REG_08_BAT_COMP_SHIFT,
			bat_comp_reg);
}

static int bq25892_aux_set_vclamp(int value)
{
	int vclamp = 0;
	u8 vclamp_reg = 0;

	vclamp = value;

	if (vclamp < VCLAMP_MIN_0) {
		hwlog_err("set vclamp %d, out of range:%d",
			value, VCLAMP_MIN_0);
		vclamp = VCLAMP_MIN_0;
	} else if (vclamp > VCLAMP_MAX_224) {
		hwlog_err("set vclamp %d, out of range:%d",
			value, VCLAMP_MAX_224);
		vclamp = VCLAMP_MAX_224;
	} else {
		/* do nothing */
	}

	vclamp_reg = (vclamp - VCLAMP_MIN_0) / VCLAMP_STEP_32;

	hwlog_info("set_vclamp [%x]=0x%x\n", BQ25892_AUX_REG_08, vclamp_reg);

	return bq25892_aux_write_mask(BQ25892_AUX_REG_08,
			BQ25892_AUX_REG_08_VCLAMP_MASK,
			BQ25892_AUX_REG_08_VCLAMP_SHIFT,
			vclamp_reg);
}

static int bq25892_aux_set_covn_start(int enable)
{
	int ret = 0;
	u8 reg = 0;
	int i = 0;

	ret = bq25892_aux_read_byte(BQ25892_AUX_REG_0B, &reg);
	if (ret)
		return -1;

	if (!(reg & BQ25892_AUX_NOT_PG_STAT)) {
		hwlog_err("bq25892_aux PG NOT GOOD, can not set covn,reg:%x\n",
			reg);
		return -1;
	}

	ret = bq25892_aux_write_mask(BQ25892_AUX_REG_02,
			BQ25892_AUX_REG_02_CONV_START_MASK,
			BQ25892_AUX_REG_02_CONV_START_SHIFT,
			enable);
	if (ret)
		return -1;

	/* The conversion result is ready after tCONV, max (10*100)ms */
	for (i = 0; i < 10; i++) {
		ret = bq25892_aux_read_mask(BQ25892_AUX_REG_02,
				BQ25892_AUX_REG_02_CONV_START_MASK,
				BQ25892_AUX_REG_02_CONV_START_SHIFT,
				&reg);
		if (ret)
			return -1;

		/* if ADC Conversion finished, CONV_RATE bit will be 0 */
		if (reg == 0)
			break;

		msleep(100); /* sleep 100ms */
	}

	hwlog_info("one-shot covn start is set %d\n", enable);
	return 0;
}

static int bq25892_aux_5v_chip_init(struct bq25892_aux_device_info *di)
{
	int ret = 0;

	/* reg init */
	/*
	 * bq25892_aux_write_mask(REG0x14,
	 * BQ25892_REG_RST_MASK, BQ25892_REG_RST_SHIFT,
	 * 0x01);
	 */
	/*
	 * do not init input current 500 ma(REG00)
	 * to support lpt without battery
	 */

	/* 02 enable Start 1s Continuous Conversion, others as default */
	ret |= bq25892_aux_write_byte(BQ25892_AUX_REG_02, 0x1D); /*adc off*/

	/* 03 WD_RST 1,CHG_CONFIG 1,SYS_MIN 3.5 */
	ret |= bq25892_aux_write_byte(BQ25892_AUX_REG_03, 0x5A);

	/* 04 Fast Charge Current Limit 1024mA */
	ret |= bq25892_aux_write_byte(BQ25892_AUX_REG_04, 0x10);

	/* 05 Precharge Current Limit 256mA,Termination Current Limit 256mA */
	ret |= bq25892_aux_write_byte(BQ25892_AUX_REG_05, 0x33);

	/*
	 * 06 Charge Voltage Limit 4.4,
	 * Battery Precharge to Fast Charge Threshold 3, Battery Recharge 100mV
	 */
	ret |= bq25892_aux_write_byte(BQ25892_AUX_REG_06, 0x8e);

	/*
	 * 07 EN_TERM 0, Watchdog Timer 80s, EN_TIMER 1, Charge Timer 20h,
	 * JEITA Low Temperature Current Setting 1
	 */
	ret |= bq25892_aux_write_byte(BQ25892_AUX_REG_07, 0x2f);

	/*
	 * 08 IR compensation voatge clamp = 224mV,
	 * IR compensation resistor setting = 80mohm
	 */
	ret |= bq25892_aux_set_bat_comp(di->param_dts.bat_comp);
	ret |= bq25892_aux_set_vclamp(di->param_dts.vclamp);

	/* boost mode current limit = 500mA,boostv 4.998v */
	ret |= bq25892_aux_write_byte(BQ25892_AUX_REG_0A, 0x70);

	/* VINDPM Threshold Setting Method 1, Absolute VINDPM Threshold 4.4v */
	/* ret = bq25892_aux_write_byte(BQ25892_AUX_REG_0D,0x92); */

	/* enable charging */
	gpio_set_value(di->gpio_cd, 0);

	return ret;
}

static int bq25892_aux_set_adc_conv_rate(int mode)
{
	if (mode)
		mode = 1;

	hwlog_info("adc conversion rate mode is set to %d\n", mode);

	return bq25892_aux_write_mask(BQ25892_AUX_REG_02,
			BQ25892_AUX_REG_02_CONV_RATE_MASK,
			BQ25892_AUX_REG_02_CONV_RATE_SHIFT,
			mode);
}

/*
 * ONLY TI bq25892 charge chip has a bug.
 * When charger is in charging done state and under PFM mode,
 * there is risk to slowly drive Q4 to open when unplug charger.
 * The result is VSYS drops to 2.0V and reach milliseconds interval.
 * The VSYS drop can be captured by hi6421 that result in SMPL.
 * The solution that TI suggest: when charger chip is bq25892,
 * and under charging done state, set IINLIM(reg00 bit0-bit5) to 400mA or
 * below to force chargeIC to exit PFM
 */
static int bq25892_aux_exit_pfm_when_cd(unsigned int limit_default)
{
	u8 reg0b = 0;
	u8 reg14 = 0;
	int ret = 0;

	ret |= bq25892_aux_read_byte(BQ25892_AUX_REG_0B, &reg0b);
	ret |= bq25892_aux_read_byte(BQ25892_AUX_REG_14, &reg14);
	if (ret)
		return limit_default;

	if (((reg14 & BQ25892_AUX_REG_14_REG_PN_MASK) ==
		BQ25892_AUX_REG_14_REG_PN_IS_25892) && /* bq25892 */
		(limit_default > IINLIM_FOR_BQ25892_EXIT_PFM)) {
		/* charging done */
		if ((reg0b & BQ25892_AUX_REG_0B_CHRG_STAT_MASK) ==
			BQ25892_AUX_CHGR_STAT_CHARGE_DONE) {
			/* first time charging done */
			if (first_charging_done == FALSE) {
				first_charging_done = TRUE;
				limit_default = IINLIM_FOR_BQ25892_EXIT_PFM;
			 /* charging done and there is NOT second charging */
			} else if (charging_again_after_charging_done ==
				FALSE) {
				limit_default = IINLIM_FOR_BQ25892_EXIT_PFM;
			} else {
				/*
				 * charging done but thers is second charging,
				 * keep iinlim as upper layer suggests
				 */
			}

			if (limit_default == IINLIM_FOR_BQ25892_EXIT_PFM) {
				hwlog_err("1stCD(%d), 2nd FC(%d), limit input current to %d to force exit PFM\n",
					first_charging_done,
					charging_again_after_charging_done,
					limit_default);
			}
		/* fast charging,  is it ever charging done? */
		} else if (first_charging_done == TRUE) {
			/* keep iinlim as upper layer suggests */
			charging_again_after_charging_done = TRUE;
		} else {
			/*
			 * keep iinlim as upper layer suggests,
			 * as normal charging before first charging done
			 */
		}
	}

	return limit_default;
}

static int bq25892_aux_set_input_current(int value)
{
	unsigned int limit_current = 0;
	u8 iin_limit = 0;

	limit_current = value;

	if (limit_current <= IINLIM_MIN_100) {
		hwlog_err("set input_current %d, out of range:%d",
			value, IINLIM_MIN_100);
		limit_current = IINLIM_MIN_100;
	} else if (limit_current > IINLIM_MAX_3250) {
		hwlog_err("set input_current %d, out of range:%d",
			value, IINLIM_MAX_3250);
		limit_current = IINLIM_MAX_3250;
	} else {
		/* do nothing */
	}

	hwlog_info("input current is set %dmA\n", limit_current);

	/*
	 * in order to avoid smpl because bq25892 bug,
	 * set iinlim to 400mA if under charging done and chip ic is bq25892,
	 * otherwise keep it change
	 */
	limit_current = bq25892_aux_exit_pfm_when_cd(limit_current);
	hwlog_debug("bq25892 aux input current is set %dmA\n", limit_current);

	iin_limit = (limit_current - IINLIM_MIN_100) / IINLIM_STEP_50;
	return bq25892_aux_write_mask(BQ25892_AUX_REG_00,
			BQ25892_AUX_REG_00_IINLIM_MASK,
			BQ25892_AUX_REG_00_IINLIM_SHIFT,
			iin_limit);
}

static int bq25892_aux_set_charge_current(int value)
{
	int currentma = 0;
	u8 ichg = 0;

	currentma = value;

	if (currentma < 0) {
		currentma = 0;
	} else if (currentma > ICHG_MAX_5056) {
		hwlog_err("set charge_current %d, out of range:%d!",
			value, ICHG_MAX_5056);
		currentma = ICHG_MAX_5056;
	} else {
		/* do nothing */
	}

	hwlog_info("charge current is set %dmA\n", currentma);

	ichg = currentma / ICHG_STEP_64;

	return bq25892_aux_write_mask(BQ25892_AUX_REG_04,
			BQ25892_AUX_REG_04_ICHG_MASK,
			BQ25892_AUX_REG_04_ICHG_SHIFT,
			ichg);
}

static int bq25892_aux_set_terminal_voltage(int value)
{
	unsigned int voltagemv = 0;
	u8 voreg = 0;

	voltagemv = value;

	if (voltagemv < VCHARGE_MIN_3840) {
		hwlog_err("set terminal_voltage %d, out of range:%d",
			value, VCHARGE_MIN_3840);
		voltagemv = VCHARGE_MIN_3840;
	} else if (voltagemv > VCHARGE_MAX_4496) {
		hwlog_err("set terminal_voltage %d, out of range:%d",
			value, VCHARGE_MAX_4496);
		voltagemv = VCHARGE_MAX_4496;
	}

	hwlog_info("terminal voltage is set %dmV\n", voltagemv);

	voreg = (voltagemv - VCHARGE_MIN_3840) / VCHARGE_STEP_16;

	return bq25892_aux_write_mask(BQ25892_AUX_REG_06,
			BQ25892_AUX_REG_06_VREG_MASK,
			BQ25892_AUX_REG_06_VREG_SHIFT,
			voreg);
}

static int bq25892_aux_set_dpm_voltage(int value)
{
	int vindpm_voltage;
	u8 vindpm;
	u8 reg0d;

	vindpm_voltage = value;

	if (vindpm_voltage > VINDPM_MAX_15300) {
		hwlog_err("set dpm_voltage %d, out of range:%d",
			value, VINDPM_MAX_15300);
		vindpm_voltage = VINDPM_MAX_15300;
	} else if (vindpm_voltage < VINDPM_MIN_3900) {
		hwlog_err("set dpm_voltage %d, out of range:%d",
			value, VINDPM_MIN_3900);
		vindpm_voltage = VINDPM_MIN_3900;
	} else {
		/* do nothing */
	}

	hwlog_info("vindpm absolute voltage is set %dmV\n", vindpm_voltage);

	vindpm = (vindpm_voltage - VINDPM_OFFSET_2600) / VINDPM_STEP_100;

	/* 1 absolute dpm */
	reg0d = (1 << BQ25892_AUX_REG_0D_FORCE_VINDPM_SHIFT) | vindpm;

	return bq25892_aux_write_byte(BQ25892_AUX_REG_0D, reg0d);
}

static int bq25892_aux_set_terminal_current(int value)
{
	unsigned int term_currentma = 0;
	u8 iterm_reg = 0;

	term_currentma = value;

	if (term_currentma < ITERM_MIN_64) {
		hwlog_err("set terminal_current %d, out of range:%d",
			value, ITERM_MIN_64);
		term_currentma = ITERM_MIN_64;
	} else if (term_currentma > ITERM_MAX_1024) {
		hwlog_err("set terminal_current %d, out of range:%d",
			value, ITERM_MAX_1024);
		term_currentma = ITERM_MAX_1024;
	} else {
		/* do nothing */
	}

	hwlog_info("term current is set %dmA\n", term_currentma);

	iterm_reg = term_currentma / ITERM_STEP_64;

	return bq25892_aux_write_mask(BQ25892_AUX_REG_05,
			BQ25892_AUX_REG_05_ITERM_MASK,
			BQ25892_AUX_REG_05_ITERM_SHIFT,
			iterm_reg);
}

static int bq25892_aux_set_charge_enable(int enable)
{
	struct bq25892_aux_device_info *di = g_bq25892_aux_dev;

	gpio_set_value(di->gpio_cd, !enable);

	return bq25892_aux_write_mask(BQ25892_AUX_REG_03,
			BQ25892_AUX_REG_03_CHG_CONFIG_MASK,
			BQ25892_AUX_REG_03_CHG_CONFIG_SHIFT,
			enable);
}

static int bq25892_aux_set_otg_enable(int enable)
{
	int val = 0;
	struct bq25892_aux_device_info *di = g_bq25892_aux_dev;

	gpio_set_value(di->gpio_cd, !enable);

	val = enable << 1;
	return bq25892_aux_write_mask(BQ25892_AUX_REG_03,
			BQ25892_AUX_REG_03_CHG_CONFIG_MASK,
			BQ25892_AUX_REG_03_CHG_CONFIG_SHIFT,
			val);
}

static int bq25892_aux_set_term_enable(int enable)
{
	return bq25892_aux_write_mask(BQ25892_AUX_REG_07,
			BQ25892_AUX_REG_07_EN_TERM_MASK,
			BQ25892_AUX_REG_07_EN_TERM_SHIFT,
			enable);
}

static int bq25892_aux_get_charge_state(unsigned int *state)
{
	u8 reg = 0;
	int ret = 0;

	ret = bq25892_aux_read_byte(BQ25892_AUX_REG_0B, &reg);

	hwlog_info("get_charge_state [%x]=0x%x\n", BQ25892_AUX_REG_0B, reg);

	if (!(reg & BQ25892_AUX_NOT_PG_STAT))
		*state |= CHAGRE_STATE_NOT_PG;

	if ((reg & BQ25892_AUX_CHGR_STAT_CHARGE_DONE) ==
		BQ25892_AUX_CHGR_STAT_CHARGE_DONE)
		*state |= CHAGRE_STATE_CHRG_DONE;

	ret |= bq25892_aux_read_byte(BQ25892_AUX_REG_0C, &reg);
	ret |= bq25892_aux_read_byte(BQ25892_AUX_REG_0C, &reg);

	hwlog_info("get_charge_state [%x]=0x%x\n", BQ25892_AUX_REG_0C, reg);

	if ((reg & BQ25892_AUX_WATCHDOG_FAULT) == BQ25892_AUX_WATCHDOG_FAULT)
		*state |= CHAGRE_STATE_WDT_FAULT;

	if ((reg & BQ25892_AUX_POWER_SUPPLY_OVP) ==
		BQ25892_AUX_POWER_SUPPLY_OVP)
		*state |= CHAGRE_STATE_VBUS_OVP;

	if ((reg & BQ25892_AUX_BAT_FAULT_OVP) == BQ25892_AUX_BAT_FAULT_OVP)
		*state |= CHAGRE_STATE_BATT_OVP;

	if ((reg & BQ25892_AUX_NTC_FAULT) > BQ25892_AUX_NTC_NORMAL)
		*state |= CHAGRE_STATE_NTC_FAULT;

	if (reg != 0)
		hwlog_err("state is not normal, BQ25892_aux REG[0x0C] is %x\n",
			reg);

	return ret;
}

static int bq25892_aux_reset_watchdog_timer(void)
{
	return bq25892_aux_write_mask(BQ25892_AUX_REG_03,
			BQ25892_AUX_REG_03_WDT_RESET_MASK,
			BQ25892_AUX_REG_03_WDT_RESET_SHIFT,
			0x01);
}

static int bq25892_aux_get_ilim(void)
{
	int ret;
	u8 ichg_reg, vbat_reg, vbus_reg;
	int ichg, vbat, vbus;

	ret = bq25892_aux_read_mask(BQ25892_AUX_REG_12,
			BQ25892_AUX_REG_12_ICHGR_MASK,
			BQ25892_AUX_REG_12_ICHGR_SHIFT,
			&ichg_reg);
	if (ret)
		return -1;

	ichg = ichg_reg * BQ25892_AUX_REG_12_ICHGR_STEP_MA +
			BQ25892_AUX_REG_12_ICHGR_OFFSET_MA;

	ret = bq25892_aux_read_mask(BQ25892_AUX_REG_0E,
			BQ25892_AUX_REG_0E_BATV_MASK,
			BQ25892_AUX_REG_0E_BATV_SHIFT,
			&vbat_reg);
	if (ret)
		return -1;

	vbat = vbat_reg * BQ25892_AUX_REG_0E_VBAT_STEP_MV +
			BQ25892_AUX_REG_0E_VBAT_OFFSET_MV;

	ret = bq25892_aux_read_mask(BQ25892_AUX_REG_11,
			BQ25892_AUX_REG_11_VBUSV_MASK,
			BQ25892_AUX_REG_11_VBUSV_SHIFT,
			&vbus_reg);
	if (ret)
		return -1;

	vbus = vbus_reg * BQ25892_AUX_REG_11_VBUSV_STEP_MV +
			BQ25892_AUX_REG_11_VBUSV_OFFSET_MV;

	if (vbus == 0)
		return 0;

	return vbat * ichg * 100 / 88 / vbus; /* ilim ratio is 100/88 */
}

static int bq25892_aux_dump_register(char *reg_value)
{
	u8 reg[BQ25892_AUX_REG_TOTAL_NUM] = {0};
	char buff[BUF_LEN] = {0};
	int i = 0;

	snprintf(buff, 26, "%-10d", bq25892_aux_get_ilim());
	strncat(reg_value, buff, strlen(buff));

	for (i = 0; i < BQ25892_AUX_REG_TOTAL_NUM; i++) {
		bq25892_aux_read_byte(i, &reg[i]);
		bq25892_aux_read_byte(i, &reg[i]);
		snprintf(buff, BUF_LEN, "0x%-10.2x", reg[i]);
		strncat(reg_value, buff, strlen(buff));
	}

	return 0;
}

static int bq25892_aux_get_register_head(char *reg_head)
{
	char buff[BUF_LEN] = {0};
	int i = 0;

	snprintf(buff, BUF_LEN, "Ibus_aux  ");
	strncat(reg_head, buff, strlen(buff));

	for (i = 0; i < BQ25892_AUX_REG_TOTAL_NUM; i++) {
		snprintf(buff, BUF_LEN, "Reg_aux[%02x] ", i);
		strncat(reg_head, buff, strlen(buff));
	}

	return 0;
}

static int bq25892_aux_set_batfet_disable(int disable)
{
	return bq25892_aux_write_mask(BQ25892_AUX_REG_09,
			BQ25892_AUX_REG_09_BATFET_DISABLE_MASK,
			BQ25892_AUX_REG_09_BATFET_DISABLE_SHIFT,
			disable);
}

static int bq25892_aux_set_watchdog_timer(int value)
{
	u8 val = 0;
	u8 dog_time = value;

	if (dog_time >= WATCHDOG_TIMER_160_S)
		val = BQ25892_AUX_REG_07_WATCHDOG_160;
	else if (dog_time >= WATCHDOG_TIMER_80_S)
		val = BQ25892_AUX_REG_07_WATCHDOG_80;
	else if (dog_time >= WATCHDOG_TIMER_40_S)
		val = BQ25892_AUX_REG_07_WATCHDOG_40;
	else
		val = BQ25892_AUX_REG_07_WATCHDOG_DIS;

	hwlog_info("watch dog timer is %d ,the register value is set %d\n",
		dog_time, val);

	return bq25892_aux_write_mask(BQ25892_AUX_REG_07,
			BQ25892_AUX_REG_07_WATCHDOG_MASK,
			BQ25892_AUX_REG_07_WATCHDOG_SHIFT,
			val);
}

static int bq25892_aux_set_charger_hiz(int enable)
{
	int ret = 0;

	if (enable > 0) {
		ret |= bq25892_aux_write_mask(BQ25892_AUX_REG_00,
				BQ25892_AUX_REG_00_EN_HIZ_MASK,
				BQ25892_AUX_REG_00_EN_HIZ_SHIFT,
				TRUE);
	} else {
		ret |= bq25892_aux_write_mask(BQ25892_AUX_REG_00,
				BQ25892_AUX_REG_00_EN_HIZ_MASK,
				BQ25892_AUX_REG_00_EN_HIZ_SHIFT,
				FALSE);
	}

	return ret;
}

static int bq25892_aux_9v_chip_init(struct bq25892_aux_device_info *di)
{
	int ret = 0;

	/* reg init */
	/*
	 * bq25892_write_mask(REG0x14,
	 * BQ25892_AUX_REG_RST_MASK, BQ25892_AUX_REG_RST_SHIFT,
	 * 0x01);
	 */
	/*
	 * do not init input current 500 ma(REG00)
	 * to support lpt without battery
	 */

	/* 02 enable Start 1s Continuous Conversion ,others as default */
	ret |= bq25892_aux_write_byte(BQ25892_AUX_REG_02, 0x1D); /*adc off*/

	/* 03 WD_RST 1,CHG_CONFIG 1,SYS_MIN 3.5 */
	ret |= bq25892_aux_write_byte(BQ25892_AUX_REG_03, 0x5A);

	/* 04 Fast Charge Current Limit 1024mA */
	ret |= bq25892_aux_write_byte(BQ25892_AUX_REG_04, 0x10);

	/* 05 Precharge Current Limit 256mA,Termination Current Limit 256mA */
	ret |= bq25892_aux_write_byte(BQ25892_AUX_REG_05, 0x33);

	/*
	 * 06 Charge Voltage Limit 4.4,
	 * Battery Precharge to Fast Charge Threshold 3,Battery Recharge 100mV
	 */
	ret |= bq25892_aux_write_byte(BQ25892_AUX_REG_06, 0x8e);

	/*
	 * 07 EN_TERM 1, Watchdog Timer 80s, EN_TIMER 1,
	 * Charge Timer 20h,JEITA Low Temperature Current Setting 1
	 */
	ret |= bq25892_aux_write_byte(BQ25892_AUX_REG_07, 0x2f);

	/*
	 * 08 IR compensation voatge clamp = 224mV,
	 * IR compensation resistor setting = 80mohm
	 */
	ret |= bq25892_aux_set_bat_comp(di->param_dts.bat_comp);
	ret |= bq25892_aux_set_vclamp(di->param_dts.vclamp);

	/*
	 * 09 FORCE_ICO 0, TMR2X_EN 1, BATFET_DIS 0,
	 * JEITA_VSET 0, BATFET_RST_EN 1
	 */
	ret |= bq25892_aux_write_byte(BQ25892_AUX_REG_09, 0x44);

	/* boost mode current limit = 500mA,boostv 4.998v */
	ret |= bq25892_aux_write_byte(BQ25892_AUX_REG_0A, 0x70);

	/* VINDPM Threshold Setting Method 1,Absolute VINDPM Threshold 4.4v */
	/* ret = bq25892_aux_write_byte(BQ25892_AUX_REG_0D,0x92); */

	/*
	 * set dpm voltage as 4700mv instead of 7600mv
	 * because chargerIC cannot reset dpm after watchdog time out
	 */
	ret = bq25892_aux_set_dpm_voltage(4700);

	/*enable charging*/
	gpio_set_value(di->gpio_cd, 0);

	return ret;
}

static int bq25892_aux_chip_init(struct chip_init_crit *init_crit)
{
	int ret = -1;
	struct bq25892_aux_device_info *di = g_bq25892_aux_dev;

	if (di == NULL || init_crit == NULL) {
		hwlog_err("di or init_crit is null\n");
		return -ENOMEM;
	}

	switch (init_crit->vbus) {
	case ADAPTER_5V:
		ret = bq25892_aux_5v_chip_init(di);
		break;

	case ADAPTER_9V:
		ret = bq25892_aux_9v_chip_init(di);
		break;

	default:
		hwlog_err("invaid init_crit vbus mode\n");
		break;
	}

	return ret;
}

static int bq25892_aux_check_input_dpm_state(void)
{
	u8 reg = 0;
	int ret = -1;

	ret = bq25892_aux_read_byte(BQ25892_AUX_REG_13, &reg);
	if (ret < 0)
		return ret;

	hwlog_info("check_input_dpm_state [%x]=0x%x\n",
		BQ25892_AUX_REG_13, reg);

	if ((reg & BQ25892_AUX_REG_13_VDPM_STAT_MASK) ||
		(reg & BQ25892_AUX_REG_13_IDPM_STAT_MASK))
		return TRUE;
	else
		return FALSE;
}

static int bq25892_aux_stop_charge_config(void)
{
	/* reset to prepare for next charger plug */
	first_charging_done = FALSE;
	charging_again_after_charging_done = FALSE;

	/*
	 * as vindpm of bq25892_aux won't be reset after watchdog timer out,
	 * if vindpm is higher than 5v ,IC will not supply power with USB/AC
	 */
	return bq25892_aux_set_dpm_voltage(CHARGE_VOLTAGE_4520_MV);
}

struct charge_device_ops bq25892_aux_ops = {
	.chip_init = bq25892_aux_chip_init,
	.dev_check = bq25892_aux_device_check,
	.set_adc_conv_rate = bq25892_aux_set_adc_conv_rate,
	.set_input_current = bq25892_aux_set_input_current,
	.set_charge_current = bq25892_aux_set_charge_current,
	.set_terminal_voltage = bq25892_aux_set_terminal_voltage,
	.set_dpm_voltage = bq25892_aux_set_dpm_voltage,
	.set_terminal_current = bq25892_aux_set_terminal_current,
	.set_charge_enable = bq25892_aux_set_charge_enable,
	.set_otg_enable = bq25892_aux_set_otg_enable,
	.set_term_enable = bq25892_aux_set_term_enable,
	.get_charge_state = bq25892_aux_get_charge_state,
	.reset_watchdog_timer = bq25892_aux_reset_watchdog_timer,
	.dump_register = bq25892_aux_dump_register,
	.get_register_head = bq25892_aux_get_register_head,
	.set_watchdog_timer = bq25892_aux_set_watchdog_timer,
	.set_batfet_disable = bq25892_aux_set_batfet_disable,
	.get_ibus = bq25892_aux_get_ilim,
	.set_covn_start = bq25892_aux_set_covn_start,
	.set_charger_hiz = bq25892_aux_set_charger_hiz,
	.check_input_dpm_state = bq25892_aux_check_input_dpm_state,
	.stop_charge_config = bq25892_aux_stop_charge_config,
};

static void bq25892_aux_irq_work(struct work_struct *work)
{
	struct bq25892_aux_device_info *di;
	u8 reg = 0, reg1 = 0;

	di = container_of(work, struct bq25892_aux_device_info, irq_work);

	msleep(100); /* sleep 100ms */

	bq25892_aux_read_byte(BQ25892_AUX_REG_0B, &reg1);
	bq25892_aux_read_byte(BQ25892_AUX_REG_0C, &reg);
	hwlog_info("1st reg[0xB]:0x%x,reg[0xC]:0x%0x\n", reg1, reg);

	bq25892_aux_read_byte(BQ25892_AUX_REG_0C, &reg);
	hwlog_info("2nd reg[0xB]:0x%x,reg[0xC]:0x%0x\n", reg1, reg);

	if (reg & BQ25892_AUX_REG_0C_BOOST) {
		hwlog_info("CHARGE_FAULT_BOOST_OCP happened\n");

		atomic_notifier_call_chain(&fault_notifier_list,
			CHARGE_FAULT_BOOST_OCP, NULL);
	}

	if (di->irq_active == 0) {
		di->irq_active = 1;
		enable_irq(di->irq_int);
	}
}

static irqreturn_t bq25892_aux_interrupt(int irq, void *_di)
{
	struct bq25892_aux_device_info *di = _di;

	if (di == NULL) {
		hwlog_err("di is null\n");
		return -1;
	}

	hwlog_info("bq25892_aux int happened (%d)\n", di->irq_active);

	if (di->irq_active == 1) {
		di->irq_active = 0;
		disable_irq_nosync(di->irq_int);
		schedule_work(&di->irq_work);
	} else {
		hwlog_info("the irq is not enable,do nothing\n");
	}

	return IRQ_HANDLED;
}

static void bq25892_aux_parse_dts(struct device_node *np,
	struct bq25892_aux_device_info *di)
{
	struct device_node *batt_node;
	int ret = 0;

	di->param_dts.bat_comp = 80;
	di->param_dts.vclamp = 224;

	ret = of_property_read_u32(np, "bat_comp", &(di->param_dts.bat_comp));
	if (ret) {
		hwlog_err("bat_comp dts read failed\n");
		return;
	}
	hwlog_info("bat_comp=%d\n", di->param_dts.bat_comp);

	ret = of_property_read_u32(np, "vclamp", &(di->param_dts.vclamp));
	if (ret) {
		hwlog_err("vclamp dts read failed\n");
		return;
	}
	hwlog_info("vclamp=%d\n", di->param_dts.vclamp);

	ret = of_property_read_u32(np, "rilim", &rilim);
	if (ret)
		hwlog_err("rilim dts read failed\n");

	hwlog_info("rilim=%d\n", rilim);

	batt_node = of_find_compatible_node(NULL, NULL,
		"huawei,hisi_bci_battery");
	if (batt_node != NULL) {
		ret = of_property_read_u32(batt_node, "battery_board_type",
			&is_board_type);
		if (ret) {
			hwlog_err("battery_board_type dts read failed\n");
			is_board_type = BAT_BOARD_ASIC;
		}
	} else {
		hwlog_err("hisi_bci_battery dts read failed\n");
		is_board_type = BAT_BOARD_ASIC;
	}
}

static int bq25892_aux_probe(struct i2c_client *client,
	const struct i2c_device_id *id)
{
	int ret = 0;
	struct bq25892_aux_device_info *di = NULL;
	struct device_node *np = NULL;
	struct class *power_class = NULL;

	hwlog_info("probe begin\n");

	if (client == NULL || id == NULL) {
		hwlog_err("client or id is null\n");
		return -ENOMEM;
	}

	di = devm_kzalloc(&client->dev, sizeof(*di), GFP_KERNEL);
	if (di == NULL)
		return -ENOMEM;

	g_bq25892_aux_dev = di;

	di->dev = &client->dev;
	np = di->dev->of_node;
	di->client = client;
	i2c_set_clientdata(client, di);

	INIT_WORK(&di->irq_work, bq25892_aux_irq_work);

	bq25892_aux_parse_dts(np, di);

	di->gpio_cd = of_get_named_gpio(np, "gpio_cd", 0);
	hwlog_info("gpio_cd=%d\n", di->gpio_cd);

	if (!gpio_is_valid(di->gpio_cd)) {
		hwlog_err("gpio(gpio_cd) is not valid\n");
		ret = -EINVAL;
		goto bq25892_aux_fail_0;
	}

	di->gpio_int = of_get_named_gpio(np, "gpio_int", 0);
	hwlog_info("gpio_int=%d\n", di->gpio_int);

	if (!gpio_is_valid(di->gpio_int)) {
		hwlog_err("gpio(gpio_int) is not valid\n");
		ret = -EINVAL;
		goto bq25892_aux_fail_0;
	}

	ret = gpio_request(di->gpio_cd, "charger_cd");
	if (ret) {
		hwlog_err("gpio(gpio_cd) request fail\n");
		goto bq25892_aux_fail_0;
	}

	/* set gpio to control CD pin to disable/enable bq25892_aux IC */
	gpio_direction_output(di->gpio_cd, 0);
	if (ret) {
		hwlog_err("gpio(gpio_cd) set output fail\n");
		goto bq25892_aux_fail_1;
	}

	ret = gpio_request(di->gpio_int, "charger_int");
	if (ret) {
		hwlog_err("gpio(gpio_int) request fail\n");
		goto bq25892_aux_fail_1;
	}

	gpio_direction_input(di->gpio_int);
	if (ret) {
		hwlog_err("gpio(gpio_int) set input fail\n");
		goto bq25892_aux_fail_2;
	}

	di->irq_int = gpio_to_irq(di->gpio_int);
	if (di->irq_int < 0) {
		hwlog_err("gpio(gpio_int) map to irq fail\n");
		ret = -EINVAL;
		goto bq25892_aux_fail_2;
	}

	if (is_board_type != BAT_BOARD_UDP) {
		ret = request_irq(di->irq_int, bq25892_aux_interrupt,
			IRQF_TRIGGER_FALLING, "charger_int_irq", di);
		if (ret) {
			hwlog_err("gpio(gpio_int) irq request fail\n");
			di->irq_int = -1;
			goto bq25892_aux_fail_3;
		}

		di->irq_active = 1;
	}

	ret = charge_aux_ops_register(&bq25892_aux_ops);
	if (ret) {
		hwlog_err("bq25892_aux charge ops register fail\n");
		goto bq25892_aux_fail_3;
	}

	ret = bq25892_aux_sysfs_create_group(di);
	if (ret) {
		hwlog_err("sysfs group create failed\n");
		goto bq25892_aux_fail_3;
	}

	power_class = hw_power_get_class();
	if (power_class != NULL) {
		if (charge_dev == NULL)
			charge_dev = device_create(power_class, NULL, 0, NULL,
				"charger");

		ret = sysfs_create_link(&charge_dev->kobj, &di->dev->kobj,
			"bq25892_aux");
		if (ret) {
			hwlog_err("sysfs link create failed\n");
			goto bq25892_aux_fail_4;
		}
	}

	hwlog_info("probe end\n");
	return 0;

bq25892_aux_fail_4:
	bq25892_aux_sysfs_remove_group(di);
bq25892_aux_fail_3:
	free_irq(di->irq_int, di);
bq25892_aux_fail_2:
	gpio_free(di->gpio_int);
bq25892_aux_fail_1:
	gpio_free(di->gpio_cd);
bq25892_aux_fail_0:
	g_bq25892_aux_dev = NULL;
	np = NULL;

	return ret;
}

static int bq25892_aux_remove(struct i2c_client *client)
{
	struct bq25892_aux_device_info *di = i2c_get_clientdata(client);

	hwlog_info("remove begin\n");

	bq25892_aux_sysfs_remove_group(di);

	gpio_set_value(di->gpio_cd, 1);

	if (di->gpio_cd)
		gpio_free(di->gpio_cd);

	if (di->irq_int)
		free_irq(di->irq_int, di);

	if (di->gpio_int)
		gpio_free(di->gpio_int);

	hwlog_info("remove end\n");
	return 0;
}

MODULE_DEVICE_TABLE(i2c, bq25892_aux);
static const struct of_device_id bq25892_aux_of_match[] = {
	{
		.compatible = "huawei,bq25892_charger_aux",
		.data = NULL,
	},
	{},
};

static const struct i2c_device_id bq25892_aux_i2c_id[] = {
	{"bq25892_charger_aux", 0}, {}
};

static struct i2c_driver bq25892_aux_driver = {
	.probe = bq25892_aux_probe,
	.remove = bq25892_aux_remove,
	.id_table = bq25892_aux_i2c_id,
	.driver = {
		.owner = THIS_MODULE,
		.name = "bq25892_charger_aux",
		.of_match_table = of_match_ptr(bq25892_aux_of_match),
	},
};

static int __init bq25892_aux_init(void)
{
	int ret = 0;

	ret = i2c_add_driver(&bq25892_aux_driver);
	if (ret)
		hwlog_err("i2c_add_driver error\n");

	return ret;
}

static void __exit bq25892_aux_exit(void)
{
	i2c_del_driver(&bq25892_aux_driver);
}

rootfs_initcall(bq25892_aux_init);
module_exit(bq25892_aux_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("bq25892_aux charger module driver");
MODULE_AUTHOR("HW Inc");
