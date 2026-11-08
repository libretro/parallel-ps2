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

/* The memory card's answers. See MemoryCardProtocol.h. */
#include <string.h>

#include "MemoryCardProtocol.h"
#include "SaveState.h"
#include "Sio.h"
#include "CDVD/MechaMg.h"

static PS1MemoryCardState ps1McState = { 2, 0, 0, 0, 0, { 0 } };

#define OUT(v) sio_fifo_push(&fifoOut, (u8)(v))

static u8 take(void)
{
	const u8 v = sio_fifo_front(&fifoIn);
	sio_fifo_pop(&fifoIn);
	return v;
}

/* A PS1 card asked over SIO2: dead air. */
static bool ps1_fail(void)
{
	if (FileMcd_IsPSX(mcd->port, mcd->slot) && sio2.commandLength > 0)
	{
		while (sio_fifo_size(&fifoOut) < sio2.commandLength)
			OUT(0x00);
		return true;
	}
	return false;
}

/* Zeros to length - 2, then 0x2b and the terminator: the end most
 * answers have. */
static void the_2b_terminator(size_t length)
{
	while (sio_fifo_size(&fifoOut) < length - 2)
		OUT(0x00);
	OUT(0x2b);
	OUT(mcd->term);
}

/* The next read or write is almost certainly the next segment of the same
 * sector; a new sector comes through set_sector. */
static void read_write_increment(size_t length)
{
	mcd->transferAddr += (u32)length;
}

static void recalculate_ps1_addr(void)
{
	mcd->sectorAddr = ((u32)ps1McState.sectorAddrMSB << 8) | ps1McState.sectorAddrLSB;
	mcd->goodSector = mcd->sectorAddr <= 0x03ff;
	mcd->transferAddr = 128 * mcd->sectorAddr;
}

void mcp_reset_ps1(void)
{
	ps1McState.currentByte = 2;
	ps1McState.sectorAddrMSB = 0;
	ps1McState.sectorAddrLSB = 0;
	ps1McState.checksum = 0;
	ps1McState.expectedChecksum = 0;
	memset(ps1McState.buf, 0, sizeof(ps1McState.buf));
}

/* The PS1 transfer in progress, if any, so a state saved mid-sector picks up
 * at the same byte. currentByte goes in as a u32: the state is the same on
 * every host. */
bool mcp_freeze_ps1(SaveStateBase* s)
{
	u32 current_byte = (u32)ps1McState.currentByte;

	SaveState_Freeze(s, current_byte);
	SaveState_Freeze(s, ps1McState.sectorAddrMSB);
	SaveState_Freeze(s, ps1McState.sectorAddrLSB);
	SaveState_Freeze(s, ps1McState.checksum);
	SaveState_Freeze(s, ps1McState.expectedChecksum);
	SaveState_Freeze(s, ps1McState.buf);
	if (!SaveState_IsOkay(s))
		return false;
	if (SaveState_IsLoading(s))
		ps1McState.currentByte = current_byte;
	return true;
}

void mcp_probe(void)
{
	if (ps1_fail())
		return;
	the_2b_terminator(4);
}

void mcp_unknown_write_delete_end(void)
{
	if (ps1_fail())
		return;
	the_2b_terminator(4);
}

void mcp_set_sector(void)
{
	u8 lsb, b2, b3, msb, expected;
	McdSizeInfo info;

	if (ps1_fail())
		return;
	lsb = take();
	b2 = take();
	b3 = take();
	msb = take();
	expected = take();
	mcd->goodSector = (u8)(lsb ^ b2 ^ b3 ^ msb) == expected;
	mcd->sectorAddr = lsb | ((u32)b2 << 8) | ((u32)b3 << 16) | ((u32)msb << 24);

	FileMcd_GetSizeInfo(mcd->port, mcd->slot, &info);
	mcd->transferAddr = (info.SectorSize + 16) * mcd->sectorAddr;

	the_2b_terminator(9);
}

void mcp_get_specs(void)
{
	McdSizeInfo info;
	if (ps1_fail())
		return;
	FileMcd_GetSizeInfo(mcd->port, mcd->slot, &info);
	OUT(0x2b);
	OUT(info.SectorSize & 0xff);
	OUT(info.SectorSize >> 8);
	OUT(info.EraseBlockSizeInSectors & 0xff);
	OUT(info.EraseBlockSizeInSectors >> 8);
	OUT(info.McdSizeInSectors & 0xff);
	OUT(info.McdSizeInSectors >> 8);
	OUT(info.McdSizeInSectors >> 16);
	OUT(info.McdSizeInSectors >> 24);
	OUT(info.Xor);
	OUT(mcd->term);
}

void mcp_set_terminator(void)
{
	if (ps1_fail())
		return;
	/* The reply carries the terminator just set: newer MCMAN revisions
	 * check [4] for the value they sent. */
	mcd->term = take();
	OUT(0x00);
	OUT(0x2b);
	OUT(mcd->term);
}

void mcp_get_terminator(void)
{
	if (ps1_fail())
		return;
	/* MCMAN checks [3] and/or [4]; newer revisions set a terminator of
	 * their own (often 0x5a), so the card's own goes back, not 0x55. */
	OUT(0x2b);
	OUT(mcd->term);
	OUT(mcd->term);
}

void mcp_write_data(void)
{
	/* A u8 length: at most 255 bytes. */
	u8 buf[256];
	u8 length, checksum = 0;
	size_t i;

	if (ps1_fail())
		return;
	OUT(0x00);
	OUT(0x2b);
	length = take();
	for (i = 0; i < length; i++)
	{
		const u8 b = take();
		checksum ^= b;
		buf[i] = b;
		OUT(0x00);
	}
	FileMcd_Save(mcd->port, mcd->slot, buf, mcd->transferAddr, length);
	OUT(checksum);
	OUT(mcd->term);
	read_write_increment(length);
}

void mcp_read_data(void)
{
	u8 buf[256];
	u8 length, checksum = 0;
	size_t i;

	if (ps1_fail())
		return;
	length = take();
	OUT(0x00);
	OUT(0x2b);
	FileMcd_Read(mcd->port, mcd->slot, buf, mcd->transferAddr, length);
	for (i = 0; i < length; i++)
	{
		checksum ^= buf[i];
		OUT(buf[i]);
	}
	OUT(checksum);
	OUT(mcd->term);
	read_write_increment(length);
}

u8 mcp_ps1_read(u8 data)
{
	bool ack = true;
	u8 ret = 0;

	switch (ps1McState.currentByte)
	{
		case 2:
			ret = 0x5a;
			break;
		case 3:
			ret = 0x5d;
			break;
		case 4:
			ps1McState.sectorAddrMSB = data;
			ret = 0x00;
			break;
		case 5:
			ps1McState.sectorAddrLSB = data;
			ret = 0x00;
			recalculate_ps1_addr();
			break;
		case 6:
			ret = 0x5c;
			break;
		case 7:
			ret = 0x5d;
			break;
		case 8:
			ret = ps1McState.sectorAddrMSB;
			break;
		case 9:
			ret = ps1McState.sectorAddrLSB;
			break;
		case 138:
			ret = ps1McState.checksum;
			break;
		case 139:
			ret = 0x47;
			ack = false;
			break;
		case 10:
			ps1McState.checksum = ps1McState.sectorAddrMSB ^ ps1McState.sectorAddrLSB;
			FileMcd_Read(mcd->port, mcd->slot, ps1McState.buf, mcd->transferAddr, sizeof(ps1McState.buf));
			/* fallthrough */
		default:
			ret = ps1McState.buf[ps1McState.currentByte - 10];
			ps1McState.checksum ^= ret;
			break;
	}

	if (ack)
		sio0.stat |= SIO0_STAT_ACK;
	ps1McState.currentByte++;
	return ret;
}

u8 mcp_ps1_state(u8 data)
{
	(void)data;
	return 0x00;
}

u8 mcp_ps1_write(u8 data)
{
	bool ack = true;
	u8 ret = 0;

	switch (ps1McState.currentByte)
	{
		case 2:
			ret = 0x5a;
			break;
		case 3:
			ret = 0x5d;
			break;
		case 4:
			ps1McState.sectorAddrMSB = data;
			ret = 0x00;
			break;
		case 5:
			ps1McState.sectorAddrLSB = data;
			ret = 0x00;
			recalculate_ps1_addr();
			break;
		case 134:
			ps1McState.expectedChecksum = data;
			ret = 0;
			break;
		case 135:
			ret = 0x5c;
			break;
		case 136:
			ret = 0x5d;
			break;
		case 137:
			if (!mcd->goodSector)
				ret = 0xff;
			else if (ps1McState.expectedChecksum != ps1McState.checksum)
				ret = 0x4e;
			else
			{
				FileMcd_Save(mcd->port, mcd->slot, ps1McState.buf, mcd->transferAddr, sizeof(ps1McState.buf));
				ret = 0x47;
				/* The flag's "directory unread" bit clears on a write, not
				 * a read (no$psx). */
				mcd->FLAG &= 0x07;
			}
			ack = false;
			break;
		case 6:
			ps1McState.checksum = ps1McState.sectorAddrMSB ^ ps1McState.sectorAddrLSB;
			/* fallthrough */
		default:
			ps1McState.buf[ps1McState.currentByte - 6] = data;
			ps1McState.checksum ^= data;
			ret = 0x00;
			break;
	}

	if (ack)
		sio0.stat |= SIO0_STAT_ACK;
	ps1McState.currentByte++;
	return ret;
}

u8 mcp_ps1_pocketstation(u8 data)
{
	(void)data;
	sio2_set_recv1(RECV1_DISCONNECTED);
	return 0x00;
}

void mcp_read_write_end(void)
{
	if (ps1_fail())
		return;
	the_2b_terminator(4);
}

void mcp_erase_block(void)
{
	if (ps1_fail())
		return;
	FileMcd_EraseBlock(mcd->port, mcd->slot, mcd->transferAddr);
	the_2b_terminator(4);
}

void mcp_unknown_boot(void)
{
	if (ps1_fail())
		return;
	the_2b_terminator(5);
}

/* The card's side of keyed MagicGate: values go out and come in last
 * byte first, an outgoing one followed by its XOR. False for the modes
 * that carry nothing, which take the unkeyed path. */
static bool auth_xor_keyed(u8 mode)
{
	const int slot = (int)sioConvertPortAndSlotToPad(mcd->port, mcd->slot);
	u8 v[8];
	size_t i;

	if (mecha_mg_card_get(slot, mode, v))
	{
		u8 x = 0;
		OUT(0x00);
		OUT(0x2b);
		for (i = 0; i < 8; i++)
		{
			if (!sio_fifo_empty(&fifoIn))
				sio_fifo_pop(&fifoIn);
			x ^= v[7 - i];
			OUT(v[7 - i]);
		}
		OUT(x);
		OUT(mcd->term);
		return true;
	}
	if (mode == 0x06 || mode == 0x07 || mode == 0x0b)
	{
		for (i = 0; i < 8; i++)
			v[7 - i] = sio_fifo_empty(&fifoIn) ? 0 : take();
		mecha_mg_card_put(slot, mode, v);
		the_2b_terminator(14);
		return true;
	}
	return false;
}

void mcp_auth_xor(void)
{
	u8 mode;
	size_t i;

	if (ps1_fail())
		return;
	mode = take();
	if (mecha_mg_keyed() && auth_xor_keyed(mode))
		return;

	switch (mode)
	{
		/* RECV3 says 14 bytes, and the PS2 wants what it sends XORed. */
		case 0x01:
		case 0x02:
		case 0x04:
		case 0x0f:
		case 0x11:
		case 0x13:
		{
			u8 x = 0;
			OUT(0x00);
			OUT(0x2b);
			for (i = 0; i < 8; i++)
			{
				x ^= take();
				OUT(0x00);
			}
			OUT(x);
			OUT(mcd->term);
			break;
		}
		/* RECV3 says 5 bytes, nothing XORed. */
		case 0x00:
		case 0x03:
		case 0x05:
		case 0x08:
		case 0x09:
		case 0x0a:
		case 0x0c:
		case 0x0d:
		case 0x0e:
		case 0x10:
		case 0x12:
		case 0x14:
			the_2b_terminator(5);
			break;
		/* RECV3 says 14 bytes of data from the PS2, and it wants 0x2b and
		 * the terminator last, not the XOR. */
		case 0x06:
		case 0x07:
		case 0x0b:
			the_2b_terminator(14);
			break;
		default:
			break;
	}
}

void mcp_auth_f3(void)
{
	int pad;
	if (ps1_fail())
		return;
	/* An authentication reset puts the card back on the default
	 * terminator, which MCMAN sets again once the card is authenticated. */
	mcd->term = TERMINATOR_DEFAULT;
	pad = (int)sioConvertPortAndSlotToPad(mcd->port, mcd->slot);
	/* The ID this card answers the handshake with: its own, when the user
	 * gave one (a real dongle's), else the emulated card's. */
	mecha_mg_card_set_id(pad, FileMcd_GetMagicGateId(mcd->port, mcd->slot));
	mecha_mg_card_auth_reset(pad);
	the_2b_terminator(5);
}

void mcp_auth_f7(void)
{
	if (ps1_fail())
		return;
	the_2b_terminator(5);
}
