// SPDX-License-Identifier: GPL-2.0
/*
 *
 * Qualcomm MSM Camera Subsystem - TPG Module
 *
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <media/media-entity.h>
#include <media/mipi-csi2.h>
#include <media/v4l2-device.h>
#include <media/v4l2-subdev.h>

#include "camss-tpg.h"
#include "camss.h"

static const struct tpg_format_info formats_gen1[] = {
	{ MEDIA_BUS_FMT_SBGGR8_1X8,   MIPI_CSI2_DT_RAW8,  ENCODE_FORMAT_UNCOMPRESSED_8_BIT,  8  },
	{ MEDIA_BUS_FMT_SGBRG8_1X8,   MIPI_CSI2_DT_RAW8,  ENCODE_FORMAT_UNCOMPRESSED_8_BIT,  8  },
	{ MEDIA_BUS_FMT_SGRBG8_1X8,   MIPI_CSI2_DT_RAW8,  ENCODE_FORMAT_UNCOMPRESSED_8_BIT,  8  },
	{ MEDIA_BUS_FMT_SRGGB8_1X8,   MIPI_CSI2_DT_RAW8,  ENCODE_FORMAT_UNCOMPRESSED_8_BIT,  8  },
	{ MEDIA_BUS_FMT_SBGGR10_1X10, MIPI_CSI2_DT_RAW10, ENCODE_FORMAT_UNCOMPRESSED_10_BIT, 10 },
	{ MEDIA_BUS_FMT_SGBRG10_1X10, MIPI_CSI2_DT_RAW10, ENCODE_FORMAT_UNCOMPRESSED_10_BIT, 10 },
	{ MEDIA_BUS_FMT_SGRBG10_1X10, MIPI_CSI2_DT_RAW10, ENCODE_FORMAT_UNCOMPRESSED_10_BIT, 10 },
	{ MEDIA_BUS_FMT_SRGGB10_1X10, MIPI_CSI2_DT_RAW10, ENCODE_FORMAT_UNCOMPRESSED_10_BIT, 10 },
	{ MEDIA_BUS_FMT_SBGGR12_1X12, MIPI_CSI2_DT_RAW12, ENCODE_FORMAT_UNCOMPRESSED_12_BIT, 12 },
	{ MEDIA_BUS_FMT_SGBRG12_1X12, MIPI_CSI2_DT_RAW12, ENCODE_FORMAT_UNCOMPRESSED_12_BIT, 12 },
	{ MEDIA_BUS_FMT_SGRBG12_1X12, MIPI_CSI2_DT_RAW12, ENCODE_FORMAT_UNCOMPRESSED_12_BIT, 12 },
	{ MEDIA_BUS_FMT_SRGGB12_1X12, MIPI_CSI2_DT_RAW12, ENCODE_FORMAT_UNCOMPRESSED_12_BIT, 12 },
	{ MEDIA_BUS_FMT_Y8_1X8,       MIPI_CSI2_DT_RAW8,  ENCODE_FORMAT_UNCOMPRESSED_8_BIT,  8  },
	{ MEDIA_BUS_FMT_Y10_1X10,     MIPI_CSI2_DT_RAW10, ENCODE_FORMAT_UNCOMPRESSED_10_BIT, 10 },
};

const struct tpg_formats tpg_formats_gen1 = {
	.nformats = ARRAY_SIZE(formats_gen1),
	.formats  = formats_gen1,
};

const struct tpg_format_info *tpg_get_fmt_entry(const struct tpg_format_info *formats,
						unsigned int nformats,
						u32 code)
{
	unsigned int i;

	for (i = 0; i < nformats; i++)
		if (code == formats[i].code)
			return &formats[i];

	return ERR_PTR(-EINVAL);
}

static int tpg_set_clock_rates(struct tpg_device *tpg)
{
	struct device *dev = tpg->camss->dev;
	int i, ret;

	for (i = 0; i < tpg->nclocks; i++) {
		struct camss_clock *clock = &tpg->clock[i];
		long round_rate;

		if (clock->freq) {
			round_rate = clk_round_rate(clock->clk, clock->freq[0]);
			if (round_rate < 0) {
				dev_err(dev, "clk round rate failed: %ld\n",
					round_rate);
				return -EINVAL;
			}

			ret = clk_set_rate(clock->clk, round_rate);
			if (ret < 0) {
				dev_err(dev, "clk set rate failed: %d\n", ret);
				return ret;
			}
		}
	}

	return 0;
}

static int tpg_set_power(struct v4l2_subdev *sd, int on)
{
	struct tpg_device *tpg = v4l2_get_subdevdata(sd);
	struct device *dev = tpg->camss->dev;

	if (on) {
		int ret;

		ret = pm_runtime_resume_and_get(dev);
		if (ret < 0)
			return ret;

		ret = tpg_set_clock_rates(tpg);
		if (ret < 0) {
			pm_runtime_put_sync(dev);
			return ret;
		}

		ret = camss_enable_clocks(tpg->nclocks, tpg->clock, dev);
		if (ret < 0) {
			pm_runtime_put_sync(dev);
			return ret;
		}

		tpg->res->hw_ops->reset(tpg);

		tpg->res->hw_ops->hw_version(tpg);
	} else {
		camss_disable_clocks(tpg->nclocks, tpg->clock);

		pm_runtime_put_sync(dev);
	}

	return 0;
}

static struct v4l2_mbus_framefmt *
__tpg_get_format(struct tpg_device *tpg,
		 struct v4l2_subdev_state *sd_state,
		 unsigned int pad, u32 stream,
		 enum v4l2_subdev_format_whence which)
{
	if (which == V4L2_SUBDEV_FORMAT_TRY)
		return v4l2_subdev_state_get_format(sd_state, pad, stream);

	if (stream >= MSM_TPG_MAX_SRC_STREAMS)
		return NULL;

	return &tpg->vc_cfg[0].dt[0].fmt;
}

static void tpg_try_format(struct tpg_device *tpg,
			   struct v4l2_mbus_framefmt *fmt)
{
	unsigned int i;

	for (i = 0; i < tpg->res->formats->nformats; i++)
		if (tpg->res->formats->formats[i].code == fmt->code)
			break;

	if (i >= tpg->res->formats->nformats)
		fmt->code = MEDIA_BUS_FMT_SBGGR8_1X8;

	fmt->width = clamp_t(u32, fmt->width, TPG_MIN_WIDTH, TPG_MAX_WIDTH);
	fmt->height = clamp_t(u32, fmt->height, TPG_MIN_HEIGHT, TPG_MAX_HEIGHT);
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_SRGB;
}

static int tpg_enum_mbus_code(struct v4l2_subdev *sd,
			      struct v4l2_subdev_state *sd_state,
			      struct v4l2_subdev_mbus_code_enum *code)
{
	struct tpg_device *tpg = v4l2_get_subdevdata(sd);

	if (code->index >= tpg->res->formats->nformats)
		return -EINVAL;

	code->code = tpg->res->formats->formats[code->index].code;

	return 0;
}

static int tpg_enum_frame_size(struct v4l2_subdev *sd,
			       struct v4l2_subdev_state *sd_state,
			       struct v4l2_subdev_frame_size_enum *fse)
{
	struct tpg_device *tpg = v4l2_get_subdevdata(sd);
	unsigned int i;

	if (fse->index != 0)
		return -EINVAL;

	for (i = 0; i < tpg->res->formats->nformats; i++)
		if (tpg->res->formats->formats[i].code == fse->code)
			break;

	if (i >= tpg->res->formats->nformats)
		return -EINVAL;

	fse->min_width = TPG_MIN_WIDTH;
	fse->min_height = TPG_MIN_HEIGHT;
	fse->max_width = TPG_MAX_WIDTH;
	fse->max_height = TPG_MAX_HEIGHT;

	return 0;
}

static int tpg_get_format(struct v4l2_subdev *sd,
			  struct v4l2_subdev_state *sd_state,
			  struct v4l2_subdev_format *fmt)
{
	struct tpg_device *tpg = v4l2_get_subdevdata(sd);
	struct v4l2_mbus_framefmt *format;

	format = __tpg_get_format(tpg, sd_state, fmt->pad, fmt->stream,
				  fmt->which);
	if (!format)
		return -EINVAL;

	fmt->format = *format;

	return 0;
}

static int tpg_set_format(struct v4l2_subdev *sd,
                          struct v4l2_subdev_state *sd_state,
                          struct v4l2_subdev_format *fmt)
{
    struct tpg_device *tpg = v4l2_get_subdevdata(sd);
    struct v4l2_mbus_framefmt *format;

    dev_err(tpg->camss->dev, "tpg_set_format: pad=%u stream=%u which=%d code=0x%x %ux%u\n",
            fmt->pad, fmt->stream, fmt->which, 
            fmt->format.code, fmt->format.width, fmt->format.height);

    if (fmt->pad != MSM_TPG_PAD_SRC)
        return -EINVAL;

    format = __tpg_get_format(tpg, sd_state, fmt->pad, fmt->stream,
                              fmt->which);
    if (!format) {
        dev_err(tpg->camss->dev, "tpg_set_format: no format found\n");
        return -EINVAL;
    }

    tpg_try_format(tpg, &fmt->format);
    *format = fmt->format;
    
    dev_err(tpg->camss->dev, "tpg_set_format: saved format code=0x%x %ux%u\n",
            format->code, format->width, format->height);

    return 0;
}

static const struct v4l2_mbus_framefmt tpg_default_format = {
	.code		= MEDIA_BUS_FMT_SBGGR8_1X8,
	.width		= 1920,
	.height		= 1080,
	.field		= V4L2_FIELD_NONE,
	.colorspace	= V4L2_COLORSPACE_SRGB,
};

static int tpg_init_formats(struct v4l2_subdev *sd,
			    struct v4l2_subdev_fh *fh)
{
	struct v4l2_subdev_format format = {
		.pad	= MSM_TPG_PAD_SRC,
		.stream	= 0,
		.which	= fh ? V4L2_SUBDEV_FORMAT_TRY :
			      V4L2_SUBDEV_FORMAT_ACTIVE,
		.format	= {
			.code	= MEDIA_BUS_FMT_SBGGR8_1X8,
			.width	= 1920,
			.height	= 1080,
		}
	};

	return tpg_set_format(sd, fh ? fh->state : NULL, &format);
}

static int tpg_set_routing(struct v4l2_subdev *sd,
                           struct v4l2_subdev_state *state,
                           enum v4l2_subdev_format_whence which,
                           struct v4l2_subdev_krouting *routing)
{
    unsigned int i;
    int ret;

    // 手动验证路由，不使用 v4l2_subdev_routing_validate
    for (i = 0; i < routing->num_routes; i++) {
        const struct v4l2_subdev_route *route = &routing->routes[i];

        // 检查 pad
        if (route->sink_pad != MSM_TPG_PAD_SRC ||
            route->source_pad != MSM_TPG_PAD_SRC) {
            dev_err(sd->dev, "Invalid pad in route %u\n", i);
            return -EINVAL;
        }

        // 检查 stream 范围
        if (route->source_stream >= MSM_TPG_MAX_SRC_STREAMS) {
            dev_err(sd->dev, "source_stream %u too large\n", route->source_stream);
            return -EINVAL;
        }

        // 检查 1-to-1 映射
        if (route->sink_stream != route->source_stream) {
            dev_err(sd->dev, "sink_stream != source_stream in route %u\n", i);
            return -EINVAL;
        }

        // 检查是否有重复的 stream
        for (unsigned int j = 0; j < i; j++) {
            if (routing->routes[j].source_stream == route->source_stream) {
                dev_err(sd->dev, "Duplicate source_stream %u in routes %u and %u\n",
                        route->source_stream, j, i);
                return -EINVAL;
            }
        }
    }

    // 直接设置路由，跳过内核验证
    dev_err(sd->dev, "Setting routing with format...\n");
    ret = v4l2_subdev_set_routing_with_fmt(sd, state, routing,
                                           &tpg_default_format);
    if (ret < 0) {
        dev_err(sd->dev, "v4l2_subdev_set_routing_with_fmt failed: %d\n", ret);
        return ret;
    }

    dev_err(sd->dev, "tpg_set_routing successful\n");
    return 0;
}

static int tpg_enable_streams(struct v4l2_subdev *sd,
			      struct v4l2_subdev_state *state,
			      u32 pad, u64 streams_mask)
{
	struct tpg_device *tpg = v4l2_get_subdevdata(sd);
	const struct v4l2_subdev_krouting *routing = &state->routing;
	unsigned int i;
	int ret;

	for (i = 0; i < routing->num_routes; i++) {
		const struct v4l2_subdev_route *route = &routing->routes[i];
		const struct v4l2_mbus_framefmt *fmt;
		u32 stream = route->source_stream;
		u32 vc_idx = stream / MSM_TPG_MAX_DT_PER_VC;
		u32 dt_idx = stream % MSM_TPG_MAX_DT_PER_VC;

		if (!(streams_mask & BIT_ULL(stream)))
			continue;

		if (!(route->flags & V4L2_SUBDEV_ROUTE_FL_ACTIVE))
			continue;

		fmt = v4l2_subdev_state_get_format(state, MSM_TPG_PAD_SRC,
						   stream);
		if (!fmt)
			continue;

		tpg->vc_cfg[vc_idx].vc = vc_idx;
		tpg->vc_cfg[vc_idx].dt[dt_idx].enabled = true;
		tpg->vc_cfg[vc_idx].dt[dt_idx].fmt = *fmt;
		tpg->vc_cfg[vc_idx].active_dts++;
		tpg->enabled_streams |= BIT_ULL(stream);
	}

	if (!tpg->enabled_streams)
		return -ENOLINK;

	ret = v4l2_ctrl_handler_setup(&tpg->ctrls);
	if (ret < 0) {
		dev_err(tpg->camss->dev,
			"could not sync v4l2 controls: %d\n", ret);
		return ret;
	}

	return tpg->res->hw_ops->configure_stream(tpg, 1);
}

static int tpg_disable_streams(struct v4l2_subdev *sd,
			       struct v4l2_subdev_state *state,
			       u32 pad, u64 streams_mask)
{
	struct tpg_device *tpg = v4l2_get_subdevdata(sd);
	const struct v4l2_subdev_krouting *routing = &state->routing;
	unsigned int i;

	for (i = 0; i < routing->num_routes; i++) {
		const struct v4l2_subdev_route *route = &routing->routes[i];
		u32 stream = route->source_stream;
		u32 vc_idx = stream / MSM_TPG_MAX_DT_PER_VC;
		u32 dt_idx = stream % MSM_TPG_MAX_DT_PER_VC;

		if (!(streams_mask & BIT_ULL(stream)))
			continue;

		if (!tpg->vc_cfg[vc_idx].dt[dt_idx].enabled)
			continue;

		tpg->vc_cfg[vc_idx].dt[dt_idx].enabled = false;
		tpg->vc_cfg[vc_idx].active_dts--;
	}

	tpg->enabled_streams &= ~streams_mask;

	if (!tpg->enabled_streams)
		tpg->res->hw_ops->configure_stream(tpg, 0);

	return 0;
}

static int tpg_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct tpg_device *tpg = container_of(ctrl->handler,
					      struct tpg_device, ctrls);
	int ret = -EINVAL;

	switch (ctrl->id) {
	case V4L2_CID_TEST_PATTERN:
		ret = tpg->res->hw_ops->configure_testgen_pattern(tpg, ctrl->val);
		break;
	}

	return ret;
}

static const struct v4l2_ctrl_ops tpg_ctrl_ops = {
	.s_ctrl = tpg_s_ctrl,
};

int msm_tpg_subdev_init(struct camss *camss,
			struct tpg_device *tpg,
			const struct camss_subdev_resources *res, u8 id)
{
	struct platform_device *pdev;
	struct device *dev;
	int i, j;

	dev  = camss->dev;
	pdev = to_platform_device(dev);

	tpg->camss = camss;
	tpg->id = id;
	tpg->res = &res->tpg;
	tpg->res->hw_ops->subdev_init(tpg);

	tpg->base = devm_platform_ioremap_resource_byname(pdev, res->reg[0]);
	if (IS_ERR(tpg->base))
		return PTR_ERR(tpg->base);

	tpg->nclocks = 0;
	while (res->clock[tpg->nclocks])
		tpg->nclocks++;

	if (!tpg->nclocks)
		return 0;

	tpg->clock = devm_kcalloc(dev, tpg->nclocks,
				  sizeof(*tpg->clock), GFP_KERNEL);
	if (!tpg->clock)
		return -ENOMEM;

	for (i = 0; i < tpg->nclocks; i++) {
		struct camss_clock *clock = &tpg->clock[i];

		clock->clk = devm_clk_get(dev, res->clock[i]);
		if (IS_ERR(clock->clk))
			return PTR_ERR(clock->clk);

		clock->name = res->clock[i];

		clock->nfreqs = 0;
		while (res->clock_rate[i][clock->nfreqs])
			clock->nfreqs++;

		if (!clock->nfreqs) {
			clock->freq = NULL;
			continue;
		}

		clock->freq = devm_kcalloc(dev, clock->nfreqs,
					   sizeof(*clock->freq), GFP_KERNEL);
		if (!clock->freq)
			return -ENOMEM;

		for (j = 0; j < clock->nfreqs; j++)
			clock->freq[j] = res->clock_rate[i][j];
	}

	return 0;
}

static int tpg_init_state(struct v4l2_subdev *sd, 
                          struct v4l2_subdev_state *state)
{
    struct tpg_device *tpg = v4l2_get_subdevdata(sd);
    int ret;

    dev_err(tpg->camss->dev, "tpg_init_state ENTERED\n");

    if (!state) {
        dev_err(tpg->camss->dev, "tpg_init_state: state is NULL\n");
        return -EINVAL;
    }

    // 只在路由为空时初始化路由
    if (state->routing.num_routes == 0) {
        struct v4l2_subdev_route routes[MSM_TPG_MAX_SRC_STREAMS] = {0};
        struct v4l2_subdev_krouting routing = {
            .routes = routes,
            .num_routes = 0,
        };
        unsigned int i;

        dev_err(tpg->camss->dev, "tpg_init_state: initializing routing\n");

        // Initialize default routing (one stream per VC/DT combination)
        for (i = 0; i < MSM_TPG_MAX_SRC_STREAMS; i++) {
            routes[i].sink_pad = MSM_TPG_PAD_SRC;
            routes[i].sink_stream = i;
            routes[i].source_pad = MSM_TPG_PAD_SRC;
            routes[i].source_stream = i;
            routes[i].flags = V4L2_SUBDEV_ROUTE_FL_ACTIVE;
            routing.num_routes++;
        }

        ret = tpg_set_routing(sd, state, V4L2_SUBDEV_FORMAT_ACTIVE, &routing);
        if (ret < 0) {
            dev_err(tpg->camss->dev, "tpg_set_routing failed: %d\n", ret);
            return ret;
        }
    } else {
        dev_err(tpg->camss->dev, "tpg_init_state: routing already exists (%u routes)\n", 
                state->routing.num_routes);
    }

    // 只在格式为空时初始化格式
    for (unsigned int i = 0; i < MSM_TPG_MAX_SRC_STREAMS; i++) {
        struct v4l2_mbus_framefmt *format;
        
        format = v4l2_subdev_state_get_format(state, MSM_TPG_PAD_SRC, i);
        if (!format) {
            dev_err(tpg->camss->dev, "tpg_init_state: failed to get format for stream %u\n", i);
            continue;
        }

        // 如果格式的宽度为0，说明未初始化
        if (format->width == 0) {
            struct v4l2_subdev_format fmt = {
                .pad = MSM_TPG_PAD_SRC,
                .stream = i,
                .which = V4L2_SUBDEV_FORMAT_ACTIVE,
                .format = tpg_default_format,
            };
            
            dev_err(tpg->camss->dev, "tpg_init_state: initializing format for stream %u\n", i);
            
            ret = tpg_set_format(sd, state, &fmt);
            if (ret < 0) {
                dev_err(tpg->camss->dev, "tpg_set_format failed for stream %u: %d\n", i, ret);
                return ret;
            }
        } else {
            dev_err(tpg->camss->dev, "tpg_init_state: format already exists for stream %u: %ux%u\n",
                    i, format->width, format->height);
        }
    }

    dev_err(tpg->camss->dev, "tpg_init_state successful\n");
    return 0;
}

static const struct v4l2_subdev_internal_ops tpg_internal_ops = {
    .init_state = tpg_init_state,
};

static const struct v4l2_subdev_core_ops tpg_core_ops = {
	.s_power = tpg_set_power,
};

static const struct v4l2_subdev_pad_ops tpg_pad_ops = {
	.enum_mbus_code		= tpg_enum_mbus_code,
	.enum_frame_size	= tpg_enum_frame_size,
	.get_fmt		= tpg_get_format,
	.set_fmt		= tpg_set_format,
	.set_routing		= tpg_set_routing,
	.enable_streams		= tpg_enable_streams,
	.disable_streams	= tpg_disable_streams,
};

static const struct v4l2_subdev_ops tpg_v4l2_ops = {
	.core	= &tpg_core_ops,
	.pad	= &tpg_pad_ops,
};

static const struct media_entity_operations tpg_media_ops = {
	.get_fwnode_pad = v4l2_subdev_get_fwnode_pad_1_to_1,
	.has_pad_interdep = v4l2_subdev_has_pad_interdep,
	.link_validate = v4l2_subdev_link_validate,
};

int msm_tpg_register_entity(struct tpg_device *tpg,
			    struct v4l2_device *v4l2_dev)
{
	struct v4l2_subdev *sd = &tpg->subdev;
	struct media_pad *pads = tpg->pads;
	struct device *dev = tpg->camss->dev;
	int ret;

	v4l2_subdev_init(sd, &tpg_v4l2_ops);
	sd->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE |
		     V4L2_SUBDEV_FL_HAS_EVENTS |
		     V4L2_SUBDEV_FL_STREAMS;
	snprintf(sd->name, ARRAY_SIZE(sd->name), "%s%d",
		 "msm_tpg", tpg->id);
	sd->grp_id = TPG_GRP_ID;
	v4l2_set_subdevdata(sd, tpg);

	ret = v4l2_ctrl_handler_init(&tpg->ctrls, 1);
	if (ret < 0) {
		dev_err(dev, "Failed to init ctrl handler: %d\n", ret);
		return ret;
	}

	tpg->testgen_mode = v4l2_ctrl_new_std_menu_items(&tpg->ctrls,
							 &tpg_ctrl_ops, V4L2_CID_TEST_PATTERN,
							 tpg->testgen.nmodes, 0, 0,
							 tpg->testgen.modes);
	if (tpg->ctrls.error) {
		dev_err(dev, "Failed to init ctrl: %d\n", tpg->ctrls.error);
		ret = tpg->ctrls.error;
		goto free_ctrl;
	}

	tpg->subdev.ctrl_handler = &tpg->ctrls;

	ret = tpg_init_formats(sd, NULL);
	if (ret < 0) {
		dev_err(dev, "Failed to init format: %d\n", ret);
		goto free_ctrl;
	}

	pads[MSM_TPG_PAD_SRC].flags = MEDIA_PAD_FL_SOURCE;

	sd->entity.ops = &tpg_media_ops;
	sd->internal_ops = &tpg_internal_ops;
	ret = media_entity_pads_init(&sd->entity, MSM_TPG_PADS_NUM, pads);
	if (ret < 0) {
		dev_err(dev, "Failed to init media entity: %d\n", ret);
		goto free_ctrl;
	}

	ret = v4l2_subdev_init_finalize(sd);
	if (ret < 0) {
		dev_err(dev, "Failed to init subdev state: %d\n", ret);
		goto cleanup_entity;
	}

	ret = v4l2_device_register_subdev(v4l2_dev, sd);
	if (ret < 0) {
		dev_err(dev, "Failed to register subdev: %d\n", ret);
		goto cleanup_finalize;
	}

	return 0;

cleanup_finalize:
	v4l2_subdev_cleanup(sd);
cleanup_entity:
	media_entity_cleanup(&sd->entity);
free_ctrl:
	v4l2_ctrl_handler_free(&tpg->ctrls);

	return ret;
}

void msm_tpg_unregister_entity(struct tpg_device *tpg)
{
	v4l2_device_unregister_subdev(&tpg->subdev);
	v4l2_subdev_cleanup(&tpg->subdev);
	media_entity_cleanup(&tpg->subdev.entity);
	v4l2_ctrl_handler_free(&tpg->ctrls);
}
