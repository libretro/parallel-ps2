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

/* SIO0 and SIO2. See Sio.h. */
#include <stddef.h>

#include "R3000A.h"
#include "IopHw.h"
#include "IopDma.h"

#include "Sio.h"
#include "SaveState.h"
#include "FireWire/p1io.h"
#include "MemoryCardProtocol.h"
#include "MultitapProtocol.h"
#include "PAD/PADsio.h"

extern retro_log_printf_t log_cb;
extern s64 PSXCLK;

SioFifo fifoIn;
SioFifo fifoOut;

Sio0 sio0;
Sio2 sio2;

static _mcd mcds[SIO_PORTS][SIO_SLOTS];
_mcd* mcd = &mcds[0][0];

/* A state carries Sio0 and Sio2 as their bytes: on a 64-bit host they keep
 * the layout states have always had. */
typedef char sio_state_layout[(sizeof(void*) != 8
	|| (sizeof(Sio0) == 32 && offsetof(Sio0, flag) == 18 && offsetof(Sio0, sioStage) == 20
		&& offsetof(Sio0, slot) == 29 && sizeof(Sio2) == 176 && offsetof(Sio2, port) == 132
		&& offsetof(Sio2, send3Position) == 136 && offsetof(Sio2, send3Complete) == 168))
	? 1 : -1];

static void reset_mcds(void)
{
	int i, j;
	for (i = 0; i < SIO_PORTS; i++)
		for (j = 0; j < SIO_SLOTS; j++)
		{
			mcds[i][j].term = TERMINATOR_DEFAULT;
			mcds[i][j].port = (u8)i;
			mcds[i][j].slot = (u8)j;
			mcds[i][j].FLAG = 0x08;
			mcds[i][j].autoEjectTicks = 0;
		}
	mcd = &mcds[0][0];
}

/* ---- SIO0 --------------------------------------------------------- */

void sio0_soft_reset(void)
{
	sio0.padStarted = false;
	sio0.sioMode = SIO_MODE_NOT_SET;
	sio0.sioCommand = 0;
	sio0.sioStage = SIO_STAGE_IDLE;
}

void sio0_full_reset(void)
{
	sio0_soft_reset();
	sio0.port = 0;
	sio0.slot = 0;
	reset_mcds();
}

void sio0_interrupt(Sio0Interrupt why)
{
	if (why == SIO0_INT_TEST_EVENT)
		iopIntcIrq(7);
	else if (why == SIO0_INT_STAT_READ)
		sio0.stat &= ~SIO0_STAT_ACK;

	if (!(psxRegs.interrupt & (1 << IopEvt_SIO)))
		PSX_INT(IopEvt_SIO, (s32)(PSXCLK / 250000));
}

u8 sio0_get_rx_data(void)
{
	sio0.stat |= SIO0_STAT_TX_READY | SIO0_STAT_TX_EMPTY;
	sio0.stat &= ~SIO0_STAT_RX_FIFO_NOT_EMPTY;
	return (u8)sio0.rxData;
}

u32 sio0_get_stat(void)
{
	const u32 ret = sio0.stat;
	sio0_interrupt(SIO0_INT_STAT_READ);
	return ret;
}

static bool sio0_is_pad_command(u8 c)
{
	return c >= PAD_COMMAND_FIRST && c <= PAD_COMMAND_LAST;
}

static bool sio0_is_memcard_command(u8 c)
{
	return c == MEMCARD_CMD_PS1_READ || c == MEMCARD_CMD_PS1_STATE || c == MEMCARD_CMD_PS1_WRITE;
}

static u8 sio0_memcard(u8 value)
{
	switch (sio0.sioCommand)
	{
		case MEMCARD_CMD_PS1_READ:
			return mcp_ps1_read(value);
		case MEMCARD_CMD_PS1_STATE:
			return mcp_ps1_state(value);
		case MEMCARD_CMD_PS1_WRITE:
			return mcp_ps1_write(value);
		case MEMCARD_CMD_PS1_POCKETSTATION:
			return mcp_ps1_pocketstation(value);
		default:
			sio0_soft_reset();
			break;
	}
	return 0xff;
}

void sio0_set_tx_data(u8 value)
{
	sio0.stat |= SIO0_STAT_TX_READY | SIO0_STAT_TX_EMPTY;
	sio0.stat |= SIO0_STAT_RX_FIFO_NOT_EMPTY;

	if (!(sio0.ctrl & SIO0_CTRL_TX_ENABLE))
		return;

	sio0.txData = value;

	switch (sio0.sioStage)
	{
		case SIO_STAGE_IDLE:
			sio0.sioMode = value;
			sio0.rxData = 0;
			sio0.stat |= SIO0_STAT_TX_READY;

			switch (sio0.sioMode)
			{
				case SIO_MODE_PAD:
					if ((sio0.rxData = PADstartPoll(sio0.port, sio0.slot)))
						sio0.stat |= SIO0_STAT_ACK;
					break;
				case SIO_MODE_MEMCARD:
					mcd = &mcds[sio0.port][sio0.slot];
					/* Auto ejected: dead air, the stage does not advance. */
					if (mcd->autoEjectTicks)
					{
						mcd->autoEjectTicks--;
						return;
					}
					/* No card, or not a PS1 one: dead air and no ACK. */
					if (!FileMcd_IsPresent(mcd->port, mcd->slot) || !FileMcd_IsPSX(mcd->port, mcd->slot))
						return;
					sio0.stat |= SIO0_STAT_ACK;
					break;
			}

			sio0.sioStage = SIO_STAGE_WAITING_COMMAND;
			break;
		case SIO_STAGE_WAITING_COMMAND:
			sio0.stat &= ~SIO0_STAT_TX_READY;

			if (sio0_is_pad_command(value))
			{
				sio0.rxData = PADpoll(value);
				if (!PADcomplete())
					sio0.stat |= SIO0_STAT_ACK;
				sio0.sioStage = SIO_STAGE_WORKING;
			}
			else if (sio0_is_memcard_command(value))
			{
				sio0.rxData = sio0.flag;
				sio0.stat |= SIO0_STAT_ACK;
				sio0.sioCommand = value;
				sio0.sioStage = SIO_STAGE_WORKING;
			}
			else if (value == MEMCARD_CMD_PS1_POCKETSTATION)
			{
				/* The line goes low, no acknowledge. */
				sio0.rxData = 0x00;
				sio0.sioStage = SIO_STAGE_IDLE;
			}
			else
			{
				sio0.rxData = 0xff;
				sio0_soft_reset();
			}
			break;
		case SIO_STAGE_WORKING:
			switch (sio0.sioMode)
			{
				case SIO_MODE_PAD:
					sio0.rxData = PADpoll(value);
					if (!PADcomplete())
						sio0.stat |= SIO0_STAT_ACK;
					break;
				case SIO_MODE_MEMCARD:
					sio0.rxData = sio0_memcard(value);
					break;
				default:
					sio0.rxData = 0xff;
					sio0_soft_reset();
					break;
			}
			break;
		default:
			sio0.rxData = 0xff;
			sio0_soft_reset();
			break;
	}

	sio0_interrupt(SIO0_INT_TX_DATA_WRITE);
}

void sio0_set_ctrl(u16 value)
{
	sio0.ctrl = value;
	sio0.port = (sio0.ctrl & SIO0_CTRL_PORT) > 0;

	/* CTRL goes to 0 between transactions (undocumented): the SIO0 state
	 * starts over there, which the card probes at boot rely on. */
	if (sio0.ctrl == 0)
	{
		mcp_reset_ps1();
		sio0_soft_reset();
	}

	/* An acknowledge clears STAT bits 3 and 9. */
	if (sio0.ctrl & SIO0_CTRL_ACK)
		sio0.stat &= ~(SIO0_STAT_IRQ | SIO0_STAT_RX_PARITY_ERROR);

	if (sio0.ctrl & SIO0_CTRL_RESET)
	{
		sio0.stat = 0;
		sio0.ctrl = 0;
		sio0.mode = 0;
		sio0_soft_reset();
	}
}

/* ---- SIO2 --------------------------------------------------------- */

void sio2_soft_reset(void)
{
	sio2.send3Read = false;
	sio2.send3Position = 0;
	sio2.commandLength = 0;
	sio2.processedLength = 0;
	/* The next command may not come over DMA11. */
	sio2.dmaBlockSize = 0;
	sio2.send3Complete = false;
	/* What the last cycle did not consume goes before the next. */
	sio_fifo_clear(&fifoIn);
}

void sio2_set_ctrl(u32 value)
{
	sio2.ctrl = value;
	if (sio2.ctrl & SIO2_CTRL_START_TRANSFER)
		iopIntcIrq(17);
}

void sio2_set_send3(size_t position, u32 value)
{
	sio2.send3[position] = value;
	if (position == 0)
		sio2_soft_reset();
}

void sio2_set_recv1(u32 value)
{
	sio2.recv1 = value;
}

void sio2_full_reset(void)
{
	size_t i;
	sio2_soft_reset();
	for (i = 0; i < SIO2_SEND3_COUNT; i++)
		sio2.send3[i] = 0;
	for (i = 0; i < 4; i++)
	{
		sio2.send1[i] = 0;
		sio2.send2[i] = 0;
	}
	sio2.dataIn = 0;
	sio2.dataOut = 0;
	sio2_set_ctrl(SIO2_CTRL_SIO2MAN_RESET);
	sio2_set_recv1(RECV1_DISCONNECTED);
	sio2.recv2 = RECV2_DEFAULT;
	sio2.recv3 = RECV3_DEFAULT;
	sio2.unknown1 = 0;
	sio2.unknown2 = 0;
	sio2.iStat = 0;
	sio2.port = 0;
	sio2.slot = 0;
	sio_fifo_clear(&fifoOut);
	reset_mcds();
}

static void sio2_pad(void)
{
	/* PAD's first answer for this port. */
	sio_fifo_push(&fifoOut, PADstartPoll(sio2.port, sio2.slot));
	/* Some games read no pad unless RECV1 reads connected whenever any
	 * pad is polled, connected or not. */
	sio2_set_recv1(RECV1_CONNECTED);
	while (!sio_fifo_empty(&fifoIn))
	{
		const u8 c = sio_fifo_front(&fifoIn);
		sio_fifo_pop(&fifoIn);
		sio_fifo_push(&fifoOut, PADpoll(c));
	}
}

static void sio2_multitap(void)
{
	const bool enabled = emu_config_multitap(sio2.port);
	sio_fifo_push(&fifoOut, 0x00);
	sio2_set_recv1(enabled ? RECV1_CONNECTED : RECV1_DISCONNECTED);
	if (enabled)
		multitap_send();
	else
		while (sio_fifo_size(&fifoOut) < sio2.commandLength)
			sio_fifo_push(&fifoOut, 0x00);
}

static void sio2_infrared(void)
{
	sio2_set_recv1(RECV1_DISCONNECTED);
	sio_fifo_pop(&fifoIn);
	while (sio_fifo_size(&fifoOut) < sio2.commandLength)
		sio_fifo_push(&fifoOut, 0xff);
}

/* Every byte left through one of the PS1 card's handlers. */
static void sio2_ps1(u8 (*handler)(u8))
{
	mcp_reset_ps1();
	while (!sio_fifo_empty(&fifoIn))
	{
		const u8 in = sio_fifo_front(&fifoIn);
		const u8 out = handler(in);
		sio_fifo_pop(&fifoIn);
		sio_fifo_push(&fifoOut, out);
	}
}

/* Dead air for the rest of the command: fill for each byte that came. */
static void sio2_dead_air(u8 fill)
{
	sio_fifo_push(&fifoOut, fill); /* sio2_write took the first byte */
	while (!sio_fifo_empty(&fifoIn))
	{
		sio_fifo_pop(&fifoIn);
		sio_fifo_push(&fifoOut, fill);
	}
}

static void sio2_memcard(void)
{
	/* A Python 1 serial board selects the second port's slot by its
	 * latch, and only slot 0 has a card behind it. */
	const int p1io_slot = sio2.port == 1 ? p1io_memcard_slot() : -1;
	u8 command;
	bool present;

	mcd = &mcds[sio2.port][p1io_slot >= 0 ? p1io_slot : sio2.slot];
	if (p1io_slot > 0)
	{
		sio2_set_recv1(RECV1_DISCONNECTED);
		sio2_dead_air(0xff);
		return;
	}

	/* Auto ejected: RECV1 disconnected and dead air over the wire. */
	if (mcd->autoEjectTicks)
	{
		sio2_set_recv1(RECV1_DISCONNECTED);
		sio2_dead_air(0x00);
		mcd->autoEjectTicks--;
		return;
	}

	present = FileMcd_IsPresent(mcd->port, mcd->slot) != 0;
	sio2_set_recv1(present ? RECV1_CONNECTED : RECV1_DISCONNECTED);
	command = sio_fifo_front(&fifoIn);
	sio_fifo_pop(&fifoIn);
	sio_fifo_push(&fifoOut, present ? 0x00 : 0xff);
	/* The FLAG byte is a PS1 card's, but the second byte is a dud on a
	 * PS2 card, so it always goes. */
	sio_fifo_push(&fifoOut, mcd->FLAG);

	switch (command)
	{
		case MEMCARD_CMD_PROBE:
			mcp_probe();
			break;
		case MEMCARD_CMD_UNKNOWN_WRITE_DELETE_END:
			mcp_unknown_write_delete_end();
			break;
		case MEMCARD_CMD_SET_ERASE_SECTOR:
		case MEMCARD_CMD_SET_WRITE_SECTOR:
		case MEMCARD_CMD_SET_READ_SECTOR:
			mcp_set_sector();
			break;
		case MEMCARD_CMD_GET_SPECS:
			mcp_get_specs();
			break;
		case MEMCARD_CMD_SET_TERMINATOR:
			mcp_set_terminator();
			break;
		case MEMCARD_CMD_GET_TERMINATOR:
			mcp_get_terminator();
			break;
		case MEMCARD_CMD_WRITE_DATA:
			mcp_write_data();
			break;
		case MEMCARD_CMD_READ_DATA:
			mcp_read_data();
			break;
		case MEMCARD_CMD_PS1_READ:
			sio2_ps1(mcp_ps1_read);
			break;
		case MEMCARD_CMD_PS1_STATE:
			sio2_ps1(mcp_ps1_state);
			break;
		case MEMCARD_CMD_PS1_WRITE:
			sio2_ps1(mcp_ps1_write);
			break;
		case MEMCARD_CMD_PS1_POCKETSTATION:
			sio2_ps1(mcp_ps1_pocketstation);
			break;
		case MEMCARD_CMD_READ_WRITE_END:
			mcp_read_write_end();
			break;
		case MEMCARD_CMD_ERASE_BLOCK:
			mcp_erase_block();
			break;
		case MEMCARD_CMD_UNKNOWN_BOOT:
			mcp_unknown_boot();
			break;
		case MEMCARD_CMD_AUTH_XOR:
			mcp_auth_xor();
			break;
		case MEMCARD_CMD_AUTH_F3:
			mcp_auth_f3();
			break;
		case MEMCARD_CMD_AUTH_F7:
			mcp_auth_f7();
			break;
		default:
			break;
	}
}

void sio2_write(u8 data)
{
	if (!sio2.send3Read)
	{
		u32 current;
		/* Past the last SEND3 position: the writes go nowhere. */
		if (sio2.send3Position >= SIO2_SEND3_COUNT)
			return;
		current = sio2.send3[sio2.send3Position];
		sio2.port = (u8)(current & SEND3_PORT);
		sio2.commandLength = (current >> 8) & SEND3_COMMAND_LENGTH_MASK;
		sio2.send3Read = true;
		/* A position of length 0: no more commands until SEND3 is
		 * written again. */
		if (sio2.commandLength == 0)
			sio2.send3Complete = true;
		/* What the last command left is not this one's. */
		sio_fifo_clear(&fifoIn);
	}

	if (sio2.send3Complete)
		return;

	sio_fifo_push(&fifoIn, data);

	/* The whole command is in: by its SEND3 length for direct writes, by
	 * the DMA block for SIO2 DMA. */
	if ((sio_fifo_size(&fifoIn) == sio2.commandLength && sio2.dmaBlockSize == 0)
		|| sio_fifo_size(&fifoIn) == sio2.dmaBlockSize)
	{
		u8 mode;
		/* The next write reads the next SEND3. */
		sio2.send3Read = false;
		sio2.send3Position++;

		mode = sio_fifo_front(&fifoIn);
		sio_fifo_pop(&fifoIn);
		switch (mode)
		{
			case SIO_MODE_PAD:
				sio2_pad();
				break;
			case SIO_MODE_MULTITAP:
				sio2_multitap();
				break;
			case SIO_MODE_INFRARED:
				sio2_infrared();
				break;
			case SIO_MODE_MEMCARD:
				sio2_memcard();
				break;
			default:
				sio_fifo_push(&fifoOut, 0x00);
				sio2_set_recv1(RECV1_DISCONNECTED);
				break;
		}

		/* Over DMA, the answer is padded to whole blocks. */
		if (sio2.dmaBlockSize > 0)
		{
			const size_t diff = sio_fifo_size(&fifoOut) % sio2.dmaBlockSize;
			if (diff > 0)
			{
				size_t i;
				for (i = 0; i < sio2.dmaBlockSize - diff; i++)
					sio_fifo_push(&fifoOut, 0x00);
			}
		}
	}
}

u8 sio2_read(void)
{
	u8 ret = 0x00;
	if (!sio_fifo_empty(&fifoOut))
	{
		ret = sio_fifo_front(&fifoOut);
		sio_fifo_pop(&fifoOut);
	}
	return ret;
}

void sioSetGameSerial(const char* serial)
{
	uint port, slot;
	for (port = 0; port < SIO_PORTS; ++port)
		for (slot = 0; slot < SIO_SLOTS; ++slot)
			if (FileMcd_ReIndex(port, slot, serial))
				AutoEject_Set(port, slot);
}

/* ---- savestate ---------------------------------------------------- */

/* The protocol state of every card: the terminator MCMAN set, the sector
 * and transfer address of the access in progress, the PS1 flag and the
 * auto-eject countdown. port and slot are fixed by position. */
static bool FreezeMcds(SaveStateBase* s)
{
	u32 port, slot;

	if (!SaveState_FreezeTag(s, "mcds"))
		return false;

	for (port = 0; port < SIO_PORTS; port++)
	{
		for (slot = 0; slot < SIO_SLOTS; slot++)
		{
			_mcd* m = &mcds[port][slot];
			u8 good = m->goodSector ? 1 : 0;

			SaveState_Freeze(s, m->sectorAddr);
			SaveState_Freeze(s, m->transferAddr);
			SaveState_Freeze(s, m->autoEjectTicks);
			SaveState_Freeze(s, m->term);
			SaveState_Freeze(s, m->FLAG);
			SaveState_Freeze(s, good);
			if (SaveState_IsLoading(s))
				m->goodSector = good != 0;
		}
	}

	return SaveState_IsOkay(s);
}

bool sio2Freeze(SaveStateBase* s)
{
	/* The cards' CRCs: a card that has not changed since the state was
	 * saved need not be ejected when it loads. */
	u64 crcs[SIO_PORTS][SIO_SLOTS];
	u32 port, slot;

	if (!SaveState_FreezeTag(s, "sio2"))
		return false;

	SaveState_Freeze(s, sio2);
	if (SaveState_IsLoading(s) && (!sio_port_slot_ok(sio2.port, sio2.slot)
			|| !sio2_send3_ok(sio2.send3Position, sio2.commandLength)))
		s->error = true;
	sio_fifo_freeze(s, &fifoIn);
	sio_fifo_freeze(s, &fifoOut);
	if (!SaveState_IsOkay(s))
		return false;

	/* States from before the card protocol state end the SIO2 block here;
	 * the cards keep the state they have. */
	if (SaveState_IsSaving(s) || SaveState_PeekTag(s, "mcds"))
	{
		if (!FreezeMcds(s) || !mcp_freeze_ps1(s))
			return false;
	}
	else
		log_cb(RETRO_LOG_INFO, "sio2Freeze: state predates the memory card protocol block; cards keep their current state.\n");

	if (SaveState_IsSaving(s))
		for (port = 0; port < SIO_PORTS; port++)
			for (slot = 0; slot < SIO_SLOTS; slot++)
				crcs[port][slot] = FileMcd_GetCRC(port, slot);
	SaveState_Freeze(s, crcs);
	if (!SaveState_IsOkay(s))
		return false;

	if (SaveState_IsLoading(s))
	{
		for (port = 0; port < SIO_PORTS; port++)
			for (slot = 0; slot < SIO_SLOTS; slot++)
				if (crcs[port][slot] != FileMcd_GetCRC(port, slot))
				{
					AutoEject_SetAll();
					return true;
				}
	}
	return true;
}

bool sioFreeze(SaveStateBase* s)
{
	if (!SaveState_FreezeTag(s, "sio0"))
		return false;

	SaveState_Freeze(s, sio0);
	if (SaveState_IsLoading(s) && !sio_port_slot_ok(sio0.port, sio0.slot))
		s->error = true;
	if (!SaveState_IsOkay(s))
		return false;

	/* A PS1 card transfer spans many SIO0 bytes, all to the card selected
	 * at its first one. */
	if (SaveState_IsLoading(s))
		mcd = &mcds[sio0.port][sio0.slot];
	return true;
}

/* ---- pads, ports and slots ---------------------------------------- */

void sioConvertPadToPortAndSlot(u32 index, u32* port, u32* slot)
{
	if (index > 4) /* [5,6,7]: 2B, 2C, 2D */
	{
		*port = 1;
		*slot = index - 4;
	}
	else if (index > 1) /* [2,3,4]: 1B, 1C, 1D */
	{
		*port = 0;
		*slot = index - 1;
	}
	else /* [0,1]: 1A, 2A */
	{
		*port = index;
		*slot = 0;
	}
}

u32 sioConvertPortAndSlotToPad(u32 port, u32 slot)
{
	if (slot == 0)
		return port;
	if (port == 0)
		return slot + 1; /* 2, 3, 4 */
	return slot + 4;     /* 5, 6, 7 */
}

bool sioPadIsMultitapSlot(u32 index)
{
	return index >= 2;
}

bool sioPortAndSlotIsMultitap(u32 port, u32 slot)
{
	(void)port;
	return slot != 0;
}

void AutoEject_Set(size_t port, size_t slot)
{
	if (emu_config_mcd_ejection())
		mcds[port][slot].autoEjectTicks = 60;
}

void AutoEject_Clear(size_t port, size_t slot)
{
	mcds[port][slot].autoEjectTicks = 0;
}

void AutoEject_SetAll(void)
{
	size_t port, slot;
	for (port = 0; port < SIO_PORTS; port++)
		for (slot = 0; slot < SIO_SLOTS; slot++)
			AutoEject_Set(port, slot);
}

void AutoEject_ClearAll(void)
{
	size_t port, slot;
	for (port = 0; port < SIO_PORTS; port++)
		for (slot = 0; slot < SIO_SLOTS; slot++)
			AutoEject_Clear(port, slot);
}
