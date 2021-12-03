// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2021, Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/debugfs.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>
#include <linux/types.h>
#include <linux/gpio/consumer.h>
#include <linux/regulator/consumer.h>
#include <linux/usb/repeater.h>

#define EUSB2_3P0_VOL_MIN			3075000 /* uV */
#define EUSB2_3P0_VOL_MAX			3300000 /* uV */
#define EUSB2_3P0_HPM_LOAD			3500	/* uA */

#define EUSB2_1P2_VOL_MIN			1200000 /* uV */
#define EUSB2_1P2_VOL_MAX			1320000 /* uV */
#define EUSB2_1P2_HPM_LOAD			18000	/* uA */

/* eUSB2 status registers */
#define EUSB2_RPTR_STATUS		0x08
#define	RPTR_OK				BIT(7)

#define EUSB2_RPTR_INFRA_STATUS		0x09
#define EUSB2_RPTR_DROPOUT_REASON	0x0A

/* eUSB2 control registers */
#define EUSB2_MODE_CTL			0x40
#define HOST_SEL			BIT(1)
#define PERIPH_SEL			BIT(0)

#define EUSB2_USB_HIZ_CTL		0x41
#define HIZ_EN				BIT(7)

#define EUSB2_AUTO_RESUME_EN		0x42
#define AUTO_RESUME_EN			BIT(7)

#define EUSB2_EN_CTL1			0x46
#define EUSB2_RPTR_EN			BIT(7)

/* eUSB2 tuning parameters registers */
#define EUSB2_TUNE_USB2_CROSSOVER	0x50
#define TUNE_USB2_CROSSOVER_MASK	0x07

#define EUSB2_TUNE_IUSB2		0x51
#define TUNE_IUSB2_MASK			0x0F

#define EUSB2_TUNE_RES_FSDIF		0x52
#define TUNE_RES_FSIDF_MASK		0x07

#define EUSB2_TUNE_HSDISC		0x53
#define TUNE_HSDISC_MASK		0x07

#define EUSB2_TUNE_SQUELCH_U		0x54
#define TUNE_SQELCH_U_MASK		0x07

#define EUSB2_TUNE_USB2_SLEW		0x55
#define TUNE_USB2_SLEW_MASK		0x03

#define EUSB2_TUNE_USB2_EQU		0x56
#define TUNE_USB2_EQU_MASK		0x03

#define EUSB2_TUNE_USB2_PREEM		0x57
#define TUNE_USB2_PREEM_MASK		0x07

#define EUSB2_TUNE_USB2_HS_COMP_CUR	0x58
#define TUNE_USB2_HS_COMP_CURRENT_MASK	0x03

#define EUSB2_TUNE_EUSB_SLEW		0x59
#define TUNE_EUSB_SLEW_MASK		0x03

#define EUSB2_TUNE_EUSB_EQU		0x5A
#define TUNE_EUSB_EQU_MASK		0x03

#define EUSB2_TUNE_EUSB_HS_COMP_CUR	0x5B
#define TUNE_EUSB_HS_COMP_CUR_MASK	0x03

struct eusb2_repeater {
	struct usb_repeater	ur;
	struct regmap		*regmap;
	u16			reg_base;
	struct regulator	*vdd12;
	struct regulator	*vdd3;
	bool			power_enabled;

	struct gpio_desc	*reset_gpiod;
	int			reset_gpio_irq;

	struct dentry		*root;
	u8			usb2_crossover;
	u8			iusb2;
	u8			res_fsdif;
	u8			hsdisc;
	u8			squelch_u;
	u8			usb2_slew;
	u8			usb2_equ;
	u8			usb2_preem;
	u8			hs_comp_current;
	u8			eusb_slew;
	u8			eusb_equ;
	u8			eusb_hs_comp_current;

	u8			*param_override_seq;
	u8			param_override_seq_cnt;
};

/* Perform one or more register read */
static int eusb2_repeater_reg_read(struct eusb2_repeater *er,
			u8 *read_val, u16 reg_offset, size_t reg_count)
{
	int ret;

	ret = regmap_bulk_read(er->regmap, er->reg_base + reg_offset,
						read_val, reg_count);
	if (ret)
		dev_err(er->ur.dev, "read failed: addr=0x%04x, ret=%d\n",
			er->reg_base + reg_offset, ret);
	return ret;
}

/* Perform multiple registers write using given block of data */
static int eusb2_repeater_bulk_reg_write(struct eusb2_repeater *er,
			u16 reg_offset, const void *write_val, size_t reg_count)
{
	int ret;

	ret = regmap_bulk_write(er->regmap, er->reg_base + reg_offset,
						write_val, reg_count);
	if (ret)
		dev_err(er->ur.dev, "bulk write failed: addr=0x%04x, ret=%d\n",
				er->reg_base + reg_offset, ret);
	return ret;
}

/* Perform specific register write using given byte value */
static inline int eusb2_repeater_reg_write(struct eusb2_repeater *er,
					u16 reg_offset, u8 write_val)
{
	return eusb2_repeater_bulk_reg_write(er, reg_offset, &write_val, 1);
}

/* Update specified register using given bit mask and value */
static int eusb2_repeater_masked_write(struct eusb2_repeater *er,
					u16 reg_offset, u8 mask, u8 write_val)
{
	int ret;

	ret = regmap_update_bits(er->regmap, er->reg_base + reg_offset,
						mask, write_val);
	if (ret)
		dev_err(er->ur.dev, "write failed: addr=0x%04x, ret=%d\n",
				er->reg_base + reg_offset, ret);
	return ret;
}

static void eusb2_repeater_update_seq(struct eusb2_repeater *er,
						u8 *seq, u8 cnt)
{
	int i;

	dev_dbg(er->ur.dev, "param override seq count:%d\n", cnt);
	for (i = 0; i < cnt; i = i+2) {
		dev_dbg(er->ur.dev, "write 0x%02x to 0x%02x\n",
						seq[i], seq[i+1]);
		eusb2_repeater_reg_write(er, seq[i+1], seq[i]);
	}
}

static void eusb2_repeater_create_debugfs(struct eusb2_repeater *er)
{
	er->root = debugfs_create_dir(dev_name(er->ur.dev), NULL);
	debugfs_create_x8("usb2_crossover", 0644, er->root,
					&er->usb2_crossover);
	debugfs_create_x8("iusb2", 0644, er->root, &er->iusb2);
	debugfs_create_x8("res_fsdif", 0644, er->root, &er->res_fsdif);
	debugfs_create_x8("hsdisc", 0644, er->root, &er->hsdisc);
	debugfs_create_x8("squelch_u", 0644, er->root, &er->squelch_u);
	debugfs_create_x8("usb2_slew", 0644, er->root, &er->usb2_slew);
	debugfs_create_x8("usb2_equ", 0644, er->root, &er->usb2_equ);
	debugfs_create_x8("usb2_preem", 0644, er->root, &er->usb2_preem);
	debugfs_create_x8("hs_comp_current", 0644, er->root,
					&er->hs_comp_current);
	debugfs_create_x8("eusb_slew", 0644, er->root, &er->eusb_slew);
	debugfs_create_x8("eusb_equ", 0644, er->root, &er->eusb_equ);
	debugfs_create_x8("eusb_hs_comp_current", 0644, er->root,
					&er->eusb_hs_comp_current);
}

static int eusb2_repeater_power(struct eusb2_repeater *er, bool on)
{
	int ret = 0;

	dev_dbg(er->ur.dev, "%s turn %s regulators. power_enabled:%d\n",
			__func__, on ? "on" : "off", er->power_enabled);

	if (er->power_enabled == on) {
		dev_dbg(er->ur.dev, "regulators' regulators are already ON.\n");
		return 0;
	}

	if (!on)
		goto disable_vdd3;

	ret = regulator_set_load(er->vdd12, EUSB2_1P2_HPM_LOAD);
	if (ret < 0) {
		dev_err(er->ur.dev, "Unable to set HPM of vdd12:%d\n", ret);
		goto err_vdd12;
	}

	ret = regulator_set_voltage(er->vdd12, EUSB2_1P2_VOL_MIN,
						EUSB2_1P2_VOL_MAX);
	if (ret) {
		dev_err(er->ur.dev,
				"Unable to set voltage for vdd12:%d\n", ret);
		goto put_vdd12_lpm;
	}

	ret = regulator_enable(er->vdd12);
	if (ret) {
		dev_err(er->ur.dev, "Unable to enable vdd12:%d\n", ret);
		goto unset_vdd12;
	}

	ret = regulator_set_load(er->vdd3, EUSB2_3P0_HPM_LOAD);
	if (ret < 0) {
		dev_err(er->ur.dev, "Unable to set HPM of vdd3:%d\n", ret);
		goto disable_vdd12;
	}

	ret = regulator_set_voltage(er->vdd3, EUSB2_3P0_VOL_MIN,
						EUSB2_3P0_VOL_MAX);
	if (ret) {
		dev_err(er->ur.dev,
				"Unable to set voltage for vdd3:%d\n", ret);
		goto put_vdd3_lpm;
	}

	ret = regulator_enable(er->vdd3);
	if (ret) {
		dev_err(er->ur.dev, "Unable to enable vdd3:%d\n", ret);
		goto unset_vdd3;
	}

	er->power_enabled = true;
	dev_dbg(er->ur.dev, "eUSB2 repeater egulators are turned ON.\n");
	return ret;

disable_vdd3:
	ret = regulator_disable(er->vdd3);
	if (ret)
		dev_err(er->ur.dev, "Unable to disable vdd3:%d\n", ret);

unset_vdd3:
	ret = regulator_set_voltage(er->vdd3, 0, EUSB2_3P0_VOL_MAX);
	if (ret)
		dev_err(er->ur.dev,
			"Unable to set (0) voltage for vdd3:%d\n", ret);

put_vdd3_lpm:
	ret = regulator_set_load(er->vdd3, 0);
	if (ret < 0)
		dev_err(er->ur.dev, "Unable to set (0) HPM of vdd3\n");

disable_vdd12:
	ret = regulator_disable(er->vdd12);
	if (ret)
		dev_err(er->ur.dev, "Unable to disable vdd12:%d\n", ret);

unset_vdd12:
	ret = regulator_set_voltage(er->vdd12, 0, EUSB2_1P2_VOL_MAX);
	if (ret)
		dev_err(er->ur.dev,
			"Unable to set (0) voltage for vdd12:%d\n", ret);

put_vdd12_lpm:
	ret = regulator_set_load(er->vdd12, 0);
	if (ret < 0)
		dev_err(er->ur.dev, "Unable to set LPM of vdd12\n");

err_vdd12:
	/* case handling when regulator turning on failed */
	if (!er->power_enabled)
		return -EINVAL;

	er->power_enabled = false;
	dev_dbg(er->ur.dev, "eUSB2 repeater's regulators are turned OFF.\n");
	return ret;
}

static int eusb2_repeater_init(struct usb_repeater *ur)
{
	u8 status;
	struct eusb2_repeater *er =
			container_of(ur, struct eusb2_repeater, ur);

	/* override init sequence using devicetree based values */
	eusb2_repeater_update_seq(er, er->param_override_seq,
			er->param_override_seq_cnt);

	/* override tune params using debugfs based values */
	if (er->usb2_crossover && er->usb2_crossover <= 0x7)
		eusb2_repeater_masked_write(er, EUSB2_TUNE_USB2_CROSSOVER,
			TUNE_USB2_CROSSOVER_MASK, er->usb2_crossover);

	if (er->iusb2 && er->iusb2 <= 0xf)
		eusb2_repeater_masked_write(er, EUSB2_TUNE_IUSB2,
			TUNE_IUSB2_MASK, er->iusb2);

	if (er->res_fsdif && er->res_fsdif <= 0x7)
		eusb2_repeater_masked_write(er, EUSB2_TUNE_RES_FSDIF,
			TUNE_RES_FSIDF_MASK, er->res_fsdif);

	if (er->hsdisc && er->hsdisc <= 0x7)
		eusb2_repeater_masked_write(er, EUSB2_TUNE_HSDISC,
			TUNE_HSDISC_MASK, er->hsdisc);

	if (er->squelch_u && er->squelch_u <= 0x7)
		eusb2_repeater_masked_write(er, EUSB2_TUNE_SQUELCH_U,
			TUNE_SQELCH_U_MASK, er->squelch_u);

	if (er->usb2_slew && er->usb2_slew <= 0x3)
		eusb2_repeater_masked_write(er, EUSB2_TUNE_USB2_SLEW,
			TUNE_USB2_SLEW_MASK, er->usb2_slew);

	if (er->usb2_equ && er->usb2_equ <= 0x3)
		eusb2_repeater_masked_write(er, EUSB2_TUNE_USB2_EQU,
			TUNE_USB2_EQU_MASK, er->usb2_equ);

	if (er->usb2_preem && er->usb2_preem <= 0x7)
		eusb2_repeater_masked_write(er, EUSB2_TUNE_USB2_PREEM,
			TUNE_USB2_PREEM_MASK, er->usb2_preem);

	if (er->hs_comp_current && er->hs_comp_current <= 0x3)
		eusb2_repeater_masked_write(er, EUSB2_TUNE_USB2_HS_COMP_CUR,
			TUNE_USB2_HS_COMP_CURRENT_MASK, er->hs_comp_current);

	if (er->eusb_slew && er->eusb_slew <= 0x3)
		eusb2_repeater_masked_write(er, EUSB2_TUNE_EUSB_SLEW,
			TUNE_EUSB_SLEW_MASK, er->eusb_slew);

	if (er->eusb_equ && er->eusb_equ <= 0x3)
		eusb2_repeater_masked_write(er, EUSB2_TUNE_EUSB_EQU,
			TUNE_EUSB_EQU_MASK, er->eusb_equ);

	if (er->eusb_hs_comp_current && er->eusb_hs_comp_current <= 0x3)
		eusb2_repeater_masked_write(er, EUSB2_TUNE_EUSB_HS_COMP_CUR,
			TUNE_EUSB_HS_COMP_CUR_MASK, er->eusb_hs_comp_current);

	/* read eUSB2 repeater status */
	eusb2_repeater_reg_read(er, &status, EUSB2_RPTR_STATUS, 1);
	dev_info(er->ur.dev, "eUSB2 repeater status:0x%02x\n", status);

	return 0;
}

static int eusb2_repeater_reset(struct usb_repeater *ur,
				bool bring_out_of_reset)
{
	struct eusb2_repeater *er =
			container_of(ur, struct eusb2_repeater, ur);

	dev_dbg(ur->dev, "reset gpio: pulling %s\n",
			bring_out_of_reset ? "high" : "low");
	gpiod_set_value_cansleep(er->reset_gpiod, bring_out_of_reset);
	return 0;
}

static int eusb2_repeater_powerup(struct usb_repeater *ur)
{
	struct eusb2_repeater *er =
			container_of(ur, struct eusb2_repeater, ur);

	return eusb2_repeater_power(er, true);
}

static int eusb2_repeater_powerdown(struct usb_repeater *ur)
{
	struct eusb2_repeater *er =
			container_of(ur, struct eusb2_repeater, ur);

	return eusb2_repeater_power(er, false);
}

static irqreturn_t eusb2_reset_gpio_irq_handler(int irq, void *dev_id)
{
	struct eusb2_repeater *er = dev_id;

	dev_dbg(er->ur.dev, "reset gpio interrupt handled\n");
	return IRQ_HANDLED;
}

static int eusb2_repeater_probe(struct platform_device *pdev)
{
	struct eusb2_repeater *er;
	struct device *dev = &pdev->dev;
	int ret = 0;

	er = devm_kzalloc(dev, sizeof(*er), GFP_KERNEL);
	if (!er) {
		ret = -ENOMEM;
		goto err_probe;
	}

	er->regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!er->regmap) {
		dev_err(&pdev->dev, "failed to get parent's regmap\n");
		ret = -EINVAL;
		goto err_probe;
	}

	ret = of_property_read_u16(pdev->dev.of_node, "reg", &er->reg_base);
	if (ret < 0) {
		dev_err(&pdev->dev, "failed to get reg base address:%d\n", ret);
		goto err_probe;
	}

	er->vdd3 = devm_regulator_get(dev, "vdd3");
	if (IS_ERR(er->vdd3)) {
		dev_err(dev, "unable to get vdd3 supply\n");
		ret = PTR_ERR(er->vdd3);
		goto err_probe;
	}

	er->vdd12 = devm_regulator_get(dev, "vdd12");
	if (IS_ERR(er->vdd12)) {
		dev_err(dev, "unable to get vdd12 supply\n");
		ret = PTR_ERR(er->vdd12);
		goto err_probe;
	}

	er->reset_gpiod = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(er->reset_gpiod)) {
		ret = PTR_ERR(er->reset_gpiod);
		goto err_probe;
	}

	er->reset_gpio_irq = gpiod_to_irq(er->reset_gpiod);
	if (er->reset_gpio_irq < 0) {
		dev_err(dev, "failed to get reset gpio IRQ\n");
		goto err_probe;
	}

	ret = devm_request_irq(dev, er->reset_gpio_irq,
			eusb2_reset_gpio_irq_handler, IRQF_TRIGGER_RISING,
			pdev->name, er);
	if (ret < 0) {
		dev_err(dev, "failed to request reset gpio irq\n");
		goto err_probe;
	}

	er->param_override_seq_cnt = of_property_count_elems_of_size(
				dev->of_node, "qcom,param-override-seq",
				sizeof(*er->param_override_seq));
	if (er->param_override_seq_cnt % 2) {
		dev_err(dev, "invalid param_override_seq_len\n");
		ret = -EINVAL;
		goto err_probe;
	}

	if (er->param_override_seq_cnt > 0) {
		er->param_override_seq = devm_kcalloc(dev,
				er->param_override_seq_cnt,
				sizeof(*er->param_override_seq), GFP_KERNEL);
		if (!er->param_override_seq) {
			ret = -ENOMEM;
			goto err_probe;
		}

		ret = of_property_read_u8_array(dev->of_node,
				"qcom,param-override-seq",
				er->param_override_seq,
				er->param_override_seq_cnt);
		if (ret) {
			dev_err(dev, "qcom,param-override-seq read failed %d\n",
									ret);
			goto err_probe;
		}
	}

	er->ur.dev = dev;
	platform_set_drvdata(pdev, er);

	er->ur.init		= eusb2_repeater_init;
	er->ur.reset		= eusb2_repeater_reset;
	er->ur.powerup		= eusb2_repeater_powerup;
	er->ur.powerdown	= eusb2_repeater_powerdown;

	ret = usb_add_repeater_dev(&er->ur);
	if (ret)
		goto err_probe;

	eusb2_repeater_create_debugfs(er);
	return 0;

err_probe:
	return ret;
}

static int eusb2_repeater_remove(struct platform_device *pdev)
{
	struct eusb2_repeater *er = platform_get_drvdata(pdev);

	if (!er)
		return 0;

	debugfs_remove_recursive(er->root);
	usb_remove_repeater_dev(&er->ur);
	eusb2_repeater_power(er, false);
	return 0;
}

static const struct of_device_id eusb2_repeater_id_table[] = {
	{
		.compatible = "qcom,pmic-eusb2-repeater",
	},
	{ },
};
MODULE_DEVICE_TABLE(of, eusb2_repeater_id_table);

static struct platform_driver eusb2_repeater_driver = {
	.probe		= eusb2_repeater_probe,
	.remove		= eusb2_repeater_remove,
	.driver = {
		.name	= "eusb2-repeater",
		.of_match_table = of_match_ptr(eusb2_repeater_id_table),
	},
};

module_platform_driver(eusb2_repeater_driver);
MODULE_DESCRIPTION("QTI PMIC eUSB2 repeater driver");
MODULE_LICENSE("GPL v2");
