/* The Konami Python 1's I/O board (P1IO), an i.LINK node: the IOP's
 * subboard driver writes commands to it and it answers with writes of
 * its own. It holds the game's storage (an HDD image, or a CF card's one
 * file), its boot ROM, battery-backed RAM and Dallas dongles, its serial
 * peripherals, the JAMMA panel and an ADPCM player mixed into the
 * console's audio.
 *
 * After Reliquary's KonamiPython1 device (DiscoStarslayer and
 * contributors), with its buffers of fixed size and its sound decoded as
 * it plays. C89. */
#ifndef P1IO_H
#define P1IO_H

#include <libretro.h>

#include "common/Pcsx2Types.h"
#include "fw_device.h"
#include "py1desc.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct p1io p1io;

/* Where the board's panel and keyboard are read from, when it reads
 * them: the frontend's input callback of the moment. */
typedef retro_input_state_t (*p1io_input_source)(void);

/* The board the descriptor describes, its files read in (a boot ROM or
 * battery-backed RAM file that does not exist is made). NULL when out of
 * memory. */
p1io* p1io_create(const py1_desc* d, p1io_input_source input);
/* Writes back what changed and lets go of the images. */
void p1io_destroy(p1io* io);
fw_device* p1io_device(p1io* io);

/* For the SIO2, on the second port: the memory card slot the board's
 * latch selects when the board is a serial one (B22, Dog Station DX),
 * which answers only in slot 0; -1 otherwise, or with no board. */
int p1io_memcard_slot(void);

const struct retro_input_descriptor* p1io_input_descriptors(int io_mode);

#ifdef __cplusplus
}
#endif

#endif
