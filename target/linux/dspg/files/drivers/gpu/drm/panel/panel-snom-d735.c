// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 John Crispin <john@phrozen.org>
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/media-bus-format.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/spi/spi.h>

#include <drm/drm_connector.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

#define D735_CMD_MAX_LEN	16
#define D735_RESET_MS		20
#define D735_SLEEP_MS		120

#define MIPI_SWRESET		0x01
#define MIPI_SLPIN		0x10
#define MIPI_SLPOUT		0x11
#define MIPI_DISPOFF		0x28
#define MIPI_DISPON		0x29

struct d735_cmd {
	u8 len;
	u8 data[D735_CMD_MAX_LEN];
};

#define D735_CMD(...)							\
	{								\
		.len = sizeof((u8[]){ __VA_ARGS__ }),			\
		.data = { __VA_ARGS__ },				\
	}

struct d735_panel_desc {
	const struct d735_cmd *init;
	unsigned int num_init;
	const struct drm_display_mode *mode;
};

struct d735_panel {
	struct drm_panel panel;
	struct spi_device *spi;
	struct gpio_desc *reset;
	struct gpio_desc *dc;
	const struct d735_panel_desc *desc;
};

static const struct d735_cmd st7789v2_init[] = {
	D735_CMD(0x3a, 0x66),
	D735_CMD(0x36, 0xa0),
	D735_CMD(0xb0, 0x11, 0xf0),
	D735_CMD(0xb1, 0x40, 0x10, 0x12),
	D735_CMD(0xb2, 0x0c, 0x0c, 0x00, 0x33, 0x33),
	D735_CMD(0xb7, 0x35),
	D735_CMD(0xbb, 0x25),
	D735_CMD(0xc0, 0x2c),
	D735_CMD(0xc2, 0x01),
	D735_CMD(0xc3, 0x17),
	D735_CMD(0xc4, 0x20),
	D735_CMD(0xc6, 0x0f),
	D735_CMD(0xca, 0x0f),
	D735_CMD(0xc8, 0x08),
	D735_CMD(0x55, 0x90),
	D735_CMD(0xd0, 0xa4, 0xa1),
	D735_CMD(0xe0, 0xd0, 0x00, 0x02, 0x07, 0x0a, 0x28, 0x32, 0x44, 0x42,
		 0x06, 0x0e, 0x12, 0x14, 0x17),
	D735_CMD(0xe1, 0xd0, 0x00, 0x02, 0x07, 0x0a, 0x28, 0x31, 0x54, 0x47,
		 0x0e, 0x1c, 0x17, 0x1b, 0x1e),
	D735_CMD(0x2b, 0x00, 0x00, 0x00, 0xef),
	D735_CMD(0x2a, 0x00, 0x00, 0x01, 0x3f),
};

static const struct d735_cmd gc9306x_init[] = {
	D735_CMD(0xfe),
	D735_CMD(0xef),
	D735_CMD(0x36, 0xe8),
	D735_CMD(0x3a, 0x66),
	D735_CMD(0xa4, 0x44, 0x44),
	D735_CMD(0xa5, 0x42, 0x42),
	D735_CMD(0xaa, 0x88, 0x88),
	D735_CMD(0xb0, 0x46),
	D735_CMD(0xf6, 0x00, 0x00, 0x02),
	D735_CMD(0xb5, 0x08, 0x0c, 0x00, 0x16),
	D735_CMD(0xe8, 0x11, 0x0b),
	D735_CMD(0xe3, 0x01, 0x10),
	D735_CMD(0xac, 0x00),
	D735_CMD(0xad, 0x33),
	D735_CMD(0xae, 0x2b),
	D735_CMD(0xaf, 0x55),
	D735_CMD(0xff, 0x61),
	D735_CMD(0xa6, 0x2a, 0x2a),
	D735_CMD(0xa7, 0x2b, 0x2b),
	D735_CMD(0xa8, 0x18, 0x18),
	D735_CMD(0xa9, 0x2a, 0x2a),
	D735_CMD(0xf0, 0x02, 0x00, 0x00, 0x00, 0x00, 0x01),
	D735_CMD(0xf1, 0x01, 0x01, 0x00, 0x00, 0x0e, 0x17),
	D735_CMD(0xf2, 0x00, 0x04, 0x1e, 0x06, 0x05, 0x30),
	D735_CMD(0xf3, 0x18, 0x0d, 0x52, 0x04, 0x03, 0x64),
	D735_CMD(0xf4, 0x0e, 0x1b, 0x19, 0x03, 0x05, 0x0f),
	D735_CMD(0xf5, 0x06, 0x12, 0x12, 0x34, 0x33, 0x0f),
	D735_CMD(0x2b, 0x00, 0x00, 0x00, 0xef),
	D735_CMD(0x2a, 0x00, 0x00, 0x01, 0x3f),
};

static const struct d735_cmd gc9307n_init[] = {
	D735_CMD(0xfe),
	D735_CMD(0xef),
	D735_CMD(0x36, 0xe8),
	D735_CMD(0x3a, 0x66),
	D735_CMD(0x21),
	D735_CMD(0x86, 0x98),
	D735_CMD(0x89, 0x03),
	D735_CMD(0x8b, 0x84),
	D735_CMD(0x8d, 0x33),
	D735_CMD(0x8e, 0x0f),
	D735_CMD(0xe8, 0x12, 0x00),
	D735_CMD(0xff, 0x62),
	D735_CMD(0x99, 0x3e),
	D735_CMD(0x9d, 0x4b),
	D735_CMD(0x98, 0x3e),
	D735_CMD(0x9c, 0x4b),
	D735_CMD(0xc3, 0x37),
	D735_CMD(0xc4, 0x34),
	D735_CMD(0xc9, 0x20),
	D735_CMD(0x84, 0x61),
	D735_CMD(0x8a, 0x40),
	D735_CMD(0xf6, 0xc6),
	D735_CMD(0xb0, 0x40),
	D735_CMD(0xb5, 0x02, 0x02, 0x14),
	D735_CMD(0xf0, 0x80, 0x00, 0x00, 0x00, 0x00, 0x33),
	D735_CMD(0xf1, 0x52, 0x97, 0x95, 0x30, 0x35, 0xaf),
	D735_CMD(0xf2, 0x40, 0x16, 0x00, 0x00, 0x00, 0x2b),
	D735_CMD(0xf3, 0x40, 0xb9, 0xda, 0x0d, 0x10, 0xff),
	D735_CMD(0x2b, 0x00, 0x00, 0x00, 0xef),
	D735_CMD(0x2a, 0x00, 0x00, 0x01, 0x3f),
};

static const struct drm_display_mode d735_rgb_mode = {
	.clock = 9778,
	.hdisplay = 320,
	.hsync_start = 320 + 13,
	.hsync_end = 320 + 13 + 10,
	.htotal = 320 + 13 + 10 + 10,
	.vdisplay = 240,
	.vsync_start = 240 + 33,
	.vsync_end = 240 + 33 + 2,
	.vtotal = 240 + 33 + 2 + 2,
	.flags = DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC,
};

static const struct drm_display_mode d735_gc9307n_mode = {
	.clock = 9778,
	.hdisplay = 320,
	.hsync_start = 320 + 2,
	.hsync_end = 320 + 2 + 129,
	.htotal = 320 + 2 + 129 + 2,
	.vdisplay = 240,
	.vsync_start = 240 + 2,
	.vsync_end = 240 + 2 + 60,
	.vtotal = 240 + 2 + 60 + 54,
	.flags = DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC,
};

static const struct d735_panel_desc st7789v2_desc = {
	.init = st7789v2_init,
	.num_init = ARRAY_SIZE(st7789v2_init),
	.mode = &d735_rgb_mode,
};

static const struct d735_panel_desc gc9306x_desc = {
	.init = gc9306x_init,
	.num_init = ARRAY_SIZE(gc9306x_init),
	.mode = &d735_rgb_mode,
};

static const struct d735_panel_desc gc9307n_desc = {
	.init = gc9307n_init,
	.num_init = ARRAY_SIZE(gc9307n_init),
	.mode = &d735_gc9307n_mode,
};

static struct d735_panel *to_d735_panel(struct drm_panel *panel)
{
	return container_of(panel, struct d735_panel, panel);
}

static int d735_panel_send(struct d735_panel *p, const u8 *data,
			   unsigned int len)
{
	int ret;

	gpiod_set_value_cansleep(p->dc, 0);
	ret = spi_write(p->spi, data, 1);
	if (ret || len == 1)
		return ret;

	gpiod_set_value_cansleep(p->dc, 1);

	return spi_write(p->spi, data + 1, len - 1);
}

static int d735_panel_cmd(struct d735_panel *p, u8 cmd)
{
	return d735_panel_send(p, &cmd, 1);
}

static int d735_panel_prepare(struct drm_panel *panel)
{
	struct d735_panel *p = to_d735_panel(panel);
	unsigned int i;
	int ret;

	gpiod_set_value_cansleep(p->reset, 1);
	msleep(D735_RESET_MS);
	gpiod_set_value_cansleep(p->reset, 0);
	msleep(D735_RESET_MS);

	ret = d735_panel_cmd(p, MIPI_SWRESET);
	if (ret)
		return ret;
	msleep(D735_SLEEP_MS);

	ret = d735_panel_cmd(p, MIPI_SLPOUT);
	if (ret)
		return ret;
	msleep(D735_SLEEP_MS);

	for (i = 0; i < p->desc->num_init; i++) {
		ret = d735_panel_send(p, p->desc->init[i].data,
				      p->desc->init[i].len);
		if (ret)
			return ret;
	}

	return 0;
}

static int d735_panel_enable(struct drm_panel *panel)
{
	return d735_panel_cmd(to_d735_panel(panel), MIPI_DISPON);
}

static int d735_panel_disable(struct drm_panel *panel)
{
	return d735_panel_cmd(to_d735_panel(panel), MIPI_DISPOFF);
}

static int d735_panel_unprepare(struct drm_panel *panel)
{
	struct d735_panel *p = to_d735_panel(panel);
	int ret;

	ret = d735_panel_cmd(p, MIPI_SLPIN);
	msleep(D735_SLEEP_MS);

	return ret;
}

static int d735_panel_get_modes(struct drm_panel *panel,
				struct drm_connector *connector)
{
	struct d735_panel *p = to_d735_panel(panel);
	static const u32 bus_format = MEDIA_BUS_FMT_RGB666_1X18;
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, p->desc->mode);
	if (!mode)
		return -ENOMEM;

	drm_mode_set_name(mode);
	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(connector, mode);

	connector->display_info.bpc = 6;
	drm_display_info_set_bus_formats(&connector->display_info,
					 &bus_format, 1);

	return 1;
}

static const struct drm_panel_funcs d735_panel_funcs = {
	.prepare = d735_panel_prepare,
	.enable = d735_panel_enable,
	.disable = d735_panel_disable,
	.unprepare = d735_panel_unprepare,
	.get_modes = d735_panel_get_modes,
};

static int d735_panel_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct d735_panel *p;
	int ret;

	p = devm_drm_panel_alloc(dev, struct d735_panel, panel,
				 &d735_panel_funcs, DRM_MODE_CONNECTOR_DPI);
	if (IS_ERR(p))
		return PTR_ERR(p);

	p->spi = spi;
	p->desc = spi_get_device_match_data(spi);

	p->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(p->reset))
		return dev_err_probe(dev, PTR_ERR(p->reset), "no reset gpio\n");

	p->dc = devm_gpiod_get(dev, "dc", GPIOD_OUT_LOW);
	if (IS_ERR(p->dc))
		return dev_err_probe(dev, PTR_ERR(p->dc), "no dc gpio\n");

	spi->bits_per_word = 8;
	ret = spi_setup(spi);
	if (ret)
		return dev_err_probe(dev, ret, "spi setup failed\n");

	ret = drm_panel_of_backlight(&p->panel);
	if (ret)
		return dev_err_probe(dev, ret, "no backlight\n");

	drm_panel_add(&p->panel);
	spi_set_drvdata(spi, p);

	return 0;
}

static void d735_panel_remove(struct spi_device *spi)
{
	struct d735_panel *p = spi_get_drvdata(spi);

	drm_panel_remove(&p->panel);
}

static const struct of_device_id d735_panel_of_match[] = {
	{ .compatible = "snom,d735-st7789v2", .data = &st7789v2_desc },
	{ .compatible = "snom,d735-gc9306x", .data = &gc9306x_desc },
	{ .compatible = "snom,d735-gc9307n", .data = &gc9307n_desc },
	{ }
};
MODULE_DEVICE_TABLE(of, d735_panel_of_match);

static const struct spi_device_id d735_panel_ids[] = {
	{ "d735-st7789v2", (kernel_ulong_t)&st7789v2_desc },
	{ "d735-gc9306x", (kernel_ulong_t)&gc9306x_desc },
	{ "d735-gc9307n", (kernel_ulong_t)&gc9307n_desc },
	{ }
};
MODULE_DEVICE_TABLE(spi, d735_panel_ids);

static struct spi_driver d735_panel_driver = {
	.probe = d735_panel_probe,
	.remove = d735_panel_remove,
	.id_table = d735_panel_ids,
	.driver = {
		.name = "panel-snom-d735",
		.of_match_table = d735_panel_of_match,
	},
};
module_spi_driver(d735_panel_driver);

MODULE_DESCRIPTION("Snom D735 RGB panel driver");
MODULE_LICENSE("GPL");
