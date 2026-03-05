// SPDX-License-Identifier: GPL-2.0
/*
 *
 * Qualcomm MSM Camera Subsystem - TPG (Test Pattern Generator) Module
 *
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */
#include <linux/completion.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/of.h>

#include "camss-tpg.h"
#include "camss.h"

/* TPG global registers */
#define TPG_HW_VERSION		0x0
# define HW_VERSION_STEPPING		GENMASK(15, 0)
# define HW_VERSION_REVISION		GENMASK(27, 16)
# define HW_VERSION_GENERATION		GENMASK(31, 28)

#define TPG_HW_VER(gen, rev, step) \
	(((u32)(gen) << 28) | ((u32)(rev) << 16) | (u32)(step))

#define TPG_HW_VER_2_0_0                TPG_HW_VER(2, 0, 0)
#define TPG_HW_VER_2_1_0                TPG_HW_VER(2, 1, 0)

#define TPG_HW_STATUS		0x4

#define TPG_CTRL		0x64
# define TPG_CTRL_TEST_EN		BIT(0)
# define TPG_CTRL_PHY_SEL		BIT(3)
# define TPG_CTRL_NUM_ACTIVE_LANES	GENMASK(5, 4)
# define TPG_CTRL_VC_DT_PATTERN_ID	GENMASK(8, 6)
# define TPG_CTRL_OVERLAP_SHDR_EN	BIT(10)
# define TPG_CTRL_NUM_ACTIVE_VC		GENMASK(31, 30)

#define TPG_CLEAR		0x1F4

/* TPG VC-based registers */
#define TPG_VC_n_GAIN_CFG(n)		(0x60 + (n) * 0x60)

#define TPG_VC_n_CFG0(n)	(0x68 + (n) * 0x60)
# define TPG_VC_n_CFG0_VC_NUM			GENMASK(4, 0)
# define TPG_VC_n_CFG0_NUM_ACTIVE_DT		GENMASK(9, 8)
# define TPG_VC_n_CFG0_NUM_BATCH		GENMASK(15, 12)
# define TPG_VC_n_CFG0_NUM_FRAMES		GENMASK(31, 16)

#define TPG_VC_n_LSFR_SEED(n)	(0x6C + (n) * 0x60)
#define TPG_VC_n_HBI_CFG(n)	(0x70 + (n) * 0x60)
#define TPG_VC_n_VBI_CFG(n)	(0x74 + (n) * 0x60)

#define TPG_VC_n_COLOR_BARS_CFG(n)		(0x78 + (n) * 0x60)
# define TPG_VC_n_COLOR_BARS_CFG_PIX_PATTERN		GENMASK(2, 0)
# define TPG_VC_n_COLOR_BARS_CFG_QCFA_EN		BIT(3)
# define TPG_VC_n_COLOR_BARS_CFG_SPLIT_EN		BIT(4)
# define TPG_VC_n_COLOR_BARS_CFG_NOISE_EN		BIT(5)
# define TPG_VC_n_COLOR_BARS_CFG_ROTATE_PERIOD		GENMASK(13, 8)
# define TPG_VC_n_COLOR_BARS_CFG_XCFA_EN		BIT(16)
# define TPG_VC_n_COLOR_BARS_CFG_SIZE_X			GENMASK(26, 24)
# define TPG_VC_n_COLOR_BARS_CFG_SIZE_Y			GENMASK(30, 28)

/* TPG DT-based registers */
#define TPG_VC_m_DT_n_CFG_0(m, n)		(0x7C + (m) * 0x60 + (n) * 0xC)
# define TPG_VC_m_DT_n_CFG_0_FRAME_HEIGHT	GENMASK(15, 0)
# define TPG_VC_m_DT_n_CFG_0_FRAME_WIDTH	GENMASK(31, 16)

#define TPG_VC_m_DT_n_CFG_1(m, n)		(0x80 + (m) * 0x60 + (n) * 0xC)
# define TPG_VC_m_DT_n_CFG_1_DATA_TYPE		GENMASK(5, 0)
# define TPG_VC_m_DT_n_CFG_1_ECC_XOR_MASK	GENMASK(13, 8)
# define TPG_VC_m_DT_n_CFG_1_CRC_XOR_MASK	GENMASK(31, 16)

#define TPG_VC_m_DT_n_CFG_2(m, n)		(0x84 + (m) * 0x60 + (n) * 0xC)
# define TPG_VC_m_DT_n_CFG_2_PAYLOAD_MODE		GENMASK(3, 0)
/* v2.0.0: USER[19:4], ENC[23:20] */
# define TPG_V2_0_0_VC_m_DT_n_CFG_2_USER_SPECIFIED_PAYLOAD		GENMASK(19, 4)
# define TPG_V2_0_0_VC_m_DT_n_CFG_2_ENCODE_FORMAT			GENMASK(23, 20)
/* v2.1.0: USER[27:4], ENC[31:28] */
# define TPG_V2_1_0_VC_m_DT_n_CFG_2_USER_SPECIFIED_PAYLOAD	GENMASK(27, 4)
# define TPG_V2_1_0_VC_m_DT_n_CFG_2_ENCODE_FORMAT			GENMASK(31, 28)

#define TPG_HBI_PCT_DEFAULT			545	/* 545% */
#define TPG_VBI_PCT_DEFAULT			10	/* 10% */
#define PERCENT_BASE				100

/* Default user-specified payload for TPG test generator.
 * Keep consistent with CSID TPG default: 0xBE.
 */
#define TPG_USER_SPECIFIED_PAYLOAD_DEFAULT	0xBE
#define TPG_LFSR_SEED_DEFAULT			0x12345678
#define TPG_COLOR_BARS_CFG_STANDARD \
	FIELD_PREP(TPG_VC_n_COLOR_BARS_CFG_ROTATE_PERIOD, 0xA)

static const char * const testgen_payload_modes[] = {
	[TPG_PAYLOAD_MODE_DISABLED]		= "Disabled",
	[TPG_PAYLOAD_MODE_INCREMENTING]		= "Incrementing",
	[TPG_PAYLOAD_MODE_ALTERNATING_55_AA]	= "Alternating 0x55/0xAA",
	[TPG_PAYLOAD_MODE_RANDOM]		= "Pseudo-random Data",
	[TPG_PAYLOAD_MODE_USER_SPECIFIED]	= "User Specified",
	[TPG_PAYLOAD_MODE_COLOR_BARS]		= "Color bars",
};

static int tpg_stream_on(struct tpg_device *tpg)
{
	struct tpg_testgen_config *tg = &tpg->testgen;
	u8 payload_mode = (tg->mode > TPG_PAYLOAD_MODE_DISABLED) ?
			   tg->mode - 1 : 0;
	u8 lane_cnt = tpg->res->lane_cnt;
	u8 last_vc = 0;
	unsigned int vc_idx;
	u32 val;

	if (!tpg->enabled_streams)
		return -ENOLINK;

	for (vc_idx = 0; vc_idx < MSM_TPG_MAX_VC; vc_idx++) {
		const struct tpg_vc_cfg *vc = &tpg->vc_cfg[vc_idx];
		const struct v4l2_mbus_framefmt *hbi_fmt = NULL;
		const struct tpg_format_info *hbi_fi;
		u8 active_dt_cnt = 0;
		unsigned int dt_idx;

		/* Skip VCs that have no enabled DT slots */
		for (dt_idx = 0; dt_idx < MSM_TPG_MAX_DT_PER_VC; dt_idx++) {
			if (vc->dt[dt_idx].enabled) {
				active_dt_cnt++;
				if (!hbi_fmt)
					hbi_fmt = &vc->dt[dt_idx].fmt;
			}
		}
		if (!active_dt_cnt)
			continue;

		last_vc = vc->vc;

		/*
		 * VC-level registers: use the first active DT's format for
		 * HBI/VBI timing, which is derived from the image resolution.
		 */
		hbi_fi = tpg_get_fmt_entry(tpg->res->formats->formats,
					   tpg->res->formats->nformats,
					   hbi_fmt->code);
		if (IS_ERR(hbi_fi))
			return -EINVAL;

		val = FIELD_PREP(TPG_VC_n_CFG0_NUM_ACTIVE_DT, active_dt_cnt - 1) |
		      FIELD_PREP(TPG_VC_n_CFG0_NUM_FRAMES, 0);
		writel(val, tpg->base + TPG_VC_n_CFG0(vc->vc));

		writel(TPG_LFSR_SEED_DEFAULT,
		       tpg->base + TPG_VC_n_LSFR_SEED(vc->vc));

		val = DIV_ROUND_UP(hbi_fmt->width * hbi_fi->bpp *
				   TPG_HBI_PCT_DEFAULT,
				   BITS_PER_BYTE * lane_cnt * PERCENT_BASE);
		writel(val, tpg->base + TPG_VC_n_HBI_CFG(vc->vc));

		val = hbi_fmt->height * TPG_VBI_PCT_DEFAULT / PERCENT_BASE;
		writel(val, tpg->base + TPG_VC_n_VBI_CFG(vc->vc));

		writel(TPG_COLOR_BARS_CFG_STANDARD,
		       tpg->base + TPG_VC_n_COLOR_BARS_CFG(vc->vc));

		/* DT-slot registers: each active slot gets its own format */
		for (dt_idx = 0; dt_idx < MSM_TPG_MAX_DT_PER_VC; dt_idx++) {
			const struct tpg_dt_cfg *dt = &vc->dt[dt_idx];
			const struct tpg_format_info *fi;

			if (!dt->enabled)
				continue;

			fi = tpg_get_fmt_entry(tpg->res->formats->formats,
					       tpg->res->formats->nformats,
					       dt->fmt.code);
			if (IS_ERR(fi))
				return -EINVAL;

			val = FIELD_PREP(TPG_VC_m_DT_n_CFG_0_FRAME_HEIGHT,
					 dt->fmt.height & 0xffff) |
			      FIELD_PREP(TPG_VC_m_DT_n_CFG_0_FRAME_WIDTH,
					 dt->fmt.width & 0xffff);
			writel(val, tpg->base +
			       TPG_VC_m_DT_n_CFG_0(vc->vc, dt_idx));

			val = FIELD_PREP(TPG_VC_m_DT_n_CFG_1_DATA_TYPE,
					 fi->data_type);
			writel(val, tpg->base +
			       TPG_VC_m_DT_n_CFG_1(vc->vc, dt_idx));

			if (tpg->hw_version == TPG_HW_VER_2_0_0) {
				val = FIELD_PREP(TPG_VC_m_DT_n_CFG_2_PAYLOAD_MODE,
						 payload_mode) |
				      FIELD_PREP(TPG_V2_0_0_VC_m_DT_n_CFG_2_USER_SPECIFIED_PAYLOAD,
						 TPG_USER_SPECIFIED_PAYLOAD_DEFAULT) |
				      FIELD_PREP(TPG_V2_0_0_VC_m_DT_n_CFG_2_ENCODE_FORMAT,
						 fi->encode_format);
			} else if (tpg->hw_version >= TPG_HW_VER_2_1_0) {
				val = FIELD_PREP(TPG_VC_m_DT_n_CFG_2_PAYLOAD_MODE,
						 payload_mode) |
				      FIELD_PREP(TPG_V2_1_0_VC_m_DT_n_CFG_2_USER_SPECIFIED_PAYLOAD,
						 TPG_USER_SPECIFIED_PAYLOAD_DEFAULT) |
				      FIELD_PREP(TPG_V2_1_0_VC_m_DT_n_CFG_2_ENCODE_FORMAT,
						 fi->encode_format);
			}
			writel(val, tpg->base +
			       TPG_VC_m_DT_n_CFG_2(vc->vc, dt_idx));
		}
	}

	/* Global TPG control:
	 * NUM_ACTIVE_VC field encodes the index of the highest active VC
	 * (0 = only VC0, 1 = VC0+VC1, 2 = VC0..VC2, 3 = VC0..VC3).
	 */
	val = FIELD_PREP(TPG_CTRL_TEST_EN, 1) |
	      FIELD_PREP(TPG_CTRL_NUM_ACTIVE_LANES, lane_cnt - 1) |
	      FIELD_PREP(TPG_CTRL_NUM_ACTIVE_VC, last_vc);
	writel(val, tpg->base + TPG_CTRL);

	return 0;
}

static int tpg_reset(struct tpg_device *tpg)
{
	writel(0, tpg->base + TPG_CTRL);
	writel(1, tpg->base + TPG_CLEAR);

	return 0;
}

static void tpg_stream_off(struct tpg_device *tpg)
{
	tpg_reset(tpg);
}

static int tpg_configure_stream(struct tpg_device *tpg, u8 enable)
{
	if (enable)
		return tpg_stream_on(tpg);

	tpg_stream_off(tpg);

	return 0;
}

static int tpg_configure_testgen_pattern(struct tpg_device *tpg, s32 val)
{
	if (val >= 0 && val <= TPG_PAYLOAD_MODE_COLOR_BARS)
		tpg->testgen.mode = val;

	return 0;
}

static u32 tpg_hw_version(struct tpg_device *tpg)
{
	u32 hw_version = readl(tpg->base + TPG_HW_VERSION);

	tpg->hw_version = hw_version;
	dev_dbg(tpg->camss->dev, "tpg HW Version = %u.%u.%u\n",
		(u32)FIELD_GET(HW_VERSION_GENERATION, hw_version),
		(u32)FIELD_GET(HW_VERSION_REVISION, hw_version),
		(u32)FIELD_GET(HW_VERSION_STEPPING, hw_version));

	return hw_version;
}

static void tpg_subdev_init(struct tpg_device *tpg)
{
	tpg->testgen.modes = testgen_payload_modes;
	tpg->testgen.nmodes = TPG_PAYLOAD_MODE_NUM_SUPPORTED_GEN1;
}

const struct tpg_hw_ops tpg_ops_gen1 = {
	.configure_stream = tpg_configure_stream,
	.configure_testgen_pattern = tpg_configure_testgen_pattern,
	.hw_version = tpg_hw_version,
	.reset = tpg_reset,
	.subdev_init = tpg_subdev_init,
};
