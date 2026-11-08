/* Konami Python 2 I/O board (P2IO). See p2io.h. */
#include "p2io.h"

#include <string.h>

/* Command codes, named after the driver's own symbols. */
enum
{
	P2IO_CMD_GET_VERSION   = 0x01, /* cmd_getver */
	P2IO_CMD_FWRITEMODE    = 0x03, /* cmd_fwritemode */
	P2IO_CMD_SET_WATCHDOG  = 0x05, /* cmd_watchdog */
	P2IO_CMD_SET_AV_MASK   = 0x22, /* cmd_avmask */
	P2IO_CMD_GET_AV_REPORT = 0x23, /* cmd_avreport */
	P2IO_CMD_LAMP_OUT      = 0x24, /* cmd_lampout, 0x24ff all banks */
	P2IO_CMD_DALLAS        = 0x25, /* cmd_dallas */
	P2IO_CMD_SEND_IR       = 0x26, /* cmd_irsend */
	P2IO_CMD_READ_DIPSW    = 0x27, /* cmd_dipsw */
	P2IO_CMD_GET_JAMMA_POR = 0x28, /* cmd_jamma_por */
	P2IO_CMD_PORT_READ     = 0x29, /* cmd_portread */
	P2IO_CMD_PORT_READ_POR = 0x2a, /* cmd_portread_por */
	P2IO_CMD_JAMMA_START   = 0x2f, /* cmd_jammastart */
	P2IO_CMD_COIN_STOCK    = 0x31, /* cmd_coinstock */
	P2IO_CMD_COIN_COUNTER  = 0x32, /* cmd_coincounter */
	P2IO_CMD_COIN_BLOCKER  = 0x33, /* cmd_coinblocker */
	P2IO_CMD_COIN_OUT      = 0x34, /* cmd_coincounterout */
	P2IO_CMD_SCI_SETUP     = 0x38, /* cmd_scisetup */
	P2IO_CMD_SCI_WRITE     = 0x3a, /* cmd_sciwrite */
	P2IO_CMD_SCI_READ      = 0x3b  /* cmd_sciread */
};

#define P2IO_MAGIC        0xaa
#define P2IO_AV_15KHZ     0x80
#define P2IO_AV_31KHZ     0x00
#define P2IO_JAMMA_RESET  0xf0ffff80u

#define JAMMA_TEST     0x10000000u
#define JAMMA_COIN1    0x20000000u
#define JAMMA_SERVICE  0x40000000u
#define JAMMA_COIN2    0x80000000u
#define JAMMA_WATCHDOG 0x00000002u

/* A player's JAMMA bits: player 2's are player 1's shifted up 8. */
#define J_P1(b)        ((uint32_t)(b))
#define J_P2(b)        ((uint32_t)(b) << 8)

#define J_START        0x0100u
#define J_DDR_UP       0x0200u
#define J_DDR_DOWN     0x0400u
#define J_DDR_LEFT     0x0800u
#define J_DDR_RIGHT    0x1000u
#define J_DDR_SEL_L    0x4000u
#define J_DDR_SEL_R    0x8000u
#define J_GF_PICK      0x0200u
#define J_GF_WAIL      0x0400u
#define J_GF_EFFECT2   0x0800u
#define J_GF_EFFECT1   0x1000u
#define J_GF_RED       0x2000u
#define J_GF_GREEN     0x4000u
#define J_GF_BLUE      0x8000u
#define J_TM_LEFT      0x0800u
#define J_TM_RIGHT     0x1000u
#define J_D864_CENTER  0x0400u
#define J_D864_PAD_L   0x0800u
#define J_D864_PAD_R   0x1000u
#define J_D864_SEL_L   0x2000u
#define J_D864_SEL_R   0x4000u

/* DrumMania is one player, with bits across both halves. */
#define J_DM_START     0x00000100u
#define J_DM_HIHAT     0x00000200u
#define J_DM_SNARE     0x00000400u
#define J_DM_HIGH_TOM  0x00000800u
#define J_DM_LOW_TOM   0x00001000u
#define J_DM_CYMBAL    0x00002000u
#define J_DM_BASS      0x00008000u
#define J_DM_SEL_L     0x00080000u
#define J_DM_SEL_R     0x00100000u

#define J_TD_START     0x00000100u
#define J_TD_GEAR_DOWN 0x00000200u
#define J_TD_GEAR_UP   0x00000400u

/* The Dance 86.4 stage (GN845-PWB(B)) serial handshake. */
enum
{
	STAGE_IDLE = 0,
	STAGE_INIT,
	STAGE_INIT_DONE
};

/* The ACIO packet header: magic, address, code (big endian), sequence
 * number, payload length. A checksum byte follows the payload. */
#define ACIO_HEADER 6

static void p2io_sci_setup(p2io_t* p)
{
	p2io_sci* a = &p->st.sci[0];
	p2io_sci* b = &p->st.sci[1];

	memset(p->st.sci, 0, sizeof(p->st.sci));
	switch (p->cfg.game)
	{
		case P2IO_GAME_DDR:
			a->kind    = P2IO_SCI_EXTIO;
			b->kind    = P2IO_SCI_ACIO;
			b->node[0] = P2IO_NODE_ICCA;
			b->node[1] = P2IO_NODE_ICCA;
			break;
		case P2IO_GAME_GUITARFREAKS:
			a->kind    = P2IO_SCI_ACIO;
			a->node[0] = P2IO_NODE_ICCA;
			a->node[1] = P2IO_NODE_ICCA;
			break;
		case P2IO_GAME_DRUMMANIA:
			a->kind    = P2IO_SCI_ACIO;
			a->node[0] = P2IO_NODE_ICCA;
			break;
		case P2IO_GAME_TOYSMARCH:
			a->kind = P2IO_SCI_DRUMPAD;
			break;
		case P2IO_GAME_THRILLDRIVE:
			b->kind    = P2IO_SCI_ACIO;
			b->node[0] = P2IO_NODE_HANDLE;
			b->node[1] = P2IO_NODE_BELT;
			break;
		default:
			break;
	}
}

void p2io_reset(p2io_t* p)
{
	unsigned i;
	memset(&p->st, 0, sizeof(p->st));
	p->st.jamma          = P2IO_JAMMA_RESET;
	p->st.dongle_request = -1;
	p->st.in.wheel       = P2IO_WHEEL_CENTER;
	for (i = 0; i < 2; i++)
		p->st.stage[i].mask = 0xff;
	p2io_sci_setup(p);
}

void p2io_init(p2io_t* p, const p2io_config* cfg)
{
	p->cfg = *cfg;
	if (p->cfg.game < 0 || p->cfg.game >= P2IO_GAME_COUNT)
		p->cfg.game = P2IO_GAME_DRUMMANIA;
	p2io_reset(p);
}

void p2io_set_input(p2io_t* p, const p2io_input* in)
{
	p->st.in = *in;
}

/* ---- escaping ---------------------------------------------------- */

/* 0xaa and 0xff go out as 0xff and their complement. */
static size_t p2io_escape(uint8_t* dst, size_t cap, const uint8_t* src, size_t len)
{
	size_t i, n = 0;
	for (i = 0; i < len; i++)
	{
		if (src[i] == 0xaa || src[i] == 0xff)
		{
			if (n + 2 > cap)
				break;
			dst[n++] = 0xff;
			dst[n++] = (uint8_t)~src[i];
		}
		else
		{
			if (n + 1 > cap)
				break;
			dst[n++] = src[i];
		}
	}
	return n;
}

/* In place; returns the new length. */
static size_t p2io_unescape(uint8_t* buf, size_t len)
{
	size_t i, n = 0;
	int invert = 0;
	for (i = 0; i < len; i++)
	{
		if (buf[i] == 0xff)
			invert = 1;
		else
		{
			buf[n++] = invert ? (uint8_t)~buf[i] : buf[i];
			invert = 0;
		}
	}
	return n;
}

/* ---- the serial ports -------------------------------------------- */

static void sci_queue(p2io_sci* s, const uint8_t* data, size_t len)
{
	if (len > (size_t)(P2IO_SCI_SIZE - s->len))
		len = (size_t)(P2IO_SCI_SIZE - s->len);
	memcpy(s->buf + s->len, data, len);
	s->len = (uint16_t)(s->len + len);
}

static unsigned acio_sum(const uint8_t* data, size_t len)
{
	unsigned sum = 0;
	size_t i;
	for (i = 0; i < len; i++)
		sum += data[i];
	return sum & 0xff;
}

/* Version answers: device id, flag, version, product code, build date
 * and time, as the readers report them. */
static size_t acio_version(uint8_t* out, const char* product)
{
	static const uint8_t head[8] = { 0x03, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00 };
	static const char date[16] = "Oct 26 2005";
	static const char time[16] = "13 : 55 : 03";
	memcpy(out, head, 8);
	memcpy(out + 8, product, 4);
	memcpy(out + 12, date, 16);
	memcpy(out + 28, time, 16);
	return 44;
}

/* The keypad codes, in the order a held key is looked for: 0 to 9, 00. */
static const uint8_t s_keypad_code[11] = { 16, 1, 5, 9, 2, 6, 10, 3, 7, 11, 4 };

static size_t icca_write(p2io_t* p, p2io_icca* c, unsigned player, unsigned code,
	const uint8_t* pkt, size_t len, uint8_t* out, int* accepted)
{
	const uint32_t pad = p->st.in.pad[player];
	uint8_t key = 0;
	unsigned i;

	*accepted = 1;
	if (code == 0x0002)
		return acio_version(out, "ICCA");
	if (code == 0x0003)
	{
		/* Startup. */
		c->accept    = 0;
		c->inserted  = 0;
		c->card_held = 0;
		c->key_state = 0;
		c->key_event[0] = c->key_event[1] = 0;
		out[0] = 0;
		return 1;
	}
	if (code == 0x0080)
		return 0;
	if (code != 0x0131 && code != 0x0134 && code != 0x0135)
	{
		out[0] = 0;
		return 1;
	}

	if (code == 0x0135 && len > 7)
	{
		switch (pkt[7])
		{
			case 0x00:
				c->accept = 0;
				break;
			case 0x11:
				c->accept = 1;
				break;
			case 0x12:
				c->accept    = 0;
				c->inserted  = 0;
				c->card_held = 0;
				break;
			default:
				break;
		}
	}

	for (i = 0; i < 11; i++)
		if (pad & P2IO_IN_KEY(i))
		{
			key = s_keypad_code[i];
			break;
		}

	if (pad & P2IO_IN_CARD)
	{
		if (!c->card_held)
		{
			if (c->accept || c->inserted)
				c->inserted = (uint8_t)!c->inserted;
			c->card_held = 1;
		}
	}
	else
		c->card_held = 0;

	/* The first insertion reads the card and takes it back out; the next
	 * poll with the button still down puts it in. */
	if (c->inserted && !c->card_loaded)
	{
		c->card_held = 0;
		c->inserted  = 0;
		memset(c->card, 0, sizeof(c->card));
		if (p->cfg.card_loaded[player])
		{
			memcpy(c->card, p->cfg.card[player], 8);
			c->card_loaded = 1;
		}
	}

	memset(out, 0, 16);
	out[0] = c->inserted ? 2 : 1;
	out[1] = (c->accept && c->inserted && c->card_loaded) ? 0x30 : 0;
	if (c->inserted)
		memcpy(out + 2, c->card, 8);
	out[11] = 3; /* keypad started */

	if (key & (c->key_state ^ key))
	{
		uint8_t ev = 0;
		if (c->key_event[0])
			ev = (uint8_t)((c->key_event[0] + 0x10) & 0xf0);
		ev |= (uint8_t)(0x80 | key);
		c->key_event[1] = c->key_event[0];
		c->key_event[0] = ev;
	}
	out[12]      = c->key_event[0];
	out[13]      = c->key_event[1];
	c->key_state = key;
	return 16;
}

static size_t handle_write(p2io_sci* s, unsigned code, const uint8_t* pkt, size_t len, uint8_t* out)
{
	if (code == 0x0002)
		return acio_version(out, "HNDL");
	if (code == 0x0100)
	{
		s->wheel_ffb         = 0;
		s->wheel_calibrating = 0;
		out[0] = 0;
		return 1;
	}
	if (code == 0x0120)
	{
		s->wheel_ffb         = (int8_t)(len > 6 ? pkt[6] : 0);
		s->wheel_calibrating = (uint8_t)(len > 9 && pkt[9] == 0xfe);
		memset(out, 0, 4);
		return 4;
	}
	out[0] = 0;
	return 1;
}

static size_t belt_write(p2io_t* p, p2io_sci* s, unsigned code, uint8_t* out)
{
	const int pressed = (p->st.in.pad[0] & P2IO_TD_SEATBELT) != 0;
	if (pressed && !s->belt_held)
		s->belt_on = (uint8_t)!s->belt_on;
	s->belt_held = (uint8_t)pressed;

	if (code == 0x0002)
		return acio_version(out, "BELT");
	if (code == 0x0100)
	{
		s->belt_on   = 0;
		s->belt_held = 0;
	}
	else if (code == 0x0111)
	{
		memset(out, 0, 4);
		return 4;
	}
	else if (code == 0x0113)
	{
		out[0] = 0;
		out[1] = 0;
		out[2] = s->belt_on ? 0 : 0xff;
		return 3;
	}
	out[0] = 0;
	return 1;
}

static void acio_write(p2io_t* p, p2io_sci* s, const uint8_t* pkt, size_t len)
{
	uint8_t resp[64];
	uint8_t framed[1 + 2 * (5 + 64 + 1)];
	size_t i, n = 0, total;
	unsigned addr, code;
	int accepted = 0;

	if (!s->open || len < ACIO_HEADER)
		return;

	/* A run of sync bytes comes straight back. */
	for (i = 0; i < len && pkt[i] == 0xaa; i++)
		;
	if (i == len)
	{
		sci_queue(s, pkt, len);
		return;
	}

	total = ACIO_HEADER + (size_t)pkt[5];
	if (pkt[0] != 0xaa || len < total + 1)
		return;
	if (acio_sum(pkt + 1, total - 1) != pkt[total])
		return;

	addr = pkt[1];
	code = ((unsigned)pkt[2] << 8) | pkt[3];
	if (addr == 0 && code == 0x0001)
	{
		resp[0] = (uint8_t)((s->node[0] != P2IO_NODE_NONE) + (s->node[1] != P2IO_NODE_NONE));
		n        = 1;
		accepted = 1;
	}
	else if ((addr == 1 || addr == 2) && s->node[addr - 1] != P2IO_NODE_NONE)
	{
		accepted = 1;
		switch (s->node[addr - 1])
		{
			case P2IO_NODE_ICCA:
				n = icca_write(p, &s->icca[addr - 1], addr - 1, code, pkt, len, resp, &accepted);
				break;
			case P2IO_NODE_HANDLE:
				n = handle_write(s, code, pkt, len, resp);
				break;
			default:
				n = belt_write(p, s, code, resp);
				break;
		}
	}
	if (!accepted)
		return;

	{
		/* Address with the response bit, code, sequence number, length,
		 * payload and a checksum of all of it. */
		uint8_t body[5 + 64 + 1];
		body[0] = (uint8_t)(pkt[1] | 0x80);
		body[1] = pkt[2];
		body[2] = pkt[3];
		body[3] = pkt[4];
		body[4] = (uint8_t)n;
		memcpy(body + 5, resp, n);
		body[5 + n] = (uint8_t)acio_sum(body, 5 + n);
		framed[0] = 0xaa;
		sci_queue(s, framed, 1 + p2io_escape(framed + 1, sizeof(framed) - 1, body, 6 + n));
	}
}

static void extio_write(p2io_sci* s, const uint8_t* pkt, size_t len)
{
	/* Lamp states only, for a cabinet the core has no lamps of. */
	static const uint8_t ack = 0x11;
	if (!s->open || len != 4)
		return;
	if (((pkt[0] + pkt[1] + pkt[2]) & 0x7f) != pkt[3])
		return;
	sci_queue(s, &ack, 1);
}

static void sci_write(p2io_t* p, p2io_sci* s, uint8_t* pkt, size_t len)
{
	/* The SCI payload is escaped on its own, inside the P2IO escaping. */
	len = p2io_unescape(pkt, len);
	if (s->kind == P2IO_SCI_ACIO)
		acio_write(p, s, pkt, len);
	else if (s->kind == P2IO_SCI_EXTIO)
		extio_write(s, pkt, len);
	/* The drum pads take keep-alive packets and answer nothing. */
}

static uint8_t drum_edge(p2io_t* p, p2io_sci* s, unsigned player, uint32_t bit, unsigned slot)
{
	const uint8_t down = (p->st.in.pad[player] & bit) != 0;
	const uint8_t was  = s->drum_held[slot];
	s->drum_held[slot] = down;
	return (down && !was) ? 0x80 : 0;
}

/* Reads up to cap of what the port holds. The drum pads are read for
 * their state whatever was asked for, as the board does. */
static size_t sci_read(p2io_t* p, p2io_sci* s, uint8_t* out, size_t want, size_t cap)
{
	size_t n;
	if (s->kind == P2IO_SCI_DRUMPAD)
	{
		uint8_t state[9];
		if (cap < 11)
			return 0;
		state[0] = s->drum_index;
		state[1] = drum_edge(p, s, 0, P2IO_TM_CYMBAL, 0);
		state[2] = drum_edge(p, s, 0, P2IO_TM_DRUM_L, 1);
		state[3] = drum_edge(p, s, 0, P2IO_TM_DRUM_R, 2);
		state[4] = 0;
		state[5] = drum_edge(p, s, 1, P2IO_TM_CYMBAL, 3);
		state[6] = drum_edge(p, s, 1, P2IO_TM_DRUM_L, 4);
		state[7] = drum_edge(p, s, 1, P2IO_TM_DRUM_R, 5);
		state[8] = 0;
		s->drum_index = (uint8_t)((s->drum_index + 1) & 7);
		out[0] = 0xaa;
		memcpy(out + 1, state, 9);
		out[10] = (uint8_t)acio_sum(state, 9);
		return 11;
	}
	if (s->kind == P2IO_SCI_NONE || !s->open || !s->len)
		return 0;
	n = want < s->len ? want : s->len;
	if (n > cap)
		n = cap;
	memcpy(out, s->buf, n);
	memmove(s->buf, s->buf + n, s->len - n);
	s->len = (uint16_t)(s->len - n);
	return n;
}

/* ---- the Dance 86.4 stage ---------------------------------------- */

/* After MAME's ksys573.cpp: the game clocks a 13-bit shift register on
 * lamp outputs, and the stage answers with an init sequence on its pad
 * inputs before they read as pads. */
static void stage_write(p2io_t* p, unsigned i, int data_out, int clk)
{
	p2io_stage* s = &p->st.stage[i];
	s->data_out = (uint8_t)(data_out != 0);
	if (clk != s->clk)
	{
		s->clk = (uint8_t)clk;
		if (clk)
		{
			s->shift = (uint16_t)(((s->shift >> 1) | ((unsigned)s->data_out << 12)) & 0x1fff);
			if (s->state == STAGE_IDLE)
			{
				if (s->shift == 0xc90)
				{
					s->state = STAGE_INIT;
					s->bit   = 0;
					s->mask  = 0xf9;
				}
			}
			else if (s->state == STAGE_INIT)
			{
				s->bit++;
				if (s->bit < 22)
				{
					s->mask = (uint8_t)~0x12;
					if (s->bit - 1 < 2)
						s->mask |= 0x10;
					s->mask |= (uint8_t)(1u << (s->bit & 1));
				}
				else
				{
					s->bit   = 0;
					s->state = STAGE_INIT_DONE;
					s->mask  = 0xff;
				}
			}
		}
	}
	if (s->state != STAGE_INIT_DONE)
	{
		p->st.jamma &= 0xff0000ffu;
		p->st.jamma |= (uint32_t)p->st.stage[0].mask << 8;
		p->st.jamma |= (uint32_t)p->st.stage[1].mask << 16;
	}
}

/* ---- commands ---------------------------------------------------- */

/* Handles the oldest complete command; returns the escaped response
 * length put in tx, or 0 when there is no complete command yet. */
static size_t p2io_command(p2io_t* p)
{
	p2io_state* st = &p->st;
	uint8_t body[2 + 256];
	size_t n = 0, total, i;
	unsigned cmd;

	/* Anything before a packet start is noise. */
	for (i = 0; i < st->rx_len && st->rx[i] != P2IO_MAGIC; i++)
		;
	if (i)
	{
		memmove(st->rx, st->rx + i, st->rx_len - i);
		st->rx_len = (uint16_t)(st->rx_len - i);
	}
	if (st->rx_len < 4)
		return 0;
	total = (size_t)st->rx[1] + 2;
	if (st->rx_len < total)
		return 0;

#define ARG(k) ((size_t)(k) < total ? st->rx[(k)] : 0)
	cmd       = st->rx[3];
	body[n++] = st->rx[2]; /* sequence number */
	body[n++] = 0;         /* status: ok */

	switch (cmd)
	{
		case P2IO_CMD_GET_VERSION:
		{
			/* D44:1.6.4 */
			static const uint8_t ver[7] = { 'D', '4', '4', 0, 1, 6, 4 };
			memcpy(body + n, ver, 7);
			n += 7;
			break;
		}
		case P2IO_CMD_GET_AV_REPORT:
			body[n++] = p->cfg.force_31khz ? P2IO_AV_31KHZ : P2IO_AV_15KHZ;
			break;
		case P2IO_CMD_DALLAS:
		{
			const unsigned sub = ARG(4);
			if (sub <= 2)
			{
				if (sub != 2)
					st->dongle_request = (int8_t)sub;
				if (st->dongle_request >= 0)
				{
					body[n++] = p->cfg.dongle_loaded[st->dongle_request];
					memcpy(body + n, p->cfg.dongle[st->dongle_request], 40);
					n += 40;
					break;
				}
			}
			else if (sub != 3)
				break;
			/* Writes, and reads before a dongle was picked, echo the
			 * request's data. */
			body[n++] = 0;
			for (i = 0; i < 40; i++)
				body[n++] = ARG(5 + i);
			break;
		}
		case P2IO_CMD_READ_DIPSW:
		{
			uint8_t v = 0;
			for (i = 0; i < 4; i++)
				if (p->cfg.dipsw[i])
					v |= (uint8_t)(1u << (3 - i));
			body[n++] = (uint8_t)(v & 0x7f);
			break;
		}
		case P2IO_CMD_COIN_STOCK:
			body[n++] = 0;
			body[n++] = (uint8_t)(st->coins[0] >> 8);
			body[n++] = (uint8_t)st->coins[0];
			body[n++] = (uint8_t)(st->coins[1] >> 8);
			body[n++] = (uint8_t)st->coins[1];
			break;
		case P2IO_CMD_LAMP_OUT:
			if (total >= 9 && ARG(4) == 0xff)
				body[n++] = 0; /* all banks */
			else if (total >= 6 && ARG(4) != 0xff)
			{
				const unsigned v = ARG(5);
				if (p->cfg.game == P2IO_GAME_DANCE864 && ARG(4) == 1)
				{
					stage_write(p, 0, (v & 1) != 0, !(v & 2));
					stage_write(p, 1, (v & 4) != 0, !(v & 8));
				}
				body[n++] = 0;
			}
			break;
		case P2IO_CMD_GET_JAMMA_POR:
			memset(body + n, 0, 4);
			n += 4;
			break;
		case P2IO_CMD_FWRITEMODE:
			if (ARG(4) == 0xaa)
				body[n++] = 0;
			break;
		case P2IO_CMD_SET_AV_MASK:
		case P2IO_CMD_COIN_COUNTER:
		case P2IO_CMD_COIN_BLOCKER:
		case P2IO_CMD_COIN_OUT:
		case P2IO_CMD_PORT_READ:
		case P2IO_CMD_PORT_READ_POR:
		case P2IO_CMD_SEND_IR:
		case P2IO_CMD_SET_WATCHDOG:
		case P2IO_CMD_JAMMA_START:
			body[n++] = 0;
			break;
		case P2IO_CMD_SCI_SETUP:
		{
			const unsigned port = ARG(4), op = ARG(5);
			body[n++] = 0;
			if (port < 2 && st->sci[port].kind != P2IO_SCI_NONE)
			{
				if (op == 0)
					st->sci[port].open = 1;
				else if (op == 0xff)
					st->sci[port].open = 0;
			}
			break;
		}
		case P2IO_CMD_SCI_WRITE:
		{
			const unsigned port = ARG(4);
			size_t len = ARG(5);
			if (total < 6)
				len = 0;
			else if (len > total - 6)
				len = total - 6;
			if (port < 2 && len)
			{
				uint8_t pkt[256];
				memcpy(pkt, st->rx + 6, len);
				sci_write(p, &st->sci[port], pkt, len);
			}
			body[n++] = (uint8_t)ARG(5);
			break;
		}
		case P2IO_CMD_SCI_READ:
		{
			const unsigned port = ARG(4);
			const size_t at = n++;
			size_t got = 0;
			/* The response's length byte counts up to 255. */
			if (port < 2 && ARG(5))
				got = sci_read(p, &st->sci[port], body + n, ARG(5), 255 - n);
			body[at] = (uint8_t)got;
			n += got;
			break;
		}
		default:
			break;
	}
#undef ARG

	memmove(st->rx, st->rx + total, st->rx_len - total);
	st->rx_len = (uint16_t)(st->rx_len - total);

	/* Magic, then the length and the body, escaped. */
	{
		uint8_t lenbyte = (uint8_t)n;
		st->tx[0] = P2IO_MAGIC;
		st->tx_len = (uint16_t)(1 + p2io_escape(st->tx + 1, P2IO_TX_SIZE - 1, &lenbyte, 1));
		st->tx_len = (uint16_t)(st->tx_len + p2io_escape(st->tx + st->tx_len, P2IO_TX_SIZE - st->tx_len, body, n));
		st->tx_pos = 0;
	}
	return st->tx_len;
}

void p2io_out(p2io_t* p, const uint8_t* data, size_t len)
{
	p2io_state* st = &p->st;
	size_t i;
	/* Unescaped as it comes, so an escape split across two transfers
	 * still pairs up. */
	for (i = 0; i < len; i++)
	{
		if (data[i] == 0xff)
		{
			st->rx_invert = 1;
			continue;
		}
		if (st->rx_len < P2IO_RX_SIZE)
			st->rx[st->rx_len++] = st->rx_invert ? (uint8_t)~data[i] : data[i];
		st->rx_invert = 0;
	}
}

size_t p2io_in(p2io_t* p, uint8_t* out, size_t cap)
{
	p2io_state* st = &p->st;
	size_t n;
	if (!cap)
		return 0;
	if (st->tx_pos >= st->tx_len && !p2io_command(p))
	{
		out[0] = 0;
		return 1;
	}
	n = (size_t)(st->tx_len - st->tx_pos);
	if (n > cap)
		n = cap;
	memcpy(out, st->tx + st->tx_pos, n);
	st->tx_pos = (uint16_t)(st->tx_pos + n);
	return n;
}

/* ---- the JAMMA report -------------------------------------------- */

static void jamma_key(p2io_state* st, int down, uint32_t bits)
{
	if (down)
		st->jamma &= ~bits;
	else
		st->jamma |= bits;
}

static void jamma_coin(p2io_state* st, uint32_t bit, unsigned i)
{
	if (!(st->jamma & bit))
	{
		if (!st->coin_held[i])
		{
			st->coins[i]++;
			st->coin_held[i] = 1;
		}
	}
	else
		st->coin_held[i] = 0;
}

static int32_t clamp_analog(int32_t v)
{
	return v < 0 ? 0 : v > P2IO_ANALOG_MAX ? P2IO_ANALOG_MAX : v;
}

static void put_be16(uint8_t* out, int32_t v)
{
	out[0] = (uint8_t)((uint32_t)v >> 8);
	out[1] = (uint8_t)v;
}

void p2io_jamma_read(p2io_t* p, uint8_t out[P2IO_JAMMA_REPORT])
{
	p2io_state* st = &p->st;
	const uint32_t p1 = st->in.pad[0], p2 = st->in.pad[1];
	unsigned i;

	memset(out, 0, P2IO_JAMMA_REPORT);

	/* The watchdog: a report whose bit has not flipped is a security
	 * error. */
	st->jamma ^= JAMMA_WATCHDOG;

	jamma_key(st, (p1 & P2IO_IN_TEST) != 0, JAMMA_TEST);
	jamma_key(st, (p1 & P2IO_IN_SERVICE) != 0, JAMMA_SERVICE);
	jamma_key(st, (p1 & P2IO_IN_COIN) != 0, JAMMA_COIN1);
	jamma_key(st, (p2 & P2IO_IN_COIN) != 0, JAMMA_COIN2);

	for (i = 0; i < 2; i++)
	{
		const uint32_t pad = st->in.pad[i];
#define KEY(in, j) jamma_key(st, (pad & (in)) != 0, i ? J_P2(j) : J_P1(j))
		switch (p->cfg.game)
		{
			case P2IO_GAME_DDR:
				KEY(P2IO_IN_START, J_START);
				KEY(P2IO_IN_LEFT, J_DDR_SEL_L);
				KEY(P2IO_IN_RIGHT, J_DDR_SEL_R);
				KEY(P2IO_DDR_UP, J_DDR_UP);
				KEY(P2IO_DDR_DOWN, J_DDR_DOWN);
				KEY(P2IO_DDR_LEFT, J_DDR_LEFT);
				KEY(P2IO_DDR_RIGHT, J_DDR_RIGHT);
				break;
			case P2IO_GAME_DANCE864:
				KEY(P2IO_IN_START, J_START);
				KEY(P2IO_IN_LEFT, J_D864_SEL_L);
				KEY(P2IO_IN_RIGHT, J_D864_SEL_R);
				if (st->stage[i].state == STAGE_INIT_DONE)
				{
					KEY(P2IO_D864_LEFT, J_D864_PAD_L);
					KEY(P2IO_D864_CENTER, J_D864_CENTER);
					KEY(P2IO_D864_RIGHT, J_D864_PAD_R);
				}
				break;
			case P2IO_GAME_TOYSMARCH:
				KEY(P2IO_IN_START, J_START);
				KEY(P2IO_IN_LEFT, J_TM_LEFT);
				KEY(P2IO_IN_RIGHT, J_TM_RIGHT);
				break;
			case P2IO_GAME_GUITARFREAKS:
			{
				const uint32_t e1 = i ? J_P2(J_GF_EFFECT1) : J_P1(J_GF_EFFECT1);
				const uint32_t e2 = i ? J_P2(J_GF_EFFECT2) : J_P1(J_GF_EFFECT2);
				KEY(P2IO_IN_START, J_START);
				KEY(P2IO_GF_PICK, J_GF_PICK);
				KEY(P2IO_GF_WAIL, J_GF_WAIL);
				KEY(P2IO_GF_RED, J_GF_RED);
				KEY(P2IO_GF_GREEN, J_GF_GREEN);
				KEY(P2IO_GF_BLUE, J_GF_BLUE);
				/* The effect knob stays where it was last turned. */
				if (pad & P2IO_GF_EFFECT1)
					st->knob[i] = 0;
				if (pad & P2IO_GF_EFFECT2)
					st->knob[i] = 1;
				if (pad & P2IO_GF_EFFECT3)
					st->knob[i] = 2;
				st->jamma |= e1 | e2;
				if (st->knob[i] == 0)
					st->jamma &= ~e1;
				else if (st->knob[i] == 1)
					st->jamma &= ~e2;
				else if (st->knob[i] == 2)
					st->jamma &= ~(e1 | e2);
				break;
			}
			default:
				break;
		}
#undef KEY
	}

	if (p->cfg.game == P2IO_GAME_DRUMMANIA)
	{
		jamma_key(st, (p1 & P2IO_IN_START) != 0, J_DM_START);
		jamma_key(st, (p1 & P2IO_DM_HIHAT) != 0, J_DM_HIHAT);
		jamma_key(st, (p1 & P2IO_DM_SNARE) != 0, J_DM_SNARE);
		jamma_key(st, (p1 & P2IO_DM_HIGH_TOM) != 0, J_DM_HIGH_TOM);
		jamma_key(st, (p1 & P2IO_DM_LOW_TOM) != 0, J_DM_LOW_TOM);
		jamma_key(st, (p1 & P2IO_DM_CYMBAL) != 0, J_DM_CYMBAL);
		jamma_key(st, (p1 & P2IO_DM_BASS) != 0, J_DM_BASS);
		jamma_key(st, (p1 & P2IO_IN_LEFT) != 0, J_DM_SEL_L);
		jamma_key(st, (p1 & P2IO_IN_RIGHT) != 0, J_DM_SEL_R);
	}
	else if (p->cfg.game == P2IO_GAME_THRILLDRIVE)
	{
		const p2io_sci* s = &st->sci[1];
		int32_t wheel = clamp_analog(st->in.wheel);
		jamma_key(st, (p1 & P2IO_IN_START) != 0, J_TD_START);
		jamma_key(st, (p1 & P2IO_TD_GEAR_DOWN) != 0, J_TD_GEAR_DOWN);
		jamma_key(st, (p1 & P2IO_TD_GEAR_UP) != 0, J_TD_GEAR_UP);
		/* While the game calibrates, the wheel follows the force it is
		 * driven with. */
		if (s->node[0] == P2IO_NODE_HANDLE && s->wheel_calibrating)
			wheel = clamp_analog(P2IO_ANALOG_MAX
				- (int32_t)((float)P2IO_ANALOG_MAX * ((float)s->wheel_ffb / 127.0f)));
		put_be16(out + 4, wheel);
		put_be16(out + 6, clamp_analog(st->in.accel));
		put_be16(out + 8, clamp_analog(st->in.brake));
	}

	/* Coins count on the board; the game reads them from it. */
	jamma_coin(st, JAMMA_COIN1, 0);
	jamma_coin(st, JAMMA_COIN2, 1);
	st->jamma_updates = (st->jamma_updates + 1) % 10;

	out[0] = (uint8_t)st->jamma;
	out[1] = (uint8_t)(st->jamma >> 8);
	out[2] = (uint8_t)(st->jamma >> 16);
	out[3] = (uint8_t)(st->jamma >> 24);
}

/* ---- files ------------------------------------------------------- */

static uint8_t dallas_crc8(const uint8_t* data, size_t len)
{
	uint8_t crc = 0xff;
	size_t i;
	unsigned bit;
	for (i = 0; i < len; i++)
	{
		uint8_t b = data[i];
		for (bit = 0; bit < 8; bit++)
		{
			const uint8_t mix = (uint8_t)((crc ^ b) & 1);
			crc >>= 1;
			if (mix)
				crc ^= 0x8c;
			b >>= 1;
		}
	}
	return crc;
}

int p2io_parse_dongle(const uint8_t in[40], uint8_t out[40])
{
	/* The payload's last byte is the complement of the CRC of the rest. */
	if ((uint8_t)~dallas_crc8(in, 0x1f) == in[0x1f])
	{
		memcpy(out, in + 0x20, 8);
		memcpy(out + 8, in, 0x20);
		return 1;
	}
	if ((uint8_t)~dallas_crc8(in + 8, 0x1f) == in[0x27])
	{
		memcpy(out, in, 40);
		return 1;
	}
	memset(out, 0, 40);
	return 0;
}

static int hex_digit(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

int p2io_parse_card(const char* text, size_t len, uint8_t out[8])
{
	size_t i = 0;
	unsigned k;
	while (i < len && (text[i] == ' ' || text[i] == '\t' || text[i] == '\r' || text[i] == '\n'))
		i++;
	if (len - i < 16)
		return 0;
	for (k = 0; k < 8; k++)
	{
		const int hi = hex_digit(text[i + 2 * k]);
		const int lo = hex_digit(text[i + 2 * k + 1]);
		if (hi < 0 || lo < 0)
			return 0;
		out[k] = (uint8_t)((hi << 4) | lo);
	}
	return 1;
}
