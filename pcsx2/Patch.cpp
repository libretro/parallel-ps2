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

#include "HostFS.h"   /* FS_OSPATH_SEPARATOR_* */
#include "ParseNumber.h"
#include "common/Pcsx2Defs.h"
#include <encodings/deflate.h>

#include "Config.h"
#include "Common.h"
#include "Patch.h"
#include "IopMem.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <compat/fnmatch.h>
#include <file/file_path.h>
#include <retro_dirent.h>
#include <streams/file_stream.h>

/* A run of a pnach line: a pointer into the line and a length. */
struct patch_piece
{
	const char* s;
	size_t n;
};

/* Whitespace off both ends of a run; all whitespace leaves it empty. */
static void patch_strip(struct patch_piece* p)
{
	while (p->n && isspace((unsigned char)p->s[0]))
	{
		p->s++;
		p->n--;
	}
	while (p->n && isspace((unsigned char)p->s[p->n - 1]))
		p->n--;
}

/* The comma-separated fields of a run, each stripped, the first max of
 * them into out; returns how many there are. Empty fields count, but a
 * trailing comma does not start one, as SplitString had it. */
static size_t patch_split(const char* s, size_t n, struct patch_piece* out, size_t max)
{
	size_t count = 0, last = 0, i;
	for (i = 0; i <= n; i++)
	{
		if (i < n && s[i] != ',')
			continue;
		if (i == n && last >= n)
			break;
		if (count < max)
		{
			out[count].s = s + last;
			out[count].n = i - last;
			patch_strip(&out[count]);
		}
		count++;
		last = i + 1;
	}
	return count;
}

static bool patch_piece_is(const struct patch_piece* p, const char* text)
{
	const size_t n = strlen(text);
	return p->n == n && memcmp(p->s, text, n) == 0;
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
					for (i = 0; i < p->nbytes; i++)
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
					for (k = 0; k < p->nbytes; k++)
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

/* The loaded patches, in load order, and the dynamic patches' entries:
 * plain arrays that double when full, emptied (not freed) by
 * ForgetLoadedPatches. A dynamic patch is a run of pattern entries then
 * a run of replacement entries in s_dyna_entries, copied out of the game
 * database so a reload of it does not leave them dangling. */
struct dyna_patch
{
	size_t first;
	u32 npattern;
	u32 nreplacement;
};

static IniPatch* s_patches;
static size_t s_patch_count, s_patch_cap;
static struct dyna_patch* s_dyna;
static size_t s_dyna_count, s_dyna_cap;
static DynamicPatchEntry* s_dyna_entries;
static size_t s_dyna_entry_count, s_dyna_entry_cap;

/* Room in *items for need elements of elem bytes; zero if it cannot be
 * made, leaving the array as it was. */
static int patch_reserve(void** items, size_t* cap, size_t need, size_t elem)
{
	size_t grown = *cap ? *cap : 16;
	void* p;
	if (need <= *cap)
		return 1;
	while (grown < need)
		grown *= 2;
	p = realloc(*items, grown * elem);
	if (!p)
		return 0;
	*items = p;
	*cap   = grown;
	return 1;
}

size_t LoadedPatchCount(void)
{
	return s_patch_count;
}

IniPatch* LoadedPatch(size_t i)
{
	return &s_patches[i];
}

void ApplyLoadedPatches(unsigned place_mask)
{
	size_t i;
	for (i = 0; i < s_patch_count; i++)
	{
		const int place = s_patches[i].placetopatch;
		if (place >= 0 && place < _PPT_END_MARKER && (place_mask & (1u << place)))
			_ApplyPatch(&s_patches[i]);
	}
}

static void _ApplyDynaPatch(const struct dyna_patch* patch, u32 address)
{
	const DynamicPatchEntry* pattern     = s_dyna_entries + patch->first;
	const DynamicPatchEntry* replacement = pattern + patch->npattern;
	u32 i;

	for (i = 0; i < patch->npattern; i++)
	{
		if (*static_cast<u32*>(PSM(address + pattern[i].offset)) != pattern[i].value)
			return;
	}

	log_cb(RETRO_LOG_INFO, "Applying Dynamic Patch to address 0x%08X\n", address);
	// If everything passes, apply the patch.
	for (i = 0; i < patch->nreplacement; i++)
		memWrite32(address + replacement[i].offset, replacement[i].value);
}

typedef void patch_table_func(const struct patch_piece* cmd, const struct patch_piece* param);

static patch_table_func patch_author;
static patch_table_func patch_comment;
static patch_table_func patch_line;

struct PatchTextTable
{
	int code;
	const char* text;
	patch_table_func* func;
};

static const PatchTextTable commands_patch[] =
	{
		{1, "author", patch_author},
		{2, "comment", patch_comment},
		{3, "patch", patch_line},
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

static int PatchTableExecute(const struct patch_piece* lhs, const struct patch_piece* rhs, const PatchTextTable* Table)
{
	int i = 0;

	while (Table[i].text)
	{
		if (patch_piece_is(lhs, Table[i].text))
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
	/* "key = value", each side stripped; no '=' is no command, and runs
	 * the table with nothing, which matches no entry. */
	const char* eq = strchr(cmd, '=');
	struct patch_piece key   = {cmd, 0};
	struct patch_piece value = {cmd, 0};
	if (eq)
	{
		key.n   = (size_t)(eq - cmd);
		value.s = eq + 1;
		value.n = strlen(eq + 1);
		patch_strip(&key);
		patch_strip(&value);
	}

	// Is this really what we want to be doing here? Seems like just leaving it empty/blank
	// would make more sense... --air
	if (value.n == 0)
		value = key;

	/*int code = */ PatchTableExecute(&key, &value, commands_patch);
}

int LoadPatchesFromString(const char* patches)
{
	const size_t before = s_patch_count;
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

	return static_cast<int>(s_patch_count - before);
}

void ForgetLoadedPatches(void)
{
	size_t i;
	for (i = 0; i < s_patch_count; i++)
		free(s_patches[i].bytes);
	s_patch_count      = 0;
	s_dyna_count       = 0;
	s_dyna_entry_count = 0;
}

static u32 rd16(const u8* p) { return p[0] | (p[1] << 8); }
static u32 rd32(const u8* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); }

// This routine loads patches from a zip file
// Returns number of patches loaded
// Note: does not reset previously loaded patches (use ForgetLoadedPatches() for that)
// Note: only load patches from the root folder of the zip
int LoadPatchesFromZip(const char* crc, const u8* zip_data, size_t zip_data_size)
{
	/* Minimal in-memory ZIP reader over rinflate, replacing libzip:
	   the entire consumer surface here is "find one named member in a
	   buffer and inflate it".  End-of-central-directory is located by
	   scanning back over a possible comment; entries are matched by
	   name from the central directory; stored members are copied and
	   deflated members run through one raw-inflate pass sized by the
	   central-directory's own uncompressed size.  Anything malformed
	   simply fails to match, as with a missing member. */
	char pnach_filename[64];
	size_t pnach_len;
	size_t scan_max, eocd = SIZE_MAX, back;
	const u8 *e, *p, *end;
	u32 cd_count, cd_offset, i;
	char* pnach_data = NULL;
	int loaded;

	if (!zip_data || zip_data_size < 22)
		return 0;
	if ((size_t)snprintf(pnach_filename, sizeof(pnach_filename), "%s.pnach", crc) >= sizeof(pnach_filename))
		return 0;
	pnach_len = strlen(pnach_filename);

	/* EOCD: signature 0x06054b50, within the last 64KB+22 */
	scan_max = pcsx2_min_sz(zip_data_size, 22 + 65535);
	for (back = 22; back <= scan_max; back++)
	{
		if (rd32(zip_data + zip_data_size - back) == 0x06054b50)
		{
			eocd = zip_data_size - back;
			break;
		}
	}
	if (eocd == SIZE_MAX)
		return 0;

	e         = zip_data + eocd;
	cd_count  = rd16(e + 10);
	cd_offset = rd32(e + 16);
	if (cd_offset >= zip_data_size)
		return 0;

	p   = zip_data + cd_offset;
	end = zip_data + zip_data_size;

	for (i = 0; i < cd_count && !pnach_data; i++)
	{
		u32 method, comp_size, uncomp, name_len, extra_len, comm_len, lho;
		const u8* name;
		if (p + 46 > end || rd32(p) != 0x02014b50)
			break;
		method    = rd16(p + 10);
		comp_size = rd32(p + 20);
		uncomp    = rd32(p + 24);
		name_len  = rd16(p + 28);
		extra_len = rd16(p + 30);
		comm_len  = rd16(p + 32);
		lho       = rd32(p + 42);
		name      = p + 46;
		if (name + name_len > end)
			break;

		if (name_len == pnach_len &&
			Strncasecmp(reinterpret_cast<const char*>(name), pnach_filename, name_len) == 0)
		{
			u32 lname, lextra;
			size_t data_off;
			const u8* src;

			/* local header: skip its own (possibly different) name/extra */
			if ((size_t)lho + 30 > zip_data_size || rd32(zip_data + lho) != 0x04034b50)
				return 0;
			lname    = rd16(zip_data + lho + 26);
			lextra   = rd16(zip_data + lho + 28);
			data_off = (size_t)lho + 30 + lname + lextra;
			if (data_off + comp_size > zip_data_size)
				return 0;
			src = zip_data + data_off;

			if (method != 0 && method != 8)
			{
				/* zstd-compressed members (method 93) were only ever
				   reachable through libzip's optional backend; pnach
				   archives are deflate in practice. */
				return 0;
			}
			if (method == 0 && comp_size != uncomp)
				return 0;

			pnach_data = (char*)malloc((size_t)uncomp + 1);
			if (!pnach_data)
				return 0;
			pnach_data[uncomp] = '\0';

			if (method == 0)
				memcpy(pnach_data, src, uncomp);
			else
			{
				size_t rdn = 0, wr = 0;
				int r;
				void* z = rinflate_new(-15); /* raw deflate, as ZIP stores it */
				if (!z)
				{
					free(pnach_data);
					return 0;
				}
				rinflate_set_in(z, src, comp_size);
				rinflate_set_out(z, reinterpret_cast<uint8_t*>(pnach_data), uncomp);
				r = rinflate_process(z, &rdn, &wr);
				rinflate_free(z);
				if (r != RDEFLATE_PROCESS_END || wr != uncomp)
				{
					free(pnach_data);
					return 0;
				}
			}
		}

		p += 46 + name_len + extra_len + comm_len;
	}

	if (!pnach_data)
		return 0;

	log_cb(RETRO_LOG_INFO, "Loading patch '%s' from archive.\n", pnach_filename);
	loaded = LoadPatchesFromString(pnach_data);
	free(pnach_data);
	return loaded;
}

// This routine loads patches from *.pnach files
// Returns number of patches loaded
// Note: does not reset previously loaded patches (use ForgetLoadedPatches() for that)
int LoadPatchesFromDir(const char* crc, const char* folder, const char* friendly_name, bool show_error_when_missing)
{
	const size_t crc_len = strlen(crc);
	int total_loaded = 0, found = 0;
	struct RDIR* dir;

	if (!path_is_directory(folder))
	{
		log_cb(RETRO_LOG_INFO, "The %s folder ('%s') is inaccessible. Skipping...\n", friendly_name, folder);
		return 0;
	}

	/* Every pnach in the folder, hidden ones too, whose name starts
	 * with the CRC in either case: "ABCD1234 - fix.pnach" styles all
	 * load. In the directory's own order, as the file search gave it. */
	dir = retro_opendir_include_hidden(folder, true);
	if (dir && !retro_dirent_error(dir))
	{
		while (retro_readdir(dir))
		{
			const char* name = retro_dirent_get_name(dir);
			char path[PCSX2_PATH_MAX];
			void* pnach_buf   = nullptr;
			int64_t pnach_len = 0;
			int loaded;

			if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0')))
				continue;
			if (retro_dirent_is_dir(dir, nullptr) || rl_fnmatch("*.pnach", name, 0) != 0)
				continue;
			if ((size_t)snprintf(path, sizeof(path), "%s%c%s", folder, FS_OSPATH_SEPARATOR_CHARACTER, name) >= sizeof(path))
				continue;
			/* an entry that cannot be sized is not a file (a broken link) */
			if ((s64)path_get_size(path) < 0)
				continue;
			found++;

			if (strlen(name) < crc_len || Strncasecmp(name, crc, crc_len) != 0)
				continue;

			log_cb(RETRO_LOG_INFO, "Found %s file: '%s'\n", friendly_name, name);

			if (!filestream_read_file(path, &pnach_buf, &pnach_len))
				continue;

			/* filestream_read_file terminates what it reads */
			loaded = LoadPatchesFromString(static_cast<const char*>(pnach_buf));
			free(pnach_buf);
			total_loaded += loaded;

			log_cb(RETRO_LOG_INFO, "Loaded %d %s from '%s'.\n", loaded, friendly_name, name);
		}
	}
	if (dir)
		retro_closedir(dir);

	if (show_error_when_missing && !found)
	{
		log_cb(RETRO_LOG_INFO, "Not found %s file: %s" FS_OSPATH_SEPARATOR_STR "%s.pnach\n",
			friendly_name, folder, crc);
	}

	log_cb(RETRO_LOG_INFO, "Overall %d %s loaded\n", total_loaded, friendly_name);
	return total_loaded;
}

// PatchFunc Functions.
static void patch_comment(const struct patch_piece* text1, const struct patch_piece* text2)
{
	(void)text1;
	log_cb(RETRO_LOG_INFO, "comment: %.*s\n", static_cast<int>(text2->n), text2->s);
}

static void patch_author(const struct patch_piece* text1, const struct patch_piece* text2)
{
	(void)text1;
	log_cb(RETRO_LOG_INFO, "Author: %.*s\n", static_cast<int>(text2->n), text2->s);
}

static void patch_line(const struct patch_piece* cmd, const struct patch_piece* param)
{
#define PATCH_ERROR(fmt, ...) log_cb(RETRO_LOG_ERROR, "(Patch) Error Parsing: %.*s=%.*s: \n" fmt, \
	static_cast<int>(cmd->n), cmd->s, static_cast<int>(param->n), param->s, \
	__VA_ARGS__)

	// [0]=PlaceToPatch,[1]=CpuType,[2]=MemAddr,[3]=OperandSize,[4]=WriteValue
	struct patch_piece pieces[5];
	const struct patch_piece none = {"", 0};
	const size_t npieces = patch_split(param->s, param->n, pieces, 5);
	IniPatch iPatch;
	u32 place = _PPT_END_MARKER;

	if (npieces != 5)
	{
		PATCH_ERROR("Expected 5 data parameters; only found %zu", npieces);
		return;
	}

	memset(&iPatch, 0, sizeof(iPatch));
	parse_number_u32(pieces[0].s, pieces[0].n, 10, &place);
	iPatch.placetopatch = place;

	if (iPatch.placetopatch >= _PPT_END_MARKER)
	{
		PATCH_ERROR("Invalid 'place' value '%.*s' (0 - once on startup, 1: continuously)",
			static_cast<int>(pieces[0].n), pieces[0].s);
		return;
	}

	iPatch.cpu = (patch_cpu_type)PatchTableExecute(&pieces[1], &none, cpuCore);
	parse_number_u32(pieces[2].s, pieces[2].n, 16, &iPatch.addr);
	iPatch.type = (patch_data_type)PatchTableExecute(&pieces[3], &none, dataType);

	if (iPatch.type == BYTES_T)
	{
		/* A hex string of whole bytes, applied as written rather than
		 * byte-swapped: the payload is a byte sequence, not a value of
		 * some width, so there is no endianness to apply to it. */
		const struct patch_piece* hex = &pieces[4];
		size_t i;

		if (hex->n == 0 || (hex->n & 1))
		{
			PATCH_ERROR("'bytes' payload must be whole bytes: '%.*s'",
			            static_cast<int>(hex->n), hex->s);
			return;
		}

		for (i = 0; i < hex->n; i += 2)
		{
			/* both characters hex: the parse alone stops at the first
			 * that is not, and "0g" would be a byte of 0 */
			if (!isxdigit((unsigned char)hex->s[i]) || !isxdigit((unsigned char)hex->s[i + 1]))
			{
				PATCH_ERROR("'bytes' payload is not hex: '%.*s'",
				            static_cast<int>(hex->n), hex->s);
				return;
			}
		}
	}
	else
		parse_number_u64(pieces[4].s, pieces[4].n, 16, &iPatch.data);

	if (iPatch.cpu == 0)
	{
		PATCH_ERROR("Unrecognized CPU Target: '%.*s'", static_cast<int>(pieces[1].n), pieces[1].s);
		return;
	}

	if (iPatch.type == 0)
	{
		PATCH_ERROR("Unrecognized Operand Size: '%.*s'", static_cast<int>(pieces[3].n), pieces[3].s);
		return;
	}

	if (!patch_reserve((void**)&s_patches, &s_patch_cap, s_patch_count + 1, sizeof(IniPatch)))
		return;

	if (iPatch.type == BYTES_T)
	{
		const struct patch_piece* hex = &pieces[4];
		size_t i;
		iPatch.nbytes = (u32)(hex->n / 2);
		iPatch.bytes  = (u8*)malloc(iPatch.nbytes);
		if (!iPatch.bytes)
			return;
		for (i = 0; i < iPatch.nbytes; i++)
			parse_number_u8(hex->s + i * 2, 2, 16, &iPatch.bytes[i]);
	}

	iPatch.enabled = 1;
	s_patches[s_patch_count++] = iPatch;

#undef PATCH_ERROR
}

void ApplyDynamicPatches(u32 pc)
{
	size_t i;
	for (i = 0; i < s_dyna_count; i++)
		_ApplyDynaPatch(&s_dyna[i], pc);
}

void LoadDynamicPatches(const DynamicPatch* patches, size_t count)
{
	size_t i;
	for (i = 0; i < count; i++)
	{
		const size_t npat = patches[i].npattern;
		const size_t nrep = patches[i].nreplacement;
		struct dyna_patch* d;

		if (!patch_reserve((void**)&s_dyna, &s_dyna_cap, s_dyna_count + 1, sizeof(*s_dyna))
			|| !patch_reserve((void**)&s_dyna_entries, &s_dyna_entry_cap,
				s_dyna_entry_count + npat + nrep, sizeof(*s_dyna_entries)))
			return;

		d               = &s_dyna[s_dyna_count++];
		d->first        = s_dyna_entry_count;
		d->npattern     = (u32)npat;
		d->nreplacement = (u32)nrep;
		if (npat)
			memcpy(s_dyna_entries + s_dyna_entry_count, patches[i].pattern, npat * sizeof(DynamicPatchEntry));
		if (nrep)
			memcpy(s_dyna_entries + s_dyna_entry_count + npat, patches[i].replacement, nrep * sizeof(DynamicPatchEntry));
		s_dyna_entry_count += npat + nrep;
	}
}
