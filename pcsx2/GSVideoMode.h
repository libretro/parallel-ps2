/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2023  PCSX2 Dev Team
 *
 *  PCSX2 is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU Lesser General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  PCSX2 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with PCSX2.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "common/Pcsx2Types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The video mode the BIOS's SetGsCrt syscall last asked for. Saved as an
 * int in save states. */
typedef enum GS_VideoMode
{
	GS_VIDEOMODE_UNINITIALIZED,
	GS_VIDEOMODE_UNKNOWN,
	GS_VIDEOMODE_NTSC,
	GS_VIDEOMODE_PAL,
	GS_VIDEOMODE_VESA,
	GS_VIDEOMODE_SDTV_480P,
	GS_VIDEOMODE_SDTV_576P,
	GS_VIDEOMODE_HDTV_720P,
	GS_VIDEOMODE_HDTV_1080I,
	GS_VIDEOMODE_HDTV_1080P,
	GS_VIDEOMODE_DVD_NTSC,
	GS_VIDEOMODE_DVD_PAL
} GS_VideoMode;

/* R5900OpcodeImpl.c sets these from SetGsCrt. */
extern GS_VideoMode gsVideoMode;
extern bool gsIsInterlaced;

/* GS.cpp: records the mode and retimes the counters. */
extern void gsSetVideoMode(GS_VideoMode mode);

#ifdef __cplusplus
}
#endif
