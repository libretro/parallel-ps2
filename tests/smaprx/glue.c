/* What the SMAP receive harness reaches inside the core: the SMAP's
 * registers, and a host adapter for the real net.c to drive (the real
 * sockets one where SMAPRX_REAL_SOCKETS is). Everything here is a
 * one-line forward; the driver model and the checks are in main.c. */
#include <stdlib.h>
#include <string.h>
#include <retro_atomic.h>
#include <rthreads/rthreads.h>

#include "DEV9/DEV9.h"
#include "DEV9/smap.h"
#include "DEV9/net.h"
#include "Config.h"

dev9Struct dev9;
static u16 s_eeprom[32];

static retro_atomic_int_t s_rxend_irqs;

/* What reaches the adapter from the PS2's side, and on which thread: it
 * belongs to the network thread, never the thread that sent. */
static uintptr_t          s_tx_caller;
static retro_atomic_int_t s_tx_frames, s_tx_resets, s_tx_on_caller, s_tx_out_of_order;

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
	emu_config_init(&EmuConfig);
	memset(&dev9, 0, sizeof(dev9));
	dev9.eeprom = s_eeprom; /* DEV9open maps it before the adapter sets the MAC */
	smap_write8(SMAP_R_RXFIFO_CTRL, SMAP_RXFIFO_RESET);
	for (i = 0; i < SMAP_BD_SIZE / 8; i++)
		smap_write16(SMAP_BD_RX_BASE + i * 8, SMAP_BD_RX_EMPTY);
	EmuConfig.DEV9.EthApi = EMU_NETAPI_SOCKETS;
	strlcpy(EmuConfig.DEV9.EthDevice, "Auto", sizeof(EmuConfig.DEV9.EthDevice));
	EmuConfig.DEV9.EthEnable = true;
	InitNet();
}

/* Whether the adapter came up (InitNet turns Ethernet off when not). */
int smaprx_net_up(void) { return EmuConfig.DEV9.EthEnable ? 1 : 0; }

/* A frame from the PS2, as the SMAP's transmit path hands it on: in two
 * runs, as a frame that wraps the TX FIFO comes, so every frame the
 * suite checks has crossed the join. */
void smaprx_tx(const unsigned char* frame, int len)
{
	const int head = len / 2;
	tx_put(frame, head, frame + head, len - head);
}

void smaprx_close(void) { TermNet(); }
void smaprx_reset(void) { ad_reset(); }
void smaprx_tx_from_here(void) { s_tx_caller = sthread_get_current_thread_id(); }
void smaprx_tx_stats(int* frames, int* resets, int* on_caller, int* out_of_order)
{
	*frames       = retro_atomic_load_acquire_int(&s_tx_frames);
	*resets       = retro_atomic_load_acquire_int(&s_tx_resets);
	*on_caller    = retro_atomic_load_acquire_int(&s_tx_on_caller);
	*out_of_order = retro_atomic_load_acquire_int(&s_tx_out_of_order);
}
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

#ifndef SMAPRX_REAL_SOCKETS
/* The host adapter, standing in for the sockets backend. */

/* Frames carry their index in their first byte. */
static bool stub_send(netif* nif, NetPacket* pkt)
{
	const int n = retro_atomic_load_acquire_int(&s_tx_frames);
	(void)nif;
	if (sthread_get_current_thread_id() == s_tx_caller)
		retro_atomic_inc_int(&s_tx_on_caller);
	if ((u8)pkt->buffer[0] != (u8)n)
		retro_atomic_inc_int(&s_tx_out_of_order);
	retro_atomic_store_release_int(&s_tx_frames, n + 1);
	return true;
}
static void stub_reset(netif* nif)
{
	(void)nif;
	if (sthread_get_current_thread_id() == s_tx_caller)
		retro_atomic_inc_int(&s_tx_on_caller);
	retro_atomic_inc_int(&s_tx_resets);
}
static void stub_nothing(netif* n) { (void)n; }
static void stub_destroy(netif* n)
{
	netif_deinit(n);
	free(n);
}

static bool stub_recv(netif* nif, NetPacket* pkt)
{
	const int n = smaprx_host_recv(pkt->buffer, (int)sizeof(pkt->buffer));
	(void)nif;
	if (n <= 0)
		return false;
	pkt->size = n;
	return true;
}
/* The stub's recv polls, as an adapter without a wait does. */
static const netif_ops stub_ops = {
	stub_recv, NULL, NULL, stub_send, stub_reset, stub_nothing, stub_nothing, stub_destroy};

netif* net_sockets_new(void)
{
	netif* n = (netif*)calloc(1, sizeof(*n));
	if (n)
		netif_init(n, &stub_ops);
	return n;
}
#endif
