/* SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team */
/* SPDX-License-Identifier: GPL-3.0+ */

#include "eyetoy-frontend.h"
#include "eyetoy-host.h"
#include "cam-frame.h"

#include <retro_atomic.h>

#include <stdlib.h>
#include <string.h>

#define MIC_RATE 16000
/* A game that starts or resumes listening hears the present: a backlog
 * past 75 ms is cut to 20 ms, which leaves room for a frame's worth of
 * samples arriving at once. */
#define MIC_TRIM_ABOVE 1200
#define MIC_TRIM_TO 320

/* What the device asked of the camera, in one word the main thread reads
 * and the emulator thread writes, so the two never need a lock:
 *   bit 0      wanted
 *   bits 1-2   format
 *   bit 3      mirror
 *   bits 4-9   width / 16   (the OV519 takes the width as a count of 16)
 *   bits 10-15 height / 8   (and the height as a count of 8)
 *   bits 16-30 generation, bumped by every open so an older frame is told
 *              apart */
#define CAM_WANTED 1
#define CAM_FORMAT_SHIFT 1
#define CAM_MIRROR (1 << 3)
#define CAM_W_SHIFT 4
#define CAM_H_SHIFT 10
#define CAM_GEN_SHIFT 16
#define CAM_GEN_MASK 0x7FFF

static int cam_config(bool wanted, enum eyetoy_frame_format format, bool mirror, int width, int height, u32 generation)
{
	return (wanted ? CAM_WANTED : 0) | ((int)format << CAM_FORMAT_SHIFT) | (mirror ? CAM_MIRROR : 0) |
		((width / 16) << CAM_W_SHIFT) | ((height / 8) << CAM_H_SHIFT) |
		((int)(generation & CAM_GEN_MASK) << CAM_GEN_SHIFT);
}

/* Shared between the main thread and the emulator's: the atomics, the
 * mailbox and the ring, each built for exactly that. */
static retro_atomic_int_t s_cam_config;
static eyetoy_frame_mailbox s_frames;
static retro_atomic_int_t s_mic_wanted;
static eyetoy_sample_ring s_ring;

/* Main thread only. */
static bool s_cam_available;
static struct retro_camera_callback s_cam;
static bool s_cam_running;
static bool s_cam_failed;
static eyetoy_frame_encoder* s_encoder;

static bool s_mic_available;
static struct retro_microphone_interface s_mic_if;
static retro_microphone_t* s_mic;
static unsigned s_mic_rate;
static bool s_mic_failed;
/* Resampling position in the current input chunk, in 1/MIC_RATE of a
 * frontend sample. */
static u64 s_mic_phase;

static void RETRO_CALLCONV on_camera_frame(const uint32_t* buffer, unsigned width, unsigned height, size_t pitch)
{
	const int config = retro_atomic_load_acquire_int(&s_cam_config);
	int w, h;
	enum eyetoy_frame_format format;
	u32 generation, size;
	if (!(config & CAM_WANTED) || !buffer || !width || !height)
		return;

	w = ((config >> CAM_W_SHIFT) & 0x3F) * 16;
	h = ((config >> CAM_H_SHIFT) & 0x3F) * 8;
	format = (enum eyetoy_frame_format)((config >> CAM_FORMAT_SHIFT) & 3);
	generation = (u32)(config >> CAM_GEN_SHIFT) & CAM_GEN_MASK;

	eyetoy_xrgb_to_rgb(buffer, (int)width, (int)height, pitch, s_encoder->rgb, w, h, (config & CAM_MIRROR) != 0);
	size = eyetoy_encode_frame(s_encoder, eyetoy_mailbox_work(&s_frames), s_encoder->rgb, w, h, format);
	eyetoy_mailbox_publish(&s_frames, size, generation);
}

/* ---- the frontend's camera ---- */

typedef struct frontend_video
{
	eyetoy_video_device base;
	/* Its own, as the camera's encoder belongs to the frontend's thread. */
	eyetoy_frame_encoder* enc;
	u8* pattern;
	u32 pattern_size;
	u32 generation;
	bool mirror;
} frontend_video;

static int frontend_video_open(eyetoy_video_device* dev, int width, int height, enum eyetoy_frame_format format, int mirror)
{
	frontend_video* v = (frontend_video*)dev;
	v->mirror = mirror != 0;
	v->generation = (v->generation + 1) & CAM_GEN_MASK;
	retro_atomic_store_release_int(&s_cam_config, cam_config(true, format, v->mirror, width, height, v->generation));
	/* Until the frontend's camera delivers, the game gets one picture so it
	 * sees the device working. */
	v->pattern_size = eyetoy_encode_test_pattern(v->enc, v->pattern, width, height, format);
	return 0;
}

static int frontend_video_close(eyetoy_video_device* dev)
{
	frontend_video* v = (frontend_video*)dev;
	retro_atomic_store_release_int(&s_cam_config, cam_config(false, EYETOY_FORMAT_MPEG, v->mirror, 16, 8, v->generation));
	return 0;
}

static int frontend_video_get_image(eyetoy_video_device* dev, u8* buf, size_t len)
{
	frontend_video* v = (frontend_video*)dev;
	const eyetoy_frame_buffer* frame = eyetoy_mailbox_take(&s_frames);
	u32 size = 0;
	if (frame && frame->generation == v->generation)
	{
		size = frame->size;
		v->pattern_size = 0;
		if (size > len)
			size = (u32)len;
		memcpy(buf, frame->data, size);
	}
	else if (v->pattern_size)
	{
		size = v->pattern_size < len ? v->pattern_size : (u32)len;
		memcpy(buf, v->pattern, size);
		v->pattern_size = 0;
	}
	return (int)size;
}

static void frontend_video_set_mirroring(eyetoy_video_device* dev, bool state)
{
	frontend_video* v = (frontend_video*)dev;
	const int config = retro_atomic_load_relaxed_int(&s_cam_config);
	v->mirror = state;
	retro_atomic_store_release_int(&s_cam_config, state ? (config | CAM_MIRROR) : (config & ~CAM_MIRROR));
}

static void frontend_video_destroy(eyetoy_video_device* dev)
{
	frontend_video* v = (frontend_video*)dev;
	frontend_video_close(dev);
	eyetoy_encoder_free(v->enc);
	free(v->pattern);
	free(v);
}

static const eyetoy_video_ops s_frontend_video_ops = {
	frontend_video_open, frontend_video_close, frontend_video_get_image,
	frontend_video_set_mirroring, frontend_video_destroy};

static eyetoy_video_device* frontend_video_create(void)
{
	frontend_video* v = (frontend_video*)calloc(1, sizeof(*v));
	if (!v)
		return NULL;
	v->base.ops = &s_frontend_video_ops;
	v->enc = eyetoy_encoder_create();
	v->pattern = (u8*)malloc(EYETOY_MAX_FRAME_SIZE);
	/* Past whatever a previous device published, so its frames are told
	 * apart. */
	v->generation = (u32)(retro_atomic_load_relaxed_int(&s_cam_config) >> CAM_GEN_SHIFT) & CAM_GEN_MASK;
	if (!v->enc || !v->pattern)
	{
		eyetoy_encoder_free(v->enc);
		free(v->pattern);
		free(v);
		return NULL;
	}
	return &v->base;
}

/* ---- no camera at all: the game still gets one picture, so it sees the
 * device working ---- */

typedef struct pattern_video
{
	eyetoy_video_device base;
	eyetoy_frame_encoder* enc;
	u8* frame;
	u32 size;
} pattern_video;

static int pattern_video_open(eyetoy_video_device* dev, int width, int height, enum eyetoy_frame_format format, int mirror)
{
	pattern_video* v = (pattern_video*)dev;
	(void)mirror;
	v->size = eyetoy_encode_test_pattern(v->enc, v->frame, width, height, format);
	return 0;
}

static int pattern_video_close(eyetoy_video_device* dev)
{
	(void)dev;
	return 0;
}

static int pattern_video_get_image(eyetoy_video_device* dev, u8* buf, size_t len)
{
	pattern_video* v = (pattern_video*)dev;
	const u32 size = v->size < len ? v->size : (u32)len;
	if (size)
		memcpy(buf, v->frame, size);
	v->size = 0;
	return (int)size;
}

static void pattern_video_set_mirroring(eyetoy_video_device* dev, bool state)
{
	(void)dev;
	(void)state;
}

static void pattern_video_destroy(eyetoy_video_device* dev)
{
	pattern_video* v = (pattern_video*)dev;
	eyetoy_encoder_free(v->enc);
	free(v->frame);
	free(v);
}

static const eyetoy_video_ops s_pattern_video_ops = {
	pattern_video_open, pattern_video_close, pattern_video_get_image,
	pattern_video_set_mirroring, pattern_video_destroy};

static eyetoy_video_device* pattern_video_create(void)
{
	pattern_video* v = (pattern_video*)calloc(1, sizeof(*v));
	if (!v)
		return NULL;
	v->base.ops = &s_pattern_video_ops;
	v->enc = eyetoy_encoder_create();
	v->frame = (u8*)malloc(EYETOY_MAX_FRAME_SIZE);
	if (!v->enc || !v->frame)
	{
		pattern_video_destroy(&v->base);
		return NULL;
	}
	return &v->base;
}

/* ---- the frontend's microphone ---- */

static u32 frontend_mic_read(eyetoy_mic* mic, s16* samples, u32 max)
{
	(void)mic;
	if (eyetoy_ring_count(&s_ring) > MIC_TRIM_ABOVE)
		eyetoy_ring_trim_to(&s_ring, MIC_TRIM_TO);
	return eyetoy_ring_pop(&s_ring, samples, max);
}

static void frontend_mic_destroy(eyetoy_mic* mic)
{
	retro_atomic_store_release_int(&s_mic_wanted, 0);
	free(mic);
}

static const eyetoy_mic_ops s_frontend_mic_ops = {frontend_mic_read, frontend_mic_destroy};

static eyetoy_mic* frontend_mic_create(void)
{
	eyetoy_mic* mic = (eyetoy_mic*)calloc(1, sizeof(*mic));
	if (!mic)
		return NULL;
	mic->ops = &s_frontend_mic_ops;
	retro_atomic_store_release_int(&s_mic_wanted, 1);
	return mic;
}

/* ---- the main thread's side ---- */

static void run_camera(void)
{
	const bool wanted = (retro_atomic_load_acquire_int(&s_cam_config) & CAM_WANTED) != 0;

	if (wanted && !s_cam_running && !s_cam_failed)
	{
		s_cam_running = s_cam.start && s_cam.start();
		s_cam_failed = !s_cam_running;
		if (s_cam_failed)
			log_cb(RETRO_LOG_WARN, "EyeToy: the frontend could not start its camera\n");
	}
	else if (!wanted)
	{
		if (s_cam_running && s_cam.stop)
			s_cam.stop();
		s_cam_running = false;
		s_cam_failed = false;
	}
}

static void run_microphone(void)
{
	const bool wanted = retro_atomic_load_acquire_int(&s_mic_wanted) != 0;
	/* 2 KB, the most a read hands over at once */
	s16 in[1024];
	int n;

	if (wanted && !s_mic && !s_mic_failed)
	{
		retro_microphone_params_t params;
		params.rate = MIC_RATE;
		s_mic = s_mic_if.open_mic(&params);
		if (s_mic && s_mic_if.get_params(s_mic, &params) && params.rate && s_mic_if.set_mic_state(s_mic, true))
		{
			s_mic_rate = params.rate;
			s_mic_phase = 0;
		}
		else
		{
			log_cb(RETRO_LOG_WARN, "EyeToy: the frontend could not open its microphone\n");
			if (s_mic)
				s_mic_if.close_mic(s_mic);
			s_mic = NULL;
			s_mic_failed = true;
		}
	}
	else if (!wanted)
	{
		if (s_mic)
			s_mic_if.close_mic(s_mic);
		s_mic = NULL;
		s_mic_failed = false;
	}
	if (!s_mic)
		return;

	while ((n = s_mic_if.read_mic(s_mic, in, C89_ARRAY_SIZE(in))) > 0)
	{
		/* Nearest-sample resampling from the frontend's rate to the
		 * EyeToy's. A full ring drops the rest of the chunk: the game is not
		 * listening, and when it does again it trims to the newest anyway. */
		const u64 end = (u64)n * MIC_RATE;
		for (; s_mic_phase < end; s_mic_phase += s_mic_rate)
		{
			if (!eyetoy_ring_push(&s_ring, in[s_mic_phase / MIC_RATE]))
			{
				s_mic_phase = end;
				break;
			}
		}
		s_mic_phase -= end;
		if (n < (int)C89_ARRAY_SIZE(in))
			break;
	}
}

void eyetoy_frontend_load(retro_environment_t env_cb)
{
	if (!s_encoder)
	{
		s_encoder = eyetoy_encoder_create();
		if (!eyetoy_mailbox_init(&s_frames))
			eyetoy_mailbox_free(&s_frames);
	}
	retro_atomic_int_init(&s_cam_config, 0);
	retro_atomic_int_init(&s_mic_wanted, 0);
	eyetoy_mailbox_reset(&s_frames);
	eyetoy_ring_reset(&s_ring);

	memset(&s_cam, 0, sizeof(s_cam));
	s_cam.caps = 1 << RETRO_CAMERA_BUFFER_RAW_FRAMEBUFFER;
	s_cam.width = 640;
	s_cam.height = 480;
	s_cam.frame_raw_framebuffer = on_camera_frame;
	s_cam_available = s_encoder && s_frames.buf[0].data &&
		env_cb(RETRO_ENVIRONMENT_GET_CAMERA_INTERFACE, &s_cam) && s_cam.start;

	memset(&s_mic_if, 0, sizeof(s_mic_if));
	s_mic_if.interface_version = RETRO_MICROPHONE_INTERFACE_VERSION;
	s_mic_available = env_cb(RETRO_ENVIRONMENT_GET_MICROPHONE_INTERFACE, &s_mic_if) &&
		s_mic_if.interface_version && s_mic_if.open_mic && s_mic_if.read_mic;

	log_cb(RETRO_LOG_INFO, "EyeToy: camera from the %s, microphone from the %s\n",
		s_cam_available ? "frontend" : "host", s_mic_available ? "frontend" : "host");
}

void eyetoy_frontend_run(void)
{
	if (s_cam_available)
		run_camera();
	if (s_mic_available)
		run_microphone();
}

void eyetoy_frontend_unload(void)
{
	if (s_cam_running && s_cam.stop)
		s_cam.stop();
	s_cam_running = false;
	s_cam_failed = false;
	s_cam_available = false;
	if (s_mic)
		s_mic_if.close_mic(s_mic);
	s_mic = NULL;
	s_mic_failed = false;
	s_mic_available = false;
}

eyetoy_video_device* eyetoy_video_create(void)
{
	eyetoy_video_device* dev;
	if (s_cam_available)
		return frontend_video_create();
	if ((dev = eyetoy_host_video_create()))
		return dev;
	return pattern_video_create();
}

eyetoy_mic* eyetoy_mic_create(void)
{
	if (s_mic_available)
		return frontend_mic_create();
	return eyetoy_host_mic_create();
}
