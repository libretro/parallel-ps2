/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2021  PCSX2 Dev Team
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

#include "USB/libretro-usb/qusb.h"
#include "USB/libretro-usb/hid.h"
#include "USB/libretro-usb/usb-state.h"
#include "libretro.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Plain factories - no DeviceProxy or config lookup. Build a boot-
 * protocol HID keyboard or mouse on the given OHCI port and return the
 * USBDevice to attach. */
USBDevice* usb_hid_create_kbd(u32 port);
USBDevice* usb_hid_create_mouse(u32 port);

/* Pump one frame of libretro keyboard/mouse input into the device. */
void usb_hid_update(USBDevice* dev, retro_input_state_t input_cb, unsigned port);

/* Saves or loads the device's own state, beyond what every USB device has. */
void usb_hid_freeze(USBDevice* dev, UsbState* st);

#ifdef __cplusplus
}
#endif
