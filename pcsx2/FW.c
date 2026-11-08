/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2020  PCSX2 Dev Team
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

/* The i.LINK link layer. See FW.h. The link's model follows Reliquary's
 * (Florin9doi's i.LINK work and the Konami Python 1 additions), with its
 * buffers of fixed size. */

#include <stdlib.h>
#include <string.h>

#include <libretro.h>

#include "FW.h"
#include "IopHw.h"
#include "IopDma.h"
#include "R3000A.h"
#include "MemoryTypes.h"
#include "IopMem.h"

extern retro_log_printf_t log_cb;

/* ---- registers --------------------------------------------------- */

#define FW_BASE          0x1f808400u
#define FW_NODE_ID       (FW_BASE + 0x000)
#define FW_CTRL0         (FW_BASE + 0x008)
#define FW_CTRL0_Root      (1u << 19)
#define FW_CTRL0_BusIDRst  (1u << 23)
#define FW_CTRL0_RxRst     (1u << 24)
#define FW_CTRL0_TxRst     (1u << 25)
#define FW_CTRL2         (FW_BASE + 0x010)
#define FW_PHY_ACCESS    (FW_BASE + 0x014)
#define FW_PHY_ACCESS_WRITE (1u << 30)
#define FW_PHY_ACCESS_READ  (1u << 31)
#define FW_INTR0         (FW_BASE + 0x020)
#define FW_INTR0_DRFR      (1u << 0)
#define FW_INTR0_PBCntR    (1u << 9)
#define FW_INTR0_AckRcvd   (1u << 14)
#define FW_INTR0_URx       (1u << 22)
#define FW_INTR0_PhyRst    (1u << 29)
#define FW_INTR0_PhyRRx    (1u << 30)
#define FW_INTR0_MASK    (FW_BASE + 0x024)
#define FW_INTR1         (FW_BASE + 0x028)
#define FW_INTR1_UTD       (1u << 1)
#define FW_INTR1_MASK    (FW_BASE + 0x02c)
#define FW_INTR2         (FW_BASE + 0x030)
#define FW_INTR2_MASK    (FW_BASE + 0x034)
#define FW_ACK_STAT      (FW_BASE + 0x03c)
#define FW_UBUF_TX_NEXT  (FW_BASE + 0x040)
#define FW_UBUF_TX_LAST  (FW_BASE + 0x044)
#define FW_UBUF_TX_CLR   (FW_BASE + 0x048)
#define FW_UBUF_RX_CLR   (FW_BASE + 0x04c)
#define FW_UBUF_RX       (FW_BASE + 0x050)
#define FW_UBUF_RX_LVL   (FW_BASE + 0x054)
#define FW_REG_7C        (FW_BASE + 0x07c)
#define FW_PHT_CTRL0     (FW_BASE + 0x080)
#define FW_PHT_CTRL_EWREQ  (1u << 16)
#define FW_PHT_CTRL_ERREQ  (1u << 17)
#define FW_PHT_CTRL_PHTRst (1u << 21)
#define FW_DMA_CTRL0     (FW_BASE + 0x0b8)
#define FW_DBUF_FIFO_LV0 (FW_BASE + 0x0c0)
#define FW_DBUF_FIFO_RESET_TX (1u << 15)
#define FW_DBUF_FIFO_RESET_RX (1u << 31)
#define FW_DBUF_TX_DATA0 (FW_BASE + 0x0c4)
#define FW_DBUF_RX_DATA  (FW_BASE + 0x0c8)
#define FW_PHT_CTRL1     (FW_BASE + 0x100)
#define FW_DMA_CTRL1     (FW_BASE + 0x138)
#define FW_DBUF_FIFO_LV1 (FW_BASE + 0x140)
#define FW_DBUF_TX_DATA1 (FW_BASE + 0x144)

/* The register file, as the stub always had it: 64 KiB indexed by the
 * address's low half. */
static s8 fwregs[0x10000];
static u8 phyregs[16];

#define fwRu32(mem) (*(u32*)&fwregs[(mem)&0xffff])
#define PHYACC fwRu32(0x8414)

static void fw_irq(void)
{
	iopIntcIrq(24);
}

/* ---- the link, with a device on the bus ---------------------------- */

#define PHY_REG01_IBR      0x40
#define PHY_REG05_ISBR     0x40
#define PHY_REG05_EN_ACCL  0x02
#define PHY_REG05_EN_MULTI 0x01

#define TCODE_WRITEQ         0
#define TCODE_WRITEB         1
#define TCODE_WRITE_RESPONSE 2
#define TCODE_READQ          4
#define TCODE_READB          5
#define TCODE_READQ_RESPONSE 6

#define PHY_SELF_ID_PACKET    0xe1
#define REMOTE_PHY_ID         0u
#define LOCAL_PHY_ID          1u
#define REMOTE_RESPONSE_SPEED 2u
#define ACK_COMPLETE          1u
#define ACK_PEND              2u
#define PORT_NOT_CONNECTED    1u
#define PORT_PARENT           2u
#define PORT_CHILD            3u
#define LOCAL_NODE_ID         (0xffc0u | LOCAL_PHY_ID)
#define LOCAL_NODE_ID_REGISTER ((0x3ffu << 22) | (LOCAL_PHY_ID << 16) | 1u)

/* FIFO sizes, in quadlets. A transmitted PHT block is the IOP's own
 * command, a few dozen bytes; larger ones are cut short. */
#define UBUF_RX_QUADS 1024
#define UBUF_TX_QUADS 1024
#define DBUF_RX_QUADS 1024
#define PHT_TX_QUADS  1024

/* The device's queued writes, a packet at a time: kind, offset high and
 * low, byte count, then the payload padded to a quadlet. The board can
 * queue a whole 512 KiB boot ROM read before the IOP takes the first. */
#define PEND_BYTES (1280u * 1024u)
#define PEND_HEADER 16u

enum
{
	PEND_QUAD = 0,
	PEND_BLOCK, /* host-order quadlets */
	PEND_BYTES_BE
};

typedef struct fw_fifo
{
	u32 head, count;
} fw_fifo;

typedef struct fw_link
{
	fw_fifo ubuf_rx, ubuf_tx, dbuf_rx;
	u32 ubuf_rx_q[UBUF_RX_QUADS];
	u32 ubuf_tx_q[UBUF_TX_QUADS];
	u32 dbuf_rx_q[DBUF_RX_QUADS];
	u32 pht_tx_q[2][PHT_TX_QUADS];
	u32 pht_tx_count[2];
	u32 pht_tx_expected[2];
	u32 pht_write_pending[2];
	u32 pend_head, pend_tail;
} fw_link;

static fw_device* s_next_dev;
static fw_device* s_dev;
static fw_link* s_link;
static u8* s_pend;

void FWsetDevice(fw_device* dev)
{
	s_next_dev = dev;
}

static void fifo_clear(fw_fifo* f)
{
	f->head = f->count = 0;
}

static void fifo_push(fw_fifo* f, u32* q, u32 cap, u32 value)
{
	if (f->count >= cap)
		return; /* full: the IOP is not reading; the newest is dropped */
	q[(f->head + f->count) % cap] = value;
	f->count++;
}

static u32 fifo_pop(fw_fifo* f, const u32* q, u32 cap)
{
	u32 v;
	if (!f->count)
		return 0;
	v = q[f->head];
	f->head = (f->head + 1) % cap;
	f->count--;
	return v;
}

static void update_ubuf_rx_level(void)
{
	fwRu32(FW_UBUF_RX_LVL) = s_link->ubuf_rx.count;
}

static void update_dbuf_rx_level(void)
{
	fwRu32(FW_DBUF_FIFO_LV0) = (s_link->dbuf_rx.count * 4u) << 16;
}

static void raise_intr0(u32 bits)
{
	fwRu32(FW_INTR0) |= bits;
	if (fwRu32(FW_INTR0_MASK) & bits)
		fw_irq();
}

static void raise_intr1(u32 bits)
{
	fwRu32(FW_INTR1) |= bits;
	if (fwRu32(FW_INTR1_MASK) & bits)
		fw_irq();
}

static void maybe_raise_pending(void)
{
	if ((fwRu32(FW_INTR0) & fwRu32(FW_INTR0_MASK)) || (fwRu32(FW_INTR1) & fwRu32(FW_INTR1_MASK))
		|| (fwRu32(FW_INTR2) & fwRu32(FW_INTR2_MASK)))
		fw_irq();
}

static void ubuf_rx_push(u32 v)
{
	fifo_push(&s_link->ubuf_rx, s_link->ubuf_rx_q, UBUF_RX_QUADS, v);
	update_ubuf_rx_level();
}

static void dbuf_rx_push(u32 v)
{
	fifo_push(&s_link->dbuf_rx, s_link->dbuf_rx_q, DBUF_RX_QUADS, v);
	update_dbuf_rx_level();
}

/* ---- IOP memory --------------------------------------------------- */

/* A physical address in IOP RAM or one of its mirrors, as the virtual
 * kuseg/kseg0/kseg1 views give it. */
static int iop_ram_offset(u32 address, u32* off)
{
	const u32 phys = address & 0x1fffffffu;
	if (phys >= 0x00800000u)
		return 0;
	*off = phys & (PS2MEM_IOP_RAM - 1);
	return 1;
}

int fw_host_iop_read(u32 address, void* data, u32 size)
{
	u8* d = (u8*)data;
	while (size)
	{
		u32 off, n;
		if (!iop_ram_offset(address, &off))
			return 0;
		n = PS2MEM_IOP_RAM - off;
		if (n > size)
			n = size;
		memcpy(d, iopMem->Main + off, n);
		d += n;
		address += n;
		size -= n;
	}
	return 1;
}

int fw_host_iop_write(u32 address, const void* data, u32 size)
{
	const u8* s = (const u8*)data;
	while (size)
	{
		u32 off, n;
		if (!iop_ram_offset(address, &off))
			return 0;
		n = PS2MEM_IOP_RAM - off;
		if (n > size)
			n = size;
		memcpy(iopMem->Main + off, s, n);
		/* What lands in RAM may be module code: as CDVD DMA does. */
		if (psxCpu)
			psxCpu->Clear(off, (n + 3) / 4);
		s += n;
		address += n;
		size -= n;
	}
	return 1;
}

u32 fw_host_cycle(void)
{
	return psxRegs.cycle;
}

void fw_host_schedule(u32 cycles)
{
	if (cycles < 1)
		cycles = 1;
	if (cycles > 0x7fffffffu)
		cycles = 0x7fffffffu;
	PSX_INT(IopEvt_FW, (s32)cycles);
}

void fw_host_clear(void)
{
	psxRegs.interrupt &= ~(1u << IopEvt_FW);
}

/* ---- the device's queued writes ----------------------------------- */

static void put32(u8* p, u32 v)
{
	memcpy(p, &v, 4);
}

static u32 get32(const u8* p)
{
	u32 v;
	memcpy(&v, p, 4);
	return v;
}

static void pend_push(u32 kind, u32 hi, u32 lo, const void* payload, u32 bytes)
{
	const u32 padded = (bytes + 3u) & ~3u;
	const u32 need = PEND_HEADER + padded;
	u8* rec;

	if (!s_link)
		return;
	if (s_link->pend_tail + need > PEND_BYTES)
	{
		/* Close the gap the IOP has drained. */
		memmove(s_pend, s_pend + s_link->pend_head, s_link->pend_tail - s_link->pend_head);
		s_link->pend_tail -= s_link->pend_head;
		s_link->pend_head = 0;
		if (s_link->pend_tail + need > PEND_BYTES)
		{
			log_cb(RETRO_LOG_WARN, "FW: device write queue full, 0x%x bytes dropped\n", bytes);
			return;
		}
	}
	rec = s_pend + s_link->pend_tail;
	put32(rec, kind);
	put32(rec + 4, hi);
	put32(rec + 8, lo);
	put32(rec + 12, bytes);
	memset(rec + PEND_HEADER, 0, padded);
	if (bytes)
		memcpy(rec + PEND_HEADER, payload, bytes);
	s_link->pend_tail += need;
}

void fw_host_queue_quad(u32 offset_high, u32 offset_low, u32 payload)
{
	pend_push(PEND_QUAD, offset_high, offset_low, &payload, 4);
}

void fw_host_queue_block(u32 offset_high, u32 offset_low, const u32* payload, u32 quads)
{
	pend_push(PEND_BLOCK, offset_high, offset_low, payload, quads * 4);
}

void fw_host_queue_bytes(u32 offset_high, u32 offset_low, const u8* payload, u32 bytes)
{
	pend_push(PEND_BYTES_BE, offset_high, offset_low, payload, bytes);
}

/* The next queued packet into the DBUF receive buffer, once the IOP has
 * emptied it; a write to 0x1000:xxxx lands in IOP RAM as it goes. */
void fw_host_flush(void)
{
	const u8* rec;
	u32 kind, hi, lo, bytes, quads, i;

	if (!s_link || s_link->pend_head == s_link->pend_tail || s_link->dbuf_rx.count)
		return;
	if (s_link->pend_tail - s_link->pend_head < PEND_HEADER)
	{
		s_link->pend_head = s_link->pend_tail = 0;
		return;
	}
	rec   = s_pend + s_link->pend_head;
	kind  = get32(rec);
	hi    = get32(rec + 4);
	lo    = get32(rec + 8);
	bytes = get32(rec + 12);
	quads = (bytes + 3) / 4;
	if (bytes > s_link->pend_tail - s_link->pend_head - PEND_HEADER
		|| quads * 4 > s_link->pend_tail - s_link->pend_head - PEND_HEADER)
	{
		/* A record past the queue's end: nothing after it can be read. */
		s_link->pend_head = s_link->pend_tail = 0;
		return;
	}
	s_link->pend_head += PEND_HEADER + quads * 4;
	if (s_link->pend_head == s_link->pend_tail)
		s_link->pend_head = s_link->pend_tail = 0;

	if (kind != PEND_QUAD && hi == 0x1000 && bytes)
		fw_host_iop_write(lo, rec + PEND_HEADER, bytes);

	if (kind == PEND_QUAD)
	{
		dbuf_rx_push((0x3ffu << 22) | (REMOTE_RESPONSE_SPEED << 16) | (1u << 10) | (TCODE_WRITEQ << 4));
		dbuf_rx_push((LOCAL_NODE_ID << 16) | hi);
		dbuf_rx_push(lo);
		dbuf_rx_push(get32(rec + PEND_HEADER));
	}
	else
	{
		dbuf_rx_push((0x3ffu << 22) | (REMOTE_RESPONSE_SPEED << 16) | (TCODE_WRITEB << 4));
		dbuf_rx_push((LOCAL_NODE_ID << 16) | hi);
		dbuf_rx_push(lo);
		dbuf_rx_push(bytes << 16);
		for (i = 0; i < quads; i++)
		{
			const u8* p = rec + PEND_HEADER + i * 4;
			dbuf_rx_push(kind == PEND_BLOCK ? get32(p)
				: ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]);
		}
	}
	dbuf_rx_push(2);
	raise_intr0(FW_INTR0_URx);
}

static u32 dbuf_rx_pop(void)
{
	u32 v;
	if (!s_link->dbuf_rx.count)
		return 0;
	v = fifo_pop(&s_link->dbuf_rx, s_link->dbuf_rx_q, DBUF_RX_QUADS);
	update_dbuf_rx_level();
	if (!s_link->dbuf_rx.count)
		fw_host_flush();
	return v;
}

/* ---- PHY and bus reset -------------------------------------------- */

static u8 read_phy_register(u8 reg)
{
	/* Port status pages: port 0 connected, the rest not. */
	if (reg == 0x08 && ((phyregs[0x07] >> 5) & 7) == 0)
		return (phyregs[0x07] & 0xf) == 0 ? 0xae : 0x00;
	if (reg == 0x09 && ((phyregs[0x07] >> 5) & 7) == 0)
		return (phyregs[0x07] & 0xf) == 0 ? 0x40 : 0x00;
	return phyregs[reg];
}

static void init_phy_registers(void)
{
	memset(phyregs, 0, sizeof(phyregs));
	phyregs[0x00] = (u8)((LOCAL_PHY_ID << 2) | 0x03); /* physical ID, root, powered */
	phyregs[0x01] = 0x3f;                             /* gap count */
	phyregs[0x02] = 0x03;                             /* three ports */
	phyregs[0x03] = 0x40;                             /* S400 */
	phyregs[0x04] = 0x80;                             /* link active */
	phyregs[0x05] = PHY_REG05_EN_ACCL | PHY_REG05_EN_MULTI;
}

static u32 self_id_quad(u32 phy, u32 p0, u32 p1, u32 p2)
{
	return 0x80000000u | (phy << 24) | (1u << 22) | (0x3fu << 16) | (2u << 14) | (4u << 8)
		| (p0 << 6) | (p1 << 4) | (p2 << 2);
}

/* The PS2 becomes node 1 under the device's node 0. */
static void bus_reset(void)
{
	phyregs[0x00] = (u8)((LOCAL_PHY_ID << 2) | 0x03);
	fwRu32(FW_NODE_ID) = LOCAL_NODE_ID_REGISTER;
	fwRu32(FW_CTRL0) = (fwRu32(FW_CTRL0) | FW_CTRL0_Root) & ~FW_CTRL0_BusIDRst;

	fifo_clear(&s_link->ubuf_rx);
	fifo_clear(&s_link->dbuf_rx);
	update_ubuf_rx_level();
	dbuf_rx_push(PHY_SELF_ID_PACKET);
	dbuf_rx_push(self_id_quad(REMOTE_PHY_ID, PORT_PARENT, 0, 0));
	dbuf_rx_push(self_id_quad(LOCAL_PHY_ID, PORT_CHILD, PORT_NOT_CONNECTED, PORT_NOT_CONNECTED));
	dbuf_rx_push(1); /* end marker */
	raise_intr0(FW_INTR0_PhyRst | FW_INTR0_DRFR);
}

static void reset_link(void)
{
	init_phy_registers();
	memset(s_link, 0, sizeof(*s_link));
	psxRegs.interrupt &= ~(1u << IopEvt_FW);
	memset(fwregs, 0, sizeof(fwregs));
	fwRu32(FW_NODE_ID) = LOCAL_NODE_ID_REGISTER;
	fwRu32(FW_CTRL2)   = 0x8;
	update_ubuf_rx_level();
	update_dbuf_rx_level();
}

/* ---- transmit ----------------------------------------------------- */

static u32 bswap32(u32 v)
{
	return (v >> 24) | ((v >> 8) & 0xff00u) | ((v & 0xff00u) << 8) | (v << 24);
}

static void queue_read_response(u32 header, u32 dest_node, u32 value)
{
	const u32 tlabel = (header >> 10) & 0x3f;
	const u32 bus_id = dest_node >> 6;
	ubuf_rx_push((bus_id << 22) | (REMOTE_RESPONSE_SPEED << 16) | (tlabel << 10) | (1u << 8) | (TCODE_READQ_RESPONSE << 4));
	ubuf_rx_push(dest_node << 16);
	ubuf_rx_push(0);
	ubuf_rx_push(value);
	ubuf_rx_push(1);
	raise_intr0(FW_INTR0_URx);
}

static void ack_transmit(u32 ack)
{
	fwRu32(FW_ACK_STAT) = ack << 28;
	raise_intr0(FW_INTR0_AckRcvd);
}

static u32 ubuf_tx_at(u32 i)
{
	return s_link->ubuf_tx_q[(s_link->ubuf_tx.head + i) % UBUF_TX_QUADS];
}

/* A packet the IOP wrote to the user buffer, sent to the device. */
static void ubuf_transmit(void)
{
	const u32 n = s_link->ubuf_tx.count;
	u32 header, tcode;

	if (!n)
		return;
	header = ubuf_tx_at(0);
	tcode  = (header >> 4) & 0xf;
	if ((tcode == TCODE_WRITEQ || tcode == TCODE_WRITEB) && n >= 4)
	{
		const u64 offset = ((u64)(ubuf_tx_at(1) & 0xffff) << 32) | ubuf_tx_at(2);
		const u32 start  = tcode == TCODE_WRITEQ ? 3 : 4;
		const u32 quads  = n > start ? n - start : 0;
		if (quads)
		{
			u32* payload = (u32*)malloc(quads * 4);
			u32 i;
			if (payload)
			{
				for (i = 0; i < quads; i++)
					payload[i] = bswap32(ubuf_tx_at(start + i));
				if (s_dev->write(s_dev, offset, payload, quads))
					raise_intr0(FW_INTR0_PBCntR);
				free(payload);
			}
		}
		ack_transmit(ACK_COMPLETE);
	}
	else if ((tcode == TCODE_READQ || tcode == TCODE_READB) && n >= 3)
	{
		const u32 node   = ubuf_tx_at(1) >> 16;
		const u64 offset = ((u64)(ubuf_tx_at(1) & 0xffff) << 32) | ubuf_tx_at(2);
		u32 value = 0;
		s_dev->read_quadlet(s_dev, offset, &value);
		ack_transmit(ACK_PEND);
		queue_read_response(header, node, value);
	}
	raise_intr1(FW_INTR1_UTD);
	fifo_clear(&s_link->ubuf_tx);
	fw_host_flush();
}

/* A PHT channel's write, sent once its payload is all in. */
static void pht_try_write(int ch)
{
	const u32 base = ch == 0 ? FW_PHT_CTRL0 : FW_PHT_CTRL1;
	const u32 expected = s_link->pht_tx_expected[ch];
	u64 offset;
	u32 quads;

	if (!s_link->pht_write_pending[ch] || !expected || s_link->pht_tx_count[ch] * 4 < expected)
		return;
	offset = ((u64)(fwRu32(base + 0x08) & 0xffff) << 32) | fwRu32(base + 0x0c);
	quads  = (expected + 3) >> 2;
	s_dev->write(s_dev, offset, s_link->pht_tx_q[ch], quads);
	s_link->pht_write_pending[ch] = 0;
	s_link->pht_tx_expected[ch]   = 0;
	s_link->pht_tx_count[ch]      = 0;
	fwRu32(base + 0x24) = 0;
	raise_intr0(FW_INTR0_PBCntR);
}

static void pht_begin(int ch, u32 control)
{
	const u32 base = ch == 0 ? FW_PHT_CTRL0 : FW_PHT_CTRL1;
	if (!(control & FW_PHT_CTRL_EWREQ))
	{
		fwRu32(base + 0x24) = 0;
		raise_intr0(FW_INTR0_PBCntR);
		return;
	}
	s_link->pht_write_pending[ch] = 1;
	s_link->pht_tx_expected[ch]   = fwRu32(base + 0x10) & 0xffff;
	if (s_link->pht_tx_expected[ch] > PHT_TX_QUADS * 4)
		s_link->pht_tx_expected[ch] = PHT_TX_QUADS * 4;
	s_link->pht_tx_count[ch] = 0;
	pht_try_write(ch);
}

static void pht_data(int ch, u32 value)
{
	if (s_link->pht_tx_count[ch] < PHT_TX_QUADS)
		s_link->pht_tx_q[ch][s_link->pht_tx_count[ch]++] = value;
	fwRu32(ch == 0 ? FW_DBUF_FIFO_LV0 : FW_DBUF_FIFO_LV1) = s_link->pht_tx_count[ch] << 16;
	pht_try_write(ch);
}

static void service_device(void)
{
	if (s_dev && s_dev->service)
		s_dev->service(s_dev);
	else
		psxRegs.interrupt &= ~(1u << IopEvt_FW);
}

/* ---- the entry points --------------------------------------------- */

void FWopen(void)
{
	FWclose();
	memset(phyregs, 0, sizeof(phyregs));
	memset(fwregs, 0, sizeof(fwregs));
	s_dev = s_next_dev;
	if (!s_dev)
		return;
	s_link = (fw_link*)calloc(1, sizeof(fw_link));
	s_pend = (u8*)malloc(PEND_BYTES);
	if (!s_link || !s_pend)
	{
		log_cb(RETRO_LOG_ERROR, "FW: out of memory for the link\n");
		free(s_link);
		free(s_pend);
		s_link = NULL;
		s_pend = NULL;
		s_dev  = NULL;
		return;
	}
	reset_link();
}

void FWclose(void)
{
	memset(phyregs, 0, sizeof(phyregs));
	memset(fwregs, 0, sizeof(fwregs));
	if (s_link)
		psxRegs.interrupt &= ~(1u << IopEvt_FW);
	free(s_link);
	free(s_pend);
	s_link = NULL;
	s_pend = NULL;
	s_dev  = NULL;
}

void FWreset(void)
{
	if (!s_link)
		return;
	reset_link();
	if (s_dev->reset)
		s_dev->reset(s_dev);
	bus_reset();
}

void FWinterrupt(void)
{
	service_device();
}

void FWmixSubboardAudio(s32* left, s32* right)
{
	if (s_dev && s_dev->mix)
		s_dev->mix(s_dev, left, right);
}

void PHYWrite(void)
{
	u8 reg, data;
	int readback;

	if (!s_link)
	{
		reg  = (PHYACC >> 8) & 0xf;
		data = PHYACC & 0xff;
		phyregs[reg] = data;
		PHYACC &= ~0x4000ffff;
		return;
	}

	reg  = (PHYACC >> 24) & 0xf;
	data = (PHYACC >> 16) & 0xff;
	phyregs[reg] = data;
	if (reg == 0x01 && (data & PHY_REG01_IBR))
	{
		phyregs[reg] &= (u8)~PHY_REG01_IBR;
		bus_reset();
	}
	else if (reg == 0x05 && (data & PHY_REG05_ISBR))
	{
		phyregs[reg] = (u8)((phyregs[reg] & ~PHY_REG05_ISBR) | PHY_REG05_EN_ACCL | PHY_REG05_EN_MULTI);
		bus_reset();
	}
	readback = (PHYACC & FW_PHY_ACCESS_READ) != 0;
	PHYACC &= ~FW_PHY_ACCESS_WRITE;
	if (readback)
		PHYRead();
}

void PHYRead(void)
{
	u8 reg;

	if (!s_link)
	{
		reg = (PHYACC >> 24) & 0xf;
		PHYACC &= ~0x80000000;
		PHYACC |= phyregs[reg] | (reg << 8);
		if (fwRu32(0x8424) & 0x40000000) /* RRx interrupt mask */
		{
			fwRu32(0x8420) |= 0x40000000;
			fw_irq();
		}
		return;
	}

	reg = (PHYACC >> 24) & 0xf;
	PHYACC &= ~(FW_PHY_ACCESS_READ | 0xfffu);
	PHYACC |= read_phy_register(reg) | ((u32)reg << 8);
	raise_intr0(FW_INTR0_PhyRRx);
}

/* The stub's registers: what a console with nothing plugged in reads. */
static u32 stub_read32(u32 addr)
{
	switch (addr)
	{
		/* Node ID: the top is the default, the bottom a real console's. */
		case 0x1f808400:
			return 0xffc00001;
		/* A real console reads 0x10000001 here; a check against the node
		 * ID. */
		case 0x1f80847c:
			return 0x10000001;
		default:
			break;
	}
	return fwRu32(addr);
}

static void stub_write32(u32 addr, u32 value)
{
	switch (addr)
	{
		case 0x1f808414: /* PHY access: the R/W bit is cleared below */
			fwRu32(addr) = value;
			if (value & 0x40000000)
				PHYWrite();
			else if (value & 0x80000000)
				PHYRead();
			break;
		case 0x1f808408: /* Control 0 */
			fwRu32(addr) = value;
			fwRu32(addr) &= ~0x800000;
			break;
		case 0x1f808410: /* Control 2: SCLK OK, ready */
			fwRu32(addr) = 0x8;
			break;
		case 0x1f808420: /* interrupts: writing 1 clears */
		case 0x1f808428:
		case 0x1f808430:
			fwRu32(addr) &= ~value;
			break;
		default:
			fwRu32(addr) = value;
			break;
	}
}

u32 FWread32(u32 addr)
{
	if (!s_link)
		return stub_read32(addr);

	service_device();
	switch (addr)
	{
		case FW_UBUF_RX:
		{
			const u32 v = fifo_pop(&s_link->ubuf_rx, s_link->ubuf_rx_q, UBUF_RX_QUADS);
			update_ubuf_rx_level();
			return v;
		}
		case FW_UBUF_RX_LVL:
			update_ubuf_rx_level();
			return fwRu32(addr);
		case FW_DBUF_FIFO_LV0:
			update_dbuf_rx_level();
			return fwRu32(addr);
		case FW_DBUF_RX_DATA:
			return dbuf_rx_pop();
		case FW_REG_7C:
			return 0x10000001;
		default:
			return fwRu32(addr);
	}
}

void FWwrite32(u32 addr, u32 value)
{
	if (!s_link)
	{
		stub_write32(addr, value);
		return;
	}

	service_device();
	switch (addr)
	{
		case FW_NODE_ID:
			fwRu32(addr) = LOCAL_NODE_ID_REGISTER;
			break;
		case FW_PHY_ACCESS:
			fwRu32(addr) = value;
			if (value & FW_PHY_ACCESS_WRITE)
				PHYWrite();
			else if (value & FW_PHY_ACCESS_READ)
				PHYRead();
			break;
		case FW_CTRL0:
			fwRu32(addr) = (value & ~(FW_CTRL0_BusIDRst | FW_CTRL0_RxRst | FW_CTRL0_TxRst)) | FW_CTRL0_Root;
			break;
		case FW_CTRL2:
			fwRu32(addr) = 0x8 | (value & 0x2);
			break;
		case FW_INTR0:
		case FW_INTR1:
		case FW_INTR2:
			fwRu32(addr) &= ~value;
			break;
		case FW_INTR0_MASK:
		case FW_INTR1_MASK:
		case FW_INTR2_MASK:
			if (addr == FW_INTR0_MASK && value)
				value |= FW_INTR0_PhyRst;
			fwRu32(addr) = value;
			maybe_raise_pending();
			break;
		case FW_UBUF_TX_NEXT:
			fifo_push(&s_link->ubuf_tx, s_link->ubuf_tx_q, UBUF_TX_QUADS, value);
			fwRu32(addr) = value;
			break;
		case FW_UBUF_TX_LAST:
			fifo_push(&s_link->ubuf_tx, s_link->ubuf_tx_q, UBUF_TX_QUADS, value);
			fwRu32(addr) = value;
			ubuf_transmit();
			break;
		case FW_UBUF_TX_CLR:
			fifo_clear(&s_link->ubuf_tx);
			fwRu32(addr) = value;
			break;
		case FW_UBUF_RX_CLR:
			fifo_clear(&s_link->ubuf_rx);
			update_ubuf_rx_level();
			fwRu32(addr) = value;
			break;
		case FW_DBUF_FIFO_LV0:
			if (value & FW_DBUF_FIFO_RESET_RX)
				fifo_clear(&s_link->dbuf_rx);
			update_dbuf_rx_level();
			fwRu32(addr) |= value & FW_DBUF_FIFO_RESET_TX;
			break;
		case FW_PHT_CTRL0:
		case FW_PHT_CTRL1:
			fwRu32(addr) = value & ~FW_PHT_CTRL_PHTRst;
			if (value & (FW_PHT_CTRL_EWREQ | FW_PHT_CTRL_ERREQ))
				pht_begin(addr == FW_PHT_CTRL0 ? 0 : 1, value);
			break;
		case FW_DBUF_TX_DATA0:
			fwRu32(addr) = value;
			pht_data(0, value);
			break;
		case FW_DBUF_TX_DATA1:
			fwRu32(addr) = value;
			pht_data(1, value);
			break;
		default:
			fwRu32(addr) = value;
			break;
	}
}

/* ---- savestate ---------------------------------------------------- */

/* With a device on the bus: the link's registers (0x8400-0x85ff), the
 * PHY's, the link and its queue, the device's own; then size, version
 * and magic. With none there is no block, as there never was. */
#define FW_STATE_REGS    0x200
#define FW_STATE_VERSION 1
#define FW_STATE_MAGIC   "LRPS2FW1"

size_t FWStateSize(void)
{
	size_t n;
	if (!s_link)
		return 0;
	n = FW_STATE_REGS + sizeof(phyregs) + sizeof(fw_link) + PEND_BYTES
		+ (s_dev->state_size ? s_dev->state_size(s_dev) : 0);
	return (n + 15) / 16 * 16 + 16;
}

void FWStateWrite(void* dst)
{
	u8* p = (u8*)dst;
	const size_t size = FWStateSize();
	const u32 version = FW_STATE_VERSION, size32 = (u32)size;
	u8* w = p;

	if (!size)
		return;
	memset(p, 0, size);
	memcpy(w, fwregs + 0x8400, FW_STATE_REGS);
	w += FW_STATE_REGS;
	memcpy(w, phyregs, sizeof(phyregs));
	w += sizeof(phyregs);
	memcpy(w, s_link, sizeof(fw_link));
	w += sizeof(fw_link);
	memcpy(w, s_pend, PEND_BYTES);
	w += PEND_BYTES;
	if (s_dev->state_write)
		s_dev->state_write(s_dev, w);
	memcpy(p + size - 16, &size32, 4);
	memcpy(p + size - 12, &version, 4);
	memcpy(p + size - 8, FW_STATE_MAGIC, 8);
}

static const u8* fw_state_find(const void* state, size_t size)
{
	const size_t want = FWStateSize();
	const u8* end = (const u8*)state + size;
	u32 block, version;
	if (!want || size < want)
		return NULL;
	memcpy(&block, end - 16, 4);
	memcpy(&version, end - 12, 4);
	if (memcmp(end - 8, FW_STATE_MAGIC, 8) || block != want || version != FW_STATE_VERSION)
		return NULL;
	return end - want;
}

size_t FWStateStrip(const void* state, size_t size)
{
	return fw_state_find(state, size) ? size - FWStateSize() : size;
}

void FWStateRead(const void* state, size_t size)
{
	const u8* p = fw_state_find(state, size);
	fw_link* link;

	if (!s_link)
		return;
	if (!p)
	{
		/* A state from without this device: the bus starts over. */
		FWreset();
		return;
	}
	/* Checked before any of it is taken: lengths past their buffers are a
	 * bad block, not something to run. */
	link = (fw_link*)malloc(sizeof(fw_link));
	if (!link)
	{
		FWreset();
		return;
	}
	memcpy(link, p + FW_STATE_REGS + sizeof(phyregs), sizeof(fw_link));
	if (link->ubuf_rx.count > UBUF_RX_QUADS || link->ubuf_tx.count > UBUF_TX_QUADS
		|| link->dbuf_rx.count > DBUF_RX_QUADS || link->ubuf_rx.head >= UBUF_RX_QUADS
		|| link->ubuf_tx.head >= UBUF_TX_QUADS || link->dbuf_rx.head >= DBUF_RX_QUADS
		|| link->pht_tx_count[0] > PHT_TX_QUADS || link->pht_tx_count[1] > PHT_TX_QUADS
		|| link->pend_head > link->pend_tail || link->pend_tail > PEND_BYTES
		|| (s_dev->state_read && !s_dev->state_read(s_dev, p + FW_STATE_REGS + sizeof(phyregs)
			+ sizeof(fw_link) + PEND_BYTES)))
	{
		free(link);
		log_cb(RETRO_LOG_WARN, "FW: state not loaded; the bus starts over\n");
		FWreset();
		return;
	}
	memcpy(fwregs + 0x8400, p, FW_STATE_REGS);
	memcpy(phyregs, p + FW_STATE_REGS, sizeof(phyregs));
	memcpy(s_link, link, sizeof(fw_link));
	memcpy(s_pend, p + FW_STATE_REGS + sizeof(phyregs) + sizeof(fw_link), PEND_BYTES);
	free(link);
	update_ubuf_rx_level();
	update_dbuf_rx_level();
}
