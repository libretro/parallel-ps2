/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2020  PCSX2 Dev Team
 *
 *  PCSX2 is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU Lesser General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  PCSX2 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with PCSX2.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdlib.h>
#include <string.h>
#include "ParseNumber.h"
#include "common/Pcsx2Defs.h"
#include <optional>


#include <formats/ryaml.h>
#include <rthreads/rthreads.h>

#include "HostFS.h"

#include "GameDatabase.h"
#include "GS/GS.h"
#include "Host.h"
#include "vtlb.h"

namespace GameDatabaseSchema
{
	static const char* getHWFixName(GSHWFixId id);
	static bool parseHWFixName(const char* name, size_t len, GSHWFixId* out);
	static bool isUserHackHWFix(GSHWFixId id);
} // namespace GameDatabaseSchema

static constexpr char GAMEDB_YAML_FILE_NAME[] = "GameIndex.yaml";

static bool s_load_once_done = false;

/* The database while it is read: each entry's lists are runs in pools
 * shared by every entry, named by offset because a pool moves when it
 * grows. Once the file is read the pools stay put, and the entries are
 * made from these with pointers into them. */
struct gamedb_build
{
	u32 serial, name, region, memcard; /* offsets into s_chars */
	u32 fix_first, nfix;
	u32 hack_first, nhack;
	u32 hw_first, nhw;
	u32 patch_first, npatch;
	u32 dyna_first, ndyna;
	FPRoundMode eeRoundMode, eeDivRoundMode, vu0RoundMode, vu1RoundMode;
	GameDatabaseSchema::ClampMode eeClampMode, vu0ClampMode, vu1ClampMode;
};

struct gamedb_patch_build
{
	u32 crc;
	u32 text; /* offset into s_chars */
};

struct gamedb_dyna_build
{
	u32 first; /* into s_dyna_entries: the pattern, then the replacement */
	u32 npattern, nreplacement;
};

/* A growable array: items, how many, room for how many. */
#define GAMEDB_POOL(type, name) static type* name; static u32 name##_count, name##_cap

GAMEDB_POOL(char, s_chars);
GAMEDB_POOL(struct gamedb_build, s_builds);
GAMEDB_POOL(GamefixId, s_fixes);
GAMEDB_POOL(GameDatabaseSpeedHack, s_hacks);
GAMEDB_POOL(GameDatabaseHWFix, s_hwfixes);
GAMEDB_POOL(struct gamedb_patch_build, s_patch_builds);
GAMEDB_POOL(struct gamedb_dyna_build, s_dyna_builds);
GAMEDB_POOL(DynamicPatchEntry, s_dyna_entries);

/* What the loaded database is read through: the entries, the patch and
 * dynamic-patch records they point at, and an open-addressed index of
 * entry numbers (plus one; 0 is empty) by serial. */
static GameDatabaseEntry* s_entries;
static u32 s_entry_count;
static GameDatabasePatch* s_patches;
static DynamicPatch* s_dynas;
static u32* s_index;
static u32 s_index_mask;

/* Room in a pool for need items of elem bytes; false when it cannot be
 * made, leaving the pool as it was. */
static bool gamedb_reserve(void** items, u32* cap, u32 need, size_t elem)
{
	u32 grown = *cap ? *cap : 64;
	void* p;
	if (need <= *cap)
		return true;
	while (grown < need)
		grown *= 2;
	p = realloc(*items, (size_t)grown * elem);
	if (!p)
		return false;
	*items = p;
	*cap   = grown;
	return true;
}

#define GAMEDB_PUSH(pool, value) \
	(gamedb_reserve((void**)&pool, &pool##_cap, pool##_count + 1, sizeof(*pool)) \
		? (pool[pool##_count++] = (value), true) : false)

/* A run of text into s_chars, terminated; its offset, or 0 (an empty
 * string: the pool starts with one) when it could not be stored. */
static u32 gamedb_chars(const char* s, size_t len)
{
	u32 at;
	if (!len || !gamedb_reserve((void**)&s_chars, &s_chars_cap, s_chars_count + (u32)len + 1, 1))
		return 0;
	at = s_chars_count;
	memcpy(s_chars + at, s, len);
	s_chars[at + len] = '\0';
	s_chars_count += (u32)len + 1;
	return at;
}

static void gamedb_free_all(void)
{
	free(s_chars);          s_chars = NULL;          s_chars_count = s_chars_cap = 0;
	free(s_builds);         s_builds = NULL;         s_builds_count = s_builds_cap = 0;
	free(s_fixes);          s_fixes = NULL;          s_fixes_count = s_fixes_cap = 0;
	free(s_hacks);          s_hacks = NULL;          s_hacks_count = s_hacks_cap = 0;
	free(s_hwfixes);        s_hwfixes = NULL;        s_hwfixes_count = s_hwfixes_cap = 0;
	free(s_patch_builds);   s_patch_builds = NULL;   s_patch_builds_count = s_patch_builds_cap = 0;
	free(s_dyna_builds);    s_dyna_builds = NULL;    s_dyna_builds_count = s_dyna_builds_cap = 0;
	free(s_dyna_entries);   s_dyna_entries = NULL;   s_dyna_entries_count = s_dyna_entries_cap = 0;
	free(s_entries);        s_entries = NULL;        s_entry_count = 0;
	free(s_patches);        s_patches = NULL;
	free(s_dynas);          s_dynas = NULL;
	free(s_index);          s_index = NULL;          s_index_mask = 0;
}

/* Serials are keyed lower-case: the database's keys are written either
 * way and callers pass either. ASCII only, as they are. */
static u32 gamedb_hash(const char* s, size_t len)
{
	u32 h = 2166136261u;
	size_t i;
	for (i = 0; i < len; i++)
	{
		char c = s[i];
		if (c >= 'A' && c <= 'Z')
			c = (char)(c - 'A' + 'a');
		h = (h ^ (u8)c) * 16777619u;
	}
	return h;
}

/* Whether a stored lower-case serial is the run s[0..len) in any case. */
static bool gamedb_serial_is(const char* stored, const char* s, size_t len)
{
	return strlen(stored) == len && Strncasecmp(stored, s, len) == 0;
}

/* The slot for a serial in the index: its entry's, or the empty one
 * where it would go. The index is never more than half full. */
static u32* gamedb_slot(const char* s, size_t len, const char* (*serial_of)(u32))
{
	u32 i = gamedb_hash(s, len) & s_index_mask;
	while (s_index[i] && !gamedb_serial_is(serial_of(s_index[i] - 1), s, len))
		i = (i + 1) & s_index_mask;
	return &s_index[i];
}

/* An indexed serial, while the file is read and once it has been. */
static const char* gamedb_build_serial(u32 n)
{
	return s_chars + s_builds[n].serial;
}

static const char* gamedb_entry_serial(u32 n)
{
	return s_entries[n].serial;
}

/* Doubles the index, rehashing what is in it. */
static bool gamedb_grow_index(void)
{
	const u32 old_size = s_index ? s_index_mask + 1 : 0;
	const u32 size     = old_size ? old_size * 2 : 4096;
	u32* old           = s_index;
	u32 i;

	s_index = (u32*)calloc(size, sizeof(u32));
	if (!s_index)
	{
		s_index = old;
		return false;
	}
	s_index_mask = size - 1;
	for (i = 0; i < old_size; i++)
	{
		if (old[i])
		{
			const char* serial = gamedb_build_serial(old[i] - 1);
			*gamedb_slot(serial, strlen(serial), gamedb_build_serial) = old[i];
		}
	}
	free(old);
	return true;
}

/* Small adapters over the C reader. A node handle of RYAML_NONE is inert,
 * so a lookup that missed can be passed straight on without a test at
 * each step. */
static u32 yamlString(const ryaml_t* yaml, int node)
{
	size_t len = 0;
	const char* str = ryaml_val(yaml, node, &len);
	return str ? gamedb_chars(str, len) : 0;
}

static const char* yamlKey(const ryaml_t* yaml, int node, size_t* len)
{
	const char* str = ryaml_key(yaml, node, len);
	if (!str)
	{
		*len = 0;
		return "";
	}
	return str;
}

static const char* yamlVal(const ryaml_t* yaml, int node, size_t* len)
{
	const char* str = ryaml_val(yaml, node, len);
	if (!str)
	{
		*len = 0;
		return "";
	}
	return str;
}

static int yamlInt(const ryaml_t* yaml, int node, int def)
{
	int value = def;
	ryaml_val_int(yaml, node, &value);
	return value;
}

static void yamlRoundMode(const ryaml_t* yaml, int roundModes, const char* key, FPRoundMode* a, FPRoundMode* b)
{
	const int v = yamlInt(yaml, ryaml_find_child(yaml, roundModes, key), -1);
	if (v >= 0 && v < static_cast<int>(FPRoundMode::MaxCount))
	{
		*a = static_cast<FPRoundMode>(v);
		if (b)
			*b = *a;
	}
}

static void yamlClampMode(const ryaml_t* yaml, int clampModes, const char* key,
	GameDatabaseSchema::ClampMode* a, GameDatabaseSchema::ClampMode* b)
{
	if (ryaml_has_child(yaml, clampModes, key))
	{
		*a = static_cast<GameDatabaseSchema::ClampMode>(
			yamlInt(yaml, ryaml_find_child(yaml, clampModes, key), -1));
		if (b)
			*b = *a;
	}
}

static void gamedb_parse(struct gamedb_build* e, const char* serial, const ryaml_t* yaml, int node)
{
	int n;

	e->name   = yamlString(yaml, ryaml_find_child(yaml, node, "name"));
	e->region = yamlString(yaml, ryaml_find_child(yaml, node, "region"));
	{
		const int roundModes = ryaml_find_child(yaml, node, "roundModes");
		yamlRoundMode(yaml, roundModes, "eeRoundMode", &e->eeRoundMode, NULL);
		yamlRoundMode(yaml, roundModes, "eeDivRoundMode", &e->eeDivRoundMode, NULL);
		yamlRoundMode(yaml, roundModes, "vuRoundMode", &e->vu0RoundMode, &e->vu1RoundMode);
		yamlRoundMode(yaml, roundModes, "vu0RoundMode", &e->vu0RoundMode, NULL);
		yamlRoundMode(yaml, roundModes, "vu1RoundMode", &e->vu1RoundMode, NULL);
	}
	{
		const int clampModes = ryaml_find_child(yaml, node, "clampModes");
		yamlClampMode(yaml, clampModes, "eeClampMode", &e->eeClampMode, NULL);
		yamlClampMode(yaml, clampModes, "vuClampMode", &e->vu0ClampMode, &e->vu1ClampMode);
		yamlClampMode(yaml, clampModes, "vu0ClampMode", &e->vu0ClampMode, NULL);
		yamlClampMode(yaml, clampModes, "vu1ClampMode", &e->vu1ClampMode, NULL);
	}

	// Validate game fixes, invalid ones will be dropped!
	e->fix_first = s_fixes_count;
	for (n = ryaml_first_child(yaml, ryaml_find_child(yaml, node, "gameFixes")); n >= 0;
		n = ryaml_next_sibling(yaml, n))
	{
		bool fixValidated = false;
		size_t len;
		const char* fix = yamlVal(yaml, n, &len);

		// Enum values don't end with Hack, but gamedb does, so remove it before comparing.
		if (len >= 4 && memcmp(fix + len - 4, "Hack", 4) == 0)
		{
			const size_t base = len - 4;
			len = base; /* the name without it, as reported below */
			for (GamefixId id = GamefixId_FIRST; id < pxEnumEnd; ++id)
			{
				const char* name = EnumToString(id);
				if (strlen(name) == base && memcmp(name, fix, base) == 0)
				{
					u32 fi;
					for (fi = 0; fi < e->nfix; fi++)
					{
						if (s_fixes[e->fix_first + fi] == id)
							break;
					}
					if (fi == e->nfix && GAMEDB_PUSH(s_fixes, id))
					{
						e->nfix++;
						fixValidated = true;
					}
					break;
				}
			}
		}

		if (!fixValidated)
			log_cb(RETRO_LOG_ERROR, "[GameDB] Invalid gamefix: '{%.*s}', specified for serial: '{%s}'. Dropping!\n", (int)len, fix, serial);
	}

	e->hack_first = s_hacks_count;
	for (n = ryaml_first_child(yaml, ryaml_find_child(yaml, node, "speedHacks")); n >= 0;
		n = ryaml_next_sibling(yaml, n))
	{
		size_t id_len, value_len;
		const char* id_name = yamlKey(yaml, n, &id_len);
		const char* value   = yamlVal(yaml, n, &value_len);
		SpeedHack id;
		s32 parsed = 0;
		bool inserted = false;

		if (Pcsx2Config::SpeedhackOptions::ParseSpeedHackName(id_name, id_len, &id)
			&& parse_number_s32(value, value_len, 10, &parsed))
		{
			u32 hi;
			for (hi = 0; hi < e->nhack; hi++)
			{
				if (s_hacks[e->hack_first + hi].id == id)
					break;
			}
			if (hi == e->nhack)
			{
				GameDatabaseSpeedHack hack;
				hack.id    = id;
				hack.value = parsed;
				if (GAMEDB_PUSH(s_hacks, hack))
				{
					e->nhack++;
					inserted = true;
				}
			}
		}
		if (!inserted)
			log_cb(RETRO_LOG_ERROR, "[GameDB] Invalid speedhack: '{%.*s}', specified for serial: '{%.*s}'. Dropping!\n", (int)id_len, id_name, (int)value_len, value);
	}

	e->hw_first = s_hwfixes_count;
	for (n = ryaml_first_child(yaml, ryaml_find_child(yaml, node, "gsHWFixes")); n >= 0;
		n = ryaml_next_sibling(yaml, n))
	{
		size_t name_len;
		const char* id_name = yamlKey(yaml, n, &name_len);
		GameDatabaseSchema::GSHWFixId id;
		const bool have_id = GameDatabaseSchema::parseHWFixName(id_name, name_len, &id);
		bool have_value    = false;
		s32 value          = 0;

		if (have_id && (id == GameDatabaseSchema::GSHWFixId::GetSkipCount ||
						   id == GameDatabaseSchema::GSHWFixId::BeforeDraw ||
						   id == GameDatabaseSchema::GSHWFixId::MoveHandler))
		{
			size_t len;
			const char* str_value = yamlVal(yaml, n, &len);
			if (id == GameDatabaseSchema::GSHWFixId::GetSkipCount)
				value = GSLookupGetSkipCountFunctionId(str_value, len);
			else if (id == GameDatabaseSchema::GSHWFixId::BeforeDraw)
				value = GSLookupBeforeDrawFunctionId(str_value, len);
			else
				value = GSLookupMoveHandlerFunctionId(str_value, len);

			if (value < 0)
			{
				log_cb(RETRO_LOG_ERROR, "[GameDB] Invalid GS HW Fix Value for '{%.*s}' in '{%s}': '{%.*s}'\n", (int)name_len, id_name, serial, (int)len, str_value);
				continue;
			}
			have_value = true;
		}
		else if (!ryaml_has_val(yaml, n))
		{
			value      = 1;
			have_value = true;
		}
		else
		{
			size_t len;
			const char* str_value = yamlVal(yaml, n, &len);
			have_value = parse_number_s32(str_value, len, 10, &value) != 0;
		}

		if (!have_id || !have_value)
		{
			log_cb(RETRO_LOG_ERROR, "[GameDB] Invalid GS HW Fix: '{%.*s}' specified for serial '{%s}'. Dropping!\n", (int)name_len, id_name, serial);
			continue;
		}

		{
			GameDatabaseHWFix fix;
			fix.id    = id;
			fix.value = value;
			if (GAMEDB_PUSH(s_hwfixes, fix))
				e->nhw++;
		}
	}

	// Memory Card Filters - currently they are used as a '/' delimited string in the app
	{
		const u32 start = s_chars_count;
		u32 count       = 0;
		for (n = ryaml_first_child(yaml, ryaml_find_child(yaml, node, "memcardFilters")); n >= 0;
			n = ryaml_next_sibling(yaml, n))
		{
			size_t len;
			const char* filter = yamlVal(yaml, n, &len);
			if (!gamedb_reserve((void**)&s_chars, &s_chars_cap, s_chars_count + (u32)len + 2, 1))
				break;
			if (count++)
				s_chars[s_chars_count++] = '/';
			memcpy(s_chars + s_chars_count, filter, len);
			s_chars_count += (u32)len;
		}
		if (count)
		{
			s_chars[s_chars_count++] = '\0';
			e->memcard = start;
		}
	}

	// Game Patches
	e->patch_first = s_patch_builds_count;
	for (n = ryaml_first_child(yaml, ryaml_find_child(yaml, node, "patches")); n >= 0;
		n = ryaml_next_sibling(yaml, n))
	{
		// use a crc of 0 for default patches
		size_t len;
		const char* crc_str = yamlKey(yaml, n, &len);
		u32 crc = 0;
		u32 pi;
		struct gamedb_patch_build patch;

		if (!(len == 7 && Strncasecmp(crc_str, "default", 7) == 0)
			&& !parse_number_u32(crc_str, len, 16, &crc))
		{
			log_cb(RETRO_LOG_ERROR, "[GameDB] Invalid CRC '{%.*s}' found for serial: '{%s}'. Skipping!\n", (int)len, crc_str, serial);
			continue;
		}
		for (pi = 0; pi < e->npatch; pi++)
		{
			if (s_patch_builds[e->patch_first + pi].crc == crc)
				break;
		}
		if (pi != e->npatch)
		{
			log_cb(RETRO_LOG_ERROR, "[GameDB] Duplicate CRC '{%.*s}' found for serial: '{%s}'. Skipping, CRCs are case-insensitive!\n", (int)len, crc_str, serial);
			continue;
		}

		patch.crc  = crc;
		patch.text = yamlString(yaml, ryaml_find_child(yaml, n, "content"));
		if (GAMEDB_PUSH(s_patch_builds, patch))
			e->npatch++;
	}

	e->dyna_first = s_dyna_builds_count;
	for (n = ryaml_first_child(yaml, ryaml_find_child(yaml, node, "dynaPatches")); n >= 0;
		n = ryaml_next_sibling(yaml, n))
	{
		struct gamedb_dyna_build dyna;
		const int pattern = ryaml_find_child(yaml, n, "pattern");

		dyna.first        = s_dyna_entries_count;
		dyna.npattern     = 0;
		dyna.nreplacement = 0;
		if (ryaml_has_children(yaml, pattern))
		{
			const int replacement = ryaml_find_child(yaml, n, "replacement");
			int x;

			for (x = ryaml_first_child(yaml, pattern); x >= 0; x = ryaml_next_sibling(yaml, x))
			{
				DynamicPatchEntry entry = {};
				ryaml_val_uint(yaml, ryaml_find_child(yaml, x, "offset"), &entry.offset);
				ryaml_val_uint(yaml, ryaml_find_child(yaml, x, "value"), &entry.value);
				if (GAMEDB_PUSH(s_dyna_entries, entry))
					dyna.npattern++;
			}
			for (x = ryaml_first_child(yaml, replacement); x >= 0; x = ryaml_next_sibling(yaml, x))
			{
				DynamicPatchEntry entry = {};
				ryaml_val_uint(yaml, ryaml_find_child(yaml, x, "offset"), &entry.offset);
				ryaml_val_uint(yaml, ryaml_find_child(yaml, x, "value"), &entry.value);
				if (GAMEDB_PUSH(s_dyna_entries, entry))
					dyna.nreplacement++;
			}
		}
		if (GAMEDB_PUSH(s_dyna_builds, dyna))
			e->ndyna++;
	}
}

/* The entries out of the build records, now that the pools are final. */
static bool gamedb_finish(void)
{
	u32 i;

	s_entries = (GameDatabaseEntry*)calloc(s_builds_count ? s_builds_count : 1, sizeof(*s_entries));
	s_patches = (GameDatabasePatch*)calloc(s_patch_builds_count ? s_patch_builds_count : 1, sizeof(*s_patches));
	s_dynas   = (DynamicPatch*)calloc(s_dyna_builds_count ? s_dyna_builds_count : 1, sizeof(*s_dynas));
	if (!s_entries || !s_patches || !s_dynas)
		return false;

	for (i = 0; i < s_patch_builds_count; i++)
	{
		s_patches[i].crc  = s_patch_builds[i].crc;
		s_patches[i].text = s_chars + s_patch_builds[i].text;
	}
	for (i = 0; i < s_dyna_builds_count; i++)
	{
		const struct gamedb_dyna_build* d = &s_dyna_builds[i];
		s_dynas[i].pattern      = s_dyna_entries + d->first;
		s_dynas[i].npattern     = d->npattern;
		s_dynas[i].replacement  = s_dyna_entries + d->first + d->npattern;
		s_dynas[i].nreplacement = d->nreplacement;
	}
	for (i = 0; i < s_builds_count; i++)
	{
		const struct gamedb_build* b = &s_builds[i];
		GameDatabaseEntry* e         = &s_entries[i];
		e->serial         = s_chars + b->serial;
		e->name           = s_chars + b->name;
		e->region         = s_chars + b->region;
		e->eeRoundMode    = b->eeRoundMode;
		e->eeDivRoundMode = b->eeDivRoundMode;
		e->vu0RoundMode   = b->vu0RoundMode;
		e->vu1RoundMode   = b->vu1RoundMode;
		e->eeClampMode    = b->eeClampMode;
		e->vu0ClampMode   = b->vu0ClampMode;
		e->vu1ClampMode   = b->vu1ClampMode;
		e->gameFixes      = s_fixes + b->fix_first;
		e->nGameFixes     = b->nfix;
		e->speedHacks     = s_hacks + b->hack_first;
		e->nSpeedHacks    = b->nhack;
		e->gsHWFixes      = s_hwfixes + b->hw_first;
		e->nGSHWFixes     = b->nhw;
		e->memcardFilters = s_chars + b->memcard;
		e->patches        = s_patches + b->patch_first;
		e->nPatches       = b->npatch;
		e->dynaPatches    = s_dynas + b->dyna_first;
		e->nDynaPatches   = b->ndyna;
	}
	s_entry_count = s_builds_count;

	/* The build records and patch offsets are not read again. */
	free(s_builds);       s_builds = NULL;       s_builds_count = s_builds_cap = 0;
	free(s_patch_builds); s_patch_builds = NULL; s_patch_builds_count = s_patch_builds_cap = 0;
	free(s_dyna_builds);  s_dyna_builds = NULL;  s_dyna_builds_count = s_dyna_builds_cap = 0;
	return true;
}

static const char* s_round_modes[static_cast<u32>(FPRoundMode::MaxCount)] = {
	"Nearest",
	"NegativeInfinity",
	"PositiveInfinity",
	"Chop"
};

static const char* s_gs_hw_fix_names[] = {
	"cpuFramebufferConversion",
	"readTCOnClose",
	"disableDepthSupport",
	"preloadFrameData",
	"disablePartialInvalidation",
	"textureInsideRT",
	"alignSprite",
	"mergeSprite",
	"mipmap",
	"forceEvenSpritePosition",
	"nativePaletteDraw",
	"estimateTextureRegion",
	"PCRTCOffsets",
	"PCRTCOverscan",
	"trilinearFiltering",
	"skipDrawStart",
	"skipDrawEnd",
	"halfBottomOverride",
	"halfPixelOffset",
	"roundSprite",
	"nativeScaling",
	"texturePreloading",
	"deinterlace",
	"cpuSpriteRenderBW",
	"cpuSpriteRenderLevel",
	"cpuCLUTRender",
	"gpuPaletteConversion",
	"minimumBlendingLevel",
	"maximumBlendingLevel",
	"recommendedBlendingLevel",
	"getSkipCount",
	"beforeDraw",
	"moveHandler",
	"limit24BitDepth",
};

const char* GameDatabaseSchema::getHWFixName(GSHWFixId id)
{
	return s_gs_hw_fix_names[static_cast<u32>(id)];
}

static bool GameDatabaseSchema::parseHWFixName(const char* name, size_t len, GSHWFixId* out)
{
	for (u32 i = 0; i < C89_ARRAY_SIZE(s_gs_hw_fix_names); i++)
	{
		if (strlen(s_gs_hw_fix_names[i]) == len && memcmp(s_gs_hw_fix_names[i], name, len) == 0)
		{
			*out = static_cast<GameDatabaseSchema::GSHWFixId>(i);
			return true;
		}
	}

	return false;
}

bool GameDatabaseSchema::isUserHackHWFix(GSHWFixId id)
{
	switch (id)
	{
		case GameDatabaseSchema::GSHWFixId::Deinterlace:
		case GameDatabaseSchema::GSHWFixId::Mipmap:
		case GameDatabaseSchema::GSHWFixId::TexturePreloading:
		case GameDatabaseSchema::GSHWFixId::TrilinearFiltering:
		case GameDatabaseSchema::GSHWFixId::MinimumBlendingLevel:
		case GameDatabaseSchema::GSHWFixId::MaximumBlendingLevel:
		case GameDatabaseSchema::GSHWFixId::RecommendedBlendingLevel:
		case GameDatabaseSchema::GSHWFixId::PCRTCOffsets:
		case GameDatabaseSchema::GSHWFixId::PCRTCOverscan:
		case GameDatabaseSchema::GSHWFixId::GetSkipCount:
		case GameDatabaseSchema::GSHWFixId::BeforeDraw:
		case GameDatabaseSchema::GSHWFixId::MoveHandler:
			return false;
		default:
			break;
	}
	return true;
}

u32 GameDatabase_ApplyGameFixes(const GameDatabaseEntry* e, Pcsx2Config& config, bool applyAuto)
{
	u32 i;
	// Only apply core game fixes if the user has enabled them.
	if (!applyAuto)
		log_cb(RETRO_LOG_WARN, "[GameDB] Game Fixes are disabled\n");

	u32 num_applied_fixes = 0;

	if (e->eeDivRoundMode < FPRoundMode::MaxCount)
	{
		if (applyAuto)
		{
			log_cb(RETRO_LOG_INFO, "(GameDB) Changing EE/FPU divison roundmode to %d [%s]\n", (int)e->eeDivRoundMode, s_round_modes[static_cast<u8>(e->eeDivRoundMode)]);
			config.Cpu.FPUDivFPCR.SetRoundMode(e->eeDivRoundMode);
		}
		else
		{
			log_cb(RETRO_LOG_WARN, "[GameDB] Skipping changing EE/FPU divison roundmode to %d [%s]\n", (int)e->eeDivRoundMode, s_round_modes[static_cast<u8>(e->eeDivRoundMode)]);
		}
	}

	if (e->eeRoundMode < FPRoundMode::MaxCount)
	{
		if (applyAuto)
		{
			log_cb(RETRO_LOG_INFO, "(GameDB) Changing EE/FPU roundmode to %d [%s]\n", (int)e->eeRoundMode, s_round_modes[static_cast<u8>(e->eeRoundMode)]);
			config.Cpu.FPUFPCR.SetRoundMode(e->eeRoundMode);
		}
		else
			log_cb(RETRO_LOG_WARN, "[GameDB] Skipping changing EE/FPU roundmode to %d [%s]\n", (int)e->eeRoundMode, s_round_modes[static_cast<u8>(e->eeRoundMode)]);
	}

	if (e->vu0RoundMode < FPRoundMode::MaxCount)
	{
		if (applyAuto)
		{
			log_cb(RETRO_LOG_INFO, "(GameDB) Changing VU0 roundmode to %d [%s]\n", (int)e->vu0RoundMode, s_round_modes[static_cast<u8>(e->vu0RoundMode)]);
			config.Cpu.VU0FPCR.SetRoundMode(e->vu0RoundMode);
		}
		else
			log_cb(RETRO_LOG_WARN, "[GameDB] Skipping changing VU0 roundmode to %d [%s]\n", (int)e->vu0RoundMode, s_round_modes[static_cast<u8>(e->vu0RoundMode)]);
	}

	if (e->vu1RoundMode < FPRoundMode::MaxCount)
	{
		if (applyAuto)
		{
			log_cb(RETRO_LOG_INFO, "(GameDB) Changing VU1 roundmode to %d [%s]\n", (int)e->vu1RoundMode, s_round_modes[static_cast<u8>(e->vu1RoundMode)]);
			config.Cpu.VU1FPCR.SetRoundMode(e->vu1RoundMode);
		}
		else
			log_cb(RETRO_LOG_WARN, "[GameDB] Skipping changing VU1 roundmode to %d [%s]\n", (int)e->vu1RoundMode, s_round_modes[static_cast<u8>(e->vu1RoundMode)]);
	}

	if (e->eeClampMode != GameDatabaseSchema::ClampMode::Undefined)
	{
		const int clampMode = enum_cast(e->eeClampMode);
		if (applyAuto)
		{
			log_cb(RETRO_LOG_INFO, "(GameDB) Changing EE/FPU clamp mode [mode=%d]\n", clampMode);
			config.Cpu.Recompiler.fpuOverflow = (clampMode >= 1);
			config.Cpu.Recompiler.fpuExtraOverflow = (clampMode >= 2);
			config.Cpu.Recompiler.fpuFullMode = (clampMode >= 3);
			num_applied_fixes++;
		}
		else
			log_cb(RETRO_LOG_WARN, "[GameDB] Skipping changing EE/FPU clamp mode [mode=%d]\n", clampMode);
	}

	if (e->vu0ClampMode != GameDatabaseSchema::ClampMode::Undefined)
	{
		const int clampMode = enum_cast(e->vu0ClampMode);
		if (applyAuto)
		{
			log_cb(RETRO_LOG_INFO, "(GameDB) Changing VU0 clamp mode [mode=%d]\n", clampMode);
			config.Cpu.Recompiler.vu0Overflow = (clampMode >= 1);
			config.Cpu.Recompiler.vu0ExtraOverflow = (clampMode >= 2);
			config.Cpu.Recompiler.vu0SignOverflow = (clampMode >= 3);
			num_applied_fixes++;
		}
		else
			log_cb(RETRO_LOG_WARN, "[GameDB] Skipping changing VU0 clamp mode [mode=%d]\n", clampMode);
	}

	if (e->vu1ClampMode != GameDatabaseSchema::ClampMode::Undefined)
	{
		const int clampMode = enum_cast(e->vu1ClampMode);
		if (applyAuto)
		{
			log_cb(RETRO_LOG_INFO, "(GameDB) Changing VU1 clamp mode [mode=%d]\n", clampMode);
			config.Cpu.Recompiler.vu1Overflow = (clampMode >= 1);
			config.Cpu.Recompiler.vu1ExtraOverflow = (clampMode >= 2);
			config.Cpu.Recompiler.vu1SignOverflow = (clampMode >= 3);
			num_applied_fixes++;
		}
		else
			log_cb(RETRO_LOG_WARN, "[GameDB] Skipping changing VU1 clamp mode [mode=%d]\n", clampMode);
	}

	// TODO - config - this could be simplified with maps instead of bitfields and enums
	for (i = 0; i < e->nSpeedHacks; i++)
	{
		const GameDatabaseSpeedHack* it = &e->speedHacks[i];
		if (!applyAuto)
		{
			log_cb(RETRO_LOG_WARN, "[GameDB] Skipping setting Speedhack '%s' to [mode=%d]\n",
					Pcsx2Config::SpeedhackOptions::GetSpeedHackName(it->id), it->value);
			continue;
		}
		// Legacy note - speedhacks are setup in the GameDB as integer values, but
		// are effectively booleans like the gamefixes
		config.Speedhacks.Set(it->id, it->value);
		log_cb(RETRO_LOG_INFO, "(GameDB) Setting Speedhack '%s' to [mode=%d]\n",
				Pcsx2Config::SpeedhackOptions::GetSpeedHackName(it->id), it->value);
		num_applied_fixes++;
	}

	// TODO - config - this could be simplified with maps instead of bitfields and enums
	for (i = 0; i < e->nGameFixes; i++)
	{
		const GamefixId id = e->gameFixes[i];
		if (!applyAuto)
		{
			log_cb(RETRO_LOG_WARN, "[GameDB] Skipping Gamefix: %s\n", EnumToString(id));
			continue;
		}
		// if the fix is present, it is said to be enabled
		config.Gamefixes.Set(id, true);
		log_cb(RETRO_LOG_INFO, "(GameDB) Enabled Gamefix: %s\n", EnumToString(id));
		num_applied_fixes++;

		// The LUT is only used for 1 game so we allocate it only when the gamefix is enabled (save 4MB)
		if (id == Fix_GoemonTlbMiss && true)
			vtlb_Alloc_Ppmap();
	}

	return num_applied_fixes;
}

static bool configMatchesHWFix(const Pcsx2Config::GSOptions& config, GameDatabaseSchema::GSHWFixId id, int value)
{
	switch (id)
	{
		case GameDatabaseSchema::GSHWFixId::CPUFramebufferConversion:
			return (static_cast<int>(config.UserHacks_CPUFBConversion) == value);

		case GameDatabaseSchema::GSHWFixId::FlushTCOnClose:
			return (static_cast<int>(config.UserHacks_ReadTCOnClose) == value);

		case GameDatabaseSchema::GSHWFixId::DisableDepthSupport:
			return (static_cast<int>(config.UserHacks_DisableDepthSupport) == value);

		case GameDatabaseSchema::GSHWFixId::PreloadFrameData:
			return (static_cast<int>(config.PreloadFrameWithGSData) == value);

		case GameDatabaseSchema::GSHWFixId::DisablePartialInvalidation:
			return (static_cast<int>(config.UserHacks_DisablePartialInvalidation) == value);

		case GameDatabaseSchema::GSHWFixId::TextureInsideRT:
			return (static_cast<int>(config.UserHacks_TextureInsideRt) == value);

		case GameDatabaseSchema::GSHWFixId::AlignSprite:
			return (config.UpscaleMultiplier <= 1.0f || static_cast<int>(config.UserHacks_AlignSpriteX) == value);

		case GameDatabaseSchema::GSHWFixId::MergeSprite:
			return (config.UpscaleMultiplier <= 1.0f || static_cast<int>(config.UserHacks_MergePPSprite) == value);

		case GameDatabaseSchema::GSHWFixId::ForceEvenSpritePosition:
			return (config.UpscaleMultiplier <= 1.0f || static_cast<int>(config.UserHacks_ForceEvenSpritePosition) == value);

		case GameDatabaseSchema::GSHWFixId::NativePaletteDraw:
			return (config.UpscaleMultiplier <= 1.0f || static_cast<int>(config.UserHacks_NativePaletteDraw) == value);

		case GameDatabaseSchema::GSHWFixId::EstimateTextureRegion:
			return (static_cast<int>(config.UserHacks_EstimateTextureRegion) == value);

		case GameDatabaseSchema::GSHWFixId::PCRTCOffsets:
			return (static_cast<int>(config.PCRTCOffsets) == value);

		case GameDatabaseSchema::GSHWFixId::PCRTCOverscan:
			return (static_cast<int>(config.PCRTCOverscan) == value);

		case GameDatabaseSchema::GSHWFixId::Mipmap:
			return (static_cast<int>(config.HWMipmapMode) == value);

		case GameDatabaseSchema::GSHWFixId::Limit24BitDepth:
			return (static_cast<int>(config.UserHacks_Limit24BitDepth) == value);

		case GameDatabaseSchema::GSHWFixId::TrilinearFiltering:
			return (config.TriFilter == TriFiltering::Automatic || static_cast<int>(config.TriFilter) == value);

		case GameDatabaseSchema::GSHWFixId::SkipDrawStart:
			return (config.SkipDrawStart == value);

		case GameDatabaseSchema::GSHWFixId::SkipDrawEnd:
			return (config.SkipDrawEnd == value);

		case GameDatabaseSchema::GSHWFixId::HalfPixelOffset:
			return (config.UpscaleMultiplier <= 1.0f || config.UserHacks_HalfPixelOffset == static_cast<GSHalfPixelOffset>(value));

		case GameDatabaseSchema::GSHWFixId::RoundSprite:
			return (config.UpscaleMultiplier <= 1.0f || config.UserHacks_RoundSprite == value);

		case GameDatabaseSchema::GSHWFixId::NativeScaling:
			return (static_cast<int>(config.UserHacks_NativeScaling) == value);

		case GameDatabaseSchema::GSHWFixId::TexturePreloading:
			return (static_cast<int>(config.TexturePreloading) <= value);

		case GameDatabaseSchema::GSHWFixId::Deinterlace:
			return (config.InterlaceMode == GSInterlaceMode::Automatic || static_cast<int>(config.InterlaceMode) == value);

		case GameDatabaseSchema::GSHWFixId::CPUSpriteRenderBW:
			return (config.UserHacks_CPUSpriteRenderBW == value);

		case GameDatabaseSchema::GSHWFixId::CPUSpriteRenderLevel:
			return (config.UserHacks_CPUSpriteRenderLevel == value);

		case GameDatabaseSchema::GSHWFixId::CPUCLUTRender:
			return (config.UserHacks_CPUCLUTRender == value);

		case GameDatabaseSchema::GSHWFixId::GPUPaletteConversion:
			return (config.GPUPaletteConversion == ((value > 1) ? (config.TexturePreloading == TexturePreloadingLevel::Full) : (value != 0)));

		case GameDatabaseSchema::GSHWFixId::MinimumBlendingLevel:
			return (static_cast<int>(config.AccurateBlendingUnit) >= value);

		case GameDatabaseSchema::GSHWFixId::MaximumBlendingLevel:
			return (static_cast<int>(config.AccurateBlendingUnit) <= value);

		case GameDatabaseSchema::GSHWFixId::RecommendedBlendingLevel:
			return true;

		case GameDatabaseSchema::GSHWFixId::GetSkipCount:
			return (static_cast<int>(config.GetSkipCountFunctionId) == value);

		case GameDatabaseSchema::GSHWFixId::BeforeDraw:
			return (static_cast<int>(config.BeforeDrawFunctionId) == value);
		case GameDatabaseSchema::GSHWFixId::MoveHandler:
			return (static_cast<int>(config.MoveHandlerFunctionId) == value);

		default:
			return false;
	}
}

u32 GameDatabase_ApplyGSHardwareFixes(const GameDatabaseEntry* e, Pcsx2Config::GSOptions& config)
{
	u32 i;

	// Only apply GS HW fixes if the user hasn't manually enabled HW fixes.
	const bool apply_auto_fixes = !config.ManualUserHacks;
	if (!apply_auto_fixes)
		log_cb(RETRO_LOG_WARN, "[GameDB] Manual GS hardware renderer fixes are enabled, not using automatic hardware renderer fixes from GameDB.\n");

	u32 num_applied_fixes = 0;
	for (i = 0; i < e->nGSHWFixes; i++)
	{
		const GameDatabaseSchema::GSHWFixId id = e->gsHWFixes[i].id;
		const s32 value = e->gsHWFixes[i].value;
		if (GameDatabaseSchema::isUserHackHWFix(id) && !apply_auto_fixes)
		{
			if (configMatchesHWFix(config, id, value))
				continue;

			log_cb(RETRO_LOG_WARN, "[GameDB] Skipping GS Hardware Fix: %s to [mode=%d]\n", GameDatabaseSchema::getHWFixName(id), value);
			continue;
		}

		switch (id)
		{
			case GameDatabaseSchema::GSHWFixId::CPUFramebufferConversion:
				config.UserHacks_CPUFBConversion = (value > 0);
				break;

			case GameDatabaseSchema::GSHWFixId::FlushTCOnClose:
				config.UserHacks_ReadTCOnClose = (value > 0);
				break;

			case GameDatabaseSchema::GSHWFixId::DisableDepthSupport:
				config.UserHacks_DisableDepthSupport = (value > 0);
				break;

			case GameDatabaseSchema::GSHWFixId::PreloadFrameData:
				config.PreloadFrameWithGSData = (value > 0);
				break;

			case GameDatabaseSchema::GSHWFixId::DisablePartialInvalidation:
				config.UserHacks_DisablePartialInvalidation = (value > 0);
				break;

			case GameDatabaseSchema::GSHWFixId::TextureInsideRT:
			{
				if (value >= 0 && value <= static_cast<int>(GSTextureInRtMode::MergeTargets))
					config.UserHacks_TextureInsideRt = static_cast<GSTextureInRtMode>(value);
			}
			break;

			case GameDatabaseSchema::GSHWFixId::AlignSprite:
				config.UserHacks_AlignSpriteX = (value > 0);
				break;

			case GameDatabaseSchema::GSHWFixId::MergeSprite:
				config.UserHacks_MergePPSprite = (value > 0);
				break;

			case GameDatabaseSchema::GSHWFixId::ForceEvenSpritePosition:
				config.UserHacks_ForceEvenSpritePosition = (value > 0);
				break;

			case GameDatabaseSchema::GSHWFixId::NativePaletteDraw:
				config.UserHacks_NativePaletteDraw = (value > 0);
				break;

			case GameDatabaseSchema::GSHWFixId::EstimateTextureRegion:
				config.UserHacks_EstimateTextureRegion = (value > 0);
				break;

			case GameDatabaseSchema::GSHWFixId::PCRTCOffsets:
				config.PCRTCOffsets = (value > 0);
				break;

			case GameDatabaseSchema::GSHWFixId::PCRTCOverscan:
				config.PCRTCOverscan = (value > 0);
				break;

			case GameDatabaseSchema::GSHWFixId::Mipmap:
				if (value >= 0 && value < static_cast<int>(GSHWMipmapMode::MaxCount))
				{
					config.HWMipmapMode = static_cast<GSHWMipmapMode>(value > 0);
					// Mipmapping is split across HWMipmapMode and the Mipmap master
					// switch (consumed by GSState::UpdateSettings into m_mipmap). A
					// database mipmap fix must drive both, or the renderer-level
					// gate keeps LOD sampling disabled regardless of the mode.
					config.Mipmap = (value > 0);
				}
				break;

			case GameDatabaseSchema::GSHWFixId::Limit24BitDepth:
				if (value >= 0 && value <= static_cast<int>(GSLimit24BitDepth::PrioritizeLower))
					config.UserHacks_Limit24BitDepth = static_cast<GSLimit24BitDepth>(value);
				break;

			case GameDatabaseSchema::GSHWFixId::TrilinearFiltering:
			{
				if (value >= 0 && value <= static_cast<int>(TriFiltering::Forced))
				{
					if (config.TriFilter == TriFiltering::Automatic)
						config.TriFilter = static_cast<TriFiltering>(value);
					else if (config.TriFilter > TriFiltering::Off)
						log_cb(RETRO_LOG_WARN, "[GameDB] Game requires trilinear filtering to be disabled.\n");
				}
			}
			break;

			case GameDatabaseSchema::GSHWFixId::SkipDrawStart:
				config.SkipDrawStart = value;
				break;

			case GameDatabaseSchema::GSHWFixId::SkipDrawEnd:
				config.SkipDrawEnd = value;
				break;

			case GameDatabaseSchema::GSHWFixId::HalfPixelOffset:
				if (value >= 0 && value < static_cast<int>(GSHalfPixelOffset::MaxCount))
					config.UserHacks_HalfPixelOffset = static_cast<GSHalfPixelOffset>(value);
				break;

			case GameDatabaseSchema::GSHWFixId::RoundSprite:
				config.UserHacks_RoundSprite = value;
				break;

			case GameDatabaseSchema::GSHWFixId::NativeScaling:
				/* The user's own setting of the option stands over the fix. */
				if (!config.NativeScalingSet)
					config.UserHacks_NativeScaling = static_cast<GSNativeScaling>(value);
				break;

			case GameDatabaseSchema::GSHWFixId::TexturePreloading:
			{
				if (value >= 0 && value <= static_cast<int>(TexturePreloadingLevel::Full))
					config.TexturePreloading = (TexturePreloadingLevel)pcsx2_min_i((int)config.TexturePreloading, value);
			}
			break;

			case GameDatabaseSchema::GSHWFixId::Deinterlace:
			{
				if (value >= static_cast<int>(GSInterlaceMode::Automatic) && value < static_cast<int>(GSInterlaceMode::Count))
				{
					if (config.InterlaceMode == GSInterlaceMode::Automatic)
						config.InterlaceMode = static_cast<GSInterlaceMode>(value);
					else
						log_cb(RETRO_LOG_WARN, "[GameDB] Game requires different deinterlace mode but it has been overridden by user setting.\n");
				}
			}
			break;

			case GameDatabaseSchema::GSHWFixId::CPUSpriteRenderBW:
				config.UserHacks_CPUSpriteRenderBW = value;
				break;

			case GameDatabaseSchema::GSHWFixId::CPUSpriteRenderLevel:
				config.UserHacks_CPUSpriteRenderLevel = value;
				break;

			case GameDatabaseSchema::GSHWFixId::CPUCLUTRender:
				config.UserHacks_CPUCLUTRender = value;
				break;

			case GameDatabaseSchema::GSHWFixId::GPUPaletteConversion:
			{
				// if 2, enable paltex when preloading is full, otherwise leave as-is
				if (value > 1)
					config.GPUPaletteConversion = (config.TexturePreloading == TexturePreloadingLevel::Full) ? true : config.GPUPaletteConversion;
				else
					config.GPUPaletteConversion = (value != 0);
			}
			break;

			case GameDatabaseSchema::GSHWFixId::MinimumBlendingLevel:
			{
				if (value >= 0 && value <= static_cast<int>(AccBlendLevel::Maximum))
					config.AccurateBlendingUnit = (AccBlendLevel)pcsx2_max_i((int)config.AccurateBlendingUnit, value);
			}
			break;

			case GameDatabaseSchema::GSHWFixId::MaximumBlendingLevel:
			{
				if (value >= 0 && value <= static_cast<int>(AccBlendLevel::Maximum))
					config.AccurateBlendingUnit = (AccBlendLevel)pcsx2_min_i((int)config.AccurateBlendingUnit, value);
			}
			break;

			case GameDatabaseSchema::GSHWFixId::RecommendedBlendingLevel:
				break;

			case GameDatabaseSchema::GSHWFixId::GetSkipCount:
				config.GetSkipCountFunctionId = static_cast<s16>(value);
				break;

			case GameDatabaseSchema::GSHWFixId::BeforeDraw:
				config.BeforeDrawFunctionId = static_cast<s16>(value);
				break;

			case GameDatabaseSchema::GSHWFixId::MoveHandler:
				config.MoveHandlerFunctionId = static_cast<s16>(value);
				break;

			default:
				break;
		}

		log_cb(RETRO_LOG_INFO, "[GameDB] Enabled GS Hardware Fix: %s to [mode=%d]\n", GameDatabaseSchema::getHWFixName(id), value);
		num_applied_fixes++;
	}

	// fixup skipdraw range just in case the db has a bad range (but the linter should catch this)
	config.SkipDrawEnd = pcsx2_max_i(config.SkipDrawStart, config.SkipDrawEnd);

	return num_applied_fixes;
}
static void gamedb_init(void)
{
	auto buf = Host::ReadResourceFileToString(GAMEDB_YAML_FILE_NAME);
	if (!buf.has_value())
	{
		log_cb(RETRO_LOG_ERROR, "[GameDB] Unable to open GameDB file, file does not exist.\n");
		return;
	}

	/* The reader borrows these bytes rather than copying them, so the
	 * buffer has to outlive the tree; it does, both are scoped here. */
	size_t err_line = 0;
	size_t err_col = 0;
	ryaml_t* yaml = ryaml_parse_ex(buf.value().data(), buf.value().size(),
		&err_line, &err_col);
	if (!yaml)
	{
		log_cb(RETRO_LOG_ERROR, "[GameDB] Parsing error at {%zu}:{%zu}\n", err_line, err_col);
		return;
	}

	/* offset 0 is the empty string every missing text points at */
	if (!GAMEDB_PUSH(s_chars, '\0'))
	{
		ryaml_free(yaml);
		return;
	}

	const int root = ryaml_root(yaml);

	for (int n = ryaml_first_child(yaml, root); n >= 0;
		n = ryaml_next_sibling(yaml, n))
	{
		size_t len;
		const char* key = yamlKey(yaml, n, &len);
		struct gamedb_build build;
		u32* slot;
		u32 i;

		if ((s_builds_count + 1) * 2 > (s_index ? s_index_mask + 1 : 0) && !gamedb_grow_index())
			break;

		// Serials must be inserted as lower-case, as that is how they are retrieved
		// this is because the application may pass a lowercase serial along
		//
		// However, YAML's keys are as expected case-sensitive, so we have to explicitly do our own duplicate checking
		slot = gamedb_slot(key, len, gamedb_build_serial);
		if (*slot)
		{
			log_cb(RETRO_LOG_ERROR, "[GameDB] Duplicate serial '{%s}' found in GameDB. Skipping, Serials are case-insensitive!\n", gamedb_build_serial(*slot - 1));
			continue;
		}
		if (!ryaml_is_map(yaml, n))
			continue;

		memset(&build, 0, sizeof(build));
		build.eeRoundMode    = FPRoundMode::MaxCount;
		build.eeDivRoundMode = FPRoundMode::MaxCount;
		build.vu0RoundMode   = FPRoundMode::MaxCount;
		build.vu1RoundMode   = FPRoundMode::MaxCount;
		build.eeClampMode    = GameDatabaseSchema::ClampMode::Undefined;
		build.vu0ClampMode   = GameDatabaseSchema::ClampMode::Undefined;
		build.vu1ClampMode   = GameDatabaseSchema::ClampMode::Undefined;
		build.serial         = gamedb_chars(key, len);
		for (i = 0; i < len; i++)
		{
			char* c = s_chars + build.serial + i;
			if (*c >= 'A' && *c <= 'Z')
				*c = (char)(*c - 'A' + 'a');
		}

		gamedb_parse(&build, s_chars + build.serial, yaml, n);
		if (!GAMEDB_PUSH(s_builds, build))
			break;
		*gamedb_slot(key, len, gamedb_build_serial) = s_builds_count;
	}

	ryaml_free(yaml);

	if (!gamedb_finish())
	{
		log_cb(RETRO_LOG_ERROR, "[GameDB] Out of memory loading the GameDB.\n");
		gamedb_free_all();
	}
}

void GameDatabase_Load(void)
{
	if (s_load_once_done)
		return;
	log_cb(RETRO_LOG_INFO, "[GameDB] Has not been initialized yet, initializing...\n");
	gamedb_init();
	log_cb(RETRO_LOG_INFO, "[GameDB] %u games on record\n", s_entry_count);
	s_load_once_done = true;
}

void GameDatabase_Unload(void)
{
	/* The parse is 12806 entries and ~50 ms of work, and every consumer of
	 * it - gamefixes, patches, the memcard and disc-swap paths - is a
	 * VM-lifetime event. With no VM there is nothing to look a game up
	 * for, so the cost is worth carrying only while content is loaded. */
	if (s_load_once_done)
	{
		gamedb_free_all();
		s_load_once_done = false;
	}
}

const GameDatabaseEntry* GameDatabase_FindGame(const char* serial)
{
	const size_t len = strlen(serial);
	u32 n;

	if (!s_index || !s_entries)
		return nullptr;

	n = *gamedb_slot(serial, len, gamedb_entry_serial);
	return n ? &s_entries[n - 1] : nullptr;
}

const char* GameDatabase_FindPatch(const GameDatabaseEntry* entry, u32 crc)
{
	u32 i;

	if (crc == 0)
		return nullptr;

	log_cb(RETRO_LOG_INFO, "[GameDB] Searching for patch with CRC '%08X'\n", crc);

	for (i = 0; i < entry->nPatches; i++)
	{
		if (entry->patches[i].crc == crc)
		{
			log_cb(RETRO_LOG_INFO, "[GameDB] Found patch with CRC '%08X'\n", crc);
			return entry->patches[i].text;
		}
	}

	for (i = 0; i < entry->nPatches; i++)
	{
		if (entry->patches[i].crc == 0)
		{
			log_cb(RETRO_LOG_INFO, "[GameDB] Found and falling back to default patch\n");
			return entry->patches[i].text;
		}
	}
	log_cb(RETRO_LOG_INFO, "[GameDB] No CRC-specific patch or default patch found\n");
	return nullptr;
}
