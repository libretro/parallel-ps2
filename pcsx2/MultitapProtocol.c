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

/* The multitap. See MultitapProtocol.h. */
#include "MultitapProtocol.h"
#include "Sio.h"

static void support_check(void)
{
	sio_fifo_push(&fifoOut, 0x5a);
	sio_fifo_push(&fifoOut, 0x04);
	sio_fifo_push(&fifoOut, 0x00);
	sio_fifo_push(&fifoOut, 0x5a);
}

static void select_slot(void)
{
	const u8 slot = sio_fifo_front(&fifoIn);
	const bool ok = slot < SIO_SLOTS;
	sio_fifo_pop(&fifoIn);
	if (ok)
		sio2.slot = slot;
	sio_fifo_push(&fifoOut, 0x5a);
	sio_fifo_push(&fifoOut, 0x00);
	sio_fifo_push(&fifoOut, 0x00);
	sio_fifo_push(&fifoOut, ok ? slot : 0xff);
	sio_fifo_push(&fifoOut, ok ? 0x5a : 0x66);
}

void multitap_send(void)
{
	const u8 command = sio_fifo_front(&fifoIn);
	sio_fifo_pop(&fifoIn);
	sio_fifo_push(&fifoOut, 0x80);
	switch (command)
	{
		case MULTITAP_MODE_PAD_SUPPORT_CHECK:
		case MULTITAP_MODE_MEMCARD_SUPPORT_CHECK:
			support_check();
			break;
		case MULTITAP_MODE_SELECT_PAD:
		case MULTITAP_MODE_SELECT_MEMCARD:
			select_slot();
			break;
		default:
			break;
	}
}
