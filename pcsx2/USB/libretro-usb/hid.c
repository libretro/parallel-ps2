/* SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team */
/* SPDX-License-Identifier: MIT */
 
/*
* QEMU HID devices
*
* Copyright (c) 2005 Fabrice Bellard
* Copyright (c) 2007 OpenMoko, Inc.  (andrew@openedhand.com)
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
* THE SOFTWARE.
*/

#include "hid.h"

#include <assert.h>
#include <retro_inline.h>
#include <retro_miscellaneous.h>
#include <stdlib.h>
#include <string.h>

#define HID_USAGE_ERROR_ROLLOVER 0x01
#define HID_USAGE_POSTFAIL 0x02
#define HID_USAGE_ERROR_UNDEFINED 0x03

bool hid_has_events(HIDState* hs)
{
	return hs->n > 0;
}

void hid_set_next_idle(HIDState* hs)
{
}

static void hid_pointer_event(HIDState* hs, InputEvent* evt)
{
	static const int bmap[INPUT_BUTTON__MAX] = {
		/*[INPUT_BUTTON_LEFT] =*/0x01,
		/*[INPUT_BUTTON_MIDDLE] =*/0x04,
		/*[INPUT_BUTTON_RIGHT] =*/0x02,
		0, 0, 0, 0};
	HIDPointerEvent* e;
	InputMoveEvent* move;
	InputBtnEvent* btn;

	assert(hs->n < QUEUE_LENGTH);
	e = &hs->u.ptr.queue[(hs->head + hs->n) & QUEUE_MASK];

	switch (evt->type)
	{
		case INPUT_EVENT_KIND_REL:
			move = &evt->u.rel;
			if (move->axis == INPUT_AXIS_X)
			{
				e->xdx += move->value;
			}
			else if (move->axis == INPUT_AXIS_Y)
			{
				e->ydy += move->value;
			}
			break;

		case INPUT_EVENT_KIND_ABS:
			move = &evt->u.abs;
			if (move->axis == INPUT_AXIS_X)
			{
				e->xdx = move->value;
			}
			else if (move->axis == INPUT_AXIS_Y)
			{
				e->ydy = move->value;
			}
			break;

		case INPUT_EVENT_KIND_BTN:
			btn = &evt->u.btn;
			if (btn->down)
			{
				e->buttons_state |= bmap[btn->button];
				if (btn->button == INPUT_BUTTON_WHEEL_UP)
				{
					e->dz--;
				}
				else if (btn->button == INPUT_BUTTON_WHEEL_DOWN)
				{
					e->dz++;
				}
			}
			else
			{
				e->buttons_state &= ~bmap[btn->button];
			}
			break;

		default:
			/* keep gcc happy */
			break;
	}
}

static void hid_pointer_sync(HIDState* hs)
{
	HIDPointerEvent *prev, *curr, *next;
	bool event_compression = false;

	if (hs->n == QUEUE_LENGTH - 1)
	{
		/*
        * Queue full.  We are losing information, but we at least
        * keep track of most recent button state.
        */
		return;
	}

	prev = &hs->u.ptr.queue[(hs->head + hs->n - 1) & QUEUE_MASK];
	curr = &hs->u.ptr.queue[(hs->head + hs->n) & QUEUE_MASK];
	next = &hs->u.ptr.queue[(hs->head + hs->n + 1) & QUEUE_MASK];

	if (hs->n > 0)
	{
		/*
        * No button state change between previous and current event
        * (and previous wasn't seen by the guest yet), so there is
        * motion information only and we can combine the two event
        * into one.
        */
		if (curr->buttons_state == prev->buttons_state)
		{
			event_compression = true;
		}
	}

	if (event_compression)
	{
		/* add current motion to previous, clear current */
		if (hs->kind == HID_MOUSE)
		{
			prev->xdx += curr->xdx;
			curr->xdx = 0;
			prev->ydy += curr->ydy;
			curr->ydy = 0;
		}
		else
		{
			prev->xdx = curr->xdx;
			prev->ydy = curr->ydy;
		}
		prev->dz += curr->dz;
		curr->dz = 0;
	}
	else
	{
		/* prepate next (clear rel, copy abs + btns) */
		if (hs->kind == HID_MOUSE)
		{
			next->xdx = 0;
			next->ydy = 0;
		}
		else
		{
			next->xdx = curr->xdx;
			next->ydy = curr->ydy;
		}
		next->dz = 0;
		next->buttons_state = curr->buttons_state;
		/* make current guest visible, notify guest */
		hs->n++;
		hs->event(hs);
	}
}

void hid_keyboard_key(HIDState* hs, uint8_t usage, bool down)
{
	if (hs->n >= QUEUE_LENGTH)
		return;
	hs->u.kbd.keycodes[(hs->head + hs->n++) & QUEUE_MASK] =
		HID_KEY_EVENT | usage | (down ? 0 : HID_KEY_UP);
	hs->event(hs);
}

void hid_keyboard_loaded(HIDState* hs)
{
	uint32_t i;
	if (hs->n > QUEUE_LENGTH)
		hs->n = 0;
	for (i = 0; i < hs->n; i++)
		if (!(hs->u.kbd.keycodes[(hs->head + i) & QUEUE_MASK] & HID_KEY_EVENT))
		{
			hs->n = 0;
			break;
		}
	hs->u.kbd.modifiers &= 0xff;
}

static void hid_keyboard_process_keycode(HIDState* hs)
{
	uint32_t e;
	uint8_t usage;
	int i;

	if (hs->n == 0)
		return;
	e = hs->u.kbd.keycodes[hs->head & QUEUE_MASK];
	QUEUE_INCR(hs->head);
	hs->n--;
	usage = (uint8_t)e;

	/* Ctrl, Shift, Alt and GUI, left then right, are the modifier byte */
	if (usage >= 0xe0 && usage <= 0xe7)
	{
		if (e & HID_KEY_UP)
			hs->u.kbd.modifiers &= ~(1 << (usage & 7));
		else
			hs->u.kbd.modifiers |= 1 << (usage & 7);
		return;
	}

	for (i = hs->u.kbd.keys - 1; i >= 0; i--)
		if (hs->u.kbd.key[i] == usage)
			break;
	if (e & HID_KEY_UP)
	{
		if (i >= 0)
		{
			hs->u.kbd.key[i] = hs->u.kbd.key[--hs->u.kbd.keys];
			hs->u.kbd.key[hs->u.kbd.keys] = 0x00;
		}
	}
	else if (i < 0 && hs->u.kbd.keys < (int32_t)sizeof(hs->u.kbd.key))
		hs->u.kbd.key[hs->u.kbd.keys++] = usage;
}

static INLINE int int_clamp(int val, int vmin, int vmax)
{
	if (val < vmin)
	{
		return vmin;
	}
	else if (val > vmax)
	{
		return vmax;
	}
	else
	{
		return val;
	}
}

void hid_pointer_activate(HIDState* hs)
{
	if (!hs->u.ptr.mouse_grabbed)
	{
		hs->u.ptr.mouse_grabbed = 1;
	}
}

int hid_pointer_poll(HIDState* hs, uint8_t* buf, int len)
{
	int dx, dy, dz, l;
	int index;
	HIDPointerEvent* e;

	hid_pointer_activate(hs);

	/* When the buffer is empty, return the last event.  Relative
    movements will all be zero.  */
	index = (hs->n ? hs->head : hs->head - 1);
	e = &hs->u.ptr.queue[index & QUEUE_MASK];

	if (hs->kind == HID_MOUSE)
	{
		dx = int_clamp(e->xdx, -127, 127);
		dy = int_clamp(e->ydy, -127, 127);
		e->xdx -= dx;
		e->ydy -= dy;
	}
	else
	{
		dx = e->xdx;
		dy = e->ydy;
	}
	dz = int_clamp(e->dz, -127, 127);
	e->dz -= dz;

	if (hs->n &&
		!e->dz &&
		(hs->kind == HID_TABLET || (!e->xdx && !e->ydy)))
	{
		/* that deals with this event */
		QUEUE_INCR(hs->head);
		hs->n--;
	}

	/* Appears we have to invert the wheel direction */
	dz = 0 - dz;
	l = 0;
	switch (hs->kind)
	{
		case HID_MOUSE:
			if (len > l)
			{
				buf[l++] = e->buttons_state;
			}
			if (len > l)
			{
				buf[l++] = dx;
			}
			if (len > l)
			{
				buf[l++] = dy;
			}
			if (len > l)
			{
				buf[l++] = dz;
			}
			break;

		case HID_TABLET:
			if (len > l)
			{
				buf[l++] = e->buttons_state;
			}
			if (len > l)
			{
				buf[l++] = dx & 0xff;
			}
			if (len > l)
			{
				buf[l++] = dx >> 8;
			}
			if (len > l)
			{
				buf[l++] = dy & 0xff;
			}
			if (len > l)
			{
				buf[l++] = dy >> 8;
			}
			if (len > l)
			{
				buf[l++] = dz;
			}
			break;

		default: /* not a pointer: no report */
			break;
	}

	return l;
}

int hid_keyboard_poll(HIDState* hs, uint8_t* buf, int len)
{
	if (len < 2)
	{
		return 0;
	}

	hid_keyboard_process_keycode(hs);

	buf[0] = hs->u.kbd.modifiers & 0xff;
	buf[1] = 0;
	if (hs->u.kbd.keys > 6)
	{
		memset(buf + 2, HID_USAGE_ERROR_ROLLOVER, MIN(8, len) - 2);
	}
	else
	{
		memcpy(buf + 2, hs->u.kbd.key, MIN(8, len) - 2);
	}

	return MIN(8, len);
}

int hid_keyboard_write(HIDState* hs, uint8_t* buf, int len)
{
#if 0
	if (len > 0)
	{
		int ledstate = 0;
		/* 0x01: Num Lock LED
        * 0x02: Caps Lock LED
        * 0x04: Scroll Lock LED
        * 0x08: Compose LED
        * 0x10: Kana LED */
		hs->u.kbd.leds = buf[0];
		if (hs->u.kbd.leds & 0x04)
		{
			ledstate |= QEMU_SCROLL_LOCK_LED;
		}
		if (hs->u.kbd.leds & 0x01)
		{
			ledstate |= QEMU_NUM_LOCK_LED;
		}
		if (hs->u.kbd.leds & 0x02)
		{
			ledstate |= QEMU_CAPS_LOCK_LED;
		}
		/*kbd_put_ledstate(ledstate); */
	}
#endif
	return 0;
}

void hid_reset(HIDState* hs)
{
	switch (hs->kind)
	{
		case HID_KEYBOARD:
			memset(hs->u.kbd.keycodes, 0, sizeof(hs->u.kbd.keycodes));
			memset(hs->u.kbd.key, 0, sizeof(hs->u.kbd.key));
			hs->u.kbd.keys = 0;
			hs->u.kbd.modifiers = 0;
			break;
		case HID_MOUSE:
		case HID_TABLET:
			memset(hs->u.ptr.queue, 0, sizeof(hs->u.ptr.queue));
			break;
	}
	hs->head = 0;
	hs->n = 0;
	hs->protocol = 1;
	hs->idle = 0;
}

void hid_free(HIDState* hs)
{
}

void hid_init(HIDState* hs, int kind, HIDEventFunc event)
{
	hs->kind = kind;
	hs->event = event;

	if (hs->kind == HID_MOUSE || hs->kind == HID_TABLET)
	{
		hs->u.ptr.eh_entry = hid_pointer_event;
		hs->u.ptr.eh_sync = hid_pointer_sync;
	}
}
