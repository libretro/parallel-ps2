/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2016  PCSX2 Dev Team
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

#include "Common.h"
#include "R3000A.h"
#include "IopMem.h"

/* The PS1 kernel's TTY calls. Their text has nowhere to go -- nothing
 * ever printed it -- so they are not collected; the guest memory they
 * name is still read as the console's kernel would read it, since a read
 * that reaches a hardware register can change it. */

void psxBiosReset(void)
{
}

/* Called for PlayStation BIOS calls at 0xA0, 0xB0 and 0xC0 in kernel
 * reserved memory (seemingly by actually calling those addresses).
 * Returns true if the call was processed here, which none is. */
bool psxBiosCall(void)
{
	switch (((psxRegs.pc << 4) & 0xf00) | (psxRegs.GPR.n.t1 & 0xff))
	{
		case 0xa03:
		case 0xb35:
			/* write(fd, data, size) */
			if (psxRegs.GPR.n.a0 == 1)
			{
				u32 data = psxRegs.GPR.n.a1;
				u32 size = psxRegs.GPR.n.a2;
				while (size--)
					iopMemRead8(data++);
			}
			break;
		case 0xa3e:
		case 0xb3f:
			/* puts(s) */
			{
				u32 str = psxRegs.GPR.n.a0;
				while (iopMemRead8(str++))
					;
			}
			break;
		default:
			/* putc(c, fd) and putchar(c) touch no memory */
			break;
	}

	return false;
}
