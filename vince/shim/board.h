/*
 * Per-phone constants of the loader and the stand-in kernel. Select the phone at build time:
 *   (nothing)  Redmi 5 Plus (vince, MSM8953, 4 GB: DDR from 0x10000000)
 *   -DROSY     Redmi 5 (rosy, SDM450, 3 GB: DDR from 0x40000000)
 *
 * rosy values, measured on the phone (TWRP /proc/iomem, kernel command line) and read from the device
 * tree appended to its boot image:
 *   System RAM 0x40000000-0x849fffff, 0x86800000-0x86bfffff, 0x8ee00000-0xffffffff
 *   splash_region@0x90001000 (reg 0x90001000 + 0x13ff000), the same address as on vince
 *   panel ft8613_ebbg_5p7_720p_video: 720 x 1440, DSI video mode, 24 bpp
 * Assumed, to be confirmed by the loader test: LK draws packed 24 bpp with a stride of 720 * 3, and
 * places the ramdisk at DDR base + 0x3600000 as it does on vince (only used if /chosen has no initrd).
 */
#pragma once

#if defined(ROSY)
#define BOARD_NAME		"ROSY"
#define FB_BASE			0x90001000UL
#define PANEL_W			720
#define PANEL_H			1440
#define PANEL_STRIDE		2160
#define RAM_LO			0x40000000UL
#define RAM_HI			0x84a00000UL
#define RAM_RANGE_TEXT		"40000000-849fffff"
#define LK_RAMDISK_FALLBACK	0x43600000UL
#define LK_RAMDISK_TEXT		"43600000"
#else
#define BOARD_NAME		"VINCE"
#define FB_BASE			0x90001000UL	/* LK splash buffer, packed 24 bpp */
#define PANEL_W			1080
#define PANEL_H			2160
#define PANEL_STRIDE		3240
/* The only RAM window we hand to xnu: LK keeps its own data above 0x84a00000, modem etc. beyond */
#define RAM_LO			0x10000000UL
#define RAM_HI			0x84a00000UL
#define RAM_RANGE_TEXT		"10000000-849fffff"
#define LK_RAMDISK_FALLBACK	0x13600000UL	/* where LK put it in the measured runs */
#define LK_RAMDISK_TEXT		"13600000"
#endif
#define PANEL_BPP		3
