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

#pragma once

#include "../MemoryTypes.h"

typedef struct BiosDebugInformation
{
	u32 eeThreadListAddr;
	u32 iopThreadListAddr;
	u32 iopModListAddr;
} BiosDebugInformation;

extern BiosDebugInformation CurrentBiosInformation;
/* MSVC decorates C++ data symbols; the ELF ABI does not. A variable a C
 * unit references therefore has to be declared -- and so, through this
 * header, defined -- with C language linkage, or the Windows link fails
 * on an undecorated reference while every other target is fine. */
#ifdef __cplusplus
extern "C" {
#endif
	extern bool AllowParams1;
	extern bool AllowParams2;
	extern u32 BiosChecksum;
	extern u32 BiosVersion;		/* Used by CDVD */
	extern u32 BiosRegion;		/* Used by CDVD */
	/* A COH-H (arcade board) BIOS: ROMVER zone T, model Z. */
	extern bool BiosIsArcade;
	extern bool NoOSD;			/* Used for HLE OSD Config Params */
	/* The BIOS image's path; empty when none is loaded. */
	extern char BiosPath[];
#ifdef __cplusplus
}
#endif
#define BIOS_DESCRIPTION_MAX 96
#define BIOS_SERIAL_MAX      32
#define BIOS_ZONE_MAX        16

#ifdef __cplusplus
extern "C" {
#endif
	extern char BiosDescription[];
#ifdef __cplusplus
}
#endif
extern char BiosSerial[];
#ifdef __cplusplus
extern "C"
#endif
bool LoadBIOS(void);
extern bool IsBIOS(const char *filename, u32 *version, char *description, size_t description_size, u32 *region, char *zone, size_t zone_size);
extern bool IsBIOSAvailable(const char* full_path);
/* The arcade BIOS a Python game boots with, as a file name in dir: the
 * selected one when it is a COH-H BIOS, otherwise the first COH-H BIOS
 * in dir by name. False when dir has none. */
extern bool FindArcadeBios(const char* dir, const char* selected, char* out, size_t out_size);

