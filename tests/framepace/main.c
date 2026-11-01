/* framepace: what a retro_run hands over, and what a savestate holds,
 * whatever the frontend does between two retro_runs and however the
 * core's threads happen to be scheduled.
 *
 * The core is driven through its libretro exports on a synthetic BIOS
 * image (the EE turns on a display and feeds the GIF forever, as in
 * tests/secondload) with the software renderer, so every frame comes
 * back as pixels. Every callback, and every call into the core, waits a
 * random fraction of a millisecond first, so the EE, GS and frontend
 * threads meet in a different order on every pass.
 *
 * What must hold:
 *   - every retro_run presents exactly one frame, and nothing is
 *     presented outside one - not from retro_serialize, retro_unserialize,
 *     retro_reset or an option change;
 *   - no retro_run hands over more than one frame's audio;
 *   - from a savestate, running on gives the same frames, the same audio
 *     sample for sample, and the same savestate at the end, as running
 *     on from where it was taken - straight, with a savestate taken
 *     before every frame, with every frame run twice and rolled back
 *     between (as runahead and netplay do, also with the frontend
 *     saying the state stays in this instance), with a state the core
 *     rejects partway offered before every frame (cut short, so its
 *     memory blocks are only partly there, in either context), and with option changes
 *     between frames (audio only: an option may change the picture).
 *
 * Usage: framepace <path-to-core> <scratch-dir> */

#ifndef _WIN32
#define _POSIX_C_SOURCE 199309L /* nanosleep */
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include <libretro.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#define FP_HANDLE HMODULE
#define fp_dlopen(p) LoadLibraryA(p)
#define fp_mkdir(p) _mkdir(p)
#define fp_sleep_us(us) Sleep((DWORD)((us) / 1000))
#else
#include <dlfcn.h>
#include <time.h>
#include <sys/stat.h>
#define FP_HANDLE void*
#define fp_dlopen(p) dlopen((p), RTLD_NOW | RTLD_LOCAL)
#define fp_mkdir(p) mkdir((p), 0755)
static void fp_sleep_us(unsigned us)
{
	struct timespec ts;
	ts.tv_sec  = 0;
	ts.tv_nsec = (long)us * 1000L;
	nanosleep(&ts, NULL);
}
#endif

#define FP_WARMUP  30
#define FP_FRAMES  60
/* One frame of 48 kHz audio is 801 stereo samples at NTSC's rate and 960
 * at PAL's; two frames' worth in one retro_run is a frame too many. */
#define FP_MAX_FRAME_AUDIO 1000

typedef void (*fp_fn_t)(void);
typedef void   retro_set_environment_t(retro_environment_t);
typedef void   retro_set_video_refresh_t(retro_video_refresh_t);
typedef void   retro_set_audio_sample_t(retro_audio_sample_t);
typedef void   retro_set_audio_sample_batch_t(retro_audio_sample_batch_t);
typedef void   retro_set_input_poll_t(retro_input_poll_t);
typedef void   retro_set_input_state_t(retro_input_state_t);
typedef void   retro_simple_t(void);
typedef bool   retro_load_game_t(const struct retro_game_info*);
typedef size_t retro_serialize_size_t(void);
typedef bool   retro_serialize_t(void*, size_t);
typedef bool   retro_unserialize_t(const void*, size_t);

/* What one retro_run handed over. */
struct fp_frame
{
	unsigned long audio_hash;
	unsigned long video_hash;
	unsigned      audio_frames;
	unsigned      videos;
};

static retro_simple_t*         s_run;
static retro_simple_t*         s_reset;
static retro_serialize_size_t* s_state_size;
static retro_serialize_t*      s_save;
static retro_unserialize_t*    s_load;

static char            s_system_dir[1024];
static int             s_failures;
static int             s_in_run;
static unsigned        s_outside;     /* presents outside a retro_run */
static unsigned        s_runs;
static unsigned long   s_rng = 1;
static int             s_antiblur;
static int             s_option_changed;
/* What GET_SAVESTATE_CONTEXT answers; 0 is RETRO_SAVESTATE_CONTEXT_NORMAL. */
static int             s_state_ctx;
static unsigned        s_pixel_bytes = 4;
static struct fp_frame s_cur;

static void fp_fail(const char* fmt, ...)
{
	va_list ap;
	s_failures++;
	if (s_failures > 20)
		return;
	printf("  FAIL: ");
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	fflush(stdout);
}

static unsigned fp_rand(unsigned n)
{
	s_rng = s_rng * 1103515245UL + 12345UL;
	return (unsigned)((s_rng >> 16) & 0x7FFF) % n;
}

/* Up to 1.5 ms, or nothing, at random. */
static void fp_jitter(void)
{
	unsigned r = fp_rand(4);
	if (r)
		fp_sleep_us(fp_rand(1500));
}

static unsigned long fp_hash(unsigned long h, const unsigned char* p, size_t n)
{
	size_t i;
	for (i = 0; i < n; i++)
		h = ((h ^ p[i]) * 16777619UL) & 0xFFFFFFFFUL;
	return h;
}

/* GetProcAddress already returns a function pointer; POSIX dlsym returns
 * an object pointer, copied across as lrps2_smoke does. */
static fp_fn_t fp_sym(FP_HANDLE h, const char* name)
{
#ifdef _WIN32
	return (fp_fn_t)GetProcAddress(h, name);
#else
	void* obj = dlsym(h, name);
	fp_fn_t fn = 0;
	memcpy(&fn, &obj, sizeof(fn));
	return fn;
#endif
}

static void fp_log(enum retro_log_level level, const char* fmt, ...)
{
	va_list ap;
	if (level < RETRO_LOG_ERROR)
		return;
	va_start(ap, fmt);
	printf("  [core] ");
	vprintf(fmt, ap);
	va_end(ap);
	fflush(stdout);
}

static void fp_video(const void* d, unsigned w, unsigned h, size_t pitch)
{
	unsigned y;
	fp_jitter();
	if (!s_in_run)
	{
		s_outside++;
		return;
	}
	s_cur.videos++;
	s_cur.video_hash = fp_hash(s_cur.video_hash, (const unsigned char*)&w, sizeof(w));
	s_cur.video_hash = fp_hash(s_cur.video_hash, (const unsigned char*)&h, sizeof(h));
	if (d)
		for (y = 0; y < h; y++)
			s_cur.video_hash = fp_hash(s_cur.video_hash,
				(const unsigned char*)d + y * pitch, (size_t)w * s_pixel_bytes);
}

static void fp_audio(int16_t l, int16_t r) { (void)l; (void)r; }

static size_t fp_audio_batch(const int16_t* d, size_t frames)
{
	fp_jitter();
	if (!s_in_run)
		fp_fail("audio handed over outside retro_run\n");
	s_cur.audio_frames += (unsigned)frames;
	s_cur.audio_hash = fp_hash(s_cur.audio_hash, (const unsigned char*)d, frames * 4);
	return frames;
}

static void fp_poll(void) { fp_jitter(); }

static int16_t fp_input(unsigned p, unsigned d, unsigned i, unsigned id)
{
	(void)p; (void)d; (void)i; (void)id;
	return 0;
}

static bool fp_environment(unsigned cmd, void* data)
{
	struct retro_variable* var;
	switch (cmd)
	{
		case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
			((struct retro_log_callback*)data)->log = fp_log;
			return true;
		case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
		case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
			*(const char**)data = s_system_dir;
			return true;
		case RETRO_ENVIRONMENT_GET_LANGUAGE:
			*(unsigned*)data = RETRO_LANGUAGE_ENGLISH;
			return true;
		case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
			*(unsigned*)data = 2;
			return true;
		case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
			s_pixel_bytes = *(const enum retro_pixel_format*)data
				== RETRO_PIXEL_FORMAT_XRGB8888 ? 4 : 2;
			return true;
		case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
		case RETRO_ENVIRONMENT_SET_HW_RENDER:
			return true;
		case RETRO_ENVIRONMENT_GET_VARIABLE:
			var = (struct retro_variable*)data;
			var->value = NULL;
			if      (!strcmp(var->key, "pcsx2_bios"))           var->value = "lrps2_framepace.bin";
			else if (!strcmp(var->key, "pcsx2_renderer"))       var->value = "Software (SW)";
			else if (!strcmp(var->key, "pcsx2_fastboot"))       var->value = "disabled";
			else if (!strcmp(var->key, "pcsx2_pcrtc_antiblur")) var->value = s_antiblur ? "enabled" : "disabled";
			return var->value != NULL;
		case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
			*(bool*)data = s_option_changed != 0;
			s_option_changed = 0;
			return true;
		case RETRO_ENVIRONMENT_GET_SAVESTATE_CONTEXT:
			if (data)
				*(enum retro_savestate_context*)data =
					(enum retro_savestate_context)s_state_ctx;
			return true;
		default:
			return false;
	}
}

static void fp_put32(unsigned char* p, unsigned v)
{
	p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
	p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}

/* MIPS encodings for the reset program: t0 = 8, t1 = 9, t2 = 10. */
#define FP_LUI(rt, imm)       (0x3C000000u | ((rt) << 16) | ((imm) & 0xFFFFu))
#define FP_ORI(rt, rs, imm)   (0x34000000u | ((rs) << 21) | ((rt) << 16) | ((imm) & 0xFFFFu))
#define FP_DSLL32(rd, rt)     (0x0000003Cu | ((rt) << 16) | ((rd) << 11))
#define FP_DSRL32(rd, rt)     (0x0000003Eu | ((rt) << 16) | ((rd) << 11))
#define FP_OR(rd, rs, rt)     (0x00000025u | ((rs) << 21) | ((rt) << 16) | ((rd) << 11))
#define FP_SD(rt, off, base)  (0xFC000000u | ((base) << 21) | ((rt) << 16) | ((off) & 0xFFFFu))
#define FP_SW(rt, off, base)  (0xAC000000u | ((base) << 21) | ((rt) << 16) | ((off) & 0xFFFFu))
#define FP_LW(rt, off, base)  (0x8C000000u | ((base) << 21) | ((rt) << 16) | ((off) & 0xFFFFu))
#define FP_ANDI(rt, rs, imm)  (0x30000000u | ((rs) << 21) | ((rt) << 16) | ((imm) & 0xFFFFu))
#define FP_BNE(rs, rt, off)   (0x14000000u | ((rs) << 21) | ((rt) << 16) | ((off) & 0xFFFFu))
#define FP_B(off)             (0x10000000u | ((off) & 0xFFFFu))

static unsigned fp_emit(unsigned char* rom, unsigned pc, unsigned op)
{
	fp_put32(rom + pc, op);
	return pc + 4;
}

/* t1 = hi:lo, then store it at offset off from base. */
static unsigned fp_emit_store64(unsigned char* rom, unsigned pc, unsigned base,
	unsigned off, unsigned hi, unsigned lo)
{
	pc = fp_emit(rom, pc, FP_LUI(9, hi >> 16));
	pc = fp_emit(rom, pc, FP_ORI(9, 9, hi));
	pc = fp_emit(rom, pc, FP_DSLL32(9, 9));
	pc = fp_emit(rom, pc, FP_LUI(10, lo >> 16));
	pc = fp_emit(rom, pc, FP_ORI(10, 10, lo));
	pc = fp_emit(rom, pc, FP_DSLL32(10, 10));
	pc = fp_emit(rom, pc, FP_DSRL32(10, 10));
	pc = fp_emit(rom, pc, FP_OR(9, 9, 10));
	return fp_emit(rom, pc, FP_SD(9, off, base));
}

/* tests/secondload's image: a 640x448 interlaced display of read circuit
 * 1, and a two-quadword GIF packet sent down the GIF DMA channel forever,
 * so every vsync is scanned out and the GS ring always carries work. */
static int fp_write_bios(const char* path)
{
	static unsigned char rom[4 * 1024 * 1024];
	FILE* f;
	unsigned pc, loop, wait;
	memset(rom, 0, sizeof(rom));
	pc = fp_emit(rom, 0, FP_LUI(8, 0xB200));
	pc = fp_emit_store64(rom, pc, 8, 0x00, 0, 0xFF25u);
	pc = fp_emit_store64(rom, pc, 8, 0x20, 0, 1u);
	pc = fp_emit_store64(rom, pc, 8, 0x70, 0, 10u << 9);
	pc = fp_emit_store64(rom, pc, 8, 0x80, 2559u | (447u << 12),
		636u | (50u << 12) | (3u << 23));
	pc = fp_emit(rom, pc, FP_LUI(11, 0xA010));
	pc = fp_emit_store64(rom, pc, 11, 0x00, 0x10000000u, 0x8001u);
	pc = fp_emit_store64(rom, pc, 11, 0x08, 0, 0xEu);
	pc = fp_emit_store64(rom, pc, 11, 0x10, 0, 0);
	pc = fp_emit_store64(rom, pc, 11, 0x18, 0, 0x3Fu);
	pc = fp_emit(rom, pc, FP_LUI(10, 0xB000));
	pc = fp_emit(rom, pc, FP_ORI(10, 10, 0xE000));
	pc = fp_emit(rom, pc, FP_ORI(9, 0, 1));
	pc = fp_emit(rom, pc, FP_SW(9, 0, 10));
	pc = fp_emit(rom, pc, FP_LUI(13, 0xB000));
	pc = fp_emit(rom, pc, FP_ORI(13, 13, 0xA000));
	pc = fp_emit(rom, pc, FP_LUI(12, 0x0010));
	pc = fp_emit(rom, pc, FP_ORI(14, 0, 2));
	pc = fp_emit(rom, pc, FP_ORI(15, 0, 0x101));
	loop = pc;
	pc = fp_emit(rom, pc, FP_SW(12, 0x10, 13));
	pc = fp_emit(rom, pc, FP_SW(14, 0x20, 13));
	pc = fp_emit(rom, pc, FP_SW(15, 0x00, 13));
	wait = pc;
	pc = fp_emit(rom, pc, FP_LW(9, 0, 13));
	pc = fp_emit(rom, pc, 0);
	pc = fp_emit(rom, pc, FP_ANDI(9, 9, 0x100));
	pc = fp_emit(rom, pc, FP_BNE(9, 0, (wait - (pc + 4)) >> 2));
	pc = fp_emit(rom, pc, 0);
	pc = fp_emit(rom, pc, FP_B((loop - (pc + 4)) >> 2));
	fp_emit(rom, pc, 0);
	memcpy(rom + 0x1000, "RESET", 5);  fp_put32(rom + 0x1000 + 12, 0x1000);
	memcpy(rom + 0x1010, "ROMDIR", 6); fp_put32(rom + 0x1010 + 12, 0x40);
	memcpy(rom + 0x1020, "ROMVER", 6); fp_put32(rom + 0x1020 + 12, 0x10);
	memcpy(rom + 0x1040, "0160AC20020207", 14);
	f = fopen(path, "wb");
	if (!f)
		return 0;
	if (fwrite(rom, 1, sizeof(rom), f) != sizeof(rom))
	{
		fclose(f);
		return 0;
	}
	return fclose(f) == 0;
}

/* One retro_run, checked on its own: one frame, at most one frame of
 * audio. */
static void fp_run(struct fp_frame* out)
{
	memset(&s_cur, 0, sizeof(s_cur));
	s_cur.audio_hash = s_cur.video_hash = 2166136261UL;
	fp_jitter();
	s_in_run = 1;
	s_run();
	s_in_run = 0;
	s_runs++;
	if (s_cur.videos != 1)
		fp_fail("retro_run %u presented %u frames\n", s_runs, s_cur.videos);
	if (s_cur.audio_frames > FP_MAX_FRAME_AUDIO)
		fp_fail("retro_run %u handed over %u samples, more than a frame's\n",
			s_runs, s_cur.audio_frames);
	if (out)
		*out = s_cur;
}

static void fp_save(void* buf, size_t size)
{
	fp_jitter();
	if (!s_save(buf, size))
		fp_fail("retro_serialize failed\n");
}

static void fp_load(const void* buf, size_t size)
{
	fp_jitter();
	if (!s_load(buf, size))
		fp_fail("retro_unserialize failed\n");
}

static void fp_compare(const char* what, const struct fp_frame* want,
	const struct fp_frame* got, int with_video)
{
	int i;
	for (i = 0; i < FP_FRAMES; i++)
	{
		if (want[i].audio_frames != got[i].audio_frames
		 || want[i].audio_hash != got[i].audio_hash)
		{
			fp_fail("%s: frame %d audio is %u samples (%08lx), straight on it is %u (%08lx)\n",
				what, i, got[i].audio_frames, got[i].audio_hash,
				want[i].audio_frames, want[i].audio_hash);
			return;
		}
		if (with_video && want[i].video_hash != got[i].video_hash)
		{
			fp_fail("%s: frame %d picture differs from running straight on\n", what, i);
			return;
		}
	}
}

static void fp_compare_state(const char* what, const unsigned char* want,
	const unsigned char* got, size_t size)
{
	size_t i;
	for (i = 0; i < size; i++)
		if (want[i] != got[i])
		{
			fp_fail("%s: the savestate at the end differs from running straight on,"
				" first at byte %lu of %lu\n", what, (unsigned long)i, (unsigned long)size);
			return;
		}
}

int main(int argc, char** argv)
{
	char path[1100];
	FP_HANDLE h;
	retro_set_environment_t* set_environment;
	retro_set_video_refresh_t* set_video;
	retro_set_audio_sample_t* set_audio;
	retro_set_audio_sample_batch_t* set_audio_batch;
	retro_set_input_poll_t* set_poll;
	retro_set_input_state_t* set_input;
	retro_simple_t *init_fn, *deinit_fn, *unload_game;
	retro_load_game_t* load_game;
	static struct fp_frame straight[FP_FRAMES], again[FP_FRAMES];
	static struct fp_frame booted[FP_WARMUP];
	unsigned char *start, *end, *other, *scratch;
	size_t size;
	int i;

	if (argc < 3)
	{
		fprintf(stderr, "usage: framepace <path-to-core> <scratch-dir>\n");
		return 1;
	}
#ifdef _WIN32
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#endif
	if (strlen(argv[2]) + 48 > sizeof(s_system_dir))
	{
		fprintf(stderr, "framepace: scratch path too long\n");
		return 1;
	}
	strcpy(s_system_dir, argv[2]);
	sprintf(path, "%s/pcsx2", s_system_dir);
	fp_mkdir(path);
	sprintf(path, "%s/pcsx2/bios", s_system_dir);
	fp_mkdir(path);
	sprintf(path, "%s/pcsx2/bios/lrps2_framepace.bin", s_system_dir);
	if (!fp_write_bios(path))
	{
		fprintf(stderr, "framepace: cannot write %s\n", path);
		return 1;
	}

	h = fp_dlopen(argv[1]);
	if (!h)
	{
		fprintf(stderr, "framepace: cannot load %s\n", argv[1]);
		return 1;
	}
	set_environment = (retro_set_environment_t*)fp_sym(h, "retro_set_environment");
	set_video       = (retro_set_video_refresh_t*)fp_sym(h, "retro_set_video_refresh");
	set_audio       = (retro_set_audio_sample_t*)fp_sym(h, "retro_set_audio_sample");
	set_audio_batch = (retro_set_audio_sample_batch_t*)fp_sym(h, "retro_set_audio_sample_batch");
	set_poll        = (retro_set_input_poll_t*)fp_sym(h, "retro_set_input_poll");
	set_input       = (retro_set_input_state_t*)fp_sym(h, "retro_set_input_state");
	init_fn         = (retro_simple_t*)fp_sym(h, "retro_init");
	deinit_fn       = (retro_simple_t*)fp_sym(h, "retro_deinit");
	unload_game     = (retro_simple_t*)fp_sym(h, "retro_unload_game");
	load_game       = (retro_load_game_t*)fp_sym(h, "retro_load_game");
	s_run           = (retro_simple_t*)fp_sym(h, "retro_run");
	s_reset         = (retro_simple_t*)fp_sym(h, "retro_reset");
	s_state_size    = (retro_serialize_size_t*)fp_sym(h, "retro_serialize_size");
	s_save          = (retro_serialize_t*)fp_sym(h, "retro_serialize");
	s_load          = (retro_unserialize_t*)fp_sym(h, "retro_unserialize");
	if (!set_environment || !set_video || !set_audio || !set_audio_batch || !set_poll
	 || !set_input || !init_fn || !deinit_fn || !unload_game || !load_game || !s_run
	 || !s_reset || !s_state_size || !s_save || !s_load)
	{
		fprintf(stderr, "framepace: core is missing a libretro export\n");
		return 2;
	}

	set_environment(fp_environment);
	set_video(fp_video);
	set_audio(fp_audio);
	set_audio_batch(fp_audio_batch);
	set_poll(fp_poll);
	set_input(fp_input);
	init_fn();
	if (!load_game(NULL))
	{
		fprintf(stderr, "framepace: retro_load_game failed\n");
		return 3;
	}

	for (i = 0; i < FP_WARMUP; i++)
		fp_run(&booted[i]);

	size    = s_state_size();
	start   = (unsigned char*)malloc(size);
	end     = (unsigned char*)malloc(size);
	other   = (unsigned char*)malloc(size);
	scratch = (unsigned char*)malloc(size);
	if (!start || !end || !other || !scratch)
	{
		fprintf(stderr, "framepace: out of memory\n");
		return 4;
	}

	/* Straight on from a savestate: the reference. */
	fp_save(start, size);
	for (i = 0; i < FP_FRAMES; i++)
		fp_run(&straight[i]);
	fp_save(end, size);
	printf("  straight on: %d frames, %u samples in the first\n", FP_FRAMES, straight[0].audio_frames);

	/* Loaded and run on again. */
	s_rng = 7;
	fp_load(start, size);
	for (i = 0; i < FP_FRAMES; i++)
		fp_run(&again[i]);
	fp_save(other, size);
	fp_compare("loaded", straight, again, 1);
	fp_compare_state("loaded", end, other, size);

	/* A savestate taken before every frame. */
	s_rng = 11;
	fp_load(start, size);
	for (i = 0; i < FP_FRAMES; i++)
	{
		fp_save(scratch, size);
		fp_run(&again[i]);
	}
	fp_save(other, size);
	fp_compare("saved before every frame", straight, again, 1);
	fp_compare_state("saved before every frame", end, other, size);

	/* Every frame run, rolled back and run again. */
	s_rng = 13;
	fp_load(start, size);
	for (i = 0; i < FP_FRAMES; i++)
	{
		fp_save(scratch, size);
		fp_run(NULL);
		fp_load(scratch, size);
		fp_run(&again[i]);
	}
	fp_save(other, size);
	fp_compare("rolled back every frame", straight, again, 1);
	fp_compare_state("rolled back every frame", end, other, size);

	/* The same, as runahead does it: the frontend says the state stays
	 * in this instance, so the core loads it without a rollback copy. */
	s_rng = 23;
	fp_load(start, size);
	s_state_ctx = RETRO_SAVESTATE_CONTEXT_RUNAHEAD_SAME_INSTANCE;
	for (i = 0; i < FP_FRAMES; i++)
	{
		fp_save(scratch, size);
		fp_run(NULL);
		fp_load(scratch, size);
		fp_run(&again[i]);
	}
	s_state_ctx = RETRO_SAVESTATE_CONTEXT_NORMAL;
	fp_save(other, size);
	fp_compare("rolled back every frame, same instance", straight, again, 1);
	fp_compare_state("rolled back every frame, same instance", end, other, size);

	/* A state cut short, rejected before every frame: the running
	 * machine is put back each time and runs on as if it was never
	 * offered. */
	s_rng = 19;
	fp_load(start, size);
	for (i = 0; i < FP_FRAMES; i++)
	{
		fp_save(scratch, size);
		fp_jitter();
		if (s_load(scratch, size / 2))
			fp_fail("a state cut to half its size was loaded\n");
		fp_run(&again[i]);
	}
	fp_save(other, size);
	fp_compare("a rejected state before every frame", straight, again, 1);
	fp_compare_state("a rejected state before every frame", end, other, size);

	/* The same with the frontend claiming the same instance: a state of
	 * the wrong size is still not one this session wrote, so it is still
	 * loaded with the rollback copy and the machine is still put back. */
	s_rng = 29;
	fp_load(start, size);
	s_state_ctx = RETRO_SAVESTATE_CONTEXT_RUNAHEAD_SAME_INSTANCE;
	for (i = 0; i < FP_FRAMES; i++)
	{
		fp_save(scratch, size);
		fp_jitter();
		if (s_load(scratch, size / 2))
			fp_fail("a state cut to half its size was loaded, same instance\n");
		fp_run(&again[i]);
	}
	s_state_ctx = RETRO_SAVESTATE_CONTEXT_NORMAL;
	fp_save(other, size);
	fp_compare("a rejected state before every frame, same instance", straight, again, 1);
	fp_compare_state("a rejected state before every frame, same instance", end, other, size);

	/* An option change, applied with the EE paused, on random frames. */
	s_rng = 17;
	fp_load(start, size);
	for (i = 0; i < FP_FRAMES; i++)
	{
		if (fp_rand(3) == 0)
		{
			s_antiblur       = !s_antiblur;
			s_option_changed = 1;
		}
		fp_run(&again[i]);
	}
	fp_compare("option changes", straight, again, 0);

	/* A reset: the new machine's frames carry the audio a freshly booted
	 * one's do, from its first frame on. */
	fp_jitter();
	s_reset();
	for (i = 0; i < FP_WARMUP; i++)
	{
		fp_run(&again[0]);
		if (again[0].audio_frames != booted[i].audio_frames
		 || again[0].audio_hash != booted[i].audio_hash)
		{
			fp_fail("reset: frame %d audio is %u samples, after a boot it is %u\n",
				i, again[0].audio_frames, booted[i].audio_frames);
			break;
		}
	}

	if (s_outside)
		fp_fail("%u frames presented outside retro_run\n", s_outside);

	free(start);
	free(end);
	free(other);
	free(scratch);
	unload_game();
	deinit_fn();

	printf(s_failures ? "framepace: FAILED (%d)\n" : "framepace: ok, %u retro_runs\n",
		s_failures ? (unsigned)s_failures : s_runs);
	return s_failures != 0;
}
