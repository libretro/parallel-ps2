/* Perfect Pool's table camera. See ppcam.h. */
#include "ppcam.h"

#include "USB/libretro-usb/USBinternal.h"
#include "USB/libretro-usb/desc.h"

#include <stdlib.h>
#include <string.h>

static const USBDescStrings ppcam_strings = {
	"Cypress",
	"AN2235 EZ-USB FX Microcontroller",
	"Perfect Pool Camera",
	"USBIO",
};

static const uint8_t ppcam_dev_desc[] = {
	0x12,          /* bLength */
	0x01,          /* bDescriptorType */
	WBVAL(0x0100), /* bcdUSB */
	0x00,          /* bDeviceClass */
	0x00,          /* bDeviceSubClass */
	0x00,          /* bDeviceProtocol */
	0x40,          /* bMaxPacketSize0 */
	WBVAL(0x0547), /* idVendor: Cypress */
	WBVAL(0x2235), /* idProduct: AN2235 EZ-USB FX */
	WBVAL(0x0001), /* bcdDevice */
	0x01,          /* iManufacturer */
	0x02,          /* iProduct */
	0x00,          /* iSerialNumber */
	0x01           /* bNumConfigurations */
};

/* Interrupt OUT 1 and IN 2, bulk OUT 3 and IN 4; alternate setting 2, as
 * the board's firmware describes it to usbio.irx. */
static const uint8_t ppcam_config_desc[] = {
	0x09, 0x02, WBVAL(0x2e), 0x01, 0x01, 0x04, 0x80, 0xfa,
	0x09, 0x04, 0x00, 0x02, 0x04, 0xff, 0x00, 0xff, 0x04,
	0x07, 0x05, 0x01, 0x03, WBVAL(0x10), 0x01,
	0x07, 0x05, 0x82, 0x03, WBVAL(0x10), 0x01,
	0x07, 0x05, 0x03, 0x02, WBVAL(0x40), 0x00,
	0x07, 0x05, 0x84, 0x02, WBVAL(0x40), 0x00
};

typedef struct PPCamState
{
	USBDevice dev;
	USBDesc desc;
	USBDescDevice desc_dev;
	u8 ram[USB_PPCAM_RAM_SIZE];
	u8 held_in_reset;
	u8 downloaded;
} PPCamState;

static void ppcam_reset(USBDevice* dev)
{
	PPCamState* s = USB_CONTAINER_OF(dev, PPCamState, dev);
	/* The firmware, once in, survives the reset that starts it. */
	if (!s->downloaded)
	{
		s->held_in_reset = 1;
		memset(s->ram, 0, sizeof(s->ram));
	}
}

static void ppcam_unrealize(USBDevice* dev)
{
	PPCamState* s = USB_CONTAINER_OF(dev, PPCamState, dev);
	usb_desc_device_free(&s->desc_dev);
	free(s);
}

static u32 ram_span(int value, int length)
{
	const u32 addr = (u32)value & 0xffff;
	const u32 len = length > 0 ? (u32)length : 0;
	return len < USB_PPCAM_RAM_SIZE - addr ? len : USB_PPCAM_RAM_SIZE - addr;
}

static void ppcam_handle_control(USBDevice* dev, USBPacket* p,
	int request, int value, int index, int length, uint8_t* data)
{
	PPCamState* s = USB_CONTAINER_OF(dev, PPCamState, dev);
	const u32 addr = (u32)value & 0xffff;
	u32 n;

	if (usb_desc_handle_control(dev, p, request, value, index, length, data) >= 0)
		return;
	switch (request)
	{
		case VendorDeviceOutRequest | 0xa0: /* firmware load */
			n = ram_span(value, length);
			if (n)
			{
				memcpy(s->ram + addr, data, n);
				s->downloaded = 1;
			}
			/* The 8051's reset latch: 0x7f92 on the FX, CPUCS at 0xe600
			 * on its successors. Let go with firmware in, it starts and
			 * comes back on the bus as the camera. */
			if ((addr == 0x7f92 || addr == 0xe600) && n)
			{
				const u8 was = s->held_in_reset;
				s->held_in_reset = (u8)(data[0] & 1);
				if (was && !s->held_in_reset && s->downloaded && dev->port)
					usb_reattach(dev->port);
			}
			p->actual_length = (int)n;
			break;
		case VendorDeviceRequest | 0xa0:
			n = ram_span(value, length);
			if (n)
				memcpy(data, s->ram + addr, n);
			p->actual_length = (int)n;
			break;
		default:
			p->status = USB_RET_STALL;
			break;
	}
}

static void ppcam_handle_data(USBDevice* dev, USBPacket* p)
{
	static const u8 nothing[0x800];
	size_t n;
	(void)dev;
	switch (p->pid)
	{
		case USB_TOKEN_IN:
			if (p->ep->nr == 2)
				n = 0x10;
			else if (p->ep->nr == 4)
				n = 0x800;
			else
			{
				p->status = USB_RET_STALL;
				return;
			}
			if (n > p->buffer_size)
				n = p->buffer_size;
			usb_packet_copy(p, (void*)nothing, n);
			break;
		case USB_TOKEN_OUT:
			if (p->ep->nr != 1 && p->ep->nr != 3)
				p->status = USB_RET_STALL;
			else
				p->actual_length = (int)usb_packet_size(p);
			break;
		default:
			p->status = USB_RET_STALL;
			break;
	}
}

USBDevice* usb_ppcam_create(void)
{
	PPCamState* s = (PPCamState*)calloc(1, sizeof(PPCamState));
	if (!s)
		return NULL;
	s->desc.full = &s->desc_dev;
	s->desc.str = ppcam_strings;
	if (usb_desc_parse_dev(ppcam_dev_desc, sizeof(ppcam_dev_desc), &s->desc, &s->desc_dev) < 0)
		goto fail;
	if (usb_desc_parse_config(ppcam_config_desc, sizeof(ppcam_config_desc), &s->desc_dev) < 0)
		goto fail;

	s->dev.speed = USB_SPEED_FULL;
	s->dev.klass.handle_attach = usb_desc_attach;
	s->dev.klass.handle_reset = ppcam_reset;
	s->dev.klass.handle_control = ppcam_handle_control;
	s->dev.klass.handle_data = ppcam_handle_data;
	s->dev.klass.unrealize = ppcam_unrealize;
	s->dev.klass.usb_desc = &s->desc;
	s->dev.klass.product_desc = s->desc.str[2];

	usb_desc_init(&s->dev);
	usb_ep_init(&s->dev);
	ppcam_reset(&s->dev);
	return &s->dev;
fail:
	ppcam_unrealize(&s->dev);
	return NULL;
}

void usb_ppcam_freeze(USBDevice* dev, UsbState* slot, UsbState* ram)
{
	PPCamState* s = USB_CONTAINER_OF(dev, PPCamState, dev);
	u8 held = s->held_in_reset, downloaded = s->downloaded;
	USB_STATE_DO(slot, held);
	USB_STATE_DO(slot, downloaded);
	if (slot->reading && (held > 1 || downloaded > 1))
		slot->error = true;
	if (slot->error)
		return;
	/* Whole or not at all: the RAM block is checked before it is taken. */
	usb_state_bytes(ram, s->ram, sizeof(s->ram));
	if (ram->error)
	{
		slot->error = true;
		return;
	}
	if (slot->reading)
	{
		s->held_in_reset = held;
		s->downloaded = downloaded;
	}
}
