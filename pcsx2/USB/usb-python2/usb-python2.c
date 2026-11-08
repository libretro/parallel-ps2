/* The Konami Python 2 I/O board as a USB device. See usb-python2.h. */
#include "usb-python2.h"

#include "USB/libretro-usb/USBinternal.h"
#include "USB/libretro-usb/desc.h"

#include <stdlib.h>
#include <string.h>

static const USBDescStrings python2_desc_strings = {
	"",
};

static const uint8_t python2_dev_desc[] = {
	0x12,         /* bLength */
	0x01,         /* bDescriptorType: device */
	WBVAL(0x101), /* bcdUSB 1.01 */
	0x00,         /* bDeviceClass */
	0x00,         /* bDeviceSubClass */
	0x00,         /* bDeviceProtocol */
	0x08,         /* bMaxPacketSize0 */
	WBVAL(0x0000), /* idVendor */
	WBVAL(0x7305), /* idProduct */
	WBVAL(0x0020), /* bcdDevice */
	0, 0, 0,      /* iManufacturer, iProduct, iSerialNumber */
	0x01          /* bNumConfigurations */
};

/* One interface: the JAMMA report on interrupt IN 3, the command
 * pipe on bulk OUT 2 and its answers on bulk IN 1. */
static const uint8_t python2_config_desc[] = {
	USB_CONFIGURATION_DESC_SIZE,
	USB_CONFIGURATION_DESCRIPTOR_TYPE,
	WBVAL(40),
	0x01, /* bNumInterfaces */
	0x01, /* bConfigurationValue */
	0x00, /* iConfiguration */
	USB_CONFIG_POWERED_MASK,
	USB_CONFIG_POWER_MA(100),

	USB_INTERFACE_DESC_SIZE,
	USB_INTERFACE_DESCRIPTOR_TYPE,
	0, 0, 3, USB_CLASS_RESERVED, 0, 0, 0,

	USB_ENDPOINT_DESC_SIZE, USB_ENDPOINT_DESCRIPTOR_TYPE,
	USB_ENDPOINT_IN(3), USB_ENDPOINT_TYPE_INTERRUPT, WBVAL(16), 3,

	USB_ENDPOINT_DESC_SIZE, USB_ENDPOINT_DESCRIPTOR_TYPE,
	USB_ENDPOINT_IN(1), USB_ENDPOINT_TYPE_BULK, WBVAL(64), 10,

	USB_ENDPOINT_DESC_SIZE, USB_ENDPOINT_DESCRIPTOR_TYPE,
	USB_ENDPOINT_OUT(2), USB_ENDPOINT_TYPE_BULK, WBVAL(64), 10,
};

typedef struct Python2State
{
	USBDevice dev;
	USBDesc desc;
	USBDescDevice desc_dev;
	p2io_t io;
} Python2State;

static p2io_config s_config;
static bool s_have_config;

void usb_python2_set_config(const p2io_config* cfg)
{
	s_config      = *cfg;
	s_have_config = true;
}

bool usb_python2_has_config(void)
{
	return s_have_config;
}

int usb_python2_game(void)
{
	return s_config.game;
}

void usb_python2_clear_config(void)
{
	memset(&s_config, 0, sizeof(s_config));
	s_have_config = false;
}

static void python2_unrealize(USBDevice* dev)
{
	Python2State* s = USB_CONTAINER_OF(dev, Python2State, dev);
	usb_desc_device_free(&s->desc_dev);
	free(s);
}

static void python2_handle_reset(USBDevice* dev)
{
	p2io_reset(&USB_CONTAINER_OF(dev, Python2State, dev)->io);
}

static void python2_handle_control(USBDevice* dev, USBPacket* p,
	int request, int value, int index, int length, uint8_t* data)
{
	if (usb_desc_handle_control(dev, p, request, value, index, length, data) >= 0)
		return;
	p->status = USB_RET_STALL;
}

static void python2_handle_data(USBDevice* dev, USBPacket* p)
{
	Python2State* s = USB_CONTAINER_OF(dev, Python2State, dev);
	uint8_t buf[P2IO_TX_SIZE];
	size_t n, room;

	switch (p->pid)
	{
		case USB_TOKEN_IN:
			room = p->buffer_size - p->actual_length;
			if (p->ep->nr == 3)
			{
				p2io_jamma_read(&s->io, buf);
				n = room < P2IO_JAMMA_REPORT ? room : P2IO_JAMMA_REPORT;
			}
			else if (p->ep->nr == 1)
				/* What does not fit this transfer is the next one's. */
				n = p2io_in(&s->io, buf, room < sizeof(buf) ? room : sizeof(buf));
			else
			{
				p->status = USB_RET_STALL;
				break;
			}
			usb_packet_copy(p, buf, n);
			break;
		case USB_TOKEN_OUT:
			if (p->ep->nr == 2)
			{
				p2io_out(&s->io, (const uint8_t*)p->buffer_ptr + p->actual_length,
					p->buffer_size - p->actual_length);
				p->actual_length = p->buffer_size;
			}
			break;
		default:
			p->status = USB_RET_STALL;
			break;
	}
}

USBDevice* usb_python2_create(void)
{
	Python2State* s = (Python2State*)calloc(1, sizeof(Python2State));
	if (!s)
		return NULL;
	s->desc.full = &s->desc_dev;
	s->desc.str  = python2_desc_strings;

	if (usb_desc_parse_dev(python2_dev_desc, sizeof(python2_dev_desc), &s->desc, &s->desc_dev) < 0)
		goto fail;
	if (usb_desc_parse_config(python2_config_desc, sizeof(python2_config_desc), &s->desc_dev) < 0)
		goto fail;

	s->dev.speed                = USB_SPEED_FULL;
	s->dev.klass.handle_attach  = usb_desc_attach;
	s->dev.klass.handle_reset   = python2_handle_reset;
	s->dev.klass.handle_control = python2_handle_control;
	s->dev.klass.handle_data    = python2_handle_data;
	s->dev.klass.unrealize      = python2_unrealize;
	s->dev.klass.usb_desc       = &s->desc;
	s->dev.klass.product_desc   = "";

	usb_desc_init(&s->dev);
	usb_ep_init(&s->dev);
	p2io_init(&s->io, &s_config);
	return &s->dev;
fail:
	python2_unrealize(&s->dev);
	return NULL;
}

/* ---- controls ------------------------------------------------- */

typedef struct PadMap
{
	unsigned id;  /* RETRO_DEVICE_ID_JOYPAD_* */
	uint32_t bit; /* P2IO_IN_* */
} PadMap;

#define PAD_END { 0xffffffffu, 0 }

/* Every game: coin on Select, start on Start. */
static const PadMap s_common[] = {
	{ RETRO_DEVICE_ID_JOYPAD_SELECT, P2IO_IN_COIN },
	{ RETRO_DEVICE_ID_JOYPAD_START, P2IO_IN_START },
	PAD_END
};

static const PadMap s_map_ddr[] = {
	{ RETRO_DEVICE_ID_JOYPAD_UP, P2IO_DDR_UP },
	{ RETRO_DEVICE_ID_JOYPAD_DOWN, P2IO_DDR_DOWN },
	{ RETRO_DEVICE_ID_JOYPAD_LEFT, P2IO_DDR_LEFT },
	{ RETRO_DEVICE_ID_JOYPAD_RIGHT, P2IO_DDR_RIGHT },
	{ RETRO_DEVICE_ID_JOYPAD_X, P2IO_DDR_UP },
	{ RETRO_DEVICE_ID_JOYPAD_B, P2IO_DDR_DOWN },
	{ RETRO_DEVICE_ID_JOYPAD_Y, P2IO_DDR_LEFT },
	{ RETRO_DEVICE_ID_JOYPAD_A, P2IO_DDR_RIGHT },
	{ RETRO_DEVICE_ID_JOYPAD_L, P2IO_IN_LEFT },
	{ RETRO_DEVICE_ID_JOYPAD_R, P2IO_IN_RIGHT },
	{ RETRO_DEVICE_ID_JOYPAD_L2, P2IO_IN_CARD },
	PAD_END
};

static const PadMap s_map_dm[] = {
	{ RETRO_DEVICE_ID_JOYPAD_L, P2IO_DM_HIHAT },
	{ RETRO_DEVICE_ID_JOYPAD_Y, P2IO_DM_SNARE },
	{ RETRO_DEVICE_ID_JOYPAD_X, P2IO_DM_HIGH_TOM },
	{ RETRO_DEVICE_ID_JOYPAD_A, P2IO_DM_LOW_TOM },
	{ RETRO_DEVICE_ID_JOYPAD_R, P2IO_DM_CYMBAL },
	{ RETRO_DEVICE_ID_JOYPAD_B, P2IO_DM_BASS },
	{ RETRO_DEVICE_ID_JOYPAD_LEFT, P2IO_IN_LEFT },
	{ RETRO_DEVICE_ID_JOYPAD_RIGHT, P2IO_IN_RIGHT },
	{ RETRO_DEVICE_ID_JOYPAD_L2, P2IO_IN_CARD },
	PAD_END
};

static const PadMap s_map_gf[] = {
	{ RETRO_DEVICE_ID_JOYPAD_B, P2IO_GF_PICK },
	{ RETRO_DEVICE_ID_JOYPAD_UP, P2IO_GF_PICK },
	{ RETRO_DEVICE_ID_JOYPAD_DOWN, P2IO_GF_PICK },
	{ RETRO_DEVICE_ID_JOYPAD_X, P2IO_GF_WAIL },
	{ RETRO_DEVICE_ID_JOYPAD_Y, P2IO_GF_RED },
	{ RETRO_DEVICE_ID_JOYPAD_A, P2IO_GF_GREEN },
	{ RETRO_DEVICE_ID_JOYPAD_R, P2IO_GF_BLUE },
	{ RETRO_DEVICE_ID_JOYPAD_L, P2IO_GF_EFFECT1 },
	{ RETRO_DEVICE_ID_JOYPAD_R2, P2IO_GF_EFFECT2 },
	{ RETRO_DEVICE_ID_JOYPAD_L2, P2IO_IN_CARD },
	PAD_END
};

static const PadMap s_map_toysmarch[] = {
	{ RETRO_DEVICE_ID_JOYPAD_LEFT, P2IO_IN_LEFT },
	{ RETRO_DEVICE_ID_JOYPAD_RIGHT, P2IO_IN_RIGHT },
	{ RETRO_DEVICE_ID_JOYPAD_L, P2IO_IN_LEFT },
	{ RETRO_DEVICE_ID_JOYPAD_R, P2IO_IN_RIGHT },
	{ RETRO_DEVICE_ID_JOYPAD_X, P2IO_TM_CYMBAL },
	{ RETRO_DEVICE_ID_JOYPAD_Y, P2IO_TM_DRUM_L },
	{ RETRO_DEVICE_ID_JOYPAD_A, P2IO_TM_DRUM_R },
	PAD_END
};

static const PadMap s_map_thrilldrive[] = {
	{ RETRO_DEVICE_ID_JOYPAD_DOWN, P2IO_TD_GEAR_DOWN },
	{ RETRO_DEVICE_ID_JOYPAD_L, P2IO_TD_GEAR_DOWN },
	{ RETRO_DEVICE_ID_JOYPAD_UP, P2IO_TD_GEAR_UP },
	{ RETRO_DEVICE_ID_JOYPAD_R, P2IO_TD_GEAR_UP },
	{ RETRO_DEVICE_ID_JOYPAD_X, P2IO_TD_SEATBELT },
	PAD_END
};

static const PadMap s_map_dance864[] = {
	{ RETRO_DEVICE_ID_JOYPAD_LEFT, P2IO_D864_LEFT },
	{ RETRO_DEVICE_ID_JOYPAD_DOWN, P2IO_D864_CENTER },
	{ RETRO_DEVICE_ID_JOYPAD_RIGHT, P2IO_D864_RIGHT },
	{ RETRO_DEVICE_ID_JOYPAD_Y, P2IO_D864_LEFT },
	{ RETRO_DEVICE_ID_JOYPAD_B, P2IO_D864_CENTER },
	{ RETRO_DEVICE_ID_JOYPAD_A, P2IO_D864_RIGHT },
	{ RETRO_DEVICE_ID_JOYPAD_L, P2IO_IN_LEFT },
	{ RETRO_DEVICE_ID_JOYPAD_R, P2IO_IN_RIGHT },
	PAD_END
};

static const PadMap* game_map(int game)
{
	switch (game)
	{
		case P2IO_GAME_DDR:          return s_map_ddr;
		case P2IO_GAME_GUITARFREAKS: return s_map_gf;
		case P2IO_GAME_TOYSMARCH:    return s_map_toysmarch;
		case P2IO_GAME_THRILLDRIVE:  return s_map_thrilldrive;
		case P2IO_GAME_DANCE864:     return s_map_dance864;
		default:                     return s_map_dm;
	}
}

/* The card reader keypads: player 1 on the numeric keypad, player 2
 * on the number row. */
static const unsigned s_keys[2][12] = {
	{ RETROK_KP0, RETROK_KP1, RETROK_KP2, RETROK_KP3, RETROK_KP4, RETROK_KP5,
		RETROK_KP6, RETROK_KP7, RETROK_KP8, RETROK_KP9, RETROK_KP_PERIOD, RETROK_KP_ENTER },
	{ RETROK_0, RETROK_1, RETROK_2, RETROK_3, RETROK_4, RETROK_5,
		RETROK_6, RETROK_7, RETROK_8, RETROK_9, RETROK_MINUS, RETROK_EQUALS },
};

static uint32_t read_map(retro_input_state_t input_cb, unsigned port, const PadMap* m)
{
	uint32_t bits = 0;
	for (; m->id != 0xffffffffu; m++)
		if (input_cb(port, RETRO_DEVICE_JOYPAD, 0, m->id))
			bits |= m->bit;
	return bits;
}

/* A trigger, 0..P2IO_ANALOG_MAX, full when the pad has only a digital
 * one. */
static int32_t read_trigger(retro_input_state_t input_cb, unsigned id)
{
	const unsigned long v = (unsigned long)(unsigned)input_cb(0, RETRO_DEVICE_ANALOG,
		RETRO_DEVICE_INDEX_ANALOG_BUTTON, id) & 0xffffu;
	if (v)
		return (int32_t)((v > 32767 ? 32767 : v) * P2IO_ANALOG_MAX / 32767);
	return input_cb(0, RETRO_DEVICE_JOYPAD, 0, id) ? P2IO_ANALOG_MAX : 0;
}

void usb_python2_update(USBDevice* dev, retro_input_state_t input_cb)
{
	Python2State* s = USB_CONTAINER_OF(dev, Python2State, dev);
	const int game  = s->io.cfg.game;
	const PadMap* m = game_map(game);
	p2io_input in;
	unsigned i, k;

	if (!input_cb)
		return;
	memset(&in, 0, sizeof(in));
	for (i = 0; i < 2; i++)
	{
		in.pad[i] = read_map(input_cb, i, s_common) | read_map(input_cb, i, m);
		for (k = 0; k < 12; k++)
			if (input_cb(0, RETRO_DEVICE_KEYBOARD, 0, s_keys[i][k]))
				in.pad[i] |= k < 11 ? P2IO_IN_KEY(k) : P2IO_IN_CARD;
	}
	if (input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3))
		in.pad[0] |= P2IO_IN_TEST;
	if (input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3))
		in.pad[0] |= P2IO_IN_SERVICE;

	/* GuitarFreaks' effect knob also turns with the right stick. */
	if (game == P2IO_GAME_GUITARFREAKS)
		for (i = 0; i < 2; i++)
		{
			const int x = (int16_t)input_cb(i, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X);
			const int y = (int16_t)input_cb(i, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y);
			if (x < -16384)
				in.pad[i] |= P2IO_GF_EFFECT1;
			else if (x > 16384)
				in.pad[i] |= P2IO_GF_EFFECT3;
			else if (y < -16384)
				in.pad[i] |= P2IO_GF_EFFECT2;
		}

	in.wheel = P2IO_WHEEL_CENTER;
	if (game == P2IO_GAME_THRILLDRIVE)
	{
		/* Larger is further left: left of centre spans up to the
		 * maximum, right of it down to zero. */
		long x = (int16_t)input_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X);
		if (input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT))
			x = -32768;
		else if (input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT))
			x = 32767;
		if (x < 0)
			in.wheel = P2IO_WHEEL_CENTER + (int32_t)((-x * (long)(P2IO_ANALOG_MAX - P2IO_WHEEL_CENTER)) / 32768);
		else
			in.wheel = P2IO_WHEEL_CENTER - (int32_t)((x * (long)P2IO_WHEEL_CENTER) / 32767);
		in.accel = read_trigger(input_cb, RETRO_DEVICE_ID_JOYPAD_R2);
		in.brake = read_trigger(input_cb, RETRO_DEVICE_ID_JOYPAD_L2);
	}
	p2io_set_input(&s->io, &in);
}

void usb_python2_freeze(USBDevice* dev, UsbState* st)
{
	Python2State* s = USB_CONTAINER_OF(dev, Python2State, dev);
	/* Static: the state is larger than a frame should hold. Savestates
	 * are taken on one thread. */
	static p2io_state tmp;
	tmp = s->io.st;
	usb_state_bytes(st, &tmp, sizeof(tmp));
	/* A block whose lengths do not fit the buffers is a bad one. */
	if (st->reading && !st->error)
	{
		if (tmp.rx_len > P2IO_RX_SIZE || tmp.tx_len > P2IO_TX_SIZE || tmp.tx_pos > tmp.tx_len
			|| tmp.sci[0].len > P2IO_SCI_SIZE || tmp.sci[1].len > P2IO_SCI_SIZE)
			st->error = true;
		else
			s->io.st = tmp;
	}
}

/* ---- input descriptors --------------------------------------- */

#define D(port, id, desc) { port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_##id, desc }
#define COMMON(port) \
	D(port, SELECT, "Coin"), D(port, START, "Start")
#define SYSTEM D(0, L3, "Test"), D(0, R3, "Service")

static const struct retro_input_descriptor s_desc_ddr[] = {
	SYSTEM, COMMON(0), COMMON(1),
	D(0, UP, "Foot Up"), D(0, DOWN, "Foot Down"), D(0, LEFT, "Foot Left"), D(0, RIGHT, "Foot Right"),
	D(0, X, "Foot Up"), D(0, B, "Foot Down"), D(0, Y, "Foot Left"), D(0, A, "Foot Right"),
	D(0, L, "Select Left"), D(0, R, "Select Right"), D(0, L2, "Card In/Out"),
	D(1, UP, "Foot Up"), D(1, DOWN, "Foot Down"), D(1, LEFT, "Foot Left"), D(1, RIGHT, "Foot Right"),
	D(1, X, "Foot Up"), D(1, B, "Foot Down"), D(1, Y, "Foot Left"), D(1, A, "Foot Right"),
	D(1, L, "Select Left"), D(1, R, "Select Right"), D(1, L2, "Card In/Out"),
	{ 0, 0, 0, 0, NULL }
};

static const struct retro_input_descriptor s_desc_dm[] = {
	SYSTEM, COMMON(0),
	D(0, L, "Hi-Hat"), D(0, Y, "Snare"), D(0, X, "High Tom"), D(0, A, "Low Tom"),
	D(0, R, "Cymbal"), D(0, B, "Bass Drum"), D(0, LEFT, "Select Left"), D(0, RIGHT, "Select Right"),
	D(0, L2, "Card In/Out"),
	{ 0, 0, 0, 0, NULL }
};

static const struct retro_input_descriptor s_desc_gf[] = {
	SYSTEM, COMMON(0), COMMON(1),
	D(0, B, "Pick"), D(0, X, "Wail"), D(0, Y, "Red"), D(0, A, "Green"), D(0, R, "Blue"),
	D(0, L, "Effect 1"), D(0, R2, "Effect 2"), D(0, L2, "Card In/Out"),
	D(1, B, "Pick"), D(1, X, "Wail"), D(1, Y, "Red"), D(1, A, "Green"), D(1, R, "Blue"),
	D(1, L, "Effect 1"), D(1, R2, "Effect 2"), D(1, L2, "Card In/Out"),
	{ 0, 0, 0, 0, NULL }
};

static const struct retro_input_descriptor s_desc_toysmarch[] = {
	SYSTEM, COMMON(0), COMMON(1),
	D(0, LEFT, "Left"), D(0, RIGHT, "Right"), D(0, X, "Cymbal"), D(0, Y, "Drum Left"), D(0, A, "Drum Right"),
	D(1, LEFT, "Left"), D(1, RIGHT, "Right"), D(1, X, "Cymbal"), D(1, Y, "Drum Left"), D(1, A, "Drum Right"),
	{ 0, 0, 0, 0, NULL }
};

static const struct retro_input_descriptor s_desc_thrilldrive[] = {
	SYSTEM, COMMON(0),
	{ 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Steering" },
	D(0, R2, "Accelerator"), D(0, L2, "Brake"), D(0, DOWN, "Gear Down"), D(0, UP, "Gear Up"),
	D(0, X, "Seatbelt"),
	{ 0, 0, 0, 0, NULL }
};

static const struct retro_input_descriptor s_desc_dance864[] = {
	SYSTEM, COMMON(0), COMMON(1),
	D(0, LEFT, "Pad Left"), D(0, DOWN, "Pad Center"), D(0, RIGHT, "Pad Right"),
	D(0, L, "Select Left"), D(0, R, "Select Right"),
	D(1, LEFT, "Pad Left"), D(1, DOWN, "Pad Center"), D(1, RIGHT, "Pad Right"),
	D(1, L, "Select Left"), D(1, R, "Select Right"),
	{ 0, 0, 0, 0, NULL }
};

#undef D
#undef COMMON
#undef SYSTEM

const struct retro_input_descriptor* usb_python2_input_descriptors(int game)
{
	switch (game)
	{
		case P2IO_GAME_DDR:          return s_desc_ddr;
		case P2IO_GAME_GUITARFREAKS: return s_desc_gf;
		case P2IO_GAME_TOYSMARCH:    return s_desc_toysmarch;
		case P2IO_GAME_THRILLDRIVE:  return s_desc_thrilldrive;
		case P2IO_GAME_DANCE864:     return s_desc_dance864;
		default:                     return s_desc_dm;
	}
}
