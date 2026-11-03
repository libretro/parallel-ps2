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

#define _PC_ // disables MIPS opcode macros.

#include "HostFS.h"
#include "StringView.h"
#include "ParseNumber.h"
#include "common/Pcsx2Defs.h"
#include <encodings/deflate.h>

#include "Config.h"
#include "Common.h"
#include "Patch.h"
#include "IopMem.h"

#include <memory>
#include <sstream>
#include <vector>

#include <file/file_path.h>

/* Split on a delimiter, each piece stripped of surrounding whitespace;
 * a patch line. */
static std::string_view StripWhitespace(const std::string_view& str)
{
	std::string_view::size_type start = 0;
	while (start < str.size() && std::isspace(str[start]))
		start++;
	if (start == str.size())
		return {};

	std::string_view::size_type end = str.size() - 1;
	while (end > start && std::isspace(str[end]))
		end--;

	return str.substr(start, end - start + 1);
}

static std::vector<std::string_view> SplitString(const std::string_view& str, char delimiter, bool skip_empty /*= true*/)
{
	std::vector<std::string_view> res;
	std::string_view::size_type last_pos = 0;
	std::string_view::size_type pos;
	while (last_pos < str.size() && (pos = str.find(delimiter, last_pos)) != std::string_view::npos)
	{
		std::string_view part(StripWhitespace(str.substr(last_pos, pos - last_pos)));
		if (!skip_empty || !part.empty())
			res.push_back(std::move(part));

		last_pos = pos + 1;
	}

	if (last_pos < str.size())
	{
		std::string_view part(StripWhitespace(str.substr(last_pos)));
		if (!skip_empty || !part.empty())
			res.push_back(std::move(part));
	}

	return res;
}

enum patch_cpu_type {
	NO_CPU = 0,
	CPU_EE,
	CPU_IOP
};

enum patch_data_type {
	NO_TYPE = 0,
	BYTE_T,
	SHORT_T,
	WORD_T,
	DOUBLE_T,
	EXTENDED_T,
	SHORT_LE_T,
	WORD_LE_T,
	DOUBLE_LE_T,
	BYTES_T      /* arbitrary-length payload; see IniPatch::bytes */
};


static u32 SkipCount          = 0;
static u32 IterationCount     = 0;
static u32 IterationIncrement = 0;
static u32 PrevCheatType      = 0;
static u32 PrevCheatAddr      = 0;
static u32 LastType           = 0;

static void writeCheat(void)
{
	switch (LastType)
	{
		case 0x0:
			memWrite8(PrevCheatAddr, IterationIncrement & 0xFF);
			break;
		case 0x1:
			memWrite16(PrevCheatAddr, IterationIncrement & 0xFFFF);
			break;
		case 0x2:
			memWrite32(PrevCheatAddr, IterationIncrement);
			break;
		default:
			break;
	}
}

static void handle_extended_t(IniPatch *p)
{
	if (SkipCount > 0)
		SkipCount--;
	else switch (PrevCheatType)
	{
	case 0x3040: // vvvvvvvv 00000000 Inc
	{
		u32 mem = memRead32(PrevCheatAddr);
		memWrite32(PrevCheatAddr, mem + (p->addr));
		PrevCheatType = 0;
		break;
	}

	case 0x3050: // vvvvvvvv 00000000 Dec
	{
		u32 mem = memRead32(PrevCheatAddr);
		memWrite32(PrevCheatAddr, mem - (p->addr));
		PrevCheatType = 0;
		break;
	}

	case 0x4000: // vvvvvvvv iiiiiiii
		for (u32 i = 0; i < IterationCount; i++)
		{
			memWrite32((u32)(PrevCheatAddr + (i * IterationIncrement)), (u32)(p->addr + ((u32)p->data * i)));
		}
		PrevCheatType = 0;
		break;

	case 0x5000: // bbbbbbbb 00000000
		for (u32 i = 0; i < IterationCount; i++)
		{
			u8 mem = memRead8(PrevCheatAddr + i);
			memWrite8((p->addr + i) & 0x0FFFFFFF, mem);
		}
		PrevCheatType = 0;
		break;

	case 0x6000: // 000Xnnnn iiiiiiii
	{
		// Get Number of pointers
		if (((u32)p->addr & 0x0000FFFF) == 0)
			IterationCount = 1;
		else
			IterationCount = (u32)p->addr & 0x0000FFFF;

		// Read first pointer
		LastType = ((u32)p->addr & 0x000F0000) >> 16;
		u32 mem = memRead32(PrevCheatAddr);

		PrevCheatAddr = mem + (u32)p->data;
		IterationCount--;

		// Check if needed to read another pointer
		if (IterationCount == 0)
		{
			PrevCheatType = 0;
			if (((mem & 0x0FFFFFFF) & 0x3FFFFFFC) != 0) writeCheat();
		}
		else
		{
			if (((mem & 0x0FFFFFFF) & 0x3FFFFFFC) == 0)
				PrevCheatType = 0;
			else
				PrevCheatType = 0x6001;
		}
	}
		break;

	case 0x6001: // 000Xnnnn iiiiiiii
	{
		// Read first pointer
		u32 mem = memRead32(PrevCheatAddr & 0x0FFFFFFF);

		PrevCheatAddr = mem + (u32)p->addr;
		IterationCount--;

		// Check if needed to read another pointer
		if (IterationCount == 0)
		{
			PrevCheatType = 0;
			if (((mem & 0x0FFFFFFF) & 0x3FFFFFFC) != 0) writeCheat();
		}
		else
		{
			mem = memRead32(PrevCheatAddr);

			PrevCheatAddr = mem + (u32)p->data;
			IterationCount--;
			if (IterationCount == 0)
			{
				PrevCheatType = 0;
				if (((mem & 0x0FFFFFFF) & 0x3FFFFFFC) != 0) writeCheat();
			}
		}
	}
		break;

	default:
		if ((p->addr & 0xF0000000) == 0x00000000)				// 0aaaaaaa 0000000vv
		{
			memWrite8(p->addr & 0x0FFFFFFF, (u8)p->data & 0x000000FF);
			PrevCheatType = 0;
		}
		else if ((p->addr & 0xF0000000) == 0x10000000)			// 1aaaaaaa 0000vvvv
		{
			memWrite16(p->addr & 0x0FFFFFFF, (u16)p->data & 0x0000FFFF);
			PrevCheatType = 0;
		}
		else if ((p->addr & 0xF0000000) == 0x20000000)			// 2aaaaaaa vvvvvvvv
		{
			memWrite32(p->addr & 0x0FFFFFFF, (u32)p->data);
			PrevCheatType = 0;
		}
		else if ((p->addr & 0xFFFF0000) == 0x30000000)			// 300000vv 0aaaaaaa Inc
		{
			u8 mem = memRead8((u32)p->data);
			memWrite8((u32)p->data, mem + (p->addr & 0x000000FF));
			PrevCheatType = 0;
		}
		else if ((p->addr & 0xFFFF0000) == 0x30100000)			// 301000vv 0aaaaaaa Dec
		{
			u8 mem = memRead8((u32)p->data);
			memWrite8((u32)p->data, mem - (p->addr & 0x000000FF));
			PrevCheatType = 0;
		}
		else if ((p->addr & 0xFFFF0000) == 0x30200000)			// 3020vvvv 0aaaaaaa Inc
		{
			u16 mem = memRead16((u32)p->data);
			memWrite16((u32)p->data, mem + (p->addr & 0x0000FFFF));
			PrevCheatType = 0;
		}
		else if ((p->addr & 0xFFFF0000) == 0x30300000)			// 3030vvvv 0aaaaaaa Dec
		{
			u16 mem = memRead16((u32)p->data);
			memWrite16((u32)p->data, mem - (p->addr & 0x0000FFFF));
			PrevCheatType = 0;
		}
		else if ((p->addr & 0xFFFF0000) == 0x30400000)			// 30400000 0aaaaaaa Inc + Another line
		{
			PrevCheatType = 0x3040;
			PrevCheatAddr = (u32)p->data;
		}
		else if ((p->addr & 0xFFFF0000) == 0x30500000)			// 30500000 0aaaaaaa Inc + Another line
		{
			PrevCheatType = 0x3050;
			PrevCheatAddr = (u32)p->data;
		}
		else if ((p->addr & 0xF0000000) == 0x40000000)			// 4aaaaaaa nnnnssss + Another line
		{
			IterationCount = ((u32)p->data & 0xFFFF0000) >> 16;
			IterationIncrement = ((u32)p->data & 0x0000FFFF) * 4;
			PrevCheatAddr = (u32)p->addr & 0x0FFFFFFF;
			PrevCheatType = 0x4000;
		}
		else if ((p->addr & 0xF0000000) == 0x50000000)			// 5sssssss nnnnnnnn + Another line
		{
			PrevCheatAddr = (u32)p->addr & 0x0FFFFFFF;
			IterationCount = ((u32)p->data);
			PrevCheatType = 0x5000;
		}
		else if ((p->addr & 0xF0000000) == 0x60000000)			// 6aaaaaaa 000000vv + Another line/s
		{
			PrevCheatAddr = (u32)p->addr & 0x0FFFFFFF;
			IterationIncrement = ((u32)p->data);
			IterationCount = 0;
			PrevCheatType = 0x6000;
		}
		else if ((p->addr & 0xF0000000) == 0x70000000)
		{
			if ((p->data & 0x00F00000) == 0x00000000)			// 7aaaaaaa 000000vv
			{
				u8 mem = memRead8((u32)p->addr & 0x0FFFFFFF);
				memWrite8((u32)p->addr & 0x0FFFFFFF, (u8)(mem | (p->data & 0x000000FF)));
			}
			else if ((p->data & 0x00F00000) == 0x00100000)		// 7aaaaaaa 0010vvvv
			{
				u16 mem = memRead16((u32)p->addr & 0x0FFFFFFF);
				memWrite16((u32)p->addr & 0x0FFFFFFF, (u16)(mem | (p->data & 0x0000FFFF)));
			}
			else if ((p->data & 0x00F00000) == 0x00200000)		// 7aaaaaaa 002000vv
			{
				u8 mem = memRead8((u32)p->addr & 0x0FFFFFFF);
				memWrite8((u32)p->addr & 0x0FFFFFFF, (u8)(mem & (p->data & 0x000000FF)));
			}
			else if ((p->data & 0x00F00000) == 0x00300000)		// 7aaaaaaa 0030vvvv
			{
				u16 mem = memRead16((u32)p->addr & 0x0FFFFFFF);
				memWrite16((u32)p->addr & 0x0FFFFFFF, (u16)(mem & (p->data & 0x0000FFFF)));
			}
			else if ((p->data & 0x00F00000) == 0x00400000)		// 7aaaaaaa 004000vv
			{
				u8 mem = memRead8((u32)p->addr & 0x0FFFFFFF);
				memWrite8((u32)p->addr & 0x0FFFFFFF, (u8)(mem ^ (p->data & 0x000000FF)));
			}
			else if ((p->data & 0x00F00000) == 0x00500000)		// 7aaaaaaa 0050vvvv
			{
				u16 mem = memRead16((u32)p->addr & 0x0FFFFFFF);
				memWrite16((u32)p->addr & 0x0FFFFFFF, (u16)(mem ^ (p->data & 0x0000FFFF)));
			}
		}
		else if ((p->addr & 0xF0000000) == 0xD0000000 || (p->addr & 0xF0000000) == 0xE0000000)
		{
			u32 addr = (u32)p->addr;
			u32 data = (u32)p->data;

			// Since D-codes now have the additional functionality present in PS2rd which
			// incorporates E-code-like functionality by making use of the unused bits in
			// D-codes, the E-codes are now just converted to D-codes to reduce bloat.

			if ((addr & 0xF0000000) == 0xE0000000)
			{
				// Ezyyvvvv taaaaaaa  ->  Daaaaaaa yytzvvvv
				addr = 0xD0000000 | ((u32)p->data & 0x0FFFFFFF);
				data = 0x00000000 | ((u32)p->addr & 0x0000FFFF);
				data = data | ((u32)p->addr & 0x00FF0000) << 8;
				data = data | ((u32)p->addr & 0x0F000000) >> 8;
				data = data | ((u32)p->data & 0xF0000000) >> 8;
			}

			const u8 type = (data & 0x000F0000) >> 16;
			const u8 cond = (data & 0x00F00000) >> 20;

			if (cond == 0)										// Daaaaaaa yy0zvvvv
			{
				if (type == 0)										// Daaaaaaa yy00vvvv
				{
					u16 mem = memRead16(addr & 0x0FFFFFFF);
					if (mem != (data & 0x0000FFFF))
					{
						SkipCount = (data & 0xFF000000) >> 24;
						if (!SkipCount)
							SkipCount = 1;
					}
					PrevCheatType = 0;
				}
				else if (type == 1)									// Daaaaaaa yy0100vv
				{
					u8 mem = memRead8(addr & 0x0FFFFFFF);
					if (mem != (data & 0x000000FF))
					{
						SkipCount = (data & 0xFF000000) >> 24;
						if (!SkipCount)
							SkipCount = 1;
					}
					PrevCheatType = 0;
				}
			}
			else if (cond == 1)									// Daaaaaaa yy1zvvvv
			{
				if (type == 0)										// Daaaaaaa yy10vvvv
				{
					u16 mem = memRead16(addr & 0x0FFFFFFF);
					if (mem == (data & 0x0000FFFF))
					{
						SkipCount = (data & 0xFF000000) >> 24;
						if (!SkipCount)
							SkipCount = 1;
					}
					PrevCheatType = 0;
				}
				else if (type == 1)									// Daaaaaaa yy1100vv
				{
					u8 mem = memRead8(addr & 0x0FFFFFFF);
					if (mem == (data & 0x000000FF))
					{
						SkipCount = (data & 0xFF000000) >> 24;
						if (!SkipCount)
							SkipCount = 1;
					}
					PrevCheatType = 0;
				}
			}
			else if (cond == 2)									// Daaaaaaa yy2zvvvv
			{
				if (type == 0)										// Daaaaaaa yy20vvvv
				{
					u16 mem = memRead16(addr & 0x0FFFFFFF);
					if (mem >= (data & 0x0000FFFF))
					{
						SkipCount = (data & 0xFF000000) >> 24;
						if (!SkipCount)
							SkipCount = 1;
					}
					PrevCheatType = 0;
				}
				else if (type == 1)									// Daaaaaaa yy2100vv
				{
					u8 mem = memRead8(addr & 0x0FFFFFFF);
					if (mem >= (data & 0x000000FF))
					{
						SkipCount = (data & 0xFF000000) >> 24;
						if (!SkipCount)
							SkipCount = 1;
					}
					PrevCheatType = 0;
				}
			}
			else if (cond == 3)									// Daaaaaaa yy3zvvvv
			{
				if (type == 0)										// Daaaaaaa yy30vvvv
				{
					u16 mem = memRead16(addr & 0x0FFFFFFF);
					if (mem <= (data & 0x0000FFFF))
					{
						SkipCount = (data & 0xFF000000) >> 24;
						if (!SkipCount)
							SkipCount = 1;
					}
					PrevCheatType = 0;
				}
				else if (type == 1)									// Daaaaaaa yy3100vv
				{
					u8 mem = memRead8(addr & 0x0FFFFFFF);
					if (mem <= (data & 0x000000FF))
					{
						SkipCount = (data & 0xFF000000) >> 24;
						if (!SkipCount)
							SkipCount = 1;
					}
					PrevCheatType = 0;
				}
			}
			else if (cond == 4)									// Daaaaaaa yy4zvvvv
			{
				if (type == 0)										// Daaaaaaa yy40vvvv
				{
					u16 mem = memRead16(addr & 0x0FFFFFFF);
					if (mem & (data & 0x0000FFFF))
					{
						SkipCount = (data & 0xFF000000) >> 24;
						if (!SkipCount)
							SkipCount = 1;
					}
					PrevCheatType = 0;
				}
				else if (type == 1)									// Daaaaaaa yy4100vv
				{
					u8 mem = memRead8(addr & 0x0FFFFFFF);
					if (mem & (data & 0x000000FF))
					{
						SkipCount = (data & 0xFF000000) >> 24;
						if (!SkipCount)
							SkipCount = 1;
					}
					PrevCheatType = 0;
				}
			}
			else if (cond == 5)									// Daaaaaaa yy5zvvvv
			{
				if (type == 0)										// Daaaaaaa yy50vvvv
				{
					u16 mem = memRead16(addr & 0x0FFFFFFF);
					if (!(mem & (data & 0x0000FFFF)))
					{
						SkipCount = (data & 0xFF000000) >> 24;
						if (!SkipCount)
							SkipCount = 1;
					}
					PrevCheatType = 0;
				}
				else if (type == 1)									// Daaaaaaa yy5100vv
				{
					u8 mem = memRead8(addr & 0x0FFFFFFF);
					if (!(mem & (data & 0x000000FF)))
					{
						SkipCount = (data & 0xFF000000) >> 24;
						if (!SkipCount)
							SkipCount = 1;
					}
					PrevCheatType = 0;
				}
			}
			else if (cond == 6)									// Daaaaaaa yy6zvvvv
			{
				if (type == 0)										// Daaaaaaa yy60vvvv
				{
					u16 mem = memRead16(addr & 0x0FFFFFFF);
					if (mem | (data & 0x0000FFFF))
					{
						SkipCount = (data & 0xFF000000) >> 24;
						if (!SkipCount)
							SkipCount = 1;
					}
					PrevCheatType = 0;
				}
				else if (type == 1)									// Daaaaaaa yy6100vv
				{
					u8 mem = memRead8(addr & 0x0FFFFFFF);
					if (mem | (data & 0x000000FF))
					{
						SkipCount = (data & 0xFF000000) >> 24;
						if (!SkipCount)
							SkipCount = 1;
					}
					PrevCheatType = 0;
				}
			}
			else if (cond == 7)									// Daaaaaaa yy7zvvvv
			{
				if (type == 0)										// Daaaaaaa yy70vvvv
				{
					u16 mem = memRead16(addr & 0x0FFFFFFF);
					if (!(mem | (data & 0x0000FFFF)))
					{
						SkipCount = (data & 0xFF000000) >> 24;
						if (!SkipCount)
							SkipCount = 1;
					}
					PrevCheatType = 0;
				}
				else if (type == 1)									// Daaaaaaa yy7100vv
				{
					u8 mem = memRead8(addr & 0x0FFFFFFF);
					if (!(mem | (data & 0x000000FF)))
					{
						SkipCount = (data & 0xFF000000) >> 24;
						if (!SkipCount)
							SkipCount = 1;
					}
					PrevCheatType = 0;
				}
			}
		}
	}
}

// Swaps endianess of InputNum
// ex. 01020304 -> 04030201
// BitLength is length of InputNum in bits, ex. double,64  word,32  short,16
static u64 SwapEndian(u64 InputNum, u8 BitLength)
{
	if (BitLength == 64) // DOUBLE_LE_T
	{
		InputNum = (InputNum & 0x00000000FFFFFFFF) << 32 | (InputNum & 0xFFFFFFFF00000000) >> 32; //Swaps 4 bytes
	}
	if ((BitLength == 32)||(BitLength==64)) // WORD_LE_T
	{
		InputNum = (InputNum & 0x0000FFFF0000FFFF) << 16 | (InputNum & 0xFFFF0000FFFF0000) >> 16; // Swaps 2 bytes
	}
	InputNum = (InputNum & 0x00FF00FF00FF00FF) << 8 | (InputNum & 0xFF00FF00FF00FF00) >> 8;   // Swaps 1 byte
	return InputNum;
}



// Only used from Patch.cpp and we don't export this in any h file.
// Patch.cpp itself declares this prototype, so make sure to keep in sync.
void _ApplyPatch(IniPatch *p)
{
	u64 ledata = 0;

	if (p->enabled == 0) return;

	switch (p->cpu)
	{
		case CPU_EE:
			switch (p->type)
			{
				case BYTES_T:
				{
					/* Written a byte at a time from the payload. The
					 * read-before-write the other cases do is per byte
					 * here, which keeps a patch that is already applied
					 * from dirtying memory it did not change -- these
					 * patches are usually code, and are re-applied every
					 * frame when the place is PPT_CONTINUOUSLY. */
					size_t i;
					for (i = 0; i < p->bytes.size(); i++)
					{
						const u32 a = p->addr + (u32)i;
						if (memRead8(a) != p->bytes[i])
							memWrite8(a, p->bytes[i]);
					}
					break;
				}

				case BYTE_T:
					if (memRead8(p->addr) != (u8)p->data)
						memWrite8(p->addr, (u8)p->data);
					break;

				case SHORT_T:
					if (memRead16(p->addr) != (u16)p->data)
						memWrite16(p->addr, (u16)p->data);
					break;

				case WORD_T:
					if (memRead32(p->addr) != (u32)p->data)
						memWrite32(p->addr, (u32)p->data);
					break;

				case DOUBLE_T:
					if (memRead64(p->addr) != (u64)p->data)
						memWrite64(p->addr, (u64)p->data);
					break;

				case EXTENDED_T:
					handle_extended_t(p);
					break;

				case SHORT_LE_T:
					ledata = SwapEndian(p->data, 16);
					if (memRead16(p->addr) != (u16)ledata)
						memWrite16(p->addr, (u16)ledata);
					break;

				case WORD_LE_T:
					ledata = SwapEndian(p->data, 32);
					if (memRead32(p->addr) != (u32)ledata)
						memWrite32(p->addr, (u32)ledata);
					break;

				case DOUBLE_LE_T:
					ledata = SwapEndian(p->data, 64);
					if (memRead64(p->addr) != (u64)ledata)
						memWrite64(p->addr, (u64)ledata);
					break;

				default:
					break;
			}
			break;

		case CPU_IOP:
			switch (p->type)
			{
				case BYTES_T:
				{
					size_t k;
					for (k = 0; k < p->bytes.size(); k++)
					{
						const u32 a = p->addr + (u32)k;
						if (iopMemRead8(a) != p->bytes[k])
							iopMemWrite8(a, p->bytes[k]);
					}
					break;
				}
				case BYTE_T:
					if (iopMemRead8(p->addr) != (u8)p->data)
						iopMemWrite8(p->addr, (u8)p->data);
					break;
				case SHORT_T:
					if (iopMemRead16(p->addr) != (u16)p->data)
						iopMemWrite16(p->addr, (u16)p->data);
					break;
				case WORD_T:
					if (iopMemRead32(p->addr) != (u32)p->data)
						iopMemWrite32(p->addr, (u32)p->data);
					break;
				default:
					break;
			}
			break;

		default:
			break;
	}
}

static void _ApplyDynaPatch(const DynamicPatch& patch, u32 address)
{
	for (const auto& pattern : patch.pattern)
	{
		if (*static_cast<u32*>(PSM(address + pattern.offset)) != pattern.value)
			return;
	}

	log_cb(RETRO_LOG_INFO, "Applying Dynamic Patch to address 0x%08X\n", address);
	// If everything passes, apply the patch.
	for (const auto& replacement : patch.replacement)
		memWrite32(address + replacement.offset, replacement.value);
}

// This is a declaration for PatchMemory.cpp::_ApplyPatch where we're (patch.cpp)
// the only consumer, so it's not made public via Patch.h
// Applies a single patch line to emulation memory regardless of its "place" value.

std::vector<IniPatch> Patch;
static std::vector<DynamicPatch> DynaPatch;

struct PatchTextTable
{
	int code;
	const char* text;
	PATCHTABLEFUNC* func;
};

static const PatchTextTable commands_patch[] =
	{
		{1, "author", PatchFunc::author},
		{2, "comment", PatchFunc::comment},
		{3, "patch", PatchFunc::patch},
		{0, nullptr, nullptr} // Array Terminator
};

static const PatchTextTable dataType[] =
	{
		{1, "byte", nullptr},
		{2, "short", nullptr},
		{3, "word", nullptr},
		{4, "double", nullptr},
		{5, "extended", nullptr},
		{6, "leshort", nullptr},
		{7, "leword", nullptr},
		{8, "ledouble", nullptr},
		{9, "bytes", nullptr},
		{0, nullptr, nullptr} // Array Terminator
};

static const PatchTextTable cpuCore[] =
	{
		{1, "EE", nullptr},
		{2, "IOP", nullptr},
		{0, nullptr, nullptr} // Array Terminator
};

// IniFile Functions.

/* Trim a pnach line in place: leading and trailing whitespace off, and
 * everything from a "//" onward dropped -- a line that starts with one is
 * a comment and becomes empty. Was StripWhitespace plus find/erase on a
 * std::string; the same rules on the caller's buffer, since the line is
 * read from a stream into a buffer either way. */
static void inifile_trim(char* buffer)
{
	char* start = buffer;
	char* end;
	char* comment;

	while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n')
		start++;

	end = start + strlen(start);
	while (end > start && (end[-1] == ' ' || end[-1] == '\t'
				|| end[-1] == '\r' || end[-1] == '\n'))
		end--;
	*end = '\0';

	comment = strstr(start, "//");
	if (comment)
		*comment = '\0';

	if (start != buffer)
		memmove(buffer, start, strlen(start) + 1);
}

static int PatchTableExecute(const std::string_view& lhs, const std::string_view& rhs, const PatchTextTable* Table)
{
	int i = 0;

	while (Table[i].text)
	{
		if (lhs.compare(Table[i].text) == 0)
		{
			if (Table[i].func)
				Table[i].func(lhs, rhs);
			break;
		}
		i++;
	}

	return Table[i].code;
}

// This routine is for executing the commands of the ini file.
static void inifile_command(const char* cmd)
{
	std::string_view key, value;
	StringView::ParseAssignmentString(cmd, &key, &value);

	// Is this really what we want to be doing here? Seems like just leaving it empty/blank
	// would make more sense... --air
	if (value.empty())
		value = key;

	/*int code = */ PatchTableExecute(key, value, commands_patch);
}

int LoadPatchesFromString(const char* patches)
{
	const size_t before = Patch.size();
	const char*  p      = patches;

	/* Walk the buffer a line at a time rather than through an
	 * istringstream and a std::string per line. A pnach line is bounded
	 * -- the format is one command per line -- so anything longer than
	 * the buffer is malformed and gets truncated rather than growing an
	 * allocation to hold it. */
	while (p && *p)
	{
		char        line[512];
		const char* nl  = strchr(p, '\n');
		size_t      len = nl ? (size_t)(nl - p) : strlen(p);

		if (len >= sizeof(line))
			len = sizeof(line) - 1;
		memcpy(line, p, len);
		line[len] = '\0';

		inifile_trim(line);
		if (line[0])
			inifile_command(line);

		if (!nl)
			break;
		p = nl + 1;
	}

	return static_cast<int>(Patch.size() - before);
}

void ForgetLoadedPatches(void)
{
	Patch.clear();
	DynaPatch.clear();
}

// This routine loads patches from a zip file
// Returns number of patches loaded
// Note: does not reset previously loaded patches (use ForgetLoadedPatches() for that)
// Note: only load patches from the root folder of the zip
int LoadPatchesFromZip(const std::string& crc, const u8* zip_data, size_t zip_data_size)
{
	/* Minimal in-memory ZIP reader over rinflate, replacing libzip:
	   the entire consumer surface here is "find one named member in a
	   buffer and inflate it".  End-of-central-directory is located by
	   scanning back over a possible comment; entries are matched by
	   name from the central directory; stored members are copied and
	   deflated members run through one raw-inflate pass sized by the
	   central-directory's own uncompressed size.  Anything malformed
	   simply fails to match, as with a missing member. */
	const std::string pnach_filename(crc + ".pnach");

	auto rd16 = [](const u8* p) -> u32 { return p[0] | (p[1] << 8); };
	auto rd32 = [](const u8* p) -> u32 { return p[0] | (p[1] << 8) | (p[2] << 16) | (u32(p[3]) << 24); };

	if (!zip_data || zip_data_size < 22)
		return 0;

	/* EOCD: signature 0x06054b50, within the last 64KB+22 */
	const size_t scan_max = pcsx2_min_sz(zip_data_size, 22 + 65535);
	size_t eocd = SIZE_MAX;
	for (size_t back = 22; back <= scan_max; back++)
	{
		const u8* p = zip_data + zip_data_size - back;
		if (rd32(p) == 0x06054b50)
		{
			eocd = zip_data_size - back;
			break;
		}
	}
	if (eocd == SIZE_MAX)
		return 0;

	const u8* e = zip_data + eocd;
	u32 cd_count  = rd16(e + 10);
	u32 cd_offset = rd32(e + 16);
	if (cd_offset >= zip_data_size)
		return 0;

	const u8* p   = zip_data + cd_offset;
	const u8* end = zip_data + zip_data_size;
	std::string pnach_data;
	bool found = false;

	for (u32 i = 0; i < cd_count && !found; i++)
	{
		if (p + 46 > end || rd32(p) != 0x02014b50)
			break;
		const u32 method    = rd16(p + 10);
		const u32 comp_size = rd32(p + 20);
		const u32 uncomp    = rd32(p + 24);
		const u32 name_len  = rd16(p + 28);
		const u32 extra_len = rd16(p + 30);
		const u32 comm_len  = rd16(p + 32);
		const u32 lho       = rd32(p + 42);
		const u8* name      = p + 46;
		if (name + name_len > end)
			break;

		if (name_len == pnach_filename.size() &&
			Strncasecmp(reinterpret_cast<const char*>(name), pnach_filename.c_str(), name_len) == 0)
		{
			/* local header: skip its own (possibly different) name/extra */
			if (lho + 30 > zip_data_size || rd32(zip_data + lho) != 0x04034b50)
				return 0;
			const u32 lname = rd16(zip_data + lho + 26);
			const u32 lextra = rd16(zip_data + lho + 28);
			const size_t data_off = size_t(lho) + 30 + lname + lextra;
			if (data_off + comp_size > zip_data_size)
				return 0;
			const u8* src = zip_data + data_off;

			if (method == 0)
			{
				if (comp_size != uncomp)
					return 0;
				pnach_data.assign(reinterpret_cast<const char*>(src), uncomp);
				found = true;
			}
			else if (method == 8)
			{
				pnach_data.resize(uncomp);
				void* z = rinflate_new(-15); /* raw deflate, as ZIP stores it */
				if (!z)
					return 0;
				rinflate_set_in(z, src, comp_size);
				rinflate_set_out(z, reinterpret_cast<uint8_t*>(pnach_data.data()), uncomp);
				size_t rdn = 0, wr = 0;
				const int r = rinflate_process(z, &rdn, &wr);
				rinflate_free(z);
				if (r != RDEFLATE_PROCESS_END || wr != uncomp)
					return 0;
				found = true;
			}
			else
			{
				/* zstd-compressed members (method 93) were only ever
				   reachable through libzip's optional backend; pnach
				   archives are deflate in practice. */
				return 0;
			}
		}

		p += 46 + name_len + extra_len + comm_len;
	}

	if (!found)
		return 0;

	log_cb(RETRO_LOG_INFO, "Loading patch '%s' from archive.\n", pnach_filename.c_str());
	return LoadPatchesFromString(pnach_data.c_str());
}

// This routine loads patches from *.pnach files
// Returns number of patches loaded
// Note: does not reset previously loaded patches (use ForgetLoadedPatches() for that)
int LoadPatchesFromDir(const std::string& crc, const std::string& folder, const char* friendly_name, bool show_error_when_missing)
{
	if (!path_is_directory(folder.c_str()))
	{
		log_cb(RETRO_LOG_INFO, "The %s folder ('%s') is inaccessible. Skipping...\n", friendly_name, folder.c_str());
		return 0;
	}

	FileSystem::FindResultsArray files;
	/* The CRC filter happens below, on the results: filenames are
	 * matched by prefix so "ABCD1234 - fix.pnach" styles all load.
	 * The pattern itself is every pnach in the folder. */
	FileSystem::FindFiles(folder.c_str(), "*.pnach",
		FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_HIDDEN_FILES, &files);

	if (show_error_when_missing && files.empty())
	{
		log_cb(RETRO_LOG_INFO, "Not found %s file: %s\n" FS_OSPATH_SEPARATOR_STR "%s.pnach",
			friendly_name, folder.c_str(), crc.c_str());
	}

	int total_loaded = 0;

	for (const FILESYSTEM_FIND_DATA& fd : files)
	{
		const std::string_view name(path_basename(fd.FileName.c_str()));
		if (name.length() < crc.length() || Strncasecmp(name.data(), crc.c_str(), crc.size()) != 0)
			continue;

		log_cb(RETRO_LOG_INFO, "Found %s file: '%.*s'\n", friendly_name, static_cast<int>(name.size()), name.data());

		void* pnach_buf = nullptr;
		int64_t pnach_len = 0;
		std::optional<std::string> pnach_data;
		if (filestream_read_file(fd.FileName.c_str(), &pnach_buf, &pnach_len))
		{
			pnach_data.emplace();
			if (pnach_len > 0)
				pnach_data->assign(static_cast<const char*>(pnach_buf), static_cast<size_t>(pnach_len));
			free(pnach_buf);
		}
		if (!pnach_data.has_value())
			continue;

		const int loaded = LoadPatchesFromString(pnach_data.value().c_str());
		total_loaded += loaded;

		log_cb(RETRO_LOG_INFO, "Loaded %d %s from '%.*s'.\n",
			loaded, friendly_name, static_cast<int>(name.size()), name.data());
	}

	log_cb(RETRO_LOG_INFO, "Overall %d %s loaded\n", total_loaded, friendly_name);
	return total_loaded;
}

// PatchFunc Functions.
namespace PatchFunc
{
	void comment(const std::string_view& text1, const std::string_view& text2)
	{
		log_cb(RETRO_LOG_INFO, "comment: %.*s\n", static_cast<int>(text2.length()), text2.data());
	}

	void author(const std::string_view& text1, const std::string_view& text2)
	{
		log_cb(RETRO_LOG_INFO, "Author: %.*s\n", static_cast<int>(text2.length()), text2.data());
	}

	void patch(const std::string_view& cmd, const std::string_view& param)
	{
#define PATCH_ERROR(fmt, ...) log_cb(RETRO_LOG_ERROR, "(Patch) Error Parsing: %.*s=%.*s: \n" fmt, \
	static_cast<int>(cmd.size()), cmd.data(), static_cast<int>(param.size()), param.data(), \
	__VA_ARGS__)

		// [0]=PlaceToPatch,[1]=CpuType,[2]=MemAddr,[3]=OperandSize,[4]=WriteValue
		const std::vector<std::string_view> pieces(SplitString(param, ',', false));
		if (pieces.size() != 5)
		{
			PATCH_ERROR("Expected 5 data parameters; only found %zu", pieces.size());
			return;
		}

		IniPatch iPatch = {0};
		iPatch.enabled = 0;
		{
			u32 place = _PPT_END_MARKER;
			parse_number_u32(pieces[0].data(), pieces[0].size(), 10, &place);
			iPatch.placetopatch = place;
		}

		if (iPatch.placetopatch >= _PPT_END_MARKER)
		{
			PATCH_ERROR("Invalid 'place' value '%.*s' (0 - once on startup, 1: continuously)",
				static_cast<int>(pieces[0].size()), pieces[0].data());
			return;
		}

		iPatch.cpu = (patch_cpu_type)PatchTableExecute(pieces[1], std::string_view(), cpuCore);
		iPatch.addr = 0;
		parse_number_u32(pieces[2].data(), pieces[2].size(), 16, &iPatch.addr);
		iPatch.type = (patch_data_type)PatchTableExecute(pieces[3], std::string_view(), dataType);

		if (iPatch.type == BYTES_T)
		{
			/* A hex string of whole bytes, applied as written rather than
			 * byte-swapped: the payload is a byte sequence, not a value of
			 * some width, so there is no endianness to apply to it. */
			const std::string_view hex = pieces[4];
			size_t i;

			if (hex.empty() || (hex.size() & 1))
			{
				PATCH_ERROR("'bytes' payload must be whole bytes: '%.*s'",
				            static_cast<int>(hex.size()), hex.data());
				return;
			}

			iPatch.bytes.reserve(hex.size() / 2);
			for (i = 0; i < hex.size(); i += 2)
			{
				u8 b;
				/* both characters hex: the parse alone stops at the first
				 * that is not, and "0g" would be a byte of 0 */
				if (!isxdigit((unsigned char)hex[i]) || !isxdigit((unsigned char)hex[i + 1])
					|| !parse_number_u8(hex.data() + i, 2, 16, &b))
				{
					PATCH_ERROR("'bytes' payload is not hex: '%.*s'",
					            static_cast<int>(hex.size()), hex.data());
					return;
				}
				iPatch.bytes.push_back(b);
			}
			iPatch.data = 0;
		}
		else
		{
			iPatch.data = 0;
			parse_number_u64(pieces[4].data(), pieces[4].size(), 16, &iPatch.data);
		}

		if (iPatch.cpu == 0)
		{
			PATCH_ERROR("Unrecognized CPU Target: '%.*s'", static_cast<int>(pieces[1].size()), pieces[1].data());
			return;
		}

		if (iPatch.type == 0)
		{
			PATCH_ERROR("Unrecognized Operand Size: '%.*s'", static_cast<int>(pieces[3].size()), pieces[3].data());
			return;
		}

		iPatch.enabled = 1;
		Patch.push_back(iPatch);

#undef PATCH_ERROR
	}
} // namespace PatchFunc

void ApplyDynamicPatches(u32 pc)
{
	for (const auto& dynpatch : DynaPatch)
	{
		_ApplyDynaPatch(dynpatch, pc);
	}
}

void LoadDynamicPatches(const std::vector<DynamicPatch>& patches)
{
	for (const DynamicPatch& it : patches){
		DynaPatch.push_back(it);
	}
}
