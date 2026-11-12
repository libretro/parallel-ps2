/* SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team */
/* SPDX-License-Identifier: GPL-3.0+ */

#pragma once

#include "videodev.h"
#include "jo_mpeg.h"
#include "cam-jpeg.h"

#include <retro_atomic.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The largest picture the camera hands over, and so the largest encoded
 * frame. */
#define EYETOY_MAX_FRAME_SIZE (640 * 480 * 3)

/* Both resample to the dst size by nearest neighbour; mirror flips left
 * and right. */
void eyetoy_xrgb_to_rgb(const u32* src, int src_w, int src_h, size_t src_pitch, u8* dst, int dst_w, int dst_h, bool mirror);
void eyetoy_yuyv_to_rgb(const u8* src, int src_w, int src_h, u8* dst, int dst_w, int dst_h, bool mirror);

/* What an encode needs, kept by whoever encodes rather than on the stack:
 * the MPEG writer's scratch, the JPEG Huffman tables, and a picture's RGB. */
typedef struct eyetoy_frame_encoder
{
	float mpeg_work[JO_MPEG_WORK_FLOATS];
	eyetoy_jpeg_tables jpeg;
	u8 rgb[EYETOY_MAX_FRAME_SIZE];
} eyetoy_frame_encoder;

/* Allocated with its tables built, when a device is created; NULL when
 * memory runs out. */
eyetoy_frame_encoder* eyetoy_encoder_create(void);
void eyetoy_encoder_free(eyetoy_frame_encoder* enc);

/* rgb is width x height RGB24; returns the encoded size in out, which
 * holds EYETOY_MAX_FRAME_SIZE. */
u32 eyetoy_encode_frame(eyetoy_frame_encoder* enc, u8* out, const u8* rgb, int width, int height, enum eyetoy_frame_format format);

/* A gradient, for when no camera delivers a picture; drawn in enc's rgb. */
u32 eyetoy_encode_test_pattern(eyetoy_frame_encoder* enc, u8* out, int width, int height, enum eyetoy_frame_format format);

/* One encoded frame handed from the thread that captures to the thread
 * that runs the device, without a lock: three buffers, of which the
 * producer owns one, the consumer owns one, and the third sits in a slot
 * both exchange. A frame published while the consumer is away is
 * overwritten by the next, so the consumer always takes the newest. Single
 * producer, single consumer. */
#define EYETOY_MAILBOX_FRESH 4

typedef struct eyetoy_frame_buffer
{
	u8* data;
	u32 size;
	u32 generation;
} eyetoy_frame_buffer;

typedef struct eyetoy_frame_mailbox
{
	eyetoy_frame_buffer buf[3];
	retro_atomic_int_t slot;
	int work;
	int hold;
} eyetoy_frame_mailbox;

/* Three buffers of EYETOY_MAX_FRAME_SIZE from one allocation; false if that
 * failed. */
bool eyetoy_mailbox_init(eyetoy_frame_mailbox* m);
void eyetoy_mailbox_free(eyetoy_frame_mailbox* m);
/* Nothing in flight: the consumer's view starts empty. Only while no
 * producer runs. */
void eyetoy_mailbox_reset(eyetoy_frame_mailbox* m);
/* Producer: where to encode into. */
u8* eyetoy_mailbox_work(const eyetoy_frame_mailbox* m);
/* Producer: what the work buffer holds is the frame, now visible to the
 * consumer. */
void eyetoy_mailbox_publish(eyetoy_frame_mailbox* m, u32 size, u32 generation);
/* Consumer: the newest frame published since the last take, or NULL. The
 * buffer stays the consumer's until the next take. */
const eyetoy_frame_buffer* eyetoy_mailbox_take(eyetoy_frame_mailbox* m);

/* 16-bit samples from the thread that captures to the thread that runs the
 * device, without a lock: a single-producer single-consumer ring. Full,
 * the producer drops what it has; the consumer trims a backlog itself. */
#define EYETOY_RING_SIZE 2048u
#define EYETOY_RING_MASK (EYETOY_RING_SIZE - 1)

typedef struct eyetoy_sample_ring
{
	s16 data[EYETOY_RING_SIZE];
	retro_atomic_int_t head; /* consumer's, next to read */
	retro_atomic_int_t tail; /* producer's, next to write */
} eyetoy_sample_ring;

void eyetoy_ring_reset(eyetoy_sample_ring* r);
/* Producer: room for how many more. */
u32 eyetoy_ring_space(eyetoy_sample_ring* r);
/* Producer: adds one; false and nothing added when full. */
bool eyetoy_ring_push(eyetoy_sample_ring* r, s16 sample);
/* Consumer: how many are waiting. */
u32 eyetoy_ring_count(eyetoy_sample_ring* r);
/* Consumer: drops all but the newest keep. */
void eyetoy_ring_trim_to(eyetoy_sample_ring* r, u32 keep);
/* Consumer: takes up to max; returns how many. */
u32 eyetoy_ring_pop(eyetoy_sample_ring* r, s16* samples, u32 max);

#ifdef __cplusplus
}
#endif
