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

#include <ctime>
#include <memory>
#include <string>
#include <optional>
#include <vector>


struct Pcsx2Config;
namespace Host
{
	/* The frontend's core options, as the config they set, and the three
	 * values beside it that no Pcsx2Config field holds. */
	const Pcsx2Config& OptionConfig();
	const char*        OptionMemcardPath();
	// Base setting retrieval, bypasses layers.

	// Allows the emucore to write settings back to the frontend. Use with care.
	
	// Settings access, thread-safe.

	/// The core options as the config they set; see OptionConfig() below.

	namespace Internal
	{
		/// Retrieves the base settings layer. Must call with lock held.

		/// Sets the base settings layer. Should be called by the host at initialization time.

	} // namespace Internal
	/// Reads a file from the resources directory of the application.
	/// This may be outside of the "normal" filesystem on platforms such as Mac.
	std::optional<std::vector<u8>> ReadResourceFile(const char* filename);
} // namespace Host
