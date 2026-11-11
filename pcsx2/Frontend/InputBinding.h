/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2023  PCSX2 Dev Team
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

/* What a pad's inputs bind to, and the generic controller layout used to
 * bind them automatically. */

#ifndef PCSX2_INPUTBINDING_H
#define PCSX2_INPUTBINDING_H

#include "../../common/Pcsx2Types.h"

enum class GenericInputBinding : u8;

struct InputBindingInfo
{
	enum class Type : u8
	{
		Unknown,
		Button,
		Axis,
		HalfAxis,
		Motor,
		Pointer, // Receive relative mouse movement events, bind_index is offset by the axis.
		Keyboard, // Receive host key events, bind_index is offset by the key code.
		Device, // Used for special-purpose device selection, e.g. force feedback.
		Macro,
	};

	const char* name;
	const char* display_name;
	Type bind_type;
	u16 bind_index;
	GenericInputBinding generic_mapping;
};

/// Generic input bindings. These roughly match a DualShock 4 or XBox One controller.
/// They are used for automatic binding to PS2 controller types.
enum class GenericInputBinding : u8
{
	Unknown,

	DPadUp,
	DPadRight,
	DPadLeft,
	DPadDown,

	LeftStickUp,
	LeftStickRight,
	LeftStickDown,
	LeftStickLeft,
	L3,

	RightStickUp,
	RightStickRight,
	RightStickDown,
	RightStickLeft,
	R3,

	Triangle, // Y on XBox pads.
	Circle, // B on XBox pads.
	Cross, // A on XBox pads.
	Square, // X on XBox pads.

	Select, // Share on DS4, View on XBox pads.
	Start, // Options on DS4, Menu on XBox pads.
	System, // PS button on DS4, Guide button on XBox pads.

	L1, // LB on Xbox pads.
	L2, // Left trigger on XBox pads.
	R1, // RB on XBox pads.
	R2, // Right trigger on Xbox pads.

	SmallMotor, // High frequency vibration.
	LargeMotor, // Low frequency vibration.

	Count,
};

#endif
