// The USB block of a savestate (libretro/USB.c): write, find, strip, read.
//
// A keyboard on port 0 and an EyeToy on the free port, through the real
// OHCI controller and device models. A register is written into the EyeToy
// through its control endpoint, the state is written, the device is reset,
// and the state is read back: the register must be back, and the devices
// must be the ones that were there, not replugged. Then the ways a state
// can disagree with the machine, each of which must replug rather than half
// load: no block at all, a port whose device changed, a slot naming a
// configuration or interface the device does not have.
//
// The frontend's camera and microphone are stubs, so the EyeToy opens
// neither V4L2 nor ALSA here.

#include "USB/USB.h"
#include "USB/usb-host.h"
#include "USB/libretro-usb/USBinternal.h"
#include "USB/libretro-usb/qusb.h"
#include "USB/libretro-usb/desc.h"
#include "USB/usb-eyetoy/eyetoy-frontend.h"
#include "USB/usb-python2/usb-python2.h"

#include "libretro.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- what the core provides around USB.cpp ---- */

static void stub_log(enum retro_log_level level, const char* fmt, ...)
{
	va_list ap;
	if (level < RETRO_LOG_WARN)
		return;
	va_start(ap, fmt);
	printf("    [log] ");
	vprintf(fmt, ap);
	va_end(ap);
}
extern "C" {
retro_log_printf_t log_cb = stub_log;
}

static u8* s_iop_ram;
u8* usb_iop_ram(void)
{
	return s_iop_ram;
}

void usbIrq(int) {}

static int16_t RETRO_CALLCONV stub_input(unsigned, unsigned, unsigned, unsigned)
{
	return 0;
}
retro_input_state_t PADGetInputStateCallback(void)
{
	return stub_input;
}

/* A frontend with a camera and a microphone that deliver nothing. */
static bool RETRO_CALLCONV cam_start(void) { return true; }
static void RETRO_CALLCONV cam_stop(void) {}
static retro_microphone_t* RETRO_CALLCONV mic_open(const retro_microphone_params_t*) { return (retro_microphone_t*)1; }
static void RETRO_CALLCONV mic_close(retro_microphone_t*) {}
static bool RETRO_CALLCONV mic_get_params(const retro_microphone_t*, retro_microphone_params_t* p) { p->rate = 48000; return true; }
static bool RETRO_CALLCONV mic_set_state(retro_microphone_t*, bool) { return true; }
static bool RETRO_CALLCONV mic_get_state(const retro_microphone_t*) { return true; }
static int RETRO_CALLCONV mic_read(retro_microphone_t*, int16_t*, size_t) { return 0; }

static bool RETRO_CALLCONV stub_environ(unsigned cmd, void* data)
{
	if (cmd == RETRO_ENVIRONMENT_GET_CAMERA_INTERFACE)
	{
		retro_camera_callback* cb = (retro_camera_callback*)data;
		cb->start = cam_start;
		cb->stop = cam_stop;
		return true;
	}
	if (cmd == RETRO_ENVIRONMENT_GET_MICROPHONE_INTERFACE)
	{
		retro_microphone_interface* mi = (retro_microphone_interface*)data;
		mi->open_mic = mic_open;
		mi->close_mic = mic_close;
		mi->get_params = mic_get_params;
		mi->set_mic_state = mic_set_state;
		mi->get_mic_state = mic_get_state;
		mi->read_mic = mic_read;
		return true;
	}
	return false;
}

/* ---- the harness ---- */

static int s_bad;

static void check(bool ok, const char* what)
{
	printf("  %-60s %s\n", what, ok ? "ok" : "WRONG");
	if (!ok)
		s_bad++;
}

/* One control transfer into a device, the way the OHCI core would make it. */
static int control(USBDevice* dev, int request, int value, int index, u8* data, int length)
{
	USBPacket p;
	memset(&p, 0, sizeof(p));
	p.ep = &dev->ep_ctl;
	p.pid = USB_TOKEN_OUT;
	p.actual_length = 0;
	dev->klass.handle_control(dev, &p, request, value, index, length, data);
	return p.status;
}

static u8 eyetoy_read_reg(USBDevice* dev, int reg)
{
	u8 data[8] = {0};
	control(dev, VendorDeviceRequest | 0x1, 0, reg, data, 1);
	return data[0];
}

/* The Capture Eye (OV511+) reads its registers through request 3. */
static u8 ov511_read_reg(USBDevice* dev, int reg)
{
	u8 data[8] = {0};
	control(dev, VendorDeviceRequest | 0x3, 0, reg, data, 1);
	return data[0];
}

static void eyetoy_write_reg(USBDevice* dev, int reg, u8 value)
{
	u8 data[8] = {value};
	control(dev, VendorDeviceOutRequest | 0x1, 0, reg, data, 1);
}

/* Where the altsetting array sits in a port's slot: the kind, then the
 * fields USBFreezeDevice writes in order. Mirrors libretro/USB.c. */
static size_t slot_altsetting_offset(void)
{
	USBDevice* d = nullptr;
	return 4 + sizeof(d->speed) + sizeof(d->addr) + sizeof(d->state) + sizeof(d->setup_buf) + sizeof(d->data_buf) +
		   sizeof(d->remote_wakeup) + sizeof(d->setup_state) + sizeof(d->setup_len) + sizeof(d->setup_index) +
		   sizeof(d->configuration);
}

#define USB_STATE_OHCI 10240
#define USB_STATE_SLOT 8192

/* One bulk or interrupt transfer on endpoint nr, as the OHCI core would
 * make it; returns the bytes moved. */
static int transfer(USBDevice* dev, int pid, int nr, u8* buf, unsigned size)
{
	USBPacket p;
	memset(&p, 0, sizeof(p));
	p.pid         = pid;
	p.ep          = usb_ep_get(dev, pid, nr);
	p.buffer_ptr  = buf;
	p.buffer_size = size;
	dev->klass.handle_data(dev, &p);
	return p.status == USB_RET_SUCCESS ? p.actual_length : -1;
}

/* A Python 2 game's I/O board takes the first port, and its state rides
 * in the block: a command answered half way through is answered the
 * rest of the way after a load. */
static void python2_checks(void)
{
	static const u8 get_version[] = { 0xaa, 0x02, 0x07, 0x01 };
	u8 first[16], rest[16], again[16];
	p2io_config cfg;
	USBDevice* io;
	size_t size = USBStateSize();
	u8* state = (u8*)malloc(size);
	int n1, n2;

	memset(&cfg, 0, sizeof(cfg));
	cfg.game = P2IO_GAME_DDR;
	usb_python2_set_config(&cfg);
	USBSetPortDevice(0, USB_DEV_KEYBOARD, 0);
	USBSetCamera(USB_CAMERA_NONE);
	check(USBopen(), "USBopen with a Python 2 game");
	io = USBGetPortDevice(0);
	check(io && io->klass.handle_data && USBGetPortDevice(1) == nullptr,
		"the I/O board is on port 0, over the port's keyboard");
	usb_desc_set_config(io, 1);

	/* The version answer is 11 bytes: magic, length, sequence, status,
	 * then D44 1.6.4. Take 4 of them, save, then the rest. */
	transfer(io, USB_TOKEN_OUT, 2, (u8*)get_version, sizeof(get_version));
	n1 = transfer(io, USB_TOKEN_IN, 1, first, 4);
	check(n1 == 4 && first[0] == 0xaa && first[2] == 0x07, "the answer's start: magic, length, sequence");
	USBStateWrite(state);
	n2 = transfer(io, USB_TOKEN_IN, 1, rest, sizeof(rest));
	check(n2 == 7 && rest[0] == 'D' && rest[1] == '4' && rest[2] == '4' && rest[6] == 4,
		"the rest of it, in the next transfer");

	USBStateRead(state, size);
	check(USBGetPortDevice(0) == io, "a matching state keeps the board");
	check(transfer(io, USB_TOKEN_IN, 1, again, sizeof(again)) == n2 && !memcmp(again, rest, (size_t)n2),
		"after a load the rest of the answer comes again");

	USBclose();
	usb_python2_clear_config();
	free(state);
}

int main()
{
	static const int REG = 0x10; /* OV519_R10_H_SIZE: harmless, logged, kept */
	s_iop_ram = (u8*)calloc(1, USB_IOP_RAM_SIZE);

	usb_eyetoy::FrontendLoad(stub_environ);
	USBSetPortDevice(0, USB_DEV_KEYBOARD, 0);
	USBSetCamera(USB_CAMERA_EYETOY);
	check(USBopen(), "USBopen with a keyboard and an EyeToy");
	USBDevice* kbd = USBGetPortDevice(0);
	USBDevice* cam = USBGetPortDevice(1);
	check(kbd && cam, "keyboard on port 0, EyeToy on port 1");

	const size_t size = USBStateSize();
	u8* state = (u8*)malloc(size);
	u8* other = (u8*)malloc(size);
	check(size == USB_STATE_OHCI + 2 * USB_STATE_SLOT + 16, "block is the fixed size");

	/* A register the state must carry: a value that is not its default,
	 * so a reset is told from a restore. Device identity is read through
	 * it too: a replugged device is back at the default, whatever address
	 * its object landed at. */
	const u8 def = eyetoy_read_reg(cam, REG);
	const u8 val = def ^ 0x55;
	eyetoy_write_reg(cam, REG, val);
	check(eyetoy_read_reg(cam, REG) == val, "EyeToy register written through the control endpoint");
	/* And a configuration, as the guest would set. */
	usb_desc_set_config(cam, 1);

	USBStateWrite(state);
	check(!memcmp(state + size - 8, "LRPS2USB", 8), "block ends on its magic");
	check(USBStateStrip(state, size) == 0, "strip finds the block");
	memset(other, 0xA5, size);
	check(USBStateStrip(other, size) == size, "strip leaves a state without the block alone");

	/* The state puts the register back on the same device. */
	cam->klass.handle_reset(cam);
	check(eyetoy_read_reg(cam, REG) == def, "device reset forgets the register");
	USBStateRead(state, size);
	check(USBGetPortDevice(1) == cam && USBGetPortDevice(0) == kbd, "a matching state keeps the devices");
	check(eyetoy_read_reg(cam, REG) == val, "a matching state restores the register");
	check(cam->configuration == 1, "a matching state restores the configuration");

	/* No block: everything replugged, nothing read. */
	eyetoy_write_reg(cam, REG, val);
	USBStateRead(other, size);
	cam = USBGetPortDevice(1);
	kbd = USBGetPortDevice(0);
	check(cam && kbd && eyetoy_read_reg(cam, REG) == def, "a state without the block plugs the devices in afresh");

	/* A slot naming an interface the device does not have: that port is
	 * replugged, the other port still loads. */
	memcpy(other, state, size);
	{
		int bad = 7;
		memcpy(other + USB_STATE_OHCI + 1 * USB_STATE_SLOT + slot_altsetting_offset(), &bad, sizeof(bad));
	}
	eyetoy_write_reg(cam, REG, val ^ 0x0F);
	USBStateRead(other, size);
	check(eyetoy_read_reg(cam, REG) == def, "a slot with an unknown interface replugs rather than half loads");
	check(USBGetPortDevice(0) == kbd, "the other port is untouched by it");

	/* A slot naming a configuration the device does not have. */
	memcpy(other, state, size);
	{
		int bad = 9;
		memcpy(other + USB_STATE_OHCI + 1 * USB_STATE_SLOT + slot_altsetting_offset() - sizeof(int), &bad, sizeof(bad));
	}
	eyetoy_write_reg(cam, REG, val ^ 0x0F);
	USBStateRead(other, size);
	check(eyetoy_read_reg(cam, REG) == def, "a slot with an unknown configuration replugs rather than half loads");

	/* The port holds another camera now: its slot is skipped, not loaded. */
	USBSetCamera(USB_CAMERA_CAPTURE_EYE);
	USBreset();
	USBDevice* eye = USBGetPortDevice(1);
	check(eye != nullptr, "USBreset plugs the new camera in");
	const u8 eye_def = ov511_read_reg(eye, REG);
	USBStateRead(state, size);
	check(USBGetPortDevice(1) == eye && ov511_read_reg(eye, REG) == eye_def,
		"a state for another device on the port replugs rather than loads");

	/* A state written with the camera unplugged has an empty slot; reading
	 * it with a camera plugged in replugs the camera. */
	USBSetCamera(USB_CAMERA_NONE);
	USBreset();
	check(USBGetPortDevice(1) == nullptr, "no camera, port 1 empty");
	USBStateWrite(other);
	USBSetCamera(USB_CAMERA_EYETOY);
	USBreset();
	cam = USBGetPortDevice(1);
	eyetoy_write_reg(cam, REG, val);
	USBStateRead(other, size);
	check(USBGetPortDevice(1) == cam && eyetoy_read_reg(cam, REG) == def, "an empty slot replugs the device, so it starts afresh");

	USBclose();

	/* Every device model's descriptors go through the parser: the mouse's
	 * too, configured as a guest would. */
	USBSetPortDevice(0, USB_DEV_MOUSE, 0);
	USBSetCamera(USB_CAMERA_CAPTURE_EYE);
	check(USBopen(), "USBopen with a mouse and a Capture Eye");
	{
		USBDevice* mouse = USBGetPortDevice(0);
		check(mouse && usb_desc_set_config(mouse, 1) == 0 && mouse->config && mouse->ninterfaces == 1
				&& mouse->ifaces[0] && mouse->ifaces[0]->neps == 1,
			"the mouse's configuration: one interface, one endpoint");
	}
	USBclose();
	usb_eyetoy::FrontendUnload();
	python2_checks();
	free(state);
	free(other);
	free(s_iop_ram);
	printf(s_bad ? "FAIL\n" : "PASS\n");
	return s_bad ? 1 : 0;
}
