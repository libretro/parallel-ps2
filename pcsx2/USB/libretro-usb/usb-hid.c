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

/* USB-HID keyboard/mouse device model, sourced directly from the libretro
 * RETRO_DEVICE_KEYBOARD / RETRO_DEVICE_MOUSE input callbacks. The OHCI/HID
 * core (QEMU-derived) is reused as-is; only the host-input binding layer is
 * replaced - upstream drove these devices through InputManager bind values,
 * here they are pumped from the frontend each frame. */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "USB/libretro-usb/USBinternal.h"
#include "USB/libretro-usb/desc.h"
#include "USB/libretro-usb/usb-hid.h"

#include "libretro.h"



enum
{
	STR_MANUFACTURER = 1,
	STR_PRODUCT_MOUSE,
	STR_PRODUCT_TABLET,
	STR_PRODUCT_KEYBOARD,
	STR_SERIALNUMBER,
	STR_CONFIG_MOUSE,
	STR_CONFIG_TABLET,
	STR_CONFIG_KEYBOARD
};

static const USBDescStrings desc_strings = {
	"QEMU",
	"QEMU USB Mouse",
	"QEMU USB Tablet",
	"QEMU USB Keyboard",
	"42", /* == remote wakeup works */
	"HID Mouse",
	"HID Tablet",
	"HID Keyboard",
};


static const USBDescStrings beatmania_dadada_desc_strings = {
	"",
	"KONAMI CPJ1",
	"USB JIS Mini Keyboard",
};

/* mostly the same values as the Bochs USB Mouse device */
static const uint8_t qemu_mouse_dev_descriptor[] = {
	0x12, /*  u8 bLength; */
	0x01, /*  u8 bDescriptorType; Device */
	0x10, 0x00, /*  u16 bcdUSB; v1.0 */

	0x00, /*  u8  bDeviceClass; */
	0x00, /*  u8  bDeviceSubClass; */
	0x00, /*  u8  bDeviceProtocol; [ low/full speeds only ] */
	0x08, /*  u8  bMaxPacketSize0; 8 Bytes */

	0x27, 0x06, /*  u16 idVendor; */
	0x01, 0x00, /*  u16 idProduct; */
	0x00, 0x00, /*  u16 bcdDevice */

	STR_MANUFACTURER, /*  u8  iManufacturer; */
	STR_PRODUCT_MOUSE, /*  u8  iProduct; */
	STR_SERIALNUMBER, /*  u8  iSerialNumber; */
	0x01 /*  u8  bNumConfigurations; */
};

static const uint8_t qemu_mouse_config_descriptor[] = {
	/* one configuration */
	0x09, /*  u8  bLength; */
	0x02, /*  u8  bDescriptorType; Configuration */
	0x22, 0x00, /*  u16 wTotalLength; */
	0x01, /*  u8  bNumInterfaces; (1) */
	0x01, /*  u8  bConfigurationValue; */
	0x04, /*  u8  iConfiguration; */
	0xa0, /*  u8  bmAttributes;
                 Bit 7: must be set,
                     6: Self-powered,
                     5: Remote wakeup,
                     4..0: resvd */
	50, /*  u8  MaxPower; */

	/* USB 1.1:
     * USB 2.0, single TT organization (mandatory):
     *  one interface, protocol 0
     *
     * USB 2.0, multiple TT organization (optional):
     *  two interfaces, protocols 1 (like single TT)
     *  and 2 (multiple TT mode) ... config is
     *  sometimes settable
     *  NOT IMPLEMENTED
     */

	/* one interface */
	0x09, /*  u8  if_bLength; */
	0x04, /*  u8  if_bDescriptorType; Interface */
	0x00, /*  u8  if_bInterfaceNumber; */
	0x00, /*  u8  if_bAlternateSetting; */
	0x01, /*  u8  if_bNumEndpoints; */
	0x03, /*  u8  if_bInterfaceClass; */
	0x01, /*  u8  if_bInterfaceSubClass; */
	0x02, /*  u8  if_bInterfaceProtocol; [usb1.1 or single tt] */
	0x05, /*  u8  if_iInterface; */

	/* HID descriptor */
	0x09, /*  u8  bLength; */
	0x21, /*  u8 bDescriptorType; */
	0x01, 0x00, /*  u16 HID_class */
	0x00, /*  u8 country_code */
	0x01, /*  u8 num_descriptors */
	0x22, /*  u8 type; Report */
	52, 0, /*  u16 len */

	/* one endpoint (status change endpoint) */
	0x07, /*  u8  ep_bLength; */
	0x05, /*  u8  ep_bDescriptorType; Endpoint */
	0x81, /*  u8  ep_bEndpointAddress; IN Endpoint 1 */
	0x03, /*  u8  ep_bmAttributes; Interrupt */
	0x04, 0x00, /*  u16 ep_wMaxPacketSize; */
	0x0a, /*  u8  ep_bInterval; (255ms -- usb 2.0 spec) */
};

static const uint8_t qemu_mouse_hid_report_descriptor[] = {
	0x05, 0x01, /* Usage Page (Generic Desktop) */
	0x09, 0x02, /* Usage (Mouse) */
	0xa1, 0x01, /* Collection (Application) */
	0x09, 0x01, /*   Usage (Pointer) */
	0xa1, 0x00, /*   Collection (Physical) */
	0x05, 0x09, /*     Usage Page (Button) */
	0x19, 0x01, /*     Usage Minimum (1) */
	0x29, 0x03, /*     Usage Maximum (3) */
	0x15, 0x00, /*     Logical Minimum (0) */
	0x25, 0x01, /*     Logical Maximum (1) */
	0x95, 0x03, /*     Report Count (3) */
	0x75, 0x01, /*     Report Size (1) */
	0x81, 0x02, /*     Input (Data, Variable, Absolute) */
	0x95, 0x01, /*     Report Count (1) */
	0x75, 0x05, /*     Report Size (5) */
	0x81, 0x01, /*     Input (Constant) */
	0x05, 0x01, /*     Usage Page (Generic Desktop) */
	0x09, 0x30, /*     Usage (X) */
	0x09, 0x31, /*     Usage (Y) */
	0x09, 0x38, /*     Usage (Wheel) */
	0x15, 0x81, /*     Logical Minimum (-0x7f) */
	0x25, 0x7f, /*     Logical Maximum (0x7f) */
	0x75, 0x08, /*     Report Size (8) */
	0x95, 0x03, /*     Report Count (3) */
	0x81, 0x06, /*     Input (Data, Variable, Relative) */
	0xc0, /*   End Collection */
	0xc0, /* End Collection */
};

static const uint8_t qemu_tablet_hid_report_descriptor[] = {
	0x05, 0x01, /* Usage Page (Generic Desktop) */
	0x09, 0x02, /* Usage (Mouse) */
	0xa1, 0x01, /* Collection (Application) */
	0x09, 0x01, /*   Usage (Pointer) */
	0xa1, 0x00, /*   Collection (Physical) */
	0x05, 0x09, /*     Usage Page (Button) */
	0x19, 0x01, /*     Usage Minimum (1) */
	0x29, 0x03, /*     Usage Maximum (3) */
	0x15, 0x00, /*     Logical Minimum (0) */
	0x25, 0x01, /*     Logical Maximum (1) */
	0x95, 0x03, /*     Report Count (3) */
	0x75, 0x01, /*     Report Size (1) */
	0x81, 0x02, /*     Input (Data, Variable, Absolute) */
	0x95, 0x01, /*     Report Count (1) */
	0x75, 0x05, /*     Report Size (5) */
	0x81, 0x01, /*     Input (Constant) */
	0x05, 0x01, /*     Usage Page (Generic Desktop) */
	0x09, 0x30, /*     Usage (X) */
	0x09, 0x31, /*     Usage (Y) */
	0x15, 0x00, /*     Logical Minimum (0) */
	0x26, 0xff, 0x7f, /*     Logical Maximum (0x7fff) */
	0x35, 0x00, /*     Physical Minimum (0) */
	0x46, 0xff, 0x7f, /*     Physical Maximum (0x7fff) */
	0x75, 0x10, /*     Report Size (16) */
	0x95, 0x02, /*     Report Count (2) */
	0x81, 0x02, /*     Input (Data, Variable, Absolute) */
	0x05, 0x01, /*     Usage Page (Generic Desktop) */
	0x09, 0x38, /*     Usage (Wheel) */
	0x15, 0x81, /*     Logical Minimum (-0x7f) */
	0x25, 0x7f, /*     Logical Maximum (0x7f) */
	0x35, 0x00, /*     Physical Minimum (same as logical) */
	0x45, 0x00, /*     Physical Maximum (same as logical) */
	0x75, 0x08, /*     Report Size (8) */
	0x95, 0x01, /*     Report Count (1) */
	0x81, 0x06, /*     Input (Data, Variable, Relative) */
	0xc0, /*   End Collection */
	0xc0, /* End Collection */
};

static const uint8_t beatmania_dev_desc[] = {
	0x12, /*  u8 bLength; */
	0x01, /*  u8 bDescriptorType; Device */
	WBVAL(0x110), /*  u16 bcdUSB; v1.10 */

	0x00, /*  u8  bDeviceClass; */
	0x00, /*  u8  bDeviceSubClass; */
	0x00, /*  u8  bDeviceProtocol; [ low/full speeds only ] */
	0x08, /*  u8  bMaxPacketSize0; 8 Bytes */

	/*  0x27, 0x06,   u16 idVendor; */
	WBVAL(0x0510),
	/* 0x01, 0x00,   u16 idProduct; */
	WBVAL(0x0002),
	WBVAL(0x0020), /*  u16 bcdDevice */

	1, /*  u8  iManufacturer; */
	2, /*  u8  iProduct; */
	0, /*  u8  iSerialNumber; */
	0x01 /*  u8  bNumConfigurations; */
};

static const uint8_t beatmania_config_desc[] = {
	0x09, /* bLength */
	0x02, /* bDescriptorType (Configuration) */
	0x22, 0x00, /* wTotalLength 34 */
	0x01, /* bNumInterfaces 1 */
	0x01, /* bConfigurationValue */
	0x02, /* iConfiguration (String Index) */
	0xA0, /* bmAttributes Remote Wakeup */
	0x14, /* bMaxPower 40mA */

	0x09, /* bLength */
	0x04, /* bDescriptorType (Interface) */
	0x00, /* bInterfaceNumber 0 */
	0x00, /* bAlternateSetting */
	0x01, /* bNumEndpoints 1 */
	0x03, /* bInterfaceClass */
	0x01, /* bInterfaceSubClass */
	0x01, /* bInterfaceProtocol */
	0x00, /* iInterface (String Index) */

	0x09, /* bLength */
	0x21, /* bDescriptorType (HID) */
	0x10, 0x01, /* bcdHID 1.10 */
	0x0F, /* bCountryCode */
	0x01, /* bNumDescriptors */
	0x22, /* bDescriptorType[0] (HID) */
	0x44, 0x00, /* wDescriptorLength[0] 68 */

	0x07, /* bLength */
	0x05, /* bDescriptorType (Endpoint) */
	0x81, /* bEndpointAddress (IN/D2H) */
	0x03, /* bmAttributes (Interrupt) */
	0x08, 0x00, /* wMaxPacketSize 8 */
	0x0A, /* bInterval 10 (unit depends on device speed) */

	/* 34 bytes */
};

static const uint8_t beatmania_dadada_hid_report_descriptor[] = {
	0x05, 0x01, /* Usage Page (Generic Desktop Ctrls) */
	0x09, 0x06, /* Usage (Keyboard) */
	0xA1, 0x01, /* Collection (Application) */
	0x05, 0x07, /*   Usage Page (Kbrd/Keypad) */
	0x19, 0xE0, /*   Usage Minimum (0xE0) */
	0x29, 0xE7, /*   Usage Maximum (0xE7) */
	0x15, 0x00, /*   Logical Minimum (0) */
	0x25, 0x01, /*   Logical Maximum (1) */
	0x75, 0x01, /*   Report Size (1) */
	0x95, 0x08, /*   Report Count (8) */
	0x81, 0x02, /*   Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position) */
	0x75, 0x08, /*   Report Size (8) */
	0x95, 0x01, /*   Report Count (1) */
	0x81, 0x01, /*   Input (Const,Array,Abs,No Wrap,Linear,Preferred State,No Null Position) */
	0x05, 0x07, /*   Usage Page (Kbrd/Keypad) */
	0x19, 0x00, /*   Usage Minimum (0x00) */
	0x29, 0xFF, /*   Usage Maximum (0xFF) */
	0x15, 0x00, /*   Logical Minimum (0) */
	0x26, 0xFF, 0x00, /*   Logical Maximum (255) */
	0x75, 0x08, /*   Report Size (8) */
	0x95, 0x06, /*   Report Count (6) */
	0x81, 0x00, /*   Input (Data,Array,Abs,No Wrap,Linear,Preferred State,No Null Position) */
	0x05, 0x08, /*   Usage Page (LEDs) */
	0x19, 0x01, /*   Usage Minimum (Num Lock) */
	0x29, 0x05, /*   Usage Maximum (Kana) */
	0x15, 0x00, /*   Logical Minimum (0) */
	0x25, 0x01, /*   Logical Maximum (1) */
	0x75, 0x01, /*   Report Size (1) */
	0x95, 0x05, /*   Report Count (5) */
	0x91, 0x02, /*   Output (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile) */
	0x75, 0x03, /*   Report Size (3) */
	0x95, 0x01, /*   Report Count (1) */
	0x91, 0x01, /*   Output (Const,Array,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile) */
	0xC0, /* End Collection */

	/* 68 bytes */
};



/* Axis selector for the mouse pointer path. Upstream this lived in
 * InputManager; defined locally here to keep the device host-agnostic.
 * Order matters: QueueMouseAxisState treats values below WheelX as
 * relative x/y motion. */
enum
{
	POINTER_AXIS_X = 0,
	POINTER_AXIS_Y,
	POINTER_AXIS_WHEEL_X,
	POINTER_AXIS_WHEEL_Y
};

typedef struct UsbHIDState
{
	USBDevice dev;
	USBDesc desc;
	USBDescDevice desc_dev;

	USBEndpoint* intr;
	HIDState hid;

	u32 port;

	/* Previous-frame state for edge detection. */
	bool key_down[RETROK_LAST];
	bool mouse_btn[3];
} UsbHIDState;

/* RETROK_* -> QKeyCode. Replaces the upstream host-string lookup through
 * InputManager::ConvertHostKeyboardStringToCode. Only the standard
 * boot-protocol keys a PS2 USB keyboard title actually reads are mapped. */
static const struct
{
	unsigned first;
	QKeyCode second;
} s_retrok_to_qcode[] = {
	{RETROK_a, Q_KEY_CODE_A}, {RETROK_b, Q_KEY_CODE_B}, {RETROK_c, Q_KEY_CODE_C},
	{RETROK_d, Q_KEY_CODE_D}, {RETROK_e, Q_KEY_CODE_E}, {RETROK_f, Q_KEY_CODE_F},
	{RETROK_g, Q_KEY_CODE_G}, {RETROK_h, Q_KEY_CODE_H}, {RETROK_i, Q_KEY_CODE_I},
	{RETROK_j, Q_KEY_CODE_J}, {RETROK_k, Q_KEY_CODE_K}, {RETROK_l, Q_KEY_CODE_L},
	{RETROK_m, Q_KEY_CODE_M}, {RETROK_n, Q_KEY_CODE_N}, {RETROK_o, Q_KEY_CODE_O},
	{RETROK_p, Q_KEY_CODE_P}, {RETROK_q, Q_KEY_CODE_Q}, {RETROK_r, Q_KEY_CODE_R},
	{RETROK_s, Q_KEY_CODE_S}, {RETROK_t, Q_KEY_CODE_T}, {RETROK_u, Q_KEY_CODE_U},
	{RETROK_v, Q_KEY_CODE_V}, {RETROK_w, Q_KEY_CODE_W}, {RETROK_x, Q_KEY_CODE_X},
	{RETROK_y, Q_KEY_CODE_Y}, {RETROK_z, Q_KEY_CODE_Z},
	{RETROK_0, Q_KEY_CODE_0}, {RETROK_1, Q_KEY_CODE_1}, {RETROK_2, Q_KEY_CODE_2},
	{RETROK_3, Q_KEY_CODE_3}, {RETROK_4, Q_KEY_CODE_4}, {RETROK_5, Q_KEY_CODE_5},
	{RETROK_6, Q_KEY_CODE_6}, {RETROK_7, Q_KEY_CODE_7}, {RETROK_8, Q_KEY_CODE_8},
	{RETROK_9, Q_KEY_CODE_9},
	{RETROK_RETURN, Q_KEY_CODE_RET}, {RETROK_ESCAPE, Q_KEY_CODE_ESC},
	{RETROK_BACKSPACE, Q_KEY_CODE_BACKSPACE}, {RETROK_TAB, Q_KEY_CODE_TAB},
	{RETROK_SPACE, Q_KEY_CODE_SPC}, {RETROK_MINUS, Q_KEY_CODE_MINUS},
	{RETROK_EQUALS, Q_KEY_CODE_EQUAL}, {RETROK_LEFTBRACKET, Q_KEY_CODE_BRACKET_LEFT},
	{RETROK_RIGHTBRACKET, Q_KEY_CODE_BRACKET_RIGHT}, {RETROK_BACKSLASH, Q_KEY_CODE_BACKSLASH},
	{RETROK_SEMICOLON, Q_KEY_CODE_SEMICOLON}, {RETROK_QUOTE, Q_KEY_CODE_APOSTROPHE},
	{RETROK_BACKQUOTE, Q_KEY_CODE_GRAVE_ACCENT}, {RETROK_COMMA, Q_KEY_CODE_COMMA},
	{RETROK_PERIOD, Q_KEY_CODE_DOT}, {RETROK_SLASH, Q_KEY_CODE_SLASH},
	{RETROK_CAPSLOCK, Q_KEY_CODE_CAPS_LOCK},
	{RETROK_F1, Q_KEY_CODE_F1}, {RETROK_F2, Q_KEY_CODE_F2}, {RETROK_F3, Q_KEY_CODE_F3},
	{RETROK_F4, Q_KEY_CODE_F4}, {RETROK_F5, Q_KEY_CODE_F5}, {RETROK_F6, Q_KEY_CODE_F6},
	{RETROK_F7, Q_KEY_CODE_F7}, {RETROK_F8, Q_KEY_CODE_F8}, {RETROK_F9, Q_KEY_CODE_F9},
	{RETROK_F10, Q_KEY_CODE_F10}, {RETROK_F11, Q_KEY_CODE_F11}, {RETROK_F12, Q_KEY_CODE_F12},
	{RETROK_RIGHT, Q_KEY_CODE_RIGHT}, {RETROK_LEFT, Q_KEY_CODE_LEFT},
	{RETROK_DOWN, Q_KEY_CODE_DOWN}, {RETROK_UP, Q_KEY_CODE_UP},
	{RETROK_INSERT, Q_KEY_CODE_INSERT}, {RETROK_HOME, Q_KEY_CODE_HOME},
	{RETROK_END, Q_KEY_CODE_END}, {RETROK_PAGEUP, Q_KEY_CODE_PGUP},
	{RETROK_PAGEDOWN, Q_KEY_CODE_PGDN}, {RETROK_DELETE, Q_KEY_CODE_DELETE},
	{RETROK_LCTRL, Q_KEY_CODE_CTRL}, {RETROK_RCTRL, Q_KEY_CODE_CTRL_R},
	{RETROK_LSHIFT, Q_KEY_CODE_SHIFT}, {RETROK_RSHIFT, Q_KEY_CODE_SHIFT_R},
	{RETROK_LALT, Q_KEY_CODE_ALT}, {RETROK_RALT, Q_KEY_CODE_ALT_R},
};

static void usb_hid_changed(HIDState* hs)
{
	UsbHIDState* us = USB_CONTAINER_OF(hs, UsbHIDState, hid);

	usb_wakeup(us->intr, 0);
}

static void usb_hid_handle_reset(USBDevice* dev)
{
	UsbHIDState* us = USB_CONTAINER_OF(dev, UsbHIDState, dev);

	hid_reset(&us->hid);
}

static void usb_hid_handle_control(USBDevice* dev, USBPacket* p,
	int request, int value, int index, int length, uint8_t* data)
{
	UsbHIDState* us = USB_CONTAINER_OF(dev, UsbHIDState, dev);
	HIDState* hs = &us->hid;
	int ret;

	ret = usb_desc_handle_control(dev, p, request, value, index, length, data);
	if (ret >= 0)
	{
		return;
	}

	switch (request)
	{
			/* hid specific requests */
		case InterfaceRequest | USB_REQ_GET_DESCRIPTOR:
			switch (value >> 8)
			{
				case 0x22:
					if (hs->kind == HID_MOUSE)
					{
						memcpy(data, qemu_mouse_hid_report_descriptor,
							sizeof(qemu_mouse_hid_report_descriptor));
						p->actual_length = sizeof(qemu_mouse_hid_report_descriptor);
					}
					else if (hs->kind == HID_TABLET)
					{
						memcpy(data, qemu_tablet_hid_report_descriptor,
							sizeof(qemu_tablet_hid_report_descriptor));
						p->actual_length = sizeof(qemu_tablet_hid_report_descriptor);
					}
					else if (hs->kind == HID_KEYBOARD)
					{
						p->actual_length = sizeof(beatmania_dadada_hid_report_descriptor);
						memcpy(data, beatmania_dadada_hid_report_descriptor, p->actual_length);
					}
					break;
				default:
					goto fail;
			}
			break;
		case GET_REPORT:
			if (hs->kind == HID_MOUSE || hs->kind == HID_TABLET)
			{
				p->actual_length = hid_pointer_poll(hs, data, length);
			}
			else if (hs->kind == HID_KEYBOARD)
			{
				p->actual_length = hid_keyboard_poll(hs, data, length);
			}
			break;
		case SET_REPORT:
			if (hs->kind == HID_KEYBOARD)
			{
				p->actual_length = hid_keyboard_write(hs, data, length);
			}
			else
			{
				goto fail;
			}
			break;
		case GET_PROTOCOL:
			if (hs->kind != HID_KEYBOARD && hs->kind != HID_MOUSE)
			{
				goto fail;
			}
			data[0] = hs->protocol;
			p->actual_length = 1;
			break;
		case SET_PROTOCOL:
			if (hs->kind != HID_KEYBOARD && hs->kind != HID_MOUSE)
			{
				goto fail;
			}
			hs->protocol = value;
			break;
		case GET_IDLE:
			data[0] = hs->idle;
			p->actual_length = 1;
			break;
		case SET_IDLE:
			hs->idle = (uint8_t)(value >> 8);
			hid_set_next_idle(hs);
			if (hs->kind == HID_MOUSE || hs->kind == HID_TABLET)
			{
				hid_pointer_activate(hs);
			}
			break;
		default:
		fail:
			p->status = USB_RET_STALL;
			break;
	}
}

static void usb_hid_handle_data(USBDevice* dev, USBPacket* p)
{
	UsbHIDState* us = USB_CONTAINER_OF(dev, UsbHIDState, dev);
	HIDState* hs = &us->hid;
	uint8_t buf[16];
	size_t len = 0;

	switch (p->pid)
	{
		case USB_TOKEN_IN:
			if (p->ep->nr == 1)
			{
				if (hs->kind == HID_MOUSE || hs->kind == HID_TABLET)
				{
					hid_pointer_activate(hs);
				}
				if (!hid_has_events(hs))
				{
					p->status = USB_RET_NAK;
					return;
				}
				hid_set_next_idle(hs);
				if (hs->kind == HID_MOUSE || hs->kind == HID_TABLET)
				{
					len = hid_pointer_poll(hs, buf, p->buffer_size);
				}
				else if (hs->kind == HID_KEYBOARD)
				{
					len = hid_keyboard_poll(hs, buf, p->buffer_size);
				}
				usb_packet_copy(p, buf, len);
			}
			else
			{
				goto fail;
			}
			break;
		case USB_TOKEN_OUT:
		default:
		fail:
			p->status = USB_RET_STALL;
			break;
	}
}

static void usb_hid_unrealize(USBDevice* dev)
{
	UsbHIDState* us = USB_CONTAINER_OF(dev, UsbHIDState, dev);
	hid_free(&us->hid);
	usb_desc_device_free(&us->desc_dev);
	free(us);
}


static void hid_queue_mouse_button(UsbHIDState* s, InputButton button, bool pressed)
{
	InputEvent evt;
	evt.type = INPUT_EVENT_KIND_BTN;
	evt.u.btn.button = button;
	evt.u.btn.down = pressed;
	s->hid.u.ptr.eh_entry(&s->hid, &evt);
	s->hid.u.ptr.eh_sync(&s->hid);
}

/* Deltas are integers end to end: the retro mouse callback hands out
 * ints, and the HID event value is an s64 - the float in between was
 * pure laundering. */
static void hid_queue_mouse_axis(UsbHIDState* s, int axis, s32 delta)
{
	if (axis < POINTER_AXIS_WHEEL_X)
	{
		/* x/y */
		InputEvent evt;
		evt.type = INPUT_EVENT_KIND_REL;
		evt.u.rel.axis = ((InputAxis)(axis));
		evt.u.rel.value = ((s64)(delta));
		s->hid.u.ptr.eh_entry(&s->hid, &evt);
		s->hid.u.ptr.eh_sync(&s->hid);
	}
	else if (axis == POINTER_AXIS_WHEEL_Y)
	{
		InputEvent evt;
		evt.type = INPUT_EVENT_KIND_BTN;
		evt.u.btn.button = (delta > 0) ? INPUT_BUTTON_WHEEL_UP : INPUT_BUTTON_WHEEL_DOWN;
		evt.u.btn.down = true;
		s->hid.u.ptr.eh_entry(&s->hid, &evt);
		s->hid.u.ptr.eh_sync(&s->hid);
	}
}
static void hid_queue_key(UsbHIDState* s, KeyValue keycode, bool pressed)
{
	InputEvent evt;
	evt.type = INPUT_EVENT_KIND_KEY;
	evt.u.key.key = keycode;
	evt.u.key.down = pressed;
	s->hid.u.kbd.eh_entry(&s->hid, &evt);
}

USBDevice* usb_hid_create_kbd(u32 port)
{
	UsbHIDState* s = (UsbHIDState*)calloc(1, sizeof(UsbHIDState));
	if (!s)
		return NULL;
	s->port = port;
	s->desc.full = &s->desc_dev;
	s->desc.str = beatmania_dadada_desc_strings;

	if (usb_desc_parse_dev(beatmania_dev_desc, sizeof(beatmania_dev_desc), &s->desc, &s->desc_dev) < 0)
		goto fail;
	if (usb_desc_parse_config(beatmania_config_desc, sizeof(beatmania_config_desc), &s->desc_dev) < 0)
		goto fail;

	s->dev.speed = USB_SPEED_FULL;
	s->dev.klass.handle_attach = usb_desc_attach;
	s->dev.klass.handle_reset = usb_hid_handle_reset;
	s->dev.klass.handle_control = usb_hid_handle_control;
	s->dev.klass.handle_data = usb_hid_handle_data;
	s->dev.klass.unrealize = usb_hid_unrealize;
	s->dev.klass.usb_desc = &s->desc;
	s->dev.klass.product_desc = s->desc.str[2];

	usb_desc_init(&s->dev);
	usb_ep_init(&s->dev);
	s->intr = usb_ep_get(&s->dev, USB_TOKEN_IN, 1);
	hid_init(&s->hid, HID_KEYBOARD, usb_hid_changed);

	usb_hid_handle_reset(&s->dev);
	return &s->dev;
fail:
	usb_hid_unrealize(&s->dev);
	return NULL;
}

USBDevice* usb_hid_create_mouse(u32 port)
{
	UsbHIDState* s = (UsbHIDState*)calloc(1, sizeof(UsbHIDState));
	if (!s)
		return NULL;
	s->port = port;
	s->desc.full = &s->desc_dev;
	s->desc.str = desc_strings;

	if (usb_desc_parse_dev(qemu_mouse_dev_descriptor, sizeof(qemu_mouse_dev_descriptor), &s->desc, &s->desc_dev) < 0)
		goto fail;
	if (usb_desc_parse_config(qemu_mouse_config_descriptor, sizeof(qemu_mouse_config_descriptor), &s->desc_dev) < 0)
		goto fail;

	s->dev.speed = USB_SPEED_FULL;
	s->dev.klass.handle_attach = usb_desc_attach;
	s->dev.klass.handle_reset = usb_hid_handle_reset;
	s->dev.klass.handle_control = usb_hid_handle_control;
	s->dev.klass.handle_data = usb_hid_handle_data;
	s->dev.klass.unrealize = usb_hid_unrealize;
	s->dev.klass.usb_desc = &s->desc;
	s->dev.klass.product_desc = s->desc.str[STR_CONFIG_MOUSE];

	usb_desc_init(&s->dev);
	usb_ep_init(&s->dev);
	s->intr = usb_ep_get(&s->dev, USB_TOKEN_IN, 1);
	hid_init(&s->hid, HID_MOUSE, usb_hid_changed);

	usb_hid_handle_reset(&s->dev);
	return &s->dev;
fail:
	usb_hid_unrealize(&s->dev);
	return NULL;
}

/* Pump one frame of frontend input into the HID event queue. Called from
 * USBasync via usb_hid_update(). input_cb is the libretro input_state
 * callback; port is the OHCI port the device sits on (used as the libretro
 * controller port for the keyboard/mouse reads). */
void usb_hid_update(USBDevice* dev, retro_input_state_t input_cb, unsigned port)
{
	UsbHIDState* s = USB_CONTAINER_OF(dev, UsbHIDState, dev);
	if (!input_cb)
		return;

	if (s->hid.kind == HID_KEYBOARD)
	{
		size_t i;
		for (i = 0; i < sizeof(s_retrok_to_qcode) / sizeof(s_retrok_to_qcode[0]); i++)
		{
			const unsigned rk = s_retrok_to_qcode[i].first;
			const bool down = input_cb(port, RETRO_DEVICE_KEYBOARD, 0, rk) != 0;
			KeyValue kv;
			if (down == s->key_down[rk])
				continue;
			s->key_down[rk] = down;

			kv.type = KEY_VALUE_KIND_QCODE;
			kv.u.qcode = s_retrok_to_qcode[i].second;
			hid_queue_key(s, kv, down);
		}
	}
	else if (s->hid.kind == HID_MOUSE)
	{
		const int dx = input_cb(port, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_X);
		const int dy = input_cb(port, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_Y);
		const bool bl = input_cb(port, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_LEFT) != 0;
		const bool br = input_cb(port, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_RIGHT) != 0;
		const bool bm = input_cb(port, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_MIDDLE) != 0;
		if (dx)
			hid_queue_mouse_axis(s, POINTER_AXIS_X, dx);
		if (dy)
			hid_queue_mouse_axis(s, POINTER_AXIS_Y, dy);

		if (bl != s->mouse_btn[0]) { s->mouse_btn[0] = bl; hid_queue_mouse_button(s, INPUT_BUTTON_LEFT, bl); }
		if (br != s->mouse_btn[1]) { s->mouse_btn[1] = br; hid_queue_mouse_button(s, INPUT_BUTTON_RIGHT, br); }
		if (bm != s->mouse_btn[2]) { s->mouse_btn[2] = bm; hid_queue_mouse_button(s, INPUT_BUTTON_MIDDLE, bm); }
	}
}

void usb_hid_freeze(USBDevice* dev, UsbState* st)
{
	UsbHIDState* s = USB_CONTAINER_OF(dev, UsbHIDState, dev);

	if (s->hid.kind == HID_KEYBOARD)
	{
		usb_state_bytes(st, s->hid.u.kbd.keycodes, sizeof(s->hid.u.kbd.keycodes));
		USB_STATE_DO(st, s->hid.u.kbd.modifiers);
		USB_STATE_DO(st, s->hid.u.kbd.leds);
		usb_state_bytes(st, s->hid.u.kbd.key, sizeof(s->hid.u.kbd.key));
		USB_STATE_DO(st, s->hid.u.kbd.keys);
	}
	else
	{
		usb_state_bytes(st, s->hid.u.ptr.queue, sizeof(s->hid.u.ptr.queue));
		USB_STATE_DO(st, s->hid.u.ptr.mouse_grabbed);
	}
	USB_STATE_DO(st, s->hid.head);
	USB_STATE_DO(st, s->hid.n);
	USB_STATE_DO(st, s->hid.protocol);
	USB_STATE_DO(st, s->hid.idle);
}
