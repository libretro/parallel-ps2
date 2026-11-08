/* Konami Python 1 game descriptors. See py1desc.h. */
#include "py1desc.h"

#include <string.h>

static int lower(int c)
{
	return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
}

/* Whether the n bytes at s spell word, case aside. */
static int same(const char* s, size_t n, const char* word)
{
	size_t i;
	if (strlen(word) != n)
		return 0;
	for (i = 0; i < n; i++)
		if (lower((unsigned char)s[i]) != lower((unsigned char)word[i]))
			return 0;
	return 1;
}

static int is_space(int c)
{
	return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static void copy_value(char* dst, size_t cap, const char* v, size_t n)
{
	if (n >= cap)
		n = cap - 1;
	memcpy(dst, v, n);
	dst[n] = '\0';
}

static int is_absolute(const char* v, size_t n)
{
	return (n >= 1 && (v[0] == '/' || v[0] == '\\'))
		|| (n >= 2 && v[1] == ':' && ((v[0] >= 'a' && v[0] <= 'z') || (v[0] >= 'A' && v[0] <= 'Z')));
}

static void copy_path(char* dst, const char* dir, const char* v, size_t n)
{
	size_t d;
	if (!n || is_absolute(v, n) || !dir || !dir[0])
	{
		if (n >= PY1_PATH)
			n = 0; /* not the file it names, nor part of one */
		copy_value(dst, PY1_PATH, v, n);
		return;
	}
	d = strlen(dir);
	if (d + 1 + n >= PY1_PATH)
	{
		dst[0] = '\0';
		return;
	}
	memcpy(dst, dir, d);
	if (dir[d - 1] != '/' && dir[d - 1] != '\\')
		dst[d++] = '/';
	memcpy(dst + d, v, n);
	dst[d + n] = '\0';
}

static const char* const s_modes[] = { "JVS", "EXTIO", "POPN", "PPOOL", "B22", "DOGSTATIONDX" };

const char* py1_io_mode_name(int mode)
{
	return mode >= 0 && mode < (int)(sizeof(s_modes) / sizeof(s_modes[0])) ? s_modes[mode] : s_modes[0];
}

static int parse_mode(const char* v, size_t n)
{
	int i;
	for (i = 0; i < (int)(sizeof(s_modes) / sizeof(s_modes[0])); i++)
		if (same(v, n, s_modes[i]))
			return i;
	return P1IO_MODE_JVS;
}

int py1_parse(const char* text, size_t len, const char* dir, py1_desc* out)
{
	size_t pos = 0;
	int in_game = 0, have_game = 0;

	memset(out, 0, sizeof(*out));
	while (pos < len)
	{
		const char* line = text + pos;
		size_t n = 0, k, ks, ke, vs, ve;

		while (pos + n < len && text[pos + n] != '\n')
			n++;
		pos += n + 1;

		ks = 0;
		while (ks < n && is_space((unsigned char)line[ks]))
			ks++;
		ve = n;
		while (ve > ks && is_space((unsigned char)line[ve - 1]))
			ve--;
		if (ks == ve || line[ks] == ';' || line[ks] == '#')
			continue;

		if (line[ks] == '[')
		{
			k = ks + 1;
			while (k < ve && line[k] != ']')
				k++;
			in_game = k < ve && same(line + ks + 1, k - ks - 1, "Game");
			have_game |= in_game;
			continue;
		}
		if (!in_game)
			continue;

		for (k = ks; k < ve && line[k] != '='; k++)
			;
		if (k == ve)
			continue;
		ke = k;
		while (ke > ks && is_space((unsigned char)line[ke - 1]))
			ke--;
		vs = k + 1;
		while (vs < ve && is_space((unsigned char)line[vs]))
			vs++;
		/* A quoted value is the text between the quotes. */
		if (ve - vs >= 2 && line[vs] == '"' && line[ve - 1] == '"')
		{
			vs++;
			ve--;
		}

#define KEY(word) same(line + ks, ke - ks, word)
#define VAL line + vs, ve - vs
		if (KEY("Name"))
			copy_value(out->name, sizeof(out->name), VAL);
		else if (KEY("GameId"))
			copy_value(out->game_id, sizeof(out->game_id), VAL);
		else if (KEY("HddImagePath"))
			copy_path(out->hdd_image, dir, VAL);
		else if (KEY("CfImagePath"))
			copy_path(out->cf_image, dir, VAL);
		else if (KEY("BbsRamPath"))
			copy_path(out->bbsram, dir, VAL);
		else if (KEY("IoBootRomPath"))
			copy_path(out->bootrom, dir, VAL);
		else if (KEY("IoConfigRomPath"))
			copy_path(out->config_rom, dir, VAL);
		else if (KEY("InternalDonglePath"))
			copy_path(out->dongle[0], dir, VAL);
		else if (KEY("ExternalDonglePath"))
			copy_path(out->dongle[1], dir, VAL);
		else if (KEY("MemoryCardDonglePath"))
			copy_path(out->memcard, dir, VAL);
		else if (KEY("MemoryCardIdPath"))
			copy_path(out->memcard_id, dir, VAL);
		else if (KEY("IoMode"))
			out->io_mode = parse_mode(VAL);
#undef KEY
#undef VAL
	}
	return have_game && (out->hdd_image[0] || out->cf_image[0]);
}
