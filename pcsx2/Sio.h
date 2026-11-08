/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2022  PCSX2 Dev Team
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

/* SIO0 (the PS1-side pad and card port) and SIO2 (the PS2 side). After
 * PSI's documentation of SIO2:
 * https://psi-rockin.github.io/ps2tek/#sio2registers
 * C89. */
#pragma once

#include <stddef.h>

#include "SioTypes.h"
#include "SioFifo.h"
#include "MemoryCardFile.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SIO2_SEND3_COUNT 16

extern SioFifo fifoIn;
extern SioFifo fifoOut;

/* A memory card's protocol state, one per port and slot. */
typedef struct _mcd
{
	u32 sectorAddr;     /* read/write sector address */
	u32 transferAddr;   /* transfer address */
	u32 autoEjectTicks;

	u8 term; /* terminator value */
	u8 FLAG; /* for PS1 cards */
	u8 port; /* port */
	u8 slot; /* and slot for this card */

	bool goodSector; /* xor sector check */
} _mcd;

/* A state carries Sio0 and Sio2 whole: their members' order and types are
 * the state's layout. */
typedef struct Sio0
{
	u32 txData; /* 0x1f801040 */
	u32 rxData; /* 0x1f801040 */
	u32 stat;   /* 0x1f801044 */
	u16 mode;   /* 0x1f801048 */
	u16 ctrl;   /* 0x1f80104a */
	u16 baud;   /* 0x1f80104e */
	u8 flag;

	SioStage sioStage;
	u8 sioMode;
	u8 sioCommand;
	bool padStarted;
	bool rxDataSet;

	u8 port;
	u8 slot;
} Sio0;

typedef struct Sio2
{
	u32 send3[SIO2_SEND3_COUNT]; /* 0x1f808200 - 0x1f80823f */
	/* SEND1 and SEND2 may be the same register: bit 2 picks SEND2. Their
	 * use is not known. */
	u32 send1[4];  /* 0x1f808240 - 0x1f80825f */
	u32 send2[4];  /* 0x1f808240 - 0x1f80825f */
	u32 dataIn;    /* 0x1f808260 */
	u32 dataOut;   /* 0x1f808264 */
	u32 ctrl;      /* 0x1f808268 */
	u32 recv1;     /* 0x1f80826c */
	u32 recv2;     /* 0x1f808270 */
	u32 recv3;     /* 0x1f808274 */
	u32 unknown1;  /* 0x1f808278 */
	u32 unknown2;  /* 0x1f80827c */
	u32 iStat;     /* 0x1f808280 */

	u8 port;
	u8 slot;

	/* Where in SEND3, the 16 command descriptors (port and length), the
	 * transfer is. */
	bool send3Read;
	size_t send3Position;
	size_t commandLength;
	size_t processedLength;
	/* One DMA11/DMA12 block's size: psxDma11 sets it before its writes,
	 * a SEND3 write clears it so a direct write does not count by it. */
	size_t dmaBlockSize;
	bool send3Complete;
} Sio2;

extern Sio0 sio0;
extern Sio2 sio2;
extern _mcd* mcd;

void sio0_full_reset(void);
void sio0_soft_reset(void);
void sio0_interrupt(Sio0Interrupt why);
u8 sio0_get_rx_data(void);
u32 sio0_get_stat(void);
void sio0_set_tx_data(u8 value);
void sio0_set_ctrl(u16 value);

void sio2_full_reset(void);
void sio2_soft_reset(void);
void sio2_set_ctrl(u32 value);
void sio2_set_send3(size_t position, u32 value);
void sio2_set_recv1(u32 value);
void sio2_write(u8 data);
u8 sio2_read(void);

/* Bounds a loaded state is held to. */
#define sio_port_slot_ok(port, slot) ((size_t)(port) < SIO_PORTS && (size_t)(slot) < SIO_SLOTS)
#define sio2_send3_ok(position, length) 	((size_t)(position) <= SIO2_SEND3_COUNT && (size_t)(length) <= SEND3_COMMAND_LENGTH_MASK)

/* A global pad index to a multitap port and slot, and back. */
void sioConvertPadToPortAndSlot(u32 index, u32* port, u32* slot);
u32 sioConvertPortAndSlotToPad(u32 port, u32 slot);
bool sioPadIsMultitapSlot(u32 index);
bool sioPortAndSlotIsMultitap(u32 port, u32 slot);
void sioSetGameSerial(const char* serial);

/* A card reads as pulled out for a while, so the game notices a swap. */
void AutoEject_Set(size_t port, size_t slot);
void AutoEject_Clear(size_t port, size_t slot);
void AutoEject_SetAll(void);
void AutoEject_ClearAll(void);

#ifdef __cplusplus
}
#endif
