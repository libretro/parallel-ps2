/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2010  PCSX2 Dev Team
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

/* Memory card images, one per port and slot, held in RAM and written
 * through to their files. C89. */
#pragma once

#include <stddef.h>

#include "common/Pcsx2Defs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct McdSizeInfo
{
	u16 SectorSize;              /* bytes per sector: 512 or 1024 */
	u16 EraseBlockSizeInSectors; /* sectors per erase block, at most 16 */
	u32 McdSizeInSectors;        /* the card's size */
	u8 Xor;                      /* checksum of the above */
} McdSizeInfo;

uint FileMcd_GetMtapPort(uint slot);
uint FileMcd_GetMtapSlot(uint slot);
bool FileMcd_IsMultitapSlot(uint slot);
void FileMcd_GetDefaultName(char* out, size_t out_size, uint slot);

uint FileMcd_ConvertToSlot(uint port, uint slot);
void FileMcd_EmuOpen(void);
void FileMcd_EmuClose(void);
s32 FileMcd_IsPresent(uint port, uint slot);
/* The card's own MagicGate ID, from <image>.id beside its image (an
 * arcade dongle's); NULL when it has none. */
const u8* FileMcd_GetMagicGateId(uint port, uint slot);
/* The ID file for a slot's card, when it is not <image>.id (a Python 1
 * game names its own); NULL or empty for the one beside the image. Taken
 * at the next open. */
void FileMcd_SetIdPath(uint slot, const char* path);
void FileMcd_GetSizeInfo(uint port, uint slot, McdSizeInfo* outways);
bool FileMcd_IsPSX(uint port, uint slot);
s32 FileMcd_Read(uint port, uint slot, u8* dest, u32 adr, int size);
s32 FileMcd_Save(uint port, uint slot, const u8* src, u32 adr, int size);
s32 FileMcd_EraseBlock(uint port, uint slot, u32 adr);
u64 FileMcd_GetCRC(uint port, uint slot);
bool FileMcd_ReIndex(uint port, uint slot, const char* filter);

/* What the configuration says (Pcsx2Config.cpp). A combined slot's card:
 * its whole path into path (empty for none), whether it is enabled. */
bool emu_config_mcd(uint slot, char* path, size_t path_size);
bool emu_config_multitap(uint port);
bool emu_config_mcd_ejection(void);

#ifdef __cplusplus
}
#endif
