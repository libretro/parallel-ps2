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

#include <algorithm>
#include <array>
#include <cmath>
#include <libretro.h>

#include <string.h>

#include "PAD.h"

#include "HostFS.h"
#include "../../common/Pcsx2Defs.h"

#include "../Frontend/InputManager.h"
#include "../Host.h"
#include "../Sio.h"
#include "../USB/USB.h"

#define MODE_DIGITAL	0x41
#define MODE_ANALOG	0x73
#define MODE_DS2_NATIVE 0x79

#if 1
#define IsDualshock2() true
#else
#define IsDualShock2() (config.padConfigs[query.port][query.slot].type == Dualshock2Pad || (config.padConfigs[query.port][query.slot].type == GuitarPad && config.GH2))
#endif

/* Total number of pad ports, across both multitaps. */
#define NUM_CONTROLLER_PORTS 8

#define DEFAULT_MOTOR_SCALE 1.0f
#define Q8_ONE  256u
#define Q16_ONE 65536u

#define TEST_BIT(value, bit) ((value) & (1 << (bit)))

enum PadCommands
{
	CMD_SET_VREF_PARAM        = 0x40,
	CMD_QUERY_DS2_ANALOG_MODE = 0x41,
	CMD_READ_DATA_AND_VIBRATE = 0x42,
	CMD_CONFIG_MODE           = 0x43,
	CMD_SET_MODE_AND_LOCK     = 0x44,
	CMD_QUERY_MODEL_AND_MODE  = 0x45,
	CMD_QUERY_ACT             = 0x46, /* ?? */
	CMD_QUERY_COMB            = 0x47, /* ?? */
	CMD_QUERY_MODE            = 0x4C, /* QUERY_MODE ?? */
	CMD_VIBRATION_TOGGLE      = 0x4D,
	CMD_SET_DS2_NATIVE_MODE   = 0x4F  /* SET_DS2_NATIVE_MODE */
};

enum gamePadValues
{
	PAD_UP = 0,   //  0  - Directional pad ↑
	PAD_RIGHT,    //  1  - Directional pad →
	PAD_DOWN,     //  2  - Directional pad ↓
	PAD_LEFT,     //  3  - Directional pad ←
	PAD_TRIANGLE, //  4  - Triangle button ▲
	PAD_CIRCLE,   //  5  - Circle button ●
	PAD_CROSS,    //  6  - Cross button ✖
	PAD_SQUARE,   //  7  - Square button ■
	PAD_SELECT,   //  8  - Select button
	PAD_START,    //  9  - Start button
	PAD_L1,       // 10  - L1 button
	PAD_L2,       // 11  - L2 button
	PAD_R1,       // 12  - R1 button
	PAD_R2,       // 13  - R2 button
	PAD_L3,       // 14  - Left joystick button (L3)
	PAD_R3,       // 15  - Right joystick button (R3)
	PAD_ANALOG,   // 16  - Analog mode toggle
	PAD_PRESSURE, // 17  - Pressure modifier
	PAD_L_UP,     // 18  - Left joystick (Up) ↑
	PAD_L_RIGHT,  // 19  - Left joystick (Right) →
	PAD_L_DOWN,   // 20  - Left joystick (Down) ↓
	PAD_L_LEFT,   // 21  - Left joystick (Left) ←
	PAD_R_UP,     // 22  - Right joystick (Up) ↑
	PAD_R_RIGHT,  // 23  - Right joystick (Right) →
	PAD_R_DOWN,   // 24  - Right joystick (Down) ↓
	PAD_R_LEFT,   // 25  - Right joystick (Left) ←
	MAX_KEYS
};

// Full state to manage save state
struct PadFullFreezeData
{
	char format[8];
	// active slot for port
	u8 slot[2];
	PadFreezeData padData[2][4];
	QueryInfo query;
};

struct KeyStatus
{
	ControllerType m_type[NUM_CONTROLLER_PORTS] = {};
	u16 m_vibration_scale_q8[NUM_CONTROLLER_PORTS][2]; /* 8.8 fixed */
};

/* Combined "USB Keyboard + Mouse" port device: one menu selection that
 * attaches a HID keyboard to USB port 0 and a HID mouse to USB port 1,
 * matching the two-device topology a real PS2 keyboard/mouse presented. */
#define RETRO_DEVICE_KEYBOARD_AND_MOUSE RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_KEYBOARD, 0)

static const InputBindingInfo s_dualshock2_binds[] = {
	{"Up", "D-Pad Up", InputBindingInfo::Type::Button, PAD_UP, GenericInputBinding::DPadUp},
	{"Right", "D-Pad Right", InputBindingInfo::Type::Button, PAD_RIGHT, GenericInputBinding::DPadRight},
	{"Down", "D-Pad Down", InputBindingInfo::Type::Button, PAD_DOWN, GenericInputBinding::DPadDown},
	{"Left", "D-Pad Left", InputBindingInfo::Type::Button, PAD_LEFT, GenericInputBinding::DPadLeft},
	{"Triangle", "Triangle", InputBindingInfo::Type::Button, PAD_TRIANGLE, GenericInputBinding::Triangle},
	{"Circle", "Circle", InputBindingInfo::Type::Button, PAD_CIRCLE, GenericInputBinding::Circle},
	{"Cross", "Cross", InputBindingInfo::Type::Button, PAD_CROSS, GenericInputBinding::Cross},
	{"Square", "Square", InputBindingInfo::Type::Button, PAD_SQUARE, GenericInputBinding::Square},
	{"Select", "Select", InputBindingInfo::Type::Button, PAD_SELECT, GenericInputBinding::Select},
	{"Start", "Start", InputBindingInfo::Type::Button, PAD_START, GenericInputBinding::Start},
	{"L1", "L1 (Left Bumper)", InputBindingInfo::Type::Button, PAD_L1, GenericInputBinding::L1},
	{"L2", "L2 (Left Trigger)", InputBindingInfo::Type::HalfAxis, PAD_L2, GenericInputBinding::L2},
	{"R1", "R1 (Right Bumper)", InputBindingInfo::Type::Button, PAD_R1, GenericInputBinding::R1},
	{"R2", "R2 (Right Trigger)", InputBindingInfo::Type::HalfAxis, PAD_R2, GenericInputBinding::R2},
	{"L3", "L3 (Left Stick Button)", InputBindingInfo::Type::Button, PAD_L3, GenericInputBinding::L3},
	{"R3", "R3 (Right Stick Button)", InputBindingInfo::Type::Button, PAD_R3, GenericInputBinding::R3},
	{"Analog", "Analog Toggle", InputBindingInfo::Type::Button, PAD_ANALOG, GenericInputBinding::System},
	{"Pressure", "Apply Pressure", InputBindingInfo::Type::Button, PAD_PRESSURE, GenericInputBinding::Unknown},
	{"LUp", "Left Stick Up", InputBindingInfo::Type::HalfAxis, PAD_L_UP, GenericInputBinding::LeftStickUp},
	{"LRight", "Left Stick Right", InputBindingInfo::Type::HalfAxis, PAD_L_RIGHT, GenericInputBinding::LeftStickRight},
	{"LDown", "Left Stick Down", InputBindingInfo::Type::HalfAxis, PAD_L_DOWN, GenericInputBinding::LeftStickDown},
	{"LLeft", "Left Stick Left", InputBindingInfo::Type::HalfAxis, PAD_L_LEFT, GenericInputBinding::LeftStickLeft},
	{"RUp", "Right Stick Up", InputBindingInfo::Type::HalfAxis, PAD_R_UP, GenericInputBinding::RightStickUp},
	{"RRight", "Right Stick Right", InputBindingInfo::Type::HalfAxis, PAD_R_RIGHT, GenericInputBinding::RightStickRight},
	{"RDown", "Right Stick Down", InputBindingInfo::Type::HalfAxis, PAD_R_DOWN, GenericInputBinding::RightStickDown},
	{"RLeft", "Right Stick Left", InputBindingInfo::Type::HalfAxis, PAD_R_LEFT, GenericInputBinding::RightStickLeft},
	{"LargeMotor", "Large (Low Frequency) Motor", InputBindingInfo::Type::Motor, 0, GenericInputBinding::LargeMotor},
	{"SmallMotor", "Small (High Frequency) Motor", InputBindingInfo::Type::Motor, 0, GenericInputBinding::SmallMotor},
};

static const PAD::ControllerInfo s_controller_info[] = {
	{NotConnected, "None", nullptr, 0, NoVibration},
	{DualShock2, "DualShock2", s_dualshock2_binds, C89_ARRAY_SIZE(s_dualshock2_binds), LargeSmallMotors},
};

static KeyStatus g_key_status;

// Typical packet response on the bus
static const u8 ConfigExit[7]    = {0x5A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const u8 noclue[7]        = {0x5A, 0x00, 0x00, 0x02, 0x00, 0x00, 0x5A};
static const u8 setMode[7]       = {0x5A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const u8 queryModelDS2[7] = {0x5A, 0x03, 0x02, 0x00, 0x02, 0x01, 0x00};
static const u8 queryModelDS1[7] = {0x5A, 0x01, 0x02, 0x00, 0x02, 0x01, 0x00};
static const u8 queryComb[7]     = {0x5A, 0x00, 0x00, 0x02, 0x00, 0x01, 0x00};
static const u8 queryMode[7]     = {0x5A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const u8 setNativeMode[7] = {0x5A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5A};

static u8 queryMaskMode[7]       = {0x5A, 0xFF, 0xFF, 0x03, 0x00, 0x00, 0x5A};

static const u8 queryAct[2][7]   = {
	{0x5A, 0x00, 0x00, 0x01, 0x02, 0x00, 0x0A},
	{0x5A, 0x00, 0x00, 0x01, 0x01, 0x01, 0x14}};

static QueryInfo query;
static Pad pads[2][4];
static int slots[2] = {0, 0};

// Set true on reset; cleared after the pad's initial mode has been forced to
// analog (when the force_analog option is on). Ensures we set the *starting*
// mode only, without overriding later game-driven mode changes.
static bool force_analog_pending[2][4];

extern retro_environment_t environ_cb;
static retro_input_poll_t poll_cb;
static retro_input_state_t input_cb;
struct retro_rumble_interface rumble;

/* What the pads, and the USB keyboard and mouse, report. Input::Update,
 * on the frontend's thread, fills the staged copy; pad_input_latch makes
 * it the live one, which PADpoll and the USB HID devices read on the EE
 * thread. The latch runs only while the EE is held - at the frame gate,
 * as retro_run lets it through, and before retro_run resumes it - so a
 * frame reads the input of the retro_run that let it start, which is the
 * retro_run that presents it, and the EE never sees a copy being
 * written. Mouse motion adds up from latch to latch and is
 * taken once, by the first read of it. */
struct pad_input
{
	u32 buttons;
	int lx, ly, rx, ry;
	int mouse_x, mouse_y;
	u8  analog[12];
	u8  keys[RETROK_LAST / 8 + 1];
	u8  mouse_buttons;
};
/* Indexed by SIO pad (sioConvertPortAndSlotToPad); see pad_for_port. */
static struct pad_input pad_staged[NUM_CONTROLLER_PORTS];
static struct pad_input pad_live[NUM_CONTROLLER_PORTS];

/* The pcsx2_multitap option as it was when the content loaded: bit 0 a
 * multitap in console port 1, bit 1 one in console port 2. It changes
 * only on the next load, as the option says. */
static unsigned pad_multitap;

static void pad_read_multitap_option(void)
{
	struct retro_variable var = { "pcsx2_multitap", NULL };
	const char* tap = environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value ? var.value : "disabled";
	pad_multitap = 0;
	if (!strcmp(tap, "port 1") || !strcmp(tap, "ports 1 and 2"))
		pad_multitap |= 1;
	if (!strcmp(tap, "port 2") || !strcmp(tap, "ports 1 and 2"))
		pad_multitap |= 2;
}

/* The SIO pad that a frontend port drives: the slots of console port 1
 * first, then those of console port 2. A console port without a multitap
 * has one slot. NUM_CONTROLLER_PORTS when the frontend port has no pad. */
static u32 pad_for_port(unsigned port)
{
	const unsigned slots0 = (pad_multitap & 1) ? 4 : 1;
	const unsigned slots1 = (pad_multitap & 2) ? 4 : 1;
	if (port < slots0)
		return sioConvertPortAndSlotToPad(0, port);
	if (port < slots0 + slots1)
		return sioConvertPortAndSlotToPad(1, port - slots0);
	return NUM_CONTROLLER_PORTS;
}

extern "C" void pad_input_latch(void)
{
	unsigned port;
	for (port = 0; port < NUM_CONTROLLER_PORTS; port++)
	{
		const int mx = pad_live[port].mouse_x + pad_staged[port].mouse_x;
		const int my = pad_live[port].mouse_y + pad_staged[port].mouse_y;
		pad_live[port]           = pad_staged[port];
		pad_live[port].mouse_x   = mx;
		pad_live[port].mouse_y   = my;
		pad_staged[port].mouse_x = 0;
		pad_staged[port].mouse_y = 0;
	}
}

/* The input_state the USB HID devices read through: the latched copy. */
static int16_t pad_latched_input(unsigned port, unsigned device, unsigned index, unsigned id)
{
	struct pad_input* in;
	int v;
	const u32 pad = pad_for_port(port);
	(void)index;
	if (pad >= NUM_CONTROLLER_PORTS)
		return 0;
	in = &pad_live[pad];
	if (device == RETRO_DEVICE_KEYBOARD)
		return id < RETROK_LAST && ((in->keys[id >> 3] >> (id & 7)) & 1);
	if (device != RETRO_DEVICE_MOUSE)
		return 0;
	switch (id)
	{
		case RETRO_DEVICE_ID_MOUSE_X:
			v          = in->mouse_x;
			in->mouse_x = 0;
			return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
		case RETRO_DEVICE_ID_MOUSE_Y:
			v          = in->mouse_y;
			in->mouse_y = 0;
			return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
		case RETRO_DEVICE_ID_MOUSE_LEFT:   return in->mouse_buttons & 1;
		case RETRO_DEVICE_ID_MOUSE_RIGHT:  return (in->mouse_buttons >> 1) & 1;
		case RETRO_DEVICE_ID_MOUSE_MIDDLE: return (in->mouse_buttons >> 2) & 1;
		default:                           return 0;
	}
}

static int pad_type[NUM_CONTROLLER_PORTS] = { -1, -1, -1, -1, -1, -1, -1, -1 };

PadSettings pad_settings[2];

static int keymap[] =
{
	RETRO_DEVICE_ID_JOYPAD_L2,     // PAD_L2
	RETRO_DEVICE_ID_JOYPAD_R2,     // PAD_R2
	RETRO_DEVICE_ID_JOYPAD_L,      // PAD_L1
	RETRO_DEVICE_ID_JOYPAD_R,      // PAD_R1
	RETRO_DEVICE_ID_JOYPAD_X,      // PAD_TRIANGLE
	RETRO_DEVICE_ID_JOYPAD_A,      // PAD_CIRCLE
	RETRO_DEVICE_ID_JOYPAD_B,      // PAD_CROSS
	RETRO_DEVICE_ID_JOYPAD_Y,      // PAD_SQUARE
	RETRO_DEVICE_ID_JOYPAD_SELECT, // PAD_SELECT
	RETRO_DEVICE_ID_JOYPAD_L3,     // PAD_L3
	RETRO_DEVICE_ID_JOYPAD_R3,     // PAD_R3
	RETRO_DEVICE_ID_JOYPAD_START,  // PAD_START
	RETRO_DEVICE_ID_JOYPAD_UP,     // PAD_UP
	RETRO_DEVICE_ID_JOYPAD_RIGHT,  // PAD_RIGHT
	RETRO_DEVICE_ID_JOYPAD_DOWN,   // PAD_DOWN
	RETRO_DEVICE_ID_JOYPAD_LEFT,   // PAD_LEFT
};

enum AnalogButtons
{
	ANALOG_BTN_L2,
	ANALOG_BTN_R2,
	ANALOG_BTN_L1,
	ANALOG_BTN_R1,
	ANALOG_BTN_TRIANGLE, // X
	ANALOG_BTN_CIRCLE,   // A
	ANALOG_BTN_CROSS,    // B
	ANALOG_BTN_SQUARE,   // Y
	ANALOG_BTN_UP,
	ANALOG_BTN_RIGHT,
	ANALOG_BTN_DOWN,
	ANALOG_BTN_LEFT,
};

/* Exact floor square root of a u64; plain shift-and-subtract, identical
 * on every host, no libm. */
static u32 isqrt64(u64 v)
{
	u64 r = 0, b = 1ULL << 62;
	while (b > v)
		b >>= 2;
	while (b)
	{
		if (v >= r + b)
		{
			v -= r + b;
			r  = (r >> 1) + b;
		}
		else
			r >>= 1;
		b >>= 2;
	}
	return (u32)r;
}

static void process_analog(int &axis_x, int &axis_y, u32 sensitivity_q16, u16 deadzone)
{
	/* All-integer path: this output is emulated machine state (the pad
	 * bytes games read), so it must not depend on the host's libm, FPU
	 * mode, or -ffast-math.  Sensitivity is 16.16 fixed point, snapped
	 * from the integer-percent core option, so the default (100%) is
	 * exactly Q16_ONE.  Verified against the old float path across a
	 * full axis sweep: identical at defaults, within 3 LSB elsewhere -
	 * and correct at full diagonal deflection, where the old path
	 * overflowed x*x+y*y in int, took sqrt of garbage, and zeroed both
	 * axes. */
	if (deadzone == 0 && sensitivity_q16 == Q16_ONE)
	{
		/* The clamp is not optional: the caller negates these axes for
		 * inversion (axis_invert_* == -1), and a raw -32768 would
		 * otherwise become +32768 and wrap the 8-bit pad axis. */
		axis_x = pcsx2_clamp_i(axis_x, -32767, 32767);
		axis_y = pcsx2_clamp_i(axis_y, -32767, 32767);
		return;
	}

	s64 x = axis_x;
	s64 y = axis_y;

	if (deadzone > 0)
	{
		u32 magnitude = isqrt64((u64)(x * x) + (u64)(y * y));
		magnitude     = pcsx2_min_u(magnitude, 32767u);

		if (magnitude > deadzone)
		{
			// If we're past the deadzone, scale our values so we can still
			// use slow movements when the stick is not fully pushed
			const u32 scaled_q16 = (u32)(((u64)(magnitude - deadzone) << 16) / (32767u - deadzone));
			x = (x * (s64)scaled_q16) >> 16;
			y = (y * (s64)scaled_q16) >> 16;
		}
		else
		{
			axis_x = 0;
			axis_y = 0;
			return;
		}
	}

	// Apply sensitivity
	x      = (x * (s64)sensitivity_q16) >> 16;
	y      = (y * (s64)sensitivity_q16) >> 16;
	axis_x = pcsx2_clamp_i((int)x, -32767, 32767);
	axis_y = pcsx2_clamp_i((int)y, -32767, 32767);
}

static u8 process_button(u16 deadzone, u32 port, int id, u32 mask)
{
	u16 value = input_cb(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_BUTTON, id);

	// If value is 0, either it's really not pressed or
	// the button is not analog, check if the bit is set
	// and return either 0 or a full press
	if (value == 0)
	{
		if (mask & (1 << id))
			return 0xFF;
		return 0;
	}

	// Apply deadzone
	if (deadzone > 0)
	{
		if (value <= deadzone)
			return 0;
		// Scale the range
		value = (value - deadzone) * 32767 / (32767 - deadzone);
	}

	// 0..32767 -> 0..255
	return value >> 7;
}

namespace Input
{
	void Init()
	{
#define JOY_DESC(port) \
		{ port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" }, \
		{ port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" }, \
		{ port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" }, \
		{ port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" }, \
		{ port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "Cross" }, \
		{ port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "Circle" }, \
		{ port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Triangle" }, \
		{ port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "Square" }, \
		{ port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "L1" }, \
		{ port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,    "L2" }, \
		{ port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3,    "L3" }, \
		{ port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "R1" }, \
		{ port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,    "R2" }, \
		{ port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3,    "R3" }, \
		{ port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,   "Select" }, \
		{ port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,    "Start" }, \
		{ port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Left Analog X" }, \
		{ port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Left Analog Y" }, \
		{ port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Right Analog X" }, \
		{ port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Right Analog Y" }

		/* Frontend ports 0 and 1 are the console ports; each multitap
		 * the option plugs in adds three more (pcsx2_multitap). Only the
		 * ports that have a pad are offered, so the frontend shows no
		 * port that is never read. */
		struct retro_input_descriptor desc[] = {
			JOY_DESC(0), JOY_DESC(1), JOY_DESC(2), JOY_DESC(3),
			JOY_DESC(4), JOY_DESC(5), JOY_DESC(6), JOY_DESC(7),
			{},
		};

		static const struct retro_controller_description port_devices[] = {
			{ "DualShock 2", RETRO_DEVICE_JOYPAD },
			{ "USB Keyboard", RETRO_DEVICE_KEYBOARD },
			{ "USB Mouse", RETRO_DEVICE_MOUSE },
			{ "USB Keyboard + Mouse", RETRO_DEVICE_KEYBOARD_AND_MOUSE },
			{},
		};

		/* The PS2 has two USB ports; a multitap slot takes only a pad. */
		static const struct retro_controller_description tap_devices[] = {
			{ "DualShock 2", RETRO_DEVICE_JOYPAD },
			{},
		};

		static const struct retro_controller_info all_ports[] = {
			{ port_devices, sizeof(port_devices) / sizeof(*port_devices) },
			{ port_devices, sizeof(port_devices) / sizeof(*port_devices) },
			{ tap_devices, 1 }, { tap_devices, 1 }, { tap_devices, 1 },
			{ tap_devices, 1 }, { tap_devices, 1 }, { tap_devices, 1 },
			{},
		};
		/* Static, as the table was: the frontend may keep the pointer. */
		static struct retro_controller_info ports[NUM_CONTROLLER_PORTS + 1];
		unsigned nports;
		const unsigned per_port = 20; /* entries JOY_DESC adds */

		pad_read_multitap_option();
		nports = 2 + ((pad_multitap & 1) ? 3 : 0) + ((pad_multitap & 2) ? 3 : 0);
		memcpy(ports, all_ports, nports * sizeof(ports[0]));
		memset(&ports[nports], 0, sizeof(ports[0]));
		memset(&desc[nports * per_port], 0, sizeof(desc[0]));

		environ_cb(RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE, &rumble);
		environ_cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void*)ports);
		environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc);

		Shutdown();
	}

	/* All released, staged and live: the EE is not running. */
	void Shutdown()
	{
		memset(pad_staged, 0, sizeof(pad_staged));
		for (unsigned pad = 0; pad < NUM_CONTROLLER_PORTS; pad++)
			pad_staged[pad].buttons = 0xFFFFFFFF;
		pad_input_latch();
	}

	void Update()
	{
		poll_cb();

		for (unsigned port = 0; port < NUM_CONTROLLER_PORTS; port++)
		{
			const u32 pad = pad_for_port(port);
			if (pad >= NUM_CONTROLLER_PORTS)
				continue;
			u32 mask            = input_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK);
			u32 new_button_mask = 0xFFFF0000;
			u32 btn_index       = 0;
			const std::tuple<u32, u32> con = sioConvertPadToPortAndSlot(pad);
			const u32 con_port  = std::get<0>(con);
			const u32 con_slot  = std::get<1>(con);
			const auto &conf    = pad_settings[con_port];

			for (int i = 0; i < 16; ++i)
			{
				// Select, Start, L3 and R3 aren't pressure sensitive
				if (keymap[i] == RETRO_DEVICE_ID_JOYPAD_SELECT || keymap[i] == RETRO_DEVICE_ID_JOYPAD_START ||
						keymap[i] == RETRO_DEVICE_ID_JOYPAD_L3 || keymap[i] == RETRO_DEVICE_ID_JOYPAD_R3)
					new_button_mask |= !(mask & (1 << keymap[i])) << i;
				else
				{
					// Apply pressure level if needed/possible and deadzone
					pad_staged[pad].analog[btn_index] = process_button(conf.button_deadzone, port, keymap[i], mask);
					new_button_mask |= (pad_staged[pad].analog[btn_index] == 0) << i;
					++btn_index;
				}
			}

			int new_lx = input_cb(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X);
			int new_ly = input_cb(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y);
			int new_rx = input_cb(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X);
			int new_ry = input_cb(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y);

			// Apply axis deadzone and sensitivity
			process_analog(new_lx, new_ly, conf.axis_scale_q16, conf.axis_deadzone);
			process_analog(new_rx, new_ry, conf.axis_scale_q16, conf.axis_deadzone);

			// Apply axis inversion (axis_invert_* being either 1 or -1)
			new_lx *= conf.axis_invert_lx;
			new_ly *= conf.axis_invert_ly;
			new_rx *= conf.axis_invert_rx;
			new_ry *= conf.axis_invert_ry;

			if (pad_type[port] == RETRO_DEVICE_KEYBOARD
			 || pad_type[port] == RETRO_DEVICE_KEYBOARD_AND_MOUSE)
			{
				unsigned k;
				memset(pad_staged[pad].keys, 0, sizeof(pad_staged[pad].keys));
				for (k = 0; k < RETROK_LAST; k++)
					if (input_cb(port, RETRO_DEVICE_KEYBOARD, 0, k))
						pad_staged[pad].keys[k >> 3] |= (u8)(1u << (k & 7));
			}
			if (pad_type[port] == RETRO_DEVICE_MOUSE
			 || pad_type[port] == RETRO_DEVICE_KEYBOARD_AND_MOUSE)
			{
				pad_staged[pad].mouse_x += input_cb(port, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_X);
				pad_staged[pad].mouse_y += input_cb(port, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_Y);
				pad_staged[pad].mouse_buttons = (u8)(
					  (input_cb(port, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_LEFT)   ? 1 : 0)
					| (input_cb(port, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_RIGHT)  ? 2 : 0)
					| (input_cb(port, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_MIDDLE) ? 4 : 0));
			}

			pad_staged[pad].buttons = new_button_mask;
			pad_staged[pad].lx      = new_lx;
			pad_staged[pad].ly      = new_ly;
			pad_staged[pad].rx      = new_rx;
			pad_staged[pad].ry      = new_ry;

			if (conf.rumble_scale_q8 > 0)
				pads[con_port][con_slot].rumble(conf.rumble_scale_q8, port);
		}
	}

} // namespace Input

void retro_set_input_poll(retro_input_poll_t cb)
{
	poll_cb = cb;
}

void retro_set_input_state(retro_input_state_t cb)
{
	input_cb = cb;
}

retro_input_state_t PADGetInputStateCallback(void)
{
	return pad_latched_input;
}

/* The per-port controller type, as the frontend set it through
 * retro_set_controller_port_device, indexed by frontend port; port 0
 * defaults to a DualShock2 and the others to nothing, as the Pad
 * section's defaults were. The motor scales are the defaults. */
static const char* s_pad_type_name[NUM_CONTROLLER_PORTS];

void retro_set_controller_port_device(unsigned port, unsigned device)
{
	if (port >= NUM_CONTROLLER_PORTS)
		return;
	if (port >= 2 && device != RETRO_DEVICE_JOYPAD)
		device = RETRO_DEVICE_NONE;
	if (pad_type[port] != (int)device)
	{
		pad_type[port] = device;

		switch (device)
		{
			case RETRO_DEVICE_JOYPAD:
				s_pad_type_name[port] = "DualShock2";
				if (port < 2)
					USBSetPortDevice(port, USB_DEV_NONE, port);
				break;
			case RETRO_DEVICE_KEYBOARD:
				/* USB HID keyboard coexists with the DualShock on this slot,
				 * as on real hardware - the pad stays available for menu/pause
				 * navigation while the keyboard drives gameplay. */
				s_pad_type_name[port] = "DualShock2";
				USBSetPortDevice(port, USB_DEV_KEYBOARD, port);
				break;
			case RETRO_DEVICE_MOUSE:
				/* USB HID mouse coexists with the DualShock on this slot. */
				s_pad_type_name[port] = "DualShock2";
				USBSetPortDevice(port, USB_DEV_MOUSE, port);
				break;
			case RETRO_DEVICE_KEYBOARD_AND_MOUSE:
				/* Keyboard + mouse together claim both USB ports (kbd on 0,
				 * mouse on 1) regardless of which controller port this was
				 * selected from - the PS2 has only two USB ports. Both read
				 * frontend input from this controller port. The DualShock on
				 * this slot is kept for menu/pause navigation. */
				s_pad_type_name[port] = "DualShock2";
				USBSetPortDevice(0, USB_DEV_KEYBOARD, port);
				USBSetPortDevice(1, USB_DEV_MOUSE, port);
				break;
			default:
				s_pad_type_name[port] = "None";
				if (port < 2)
					USBSetPortDevice(port, USB_DEV_NONE, port);
				break;
		}

		PAD::LoadConfig();
	}
}

void do_rumble(u8 intensity, unsigned type, unsigned port)
{
	if (!rumble.set_rumble_state)
		return;

	// 0..255 -> 0..65535
	u16 scaled = (intensity << 8) | intensity;

	if (type == 0)
		rumble.set_rumble_state(port, RETRO_RUMBLE_STRONG, scaled);
	else
		rumble.set_rumble_state(port, RETRO_RUMBLE_WEAK, scaled);
}

void stop_rumble()
{
	if (!rumble.set_rumble_state)
		return;

	for (int port = 0; port < NUM_CONTROLLER_PORTS; ++port)
	{
		rumble.set_rumble_state(port, RETRO_RUMBLE_STRONG, 0);
		rumble.set_rumble_state(port, RETRO_RUMBLE_WEAK, 0);
	}
}

//////////////////////////////////////////////////////////////////////
// Pad implementation
//////////////////////////////////////////////////////////////////////

void Pad::reset()
{
	memset(this, 0, sizeof(PadFreezeData));

	mode           = MODE_DIGITAL;
	umask[0]       = 0xFF;
	umask[1]       = 0xFF;
	umask[2]       = 0x03;
	// Sets up vibrate variable.
	nextVibrate[0] = 0;
	nextVibrate[1] = 0;
	memset(vibrate, 0xFF, sizeof(vibrate));
	vibrate[0]     = 0x5A;
}

void Pad::rumble(u32 rumble_scale_q8, unsigned port)
{
	if (nextVibrate[0] == currentVibrate[0] && nextVibrate[1] == currentVibrate[1])
		return;

	for (int i = 0; i < 2; ++i)
	{
		currentVibrate[i] = nextVibrate[i];
		do_rumble((u8)pcsx2_min_u(((u32)currentVibrate[i] * rumble_scale_q8) >> 8, 255u), i, port);
	}
}

void Pad::stop_vibrate_all()
{
	for (int port = 0; port < 2; port++)
	{
		for (int slot = 0; slot < 4; slot++)
		{
			pads[port][slot].nextVibrate[0] = 0;
			pads[port][slot].nextVibrate[1] = 0;
			memset(pads[port][slot].vibrate, 0xFF, sizeof(pads[port][slot].vibrate));
			pads[port][slot].vibrate[0] = 0x5A;
		}
	}

	stop_rumble();
}

//////////////////////////////////////////////////////////////////////
// Pad implementation
//////////////////////////////////////////////////////////////////////

s32 PADinit(void)
{
	for (int port = 0; port < 2; port++)
		for (int slot = 0; slot < 4; slot++)
		{
			pads[port][slot].reset();
			force_analog_pending[port][slot] = true;
		}

	query.port           = 0;
	query.slot           = 0;
	query.lastByte       = 1;
	query.currentCommand = 0;
	query.numBytes       = 0;
	query.queryDone      = 1;
	memset(query.response, 0xF3, sizeof(query.response));

	for (int port = 0; port < 2; port++)
		slots[port] = 0;

	return 0;
}

void PADshutdown(void) { }
s32 PADopen(void)      { return 0; }
void PADclose(void)    { }

s32 PADsetSlot(u8 port, u8 slot)
{
	port--;
	slot--;
	if (port > 1 || slot > 3)
		return 0;
	// Even if no pad there, record the slot, as it is the active slot regardless.
	slots[port] = slot;

	return 1;
}

s32 PADfreeze(FreezeAction mode, freezeData* data)
{
	if (!data)
		return -1;

	if (mode == FREEZE_SIZE)
		data->size = sizeof(PadFullFreezeData);
	else if (mode == FREEZE_LOAD)
	{
		PadFullFreezeData* pdata = (PadFullFreezeData*)(data->data);

		Pad::stop_vibrate_all();

		if ((data->size != sizeof(PadFullFreezeData)))
			return 0;

		query = pdata->query;
		if (pdata->query.slot < 4)
			query = pdata->query;

		// Tales of the Abyss - pad fix
		// - restore data for both ports
		for (int port = 0; port < 2; port++)
		{
			for (int slot = 0; slot < 4; slot++)
			{
				u8 mode = pdata->padData[port][slot].mode;

				if (mode != MODE_DIGITAL && mode != MODE_ANALOG && mode != MODE_DS2_NATIVE)
					break;

				memcpy(&pads[port][slot], &pdata->padData[port][slot], sizeof(PadFreezeData));
			}

			if (pdata->slot[port] < 4)
				slots[port] = pdata->slot[port];
		}
	}
	else if (mode == FREEZE_SAVE)
	{
		if (data->size != sizeof(PadFullFreezeData))
			return 0;

		PadFullFreezeData* pdata = (PadFullFreezeData*)(data->data);

		// Tales of the Abyss - pad fix
		// - PCSX2 only saves port0 (save #1), then port1 (save #2)

		memset(pdata, 0, data->size);
		pdata->query = query;

		for (int port = 0; port < 2; port++)
		{
			for (int slot = 0; slot < 4; slot++)
				pdata->padData[port][slot] = pads[port][slot];

			pdata->slot[port] = slots[port];
		}
	}
	else
		return -1;

	return 0;
}

u8 PADstartPoll(int _port, int _slot)
{
	if (_port >= 2)
	{
		query.port           = 0;
		query.slot           = 0;
		query.lastByte       = 1;
		query.currentCommand = 0;
		query.numBytes       = 0;
		query.queryDone      = 1;
		memset(query.response, 0xF3, sizeof(query.response));
		return 0;
	}

	query.port = _port;
	query.slot = _slot;

	const u32 ext_port = sioConvertPortAndSlotToPad(query.port, query.slot);

	if (g_key_status.m_type[ext_port] == ControllerType::NotConnected)
	{
		query.queryDone = 1;
		query.numBytes  = 0;
		query.lastByte  = 1;
		return 0;
	}
	query.queryDone         = 0;
	query.numBytes          = 2;
	query.lastByte          = 0;

	return 0xFF;
}

u8 PADpoll(u8 value)
{
	if (query.lastByte + 1 >= query.numBytes)
		return 0;
	if (query.lastByte && query.queryDone)
		return query.response[++query.lastByte];

	Pad* pad = &pads[query.port][query.slot];

	if (query.lastByte == 0)
	{
		query.lastByte++;
		query.currentCommand = value;

		switch (value)
		{
			case CMD_CONFIG_MODE:
				if (pad->config)
				{
					// In config mode.  Might not actually be leaving it.
					query.set_result(ConfigExit);
					return 0xF3;
				}
				// fallthrough on purpose (but I don't know why)
				PCSX2_FALLTHROUGH;
			case CMD_READ_DATA_AND_VIBRATE:
				{
					query.response[2] = 0x5A;

					const u32 ext_port = sioConvertPortAndSlotToPad(query.port, query.slot);
					const u32 buttons  = pad_live[ext_port].buttons;

					// "Start in analog mode" option: on the first read after
					// reset, promote a still-digital, unlocked pad to analog.
					// Done once (latch) so we set only the starting mode and do
					// not override later game-driven mode changes.
					if (force_analog_pending[query.port][query.slot])
					{
						force_analog_pending[query.port][query.slot] = false;
						if (pad_settings[query.port].force_analog &&
							pad->mode == MODE_DIGITAL && !pad->modeLock)
							pad->mode = MODE_ANALOG;
					}

					if (!TEST_BIT(buttons, PAD_ANALOG) && !pad->modeLock)
					{
						switch (pad->mode)
						{
							case MODE_ANALOG:
							case MODE_DS2_NATIVE:
								pad->mode = MODE_DIGITAL;
								break;
							case MODE_DIGITAL:
							default:
								pad->mode = MODE_ANALOG;
								break;
						}
					}


					query.response[3]         = (buttons >> 8) & 0xFF;
					query.response[4]         = (buttons >> 0) & 0xFF;

					if (pad->mode != MODE_DIGITAL) // ANALOG || DS2 native
					{
						query.response[5] = 0x80 + (pad_live[ext_port].rx >> 8);
						query.response[6] = 0x80 + (pad_live[ext_port].ry >> 8);
						query.response[7] = 0x80 + (pad_live[ext_port].lx >> 8);
						query.response[8] = 0x80 + (pad_live[ext_port].ly >> 8);

						if (pad->mode != MODE_ANALOG) /* DS2 native */
						{
							query.numBytes             = 21;

							query.response[9]          = TEST_BIT(buttons, 13) ? 0 : pad_live[ext_port].analog[ANALOG_BTN_RIGHT];
							query.response[10]         = TEST_BIT(buttons, 15) ? 0 : pad_live[ext_port].analog[ANALOG_BTN_LEFT];
							query.response[11]         = TEST_BIT(buttons, 12) ? 0 : pad_live[ext_port].analog[ANALOG_BTN_UP];
							query.response[12]         = TEST_BIT(buttons, 14) ? 0 : pad_live[ext_port].analog[ANALOG_BTN_DOWN];
							query.response[13]         = TEST_BIT(buttons,  4) ? 0 : pad_live[ext_port].analog[ANALOG_BTN_TRIANGLE];
							query.response[14]         = TEST_BIT(buttons,  5) ? 0 : pad_live[ext_port].analog[ANALOG_BTN_CIRCLE];
							query.response[15]         = TEST_BIT(buttons,  6) ? 0 : pad_live[ext_port].analog[ANALOG_BTN_CROSS];
							query.response[16]         = TEST_BIT(buttons,  7) ? 0 : pad_live[ext_port].analog[ANALOG_BTN_SQUARE];
							query.response[17]         = TEST_BIT(buttons,  2) ? 0 : pad_live[ext_port].analog[ANALOG_BTN_L1];
							query.response[18]         = TEST_BIT(buttons,  3) ? 0 : pad_live[ext_port].analog[ANALOG_BTN_R1];
							query.response[19]         = TEST_BIT(buttons,  0) ? 0 : pad_live[ext_port].analog[ANALOG_BTN_L2];
							query.response[20]         = TEST_BIT(buttons,  1) ? 0 : pad_live[ext_port].analog[ANALOG_BTN_R2];
						}
						else
							query.numBytes             = 9;
					}
					else
						query.numBytes                     = 5;

				}

				query.lastByte = 1;
				return pad->mode;

			case CMD_SET_VREF_PARAM:
				query.set_result(noclue);
				query.queryDone = 1;
				break;

			case CMD_QUERY_DS2_ANALOG_MODE:
				// Right?  Wrong?  No clue.
				if (pad->mode == MODE_DIGITAL)
				{
					queryMaskMode[1] = 0;
					queryMaskMode[2] = 0;
					queryMaskMode[3] = 0;
					queryMaskMode[6] = 0x00;
				}
				else
				{
					queryMaskMode[1] = pad->umask[0];
					queryMaskMode[2] = pad->umask[1];
					queryMaskMode[3] = pad->umask[2];
					// Not entirely sure about this.
					//queryMaskMode[3] = 0x01 | (pad->mode == MODE_DS2_NATIVE)*2;
					queryMaskMode[6] = 0x5A;
				}
				query.set_result(queryMaskMode);
				query.queryDone = 1;
				break;

			case CMD_SET_MODE_AND_LOCK:
				query.set_result(setMode);
				pad->nextVibrate[0] = 0;
				pad->nextVibrate[1] = 0;
				memset(pad->vibrate, 0xFF, sizeof(pad->vibrate));
				pad->vibrate[0] = 0x5A;
				break;

			case CMD_QUERY_MODEL_AND_MODE:
				if (IsDualshock2())
					query.set_result(queryModelDS2);
				else
					query.set_result(queryModelDS1);
				query.queryDone   = 1;
				// Not digital mode.
				query.response[5] = (pad->mode & 0xF) != 1;
				break;

			case CMD_QUERY_ACT:
				query.set_result(queryAct[0]);
				break;

			case CMD_QUERY_COMB:
				query.set_result(queryComb);
				query.queryDone = 1;
				break;

			case CMD_QUERY_MODE:
				query.set_result(queryMode);
				break;

			case CMD_VIBRATION_TOGGLE:
				memcpy(query.response + 2, pad->vibrate, 7);
				query.numBytes = 9;
				pad->nextVibrate[0] = 0;
				pad->nextVibrate[1] = 0;
				memset(pad->vibrate, 0xFF, sizeof(pad->vibrate));
				pad->vibrate[0] = 0x5A;
				break;

			case CMD_SET_DS2_NATIVE_MODE:
				query.set_result(setNativeMode);
				if (!IsDualshock2())
					query.queryDone = 1;
				break;

			default:
				query.numBytes  = 0;
				query.queryDone = 1;
				break;
		}

		return 0xF3;
	}
	query.lastByte++;

	switch (query.currentCommand)
	{
		case CMD_READ_DATA_AND_VIBRATE:
			if (query.lastByte == pad->vibrateI[0])
				pad->nextVibrate[1] = 255 * (value & 1);
			else if (query.lastByte == pad->vibrateI[1])
				pad->nextVibrate[0] = value;

			break;

		case CMD_CONFIG_MODE:
			if (query.lastByte == 3)
			{
				query.queryDone = 1;
				pad->config = value;
			}
			break;

		case CMD_SET_MODE_AND_LOCK:
			if (query.lastByte == 3 && value < 2)
				pad->mode = value ? MODE_ANALOG : MODE_DIGITAL;
			else if (query.lastByte == 4)
			{
				if (value == 3)
					pad->modeLock = 3;
				else
					pad->modeLock = 0;

				query.queryDone = 1;
			}
			break;

		case CMD_QUERY_ACT:
			if (query.lastByte == 3)
			{
				if (value < 2)
					query.set_result(queryAct[value]);
				// bunch of 0's
				// else query.set_result(setMode);
				query.queryDone = 1;
			}
			break;

		case CMD_QUERY_MODE:
			if (query.lastByte == 3 && value < 2)
			{
				query.response[6] = 4 + value * 3;
				query.queryDone = 1;
			}
			// bunch of 0's
			//else data = setMode;
			break;

		case CMD_VIBRATION_TOGGLE:
			if (query.lastByte >= 3)
			{
				if (value == 0)
					pad->vibrateI[0] = (u8)query.lastByte;
				else if (value == 1)
					pad->vibrateI[1] = (u8)query.lastByte;
				pad->vibrate[query.lastByte - 2] = value;
			}
			break;

		case CMD_SET_DS2_NATIVE_MODE:
			if (query.lastByte > 2 && query.lastByte < 6)
				pad->umask[query.lastByte - 3] = value;
			pad->mode = MODE_DS2_NATIVE;
			break;

		default:
			return 0;
	}

	return query.response[query.lastByte];
}

bool PADcomplete(void) { return query.queryDone; }

void PAD::LoadConfig()
{
	EmuConfig.MultitapPort0_Enabled = (pad_multitap & 1) != 0;
	EmuConfig.MultitapPort1_Enabled = (pad_multitap & 2) != 0;
	for (u32 i = 0; i < NUM_CONTROLLER_PORTS; i++)
		g_key_status.m_type[i] = NotConnected;
	for (u32 port = 0; port < NUM_CONTROLLER_PORTS; port++)
	{
		const u32 i = pad_for_port(port);
		const char* type = s_pad_type_name[port] ? s_pad_type_name[port] : ((port == 0) ? "DualShock2" : "None");
		if (i >= NUM_CONTROLLER_PORTS)
			continue;
		for (const ControllerInfo& info : s_controller_info)
		{
			if (!strcmp(type, info.name))
			{
				const u16 motor_scale = (u16)pcsx2_clamp_i(lrintf(DEFAULT_MOTOR_SCALE * 256.0f), 0, 65535);
				if (info.vibration_caps != NoVibration)
				{
					g_key_status.m_vibration_scale_q8[i][0] = motor_scale;
					g_key_status.m_vibration_scale_q8[i][1] = motor_scale;
				}
				g_key_status.m_type[i]     = info.type;
			}
		}
	}
}
