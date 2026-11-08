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


#include <cstring>

#include "MemoryCardProtocol.h"
#include "SaveState.h"
#include "Sio.h"
#include "CDVD/MechaMg.h"

MemoryCardProtocol g_MemoryCardProtocol;

// Check if the memcard is for PS1, and if we are working on a command sent over SIO2.
// If so, return dead air.
bool MemoryCardProtocol::PS1Fail()
{
	if (FileMcd_IsPSX(mcd->port, mcd->slot) && sio2.commandLength > 0)
	{
		while (fifoOut.size() < sio2.commandLength)
		{
			fifoOut.push_back(0x00);
		}

		return true;
	}

	return false;
}

// A repeated pattern in memcard commands is to pad with zero bytes,
// then end with 0x2b and terminator bytes. This function is a shortcut for that.
void MemoryCardProtocol::The2bTerminator(size_t length)
{
	while (fifoOut.size() < length - 2)
		fifoOut.push_back(0x00);

	fifoOut.push_back(0x2b);
	fifoOut.push_back(mcd->term);
}

// After one read or write, the memcard is almost certainly going to be issued a new read or write
// for the next segment of the same sector. Bump the transferAddr to where that segment begins.
// If it is the end and a new sector is being accessed, the SetSector function will deal with
// both sectorAddr and transferAddr.
void MemoryCardProtocol::ReadWriteIncrement(size_t length)
{
	mcd->transferAddr += length;
}

void MemoryCardProtocol::RecalculatePS1Addr()
{
	mcd->sectorAddr = ((ps1McState.sectorAddrMSB << 8) | ps1McState.sectorAddrLSB);
	mcd->goodSector = (mcd->sectorAddr <= 0x03ff);
	mcd->transferAddr = 128 * mcd->sectorAddr;
}

void MemoryCardProtocol::ResetPS1State()
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
bool MemoryCardProtocol::FreezePS1State(SaveStateBase* s)
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

void MemoryCardProtocol::Probe()
{
	if (this->PS1Fail()) return;
	The2bTerminator(4);
}

void MemoryCardProtocol::UnknownWriteDeleteEnd()
{
	if (this->PS1Fail()) return;
	The2bTerminator(4);
}

void MemoryCardProtocol::SetSector()
{
	if (this->PS1Fail()) return;
	const u8 sectorLSB = fifoIn.front();
	fifoIn.pop_front();
	const u8 sector2nd = fifoIn.front();
	fifoIn.pop_front();
	const u8 sector3rd = fifoIn.front();
	fifoIn.pop_front();
	const u8 sectorMSB = fifoIn.front();
	fifoIn.pop_front();
	const u8 expectedChecksum = fifoIn.front();
	fifoIn.pop_front();

	u8 computedChecksum = sectorLSB ^ sector2nd ^ sector3rd ^ sectorMSB;
	mcd->goodSector = (computedChecksum == expectedChecksum);

	u32 newSector = sectorLSB | (sector2nd << 8) | (sector3rd << 16) | (sectorMSB << 24);
	mcd->sectorAddr = newSector;

	McdSizeInfo info;
	FileMcd_GetSizeInfo(mcd->port, mcd->slot, &info);
	mcd->transferAddr = (info.SectorSize + 16) * mcd->sectorAddr;

	The2bTerminator(9);
}

void MemoryCardProtocol::GetSpecs()
{
	if (this->PS1Fail()) return;
	McdSizeInfo info;
	FileMcd_GetSizeInfo(mcd->port, mcd->slot, &info);
	fifoOut.push_back(0x2b);
	
	const u8 sectorSizeLSB = (info.SectorSize & 0xff);
	fifoOut.push_back(sectorSizeLSB);

	const u8 sectorSizeMSB = (info.SectorSize >> 8);
	fifoOut.push_back(sectorSizeMSB);

	const u8 eraseBlockSizeLSB = (info.EraseBlockSizeInSectors & 0xff);
	fifoOut.push_back(eraseBlockSizeLSB);

	const u8 eraseBlockSizeMSB = (info.EraseBlockSizeInSectors >> 8);
	fifoOut.push_back(eraseBlockSizeMSB);

	const u8 sectorCountLSB = (info.McdSizeInSectors & 0xff);
	fifoOut.push_back(sectorCountLSB);

	const u8 sectorCount2nd = (info.McdSizeInSectors >> 8);
	fifoOut.push_back(sectorCount2nd);

	const u8 sectorCount3rd = (info.McdSizeInSectors >> 16);
	fifoOut.push_back(sectorCount3rd);

	const u8 sectorCountMSB = (info.McdSizeInSectors >> 24);
	fifoOut.push_back(sectorCountMSB);
	
	fifoOut.push_back(info.Xor);
	fifoOut.push_back(mcd->term);
}

void MemoryCardProtocol::SetTerminator()
{
	if (this->PS1Fail()) return;
	/* The reply carries the terminator just set: newer MCMAN revisions
	 * check [4] for the value they sent. */
	mcd->term = fifoIn.front();
	fifoIn.pop_front();
	fifoOut.push_back(0x00);
	fifoOut.push_back(0x2b);
	fifoOut.push_back(mcd->term);
}

void MemoryCardProtocol::GetTerminator()
{
	if (this->PS1Fail()) return;
	fifoOut.push_back(0x2b);
	fifoOut.push_back(mcd->term);
	// MCMAN revisions check byte [3] and/or byte [4] for the terminator and
	// expect to read back a valid terminator value. Older revisions only ever
	// use 0x55, but newer ones set the terminator to another value (commonly
	// 0x5a) via SetTerminator. Echo the terminator the card actually holds
	// rather than a hardcoded default, so a game that set a custom terminator
	// reads back the same value here.
	fifoOut.push_back(mcd->term);
}

void MemoryCardProtocol::WriteData()
{
	if (this->PS1Fail()) return;
	fifoOut.push_back(0x00);
	fifoOut.push_back(0x2b);
	const u8 writeLength = fifoIn.front();
	fifoIn.pop_front();
	u8 checksum = 0x00;
	/* writeLength is a u8, so at most 255 bytes are ever buffered. */
	u8 buf[256];

	for (size_t writeCounter = 0; writeCounter < writeLength; writeCounter++)
	{
		const u8 writeByte = fifoIn.front();
		fifoIn.pop_front();
		checksum ^= writeByte;
		buf[writeCounter] = writeByte;
		fifoOut.push_back(0x00);
	}

	FileMcd_Save(mcd->port, mcd->slot, buf, mcd->transferAddr, writeLength);
	fifoOut.push_back(checksum);
	fifoOut.push_back(mcd->term);

	ReadWriteIncrement(writeLength);
}

void MemoryCardProtocol::ReadData()
{
	if (this->PS1Fail()) return;
	const u8 readLength = fifoIn.front();
	fifoIn.pop_front();
	fifoOut.push_back(0x00);
	fifoOut.push_back(0x2b);
	/* readLength is a u8, so at most 255 bytes are ever read. */
	u8 buf[256];
	FileMcd_Read(mcd->port, mcd->slot, buf, mcd->transferAddr, readLength);
	u8 checksum = 0x00;

	for (size_t i = 0; i < readLength; i++)
	{
		const u8 readByte = buf[i];
		checksum ^= readByte;
		fifoOut.push_back(readByte);
	}

	fifoOut.push_back(checksum);
	fifoOut.push_back(mcd->term);

	ReadWriteIncrement(readLength);
}

u8 MemoryCardProtocol::PS1Read(u8 data)
{
	bool sendAck = true;
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
			RecalculatePS1Addr();
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
			sendAck = false;
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

	if (sendAck)
		sio0.stat |= SIO0_STAT::ACK;

	ps1McState.currentByte++;
	return ret;
}

u8 MemoryCardProtocol::PS1State(u8 data)
{
	return 0x00;
}

u8 MemoryCardProtocol::PS1Write(u8 data)
{
	bool sendAck = true;
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
			RecalculatePS1Addr();
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
				// Clear the "directory unread" bit of the flag byte. Per no$psx, this is cleared
				// on writes, not reads.
				mcd->FLAG &= 0x07;
			}

			sendAck = false;
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

	if (sendAck)
		sio0.stat |= SIO0_STAT::ACK;

	ps1McState.currentByte++;
	return ret;
}

u8 MemoryCardProtocol::PS1Pocketstation(u8 data)
{
	sio2.SetRecv1(Recv1::DISCONNECTED);
	return 0x00;
}

void MemoryCardProtocol::ReadWriteEnd()
{
	if (this->PS1Fail()) return;
	The2bTerminator(4);
}

void MemoryCardProtocol::EraseBlock()
{
	if (this->PS1Fail()) return;
	FileMcd_EraseBlock(mcd->port, mcd->slot, mcd->transferAddr);
	The2bTerminator(4);
}

void MemoryCardProtocol::UnknownBoot()
{
	if (this->PS1Fail()) return;
	The2bTerminator(5);
}

/* The card's side of keyed MagicGate: values go out and come in last
 * byte first, an outgoing one followed by its XOR. False for the modes
 * that carry nothing, which take the unkeyed path. */
bool MemoryCardProtocol::AuthXorKeyed(u8 modeByte)
{
	const int slot = (int)sioConvertPortAndSlotToPad(mcd->port, mcd->slot);
	u8 v[8];
	size_t i;

	if (mecha_mg_card_get(slot, modeByte, v))
	{
		u8 xorResult = 0;
		fifoOut.push_back(0x00);
		fifoOut.push_back(0x2b);
		for (i = 0; i < 8; i++)
		{
			if (!fifoIn.empty())
				fifoIn.pop_front();
			xorResult ^= v[7 - i];
			fifoOut.push_back(v[7 - i]);
		}
		fifoOut.push_back(xorResult);
		fifoOut.push_back(mcd->term);
		return true;
	}
	if (modeByte == 0x06 || modeByte == 0x07 || modeByte == 0x0b)
	{
		for (i = 0; i < 8; i++)
		{
			v[7 - i] = fifoIn.empty() ? 0 : fifoIn.front();
			if (!fifoIn.empty())
				fifoIn.pop_front();
		}
		mecha_mg_card_put(slot, modeByte, v);
		The2bTerminator(14);
		return true;
	}
	return false;
}

void MemoryCardProtocol::AuthXor()
{
	if (this->PS1Fail()) return;
	const u8 modeByte = fifoIn.front();
	fifoIn.pop_front();

	if (mecha_mg_keyed() && AuthXorKeyed(modeByte))
		return;

	switch (modeByte)
	{
		// When encountered, the command length in RECV3 is guaranteed to be 14,
		// and the PS2 is expecting us to XOR the data it is about to send.
		case 0x01:
		case 0x02:
		case 0x04:
		case 0x0f:
		case 0x11:
		case 0x13:
		{
			// Long + XOR
			fifoOut.push_back(0x00);
			fifoOut.push_back(0x2b);
			u8 xorResult = 0x00;

			for (size_t xorCounter = 0; xorCounter < 8; xorCounter++)
			{
				const u8 toXOR = fifoIn.front();
				fifoIn.pop_front();
				xorResult ^= toXOR;
				fifoOut.push_back(0x00);
			}

			fifoOut.push_back(xorResult);
			fifoOut.push_back(mcd->term);
			break;
		}
		// When encountered, the command length in RECV3 is guaranteed to be 5,
		// and there is no attempt to XOR anything.
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
			// Short + No XOR
			The2bTerminator(5);
			break;
		// When encountered, the command length in RECV3 is guaranteed to be 14,
		// and the PS2 is about to send us data, BUT the PS2 does NOT want us
		// to send the XOR, it wants us to send the 0x2b and terminator as the
		// last two bytes.
		case 0x06:
		case 0x07:
		case 0x0b:
			// Long + No XOR
			The2bTerminator(14);
			break;
		default:
			break;
	}
}

void MemoryCardProtocol::AuthF3()
{
	if (this->PS1Fail()) return;
	/* An authentication reset puts the card back on the default
	 * terminator, which MCMAN sets again once the card is authenticated. */
	mcd->term = Terminator::DEFAULT;
	{
		const int pad = (int)sioConvertPortAndSlotToPad(mcd->port, mcd->slot);
		/* The ID this card answers the handshake with: its own, when the
		 * user gave one (a real dongle's), else the emulated card's. */
		mecha_mg_card_set_id(pad, FileMcd_GetMagicGateId(mcd->port, mcd->slot));
		mecha_mg_card_auth_reset(pad);
	}
	The2bTerminator(5);
}

void MemoryCardProtocol::AuthF7()
{
	if (this->PS1Fail()) return;
	The2bTerminator(5);
}
