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

/* C++ keeps <cstring>: it declares std::memcpy and friends, which TUs
 * reaching this header have always been able to use. */
#ifdef __cplusplus
#include <cstring>
#else
#include <string.h>
#endif

#include "MemoryTypes.h"
#include "Dmac.h"

#define FIFO_SIF_W 128

/* Despite its name, this is actually the IOP's DMAtag, which itself also contains
 * the EE's DMAtag in its upper 64 bits.  Note that only the lower 24 bits of 'data' is
 * the IOP's chain transfer address (loaded into MADR).  Bits 30 and 31 are transfer stop
 * bits of some sort. */
typedef struct sifData
{
	s32 data;
	s32 words;

	tDMA_TAG	tag_lo;		/* EE DMA tag */
	tDMA_TAG	tag_hi;		/* EE DMA tag */
} sifData;

typedef struct sifFifo
{
	u32 data[FIFO_SIF_W];
	u32 junk[4];
	s32 readPos;
	s32 writePos;
	s32 size;

} sifFifo;

/* The fifo is a ring of FIFO_SIF_W words.  Callers never move more than
 * the free space (write) or the fill (read), so neither copy laps itself;
 * each splits at most once, where the ring wraps. */
static PCSX2_INLINE void sif_fifo_write(sifFifo *f, const void *from, int words)
{
	const int wP0 = pcsx2_min_i((FIFO_SIF_W - f->writePos), words);
	const int wP1 = words - wP0;

	memcpy(&f->data[f->writePos], from, wP0 << 2);
	memcpy(&f->data[0], (const u8*)from + (wP0 << 2), wP1 << 2);

	f->writePos = (f->writePos + words) & (FIFO_SIF_W - 1);
	f->size    += words;
}

/* Junk data writing
 *
 * If there is not enough data produced from the IOP, it will always use the previous full quad word to
 * fill in the missing data.
 * One thing to note, when the IOP transfers the EE tag, it transfers a whole QW of data, which will include
 * the EE Tag and the next IOP tag, since the EE reads 1QW of data for DMA tags.
 *
 * So the data used will be as follows:
 * Less than 1QW = Junk data is made up of the EE tag + address (64 bits) and the following IOP tag (64 bits).
 * More than 1QW = Junk data is made up of the last complete QW of data that was transferred in this packet.
 *
 * Data is always offset in to the junk by the amount the IOP actually transferred, so if it sent 2 words
 * it will read words 3 and 4 out of the junk to fill the space.
 *
 * PS2 test results:
 *
 * Example of less than 1QW being sent with the only data being set being 0x69
 *
 *	addr 0x1500a0 value 0x69        <-- actual data (junk behind this would be the EE tag)
 *	addr 0x1500a4 value 0x1500a0    <-- EE address
 *	addr 0x1500a8 value 0x8001a170  <-- following IOP tag
 *	addr 0x1500ac value 0x10        <-- following IOP tag word count
 *
 * Example of more than 1QW being sent with the data going from 0x20 to 0x25
 *
 *	addr 0x150080 value 0x21 <-- start of previously completed QW
 *	addr 0x150084 value 0x22
 *	addr 0x150088 value 0x23
 *	addr 0x15008c value 0x24 <-- end of previously completed QW
 *	addr 0x150090 value 0x25 <-- end of recorded data
 *	addr 0x150094 value 0x22 <-- from position 2 of the previously completed quadword
 *	addr 0x150098 value 0x23 <-- from position 3 of the previously completed quadword
 *	addr 0x15009c value 0x24 <-- from position 4 of the previously completed quadword
 */
static PCSX2_INLINE void sif_fifo_write_junk(sifFifo *f, int words)
{
	/* Get the start position of the previously completed whole QW.
	 * Position is in word (32bit) units. */
	const int transferredWords = 4 - words;
	const int prevQWPos = (f->writePos - (4 + transferredWords)) & (FIFO_SIF_W - 1);

	/* Read the old data in to our junk array in case of wrapping. */
	const int rP0 = pcsx2_min_i((FIFO_SIF_W - prevQWPos), 4);
	const int rP1 = 4 - rP0;

	/* Fill the missing words to fill the QW. */
	const int wP0 = pcsx2_min_i((FIFO_SIF_W - f->writePos), words);
	const int wP1 = words - wP0;

	memcpy(&f->junk[0], &f->data[prevQWPos], rP0 << 2);
	memcpy(&f->junk[rP0], &f->data[0], rP1 << 2);

	memcpy(&f->data[f->writePos], &f->junk[4-wP0], wP0 << 2);
	memcpy(&f->data[0], &f->junk[wP0], wP1 << 2);

	f->writePos = (f->writePos + words) & (FIFO_SIF_W - 1);
	f->size    += words;
}

static PCSX2_INLINE void sif_fifo_read(sifFifo *f, void *to, int words)
{
	const int wP0 = pcsx2_min_i((FIFO_SIF_W - f->readPos), words);
	const int wP1 = words - wP0;

	memcpy(to, &f->data[f->readPos], wP0 << 2);
	memcpy((u8*)to + (wP0 << 2), &f->data[0], wP1 << 2);

	f->readPos = (f->readPos + words) & (FIFO_SIF_W - 1);
	f->size   -= words;
}

typedef struct old_sif_structure
{
	sifFifo fifo; /* Used in both. */
	s32 chain; /* Not used. */
	s32 end; /* Only used for EE. */
	s32 tagMode; /* No longer used. */
	s32 counter; /* Used to keep track of how much is left in IOP. */
	struct sifData data; /* Only used in IOP. */
} old_sif_structure;

typedef struct sif_ee
{
	bool end; /* Only used for EE. */
	bool busy;

	s32 cycles;
} sif_ee;

typedef struct sif_iop
{
	bool end;
	bool busy;

	s32 cycles;
	s32 writeJunk;

	s32 counter; /* Used to keep track of how much is left in IOP. */
	struct sifData data; /* Only used in IOP. */
} sif_iop;

typedef struct _sif
{
	sifFifo fifo; /* Used in both. */
	sif_ee ee;
	sif_iop iop;
} _sif;

#ifdef __cplusplus
extern "C" {
#endif

extern _sif sif0, sif1, sif2;

extern void sifReset(void);

extern void SIF0Dma(void);
extern void SIF1Dma(void);

extern void dmaSIF0(void);
extern void dmaSIF1(void);
extern void dmaSIF2(void);

extern void EEsif0Interrupt(void);
extern void EEsif1Interrupt(void);

extern void sif0Interrupt(void);
extern void sif1Interrupt(void);
extern void sif2Interrupt(void);

extern void ReadFifoSingleWord(void);

/* Sif.c: the copies between the fifo and either side's memory.
 *
 * The EE side resolves ch->madr through the DMAC.  A bus error at the
 * first quadword raises it on the channel (dmach_get_addr) and returns -1
 * with nothing moved; otherwise the count of quadwords moved comes back,
 * which falls short of qwc only if the run hits a bus error part way
 * (that address is then raised the same way).  qwc may be 0: the address
 * is still checked, as the DMAC does before every burst.
 *
 * The IOP side addresses its 2 MiB of RAM by byte; a run that crosses the
 * top wraps to the bottom, as the address decode does. */
extern int  sif_fifo_to_ee(sifFifo *f, DMACh *ch, u32 num, int qwc);
extern int  sif_ee_to_fifo(sifFifo *f, DMACh *ch, u32 num, int qwc);
extern void sif_iop_to_fifo(sifFifo *f, u32 addr, int words);
extern void sif_fifo_to_iop(sifFifo *f, u32 addr, int words);
extern void sif_iop_read(void *dst, u32 addr, u32 bytes);

#ifdef __cplusplus
}
#endif

#define sif0data sif0.iop.data.data
#define sif1data sif1.iop.data.data
#define sif2data sif2.iop.data.data

#define sif0words sif0.iop.data.words
#define sif1words sif1.iop.data.words
#define sif2words sif2.iop.data.words
