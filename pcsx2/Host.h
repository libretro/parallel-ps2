/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2021 PCSX2 Dev Team
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

#include "../common/Pcsx2Defs.h"

#ifdef __cplusplus
extern "C" {
#endif

struct Pcsx2Config;

/* The frontend's core options, as the config they set, and the values
 * beside it that no Pcsx2Config field holds. */
const struct Pcsx2Config* host_option_config(void);
const char* host_option_memcard_path(void);
/* The content's name without its extension, which names the first
 * memory card; empty for the shared cards. */
const char* host_content_name(void);

/* A file from the resources folder, read whole into memory the caller
 * frees; NULL when it cannot be read. */
void* host_read_resource_file(const char* filename, int64_t* size);

/* The running game changed; its serial is empty when none runs. */
void host_on_game_changed(const char* disc_path, const char* elf_override,
	const char* game_serial, u32 game_crc);

#ifdef __cplusplus
}
#endif
