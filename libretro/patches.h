#ifndef LRPS2_PATCHES_H
#define LRPS2_PATCHES_H

#include "common/Pcsx2Defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Loads the core's built-in patches for serial under the given hints;
 * fastcdvd is the pcsx2_fastcdvd option, which some patches need. 1 when
 * the frontend's AV info has to be resent, else 0. */
int lrps2_ingame_patches(const char *serial,
		u32 game_crc,
		const char *renderer,
		bool   hint_nointerlacing,
		bool   hint_disable_mipmaps,
		bool   hint_game_enhancements,
		int8_t hint_widescreen,
		int8_t hint_uncapped_framerate,
		int8_t hint_language_unlock,
		bool   fastcdvd);

#ifdef __cplusplus
}
#endif

#endif
