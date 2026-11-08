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

/* The multitap's side of SIO2. C89. */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define MULTITAP_MODE_NOT_SET               0xff
#define MULTITAP_MODE_PAD_SUPPORT_CHECK     0x12
#define MULTITAP_MODE_MEMCARD_SUPPORT_CHECK 0x13
#define MULTITAP_MODE_SELECT_PAD            0x21
#define MULTITAP_MODE_SELECT_MEMCARD        0x22

void multitap_send(void);

#ifdef __cplusplus
}
#endif
