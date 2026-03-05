/* SPDX-License-Identifier: GPL-2.0 */
/*
 * camss-tpg.h
 *
 * Qualcomm MSM Camera Subsystem - TPG Module
 *
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */
#ifndef QC_MSM_CAMSS_TPG_H
#define QC_MSM_CAMSS_TPG_H

#include <linux/clk.h>
#include <linux/bitfield.h>
#include <media/media-entity.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-mediabus.h>
#include <media/v4l2-subdev.h>

#define ENCODE_FORMAT_UNCOMPRESSED_8_BIT	0x1
#define ENCODE_FORMAT_UNCOMPRESSED_10_BIT	0x2
#define ENCODE_FORMAT_UNCOMPRESSED_12_BIT	0x3
#define ENCODE_FORMAT_UNCOMPRESSED_14_BIT	0x4
#define ENCODE_FORMAT_UNCOMPRESSED_16_BIT	0x5
#define ENCODE_FORMAT_UNCOMPRESSED_20_BIT	0x6
#define ENCODE_FORMAT_UNCOMPRESSED_24_BIT	0x7

#define MSM_TPG_PAD_SRC			0
#define MSM_TPG_PADS_NUM		1
#define MSM_TPG_MAX_VC			4
#define MSM_TPG_MAX_DT_PER_VC		4
#define MSM_TPG_MAX_SRC_STREAMS		(MSM_TPG_MAX_VC * MSM_TPG_MAX_DT_PER_VC)

#define TPG_MIN_WIDTH   1
#define TPG_MIN_HEIGHT  1
#define TPG_MAX_WIDTH   8191
#define TPG_MAX_HEIGHT  8191

#define TPG_GRP_ID 0

enum tpg_testgen_mode {
	TPG_PAYLOAD_MODE_DISABLED = 0,
	TPG_PAYLOAD_MODE_INCREMENTING = 1,
	TPG_PAYLOAD_MODE_ALTERNATING_55_AA = 2,
	TPG_PAYLOAD_MODE_RANDOM = 5,
	TPG_PAYLOAD_MODE_USER_SPECIFIED = 6,
	TPG_PAYLOAD_MODE_COLOR_BARS = 9,
	TPG_PAYLOAD_MODE_NUM_SUPPORTED_GEN1 = 9,
};

struct tpg_testgen_config {
	enum tpg_testgen_mode mode;
	const char * const*modes;
	u8 nmodes;
};

struct tpg_format_info {
	u32 code;
	u8 data_type;
	u8 encode_format;
	u8 bpp;
};

struct tpg_formats {
	unsigned int nformats;
	const struct tpg_format_info *formats;
};

struct tpg_device;

/**
 * struct tpg_dt_cfg - per-DT-slot hardware configuration
 * @enabled: true if this DT slot is active
 * @fmt:     mbus format (resolution + pixel format) for this DT slot
 */
struct tpg_dt_cfg {
	bool enabled;
	struct v4l2_mbus_framefmt fmt;
};

/**
 * struct tpg_vc_cfg - per-VC hardware configuration
 * @vc:          CSI-2 Virtual Channel number (equals vc_idx)
 * @active_dts:  number of enabled DT slots in this VC
 * @dt:          per-DT-slot configuration, indexed by DT slot (0..MSM_TPG_MAX_DT_PER_VC-1)
 *
 * stream_id encoding (single pad, sink_stream == source_stream):
 *   vc_idx = stream_id / MSM_TPG_MAX_DT_PER_VC
 *   dt_idx = stream_id % MSM_TPG_MAX_DT_PER_VC
 *
 * e.g. stream 0..3 -> VC0 DT0..DT3, stream 4..7 -> VC1 DT0..DT3, etc.
 */
struct tpg_vc_cfg {
	u8 vc;
	u8 active_dts;
	struct tpg_dt_cfg dt[MSM_TPG_MAX_DT_PER_VC];
};

struct tpg_hw_ops {
	int (*configure_stream)(struct tpg_device *tpg, u8 enable);

	int (*configure_testgen_pattern)(struct tpg_device *tpg, s32 val);

	u32 (*hw_version)(struct tpg_device *tpg);

	int (*reset)(struct tpg_device *tpg);

	void (*subdev_init)(struct tpg_device *tpg);
};

struct tpg_subdev_resources {
	u8 lane_cnt;
	const struct tpg_formats *formats;
	const struct tpg_hw_ops *hw_ops;
};

struct tpg_device {
	struct camss *camss;
	u8 id;
	struct v4l2_subdev subdev;
	struct media_pad pads[MSM_TPG_PADS_NUM];
	void __iomem *base;
	struct camss_clock *clock;
	int nclocks;
	/* Bitmask of active stream IDs (bit N <-> stream N). */
	u64 enabled_streams;
	struct tpg_testgen_config testgen;
	/*
	 * Per-VC/DT configuration.
	 * vc_cfg[v].dt[d] corresponds to stream_id = v * MSM_TPG_MAX_DT_PER_VC + d.
	 */
	struct tpg_vc_cfg vc_cfg[MSM_TPG_MAX_VC];
	struct v4l2_ctrl_handler ctrls;
	struct v4l2_ctrl *testgen_mode;
	const struct tpg_subdev_resources *res;
	u32 hw_version;
};

struct camss_subdev_resources;

const struct tpg_format_info *tpg_get_fmt_entry(const struct tpg_format_info *formats,
						unsigned int nformats,
						u32 code);

int msm_tpg_subdev_init(struct camss *camss,
			struct tpg_device *tpg,
			const struct camss_subdev_resources *res, u8 id);

int msm_tpg_register_entity(struct tpg_device *tpg,
			    struct v4l2_device *v4l2_dev);

void msm_tpg_unregister_entity(struct tpg_device *tpg);

extern const struct tpg_formats tpg_formats_gen1;

extern const struct tpg_hw_ops tpg_ops_gen1;

#endif /* QC_MSM_CAMSS_TPG_H */
