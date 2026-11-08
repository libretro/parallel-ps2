/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2020  PCSX2 Dev Team
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

/* The i.LINK (FireWire) link layer at 0x1f808400. With no device on the
 * bus it is the register file it always was; with one (FWsetDevice) it
 * runs the link: PHY registers and bus reset with self-ID, async
 * transmit through the user buffer, the PHT channels, and receive into
 * the user and DBUF buffers. */
#pragma once

#include "../common/Pcsx2Types.h"
#include "FireWire/fw_device.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void FWopen(void);
void FWclose(void);
/* The machine's reset: with a device, a bus reset follows. */
void FWreset(void);

void PHYWrite(void);
void PHYRead(void);

u32 FWread32(u32 addr);
void FWwrite32(u32 addr, u32 value);

/* The IOP event the device scheduled (IopEvt_FW). */
void FWinterrupt(void);
/* One SPU2 output sample, for the device to mix into. */
void FWmixSubboardAudio(s32* left, s32* right);

/* The savestate block: fixed for the session, the device's state in it
 * when there is one. */
size_t FWStateSize(void);
void FWStateWrite(void* dst);
size_t FWStateStrip(const void* state, size_t size);
void FWStateRead(const void* state, size_t size);

#ifdef __cplusplus
}
#endif
