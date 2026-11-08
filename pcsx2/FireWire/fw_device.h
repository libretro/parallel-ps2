/* A device on the PS2's i.LINK bus, as the link layer (FW.c) sees it:
 * quadlet reads and block writes addressed to it, a bus reset, an event
 * it scheduled, and an audio tap. One device at most; FWsetDevice before
 * FWopen. */
#pragma once

#include "common/Pcsx2Types.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Where a node's configuration ROM starts. */
#define FW_CROM_BASE 0xfffff0000400ULL

typedef struct fw_device fw_device;
struct fw_device
{
	/* The machine was reset. */
	void (*reset)(fw_device* dev);
	/* A read the IOP sent the node; nonzero when answered. */
	int (*read_quadlet)(fw_device* dev, u64 offset, u32* value);
	/* A write the IOP sent the node, payload in host order. */
	int (*write)(fw_device* dev, u64 offset, const u32* payload, u32 quads);
	/* The event fw_host_schedule asked for came due. */
	void (*service)(fw_device* dev);
	/* One output sample of the SPU2's, to add to. */
	void (*mix)(fw_device* dev, s32* left, s32* right);
	/* Its savestate: a fixed size, written or read whole. read returns
	 * nonzero when the block was its own and loaded. */
	size_t (*state_size)(fw_device* dev);
	void (*state_write)(fw_device* dev, u8* block);
	int (*state_read)(fw_device* dev, const u8* block);
};

/* The device the next FWopen attaches; NULL for none. */
void FWsetDevice(fw_device* dev);

/* ---- what the link layer gives the device ------------------------- */

u32 fw_host_cycle(void);
/* Calls dev->service cycles from now; one event at a time. */
void fw_host_schedule(u32 cycles);
void fw_host_clear(void);
/* IOP RAM, by its physical address; zero when the range is outside it. */
int fw_host_iop_read(u32 address, void* data, u32 size);
int fw_host_iop_write(u32 address, const void* data, u32 size);

/* Async writes from the device to the PS2's node, queued and handed to
 * the IOP's receive buffer one packet at a time as it drains. A write to
 * offset 0x1000:xxxx also lands in IOP RAM at xxxx, as the receive DMA
 * puts it. Quads are in host order; bytes go out big endian. */
void fw_host_queue_quad(u32 offset_high, u32 offset_low, u32 payload);
void fw_host_queue_block(u32 offset_high, u32 offset_low, const u32* payload, u32 quads);
void fw_host_queue_bytes(u32 offset_high, u32 offset_low, const u8* payload, u32 bytes);
/* Hands the next queued packet over now, if the receive buffer is empty. */
void fw_host_flush(void);

#ifdef __cplusplus
}
#endif
