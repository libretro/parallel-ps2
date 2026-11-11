/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2021  PCSX2 Dev Team
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

#include "../../common/Pcsx2Defs.h"
#include "../FreezeTypes.h"
#include "PADsio.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The state of the PS2 bus. */
typedef struct QueryInfo
{
	u8 port;
	u8 slot;
	u8 lastByte;
	u8 currentCommand;
	u8 numBytes;
	u8 queryDone;
	u8 response[42];
} QueryInfo;

/* All the state of one pad that a PS2 can set; what a save state keeps. */
typedef struct PadFreezeData
{
	/* digital, analog or DS2 native */
	u8 mode;
	u8 modeLock;
	/* in config mode */
	u8 config;
	u8 vibrate[8];
	u8 umask[3];
	/* vibration indices */
	u8 vibrateI[2];
	/* The last vibration sent to the controller, so nothing is sent while
	 * old and new are both 0. */
	u8 currentVibrate[2];
	/* The next one; sent on every update unless both it and the current
	 * are 0. */
	u8 nextVibrate[2];
} PadFreezeData;

/* What a save state keeps of the pads: bytes only, so the same layout on
 * every host. */
typedef struct PadFullFreezeData
{
	char format[8];
	/* the active slot of each port */
	u8 slot[2];
	PadFreezeData padData[2][4];
	QueryInfo query;
} PadFullFreezeData;

/* A console port's options, as the frontend set them. */
typedef struct PadSettings
{
	u32 axis_scale_q16;  /* 16.16; the option is an integer percent */
	u32 rumble_scale_q8; /* 8.8 */
	int axis_invert_lx;
	int axis_invert_ly;
	int axis_invert_rx;
	int axis_invert_ry;
	u16 axis_deadzone;
	u16 button_deadzone;
	/* The pad starts analog instead of digital. Some games (Ridge Racer V)
	 * boot it digital and turn analog on only at the ANALOG button, which
	 * libretro has no spare input for. */
	bool force_analog;
} PadSettings;

extern PadSettings pad_settings[2];

/* The frontend's side: the ports and descriptors offered, the input read
 * each frame, and all released. */
void pad_input_init(void);
void pad_input_update(void);
void pad_input_shutdown(void);

/* Nonzero when a state's block is refused, which leaves the pads as they
 * were. */
s32 PADfreeze(FreezeAction mode, freezeData* data);

#ifdef __cplusplus
}
#endif
