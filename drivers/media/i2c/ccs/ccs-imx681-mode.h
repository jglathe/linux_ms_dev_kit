/* SPDX-License-Identifier: GPL-2.0-only */
/* Sony IMX681 mode validated on the Surface Pro 11 C-PHY receiver. */

#ifndef __CCS_IMX681_MODE_H__
#define __CCS_IMX681_MODE_H__

struct ccs_imx681_mode {
	const struct ccs_reg_8 *regs;
	size_t num_regs;
	u32 width;
	u32 height;
	u32 line_length_pck;
	u32 frame_length_lines;
	u32 pixel_rate;
	u32 exposure_default;
	u32 gain_default;
};

static const struct ccs_imx681_mode imx681_mode = {
	.regs = imx681_mode_3844x2640_regs,
	.num_regs = ARRAY_SIZE(imx681_mode_3844x2640_regs),
	.width = 3844,
	.height = 2640,
	.line_length_pck = 8704,
	.frame_length_lines = 3177,
	.pixel_rate = 432000000,
	/* Initial values from the hardware-proven Snapdragon stream recipe. */
	.exposure_default = 3100,
	/* Control value 192 maps to global U8.8 code 0x0400 (4x). */
	.gain_default = 192,
};

#endif /* __CCS_IMX681_MODE_H__ */
