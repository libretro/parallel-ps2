/* SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team */
/* SPDX-License-Identifier: GPL-3.0+ */

#include "eyetoy-host.h"

#if defined(__linux__) && !defined(__ANDROID__)

#include "cam-frame.h"

#include <rthreads/rthreads.h>
#include <retro_atomic.h>

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <linux/videodev2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <unistd.h>

#include <compat/strl.h>

static int xioctl(int fd, unsigned long request, void* arg)
{
	int r;
	do
		r = ioctl(fd, request, arg);
	while (r == -1 && errno == EINTR);
	return r;
}

/* The first node that streams video capture: a UVC camera's metadata node,
 * or a codec, does not. */
static int open_capture_device(void)
{
	int index;
	for (index = 0; index < 64; index++)
	{
		char name[32];
		struct v4l2_capability cap;
		int fd;
		snprintf(name, sizeof(name), "/dev/video%d", index);
		fd = open(name, O_RDWR | O_NONBLOCK, 0);
		if (fd < 0)
			continue;
		memset(&cap, 0, sizeof(cap));
		if (xioctl(fd, VIDIOC_QUERYCAP, &cap) == 0)
		{
			const u32 caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
			if ((caps & V4L2_CAP_VIDEO_CAPTURE) && (caps & V4L2_CAP_STREAMING))
			{
				log_cb(RETRO_LOG_INFO, "EyeToy: camera %s (%s)\n", name, (const char*)cap.card);
				return fd;
			}
		}
		close(fd);
	}
	return -1;
}

/* The most capture buffers mapped at once; a driver that insists on more
 * is not used. */
#define V4L2_MAX_BUFFERS 8

typedef struct v4l2_video
{
	eyetoy_video_device base;
	sthread_t* thread;
	retro_atomic_int_t running;
	retro_atomic_int_t mirror;
	int fd;
	struct
	{
		void* start;
		size_t length;
	} buffers[V4L2_MAX_BUFFERS];
	u32 nbuffers;
	int width, height;
	int capture_width, capture_height;
	enum eyetoy_frame_format format;
	/* The capture thread's; the RGB conversion of a capture is its rgb. */
	eyetoy_frame_encoder* enc;
	/* The emulator thread's, for the picture before the first capture. */
	eyetoy_frame_encoder* pattern_enc;
	u8* pattern;
	u32 pattern_size;
	eyetoy_frame_mailbox frames;
} v4l2_video;

static void v4l2_stop_capture(v4l2_video* v)
{
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	u32 i;
	if (v->fd < 0)
		return;
	xioctl(v->fd, VIDIOC_STREAMOFF, &type);
	for (i = 0; i < v->nbuffers; i++)
		munmap(v->buffers[i].start, v->buffers[i].length);
	v->nbuffers = 0;
	close(v->fd);
	v->fd = -1;
}

static bool v4l2_start_capture(v4l2_video* v)
{
	struct v4l2_format fmt;
	struct v4l2_requestbuffers req;
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	u32 i;

	v->fd = open_capture_device();
	if (v->fd < 0)
	{
		log_cb(RETRO_LOG_WARN, "EyeToy: no camera found\n");
		return false;
	}

	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt.fmt.pix.width = (u32)v->width;
	fmt.fmt.pix.height = (u32)v->height;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
	if (xioctl(v->fd, VIDIOC_S_FMT, &fmt) == -1 || fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV)
	{
		log_cb(RETRO_LOG_WARN, "EyeToy: the camera does not offer YUYV\n");
		return false;
	}
	/* Cameras that cannot capture the game's size give another; frames are
	 * scaled to fit. */
	v->capture_width = (int)fmt.fmt.pix.width;
	v->capture_height = (int)fmt.fmt.pix.height;

	memset(&req, 0, sizeof(req));
	req.count = 4;
	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	req.memory = V4L2_MEMORY_MMAP;
	if (xioctl(v->fd, VIDIOC_REQBUFS, &req) == -1 || req.count < 2 || req.count > V4L2_MAX_BUFFERS)
		return false;
	for (i = 0; i < req.count; i++)
	{
		struct v4l2_buffer buf;
		void* start;
		memset(&buf, 0, sizeof(buf));
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = i;
		if (xioctl(v->fd, VIDIOC_QUERYBUF, &buf) == -1)
			return false;
		start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, v->fd, buf.m.offset);
		if (start == MAP_FAILED)
			return false;
		v->buffers[v->nbuffers].start = start;
		v->buffers[v->nbuffers].length = buf.length;
		v->nbuffers++;
		if (xioctl(v->fd, VIDIOC_QBUF, &buf) == -1)
			return false;
	}
	return xioctl(v->fd, VIDIOC_STREAMON, &type) == 0;
}

static void v4l2_capture_thread(void* arg)
{
	v4l2_video* v = (v4l2_video*)arg;
	while (retro_atomic_load_acquire_int(&v->running))
	{
		fd_set fds;
		struct timeval timeout;
		struct v4l2_buffer buf;
		int ret;
		FD_ZERO(&fds);
		FD_SET(v->fd, &fds);
		timeout.tv_sec = 0;
		timeout.tv_usec = 100000;
		ret = select(v->fd + 1, &fds, NULL, NULL, &timeout);
		if (ret < 0 && errno != EINTR)
			break;
		if (ret <= 0)
			continue;

		memset(&buf, 0, sizeof(buf));
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		if (xioctl(v->fd, VIDIOC_DQBUF, &buf) == -1)
		{
			if (errno == EAGAIN)
				continue;
			log_cb(RETRO_LOG_WARN, "EyeToy: the camera stopped (%s)\n", strerror(errno));
			break;
		}
		if (buf.index < v->nbuffers && buf.bytesused >= (u32)(v->capture_width * v->capture_height * 2))
		{
			u32 size;
			eyetoy_yuyv_to_rgb((const u8*)v->buffers[buf.index].start, v->capture_width, v->capture_height,
				v->enc->rgb, v->width, v->height, retro_atomic_load_acquire_int(&v->mirror) != 0);
			size = eyetoy_encode_frame(v->enc, eyetoy_mailbox_work(&v->frames), v->enc->rgb, v->width, v->height, v->format);
			eyetoy_mailbox_publish(&v->frames, size, 0);
		}
		xioctl(v->fd, VIDIOC_QBUF, &buf);
	}
}

static int v4l2_close(eyetoy_video_device* dev)
{
	v4l2_video* v = (v4l2_video*)dev;
	if (v->thread)
	{
		retro_atomic_store_release_int(&v->running, 0);
		sthread_join(v->thread);
		v->thread = NULL;
	}
	v4l2_stop_capture(v);
	return 0;
}

static int v4l2_open(eyetoy_video_device* dev, int width, int height, enum eyetoy_frame_format format, int mirror)
{
	v4l2_video* v = (v4l2_video*)dev;
	v4l2_close(dev);

	v->width = width;
	v->height = height;
	v->format = format;
	retro_atomic_store_release_int(&v->mirror, mirror);
	/* No capture thread runs here, so the mailbox is the device's to
	 * clear. */
	eyetoy_mailbox_reset(&v->frames);
	/* Until the camera delivers, the game gets one picture so it sees the
	 * device working. */
	v->pattern_size = eyetoy_encode_test_pattern(v->pattern_enc, v->pattern, width, height, format);

	if (!v4l2_start_capture(v))
	{
		v4l2_stop_capture(v);
		return -1;
	}
	retro_atomic_store_release_int(&v->running, 1);
	v->thread = sthread_create(v4l2_capture_thread, v);
	if (!v->thread)
	{
		retro_atomic_store_release_int(&v->running, 0);
		v4l2_stop_capture(v);
		return -1;
	}
	return 0;
}

static int v4l2_get_image(eyetoy_video_device* dev, u8* buf, size_t len)
{
	v4l2_video* v = (v4l2_video*)dev;
	const eyetoy_frame_buffer* frame = eyetoy_mailbox_take(&v->frames);
	u32 size = 0;
	if (frame)
	{
		size = frame->size < len ? frame->size : (u32)len;
		memcpy(buf, frame->data, size);
		v->pattern_size = 0;
	}
	else if (v->pattern_size)
	{
		size = v->pattern_size < len ? v->pattern_size : (u32)len;
		memcpy(buf, v->pattern, size);
		v->pattern_size = 0;
	}
	return (int)size;
}

static void v4l2_set_mirroring(eyetoy_video_device* dev, bool state)
{
	v4l2_video* v = (v4l2_video*)dev;
	retro_atomic_store_release_int(&v->mirror, state);
}

static void v4l2_destroy(eyetoy_video_device* dev)
{
	v4l2_video* v = (v4l2_video*)dev;
	v4l2_close(dev);
	eyetoy_mailbox_free(&v->frames);
	eyetoy_encoder_free(v->enc);
	eyetoy_encoder_free(v->pattern_enc);
	free(v->pattern);
	free(v);
}

static const eyetoy_video_ops s_v4l2_ops = {
	v4l2_open, v4l2_close, v4l2_get_image, v4l2_set_mirroring, v4l2_destroy};

eyetoy_video_device* eyetoy_host_video_create(void)
{
	v4l2_video* v = (v4l2_video*)calloc(1, sizeof(*v));
	if (!v)
		return NULL;
	v->base.ops = &s_v4l2_ops;
	v->fd = -1;
	retro_atomic_int_init(&v->running, 0);
	retro_atomic_int_init(&v->mirror, 0);
	/* Everything the device uses, now rather than when the game opens it. */
	v->enc = eyetoy_encoder_create();
	v->pattern_enc = eyetoy_encoder_create();
	v->pattern = (u8*)malloc(EYETOY_MAX_FRAME_SIZE);
	if (!v->enc || !v->pattern_enc || !v->pattern || !eyetoy_mailbox_init(&v->frames))
	{
		eyetoy_encoder_free(v->enc);
		eyetoy_encoder_free(v->pattern_enc);
		free(v->pattern);
		free(v);
		return NULL;
	}
	return &v->base;
}

/* libasound is loaded when a microphone is wanted, so the core does not
 * need it to run. */
typedef struct snd_pcm_t snd_pcm_t;
typedef struct alsa_api
{
	int (*pcm_open)(snd_pcm_t**, const char*, int, int);
	int (*pcm_set_params)(snd_pcm_t*, int, int, unsigned int, unsigned int, int, unsigned int);
	int (*pcm_start)(snd_pcm_t*);
	long (*pcm_readi)(snd_pcm_t*, void*, unsigned long);
	int (*pcm_recover)(snd_pcm_t*, int, int);
	int (*pcm_close)(snd_pcm_t*);
	const char* (*strerror)(int);
} alsa_api;

/* Stable ALSA ABI values. */
#define SND_PCM_STREAM_CAPTURE 1
#define SND_PCM_NONBLOCK 1
#define SND_PCM_ACCESS_RW_INTERLEAVED 3
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define SND_PCM_FORMAT_S16 3
#else
#define SND_PCM_FORMAT_S16 2
#endif

/* dlsym's object pointer, as a function pointer: the cast POSIX allows. */
#define ALSA_SYM(field, name) (*(void**)&alsa.field = dlsym(lib, name))

static const alsa_api* load_alsa(void)
{
	static alsa_api alsa;
	static bool tried, loaded;
	void* lib;
	if (tried)
		return loaded ? &alsa : NULL;
	tried = true;
	lib = dlopen("libasound.so.2", RTLD_NOW | RTLD_LOCAL);
	if (!lib)
		return NULL;
	ALSA_SYM(pcm_open, "snd_pcm_open");
	ALSA_SYM(pcm_set_params, "snd_pcm_set_params");
	ALSA_SYM(pcm_start, "snd_pcm_start");
	ALSA_SYM(pcm_readi, "snd_pcm_readi");
	ALSA_SYM(pcm_recover, "snd_pcm_recover");
	ALSA_SYM(pcm_close, "snd_pcm_close");
	ALSA_SYM(strerror, "snd_strerror");
	loaded = alsa.pcm_open && alsa.pcm_set_params && alsa.pcm_start && alsa.pcm_readi && alsa.pcm_recover &&
		alsa.pcm_close && alsa.strerror;
	if (!loaded)
		dlclose(lib);
	return loaded ? &alsa : NULL;
}

/* What the search for a microphone needs, on the heap rather than a
 * frame: a handful of paths. */
#define HOST_MAX_CAMERAS 16
typedef struct host_mic_search
{
	char cameras[HOST_MAX_CAMERAS][PATH_MAX];
	int ncameras;
	char path[PATH_MAX];
	char usb[PATH_MAX];
	char usb_microphone[64];
	char name[64];
} host_mic_search;

/* The USB device behind a sysfs link, into out; false when there is none.
 * The link names a USB interface, and its parent is the device. */
static bool usb_device_of(const char* link, char* out, size_t out_size, bool* is_usb)
{
	char* real = realpath(link, NULL);
	if (!real)
		return false;
	if (is_usb)
		*is_usb = strstr(real, "/usb") != NULL;
	strlcpy(out, dirname(real), out_size);
	free(real);
	return true;
}

static bool path_exists(const char* path)
{
	struct stat st;
	return stat(path, &st) == 0;
}

/* The microphone on the same USB device as a camera, else any USB
 * microphone; into s->name, empty when none. */
static void find_capture_device(host_mic_search* s)
{
	DIR* d;
	int card, i;
	s->name[0] = '\0';
	s->usb_microphone[0] = '\0';
	s->ncameras = 0;
	if ((d = opendir("/sys/class/video4linux")))
	{
		struct dirent* e;
		while ((e = readdir(d)) && s->ncameras < HOST_MAX_CAMERAS)
		{
			if (e->d_name[0] == '.')
				continue;
			snprintf(s->path, sizeof(s->path), "/sys/class/video4linux/%s/device", e->d_name);
			if (usb_device_of(s->path, s->cameras[s->ncameras], sizeof(s->cameras[0]), NULL))
				s->ncameras++;
		}
		closedir(d);
	}

	for (card = 0; card < 32; card++)
	{
		int pcm = 0;
		bool is_usb = false;
		for (; pcm < 32; pcm++)
		{
			snprintf(s->path, sizeof(s->path), "/proc/asound/card%d/pcm%dc", card, pcm);
			if (path_exists(s->path))
				break;
		}
		if (pcm == 32)
			continue;

		snprintf(s->path, sizeof(s->path), "/sys/class/sound/card%d/device", card);
		if (!usb_device_of(s->path, s->usb, sizeof(s->usb), &is_usb))
			continue;
		snprintf(s->name, sizeof(s->name), "plughw:%d,%d", card, pcm);
		for (i = 0; i < s->ncameras; i++)
		{
			if (!strcmp(s->cameras[i], s->usb))
				return;
		}
		if (!s->usb_microphone[0] && is_usb)
			strlcpy(s->usb_microphone, s->name, sizeof(s->usb_microphone));
	}
	strlcpy(s->name, s->usb_microphone, sizeof(s->name));
}

typedef struct alsa_mic
{
	eyetoy_mic base;
	const alsa_api* alsa;
	snd_pcm_t* pcm;
} alsa_mic;

/* Non-blocking reads as the game asks; an overrun from a game that stopped
 * listening restarts the stream rather than playing back what it missed. */
static u32 alsa_mic_read(eyetoy_mic* mic, s16* samples, u32 max)
{
	alsa_mic* m = (alsa_mic*)mic;
	const long n = m->alsa->pcm_readi(m->pcm, samples, max);
	if (n >= 0)
		return (u32)n;
	if (n != -EAGAIN && m->alsa->pcm_recover(m->pcm, (int)n, 1) == 0)
		m->alsa->pcm_start(m->pcm);
	return 0;
}

static void alsa_mic_destroy(eyetoy_mic* mic)
{
	alsa_mic* m = (alsa_mic*)mic;
	m->alsa->pcm_close(m->pcm);
	free(m);
}

static const eyetoy_mic_ops s_alsa_mic_ops = {alsa_mic_read, alsa_mic_destroy};

eyetoy_mic* eyetoy_host_mic_create(void)
{
	host_mic_search* s = (host_mic_search*)malloc(sizeof(*s));
	const alsa_api* alsa;
	snd_pcm_t* pcm = NULL;
	alsa_mic* m;
	int err;

	if (!s)
		return NULL;
	find_capture_device(s);
	if (!s->name[0])
	{
		log_cb(RETRO_LOG_INFO, "EyeToy: no microphone found\n");
		free(s);
		return NULL;
	}
	alsa = load_alsa();
	if (!alsa)
	{
		log_cb(RETRO_LOG_WARN, "EyeToy: libasound.so.2 is not available\n");
		free(s);
		return NULL;
	}
	err = alsa->pcm_open(&pcm, s->name, SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK);
	if (err == 0)
	{
		err = alsa->pcm_set_params(pcm, SND_PCM_FORMAT_S16, SND_PCM_ACCESS_RW_INTERLEAVED, 1, 16000, 1, 100000);
		if (err == 0)
			err = alsa->pcm_start(pcm);
		if (err != 0)
			alsa->pcm_close(pcm);
	}
	if (err != 0)
	{
		log_cb(RETRO_LOG_WARN, "EyeToy: cannot capture from %s: %s\n", s->name, alsa->strerror(err));
		free(s);
		return NULL;
	}
	log_cb(RETRO_LOG_INFO, "EyeToy: microphone %s\n", s->name);
	free(s);
	m = (alsa_mic*)calloc(1, sizeof(*m));
	if (!m)
	{
		alsa->pcm_close(pcm);
		return NULL;
	}
	m->base.ops = &s_alsa_mic_ops;
	m->alsa = alsa;
	m->pcm = pcm;
	return &m->base;
}

#else

eyetoy_video_device* eyetoy_host_video_create(void)
{
	return NULL;
}

eyetoy_mic* eyetoy_host_mic_create(void)
{
	return NULL;
}

#endif
