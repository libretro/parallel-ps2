/* Konami Python 2 I/O board (P2IO), the device behind the cabinet's USB
 * port: the JAMMA inputs, the Dallas security dongles, the DIP switches,
 * the coin mechanism and two serial (SCI) ports with the game's own
 * peripherals on them - e-amusement card readers, the DDR extio, the
 * Thrill Drive wheel and seatbelt, the Toy's March drum pads.
 *
 * Byte level and free of I/O: usb-python2.cpp moves the USB packets, the
 * libretro front end fills in the inputs. Everything that changes as the
 * game runs is in p2io_t.st, which a savestate copies as it is. */
#ifndef P2IO_H
#define P2IO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The descriptor's InputType. */
enum
{
	P2IO_GAME_DRUMMANIA = 0,
	P2IO_GAME_GUITARFREAKS,
	P2IO_GAME_DDR,
	P2IO_GAME_TOYSMARCH,
	P2IO_GAME_THRILLDRIVE,
	P2IO_GAME_DANCE864,
	P2IO_GAME_COUNT
};

/* One word of buttons per player. Test and service are read from
 * player 1's word. The K slots are the game's own buttons: */
#define P2IO_IN_START   (1u << 0)
#define P2IO_IN_LEFT    (1u << 1)  /* select left */
#define P2IO_IN_RIGHT   (1u << 2)  /* select right */
#define P2IO_IN_K(n)    (1u << (3 + (n)))  /* n 0..7 */
#define P2IO_IN_KEY(n)  (1u << (11 + (n))) /* keypad 0..9, 10 is '00' */
#define P2IO_IN_CARD    (1u << 22) /* card in / out */
#define P2IO_IN_COIN    (1u << 23)
#define P2IO_IN_TEST    (1u << 24)
#define P2IO_IN_SERVICE (1u << 25)

#define P2IO_DDR_UP       P2IO_IN_K(0)
#define P2IO_DDR_DOWN     P2IO_IN_K(1)
#define P2IO_DDR_LEFT     P2IO_IN_K(2)
#define P2IO_DDR_RIGHT    P2IO_IN_K(3)

#define P2IO_DM_HIHAT     P2IO_IN_K(0)
#define P2IO_DM_SNARE     P2IO_IN_K(1)
#define P2IO_DM_HIGH_TOM  P2IO_IN_K(2)
#define P2IO_DM_LOW_TOM   P2IO_IN_K(3)
#define P2IO_DM_CYMBAL    P2IO_IN_K(4)
#define P2IO_DM_BASS      P2IO_IN_K(5)

#define P2IO_GF_PICK      P2IO_IN_K(0)
#define P2IO_GF_WAIL      P2IO_IN_K(1)
#define P2IO_GF_RED       P2IO_IN_K(2)
#define P2IO_GF_GREEN     P2IO_IN_K(3)
#define P2IO_GF_BLUE      P2IO_IN_K(4)
#define P2IO_GF_EFFECT1   P2IO_IN_K(5)
#define P2IO_GF_EFFECT2   P2IO_IN_K(6)
#define P2IO_GF_EFFECT3   P2IO_IN_K(7)

#define P2IO_TM_CYMBAL    P2IO_IN_K(0)
#define P2IO_TM_DRUM_L    P2IO_IN_K(1)
#define P2IO_TM_DRUM_R    P2IO_IN_K(2)

#define P2IO_D864_LEFT    P2IO_IN_K(0)
#define P2IO_D864_CENTER  P2IO_IN_K(1)
#define P2IO_D864_RIGHT   P2IO_IN_K(2)

#define P2IO_TD_GEAR_DOWN P2IO_IN_K(0)
#define P2IO_TD_GEAR_UP   P2IO_IN_K(1)
#define P2IO_TD_SEATBELT  P2IO_IN_K(2)

/* Thrill Drive's analog range, and the wheel's rest position. */
#define P2IO_ANALOG_MAX    0xffff
#define P2IO_WHEEL_CENTER  0x7fdf

typedef struct p2io_input
{
	uint32_t pad[2];
	int32_t  wheel; /* 0..P2IO_ANALOG_MAX, larger is further left */
	int32_t  accel; /* 0..P2IO_ANALOG_MAX */
	int32_t  brake; /* 0..P2IO_ANALOG_MAX */
} p2io_input;

/* What the descriptor says; not part of the savestate. */
typedef struct p2io_config
{
	int     game;
	uint8_t dipsw[4];      /* DIPSW1..DIPSW4, 0 or 1 */
	uint8_t force_31khz;
	uint8_t dongle_loaded[2]; /* black, white */
	uint8_t dongle[2][40];    /* serial (8) then payload (32) */
	uint8_t card_loaded[2];
	uint8_t card[2][8];
} p2io_config;

#define P2IO_RX_SIZE  640
#define P2IO_TX_SIZE  640
#define P2IO_SCI_SIZE 384

enum
{
	P2IO_SCI_NONE = 0,
	P2IO_SCI_ACIO,
	P2IO_SCI_EXTIO,
	P2IO_SCI_DRUMPAD
};

enum
{
	P2IO_NODE_NONE = 0,
	P2IO_NODE_ICCA,
	P2IO_NODE_HANDLE,
	P2IO_NODE_BELT
};

typedef struct p2io_icca
{
	uint8_t accept;
	uint8_t inserted;
	uint8_t card_held;
	uint8_t card_loaded;
	uint8_t key_state;
	uint8_t key_event[2];
	uint8_t card[8];
} p2io_icca;

typedef struct p2io_sci
{
	uint8_t  kind;
	uint8_t  open;
	uint16_t len;
	uint8_t  buf[P2IO_SCI_SIZE];
	uint8_t  node[2];      /* the ACIO nodes at addresses 1 and 2 */
	p2io_icca icca[2];
	int8_t   wheel_ffb;
	uint8_t  wheel_calibrating;
	uint8_t  belt_on;
	uint8_t  belt_held;
	uint8_t  drum_index;
	uint8_t  drum_held[6];
} p2io_sci;

typedef struct p2io_stage
{
	uint8_t  data_out;
	uint8_t  clk;
	uint8_t  state;
	uint8_t  bit;
	uint8_t  mask;
	uint16_t shift;
} p2io_stage;

typedef struct p2io_state
{
	p2io_input in;
	uint32_t jamma;
	uint32_t jamma_updates;
	uint16_t coins[2];
	uint8_t  coin_held[2];
	int8_t   dongle_request;
	uint8_t  knob[2];
	p2io_stage stage[2];
	uint8_t  rx_invert;
	uint16_t rx_len;
	uint16_t tx_len;
	uint16_t tx_pos;
	uint8_t  rx[P2IO_RX_SIZE];
	uint8_t  tx[P2IO_TX_SIZE];
	p2io_sci sci[2];
} p2io_state;

typedef struct p2io
{
	p2io_config cfg;
	p2io_state  st;
} p2io_t;

/* Power on with cfg: the state the board has after a USB reset. */
void p2io_init(p2io_t* p, const p2io_config* cfg);
/* A USB reset: the state starts over, the configuration stays. */
void p2io_reset(p2io_t* p);

void p2io_set_input(p2io_t* p, const p2io_input* in);

/* Endpoint 3, interrupt IN: the 12-byte JAMMA report. */
#define P2IO_JAMMA_REPORT 12
void p2io_jamma_read(p2io_t* p, uint8_t out[P2IO_JAMMA_REPORT]);

/* Endpoint 2, bulk OUT: command bytes from the game. */
void p2io_out(p2io_t* p, const uint8_t* data, size_t len);

/* Endpoint 1, bulk IN: up to cap bytes of the response to the oldest
 * complete command, the rest kept for the next read. With no command
 * waiting it is a single 0 byte, as the board answers. */
size_t p2io_in(p2io_t* p, uint8_t* out, size_t cap);

/* A 40-byte dongle dump, either layout (8-byte serial then the 32-byte
 * payload, or MAME's payload then serial), checked by its CRC and put in
 * the board's order: serial then payload. Returns 0 and zeroes out when
 * neither layout's CRC holds. */
int p2io_parse_dongle(const uint8_t in[40], uint8_t out[40]);

/* A card file: 16 hex digits (whitespace around them allowed). Returns 0
 * when it does not hold at least 8 bytes of hex. */
int p2io_parse_card(const char* text, size_t len, uint8_t out[8]);

#ifdef __cplusplus
}
#endif

#endif
