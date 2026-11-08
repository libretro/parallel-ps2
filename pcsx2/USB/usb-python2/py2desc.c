/* Konami Python 2 game descriptors. See py2desc.h. */
#include "py2desc.h"

#include <stdlib.h>
#include <string.h>

#include <streams/file_stream.h>

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
		copy_value(dst, PY2_PATH, v, n);
		return;
	}
	d = strlen(dir);
	if (d + 1 + n >= PY2_PATH)
	{
		/* Too long to be the file it names: no file, not part of one. */
		dst[0] = '\0';
		return;
	}
	memcpy(dst, dir, d);
	if (dir[d - 1] != '/' && dir[d - 1] != '\\')
		dst[d++] = '/';
	memcpy(dst + d, v, n);
	dst[d + n] = '\0';
}

static int parse_bool(const char* v, size_t n)
{
	return same(v, n, "true") || same(v, n, "yes") || same(v, n, "on") || same(v, n, "1");
}

static int parse_int(const char* v, size_t n)
{
	char tmp[16];
	copy_value(tmp, sizeof(tmp), v, n);
	return atoi(tmp);
}

int py2_parse(const char* text, size_t len, const char* dir, py2_desc* out)
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

		/* Trimmed both ends. */
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

#define KEY(word) same(line + ks, ke - ks, word)
#define VAL line + vs, ve - vs
		if (KEY("Name"))
			copy_value(out->name, sizeof(out->name), VAL);
		else if (KEY("GameId"))
			copy_value(out->game_id, sizeof(out->game_id), VAL);
		else if (KEY("HddImagePath"))
			copy_path(out->hdd_image, dir, VAL);
		else if (KEY("HddIdPath"))
			copy_path(out->hdd_id, dir, VAL);
		else if (KEY("NvRamPath"))
			copy_path(out->nvram, dir, VAL);
		else if (KEY("IlinkIdPath"))
			copy_path(out->ilink_id, dir, VAL);
		else if (KEY("DongleBlackPath"))
			copy_path(out->dongle[0], dir, VAL);
		else if (KEY("DongleWhitePath"))
			copy_path(out->dongle[1], dir, VAL);
		else if (KEY("Player1Card"))
			copy_path(out->card[0], dir, VAL);
		else if (KEY("Player2Card"))
			copy_path(out->card[1], dir, VAL);
		else if (KEY("PatchFile"))
			copy_path(out->patch, dir, VAL);
		else if (KEY("InputType"))
			out->input_type = parse_int(VAL);
		else if (KEY("Force31kHz"))
			out->force_31khz = (unsigned char)parse_bool(VAL);
		else if (ke - ks == 6 && same(line + ks, 5, "DIPSW") && line[ks + 5] >= '1' && line[ks + 5] <= '4')
			out->dipsw[line[ks + 5] - '1'] = (unsigned char)parse_bool(VAL);
#undef KEY
#undef VAL
	}
	return have_game && out->hdd_image[0];
}

/* The file at path, whole; NULL when it cannot be read. */
static unsigned char* read_all(const char* path, int64_t* size)
{
	void* data = NULL;
	if (!path[0] || !filestream_read_file(path, &data, size) || *size < 0)
	{
		free(data);
		return NULL;
	}
	return (unsigned char*)data;
}

unsigned py2_make_config(const py2_desc* d, p2io_config* cfg)
{
	unsigned missing = 0, i;
	unsigned char* data;
	int64_t size;

	memset(cfg, 0, sizeof(*cfg));
	cfg->game = d->input_type;
	memcpy(cfg->dipsw, d->dipsw, sizeof(cfg->dipsw));
	cfg->force_31khz = d->force_31khz;

	for (i = 0; i < 2; i++)
	{
		if (d->dongle[i][0])
		{
			if ((data = read_all(d->dongle[i], &size)) && size >= 40)
				cfg->dongle_loaded[i] = (uint8_t)p2io_parse_dongle(data, cfg->dongle[i]);
			else
				missing |= 1u << i;
			free(data);
		}
		if (d->card[i][0])
		{
			if ((data = read_all(d->card[i], &size)))
				cfg->card_loaded[i] = (uint8_t)p2io_parse_card((const char*)data, (size_t)size, cfg->card[i]);
			else
				missing |= 4u << i;
			free(data);
		}
	}
	return missing;
}
