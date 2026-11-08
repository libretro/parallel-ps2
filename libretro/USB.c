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

#include <stdlib.h>
#include <string.h>

#include "../pcsx2/USB/USB.h"
#include "../pcsx2/USB/usb-host.h"
#include "../pcsx2/USB/libretro-usb/USBinternal.h"
#include "../pcsx2/USB/libretro-usb/usb-hid.h"
#include "../pcsx2/USB/usb-eyetoy/eyetoy.h"
#include "../pcsx2/USB/usb-python2/usb-python2.h"
#include "../pcsx2/USB/libretro-usb/desc.h"
#include "../pcsx2/USB/libretro-usb/usb-state.h"

#include "libretro.h"

/* IOP clock, 36.864 MHz. */
#define PSXCLK 36864000

/* OHCI host-controller clock-timing globals. Computed inside ohci_create()
 * from usb_get_ticks_per_second(); defined here so the controller core has
 * storage to link against. */
s64 g_usb_frame_time = 0;
s64 g_usb_bit_time   = 0;
s64 g_usb_last_cycle = 0;

static OHCIState* s_qemu_ohci   = NULL;
static s64        s_usb_clocks  = 0;
static s64        s_usb_remaining = 0;

static int        s_port_device[USB_NUM_PORTS] = { USB_DEV_NONE, USB_DEV_NONE };
static unsigned   s_input_port[USB_NUM_PORTS]  = { 0, 1 };
static int        s_camera      = USB_CAMERA_NONE;
static int        s_camera_port = -1;
static int        s_python2_port = -1;
static USBDevice* s_usb_device[USB_NUM_PORTS]  = { NULL, NULL };


/* Set the device type for a USB port and which frontend controller port it
 * reads input from. Takes effect on the next USBopen()/USBreset(). For most
 * cases input_port == port; the combined keyboard+mouse selection routes both
 * USB ports to the same frontend port. */
void USBSetPortDevice(unsigned port, int device, unsigned input_port)
{
	if (port < USB_NUM_PORTS)
	{
		s_port_device[port] = device;
		s_input_port[port]  = input_port;
	}
}

static OHCIPort* GetOHCIPort(u32 port)
{
	const u32 rhport = (port < s_qemu_ohci->num_ports) ? port : 0;
	return &s_qemu_ohci->rhport[rhport];
}

void USBSetCamera(int camera)
{
	s_camera = camera;
}

static void USBAttachDevice(u32 port, USBDevice* dev)
{
	GetOHCIPort(port)->port.dev = dev;
	dev->attached = true;
	usb_attach(&GetOHCIPort(port)->port);
	s_usb_device[port] = dev;
}

static void USBCreateDevice(u32 port)
{
	USBDevice* dev = NULL;
	/* A Python 2 game's I/O board is on the first port, whatever the
	 * port's own setting. */
	if (port == 0 && usb_python2_has_config())
	{
		if ((dev = usb_python2_create()))
		{
			USBAttachDevice(port, dev);
			s_python2_port = 0;
		}
		return;
	}
	switch (s_port_device[port])
	{
		case USB_DEV_KEYBOARD:
			dev = usb_hid_create_kbd(port);
			break;
		case USB_DEV_MOUSE:
			dev = usb_hid_create_mouse(port);
			break;
		default:
			return;
	}
	if (dev)
		USBAttachDevice(port, dev);
}

static void USBCreateCamera(void)
{
	u32 port;
	USBDevice* dev;

	s_camera_port = -1;
	if (s_camera == USB_CAMERA_NONE)
		return;
	for (port = 0; port < USB_NUM_PORTS && s_usb_device[port]; port++)
		;
	if (port == USB_NUM_PORTS)
	{
		log_cb(RETRO_LOG_WARN, "USB: no free port for the camera\n");
		return;
	}
	dev = usb_eyetoy_create(
		s_camera == USB_CAMERA_EYETOY ? USB_EYETOY_MODEL_EYETOY : USB_EYETOY_MODEL_OV511P);
	if (!dev)
		return;
	USBAttachDevice(port, dev);
	s_camera_port = (int)port;
}

static void USBDestroyDevice(u32 port)
{
	USBDevice* dev = s_usb_device[port];
	if (!dev)
		return;
	if (dev->klass.unrealize)
		dev->klass.unrealize(dev);
	GetOHCIPort(port)->port.dev = NULL;
	s_usb_device[port] = NULL;
	if ((int)port == s_python2_port)
		s_python2_port = -1;
}

int usb_get_ticks_per_second(void)
{
	return PSXCLK;
}

s64 usb_get_clock(void)
{
	return s_usb_clocks;
}

void USBconfigure(void) {}

void USBinit(void) {}

void USBshutdown(void) {}

bool USBopen(void)
{
	u32 port;
	s_qemu_ohci = ohci_create(0x1f801600, 2);
	if (!s_qemu_ohci)
		return false;

	s_usb_clocks     = 0;
	s_usb_remaining  = 0;
	g_usb_last_cycle = 0;

	for (port = 0; port < USB_NUM_PORTS; port++)
		USBCreateDevice(port);
	USBCreateCamera();
	return true;
}

void USBclose(void)
{
	u32 port;
	for (port = 0; port < USB_NUM_PORTS; port++)
		USBDestroyDevice(port);
	if (s_qemu_ohci)
	{
		free(s_qemu_ohci);
		s_qemu_ohci = NULL;
	}
}

void USBreset(void)
{
	u32 port;

	s_usb_clocks     = 0;
	s_usb_remaining  = 0;
	g_usb_last_cycle = 0;
	if (!s_qemu_ohci)
		return;

	/* Reconcile attached devices with the current per-port selection, so a
	 * device-type change applied while running takes effect on reset. */
	for (port = 0; port < USB_NUM_PORTS; port++)
		USBDestroyDevice(port);

	ohci_hard_reset(s_qemu_ohci);

	for (port = 0; port < USB_NUM_PORTS; port++)
		USBCreateDevice(port);
	USBCreateCamera();
}

void USBasync(u32 cycles)
{
	if (!s_qemu_ohci)
		return;

	/* Pump one frame of frontend keyboard/mouse input into attached HID
	 * devices before advancing the controller. */
	{
		retro_input_state_t input_cb = PADGetInputStateCallback();
		u32 port;
		for (port = 0; port < USB_NUM_PORTS; port++)
		{
			if (!s_usb_device[port] || (int)port == s_camera_port)
				continue;
			if ((int)port == s_python2_port)
				usb_python2_update(s_usb_device[port], input_cb);
			else
				usb_hid_update(s_usb_device[port], input_cb, s_input_port[port]);
		}
	}

	s_usb_remaining += cycles;
	s_usb_clocks    += s_usb_remaining;
	if (s_qemu_ohci->eof_timer > 0)
	{
		while ((u64)s_usb_remaining >= s_qemu_ohci->eof_timer)
		{
			s_usb_remaining -= s_qemu_ohci->eof_timer;
			s_qemu_ohci->eof_timer = 0;
			ohci_frame_boundary(s_qemu_ohci);

			/* Break out of the loop if the bus was stopped. */
			if (!s_qemu_ohci->eof_timer)
				break;
		}
		if ((s_usb_remaining > 0) && (s_qemu_ohci->eof_timer > 0))
		{
			s64 m = s_qemu_ohci->eof_timer;
			if (s_usb_remaining < m)
				m = s_usb_remaining;
			s_qemu_ohci->eof_timer -= m;
			s_usb_remaining        -= m;
		}
	}
}

/* The savestate block: the controller, a slot per port, then a footer of
 * size, version and magic. Fixed size, so retro_serialize_size does not
 * change with what is plugged in. */
#define USB_STATE_OHCI    10240
#define USB_STATE_SLOT    8192
#define USB_STATE_SIZE    (USB_STATE_OHCI + USB_NUM_PORTS * USB_STATE_SLOT + 16)
#define USB_STATE_VERSION 1
#define USB_STATE_MAGIC   "LRPS2USB"

enum UsbStateKind
{
	USB_KIND_NONE = 0,
	USB_KIND_KEYBOARD,
	USB_KIND_MOUSE,
	USB_KIND_EYETOY,
	USB_KIND_CAPTURE_EYE,
	USB_KIND_PYTHON2
};

static u32 USBPortKind(u32 port)
{
	if (!s_usb_device[port])
		return USB_KIND_NONE;
	if ((int)port == s_camera_port)
		return s_camera == USB_CAMERA_EYETOY ? USB_KIND_EYETOY : USB_KIND_CAPTURE_EYE;
	if ((int)port == s_python2_port)
		return USB_KIND_PYTHON2;
	return s_port_device[port] == USB_DEV_KEYBOARD ? USB_KIND_KEYBOARD : USB_KIND_MOUSE;
}

static void USBFreezeOHCI(UsbState* st)
{
	OHCIState* o = s_qemu_ohci;
	u32 fsmps = o->fsmps, fit = o->fit, fi = o->fi, frt = o->frt;
	u32 i;

	USB_STATE_DO(st, g_usb_last_cycle);
	USB_STATE_DO(st, s_usb_clocks);
	USB_STATE_DO(st, s_usb_remaining);
	USB_STATE_DO(st, o->eof_timer);
	USB_STATE_DO(st, o->sof_time);
	USB_STATE_DO(st, o->ctl);
	USB_STATE_DO(st, o->status);
	USB_STATE_DO(st, o->intr_status);
	USB_STATE_DO(st, o->intr);
	USB_STATE_DO(st, o->hcca);
	USB_STATE_DO(st, o->ctrl_head);
	USB_STATE_DO(st, o->ctrl_cur);
	USB_STATE_DO(st, o->bulk_head);
	USB_STATE_DO(st, o->bulk_cur);
	USB_STATE_DO(st, o->per_cur);
	USB_STATE_DO(st, o->done);
	USB_STATE_DO(st, o->done_count);
	USB_STATE_DO(st, fsmps);
	USB_STATE_DO(st, fit);
	USB_STATE_DO(st, fi);
	USB_STATE_DO(st, frt);
	USB_STATE_DO(st, o->frame_number);
	USB_STATE_DO(st, o->pstart);
	USB_STATE_DO(st, o->lst);
	USB_STATE_DO(st, o->rhdesc_a);
	USB_STATE_DO(st, o->rhdesc_b);
	for (i = 0; i < OHCI_MAX_PORTS; i++)
		USB_STATE_DO(st, o->rhport[i].ctrl);
	USB_STATE_DO(st, o->old_ctl);
	usb_state_bytes(st, o->usb_buf, sizeof(o->usb_buf));
	USB_STATE_DO(st, o->async_td);
	USB_STATE_DO(st, o->async_complete);
	if (st->reading)
	{
		o->fsmps = fsmps;
		o->fit   = fit;
		o->fi    = fi;
		o->frt   = frt;
	}
}

static void USBFreezeEndpoint(USBEndpoint* ep, UsbState* st)
{
	USB_STATE_DO(st, ep->pipeline);
	USB_STATE_DO(st, ep->halted);
	if (st->reading)
		while (!QTAILQ_EMPTY(&ep->queue))
			QTAILQ_REMOVE(&ep->queue, QTAILQ_FIRST(&ep->queue), queue);
}

/* What every USB device has: address, configuration, endpoints. */
static void USBFreezeDevice(USBDevice* dev, UsbState* st)
{
	int altsetting[USB_MAX_INTERFACES];
	u32 i;

	USB_STATE_DO(st, dev->speed);
	USB_STATE_DO(st, dev->addr);
	USB_STATE_DO(st, dev->state);
	usb_state_bytes(st, dev->setup_buf, sizeof(dev->setup_buf));
	usb_state_bytes(st, dev->data_buf, sizeof(dev->data_buf));
	USB_STATE_DO(st, dev->remote_wakeup);
	USB_STATE_DO(st, dev->setup_state);
	USB_STATE_DO(st, dev->setup_len);
	USB_STATE_DO(st, dev->setup_index);
	USB_STATE_DO(st, dev->configuration);
	if (st->reading && !st->error)
	{
		/* A configuration the descriptors do not have is a bad slot, not
		 * a device left on whatever it had before. */
		const int configuration = dev->configuration;
		usb_desc_set_config(dev, configuration);
		if (configuration != 0 && (!dev->config || dev->config->bConfigurationValue != configuration))
			st->error = true;
	}
	memcpy(altsetting, dev->altsetting, sizeof(altsetting));
	usb_state_bytes(st, altsetting, sizeof(altsetting));
	if (st->reading && !st->error)
		for (i = 0; i < (u32)dev->ninterfaces && i < USB_MAX_INTERFACES; i++)
		{
			if (usb_desc_set_interface(dev, i, altsetting[i]) < 0)
				st->error = true;
		}
	USBFreezeEndpoint(&dev->ep_ctl, st);
	for (i = 0; i < USB_MAX_ENDPOINTS; i++)
		USBFreezeEndpoint(&dev->ep_in[i], st);
	for (i = 0; i < USB_MAX_ENDPOINTS; i++)
		USBFreezeEndpoint(&dev->ep_out[i], st);
}

static void USBFreezePort(u32 port, u32 kind, UsbState* st)
{
	USBDevice* dev = s_usb_device[port];

	USBFreezeDevice(dev, st);
	if (kind == USB_KIND_EYETOY || kind == USB_KIND_CAPTURE_EYE)
		usb_eyetoy_freeze(dev, st);
	else if (kind == USB_KIND_PYTHON2)
		usb_python2_freeze(dev, st);
	else
		usb_hid_freeze(dev, st);
}

static const u8* USBStateFind(const void* state, size_t size)
{
	const u8* end = (const u8*)state + size;
	u32 block_size, version;

	if (size < USB_STATE_SIZE)
		return NULL;
	memcpy(&block_size, end - 16, 4);
	memcpy(&version, end - 12, 4);
	if (memcmp(end - 8, USB_STATE_MAGIC, 8) || block_size != USB_STATE_SIZE || version != USB_STATE_VERSION)
		return NULL;
	return end - USB_STATE_SIZE;
}

size_t USBStateSize(void)
{
	return USB_STATE_SIZE;
}

/* A block of size at data, read or written. */
static void usb_state_init(UsbState* st, u8* data, size_t size, bool reading)
{
	st->data    = data;
	st->size    = size;
	st->pos     = 0;
	st->reading = reading;
	st->error   = false;
}

void USBStateWrite(void* dst)
{
	u8* p = (u8*)dst;
	const u32 size = USB_STATE_SIZE, version = USB_STATE_VERSION;
	UsbState st, slot;
	u32 port, kind;

	memset(p, 0, USB_STATE_SIZE);
	if (!s_qemu_ohci)
		return;

	usb_state_init(&st, p, USB_STATE_OHCI, false);
	USBFreezeOHCI(&st);
	for (port = 0; port < USB_NUM_PORTS && !st.error; port++)
	{
		usb_state_init(&slot, p + USB_STATE_OHCI + port * USB_STATE_SLOT, USB_STATE_SLOT, false);
		kind = USBPortKind(port);
		USB_STATE_DO(&slot, kind);
		if (kind != USB_KIND_NONE)
			USBFreezePort(port, kind, &slot);
		st.error = slot.error;
	}
	if (st.error)
	{
		log_cb(RETRO_LOG_ERROR, "USB: the state does not fit its block\n");
		memset(p, 0, USB_STATE_SIZE);
		return;
	}
	memcpy(p + USB_STATE_SIZE - 16, &size, 4);
	memcpy(p + USB_STATE_SIZE - 12, &version, 4);
	memcpy(p + USB_STATE_SIZE - 8, USB_STATE_MAGIC, 8);
}

size_t USBStateStrip(const void* state, size_t size)
{
	return USBStateFind(state, size) ? size - USB_STATE_SIZE : size;
}

/* A state without the block, or one left mid-transfer, puts the devices
 * back as if plugged in afresh; a port whose device differs from the
 * state's is plugged in afresh too. */
void USBStateRead(const void* state, size_t size)
{
	const u8* p = USBStateFind(state, size);
	UsbState st, slot;
	u32 port, kind;

	if (!s_qemu_ohci)
		return;
	if (!p)
	{
		USBreset();
		return;
	}

	/* Read only: the cast is for the shared reader/writer. */
	usb_state_init(&st, (u8*)p, USB_STATE_OHCI, true);
	USBFreezeOHCI(&st);
	if (st.error || s_qemu_ohci->async_td)
	{
		log_cb(RETRO_LOG_WARN, "USB: state not loaded; devices plugged in afresh\n");
		USBreset();
		return;
	}
	for (port = 0; port < USB_NUM_PORTS; port++)
	{
		usb_state_init(&slot, (u8*)p + USB_STATE_OHCI + port * USB_STATE_SLOT, USB_STATE_SLOT, true);
		kind = USB_KIND_NONE;
		USB_STATE_DO(&slot, kind);
		if (kind != USBPortKind(port))
		{
			/* Plugged in afresh: the port cycles and the device model goes
			 * back to its defaults, as a guest's port reset would do later. */
			if (s_usb_device[port])
				usb_port_reset(&GetOHCIPort(port)->port);
			continue;
		}
		if (kind != USB_KIND_NONE)
		{
			USBFreezePort(port, kind, &slot);
			/* A slot that ran short, or named a configuration or interface
			 * the device does not have, leaves the device half loaded:
			 * plugged in afresh instead. */
			if (slot.error)
			{
				log_cb(RETRO_LOG_WARN, "USB: port %u state not loaded; device plugged in afresh\n", port);
				usb_port_reset(&GetOHCIPort(port)->port);
			}
		}
	}
}

USBDevice* USBGetPortDevice(unsigned port)
{
	return port < USB_NUM_PORTS ? s_usb_device[port] : NULL;
}

u8 USBread8(u32 addr) { return 0; }
u16 USBread16(u32 addr) { return 0; }

u32 USBread32(u32 addr)
{
	if (!s_qemu_ohci)
		return 0;
	return ohci_mem_read(s_qemu_ohci, addr);
}

void USBwrite8(u32 addr, u8 value) {}
void USBwrite16(u32 addr, u16 value) {}

void USBwrite32(u32 addr, u32 value)
{
	if (s_qemu_ohci)
		ohci_mem_write(s_qemu_ohci, addr, value);
}
