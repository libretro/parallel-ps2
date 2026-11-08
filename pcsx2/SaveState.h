/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2021  PCSX2 Dev Team
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

#include <stddef.h> /* size_t */

#include "FreezeTypes.h"

/* Savestate Versioning!
 *
 * NOTICE: When updating g_SaveVersion, please make sure you add the following
 * line to your commit message somewhere:
 * [SAVEVERSION+] */

#define g_SaveVersion ((u32)((0x9A59 << 16) | 0x0000))

/* --------------------------------------------------------------------------
 *  SaveStateBase
 * --------------------------------------------------------------------------
 * Reading and writing a savestate both run through this. is_saving picks the
 * direction, and the buffer grows on demand while saving, so the owner hands
 * over an allocation and takes back whatever it became.
 *
 * Members are ordered widest first so the struct packs without padding
 * between them. */

typedef struct SaveStateBase
{
	u8    *memory;       /* the block being written to or read from */
	size_t memory_size;  /* bytes live in it */
	size_t memory_cap;   /* bytes allocated */
	int    idx;          /* read/write cursor */
	char   tagspace[32];
	bool   error;        /* something went wrong reading or writing */
	bool   is_saving;    /* direction: saving when set, loading when not */
	bool   fixed;        /* saving into memory the writer does not own:
	                      * running out of it is an error, not a realloc */
} SaveStateBase;

#ifdef __cplusplus
extern "C" {
#endif

/* Point one at a block. While saving, memory may be NULL and cap 0; it is
 * grown as needed and the caller reads memory and memory_size back out
 * afterwards. While loading, the block has to hold the whole state. */
void SaveState_Init(SaveStateBase *s, u8 *memory, size_t size, size_t cap,
                    bool is_saving);

/* Loads or saves a block of memory, in whichever direction Init picked. */
void SaveState_FreezeMem(SaveStateBase *s, void *data, int size);

/* Make sure size bytes are available at the cursor. */
void SaveState_PrepBlock(SaveStateBase *s, int size);

/* An identifier, for working out where a state went skew: if the tag that
 * comes back is not the one written, the damage is somewhere before it. */
bool SaveState_FreezeTag(SaveStateBase *s, const char *src);

/* Loading only: whether the next thing in the state is the given tag. The
 * cursor does not move and a miss is not an error, so a block added to the
 * format later can be skipped when an older state does not carry it. */
bool SaveState_PeekTag(const SaveStateBase *s, const char *tag);

bool SaveState_FreezeBios(SaveStateBase *s);
bool SaveState_FreezeInternals(SaveStateBase *s);

/* Load or save one object. Usable on scalars, structs and arrays; for a
 * pointer to memory the object does not contain, use SaveState_FreezeMem. */
#define SaveState_Freeze(s, obj) \
	SaveState_FreezeMem((s), (void *)&(obj), (int)sizeof(obj))

#define SaveState_IsOkay(s)    (!(s)->error)
#define SaveState_IsSaving(s)  ((s)->is_saving)
#define SaveState_IsLoading(s) (!(s)->is_saving)
#define SaveState_BlockPtr(s)  (&(s)->memory[(s)->idx])

/* How much of the block is still ahead of the cursor. A freeze function
 * that reads its own length out of the state it is loading has no other
 * ceiling to check that length against: however large the count says the
 * queue was, it cannot have held more bytes than are left to read. */
#define SaveState_BytesLeft(s) \
	((s)->idx < (s)->memory_size ? ((s)->memory_size - (s)->idx) : (size_t)0)
#define SaveState_CommitBlock(s, n) ((s)->idx += (n))

/* Load/Save for the various components of our glorious emulator. gsFreeze is
 * reached from the GSState recorder as well as from here. */
bool gsFreeze(SaveStateBase *s);
bool mtvuFreeze(SaveStateBase *s);
bool rcntFreeze(SaveStateBase *s);
bool vuMicroFreeze(SaveStateBase *s);
bool vuJITFreeze(SaveStateBase *s);
bool vif0Freeze(SaveStateBase *s);
bool vif1Freeze(SaveStateBase *s);
bool sifFreeze(SaveStateBase *s);
bool ipuFreeze(SaveStateBase *s);
bool ipuDmaFreeze(SaveStateBase *s);
bool gifFreeze(SaveStateBase *s);
bool gifDmaFreeze(SaveStateBase *s);
bool gifPathFreeze(SaveStateBase *s, u32 path); /* called by gifFreeze */
bool sprFreeze(SaveStateBase *s);
bool sioFreeze(SaveStateBase *s);
bool cdrFreeze(SaveStateBase *s);
bool cdvdFreeze(SaveStateBase *s);
bool psxRcntFreeze(SaveStateBase *s);
bool sio2Freeze(SaveStateBase *s);
bool deci2Freeze(SaveStateBase *s);

#ifdef __cplusplus
}
#endif
