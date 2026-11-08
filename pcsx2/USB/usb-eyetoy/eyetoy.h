/* The EyeToy and Capture Eye cameras, as the USB front end reaches them:
 * C linkage, so the C front end can create and save them. */
#pragma once

#include "USB/libretro-usb/qusb.h"
#include "USB/libretro-usb/usb-state.h"

#ifdef __cplusplus
extern "C" {
#endif

enum
{
	USB_EYETOY_MODEL_EYETOY = 0, /* videodev.h TYPE_EYETOY */
	USB_EYETOY_MODEL_OV511P      /* videodev.h TYPE_OV511P */
};

USBDevice* usb_eyetoy_create(int model);

/* Saves or loads the device's own state, beyond what every USB device has. */
void usb_eyetoy_freeze(USBDevice* dev, UsbState* st);

#ifdef __cplusplus
}
#endif
