/* The C++ the SMAP receive harness cannot avoid: the host adapter the
 * real net.cpp instantiates is a C++ class, and the SMAP's registers
 * are spelled in a C++ header. Everything here is a one-line forward;
 * the driver model and the checks are in main.c. */
#include <string.h>
#include <retro_atomic.h>

#include "DEV9/DEV9.h"
#include "DEV9/smap.h"
#include "DEV9/net.h"
#include "DEV9/sockets.h"
#include "Config.h"

dev9Struct dev9;
static u16 s_eeprom[32];

static retro_atomic_int_t s_rxend_irqs;

extern "C" {

/* main.c: the host's next frame, on the RX thread. Returns its length,
 * 0 when the host has nothing. */
int smaprx_host_recv(char* buf, int max);

void _DEV9irq(int cause, int cycles)
{
	(void)cycles;
	if (cause & SMAP_INTR_RXEND)
		retro_atomic_inc_int(&s_rxend_irqs);
}

int smaprx_rxend_irqs(void) { return retro_atomic_load_acquire_int(&s_rxend_irqs); }
int smaprx_bd_count(void) { return SMAP_BD_SIZE / 8; }
unsigned smaprx_bd_empty_flag(void) { return SMAP_BD_RX_EMPTY; }

void smaprx_open(void)
{
	int i;
	memset(&dev9, 0, sizeof(dev9));
	dev9.eeprom = s_eeprom; /* DEV9open maps it before the adapter sets the MAC */
	smap_write8(SMAP_R_RXFIFO_CTRL, SMAP_RXFIFO_RESET);
	for (i = 0; i < SMAP_BD_SIZE / 8; i++)
		smap_write16(SMAP_BD_RX_BASE + i * 8, SMAP_BD_RX_EMPTY);
	EmuConfig.DEV9.EthApi = Pcsx2Config::DEV9Options::NetApi::Sockets;
	EmuConfig.DEV9.EthDevice = "Auto";
	EmuConfig.DEV9.EthEnable = true;
	InitNet();
}

/* Whether the adapter came up (InitNet turns Ethernet off when not). */
int smaprx_net_up(void) { return EmuConfig.DEV9.EthEnable ? 1 : 0; }

/* A frame from the PS2, as the SMAP's transmit path hands it on. */
void smaprx_tx(const unsigned char* frame, int len)
{
	NetPacket pkt;
	memcpy(pkt.buffer, frame, len);
	pkt.size = len;
	tx_put(&pkt);
}

void smaprx_close(void) { TermNet(); }
void smaprx_tick(void) { smap_async(1); }
unsigned smaprx_frame_count(void) { return smap_read8(SMAP_R_RXFIFO_FRAME_CNT); }
unsigned smaprx_bd_read(int bd, int word) { return smap_read16(SMAP_BD_RX_BASE + bd * 8 + word * 2); }
void smaprx_bd_write(int bd, int word, unsigned v) { smap_write16(SMAP_BD_RX_BASE + bd * 8 + word * 2, (u16)v); }
unsigned smaprx_fifo_read32(void) { return smap_read32(SMAP_R_RXFIFO_DATA); }
void smaprx_frame_dec(void) { smap_write8(SMAP_R_RXFIFO_FRAME_DEC, 1); }

/* The FIFO pointer registers, 0 the TX write pointer and 1 the RX read
 * pointer, written the 16-bit way a guest can. */
static u32 fifo_ptr_reg(int which) { return which ? SMAP_R_RXFIFO_RD_PTR : SMAP_R_TXFIFO_WR_PTR; }
void smaprx_ptr_write16(int which, unsigned v) { smap_write16(fifo_ptr_reg(which), (u16)v); }
unsigned smaprx_ptr_read(int which) { return dev9Ru32(fifo_ptr_reg(which)); }
void smaprx_txfifo_write32(unsigned v) { smap_write32(SMAP_R_TXFIFO_DATA, v); }
unsigned smaprx_txfifo_byte(int i) { return dev9.txfifo[i]; }
void smaprx_rxfifo_set(int i, unsigned v) { dev9.rxfifo[i] = (u8)v; }
void smaprx_dma_write(unsigned* words, int bytes)
{
	dev9Ru16(SMAP_R_TXFIFO_CTRL) |= SMAP_TXFIFO_DMAEN;
	smap_writeDMA8Mem((u32*)words, bytes);
}
void smaprx_dma_read(unsigned* words, int bytes)
{
	dev9Ru16(SMAP_R_RXFIFO_CTRL) |= SMAP_RXFIFO_DMAEN;
	smap_readDMA8Mem((u32*)words, bytes);
}

} /* extern "C" */

#ifndef SMAPRX_REAL_SOCKETS
/* The host adapter, standing in for the sockets backend. */
SocketAdapter::SocketAdapter() { initialized = true; }
SocketAdapter::~SocketAdapter() {}
bool SocketAdapter::isInitialised() { return initialized; }
bool SocketAdapter::send(NetPacket* pkt) { (void)pkt; return true; }
void SocketAdapter::reset() {}
void SocketAdapter::reloadSettings() {}
void SocketAdapter::close() {}
/* The stub's recv polls, as the base adapter's wait does. */
void SocketAdapter::wait_recv() { NetAdapter::wait_recv(); }
void SocketAdapter::wake_recv() {}

bool SocketAdapter::recv(NetPacket* pkt)
{
	const int n = smaprx_host_recv(pkt->buffer, (int)sizeof(pkt->buffer));
	if (n <= 0)
		return false;
	pkt->size = n;
	return true;
}
#endif
