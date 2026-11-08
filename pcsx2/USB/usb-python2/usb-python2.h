/* The Konami Python 2 I/O board as a USB device: p2io.c behind the
 * board's descriptors and endpoints. */
#pragma once

#include "USB/libretro-usb/qusb.h"
#include "USB/libretro-usb/usb-state.h"
#include "p2io.h"

#include <libretro.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The board the next create makes; set from the .py2 descriptor, cleared
 * when other content is loaded. */
void usb_python2_set_config(const p2io_config* cfg);
bool usb_python2_has_config(void);
void usb_python2_clear_config(void);
/* The configured board's game type (P2IO_GAME_*). */
int usb_python2_game(void);

USBDevice* usb_python2_create(void);

/* One frame of the frontend's controls: RetroPads 1 and 2 are the
 * cabinet's players, the keyboard their card reader keypads. */
void usb_python2_update(USBDevice* dev, retro_input_state_t input_cb);

/* Saves or loads the board's own state, beyond what every USB device
 * has. */
void usb_python2_freeze(USBDevice* dev, UsbState* st);

/* The RetroPad mapping of a game type, for the frontend's input
 * descriptors; terminated by a NULL description. */
const struct retro_input_descriptor* usb_python2_input_descriptors(int game);

#ifdef __cplusplus
}
#endif
