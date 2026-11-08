/* SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team */
/* SPDX-License-Identifier: GPL-3.0+ */

#pragma once

#include "common/Pcsx2Types.h"

#include <stddef.h>
#include <string.h>
#include <retro_inline.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Copies USB state into or out of a block of fixed size. A copy that
 * would run past the end copies nothing and marks the block bad. */
typedef struct UsbState
{
	u8* data;
	size_t size;
	size_t pos;
	bool reading;
	bool error;
} UsbState;

static INLINE void usb_state_bytes(UsbState* st, void* p, size_t n)
{
	if (st->error || n > st->size - st->pos)
	{
		st->error = true;
		return;
	}
	if (st->reading)
		memcpy(p, st->data + st->pos, n);
	else
		memcpy(st->data + st->pos, p, n);
	st->pos += n;
}

/* One variable, its whole size. */
#define USB_STATE_DO(st, v) usb_state_bytes((st), &(v), sizeof(v))

#ifdef __cplusplus
}
#endif
