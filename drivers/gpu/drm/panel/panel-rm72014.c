// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) STMicroelectronics SA 2017
 *
 * Authors: Philippe Cornu <philippe.cornu@st.com>
 *          Yannick Fertre <yannick.fertre@st.com>
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

struct rm72014 {
	struct device *dev;
	struct drm_panel panel;
	struct gpio_desc *reset_gpio;
	struct regulator *supply;
};

static const struct drm_display_mode default_mode = {
	.clock = 80000,
	.hdisplay = 800,
	.hsync_start = 800 + 210,
	.hsync_end = 800 + 210 + 18,
	.htotal = 800 + 210 + 18 + 18,
	.vdisplay = 1280,
	.vsync_start = 1280 + 8,
	.vsync_end = 1280 + 8 + 2,
	.vtotal = 1280 + 8 + 2 + 4,
	.flags = 0,
	.width_mm = 108,
	.height_mm = 172,
};

static inline struct rm72014 *panel_to_rm72014(struct drm_panel *panel)
{
	return container_of(panel, struct rm72014, panel);
}

static void rm72014_dcs_write_buf(struct rm72014 *ctx, const void *data,
				  size_t len)
{
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	int err;

	err = mipi_dsi_dcs_write_buffer(dsi, data, len);
	if (err < 0)
		dev_err_ratelimited(ctx->dev, "MIPI DSI DCS write buffer failed: %d\n", err);
}

#define dcs_write_seq(ctx, seq...)				\
({								\
	static const u8 d[] = { seq };				\
								\
	rm72014_dcs_write_buf(ctx, d, ARRAY_SIZE(d));		\
})

static void rm72014_init_sequence(struct rm72014 *ctx)
{
	dcs_write_seq(ctx, 0x53, 0x24);
	dcs_write_seq(ctx, 0xf0, 0x5a, 0x5a);
	msleep(30);
	dcs_write_seq(ctx, 0x11);
	msleep(120);
	dcs_write_seq(ctx, 0x29);
	msleep(30);
	dcs_write_seq(ctx, 0xc3, 0x40, 0x00, 0x28);
	dcs_write_seq(ctx, 0x50, 0x77);
	dcs_write_seq(ctx, 0xe1, 0x66);
	dcs_write_seq(ctx, 0xdc, 0x67);
	dcs_write_seq(ctx, 0xd3, 0xc8);
	dcs_write_seq(ctx, 0x50, 0x00);
	dcs_write_seq(ctx, 0xf0, 0x5a);
	dcs_write_seq(ctx, 0xf5, 0x80);
	msleep(120);
}

static int rm72014_unprepare(struct drm_panel *panel)
{
	struct rm72014 *ctx = panel_to_rm72014(panel);

	if (ctx->reset_gpio) {
		gpiod_set_value_cansleep(ctx->reset_gpio, 1);
		msleep(20);
	}

	regulator_disable(ctx->supply);

	return 0;
}

static int rm72014_prepare(struct drm_panel *panel)
{
	struct rm72014 *ctx = panel_to_rm72014(panel);
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	int ret;

	ret = regulator_enable(ctx->supply);
	if (ret < 0) {
		dev_err(ctx->dev, "failed to enable supply: %d\n", ret);
		return ret;
	}

	if (ctx->reset_gpio) {
		gpiod_set_value_cansleep(ctx->reset_gpio, 1);
		msleep(20);
		gpiod_set_value_cansleep(ctx->reset_gpio, 0);
		msleep(100);
	}

	rm72014_init_sequence(ctx);

	ret = mipi_dsi_dcs_exit_sleep_mode(dsi);
	if (ret)
		return ret;

	msleep(125);

	ret = mipi_dsi_dcs_set_display_on(dsi);
	if (ret)
		return ret;

	msleep(20);

	return 0;
}

static int rm72014_get_modes(struct drm_panel *panel,
			     struct drm_connector *connector)
{
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, &default_mode);
	if (!mode) {
		dev_err(panel->dev, "failed to add mode %ux%u@%u\n",
			default_mode.hdisplay, default_mode.vdisplay,
			drm_mode_vrefresh(&default_mode));
		return -ENOMEM;
	}

	drm_mode_set_name(mode);

	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(connector, mode);

	connector->display_info.width_mm = mode->width_mm;
	connector->display_info.height_mm = mode->height_mm;

	return 1;
}

static const struct drm_panel_funcs rm72014_drm_funcs = {
	.unprepare = rm72014_unprepare,
	.prepare = rm72014_prepare,
	.get_modes = rm72014_get_modes,
};

static int rm72014_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct rm72014 *ctx;
	int ret;

	ctx = devm_drm_panel_alloc(dev, struct rm72014, panel,
				   &rm72014_drm_funcs,
				   DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	ctx->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->reset_gpio)) {
		ret = PTR_ERR(ctx->reset_gpio);
		dev_err(dev, "cannot get reset GPIO: %d\n", ret);
		return ret;
	}

	ctx->supply = devm_regulator_get(dev, "power");
	if (IS_ERR(ctx->supply)) {
		ret = PTR_ERR(ctx->supply);
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "cannot get regulator: %d\n", ret);
		return ret;
	}

	mipi_dsi_set_drvdata(dsi, ctx);

	ctx->dev = dev;

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST;

	ret = drm_panel_of_backlight(&ctx->panel);
	if (ret)
		return ret;

	drm_panel_add(&ctx->panel);

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		dev_err(dev, "mipi_dsi_attach() failed: %d\n", ret);
		drm_panel_remove(&ctx->panel);
		return ret;
	}

	return 0;
}

static void rm72014_remove(struct mipi_dsi_device *dsi)
{
	struct rm72014 *ctx = mipi_dsi_get_drvdata(dsi);

	mipi_dsi_detach(dsi);
	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id rm72014_of_match[] = {
	{ .compatible = "unknown,rm72014" },
	{ }
};
MODULE_DEVICE_TABLE(of, raydium_rm72014_of_match);

static struct mipi_dsi_driver raydium_rm72014_driver = {
	.probe = rm72014_probe,
	.remove = rm72014_remove,
	.driver = {
		.name = "panel-rm72014",
		.of_match_table = rm72014_of_match,
	},
};
module_mipi_dsi_driver(raydium_rm72014_driver);

MODULE_AUTHOR("Philippe Cornu <philippe.cornu@st.com>");
MODULE_AUTHOR("Yannick Fertre <yannick.fertre@st.com>");
MODULE_DESCRIPTION("DRM Driver for Raydium RM72014 MIPI DSI panel");
MODULE_LICENSE("GPL v2");
