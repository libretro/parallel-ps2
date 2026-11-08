/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2022  PCSX2 Dev Team
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

/* What a memory card answers each command with. C89. */
#pragma once

#include <stddef.h>

#include "../common/Pcsx2Types.h"

#ifdef __cplusplus
extern "C" {
#endif

struct SaveStateBase;

/* A PS1 card transfer, byte by byte. */
typedef struct PS1MemoryCardState
{
	size_t currentByte;
	u8 sectorAddrMSB;
	u8 sectorAddrLSB;
	u8 checksum;
	u8 expectedChecksum;
	u8 buf[128];
} PS1MemoryCardState;

void mcp_reset_ps1(void);
bool mcp_freeze_ps1(struct SaveStateBase* s);

void mcp_probe(void);
void mcp_unknown_write_delete_end(void);
void mcp_set_sector(void);
void mcp_get_specs(void);
void mcp_set_terminator(void);
void mcp_get_terminator(void);
void mcp_write_data(void);
void mcp_read_data(void);
u8 mcp_ps1_read(u8 data);
u8 mcp_ps1_state(u8 data);
u8 mcp_ps1_write(u8 data);
u8 mcp_ps1_pocketstation(u8 data);
void mcp_read_write_end(void);
void mcp_erase_block(void);
void mcp_unknown_boot(void);
void mcp_auth_xor(void);
void mcp_auth_f3(void);
void mcp_auth_f7(void);

#ifdef __cplusplus
}
#endif
