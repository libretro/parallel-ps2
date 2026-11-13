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

	/* A trackball's motion not yet sent, in half counts. */
	bool trackball;
	s32 roll[2];
} UsbHIDState;

/* RETROK_* -> HID usage, the boot-protocol keys a PS2 USB keyboard title
 * actually reads. */
static const struct
{
	unsigned retrok;
	u8 usage;
} s_retrok_to_hid[] = {
	{RETROK_a, 0x04}, {RETROK_b, 0x05}, {RETROK_c, 0x06},
	{RETROK_d, 0x07}, {RETROK_e, 0x08}, {RETROK_f, 0x09},
	{RETROK_g, 0x0a}, {RETROK_h, 0x0b}, {RETROK_i, 0x0c},
	{RETROK_j, 0x0d}, {RETROK_k, 0x0e}, {RETROK_l, 0x0f},
	{RETROK_m, 0x10}, {RETROK_n, 0x11}, {RETROK_o, 0x12},
	{RETROK_p, 0x13}, {RETROK_q, 0x14}, {RETROK_r, 0x15},
	{RETROK_s, 0x16}, {RETROK_t, 0x17}, {RETROK_u, 0x18},
	{RETROK_v, 0x19}, {RETROK_w, 0x1a}, {RETROK_x, 0x1b},
	{RETROK_y, 0x1c}, {RETROK_z, 0x1d}, {RETROK_0, 0x27},
	{RETROK_1, 0x1e}, {RETROK_2, 0x1f}, {RETROK_3, 0x20},
	{RETROK_4, 0x21}, {RETROK_5, 0x22}, {RETROK_6, 0x23},
	{RETROK_7, 0x24}, {RETROK_8, 0x25}, {RETROK_9, 0x26},
	{RETROK_RETURN, 0x28}, {RETROK_ESCAPE, 0x29}, {RETROK_BACKSPACE, 0x2a},
	{RETROK_TAB, 0x2b}, {RETROK_SPACE, 0x2c}, {RETROK_MINUS, 0x2d},
	{RETROK_EQUALS, 0x2e}, {RETROK_LEFTBRACKET, 0x2f}, {RETROK_RIGHTBRACKET, 0x30},
	{RETROK_BACKSLASH, 0x31}, {RETROK_SEMICOLON, 0x33}, {RETROK_QUOTE, 0x34},
	{RETROK_BACKQUOTE, 0x35}, {RETROK_COMMA, 0x36}, {RETROK_PERIOD, 0x37},
	{RETROK_SLASH, 0x38}, {RETROK_CAPSLOCK, 0x39}, {RETROK_F1, 0x3a},
	{RETROK_F2, 0x3b}, {RETROK_F3, 0x3c}, {RETROK_F4, 0x3d},
	{RETROK_F5, 0x3e}, {RETROK_F6, 0x3f}, {RETROK_F7, 0x40},
	{RETROK_F8, 0x41}, {RETROK_F9, 0x42}, {RETROK_F10, 0x43},
	{RETROK_F11, 0x44}, {RETROK_F12, 0x45}, {RETROK_RIGHT, 0x4f},
	{RETROK_LEFT, 0x50}, {RETROK_DOWN, 0x51}, {RETROK_UP, 0x52},
	{RETROK_INSERT, 0x49}, {RETROK_HOME, 0x4a}, {RETROK_END, 0x4d},
	{RETROK_PAGEUP, 0x4b}, {RETROK_PAGEDOWN, 0x4e}, {RETROK_DELETE, 0x4c},
	{RETROK_LCTRL, 0xe0}, {RETROK_RCTRL, 0xe4}, {RETROK_LSHIFT, 0xe1},
	{RETROK_RSHIFT, 0xe5}, {RETROK_LALT, 0xe2}, {RETROK_RALT, 0xe6},
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
	us->roll[0] = us->roll[1] = 0;
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

USBDevice* usb_hid_create_trackball(u32 port)
{
	USBDevice* dev = usb_hid_create_mouse(port);
	if (dev)
		USB_CONTAINER_OF(dev, UsbHIDState, dev)->trackball = true;
	return dev;
}

/* Half of what has rolled, whole counts toward zero; the rest is kept.
 * A report carries at most 127 either way and the excess is dropped. */
static s32 trackball_counts(s32* roll, s32 delta)
{
	s32 n;
	*roll += delta;
	n = *roll >= 0 ? *roll / 2 : -(-*roll / 2);
	*roll -= n * 2;
	return n > 127 ? 127 : n < -127 ? -127 : n;
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
		for (i = 0; i < sizeof(s_retrok_to_hid) / sizeof(s_retrok_to_hid[0]); i++)
		{
			const unsigned rk = s_retrok_to_hid[i].retrok;
			const bool down = input_cb(port, RETRO_DEVICE_KEYBOARD, 0, rk) != 0;
			if (down == s->key_down[rk])
				continue;
			s->key_down[rk] = down;
			hid_keyboard_key(&s->hid, s_retrok_to_hid[i].usage, down);
		}
	}
	else if (s->hid.kind == HID_MOUSE)
	{
		const int dx = input_cb(port, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_X);
		const int dy = input_cb(port, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_Y);
		const bool bl = input_cb(port, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_LEFT) != 0;
		const bool br = input_cb(port, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_RIGHT) != 0;
		const bool bm = input_cb(port, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_MIDDLE) != 0;
		if (s->trackball)
		{
			/* The stick at full tilt is 32 counts a frame before halving. */
			const int sx = input_cb(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X) / 1024;
			const int sy = input_cb(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y) / 1024;
			s32 nx, ny;
			if (bl)
				s->roll[0] = s->roll[1] = 0;
			else
			{
				if ((nx = trackball_counts(&s->roll[0], dx + sx)) != 0)
					hid_queue_mouse_axis(s, POINTER_AXIS_X, nx);
				if ((ny = trackball_counts(&s->roll[1], dy + sy)) != 0)
					hid_queue_mouse_axis(s, POINTER_AXIS_Y, ny);
			}
		}
		else
		{
			if (dx)
				hid_queue_mouse_axis(s, POINTER_AXIS_X, dx);
			if (dy)
				hid_queue_mouse_axis(s, POINTER_AXIS_Y, dy);
		}

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
		if (s->trackball)
		{
			usb_state_bytes(st, s->roll, sizeof(s->roll));
			if (st->reading && (s->roll[0] < -1 || s->roll[0] > 1 || s->roll[1] < -1 || s->roll[1] > 1))
				st->error = true;
		}
	}
	USB_STATE_DO(st, s->hid.head);
	USB_STATE_DO(st, s->hid.n);
	USB_STATE_DO(st, s->hid.protocol);
	USB_STATE_DO(st, s->hid.idle);
	if (st->reading && s->hid.kind == HID_KEYBOARD)
		hid_keyboard_loaded(&s->hid);
}
