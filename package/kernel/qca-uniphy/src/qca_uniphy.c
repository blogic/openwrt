// SPDX-License-Identifier: GPL-2.0-only
/*
 * Qualcomm IPQ6018 UNIPHY PCS driver
 *
 * Copyright (c) 2025 The OpenWrt Project
 *
 * Standalone PCS driver for the UNIPHY SerDes blocks in IPQ6018 SoCs.
 * Each UNIPHY instance provides up to 5 SGMII channels (PSGMII mode)
 * or a single channel for SGMII/USXGMII. The driver is consumed by the
 * IPQ6018 PPE DSA switch driver via qca_uniphy_pcs_get().
 */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>

#include "qca_uniphy.h"

static struct qca_uniphy_pcs *
pcs_to_uniphy_pcs(struct phylink_pcs *pcs)
{
	return container_of(pcs, struct qca_uniphy_pcs, pcs);
}

static u32 uniphy_read(struct qca_uniphy *uniphy, u32 reg)
{
	return readl(uniphy->base + reg);
}

static void uniphy_write(struct qca_uniphy *uniphy, u32 reg, u32 val)
{
	writel(val, uniphy->base + reg);
}

static unsigned long
qca_uniphy_clk_recalc_rate(struct clk_hw *hw, unsigned long parent_rate)
{
	struct qca_uniphy_clk *uclk =
		container_of(hw, struct qca_uniphy_clk, hw);

	return uclk->uniphy->pll_rate;
}

static int
qca_uniphy_clk_determine_rate(struct clk_hw *hw,
				struct clk_rate_request *req)
{
	if (req->rate <= 125000000)
		req->rate = 125000000;
	else
		req->rate = 312500000;

	return 0;
}

static int
qca_uniphy_clk_set_rate(struct clk_hw *hw, unsigned long rate,
			  unsigned long parent_rate)
{
	struct qca_uniphy_clk *uclk =
		container_of(hw, struct qca_uniphy_clk, hw);

	if (rate != 125000000 && rate != 312500000)
		return -EINVAL;

	uclk->uniphy->pll_rate = rate;

	return 0;
}

static const struct clk_ops qca_uniphy_clk_ops = {
	.recalc_rate = qca_uniphy_clk_recalc_rate,
	.determine_rate = qca_uniphy_clk_determine_rate,
	.set_rate = qca_uniphy_clk_set_rate,
};

static int qca_uniphy_clk_register(struct qca_uniphy *uniphy,
				     struct qca_uniphy_clk *uclk,
				     const char *name)
{
	struct clk_init_data init = {
		.name = name,
		.ops = &qca_uniphy_clk_ops,
	};

	uclk->hw.init = &init;
	uclk->uniphy = uniphy;

	return devm_clk_hw_register(uniphy->dev, &uclk->hw);
}

static int qca_uniphy_psgmii_calibrate(struct qca_uniphy *uniphy)
{
	u32 val;
	int ret, i;

	if (uniphy->psgmii_calibrated)
		return 0;

	for (i = 2; i < uniphy->num_clks; i++)
		clk_disable(uniphy->clks[i].clk);

	reset_control_assert(uniphy->rst_xpcs);
	reset_control_bulk_assert(ARRAY_SIZE(uniphy->rst_ports),
				  uniphy->rst_ports);
	usleep_range(100, 200);

	uniphy_write(uniphy, UNIPHY_MODE_CTRL,
		     UNIPHY_CH0_PSGMII_QSGMII | UNIPHY_SG_AUTONEG);

	reset_control_assert(uniphy->rst_psgmii);
	msleep(100);
	reset_control_deassert(uniphy->rst_psgmii);

	ret = read_poll_timeout(uniphy_read, val,
				val & UNIPHY_CALIBRATION_DONE,
				UNIPHY_CALIBRATION_POLL_US,
				UNIPHY_CALIBRATION_TIMEOUT_US,
				false, uniphy, UNIPHY_OFFSET_CALIB_4);
	if (ret)
		dev_err(uniphy->dev, "PSGMII calibration timeout\n");

	reset_control_bulk_deassert(ARRAY_SIZE(uniphy->rst_ports),
				   uniphy->rst_ports);

	for (i = 2; i < uniphy->num_clks; i++)
		clk_enable(uniphy->clks[i].clk);

	if (ret)
		return ret;

	uniphy->psgmii_calibrated = true;
	return 0;
}

static void qca_uniphy_sgmii_setup(struct qca_uniphy *uniphy,
				    u32 phy_mode, u32 mode_ctrl,
				    unsigned long rate)
{
	int i;

	uniphy_write(uniphy, UNIPHY_MISC2_PHY_MODE, phy_mode);

	uniphy_write(uniphy, UNIPHY_PLL_POWER_ON_AND_RESET,
		     UNIPHY_PLL_RESET_ASSERT);
	msleep(500);
	uniphy_write(uniphy, UNIPHY_PLL_POWER_ON_AND_RESET,
		     UNIPHY_PLL_RESET_DEASSERT);
	msleep(500);

	reset_control_assert(uniphy->rst_xpcs);
	usleep_range(100, 200);

	if (uniphy->calibrated) {
		for (i = 2; i < uniphy->num_clks; i++)
			clk_disable(uniphy->clks[i].clk);
	}

	uniphy_write(uniphy, UNIPHY_MODE_CTRL, mode_ctrl);

	reset_control_assert(uniphy->rst_soft);
	usleep_range(500, 600);
	reset_control_deassert(uniphy->rst_soft);

	uniphy->sgmii_rate = rate;
	uniphy->calibrated = false;
}

static void
qca_uniphy_sgmii_calibration_complete(struct qca_uniphy *uniphy)
{
	int i;

	for (i = 2; i < uniphy->num_clks; i++)
		clk_enable(uniphy->clks[i].clk);

	reset_control_deassert(uniphy->rst_xpcs);
	usleep_range(100, 200);

	clk_set_rate(uniphy->rx_clk_ref, uniphy->sgmii_rate);
	clk_set_rate(uniphy->tx_clk_ref, uniphy->sgmii_rate);

	uniphy->calibrated = true;
}

static bool qca_uniphy_sgmii_calibration_done(struct qca_uniphy *uniphy)
{
	u32 val;

	if (uniphy->calibrated)
		return true;

	val = uniphy_read(uniphy, UNIPHY_OFFSET_CALIB_4);
	if (!(val & UNIPHY_CALIBRATION_DONE))
		return false;

	qca_uniphy_sgmii_calibration_complete(uniphy);
	return true;
}

static unsigned int
qca_uniphy_pcs_inband_caps(struct phylink_pcs *pcs,
			     phy_interface_t interface)
{
	switch (interface) {
	case PHY_INTERFACE_MODE_QSGMII:
	case PHY_INTERFACE_MODE_PSGMII:
	case PHY_INTERFACE_MODE_SGMII:
		return LINK_INBAND_DISABLE | LINK_INBAND_ENABLE;
	case PHY_INTERFACE_MODE_2500BASEX:
		return LINK_INBAND_DISABLE;
	default:
		return 0;
	}
}

static void qca_uniphy_pcs_get_state(struct phylink_pcs *pcs,
				       struct phylink_link_state *state)
{
	struct qca_uniphy_pcs *upcs = pcs_to_uniphy_pcs(pcs);
	u32 val;

	if (!qca_uniphy_sgmii_calibration_done(upcs->uniphy)) {
		state->link = false;
		return;
	}

	val = uniphy_read(upcs->uniphy,
			  UNIPHY_CH_INPUT_OUTPUT_6(upcs->channel));

	state->link = !!(val & UNIPHY_CH_LINK);
	if (!state->link)
		return;

	state->duplex = (val & UNIPHY_CH_DUPLEX) ? DUPLEX_FULL : DUPLEX_HALF;

	switch (FIELD_GET(UNIPHY_CH_SPEED_MODE, val)) {
	case 0:
		state->speed = SPEED_10;
		break;
	case 1:
		state->speed = SPEED_100;
		break;
	case 2:
		state->speed = SPEED_1000;
		break;
	default:
		state->link = false;
		return;
	}

	state->pause = 0;
	if (val & UNIPHY_CH_RX_PAUSE)
		state->pause |= MLO_PAUSE_RX;
	if (val & UNIPHY_CH_TX_PAUSE)
		state->pause |= MLO_PAUSE_TX;

	state->an_complete = state->link;
}

static int qca_uniphy_pcs_config(struct phylink_pcs *pcs,
				   unsigned int neg_mode,
				   phy_interface_t interface,
				   const unsigned long *advertising,
				   bool permit_pause_to_mac)
{
	struct qca_uniphy_pcs *upcs = pcs_to_uniphy_pcs(pcs);
	struct qca_uniphy *uniphy = upcs->uniphy;

	switch (interface) {
	case PHY_INTERFACE_MODE_PSGMII:
		return qca_uniphy_psgmii_calibrate(uniphy);
	case PHY_INTERFACE_MODE_SGMII:
	case PHY_INTERFACE_MODE_QSGMII:
	case PHY_INTERFACE_MODE_2500BASEX:
		if (interface == uniphy->current_mode)
			return 0;

		if (interface == PHY_INTERFACE_MODE_SGMII)
			qca_uniphy_sgmii_setup(uniphy,
				UNIPHY_MISC2_SGMII,
				UNIPHY_SG_MODE | UNIPHY_SG_AUTONEG,
				125000000);
		else if (interface == PHY_INTERFACE_MODE_QSGMII)
			qca_uniphy_sgmii_setup(uniphy,
				UNIPHY_MISC2_SGMII,
				UNIPHY_CH0_QSGMII_SGMII | UNIPHY_SG_AUTONEG,
				125000000);
		else if (interface == PHY_INTERFACE_MODE_2500BASEX)
			qca_uniphy_sgmii_setup(uniphy,
				UNIPHY_MISC2_SGMIIPLUS,
				UNIPHY_SGPLUS_MODE | UNIPHY_SG_AUTONEG,
				312500000);

		uniphy->current_mode = interface;
		return 0;
	default:
		return 0;
	}
}

static void qca_uniphy_pcs_link_up(struct phylink_pcs *pcs,
				     unsigned int neg_mode,
				     phy_interface_t interface,
				     int speed, int duplex)
{
	struct qca_uniphy_pcs *upcs = pcs_to_uniphy_pcs(pcs);
	struct qca_uniphy *uniphy = upcs->uniphy;
	u32 val;
	int ret;

	if (!uniphy->calibrated) {
		ret = read_poll_timeout(uniphy_read, val,
					val & UNIPHY_CALIBRATION_DONE,
					UNIPHY_CALIBRATION_POLL_US,
					UNIPHY_CALIBRATION_TIMEOUT_US,
					false, uniphy, UNIPHY_OFFSET_CALIB_4);
		if (ret) {
			dev_err(uniphy->dev, "SGMII calibration timeout\n");
			return;
		}

		qca_uniphy_sgmii_calibration_complete(uniphy);
	}

	val = uniphy_read(uniphy, UNIPHY_CH_INPUT_OUTPUT_4(upcs->channel));
	uniphy_write(uniphy, UNIPHY_CH_INPUT_OUTPUT_4(upcs->channel),
		     val & ~UNIPHY_CH_ADP_SW_RSTN);
	usleep_range(1000, 2000);
	uniphy_write(uniphy, UNIPHY_CH_INPUT_OUTPUT_4(upcs->channel), val);
}

static const struct phylink_pcs_ops qca_uniphy_pcs_ops = {
	.pcs_inband_caps = qca_uniphy_pcs_inband_caps,
	.pcs_get_state = qca_uniphy_pcs_get_state,
	.pcs_config = qca_uniphy_pcs_config,
	.pcs_link_up = qca_uniphy_pcs_link_up,
};

static const struct of_device_id qca_uniphy_of_match[] = {
	{ .compatible = "qualcomm,ipq6018-uniphy" },
	{ .compatible = "qualcomm,ipq8074-uniphy" },
	{ },
};
MODULE_DEVICE_TABLE(of, qca_uniphy_of_match);

struct phylink_pcs *qca_uniphy_pcs_get(struct device *dev,
					 struct device_node *np,
					 int channel)
{
	struct platform_device *pdev;
	struct qca_uniphy *uniphy;

	if (!np)
		return NULL;

	if (!of_device_is_available(np))
		return ERR_PTR(-ENODEV);

	if (!of_match_node(qca_uniphy_of_match, np))
		return ERR_PTR(-EINVAL);

	pdev = of_find_device_by_node(np);
	if (!pdev || !platform_get_drvdata(pdev)) {
		if (pdev)
			put_device(&pdev->dev);
		return ERR_PTR(-EPROBE_DEFER);
	}

	uniphy = platform_get_drvdata(pdev);

	if (channel < 0 || channel >= QCA_UNIPHY_CHANNELS) {
		put_device(&pdev->dev);
		return ERR_PTR(-EINVAL);
	}

	device_link_add(dev, uniphy->dev, DL_FLAG_AUTOREMOVE_CONSUMER);

	return &uniphy->port_pcs[channel].pcs;
}
EXPORT_SYMBOL_GPL(qca_uniphy_pcs_get);

void qca_uniphy_pcs_put(struct phylink_pcs *pcs)
{
	struct qca_uniphy *uniphy;
	struct qca_uniphy_pcs *upcs;

	if (!pcs)
		return;

	upcs = pcs_to_uniphy_pcs(pcs);
	uniphy = upcs->uniphy;

	put_device(uniphy->dev);
}
EXPORT_SYMBOL_GPL(qca_uniphy_pcs_put);

static void qca_uniphy_clk_disable_unprepare(void *data)
{
	struct qca_uniphy *uniphy = data;

	clk_bulk_disable_unprepare(uniphy->num_clks, uniphy->clks);
}

static int qca_uniphy_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct clk_bulk_data *clks;
	struct qca_uniphy *uniphy;
	struct resource *res;
	char name[32];
	int ret, i, index, num_clks;

	uniphy = devm_kzalloc(dev, sizeof(*uniphy), GFP_KERNEL);
	if (!uniphy)
		return -ENOMEM;

	uniphy->dev = dev;

	uniphy->base = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(uniphy->base))
		return PTR_ERR(uniphy->base);

	num_clks = devm_clk_bulk_get_all(dev, &clks);
	if (num_clks < 0)
		return dev_err_probe(dev, num_clks, "failed to get clocks\n");

	ret = clk_bulk_prepare_enable(num_clks, clks);
	if (ret)
		return dev_err_probe(dev, ret, "failed to enable clocks\n");

	uniphy->clks = clks;
	uniphy->num_clks = num_clks;

	ret = devm_add_action_or_reset(dev, qca_uniphy_clk_disable_unprepare,
				       uniphy);
	if (ret)
		return ret;

	uniphy->rst_soft = devm_reset_control_get_exclusive(dev, "soft");
	if (IS_ERR(uniphy->rst_soft))
		return dev_err_probe(dev, PTR_ERR(uniphy->rst_soft),
				     "failed to get soft reset\n");

	uniphy->rst_xpcs = devm_reset_control_get_exclusive(dev, "xpcs");
	if (IS_ERR(uniphy->rst_xpcs))
		return dev_err_probe(dev, PTR_ERR(uniphy->rst_xpcs),
				     "failed to get xpcs reset\n");

	if (of_property_read_bool(dev->of_node, "qcom,psgmii")) {
		uniphy->rst_psgmii = devm_reset_control_get_exclusive(dev, "psgmii");
		if (IS_ERR(uniphy->rst_psgmii))
			return dev_err_probe(dev, PTR_ERR(uniphy->rst_psgmii),
					     "failed to get psgmii reset\n");

		uniphy->rst_ports[0].id = "port1";
		uniphy->rst_ports[1].id = "port2";
		uniphy->rst_ports[2].id = "port3";
		ret = devm_reset_control_bulk_get_exclusive(dev,
					ARRAY_SIZE(uniphy->rst_ports),
					uniphy->rst_ports);
		if (ret)
			return dev_err_probe(dev, ret,
					     "failed to get port resets\n");

	}

	index = (res->start - UNIPHY0_BASE_ADDR) >> 16;
	uniphy->pll_rate = 125000000;

	snprintf(name, sizeof(name), "uniphy%d_gcc_rx_clk", index);
	ret = qca_uniphy_clk_register(uniphy, &uniphy->rx_clk, name);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to register RX clock\n");

	uniphy->rx_clk_ref = devm_clk_hw_get_clk(dev, &uniphy->rx_clk.hw,
						   "uniphy_rx");
	if (IS_ERR(uniphy->rx_clk_ref))
		return dev_err_probe(dev, PTR_ERR(uniphy->rx_clk_ref),
				     "failed to get RX clock ref\n");

	snprintf(name, sizeof(name), "uniphy%d_gcc_tx_clk", index);
	ret = qca_uniphy_clk_register(uniphy, &uniphy->tx_clk, name);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to register TX clock\n");

	uniphy->tx_clk_ref = devm_clk_hw_get_clk(dev, &uniphy->tx_clk.hw,
						   "uniphy_tx");
	if (IS_ERR(uniphy->tx_clk_ref))
		return dev_err_probe(dev, PTR_ERR(uniphy->tx_clk_ref),
				     "failed to get TX clock ref\n");

	uniphy->calibrated = true;

	for (i = 0; i < QCA_UNIPHY_CHANNELS; i++) {
		uniphy->port_pcs[i].pcs.ops = &qca_uniphy_pcs_ops;
		uniphy->port_pcs[i].pcs.neg_mode = true;
		uniphy->port_pcs[i].pcs.poll = true;
		uniphy->port_pcs[i].uniphy = uniphy;
		uniphy->port_pcs[i].channel = i;
	}

	platform_set_drvdata(pdev, uniphy);

	return 0;
}

static struct platform_driver qca_uniphy_driver = {
	.driver = {
		.name			= "qca-uniphy",
		.suppress_bind_attrs	= true,
		.of_match_table		= qca_uniphy_of_match,
	},
	.probe = qca_uniphy_probe,
};
module_platform_driver(qca_uniphy_driver);

MODULE_DESCRIPTION("Qualcomm IPQ6018 UNIPHY PCS driver");
MODULE_LICENSE("GPL");
