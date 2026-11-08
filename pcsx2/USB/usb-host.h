/* What the USB controller and its devices need from the rest of the
 * core, with C linkage so the C translation units reach the C++ ones. */
#pragma once

#include "common/Pcsx2Types.h"
#include <libretro.h>

#ifdef __cplusplus
extern "C" {
#endif

/* IOP RAM, which the OHCI controller reads its lists and buffers from. */
#define USB_IOP_RAM_SIZE (2 * 1024 * 1024)
u8* usb_iop_ram(void); /* IopMem.cpp */

/* Raises the controller's IOP interrupt, cycles from now. IopIrq.cpp */
void usbIrq(int cycles);

/* The frontend's input_state callback. PAD.cpp */
retro_input_state_t PADGetInputStateCallback(void);

/* The controller's clock, in IOP cycles. USB.c */
s64 usb_get_clock(void);
int usb_get_ticks_per_second(void);

#ifdef __cplusplus
}
#endif
