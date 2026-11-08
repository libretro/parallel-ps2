/* A Konami Python 1 game descriptor (.py1): an INI file whose [Game]
 * section names the game's storage image (an HDD or a CF card), the I/O
 * board's own files (its boot ROM, config ROM, battery-backed RAM and
 * dongles), the memory card the game keeps in slot 1 and the board's I/O
 * mode. Paths in it are taken from the descriptor's own directory. C89. */
#ifndef PY1DESC_H
#define PY1DESC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PY1_PATH 1024

/* How the board's JAMMA and serial lines are wired, game by game. */
enum
{
	P1IO_MODE_JVS = 0,  /* the plain JAMMA panel */
	P1IO_MODE_EXTIO,    /* Dance Dance Revolution's foot panels */
	P1IO_MODE_POPN,     /* pop'n music's nine buttons */
	P1IO_MODE_PPOOL,    /* Perfect Pool */
	P1IO_MODE_B22,      /* Keyboardmania's B22 serial board */
	P1IO_MODE_DOGSTATIONDX
};

typedef struct py1_desc
{
	char name[128];
	char game_id[32];
	char hdd_image[PY1_PATH];
	char cf_image[PY1_PATH];
	char bbsram[PY1_PATH];
	char bootrom[PY1_PATH];
	char config_rom[PY1_PATH];
	char dongle[2][PY1_PATH]; /* internal, external */
	char memcard[PY1_PATH];
	char memcard_id[PY1_PATH];
	int  io_mode;
} py1_desc;

/* Parses text (len bytes, not terminated) with relative paths joined to
 * dir. Returns 0 when there is no [Game] section or it names neither an
 * HddImagePath nor a CfImagePath. */
int py1_parse(const char* text, size_t len, const char* dir, py1_desc* out);

/* The name of an I/O mode, as a descriptor spells it. */
const char* py1_io_mode_name(int mode);

#ifdef __cplusplus
}
#endif

#endif
