/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2010  PCSX2 Dev Team
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

#pragma once

#include "microVU_Types.h"

#include <string.h>
#include <stdlib.h>

typedef struct microBlockManager microBlockManager;

typedef struct microBlockLink
{
	microBlock block;
	struct microBlockLink* next;
} microBlockLink;

typedef struct microBlockLinkRef
{
	microBlock* pBlock;
	u64 quick;
} microBlockLinkRef;

typedef struct microRange
{
	s32 start; /* Start PC (The opcode the block starts at) */
	s32 end;   /* End PC   (The opcode the block ends with) */
} microRange;

/* POD flat-array lists used in place of std::deque. Both are accessed only at
 * compile / cache-invalidation time and hold a handful of entries, so a simple
 * dynamic array supporting prepend, mid-iteration erase and indexed access is
 * sufficient and avoids per-node heap allocation. Manipulated via the
 * mvu_*list_* free functions below. */
typedef struct microRangeList
{
	microRange* data;
	u32         count;
	u32         capacity;
} microRangeList;

typedef struct microProgramList
{
	microProgram** data;
	u32            count;
	u32            capacity;
} microProgramList;

#define mProgSize (0x4000 / 4)
struct microProgram
{
	u32                data [mProgSize];     /* Holds a copy of the VU microProgram */
	microBlockManager* block[mProgSize / 2]; /* Array of Block Managers */
	microRangeList*    ranges;               /* The ranges of the microProgram that have already been recompiled */
	u32 startPC; /* Start PC of this program */
	int idx;     /* Program index */
};

/* ---- microRangeList operations (flat POD array, prepend/erase/index) ---- */
static inline microRangeList* mvu_rangelist_new(void)
{
	microRangeList* l = (microRangeList*)malloc(sizeof(microRangeList));
	l->data     = NULL;
	l->count    = 0;
	l->capacity = 0;
	return l;
}

static inline void mvu_rangelist_delete(microRangeList* l)
{
	if (!l)
		return;
	free(l->data);
	free(l);
}

static inline void mvu_rangelist_reserve(microRangeList* l, u32 need)
{
	u32 cap;
	if (need <= l->capacity)
		return;
	cap = l->capacity ? l->capacity * 2 : 4;
	if (cap < need)
		cap = need;
	l->data     = (microRange*)realloc(l->data, cap * sizeof(microRange));
	l->capacity = cap;
}

/* Insert at the front (index 0), shifting existing entries up. */
static inline void mvu_rangelist_push_front(microRangeList* l, microRange r)
{
	mvu_rangelist_reserve(l, l->count + 1);
	memmove(&l->data[1], &l->data[0], l->count * sizeof(microRange));
	l->data[0] = r;
	l->count++;
}

/* Erase the entry at index i, shifting the tail down. Mirrors deque::erase:
 * the slot at i now holds what was the next entry, so the caller must NOT
 * advance its index when it erases. */
static inline void mvu_rangelist_erase(microRangeList* l, u32 i)
{
	memmove(&l->data[i], &l->data[i + 1], (l->count - i - 1) * sizeof(microRange));
	l->count--;
}

/* ---- microProgramList operations (flat POD array of microProgram*) ---- */
static inline microProgramList* mvu_proglist_new(void)
{
	microProgramList* l = (microProgramList*)malloc(sizeof(microProgramList));
	l->data     = NULL;
	l->count    = 0;
	l->capacity = 0;
	return l;
}

static inline void mvu_proglist_delete(microProgramList* l)
{
	if (!l)
		return;
	free(l->data);
	free(l);
}

static inline void mvu_proglist_clear(microProgramList* l)
{
	l->count = 0;
}

static inline void mvu_proglist_reserve(microProgramList* l, u32 need)
{
	u32 cap;
	if (need <= l->capacity)
		return;
	cap = l->capacity ? l->capacity * 2 : 4;
	if (cap < need)
		cap = need;
	l->data     = (microProgram**)realloc(l->data, cap * sizeof(microProgram*));
	l->capacity = cap;
}

static inline void mvu_proglist_push_front(microProgramList* l, microProgram* p)
{
	mvu_proglist_reserve(l, l->count + 1);
	memmove(&l->data[1], &l->data[0], l->count * sizeof(microProgram*));
	l->data[0] = p;
	l->count++;
}

static inline void mvu_proglist_erase(microProgramList* l, u32 i)
{
	memmove(&l->data[i], &l->data[i + 1], (l->count - i - 1) * sizeof(microProgram*));
	l->count--;
}

typedef struct microProgramQuick
{
	microBlockManager* block; /* Quick reference to valid microBlockManager for current startPC */
	microProgram*      prog;  /* The microProgram who is the owner of 'block' */
} microProgramQuick;

typedef struct microProgManager
{
	microIR            IRinfo;             /* IR information */
	microProgramList*  prog [mProgSize/2]; /* List of microPrograms indexed by startPC values */
	microProgramQuick  quick[mProgSize/2]; /* Quick reference to valid microPrograms for current execution */
	microProgram*      cur;                /* Pointer to currently running MicroProgram */
	int                total;              /* Total Number of valid MicroPrograms */
	int                isSame;             /* Current cached microProgram is Exact Same program as vuRegs[mVU->index].Micro (-1 = unknown, 0 = No, 1 = Yes) */
	int                cleared;            /* Micro Program is Indeterminate so must be searched for (and if no matches are found then recompile a new one) */
	u32                curFrame;           /* Frame Counter */
	u8*                codePtr;            /* Pointer to program's recompilation code */
	u8*                codeStart;          /* Start of program's rec-cache */
	u8*                codeEnd;            /* Limit of program's rec-cache */
	microRegInfo       lpState;            /* Pipeline state from where program left off (useful for continuing execution) */
} microProgManager;

#define mVUdispCacheSize ((uint)(__pagesize * 2)) /* Dispatcher Cache Size (in bytes; dispatchers + emitted helpers incl. the exact-multiply tree stub) */
#define mVUcacheSafeZone ((uint)(3)) /* Safe-Zone for program recompilation (in megabytes) */
#define mVUcacheReserve ((uint)(64)) /* mVU0, mVU1 Reserve Cache Size (in megabytes) */
