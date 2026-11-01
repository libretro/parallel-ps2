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

#include "common/Pcsx2Defs.h"

#include "DEV9/PacketReader/MAC_Address.h"
#include <cstring>
#include "DEV9/PacketReader/IP/IP_Address.h"

#ifdef _WIN32
#include "winsock.h"
#else
#include <arpa/inet.h>
#endif

/* Fields of a packet sit wherever its layout puts them, an IPv4 address
 * at byte 26 of a frame: read and written through memcpy, which is a
 * plain load or store where that is allowed and a correct one where an
 * unaligned access is not. */
namespace PacketReader::NetLib
{
	// Write.
	inline void WriteByte08(u8* data, int* index, u8 value)
	{
		data[*index] = value;
		*index += sizeof(u8);
	}
	inline void WriteUInt16(u8* data, int* index, u16 value)
	{
		const u16 v = htons(value);
		memcpy(&data[*index], &v, sizeof(v));
		*index += sizeof(u16);
	}
	inline void WriteUInt32(u8* data, int* index, u32 value)
	{
		const u32 v = htonl(value);
		memcpy(&data[*index], &v, sizeof(v));
		*index += sizeof(u32);
	}

	// Special write.
	inline void WriteMACAddress(u8* data, int* index, PacketReader::MAC_Address value)
	{
		memcpy(&data[*index], &value, sizeof(value));
		*index += sizeof(PacketReader::MAC_Address);
	}
	inline void WriteIPAddress(u8* data, int* index, PacketReader::IP::IP_Address value)
	{
		memcpy(&data[*index], &value, sizeof(value));
		*index += sizeof(PacketReader::IP::IP_Address);
	}
	inline void WriteByteArray(u8* data, int* index, int length, u8* value)
	{
		memcpy(&data[*index], value, length);
		*index += length;
	}

	// Read.
	inline void ReadByte08(u8* data, int* index, u8* value)
	{
		*value = data[*index];
		*index += sizeof(u8);
	}
	inline void ReadUInt16(u8* data, int* index, u16* value)
	{
		u16 v;
		memcpy(&v, &data[*index], sizeof(v));
		*value = ntohs(v);
		*index += sizeof(u16);
	}
	inline void ReadUInt32(u8* data, int* index, u32* value)
	{
		u32 v;
		memcpy(&v, &data[*index], sizeof(v));
		*value = ntohl(v);
		*index += sizeof(u32);
	}

	// Special read.
	inline void ReadMACAddress(u8* data, int* index, PacketReader::MAC_Address* value)
	{
		memcpy(value, &data[*index], sizeof(*value));
		*index += sizeof(PacketReader::MAC_Address);
	}
	inline void ReadIPAddress(u8* data, int* index, PacketReader::IP::IP_Address* value)
	{
		memcpy(value, &data[*index], sizeof(*value));
		*index += sizeof(PacketReader::IP::IP_Address);
	}
	inline void ReadByteArray(u8* data, int* index, int length, u8* value)
	{
		memcpy(value, &data[*index], length);
		*index += length;
	}
} // namespace PacketReader::NetLib
