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

#pragma once

#include <optional>
#include <variant>
#include <utility>
#include <string>
#include <vector>

#include "../../common/Pcsx2Types.h"

#include "../Config.h"
#include "InputBinding.h"

namespace InputManager
{
	/// Retrieves bindings that match the generic bindings for the specified device.
	using GenericInputBindingMapping = std::vector<std::pair<GenericInputBinding, std::string>>;

	/// Internal method used by pads to dispatch vibration updates to input sources.
	/// Intensity is normalized from 0 to 1.
	void SetPadVibrationIntensity(u32 pad_index, float large_or_single_motor_intensity, float small_motor_intensity);
} // namespace InputManager
