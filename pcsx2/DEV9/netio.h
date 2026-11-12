/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2020  PCSX2 Dev Team
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

/* Between the DEV9 core (DEV9.c, smap.c) and the network behind it
 * (net.cpp): one packet's bytes, and the calls each side makes of the
 * other. C linkage both ways. */

#pragma once

#include "common/Pcsx2Defs.h"

#ifdef __cplusplus
extern "C" {
#endif

struct Pcsx2Config;

typedef struct NetPacket
{
	int size;
	/* 1536 is what is needed; padded up to 2048 bytes */
	char buffer[2048 - sizeof(int)];
} NetPacket;

/* The SMAP's, on the EE thread: whether the packet went into the RX FIFO,
 * and whether there is room for one. */
bool rx_process(NetPacket* pk);
bool rx_fifo_can_rx(void);

/* The network's. A frame for the network thread to send, read straight
 * out of the SMAP's TX FIFO: head_len bytes at head, then tail_len at
 * tail where the frame wraps the FIFO (0 where it does not). */
void tx_put(const void* head, int head_len, const void* tail, int tail_len);
void ad_reset(void);
/* EE thread: hand the SMAP every received packet it has room for.
 * Returns whether any was delivered. */
bool net_rx_deliver(void);
void InitNet(void);
void ReconfigureLiveNet(const struct Pcsx2Config* old_config);
void TermNet(void);

#ifdef __cplusplus
}
#endif
