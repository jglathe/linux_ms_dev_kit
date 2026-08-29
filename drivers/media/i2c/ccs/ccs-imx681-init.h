/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Sony IMX681 3844x2640, 969.6-Msymbol/s Linux mode based on the Andre
 * Gilerson table from linux-surface-kernel and validated by the Snapdragon
 * reference port. It is distinct from the static Windows 3840x2640,
 * 1.2-Gsymbol/s mode. Multi-byte registers remain byte-split to preserve the
 * proven write order.
 */

#ifndef __CCS_IMX681_INIT_H__
#define __CCS_IMX681_INIT_H__

static const struct ccs_reg_8 imx681_mode_3844x2640_regs[] = {
	/* Software standby and 19.2 MHz external clock. */
	{ 0x0100, 0x00 },
	{ 0x0136, 0x13 },
	{ 0x0137, 0x33 },
	{ 0x002c, 0x05 },
	{ 0x002d, 0x05 },
	/* C-PHY, RAW10, one trio; the requested orientation is restored later. */
	{ 0x0110, 0x00 },
	{ 0x0111, 0x03 },
	{ 0x0112, 0x0a },
	{ 0x0113, 0x0a },
	{ 0x0114, 0x00 },
	{ 0x0101, 0x00 },
	/* Vendor unlock sequence; ordering is significant. */
	{ 0x30eb, 0x05 },
	{ 0x30eb, 0x0c },
	{ 0x300a, 0xff },
	{ 0x300b, 0xff },
	{ 0x3532, 0xff },
	{ 0x3533, 0xff },
	/*
	 * LLP 8704 leaves about 13 percent line-idle headroom. The earlier
	 * 7552 value caused periodic receiver FIFO overruns.
	 */
	{ 0x0342, 0x22 },
	{ 0x0343, 0x00 },
	/* Frame length is 3177 lines. */
	{ 0x033d, 0x00 },
	{ 0x033e, 0x0c },
	{ 0x033f, 0x69 },
	/* Analogue crop window from the proven Gilerson sequence. */
	{ 0x0345, 0x64 },
	{ 0x0346, 0x01 },
	{ 0x0347, 0x00 },
	{ 0x0349, 0x67 },
	{ 0x034a, 0x0b },
	{ 0x034b, 0x4f },
	{ 0x040d, 0x04 },
	{ 0x040e, 0x0a },
	{ 0x040f, 0x50 },
	/* The sensor emits 3844x2640; any 3840 crop belongs downstream. */
	{ 0x034c, 0x0f },
	{ 0x034d, 0x04 },
	{ 0x034e, 0x0a },
	{ 0x034f, 0x50 },
	/*
	 * Preserve the branch dividers selected by the CCS PLL calculator and
	 * override only the multipliers from the hardware-proven Snapdragon
	 * sequence.  The 0097c12-derived complete divider tuple produced no C-PHY
	 * packets on this device.
	 */
	{ 0x0307, 0xe1 },
	{ 0x030d, 0x03 },
	{ 0x030e, 0x01 },
	{ 0x030f, 0x2f },
	{ 0x7e9b, 0x02 },
	{ 0x0368, 0x00 },
	{ 0xd383, 0x01 },
};

#endif /* __CCS_IMX681_INIT_H__ */
