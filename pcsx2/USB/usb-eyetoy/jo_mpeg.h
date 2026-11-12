/* SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team */
/* SPDX-License-Identifier: GPL-3.0+ */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	JO_RGBX,
	JO_RGB24,
	JO_BGR24,
	JO_YUYV
} jo_mpeg_format_t;

typedef enum {
	JO_NONE,
	JO_FLIP_X,
	JO_FLIP_Y
} jo_mpeg_flip_t;

/* work holds JO_MPEG_WORK_FLOATS floats: the macroblock scratch, which the
 * caller keeps off the stack. */
#define JO_MPEG_WORK_FLOATS (256 * 3 + 64 * 2)

unsigned long jo_write_mpeg(unsigned char *mpeg_buf, const unsigned char *rgbx, int width, int height, int format, int flipx, int flipy, float *work);

#ifdef __cplusplus
}
#endif
