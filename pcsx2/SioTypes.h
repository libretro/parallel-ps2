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

/* The SIO0 and SIO2 registers' bits and the commands that cross them.
 * C89, read by C and C++ alike. */
#pragma once

#include "../common/Pcsx2Types.h"

typedef enum SioStage
{
	SIO_STAGE_IDLE = 0,
	SIO_STAGE_WAITING_COMMAND,
	SIO_STAGE_WORKING
} SioStage;

/* The first byte of a transfer: what it is for. */
#define SIO_MODE_NOT_SET  0x00
#define SIO_MODE_PAD      0x01
#define SIO_MODE_MULTITAP 0x21
#define SIO_MODE_INFRARED 0x61
#define SIO_MODE_MEMCARD  0x81

/* Pad commands span this range. */
#define PAD_COMMAND_FIRST 0x40
#define PAD_COMMAND_LAST  0x4f

#define MEMCARD_CMD_NOT_SET                  0x00
#define MEMCARD_CMD_PROBE                    0x11
#define MEMCARD_CMD_UNKNOWN_WRITE_DELETE_END 0x12
#define MEMCARD_CMD_SET_ERASE_SECTOR         0x21
#define MEMCARD_CMD_SET_WRITE_SECTOR         0x22
#define MEMCARD_CMD_SET_READ_SECTOR          0x23
#define MEMCARD_CMD_GET_SPECS                0x26
#define MEMCARD_CMD_SET_TERMINATOR           0x27
#define MEMCARD_CMD_GET_TERMINATOR           0x28
#define MEMCARD_CMD_WRITE_DATA               0x42
#define MEMCARD_CMD_READ_DATA                0x43
#define MEMCARD_CMD_PS1_READ                 0x52
#define MEMCARD_CMD_PS1_STATE                0x53
#define MEMCARD_CMD_PS1_WRITE                0x57
#define MEMCARD_CMD_PS1_POCKETSTATION        0x58
#define MEMCARD_CMD_READ_WRITE_END           0x81
#define MEMCARD_CMD_ERASE_BLOCK              0x82
#define MEMCARD_CMD_UNKNOWN_BOOT             0xbf
#define MEMCARD_CMD_AUTH_XOR                 0xf0
#define MEMCARD_CMD_AUTH_F3                  0xf3
#define MEMCARD_CMD_AUTH_F7                  0xf7

typedef enum Sio0Interrupt
{
	SIO0_INT_TEST_EVENT = 0,
	SIO0_INT_STAT_READ,
	SIO0_INT_TX_DATA_WRITE
} Sio0Interrupt;

#define SIO_PORTS 2
#define SIO_SLOTS 4

#define SIO0_STAT_TX_READY          0x01
#define SIO0_STAT_RX_FIFO_NOT_EMPTY 0x02
#define SIO0_STAT_TX_EMPTY          0x04
#define SIO0_STAT_RX_PARITY_ERROR   0x08
#define SIO0_STAT_ACK               0x80
#define SIO0_STAT_IRQ               0x0200

#define SIO0_CTRL_TX_ENABLE      0x01
#define SIO0_CTRL_RX_ENABLE      0x04
#define SIO0_CTRL_ACK            0x10
#define SIO0_CTRL_RESET          0x40
#define SIO0_CTRL_RX_INT_MODE_LSB 0x0100
#define SIO0_CTRL_RX_INT_MODE_MSB 0x0200
#define SIO0_CTRL_TX_INT_ENABLE  0x0400
#define SIO0_CTRL_RX_INT_ENABLE  0x0800
#define SIO0_CTRL_ACK_INT_ENABLE 0x1000
#define SIO0_CTRL_PORT           0x2000

#define SEND3_PORT                0x01
#define SEND3_COMMAND_LENGTH_MASK 0x3ff

#define SIO2_CTRL_START_TRANSFER 0x1
#define SIO2_CTRL_RESET          0xc
#define SIO2_CTRL_PORT           0x2000
/* What SIO2MAN resets SIO2_CTRL to after a system reset. */
#define SIO2_CTRL_SIO2MAN_RESET  0x000003bc

#define RECV1_DISCONNECTED 0x1d100
#define RECV1_CONNECTED    0x1100

#define RECV2_DEFAULT 0xf

/* Most RECV3 values are undocumented, inferred from how older code used
 * them; tests suggest they are not read at all. */
#define RECV3_DEFAULT        0x0
#define RECV3_SPECS          0x83 /* getting the card's specs */
#define RECV3_TERMINATOR     0x8b /* getting or setting the terminator */
#define RECV3_READ_WRITE_END 0x8c /* setting the read or write sector */

#define TERMINATOR_DEFAULT 0x55
