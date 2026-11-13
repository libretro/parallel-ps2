/* The USB keyboard (USB/libretro-usb): every key the core maps, from the
 * frontend's RETROK_ code to the boot report a game reads off the
 * interrupt endpoint - one poll per key, with modifiers in their byte -
 * and queued keys an older state left, which it drops. */
#include <stdio.h>
#include <string.h>

#include "USB/libretro-usb/usb-hid.h"

static void RETRO_CALLCONV quiet_log(enum retro_log_level level, const char* fmt, ...)
{
	(void)level; (void)fmt;
}
retro_log_printf_t log_cb = quiet_log;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* Each key alone: the modifier byte and the first key slot */
static const struct
{
	unsigned retrok;
	u8 mod, key;
} s_keys[] = {
	{RETROK_a, 0x00, 0x04},
	{RETROK_b, 0x00, 0x05},
	{RETROK_c, 0x00, 0x06},
	{RETROK_d, 0x00, 0x07},
	{RETROK_e, 0x00, 0x08},
	{RETROK_f, 0x00, 0x09},
	{RETROK_g, 0x00, 0x0a},
	{RETROK_h, 0x00, 0x0b},
	{RETROK_i, 0x00, 0x0c},
	{RETROK_j, 0x00, 0x0d},
	{RETROK_k, 0x00, 0x0e},
	{RETROK_l, 0x00, 0x0f},
	{RETROK_m, 0x00, 0x10},
	{RETROK_n, 0x00, 0x11},
	{RETROK_o, 0x00, 0x12},
	{RETROK_p, 0x00, 0x13},
	{RETROK_q, 0x00, 0x14},
	{RETROK_r, 0x00, 0x15},
	{RETROK_s, 0x00, 0x16},
	{RETROK_t, 0x00, 0x17},
	{RETROK_u, 0x00, 0x18},
	{RETROK_v, 0x00, 0x19},
	{RETROK_w, 0x00, 0x1a},
	{RETROK_x, 0x00, 0x1b},
	{RETROK_y, 0x00, 0x1c},
	{RETROK_z, 0x00, 0x1d},
	{RETROK_0, 0x00, 0x27},
	{RETROK_1, 0x00, 0x1e},
	{RETROK_2, 0x00, 0x1f},
	{RETROK_3, 0x00, 0x20},
	{RETROK_4, 0x00, 0x21},
	{RETROK_5, 0x00, 0x22},
	{RETROK_6, 0x00, 0x23},
	{RETROK_7, 0x00, 0x24},
	{RETROK_8, 0x00, 0x25},
	{RETROK_9, 0x00, 0x26},
	{RETROK_RETURN, 0x00, 0x28},
	{RETROK_ESCAPE, 0x00, 0x29},
	{RETROK_BACKSPACE, 0x00, 0x2a},
	{RETROK_TAB, 0x00, 0x2b},
	{RETROK_SPACE, 0x00, 0x2c},
	{RETROK_MINUS, 0x00, 0x2d},
	{RETROK_EQUALS, 0x00, 0x2e},
	{RETROK_LEFTBRACKET, 0x00, 0x2f},
	{RETROK_RIGHTBRACKET, 0x00, 0x30},
	{RETROK_BACKSLASH, 0x00, 0x31},
	{RETROK_SEMICOLON, 0x00, 0x33},
	{RETROK_QUOTE, 0x00, 0x34},
	{RETROK_BACKQUOTE, 0x00, 0x35},
	{RETROK_COMMA, 0x00, 0x36},
	{RETROK_PERIOD, 0x00, 0x37},
	{RETROK_SLASH, 0x00, 0x38},
	{RETROK_CAPSLOCK, 0x00, 0x39},
	{RETROK_F1, 0x00, 0x3a},
	{RETROK_F2, 0x00, 0x3b},
	{RETROK_F3, 0x00, 0x3c},
	{RETROK_F4, 0x00, 0x3d},
	{RETROK_F5, 0x00, 0x3e},
	{RETROK_F6, 0x00, 0x3f},
	{RETROK_F7, 0x00, 0x40},
	{RETROK_F8, 0x00, 0x41},
	{RETROK_F9, 0x00, 0x42},
	{RETROK_F10, 0x00, 0x43},
	{RETROK_F11, 0x00, 0x44},
	{RETROK_F12, 0x00, 0x45},
	{RETROK_RIGHT, 0x00, 0x4f},
	{RETROK_LEFT, 0x00, 0x50},
	{RETROK_DOWN, 0x00, 0x51},
	{RETROK_UP, 0x00, 0x52},
	{RETROK_INSERT, 0x00, 0x49},
	{RETROK_HOME, 0x00, 0x4a},
	{RETROK_END, 0x00, 0x4d},
	{RETROK_PAGEUP, 0x00, 0x4b},
	{RETROK_PAGEDOWN, 0x00, 0x4e},
	{RETROK_DELETE, 0x00, 0x4c},
	{RETROK_LCTRL, 0x01, 0x00},
	{RETROK_RCTRL, 0x10, 0x00},
	{RETROK_LSHIFT, 0x02, 0x00},
	{RETROK_RSHIFT, 0x20, 0x00},
	{RETROK_LALT, 0x04, 0x00},
	{RETROK_RALT, 0x40, 0x00},
};

static unsigned s_down[8];
static int16_t input_state(unsigned port, unsigned device, unsigned index, unsigned id)
{
	unsigned i;
	(void)port; (void)index;
	if (device != RETRO_DEVICE_KEYBOARD)
		return 0;
	for (i = 0; i < 8; i++)
		if (s_down[i] && s_down[i] == id)
			return 1;
	return 0;
}

/* One interrupt IN poll; the report's length, or 0 on NAK */
static int poll_report(USBDevice* dev, u8* report)
{
	USBPacket p;
	u8 buf[8];
	memset(&p, 0, sizeof(p));
	memset(buf, 0xcc, sizeof(buf));
	usb_packet_setup(&p, USB_TOKEN_IN, usb_ep_get(dev, USB_TOKEN_IN, 1), 0, 1, false, false);
	usb_packet_addbuf(&p, buf, sizeof(buf));
	dev->klass.handle_data(dev, &p);
	usb_packet_cleanup(&p);
	if (p.status != USB_RET_SUCCESS)
		return 0;
	memcpy(report, buf, 8);
	return (int)p.actual_length;
}

static void press(USBDevice* dev, unsigned a, unsigned b)
{
	memset(s_down, 0, sizeof(s_down));
	s_down[0] = a;
	s_down[1] = b;
	usb_hid_update(dev, input_state, 0);
}

int main(void)
{
	USBDevice* dev = usb_hid_create_kbd(0);
	u8 r[8];
	unsigned i;

	CHECK(dev != NULL);
	if (!dev)
		return 1;

	/* Every key, pressed then let go, each seen in one poll */
	for (i = 0; i < sizeof(s_keys) / sizeof(s_keys[0]); i++)
	{
		press(dev, s_keys[i].retrok, 0);
		CHECK(poll_report(dev, r) == 8);
		if (r[0] != s_keys[i].mod || r[2] != s_keys[i].key || r[3] != 0)
		{
			printf("  key %u (RETROK %u): %02x %02x, want %02x %02x\n", i, s_keys[i].retrok,
				r[0], r[2], s_keys[i].mod, s_keys[i].key);
			fails++;
		}
		CHECK(poll_report(dev, r) == 0);
		press(dev, 0, 0);
		CHECK(poll_report(dev, r) == 8 && r[0] == 0 && r[2] == 0);
	}

	/* Right Ctrl with Up, both in place after two polls - they took
	 * four as PS/2 scancodes, each behind an 0xe0 - then each let go on
	 * its own */
	press(dev, RETROK_RCTRL, RETROK_UP);
	poll_report(dev, r);
	CHECK(poll_report(dev, r) == 8 && r[0] == 0x10 && r[2] == 0x52);
	press(dev, RETROK_UP, 0);
	CHECK(poll_report(dev, r) == 8 && r[0] == 0x00 && r[2] == 0x52);
	press(dev, 0, 0);
	CHECK(poll_report(dev, r) == 8 && r[0] == 0 && r[2] == 0);

	/* Seven keys at once roll over */
	{
		static const unsigned k[7] = {RETROK_a, RETROK_b, RETROK_c, RETROK_d, RETROK_e, RETROK_f, RETROK_g};
		unsigned j;
		memset(s_down, 0, sizeof(s_down));
		for (j = 0; j < 7; j++)
		{
			s_down[j] = k[j];
			usb_hid_update(dev, input_state, 0);
			poll_report(dev, r);
		}
	}
	CHECK(r[2] == 0x01 && r[7] == 0x01);
	press(dev, 0, 0);
	for (i = 0; i < 7; i++)
		poll_report(dev, r);
	CHECK(r[0] == 0 && r[2] == 0);

	/* A state from before the queue held HID usages: its scancodes are
	 * dropped, and so are the old 0xe0/0xe1 prefix bits */
	{
		HIDState hs;
		memset(&hs, 0, sizeof(hs));
		hid_init(&hs, HID_KEYBOARD, NULL);
		hid_reset(&hs);
		hs.head = 14;
		hs.n = 3;
		hs.u.kbd.keycodes[14] = HID_KEY_EVENT | 0x04;
		hs.u.kbd.keycodes[15] = 0xe0;
		hs.u.kbd.keycodes[0] = 0x48;
		hs.u.kbd.modifiers = 0x102;
		hid_keyboard_loaded(&hs);
		CHECK(hs.n == 0 && hs.u.kbd.modifiers == 0x02);
		hs.n = 2;
		hs.u.kbd.keycodes[15] = HID_KEY_EVENT | 0x05 | HID_KEY_UP;
		hs.u.kbd.keycodes[0] = 0;
		hs.n = 2;
		hid_keyboard_loaded(&hs);
		CHECK(hs.n == 2);
		hs.n = 99;
		hid_keyboard_loaded(&hs);
		CHECK(hs.n == 0);
	}

	dev->klass.unrealize(dev);

	if (fails)
	{
		printf("usbhid: %d failed\n", fails);
		return 1;
	}
	printf("usbhid: ok\n");
	return 0;
}
