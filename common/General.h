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

#include "Pcsx2Defs.h"

/* This header is reached from C through VirtualMemory.h, so these cannot be
 * unconditional -- but most of the tree, the DX and Vulkan backends among
 * them, has always picked up <memory> and friends through here. Keep giving
 * C++ what it had rather than chasing the includes across every dependent. */
#ifdef __cplusplus
#include <map>
#include <memory>
#include <string>
#endif

/* -------------------------------------------------------------------------------------- */
/*  PageProtectionMode */
/* -------------------------------------------------------------------------------------- */
typedef struct PageProtectionMode
{
	bool m_read;
	bool m_write;
	bool m_exec;
} PageProtectionMode;

