/* SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team */
/* SPDX-License-Identifier: GPL-2.0+ */

#pragma once
#include "common/Pcsx2Types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* include/ui/console.h */
/* keyboard/mouse support */

#define MOUSE_EVENT_LBUTTON 0x01
#define MOUSE_EVENT_RBUTTON 0x02
#define MOUSE_EVENT_MBUTTON 0x04
/* identical to the ps/2 keyboard bits */
#define QEMU_SCROLL_LOCK_LED (1 << 0)
#define QEMU_NUM_LOCK_LED (1 << 1)
#define QEMU_CAPS_LOCK_LED (1 << 2)

#define HID_MOUSE 1
#define HID_TABLET 2
#define HID_KEYBOARD 3

typedef enum InputEventKind
{
	INPUT_EVENT_KIND_KEY = 0,
	INPUT_EVENT_KIND_BTN = 1,
	INPUT_EVENT_KIND_REL = 2,
	INPUT_EVENT_KIND_ABS = 3,
	INPUT_EVENT_KIND__MAX = 4
} InputEventKind;

typedef enum InputAxis
{
	INPUT_AXIS_X = 0,
	INPUT_AXIS_Y = 1,
	INPUT_AXIS__MAX = 2
} InputAxis;

typedef enum InputButton
{
	INPUT_BUTTON_LEFT = 0,
	INPUT_BUTTON_MIDDLE = 1,
	INPUT_BUTTON_RIGHT = 2,
	INPUT_BUTTON_WHEEL_UP = 3,
	INPUT_BUTTON_WHEEL_DOWN = 4,
	INPUT_BUTTON_SIDE = 5,
	INPUT_BUTTON_EXTRA = 6,
	INPUT_BUTTON__MAX = 7
} InputButton;

typedef struct InputBtnEvent InputBtnEvent;
typedef struct InputMoveEvent InputMoveEvent;
typedef struct InputEvent InputEvent;

struct InputBtnEvent
{
	InputButton button;
	bool down;
};

struct InputMoveEvent
{
	InputAxis axis;
	int64_t value;
};

struct InputEvent
{
	InputEventKind type;
	union
	{
		InputBtnEvent btn;
		InputMoveEvent rel;
		InputMoveEvent abs;
	} u;
};

typedef struct HIDState HIDState;
typedef void (*HIDEventFunc)(HIDState* s);

typedef void QEMUPutLEDEvent(void* opaque, int ledstate);
typedef void QEMUPutMouseEvent(HIDState* hs, InputEvent* evt);

typedef struct HIDPointerEvent
{
	int32_t xdx, ydy; /* relative if it's a mouse, otherwise absolute */
	int32_t dz, buttons_state;
} HIDPointerEvent;

#define QUEUE_LENGTH 16 /* should be enough for a triple-click */
#define QUEUE_MASK (QUEUE_LENGTH - 1u)
#define QUEUE_INCR(v) ((v)++, (v) &= QUEUE_MASK)

typedef struct HIDMouseState
{
	HIDPointerEvent queue[QUEUE_LENGTH];
	int mouse_grabbed;
	QEMUPutMouseEvent* eh_entry;
	HIDEventFunc eh_sync;
} HIDMouseState;

/* A queued key: HID_KEY_EVENT, the HID usage, and HID_KEY_UP on release.
 * The tag sets these apart from the PS/2 scancodes older states queued. */
#define HID_KEY_EVENT 0x10000u
#define HID_KEY_UP    0x100u

typedef struct HIDKeyboardState
{
	uint32_t keycodes[QUEUE_LENGTH];
	uint16_t modifiers;
	uint8_t leds;
	uint8_t key[16];
	int32_t keys;
} HIDKeyboardState;

struct HIDState
{
	union
	{
		HIDMouseState ptr;
		HIDKeyboardState kbd;
	} u;
	uint32_t head; /* index into circular queue */
	uint32_t n;
	int kind;
	int32_t protocol;
	uint8_t idle;
	HIDEventFunc event;
};

void hid_init(HIDState* hs, int kind, HIDEventFunc event);
void hid_reset(HIDState* hs);
void hid_free(HIDState* hs);

bool hid_has_events(HIDState* hs);
void hid_set_next_idle(HIDState* hs);
void hid_pointer_activate(HIDState* hs);
int hid_pointer_poll(HIDState* hs, uint8_t* buf, int len);
int hid_keyboard_poll(HIDState* hs, uint8_t* buf, int len);
int hid_keyboard_write(HIDState* hs, uint8_t* buf, int len);
/* Queues a press or release of a key by its HID usage (0x04 to 0xe7) */
void hid_keyboard_key(HIDState* hs, uint8_t usage, bool down);
/* Drops queued keys a state from before HID_KEY_EVENT left */
void hid_keyboard_loaded(HIDState* hs);

#ifdef __cplusplus
}
#endif
