/* Perfect Pool's table camera, as far as the game needs one: a Cypress
 * EZ-USB FX the game downloads its 8051 firmware into and starts, which
 * then comes back on the bus as the camera. The camera's pictures are
 * not emulated: its reads return nothing seen.
 *
 * After Reliquary's perfect-pool-camera device. C89. */
#pragma once

#include "USB/libretro-usb/qusb.h"
#include "USB/libretro-usb/usb-state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The firmware RAM, which a savestate carries beside the port's slot. */
#define USB_PPCAM_RAM_SIZE 0x10000

USBDevice* usb_ppcam_create(void);

/* The device's state: its flags in the port's slot, its RAM in a block
 * of USB_PPCAM_RAM_SIZE of its own. */
void usb_ppcam_freeze(USBDevice* dev, UsbState* slot, UsbState* ram);

#ifdef __cplusplus
}
#endif
