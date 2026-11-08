/* A Konami Python 2 game descriptor (.py2): an INI file whose [Game]
 * section names the game's HDD image, its identity files, its dongles and
 * card files, the I/O profile and the DIP switches. Paths in it are taken
 * from the descriptor's own directory. */
#ifndef PY2DESC_H
#define PY2DESC_H

#include <stddef.h>

#include "p2io.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PY2_PATH 1024

typedef struct py2_desc
{
	char name[128];
	char game_id[32];
	char hdd_image[PY2_PATH];
	char hdd_id[PY2_PATH];
	char nvram[PY2_PATH];
	char ilink_id[PY2_PATH];
	char dongle[2][PY2_PATH]; /* black, white */
	char card[2][PY2_PATH];
	char patch[PY2_PATH];
	int  input_type;
	unsigned char dipsw[4];
	unsigned char force_31khz;
} py2_desc;

/* Parses text (len bytes, not terminated) with relative paths joined to
 * dir. Returns 0 when there is no [Game] section or no HddImagePath. */
int py2_parse(const char* text, size_t len, const char* dir, py2_desc* out);

/* The I/O board the descriptor describes: its game type, DIP switches
 * and video mode, with the dongle and card files it names read in. A file
 * that is missing or does not check out leaves that dongle or card out;
 * missing gets a bit in the return value: 1 and 2 the black and white
 * dongles, 4 and 8 the player 1 and 2 cards. */
unsigned py2_make_config(const py2_desc* d, p2io_config* cfg);

#ifdef __cplusplus
}
#endif

#endif
