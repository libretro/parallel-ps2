/* The Konami Python 1 I/O board. See p1io.h. */
#include "p1io.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <compat/strl.h>
#include <file/file_path.h>
#include <streams/file_stream.h>

#include "fatimg.h"
#include "DEV9/ATA/HddImage.h"

extern retro_log_printf_t log_cb;
extern s64 PSXCLK;

/* ---- the board's constants ---------------------------------------- */

#define UART_RX_LIMIT             0x400
#define UART_CALLBACK_BYTE_LIMIT  38
#define UART_WRITE_LIMIT          0x1000
#define STORAGE_BYTES_PER_SECOND  (24u * 1024 * 1024)
#define ADPCM_HEADER_SIZE         0x10
#define ADPCM_MAX_ENCODED         (32u * 1024 * 1024)
#define ADPCM_INPUT_RATE          22050
#define ADPCM_OUTPUT_RATE         48000
#define ADPCM_FADE_SAMPLES        (ADPCM_OUTPUT_RATE / 200) /* 5 ms */
#define ADPCM_LOWPASS_ALPHA       0.60f

#define CMD_CF                    0x390
#define CMD_ATA                   0x3a0
#define CMD_ADPCM                 0x0d0
#define CMD_UART                  0x100
#define CMD_DALLAS                0x120
#define CMD_BBSRAM                0x140
#define CMD_BOOTROM               0x150
#define CMD_FSCI                  0x160
#define CMD_NET_BASE              0x180
#define CMD_NET_STRIDE            0x20
#define CMD_JAMMA_INIT            0x0e0
#define CMD_JAMMA_OUTPUT          0xf80
#define NET_CHANNELS              8

#define ST_DALLAS                 0x00010000u
#define ST_UART                   0x00030000u
#define ST_CF                     0x00050000u
#define ST_ATA                    0x00060000u
#define ST_ADPCM                  0x00070000u
#define ST_BBSRAM                 0x00080000u
#define ST_BOOTROM                0x00090000u
#define ST_FSCI                   0x000a0000u
#define ST_NET_BASE               0x000b0000u
#define ST_NET_STRIDE             0x1000u

#define BBSRAM_NETWORK_ID         0x686
#define RUNTIME_READY_OFFSET      0xfffd05735730ULL
#define BOOT_READY_OFFSET         0xfffd05735734ULL
#define NO_JAMMA_DEST             0xffffffffu
#define JAMMA_REPORT_QUADS        9

#define SECTOR_SIZE               0x200
#define DBUF_WRITEB_MAX           0x200
#define LEGACY_BOOTROM_SIZE       0x10000
#define BOOTROM_SIZE              0x80000
#define BBSRAM_SIZE               0x2000
#define BBSRAM_VOLATILE_TEST      0x1d00
#define DALLAS_SERIAL             8
#define DALLAS_PAYLOAD            0x20
#define DALLAS_TOTAL              (DALLAS_SERIAL + DALLAS_PAYLOAD)
#define BOOTROM_MAC_BACKUP        0xf000
#define FSCI_FRAME                18
#define FSCI_FRAMES               9
#define FSCI_STREAM               (FSCI_FRAMES * FSCI_FRAME)
#define PENDING_MAX               64
#define KEY_EVENTS                256

/* The JAMMA panel's source bits, as the EE reads them. */
#define SRC_P1_LEFT    (1u << 0)
#define SRC_P1_RIGHT   (1u << 1)
#define SRC_P1_UP      (1u << 2)
#define SRC_P1_DOWN    (1u << 3)
#define SRC_P1_BUTTON1 (1u << 4)
#define SRC_P1_BUTTON2 (1u << 5)
#define SRC_P1_BUTTON3 (1u << 6)
#define SRC_P1_BUTTON4 (1u << 7)
#define SRC_P1_BUTTON5 (1u << 8)
#define SRC_P1_BUTTON6 (1u << 9)
#define SRC_P1_START   (1u << 11)
#define SRC_SERVICE    (1u << 14)
#define SRC_TEST       (1u << 15)
#define SRC_P2_LEFT    (1u << 16)
#define SRC_P2_RIGHT   (1u << 17)
#define SRC_P2_UP      (1u << 18)
#define SRC_P2_DOWN    (1u << 19)
#define SRC_P2_BUTTON1 (1u << 20)
#define SRC_P2_BUTTON2 (1u << 21)
#define SRC_P2_BUTTON3 (1u << 22)
#define SRC_P2_BUTTON4 (1u << 23)
#define SRC_P2_BUTTON5 (1u << 24)
#define SRC_P2_BUTTON6 (1u << 25)
#define SRC_P2_START   (1u << 27)

/* The EE byte-swaps the JAMMA word, then maps source bit 8 to P1 bit
 * 0x200. */
#define JAMMA_JVS_PRESENT   0x00010000u
#define JAMMA_NEUTRAL       0x0100ffffu
#define JAMMA_ACTIVE_LOW    0x0000ffffu

enum
{
	B_TEST, B_SERVICE, B_COIN1, B_COIN2,
	B_P1_START, B_P1_UP, B_P1_DOWN, B_P1_LEFT, B_P1_RIGHT,
	B_P1_1, B_P1_2, B_P1_3, B_P1_4, B_P1_5, B_P1_6,
	B_P2_START, B_P2_UP, B_P2_DOWN, B_P2_LEFT, B_P2_RIGHT,
	B_P2_1, B_P2_2, B_P2_3, B_P2_4, B_P2_5, B_P2_6,
	B_COUNT
};

#define BIT(b) (1u << (b))

static const u8 s_factory_mac[6] = { 0x00, 0x04, 0x5f, 0x00, 0x00, 0x01 };
static const u8 s_mac_backup[] = {
	0x00, 0x04, 0x5f, 0x00, 0x00, 0x01, 0xff, 0xfb, 0xa0, 0xff, 0xff, 0xfe
};
static const u8 s_net_subnet[4] = { 255, 255, 255, 0 };
static const u8 s_net_zero[4] = { 0, 0, 0, 0 };
static const u8 s_net_machine_id[64] = {
	'P', 'Y', 'T', 'H', 'O', 'N', '1', '-', '0', '0', '0', '0', '0', '0', '0', '1'
};
/* WE2K3 takes a programmed backup Dallas record when no key is in. */
static const u8 s_we2k3_dallas[] = {
	0x00, 0x04, 0x5f, 0x00, 0x00, 0x01, 0x00, 0x00,
	0x8c, 0x02, 0x4d, 0x83, 0x06, 0xd2, 0x00, 0x00,
	'G', 'E', 'C', '2', '7', 0x00, 0x00, 0x00,
	0x20, 0x03, 'J', 'A', 'E', 0x00, 0x8f, 0x43
};
static const u8 s_default_dallas_id[6] = { 0x5f, 0x04, 0x00, 0x01, 0x00, 0x01 };
static const u32 s_dallas_no_key[3] = { 0x01000000, 0, 0 };

static const u8 s_extio_sync[] = { 0xaa, 0xaa, 0xaa, 0x55 };
static const u8 s_extio_reset[] = { 0xaa, 0xaa, 0x00, 0x00 };
static const u8 s_extio_count[] = { 0xaa, 0xaa, 0x00, 0x01, 0x01, 0x01, 0xad };
static const u8 s_b22_count[] = {
	0xaa, 0xaa, 0x00, 0x01, 0x01, 0x00, 0xac,
	0xaa, 0xaa, 0x00, 0x01, 0x01, 0x01, 0xad
};
static const u8 s_dogstation_count[] = { 0xaa, 0xaa, 0x00, 0x01, 0x01, 0x02, 0xae };
static const u8 s_extio_product[] = {
	0xaa, 0xaa, 0x01, 0x02, 0x00, 0x00,
	0xaa, 0x01, 0x00, 0x02, 0x00,
	0x03, 0x00, 0x00, 0x00,
	0x00,
	0x01, 0x01, 0x00,
	'I', 'C', 'C', 'A', 0x00, 0x00, 0x00, 0x00, 0x18
};
static const u8 s_extio_startup[] = {
	0xaa, 0xaa, 0x01, 0x03, 0x00, 0x00,
	0xaa, 0x01, 0x00, 0x03, 0x00, 0x00, 0x04
};
static const u8 s_icca_product[] = {
	0xaa, 0x81, 0x00, 0x02, 0x00, 0x2c,
	0x03, 0x00, 0x00, 0x00, /* device ID */
	0x00,                   /* flag */
	0x01, 0x01, 0x00,       /* version */
	'I', 'C', 'C', 'A',
	'O', 'c', 't', ' ', '2', '6', ' ', '2', '0', '0', '5', 0x00, 0x00, 0x00, 0x00, 0x00,
	'1', '3', ' ', ':', ' ', '5', '5', ' ', ':', ' ', '0', '3', 0x00, 0x00, 0x00, 0x00
};
static const u8 s_icca_startup[] = { 0xaa, 0x81, 0x00, 0x03, 0x00, 0x01, 0x00 };
static const u8 s_icca_status[] = {
	0xaa, 0x81, 0x01, 0x31, 0x00, 0x10,
	0x01, 0x00, /* reader present, no card, no sensor */
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x03, 0x00, 0x00, 0x00, 0x00
};

static const s32 s_adpcm_steps[49] = {
	256, 272, 304, 336, 368, 400, 448, 496, 544, 592, 656, 720,
	800, 880, 960, 1056, 1168, 1280, 1408, 1552, 1712, 1888, 2080, 2288,
	2512, 2768, 3040, 3344, 3680, 4048, 4464, 4912, 5392, 5936, 6528, 7184,
	7904, 8704, 9568, 10528, 11584, 12736, 14016, 15408, 16960, 18656, 20512, 22576,
	24832
};
static const s32 s_adpcm_index[16] = {
	-1, -1, -1, -1, 2, 4, 6, 8,
	-1, -1, -1, -1, 2, 4, 6, 8
};

/* The WE2K3 board's config ROM, as captured. */
static const u32 s_crom[] = {
	0x0404a1a4, 0x31333934, 0x407d8002, 0x00000000, 0x00000000, 0x00053f04,
	0x03000679, 0x8100000a, 0x0c0083c0, 0xc3000005, 0xd1000001, 0x00020901,
	0x12000679, 0x13001000, 0x000231e6, 0x17000000, 0x81000006, 0x0004f2d6,
	0x00000000, 0x00000000, 0x4b4f4e41, 0x4d490000, 0x000428a6, 0x00000000,
	0x00000000, 0x5053322d, 0x41430000
};

/* ---- state -------------------------------------------------------- */

typedef struct pending_status
{
	u32 ready;
	u32 status_offset;
	u32 checksum;
} pending_status;

typedef struct adpcm_state
{
	u32 sector, size;
	u32 position, loop_start, loop_end;
	u32 resample, vol_l, vol_r, fade_in, fade_out;
	float filter[2];
	/* The decoder after the sample at position, and as it was after the
	 * sample at loop_start, which a loop goes back to. */
	s32 step[2], pcm[2];
	s32 loop_step[2], loop_pcm[2];
	u8 playing, looping, stopping, filter_init, loop_saved, from_cf, pad[2];
} adpcm_state;

/* What a savestate holds besides the board's memories. */
typedef struct p1io_meta
{
	u32 version;
	u32 fsci_offset;
	u32 uart_len;
	u32 npending;
	pending_status pending[PENDING_MAX];
	u32 jamma_dest, last_jamma, last_bind, latch, memcard_slot;
	u32 next_ready;
	u32 bind_state;
	u32 key_head, key_count;
	u16 coin[2];
	adpcm_state adpcm;
	u8 next_ready_valid, bootrom_dirty, bbsram_dirty, formatted;
	u8 dallas_loaded[2], dallas_dirty[2];
	u8 factory_mac[8];
	u8 net_prop[64];
	u8 key_pressed[64];
} p1io_meta;

#define P1IO_STATE_VERSION 1

typedef struct p1io_state
{
	p1io_meta m;
	u8 bootrom[BOOTROM_SIZE];
	u8 bbsram[BBSRAM_SIZE];
	u8 dallas[2][DALLAS_TOTAL];
	u8 fsci[FSCI_STREAM];
	u8 uart[UART_RX_LIMIT];
	u8 key_events[KEY_EVENTS];
} p1io_state;

typedef struct storage
{
	const char* path;
	hdd_image_t* img;
	RFILE* raw;
	u64 size;
	int tried;
	int is_cf;
	fat_file fat;
	int fat_ok;
} storage;

struct p1io
{
	fw_device dev;
	py1_desc desc;
	p1io_input_source input;
	u32 crom[256];
	u32 crom_words;
	storage hdd, cf;
	u8* adpcm_data;
	u8 scratch[UART_WRITE_LIMIT * 2 + 256];
	p1io_state st;
};

static p1io* s_active;

/* ---- small helpers ------------------------------------------------ */

static u32 bswap32(u32 v)
{
	return (v >> 24) | ((v >> 8) & 0xff00u) | ((v & 0xff00u) << 8) | (v << 24);
}

static u32 be32(const u8* p)
{
	return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static u32 min_u32(u32 a, u32 b)
{
	return a < b ? a : b;
}

static int serial_mode(const p1io* io)
{
	return io->desc.io_mode == P1IO_MODE_B22 || io->desc.io_mode == P1IO_MODE_DOGSTATIONDX;
}

static void queue_status(u32 offset, u32 value)
{
	fw_host_queue_quad(0xfffe, offset, value);
}

/* ---- files -------------------------------------------------------- */

static int write_file(const char* path, const void* data, u32 size)
{
	char* dir;
	int ok;
	if (!path[0])
		return 0;
	dir = (char*)malloc(PY1_PATH);
	if (dir)
	{
		strlcpy(dir, path, PY1_PATH);
		path_basedir(dir);
		if (dir[0])
			path_mkdir(dir);
		free(dir);
	}
	ok = filestream_write_file(path, data, size);
	if (!ok)
		log_cb(RETRO_LOG_WARN, "Python 1: could not write %s\n", path);
	return ok;
}

/* The file at path when it is exactly size bytes (or, with alt, alt
 * bytes); its size in *got, -1 when missing. */
static u8* read_file(const char* path, s64* got)
{
	void* data = NULL;
	int64_t len = 0;
	*got = -1;
	if (!path[0] || !path_is_valid(path) || !filestream_read_file(path, &data, &len))
		return NULL;
	*got = len;
	return (u8*)data;
}

static void save_bootrom(p1io* io)
{
	if (io->st.m.bootrom_dirty && io->desc.bootrom[0] && write_file(io->desc.bootrom, io->st.bootrom, BOOTROM_SIZE))
		io->st.m.bootrom_dirty = 0;
}

static void save_bbsram(p1io* io)
{
	if (io->st.m.bbsram_dirty && io->desc.bbsram[0] && write_file(io->desc.bbsram, io->st.bbsram, BBSRAM_SIZE))
		io->st.m.bbsram_dirty = 0;
}

static void save_dallas(p1io* io)
{
	int i;
	for (i = 0; i < 2; i++)
	{
		if (!io->st.m.dallas_dirty[i])
			continue;
		if (!io->desc.dongle[i][0])
			continue; /* the built-in key: nowhere to keep it */
		if (write_file(io->desc.dongle[i], io->st.dallas[i], DALLAS_TOTAL))
			io->st.m.dallas_dirty[i] = 0;
	}
}

/* ---- Dallas keys, the MAC and the FSCI stream --------------------- */

static u8 dallas_crc8(const u8* data, u32 size, u8 crc)
{
	u32 i, bit;
	for (i = 0; i < size; i++)
	{
		u8 in = data[i];
		for (bit = 0; bit < 8; bit++)
		{
			const u8 mix = (u8)((crc ^ in) & 1);
			crc >>= 1;
			if (mix)
				crc ^= 0x8c;
			in >>= 1;
		}
	}
	return crc;
}

static void set_dallas_key_id(p1io* io, const u8* id)
{
	u8* k = io->st.dallas[0];
	k[0] = 0x14;
	memcpy(k + 1, id, 6);
	k[7] = dallas_crc8(k, DALLAS_SERIAL - 1, 0);
}

static void dallas_key_from_bootrom(p1io* io)
{
	u32 off;
	set_dallas_key_id(io, s_default_dallas_id);
	for (off = 0xf000; off <= BOOTROM_SIZE - 0x20; off += 0x10)
	{
		const u8* r = io->st.bootrom + off;
		if (r[0x10] == 'G' && r[0x1a] == 'J' && r[0x1b] == 'A')
		{
			set_dallas_key_id(io, r);
			return;
		}
	}
}

/* A dongle file in MAME's layout (payload, then serial) or the older one
 * (serial, then payload). */
static int normalize_dallas(const u8* raw, u8* out)
{
	if (dallas_crc8(raw + DALLAS_PAYLOAD, DALLAS_SERIAL - 1, 0) == raw[DALLAS_TOTAL - 1])
	{
		memcpy(out, raw + DALLAS_PAYLOAD, DALLAS_SERIAL);
		memcpy(out + DALLAS_SERIAL, raw, DALLAS_PAYLOAD);
		return 1;
	}
	if (dallas_crc8(raw, DALLAS_SERIAL - 1, 0) == raw[DALLAS_SERIAL - 1])
	{
		memcpy(out, raw, DALLAS_TOTAL);
		return 1;
	}
	return 0;
}

static int valid_unicast_mac(const u8* mac)
{
	int i, zero = 1, ff = 1;
	for (i = 0; i < 6; i++)
	{
		zero &= mac[i] == 0;
		ff &= mac[i] == 0xff;
	}
	return !zero && !ff && !(mac[0] & 1);
}

static u8 hex_digit(u8 v)
{
	return (u8)(v < 10 ? '0' + v : 'A' + v - 10);
}

static void fsci_hex(u8* d, u8 v)
{
	d[0] = hex_digit((u8)(v >> 4));
	d[1] = hex_digit((u8)(v & 0xf));
}

static u8 fsci_checksum(const u8* d, u32 n)
{
	u32 sum = 0, i;
	for (i = 0; i < n; i++)
		sum += d[i];
	while (sum > 0xff)
		sum = (sum & 0xff) + (sum >> 8);
	return (u8)sum;
}

static void fsci_build(p1io* io)
{
	u32 f, i;
	for (f = 0; f < FSCI_FRAMES; f++)
	{
		u8* d = io->st.fsci + f * FSCI_FRAME;
		d[0] = '@';
		fsci_hex(d + 1, 0x0d);
		d[3] = 'M';
		for (i = 0; i < 6; i++)
			fsci_hex(d + 4 + i * 2, io->st.m.factory_mac[i]);
		fsci_hex(d + 16, fsci_checksum(d + 3, 0x0d));
	}
}

static void mac_from_bootrom(p1io* io)
{
	const u8* r = io->st.bootrom + BOOTROM_MAC_BACKUP;
	int i, ok = valid_unicast_mac(r);
	for (i = 0; ok && i < 6; i++)
		ok = (r[i] ^ r[i + 6]) == 0xff;
	memcpy(io->st.m.factory_mac, ok ? r : s_factory_mac, 6);
	if (!ok)
		log_cb(RETRO_LOG_WARN, "Python 1: the boot ROM's MAC backup is not valid; the default is used\n");
	fsci_build(io);
}

/* ---- loading ------------------------------------------------------ */

static void generated_bootrom(p1io* io)
{
	memset(io->st.bootrom, 0xff, BOOTROM_SIZE);
	memcpy(io->st.bootrom, s_factory_mac, sizeof(s_factory_mac));
	memcpy(io->st.bootrom + BOOTROM_MAC_BACKUP, s_mac_backup, sizeof(s_mac_backup));
	memcpy(io->st.bootrom + 0xf030, s_we2k3_dallas, sizeof(s_we2k3_dallas));
}

static void load_bootrom(p1io* io)
{
	s64 len;
	u8* data;

	io->st.m.bootrom_dirty = 0;
	generated_bootrom(io);
	data = read_file(io->desc.bootrom, &len);
	if (!data)
	{
		/* None yet: the board starts with the generated one, kept. */
		if (io->desc.bootrom[0])
			write_file(io->desc.bootrom, io->st.bootrom, BOOTROM_SIZE);
	}
	else if (len == BOOTROM_SIZE || len == LEGACY_BOOTROM_SIZE)
	{
		memcpy(io->st.bootrom, data, (size_t)len);
		if (len == LEGACY_BOOTROM_SIZE)
		{
			/* The older 64 KiB dump is the start of the full ROM. */
			io->st.m.bootrom_dirty = 1;
			save_bootrom(io);
		}
	}
	else
		log_cb(RETRO_LOG_WARN, "Python 1: %s is %d bytes, not a boot ROM of 0x%x or 0x%x\n",
			io->desc.bootrom, (int)len, LEGACY_BOOTROM_SIZE, BOOTROM_SIZE);
	free(data);
	mac_from_bootrom(io);
	dallas_key_from_bootrom(io);
}

static void load_bbsram(p1io* io)
{
	s64 len;
	u8* data;
	memset(io->st.bbsram, 0, BBSRAM_SIZE);
	io->st.m.bbsram_dirty = 0;
	data = read_file(io->desc.bbsram, &len);
	if (data)
		memcpy(io->st.bbsram, data, (size_t)(len < BBSRAM_SIZE ? len : BBSRAM_SIZE));
	else if (io->desc.bbsram[0])
		write_file(io->desc.bbsram, io->st.bbsram, BBSRAM_SIZE);
	free(data);
}

static void load_dallas(p1io* io)
{
	int i;
	for (i = 0; i < 2; i++)
	{
		s64 len;
		u8* data;
		memset(io->st.dallas[i], 0, DALLAS_TOTAL);
		io->st.m.dallas_loaded[i] = 0;
		io->st.m.dallas_dirty[i] = 0;
		if (!io->desc.dongle[i][0])
			continue;
		data = read_file(io->desc.dongle[i], &len);
		if (data && len == DALLAS_TOTAL && normalize_dallas(data, io->st.dallas[i]))
			io->st.m.dallas_loaded[i] = 1;
		else
			log_cb(RETRO_LOG_WARN, "Python 1: %s is not a Dallas key\n", io->desc.dongle[i]);
		free(data);
	}
	/* The internal key, when there is no file for it, is the boot ROM's. */
	if (!io->st.m.dallas_loaded[0])
	{
		dallas_key_from_bootrom(io);
		io->st.m.dallas_loaded[0] = 1;
	}
}

static void load_config_rom(p1io* io)
{
	s64 len;
	u8* data = read_file(io->desc.config_rom, &len);
	u32 i;
	io->crom_words = 0;
	if (data && len > 0 && !(len & 3))
	{
		io->crom_words = (u32)(len / 4 > 256 ? 256 : len / 4);
		for (i = 0; i < io->crom_words; i++)
			io->crom[i] = be32(data + i * 4);
	}
	else if (io->desc.config_rom[0])
		log_cb(RETRO_LOG_WARN, "Python 1: config ROM %s not read; the built-in one is used\n", io->desc.config_rom);
	free(data);
}

/* ---- storage ------------------------------------------------------ */

static int storage_io(void* ctx, u64 offset, void* buf, u32 len, int write)
{
	storage* s = (storage*)ctx;
	if (s->img)
		return write ? hdd_image_write(s->img, offset, buf, len) && hdd_image_flush(s->img)
			: hdd_image_read(s->img, offset, buf, len);
	if (!s->raw || filestream_seek(s->raw, (int64_t)offset, RETRO_VFS_SEEK_POSITION_START) != 0)
		return 0;
	if (write)
		return filestream_write(s->raw, buf, len) == (int64_t)len && filestream_flush(s->raw) == 0;
	return filestream_read(s->raw, buf, len) == (int64_t)len;
}

static void storage_close(storage* s)
{
	fat_close(&s->fat);
	s->fat_ok = 0;
	if (s->img)
		hdd_image_close(s->img);
	if (s->raw)
		filestream_close(s->raw);
	s->img = NULL;
	s->raw = NULL;
	s->size = 0;
	s->tried = 0;
}

/* Opened at the first use, read and write; a raw image that cannot be
 * written is read. */
static int storage_open(storage* s)
{
	if (s->tried)
		return s->img || s->raw;
	s->tried = 1;
	if (!s->path || !s->path[0])
		return 0;
	if (hdd_image_kind(s->path) != HDD_IMAGE_RAW)
	{
		s->img = hdd_image_open(s->path);
		if (s->img)
			s->size = hdd_image_size(s->img);
	}
	else
	{
		s->raw = filestream_open(s->path, RETRO_VFS_FILE_ACCESS_READ_WRITE | RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING,
			RETRO_VFS_FILE_ACCESS_HINT_NONE);
		if (!s->raw)
			s->raw = filestream_open(s->path, RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);
		if (s->raw)
		{
			const int64_t size = filestream_get_size(s->raw);
			s->size = size > 0 ? (u64)size : 0;
		}
	}
	if (!s->img && !s->raw)
	{
		log_cb(RETRO_LOG_ERROR, "Python 1: could not open %s\n", s->path);
		return 0;
	}
	if (s->is_cf)
	{
		s->fat_ok = fat_open_first_file(&s->fat, storage_io, s, s->size);
		if (s->fat_ok)
			log_cb(RETRO_LOG_INFO, "Python 1: the CF card's file system is %s, 0x%x bytes\n", s->fat.name, s->fat.size);
		else
			log_cb(RETRO_LOG_ERROR, "Python 1: %s has no FAT volume with a file in its root\n", s->path);
	}
	return 1;
}

/* Bytes of the disk the board sees: the HDD image, or the CF card's one
 * file. */
static int storage_rw(storage* s, u64 offset, void* buf, u32 len, int write)
{
	if (!storage_open(s))
		return 0;
	if (s->is_cf)
		return s->fat_ok && fat_rw(&s->fat, offset, buf, len, write);
	if (offset > s->size || len > s->size - offset)
		return 0;
	return storage_io(s, offset, buf, len, write);
}

/* ---- the ADPCM player --------------------------------------------- */

static s32 clamp_s32(s32 v, s32 lo, s32 hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

static void adpcm_nibble(u8 nibble, s32* step_index, s32* pcm)
{
	const s32 step = s_adpcm_steps[*step_index];
	s32 delta = (step >> 3) + ((step >> 2) & -(s32)(nibble & 1))
		+ ((step >> 1) & -(s32)((nibble >> 1) & 1)) + (step & -(s32)((nibble >> 2) & 1));
	if (nibble & 8)
		delta = -delta;
	*step_index = clamp_s32(*step_index + s_adpcm_index[nibble], 0, 48);
	*pcm = clamp_s32(*pcm + delta, -32768, 32767);
}

/* The decoder after the byte at i, from where it was before. */
static void adpcm_decode(const u8* data, u32 i, s32* step, s32* pcm)
{
	adpcm_nibble((u8)(data[i] >> 4), &step[0], &pcm[0]);
	adpcm_nibble((u8)(data[i] & 0xf), &step[1], &pcm[1]);
}

static void adpcm_stop(p1io* io)
{
	adpcm_state* a = &io->st.m.adpcm;
	const u32 vl = a->vol_l, vr = a->vol_r;
	memset(a, 0, sizeof(*a));
	a->vol_l = vl;
	a->vol_r = vr;
	free(io->adpcm_data);
	io->adpcm_data = NULL;
}

static void adpcm_request_stop(p1io* io)
{
	adpcm_state* a = &io->st.m.adpcm;
	if (!a->playing)
	{
		adpcm_stop(io);
		return;
	}
	a->stopping = 1;
	a->fade_in = 0;
	if (!a->fade_out)
		a->fade_out = ADPCM_FADE_SAMPLES;
}

/* The sound's bytes, from the disk: at a start, and after a state load. */
static int adpcm_read(p1io* io, u32 sector, int from_cf, u8* header, u8** data, u32* size)
{
	storage* s = from_cf ? &io->cf : &io->hdd;
	const u64 off = (u64)sector * SECTOR_SIZE;
	u32 n;
	*data = NULL;
	if (!storage_rw(s, off, header, ADPCM_HEADER_SIZE, 0))
	{
		log_cb(RETRO_LOG_WARN, "Python 1: ADPCM header at sector 0x%x not read\n", sector);
		return 0;
	}
	if (header[0] != 'A' || header[1] != 'D' || header[2] != 'P')
	{
		log_cb(RETRO_LOG_WARN, "Python 1: no ADPCM sound at sector 0x%x\n", sector);
		return 0;
	}
	n = be32(header + 4);
	if (!n || n > ADPCM_MAX_ENCODED || !(*data = (u8*)malloc(n)))
		return 0;
	if (!storage_rw(s, off + ADPCM_HEADER_SIZE, *data, n, 0))
	{
		log_cb(RETRO_LOG_WARN, "Python 1: ADPCM sound at sector 0x%x is cut short\n", sector);
		free(*data);
		*data = NULL;
		return 0;
	}
	*size = n;
	return 1;
}

static void adpcm_mark_loop(adpcm_state* a)
{
	if (a->position == a->loop_start && !a->loop_saved)
	{
		memcpy(a->loop_step, a->step, sizeof(a->step));
		memcpy(a->loop_pcm, a->pcm, sizeof(a->pcm));
		a->loop_saved = 1;
	}
}

static void adpcm_start(p1io* io, u32 sector)
{
	adpcm_state* a = &io->st.m.adpcm;
	const int from_cf = !io->desc.hdd_image[0];
	u8 header[ADPCM_HEADER_SIZE];
	u8* data;
	u32 size, loop_start = 0, loop_end, vl, vr;
	int looping = 0;

	if (!io->desc.hdd_image[0] && !io->desc.cf_image[0])
	{
		adpcm_stop(io);
		return;
	}
	if (!adpcm_read(io, sector, from_cf, header, &data, &size))
	{
		adpcm_stop(io);
		return;
	}
	loop_end = size;
	if (header[3] == 0x02)
	{
		const u32 ls = be32(header + 8), le = be32(header + 12);
		loop_start = ls > ADPCM_HEADER_SIZE ? ls - ADPCM_HEADER_SIZE : 0;
		loop_end = le > ADPCM_HEADER_SIZE ? min_u32(le - ADPCM_HEADER_SIZE, size) : size;
		loop_start = min_u32(loop_start, size);
		looping = loop_start < loop_end;
	}
	adpcm_stop(io);
	vl = a->vol_l;
	vr = a->vol_r;
	memset(a, 0, sizeof(*a));
	a->vol_l = vl;
	a->vol_r = vr;
	io->adpcm_data = data;
	a->sector = sector;
	a->from_cf = (u8)from_cf;
	a->size = size;
	a->loop_start = loop_start;
	a->loop_end = loop_end;
	a->looping = (u8)looping;
	a->fade_in = ADPCM_FADE_SAMPLES;
	a->playing = 1;
	adpcm_decode(data, 0, a->step, a->pcm);
	adpcm_mark_loop(a);
}

static void adpcm_advance(p1io* io)
{
	adpcm_state* a = &io->st.m.adpcm;
	const u32 end = a->looping ? a->loop_end : a->size;
	if (!a->playing)
		return;
	a->position++;
	if (a->position < end)
	{
		adpcm_decode(io->adpcm_data, a->position, a->step, a->pcm);
		adpcm_mark_loop(a);
		return;
	}
	if (a->looping && a->loop_start < a->loop_end && a->loop_saved)
	{
		a->position = a->loop_start;
		memcpy(a->step, a->loop_step, sizeof(a->step));
		memcpy(a->pcm, a->loop_pcm, sizeof(a->pcm));
		return;
	}
	adpcm_stop(io);
}

/* The sample after position's, as the player will go. */
static void adpcm_next(p1io* io, s32* out)
{
	const adpcm_state* a = &io->st.m.adpcm;
	const u32 end = a->looping ? a->loop_end : a->size;
	if (a->position + 1 < end)
	{
		s32 step[2], pcm[2];
		memcpy(step, a->step, sizeof(step));
		memcpy(pcm, a->pcm, sizeof(pcm));
		adpcm_decode(io->adpcm_data, a->position + 1, step, pcm);
		out[0] = pcm[0];
		out[1] = pcm[1];
	}
	else if (a->looping && a->loop_start < a->loop_end && a->loop_saved)
	{
		out[0] = a->loop_pcm[0];
		out[1] = a->loop_pcm[1];
	}
	else
	{
		out[0] = a->pcm[0];
		out[1] = a->pcm[1];
	}
}

static s32 adpcm_scale(float sample, u32 volume, float envelope)
{
	const float v = (float)(volume > 0xffff ? 0xffff : volume);
	return (s32)((sample * v * envelope) / 65536.0f);
}

static void p1io_mix(fw_device* dev, s32* left, s32* right)
{
	p1io* io = (p1io*)dev;
	adpcm_state* a = &io->st.m.adpcm;
	float envelope = 1.0f, sample[2], frac;
	s32 next[2];
	int stop_after = 0, ch;

	if (!a->playing || !io->adpcm_data || a->position >= a->size)
		return;

	if (a->fade_in)
	{
		envelope *= (float)(ADPCM_FADE_SAMPLES - a->fade_in) / (float)ADPCM_FADE_SAMPLES;
		a->fade_in--;
	}
	if (a->stopping)
	{
		envelope *= (float)a->fade_out / (float)ADPCM_FADE_SAMPLES;
		if (a->fade_out)
			a->fade_out--;
		if (!a->fade_out)
			stop_after = 1;
	}

	adpcm_next(io, next);
	frac = (float)a->resample / (float)ADPCM_OUTPUT_RATE;
	for (ch = 0; ch < 2; ch++)
	{
		sample[ch] = (float)a->pcm[ch] + ((float)next[ch] - (float)a->pcm[ch]) * frac;
		if (a->filter_init)
			a->filter[ch] += (sample[ch] - a->filter[ch]) * ADPCM_LOWPASS_ALPHA;
		else
			a->filter[ch] = sample[ch];
	}
	a->filter_init = 1;
	*left += adpcm_scale(a->filter[0], a->vol_l, envelope);
	*right += adpcm_scale(a->filter[1], a->vol_r, envelope);

	a->resample += ADPCM_INPUT_RATE;
	while (a->playing && a->resample >= ADPCM_OUTPUT_RATE)
	{
		a->resample -= ADPCM_OUTPUT_RATE;
		adpcm_advance(io);
	}
	if (stop_after)
		adpcm_stop(io);
}

/* ---- sector status, when the disk would have the data -------------- */

static void schedule_pending(p1io* io)
{
	const p1io_meta* m = &io->st.m;
	s32 delta;
	if (!m->npending)
	{
		fw_host_clear();
		return;
	}
	delta = (s32)(m->pending[0].ready - fw_host_cycle());
	fw_host_schedule(delta > 0 ? (u32)delta : 1);
}

static void p1io_service(fw_device* dev)
{
	p1io* io = (p1io*)dev;
	p1io_meta* m = &io->st.m;
	const u32 now = fw_host_cycle();
	u32 done = 0;

	while (done < m->npending && (s32)(m->pending[done].ready - now) <= 0)
	{
		queue_status(m->pending[done].status_offset, bswap32(m->pending[done].checksum));
		done++;
	}
	if (done)
	{
		memmove(m->pending, m->pending + done, (m->npending - done) * sizeof(m->pending[0]));
		m->npending -= done;
		fw_host_flush();
	}
	schedule_pending(io);
}

static u32 storage_read_cycles(u32 bytes)
{
	const u64 clk = PSXCLK > 0 ? (u64)PSXCLK : 36864000u;
	const u64 base = clk / 2000 ? clk / 2000 : 1; /* 0.5 ms of setup */
	u64 xfer = ((u64)bytes * clk + STORAGE_BYTES_PER_SECOND - 1) / STORAGE_BYTES_PER_SECOND;
	if (!xfer)
		xfer = 1;
	return (u32)(base + xfer > 0x7fffffffu ? 0x7fffffffu : base + xfer);
}

static void queue_read_status(p1io* io, u32 status_offset, u32 checksum, u32 bytes)
{
	p1io_meta* m = &io->st.m;
	const u32 now = fw_host_cycle();
	u32 start = now;

	if (status_offset != ST_CF && status_offset != ST_ATA)
	{
		queue_status(status_offset, bswap32(checksum));
		return;
	}
	if (m->next_ready_valid && (s32)(m->next_ready - now) > 0)
		start = m->next_ready;
	m->next_ready = start + storage_read_cycles(bytes);
	m->next_ready_valid = 1;
	if (m->npending == PENDING_MAX)
	{
		/* More reads in flight than a board has: the oldest is done. */
		queue_status(m->pending[0].status_offset, bswap32(m->pending[0].checksum));
		memmove(m->pending, m->pending + 1, (PENDING_MAX - 1) * sizeof(m->pending[0]));
		m->npending--;
	}
	m->pending[m->npending].ready = m->next_ready;
	m->pending[m->npending].status_offset = status_offset;
	m->pending[m->npending].checksum = checksum;
	m->npending++;
	schedule_pending(io);
}

static u32 read_checksum(const u8* d, u32 n)
{
	u32 sum = 0, i;
	for (i = 0; i < n; i += 2)
	{
		u32 w = (u32)d[i] << 8;
		if (i + 1 < n)
			w |= d[i + 1];
		sum = (sum + w) & 0x7fffffffu;
	}
	return sum;
}

static storage* command_storage(p1io* io, u32 command)
{
	/* HDD games read through the CF command too. */
	if (command == CMD_ATA || !io->desc.cf_image[0])
		return &io->hdd;
	return &io->cf;
}

static int read_sectors(p1io* io, storage* s, u32 sector, u32 count, u32 dest, u32 status_offset)
{
	const u64 bytes = (u64)count * SECTOR_SIZE;
	u8* data;
	int ok;
	if (!count)
		return 1;
	if (bytes > 16u * 1024 * 1024)
		return 0;
	if (!(data = (u8*)malloc((size_t)bytes)))
		return 0;
	ok = storage_rw(s, (u64)sector * SECTOR_SIZE, data, (u32)bytes, 0);
	if (!ok)
		log_cb(RETRO_LOG_WARN, "Python 1: sectors 0x%x+0x%x not read\n", sector, count);
	/* The data goes into IOP RAM as the board's DMA would put it; only the
	 * status is a packet. */
	if (ok && !fw_host_iop_write(dest, data, (u32)bytes))
		ok = 0;
	if (ok)
		queue_read_status(io, status_offset, read_checksum(data, (u32)bytes), (u32)bytes);
	free(data);
	return ok;
}

static int write_sectors(storage* s, u32 sector, u32 count, u32 source, u32 status_offset)
{
	const u64 bytes = (u64)count * SECTOR_SIZE;
	u8* data;
	int ok;
	if (!count)
	{
		queue_status(status_offset, 0);
		return 1;
	}
	if (bytes > 16u * 1024 * 1024 || !(data = (u8*)malloc((size_t)bytes)))
		return 0;
	ok = fw_host_iop_read(source, data, (u32)bytes) && storage_rw(s, (u64)sector * SECTOR_SIZE, data, (u32)bytes, 1);
	if (!ok)
		log_cb(RETRO_LOG_WARN, "Python 1: sectors 0x%x+0x%x not written\n", sector, count);
	else
		queue_status(status_offset, 0);
	free(data);
	return ok;
}

/* ---- the panel and the keyboard ----------------------------------- */

typedef struct pad_bind
{
	u8 port, id, bind;
} pad_bind;

#define PB_END { 0xff, 0, 0 }

static const pad_bind s_bind_common[] = {
	{ 0, RETRO_DEVICE_ID_JOYPAD_L3, B_TEST }, { 0, RETRO_DEVICE_ID_JOYPAD_R3, B_SERVICE },
	{ 0, RETRO_DEVICE_ID_JOYPAD_SELECT, B_COIN1 }, { 1, RETRO_DEVICE_ID_JOYPAD_SELECT, B_COIN2 },
	PB_END
};

#define JVS_PLAYER(p, o) \
	{ p, RETRO_DEVICE_ID_JOYPAD_START, B_P1_START + o }, { p, RETRO_DEVICE_ID_JOYPAD_UP, B_P1_UP + o }, \
	{ p, RETRO_DEVICE_ID_JOYPAD_DOWN, B_P1_DOWN + o }, { p, RETRO_DEVICE_ID_JOYPAD_LEFT, B_P1_LEFT + o }, \
	{ p, RETRO_DEVICE_ID_JOYPAD_RIGHT, B_P1_RIGHT + o }, { p, RETRO_DEVICE_ID_JOYPAD_B, B_P1_1 + o }, \
	{ p, RETRO_DEVICE_ID_JOYPAD_A, B_P1_2 + o }, { p, RETRO_DEVICE_ID_JOYPAD_Y, B_P1_3 + o }, \
	{ p, RETRO_DEVICE_ID_JOYPAD_X, B_P1_4 + o }, { p, RETRO_DEVICE_ID_JOYPAD_L, B_P1_5 + o }, \
	{ p, RETRO_DEVICE_ID_JOYPAD_R, B_P1_6 + o }

static const pad_bind s_bind_jvs[] = { JVS_PLAYER(0, 0), JVS_PLAYER(1, B_P2_START - B_P1_START), PB_END };

/* The foot panels on the d-pad and the face buttons both. */
#define DDR_PLAYER(p, o) \
	{ p, RETRO_DEVICE_ID_JOYPAD_START, B_P1_START + o }, \
	{ p, RETRO_DEVICE_ID_JOYPAD_UP, B_P1_UP + o }, { p, RETRO_DEVICE_ID_JOYPAD_X, B_P1_UP + o }, \
	{ p, RETRO_DEVICE_ID_JOYPAD_DOWN, B_P1_DOWN + o }, { p, RETRO_DEVICE_ID_JOYPAD_B, B_P1_DOWN + o }, \
	{ p, RETRO_DEVICE_ID_JOYPAD_LEFT, B_P1_LEFT + o }, { p, RETRO_DEVICE_ID_JOYPAD_Y, B_P1_LEFT + o }, \
	{ p, RETRO_DEVICE_ID_JOYPAD_RIGHT, B_P1_RIGHT + o }, { p, RETRO_DEVICE_ID_JOYPAD_A, B_P1_RIGHT + o }, \
	{ p, RETRO_DEVICE_ID_JOYPAD_L, B_P1_1 + o }, { p, RETRO_DEVICE_ID_JOYPAD_R, B_P1_2 + o }

static const pad_bind s_bind_extio[] = { DDR_PLAYER(0, 0), DDR_PLAYER(1, B_P2_START - B_P1_START), PB_END };

/* pop'n music's nine buttons, where Reliquary puts them on a pad. */
static const pad_bind s_bind_popn[] = {
	{ 0, RETRO_DEVICE_ID_JOYPAD_START, B_P1_START },
	{ 0, RETRO_DEVICE_ID_JOYPAD_X, B_P1_UP }, { 0, RETRO_DEVICE_ID_JOYPAD_A, B_P1_DOWN },
	{ 0, RETRO_DEVICE_ID_JOYPAD_R, B_P1_LEFT }, { 0, RETRO_DEVICE_ID_JOYPAD_B, B_P1_RIGHT },
	{ 0, RETRO_DEVICE_ID_JOYPAD_L, B_P1_1 }, { 0, RETRO_DEVICE_ID_JOYPAD_Y, B_P2_UP },
	{ 0, RETRO_DEVICE_ID_JOYPAD_R2, B_P2_DOWN }, { 0, RETRO_DEVICE_ID_JOYPAD_UP, B_P2_LEFT },
	{ 0, RETRO_DEVICE_ID_JOYPAD_L2, B_P2_RIGHT },
	PB_END
};

static const pad_bind* mode_binds(int mode)
{
	switch (mode)
	{
		case P1IO_MODE_EXTIO: return s_bind_extio;
		case P1IO_MODE_POPN:  return s_bind_popn;
		default:              return s_bind_jvs;
	}
}

/* The keyboard, as PS/2 scan code set 2: make codes, 0x100 for the E0
 * ones. */
typedef struct key_map
{
	u16 retrok;
	u16 code;
} key_map;

#define E0 0x100
static const key_map s_keys[] = {
	{ RETROK_a, 0x1c }, { RETROK_b, 0x32 }, { RETROK_c, 0x21 }, { RETROK_d, 0x23 }, { RETROK_e, 0x24 },
	{ RETROK_f, 0x2b }, { RETROK_g, 0x34 }, { RETROK_h, 0x33 }, { RETROK_i, 0x43 }, { RETROK_j, 0x3b },
	{ RETROK_k, 0x42 }, { RETROK_l, 0x4b }, { RETROK_m, 0x3a }, { RETROK_n, 0x31 }, { RETROK_o, 0x44 },
	{ RETROK_p, 0x4d }, { RETROK_q, 0x15 }, { RETROK_r, 0x2d }, { RETROK_s, 0x1b }, { RETROK_t, 0x2c },
	{ RETROK_u, 0x3c }, { RETROK_v, 0x2a }, { RETROK_w, 0x1d }, { RETROK_x, 0x22 }, { RETROK_y, 0x35 },
	{ RETROK_z, 0x1a },
	{ RETROK_0, 0x45 }, { RETROK_1, 0x16 }, { RETROK_2, 0x1e }, { RETROK_3, 0x26 }, { RETROK_4, 0x25 },
	{ RETROK_5, 0x2e }, { RETROK_6, 0x36 }, { RETROK_7, 0x3d }, { RETROK_8, 0x3e }, { RETROK_9, 0x46 },
	{ RETROK_RETURN, 0x5a }, { RETROK_ESCAPE, 0x76 }, { RETROK_BACKSPACE, 0x66 }, { RETROK_TAB, 0x0d },
	{ RETROK_SPACE, 0x29 }, { RETROK_MINUS, 0x4e }, { RETROK_EQUALS, 0x55 }, { RETROK_AT, 0x54 },
	{ RETROK_LEFTBRACKET, 0x5b }, { RETROK_RIGHTBRACKET, 0x5d }, { RETROK_SEMICOLON, 0x4c },
	{ RETROK_QUOTE, 0x52 }, { RETROK_BACKQUOTE, 0x0e }, { RETROK_COMMA, 0x41 }, { RETROK_PERIOD, 0x49 },
	{ RETROK_SLASH, 0x4a }, { RETROK_CARET, 0x55 }, { RETROK_COLON, 0x52 }, { RETROK_BACKSLASH, 0x51 },
	{ RETROK_F1, 0x05 }, { RETROK_F2, 0x06 }, { RETROK_F3, 0x04 }, { RETROK_F4, 0x0c }, { RETROK_F5, 0x03 },
	{ RETROK_F6, 0x0b }, { RETROK_F7, 0x83 }, { RETROK_F8, 0x0a }, { RETROK_F9, 0x01 }, { RETROK_F10, 0x09 },
	{ RETROK_F11, 0x78 }, { RETROK_F12, 0x07 },
	{ RETROK_LSHIFT, 0x12 }, { RETROK_RSHIFT, 0x59 }, { RETROK_LCTRL, 0x14 }, { RETROK_LALT, 0x11 },
	{ RETROK_UP, 0xf5 }, { RETROK_DOWN, 0xf2 }, { RETROK_LEFT, 0xeb }, { RETROK_RIGHT, 0xf4 },
	{ RETROK_INSERT, E0 | 0x70 }, { RETROK_DELETE, E0 | 0x71 }, { RETROK_HOME, E0 | 0x6c },
	{ RETROK_END, E0 | 0x69 }, { RETROK_PAGEUP, E0 | 0x7d }, { RETROK_PAGEDOWN, E0 | 0x7a },
	{ RETROK_CAPSLOCK, 0x58 },
	{ RETROK_KP7, E0 | 0xec }, { RETROK_KP8, E0 | 0xf5 }, { RETROK_KP9, E0 | 0xfd },
	{ RETROK_KP4, E0 | 0xeb }, { RETROK_KP5, E0 | 0xf3 }, { RETROK_KP6, 0x74 },
	{ RETROK_KP1, E0 | 0xe9 }, { RETROK_KP2, E0 | 0xf2 }, { RETROK_KP3, E0 | 0xfa },
	{ RETROK_KP0, 0x70 }, { RETROK_KP_ENTER, 0xda }
};

static void key_event(p1io* io, u8 byte)
{
	p1io_meta* m = &io->st.m;
	if (m->key_count == KEY_EVENTS)
		return;
	io->st.key_events[(m->key_head + m->key_count) % KEY_EVENTS] = byte;
	m->key_count++;
}

static void read_keyboard(p1io* io, retro_input_state_t in)
{
	p1io_meta* m = &io->st.m;
	u32 i;
	for (i = 0; i < sizeof(s_keys) / sizeof(s_keys[0]); i++)
	{
		const u32 code = s_keys[i].code;
		const u8 bit = (u8)(1u << (code & 7));
		const int was = (m->key_pressed[code >> 3] & bit) != 0;
		const int now = in(0, RETRO_DEVICE_KEYBOARD, 0, s_keys[i].retrok) != 0;
		if (now == was)
			continue;
		/* Two host keys on one code: the code is down while either is. */
		if (!now)
		{
			u32 j;
			int other = 0;
			for (j = 0; j < sizeof(s_keys) / sizeof(s_keys[0]); j++)
				if (j != i && s_keys[j].code == code && in(0, RETRO_DEVICE_KEYBOARD, 0, s_keys[j].retrok))
					other = 1;
			if (other)
				continue;
		}
		if (code & E0)
			key_event(io, 0xe0);
		if (now)
			m->key_pressed[code >> 3] |= bit;
		else
		{
			m->key_pressed[code >> 3] &= (u8)~bit;
			key_event(io, 0xf0);
		}
		key_event(io, (u8)code);
	}
}

/* The panel (and in a serial mode the keyboard) as they are now. */
static void read_input(p1io* io)
{
	retro_input_state_t in = io->input ? io->input() : NULL;
	const pad_bind* b;
	u32 bits = 0;
	if (!in)
		return;
	for (b = s_bind_common; b->port != 0xff; b++)
		if (in(b->port, RETRO_DEVICE_JOYPAD, 0, b->id))
			bits |= BIT(b->bind);
	for (b = mode_binds(io->desc.io_mode); b->port != 0xff; b++)
		if (in(b->port, RETRO_DEVICE_JOYPAD, 0, b->id))
			bits |= BIT(b->bind);
	io->st.m.bind_state = bits;
	if (serial_mode(io))
		read_keyboard(io, in);
}

static u32 pop_key_events(p1io* io, u8* dst, u32 max)
{
	p1io_meta* m = &io->st.m;
	u32 n = 0;
	while (n < max && m->key_count)
	{
		dst[n++] = io->st.key_events[m->key_head];
		m->key_head = (m->key_head + 1) % KEY_EVENTS;
		m->key_count--;
	}
	return n;
}

/* Pressed is a low bit, for the first fifteen. */
static u32 jamma_status(const p1io* io)
{
	static const u32 masks[15] = {
		0x0001, 0x0002, 0x0004, 0x0008, 0x0010, 0x0020, 0x0040, 0x0080,
		0x0100, 0x0200, 0x0400, 0x0800, 0x1000, 0x2000, 0x4000
	};
	u32 status = JAMMA_NEUTRAL, b;
	for (b = 0; b < 15; b++)
		if (io->st.m.bind_state & BIT(b))
			status &= ~(masks[b] & JAMMA_ACTIVE_LOW);
	return status;
}

static void count_coins(p1io* io)
{
	p1io_meta* m = &io->st.m;
	const u32 pressed = m->bind_state & ~m->last_bind;
	if (pressed & BIT(B_COIN1))
		m->coin[0]++;
	if (pressed & BIT(B_COIN2))
		m->coin[1]++;
	m->last_bind = m->bind_state;
}

static u32 source_bits(const p1io* io)
{
	static const struct { u8 bind; u32 bit; } jvs[] = {
		{ B_TEST, SRC_TEST }, { B_SERVICE, SRC_SERVICE },
		{ B_P1_START, SRC_P1_START }, { B_P1_UP, SRC_P1_UP }, { B_P1_DOWN, SRC_P1_DOWN },
		{ B_P1_LEFT, SRC_P1_LEFT }, { B_P1_RIGHT, SRC_P1_RIGHT }, { B_P1_1, SRC_P1_BUTTON1 },
		{ B_P1_2, SRC_P1_BUTTON2 }, { B_P1_3, SRC_P1_BUTTON3 }, { B_P1_4, SRC_P1_BUTTON4 },
		{ B_P1_5, SRC_P1_BUTTON5 }, { B_P1_6, SRC_P1_BUTTON6 },
		{ B_P2_START, SRC_P2_START }, { B_P2_UP, SRC_P2_UP }, { B_P2_DOWN, SRC_P2_DOWN },
		{ B_P2_LEFT, SRC_P2_LEFT }, { B_P2_RIGHT, SRC_P2_RIGHT }, { B_P2_1, SRC_P2_BUTTON1 },
		{ B_P2_2, SRC_P2_BUTTON2 }, { B_P2_3, SRC_P2_BUTTON3 }, { B_P2_4, SRC_P2_BUTTON4 },
		{ B_P2_5, SRC_P2_BUTTON5 }, { B_P2_6, SRC_P2_BUTTON6 }
	};
	/* DDR: the foot panel's own bits, the selector on buttons 2 and 3. */
	static const struct { u8 bind; u32 bit; } ddr[] = {
		{ B_TEST, SRC_TEST }, { B_SERVICE, SRC_SERVICE },
		{ B_P1_START, SRC_P1_START }, { B_P1_UP, SRC_P1_UP }, { B_P1_DOWN, SRC_P1_DOWN },
		{ B_P1_LEFT, SRC_P1_LEFT }, { B_P1_RIGHT, SRC_P1_RIGHT },
		{ B_P1_1, SRC_P1_BUTTON2 }, { B_P1_2, SRC_P1_BUTTON3 },
		{ B_P2_START, SRC_P2_START }, { B_P2_UP, SRC_P2_UP }, { B_P2_DOWN, SRC_P2_DOWN },
		{ B_P2_LEFT, SRC_P2_LEFT }, { B_P2_RIGHT, SRC_P2_RIGHT },
		{ B_P2_1, SRC_P2_BUTTON2 }, { B_P2_2, SRC_P2_BUTTON3 }
	};
	/* pop'n: switches 1-9 on these. */
	static const struct { u8 bind; u32 bit; } popn[] = {
		{ B_TEST, SRC_TEST }, { B_SERVICE, SRC_SERVICE }, { B_P1_START, SRC_P1_START },
		{ B_P1_UP, SRC_P1_UP }, { B_P1_DOWN, SRC_P1_DOWN }, { B_P1_LEFT, SRC_P1_LEFT },
		{ B_P1_RIGHT, SRC_P1_RIGHT }, { B_P1_1, SRC_P1_BUTTON1 }, { B_P2_UP, SRC_P2_UP },
		{ B_P2_DOWN, SRC_P2_DOWN }, { B_P2_LEFT, SRC_P2_LEFT }, { B_P2_RIGHT, SRC_P2_RIGHT }
	};
	const u32 st = io->st.m.bind_state;
	u32 bits = 0, i;
	switch (io->desc.io_mode)
	{
		case P1IO_MODE_POPN:
			for (i = 0; i < sizeof(popn) / sizeof(popn[0]); i++)
				if (st & BIT(popn[i].bind))
					bits |= popn[i].bit;
			break;
		case P1IO_MODE_EXTIO:
			for (i = 0; i < sizeof(ddr) / sizeof(ddr[0]); i++)
				if (st & BIT(ddr[i].bind))
					bits |= ddr[i].bit;
			break;
		default:
			for (i = 0; i < sizeof(jvs) / sizeof(jvs[0]); i++)
				if (st & BIT(jvs[i].bind))
					bits |= jvs[i].bit;
			break;
	}
	return bits;
}

/* ---- serial peripherals ------------------------------------------- */

/* The ACIO checksum: every byte after the first. */
static u8 acio_sum(const u8* d, u32 n)
{
	u8 sum = 0;
	u32 i;
	for (i = 1; i < n; i++)
		sum = (u8)(sum + d[i]);
	return sum;
}

static void uart_queue(p1io* io, const u8* d, u32 n)
{
	p1io_meta* m = &io->st.m;
	if (!n)
		return;
	if (n >= UART_RX_LIMIT)
	{
		d += n - UART_RX_LIMIT;
		n = UART_RX_LIMIT;
	}
	if (m->uart_len + n > UART_RX_LIMIT)
	{
		/* The oldest bytes go: the IOP is not reading. */
		const u32 drop = m->uart_len + n - UART_RX_LIMIT;
		memmove(io->st.uart, io->st.uart + drop, m->uart_len - drop);
		m->uart_len -= drop;
	}
	memcpy(io->st.uart + m->uart_len, d, n);
	m->uart_len += n;
}

static void acio_respond(p1io* io, const u8* d, u32 n)
{
	u8 packet[64], out[128];
	u32 i, k = 0;
	memcpy(packet, d, n);
	packet[n] = acio_sum(packet, n);
	for (i = 0; i <= n; i++)
	{
		if (packet[i] == 0xaa || packet[i] == 0xff)
		{
			out[k++] = 0xff;
			out[k++] = (u8)~packet[i];
		}
		else
			out[k++] = packet[i];
	}
	uart_queue(io, out, k);
}

/* The request, the board's header, the response and its checksum. */
static void extio_wrapped(p1io* io, const u8* req, u32 req_n, const u8* resp, u32 resp_n)
{
	u8* p = io->scratch + UART_WRITE_LIMIT;
	u32 n = 0;
	memcpy(p, req, req_n);
	n += req_n;
	p[n++] = 0xaa;
	p[n++] = (u8)(serial_mode(io) ? 1 : 0);
	p[n++] = req[2];
	p[n++] = req[3];
	memcpy(p + n, resp, resp_n);
	n += resp_n;
	p[n] = acio_sum(p + req_n, resp_n + 4);
	n++;
	uart_queue(io, p, n);
}

static void uart_write(p1io* io, const u32* payload, u32 quads, u32 n, int extio)
{
	u8* b = io->scratch;
	const u32 inline_bytes = quads > 2 ? (quads - 2) * 4 : 0;
	const int serial = serial_mode(io);
	const int b22 = io->desc.io_mode == P1IO_MODE_B22;
	u32 i;

	if (n > UART_WRITE_LIMIT)
	{
		log_cb(RETRO_LOG_WARN, "Python 1: a UART write of 0x%x bytes is too long\n", n);
		return;
	}
	if (n > inline_bytes)
	{
		const u32 src = quads > 2 ? payload[2] : 0;
		if (!src || !fw_host_iop_read(src, b, n))
			return;
	}
	else
		for (i = 0; i < n; i++)
			b[i] = (u8)(payload[2 + i / 4] >> (24 - (i & 3) * 8));

#define AT(k, v) (n > (k) && b[k] == (v))
	if (n >= 4 && AT(0, 0xaa) && AT(1, 0xaa) && AT(2, 0xaa) && AT(3, 0x55))
		uart_queue(io, s_extio_sync, sizeof(s_extio_sync));
	else if (n >= 4 && AT(0, 0xaa) && AT(1, 0xaa) && AT(2, 0x00) && AT(3, 0x00))
		uart_queue(io, s_extio_reset, sizeof(s_extio_reset));
	else if (n >= 4 && AT(0, 0xaa) && AT(1, 0xaa) && AT(2, 0x00) && AT(3, 0x01))
	{
		if (b22)
			uart_queue(io, s_b22_count, sizeof(s_b22_count));
		else if (io->desc.io_mode == P1IO_MODE_DOGSTATIONDX)
			uart_queue(io, s_dogstation_count, sizeof(s_dogstation_count));
		else
			uart_queue(io, s_extio_count, sizeof(s_extio_count));
	}
	else if (serial && n >= 6 && AT(0, 0xaa) && AT(1, 0xaa) && b[2] >= 1 && b[2] <= 2 && b[3] == 0x02)
	{
		/* B22's EE parser reads bytes 15..26 as flags and the B22C.zin
		 * version stamp. */
		u8 r[28] = {
			0xaa, 0xaa, 0, 0x02, 0x00, 0x00,
			0xaa, 0xa5, 0, 0x02, 0x05,
			'C', 'R', '-', '2', 0x36, 0x01, 0x00, 0x07, 'B', '2', '2', 'C', 0x00, 0x00, 0x02, 0x01,
			0x00
		};
		r[2] = r[8] = b[2];
		r[5] = acio_sum(r, 5);
		r[27] = acio_sum(r + 6, 21);
		uart_queue(io, r, sizeof(r));
	}
	else if (serial && n >= 6 && AT(0, 0xaa) && AT(1, 0xaa) && b[2] >= 1 && b[2] <= 2 && (b[3] == 0x03 || b[3] == 0x04))
	{
		u8 r[13];
		r[0] = 0xaa; r[1] = 0xaa; r[2] = b[2]; r[3] = b[3]; r[4] = 0;
		r[5] = (u8)(0xaa + b[2] + b[3]);
		r[6] = 0xaa; r[7] = 0xa5; r[8] = b[2]; r[9] = b[3]; r[10] = 1; r[11] = 0;
		r[12] = (u8)(0xa6 + b[2] + b[3]);
		uart_queue(io, r, sizeof(r));
	}
	else if (!serial && n >= 6 && AT(0, 0xaa) && AT(1, 0xaa) && AT(2, 0x01) && AT(3, 0x02))
		uart_queue(io, s_extio_product, sizeof(s_extio_product));
	else if (!serial && n >= 6 && AT(0, 0xaa) && AT(1, 0xaa) && AT(2, 0x01) && AT(3, 0x03))
		uart_queue(io, s_extio_startup, sizeof(s_extio_startup));
	else if (extio && n >= 5 && AT(0, 0xaa) && AT(1, 0x01) && AT(2, 0x00) && AT(3, 0x02))
		acio_respond(io, s_icca_product, sizeof(s_icca_product));
	else if (extio && n >= 5 && AT(0, 0xaa) && AT(1, 0x01) && AT(2, 0x00) && AT(3, 0x03))
		acio_respond(io, s_icca_startup, sizeof(s_icca_startup));
	else if (extio && n >= 5 && AT(0, 0xaa) && AT(1, 0x01) && AT(2, 0x01) && (b[3] == 0x31 || b[3] == 0x34 || b[3] == 0x35))
		acio_respond(io, s_icca_status, sizeof(s_icca_status));
	else if (extio && n >= 4 && ((b[0] + b[1] + b[2]) & 0x7f) == b[3])
	{
		/* DDR's foot-panel packets: four raw bytes, not ACIO framed. */
		static const u8 ack = 0x11;
		uart_queue(io, &ack, 1);
	}
	else if (b22 && n >= 6 && AT(0, 0xaa) && AT(1, 0xff))
	{
		u8 r[0x4c];
		memset(r, 0, sizeof(r));
		r[0] = 0xaa; r[1] = 0xff; r[2] = b[2]; r[3] = 0x02; r[4] = 0x00; r[5] = (u8)(b[2] + 1);
		r[6] = 0xaa; r[7] = 0xf0; r[8] = b[2]; r[9] = 0x02; r[10] = 0x07;
		memset(r + 11, 0xff, 7);
		uart_queue(io, r, sizeof(r));
	}
	else if (n >= 6 && AT(0, 0xaa) && AT(1, 0x00))
	{
		u32 size;
		u8 cmd;
		u8 ok[2];
		for (size = 6; size <= n; size++)
			if (acio_sum(b, size - 1) == b[size - 1])
				break;
		if (size > n)
			return;
		cmd = serial ? (u8)(b[3] & ~0x40) : b[3];
		ok[0] = (u8)(serial ? 1 : 0);
		ok[1] = (u8)(serial && b[2] != 1 ? 1 : 0);
		switch (cmd)
		{
			case 0x00: case 0x01: case 0x10: case 0x11: case 0x12:
			case 0x14: case 0x15: case 0x16: case 0x1e: case 0x1f:
				extio_wrapped(io, b, size, ok, sizeof(ok));
				break;
			case 0x18:
			{
				static const u8 empty_card[0x82];
				extio_wrapped(io, b, size, empty_card, sizeof(empty_card));
				break;
			}
			case 0x26:
			case 0x36:
			{
				u8 status[9];
				if (!serial)
					break;
				memset(status, 0, sizeof(status));
				status[0] = 0x04;
				if (b[2] == 0x01 && cmd == 0x26)
				{
					read_input(io);
					pop_key_events(io, status + 1, 8);
				}
				extio_wrapped(io, b, size, status, sizeof(status));
				break;
			}
			default:
				break;
		}
	}
#undef AT
}

/* What the board has received, in a callback block to the IOP. */
static int uart_read(p1io* io, u32 requested)
{
	p1io_meta* m = &io->st.m;
	const u32 n = min_u32(requested, m->uart_len);
	u32 cb[2 + (UART_CALLBACK_BYTE_LIMIT + 1) / 2];
	const u32 quads = 2 + (n + 1) / 2;
	u32 i;
	if (!n)
		return 0;
	memset(cb, 0, sizeof(cb));
	cb[1] = (n << 16) | io->st.uart[0];
	for (i = 1; i < n; i++)
	{
		u32* w = &cb[2 + (i - 1) / 2];
		if (((i - 1) & 1) == 0)
			*w |= (u32)io->st.uart[i] << 16;
		else
			*w |= io->st.uart[i];
	}
	for (i = 0; i < quads; i++)
		cb[i] = bswap32(cb[i]);
	fw_host_queue_block(0xfffe, ST_UART, cb, quads);
	memmove(io->st.uart, io->st.uart + n, m->uart_len - n);
	m->uart_len -= n;
	return 1;
}

/* ---- commands ----------------------------------------------------- */

static int cmd_uart(p1io* io, const u32* p, u32 q)
{
	const u32 subop = p[0];
	const u32 word1 = q > 1 ? p[1] : 0;
	const int extio = io->desc.io_mode == P1IO_MODE_EXTIO;
	int read = 0;
	if (subop == 1 && word1)
		uart_write(io, p, q, word1, extio);
	else if (subop == 2 && (extio || io->st.m.uart_len))
		read = uart_read(io, UART_CALLBACK_BYTE_LIMIT);
	queue_status(ST_UART, bswap32(2));
	if ((extio && subop == 2) || read)
		queue_status(ST_UART, bswap32(1));
	return 1;
}

static int cmd_dallas(p1io* io, const u32* p, u32 q)
{
	const u32 subop = p[0];
	const u32 key = q > 1 ? p[1] : 0;
	const u32 off = q > 2 ? p[2] : 0;
	const u32 n = q > 3 ? p[3] : 0;
	const u32 dest = q > 4 ? p[4] : 0;
	const int have = key < 2 && io->st.m.dallas_loaded[key];

	if (subop == 0)
	{
		if (have)
		{
			const u8* k = io->st.dallas[key];
			u32 r[3];
			/* The serial goes out of the status buffer as native words. */
			r[0] = 0;
			r[1] = (u32)k[0] | ((u32)k[1] << 8) | ((u32)k[2] << 16) | ((u32)k[3] << 24);
			r[2] = (u32)k[4] | ((u32)k[5] << 8) | ((u32)k[6] << 16) | ((u32)k[7] << 24);
			fw_host_queue_block(0xfffe, ST_DALLAS, r, 3);
		}
		else
			fw_host_queue_block(0xfffe, ST_DALLAS, s_dallas_no_key, 3);
		return 1;
	}
	if ((subop == 1 || subop == 2) && (!have || off > DALLAS_PAYLOAD || n > DALLAS_PAYLOAD - off))
	{
		queue_status(ST_DALLAS, s_dallas_no_key[0]);
		return 1;
	}
	if (subop == 1)
	{
		if (n && dest)
			fw_host_queue_bytes(0x1000, dest, io->st.dallas[key] + DALLAS_SERIAL + off, n);
		queue_status(ST_DALLAS, 0);
		return 1;
	}
	if (subop == 2)
	{
		if (n && dest && !fw_host_iop_read(dest, io->st.dallas[key] + DALLAS_SERIAL + off, n))
		{
			queue_status(ST_DALLAS, s_dallas_no_key[0]);
			return 1;
		}
		io->st.m.dallas_dirty[key] = 1;
		save_dallas(io);
		queue_status(ST_DALLAS, 0);
		return 1;
	}
	queue_status(ST_DALLAS, s_dallas_no_key[0]);
	return 1;
}

static void queue_memory(u32 dest, const u8* src, u32 n)
{
	u32 off;
	for (off = 0; off < n; off += DBUF_WRITEB_MAX)
		fw_host_queue_bytes(0x1000, dest + off, src + off, min_u32(DBUF_WRITEB_MAX, n - off));
}

static int cmd_bbsram(p1io* io, const u32* p, u32 q)
{
	u32 subop, off, n, dest;
	if (q < 4)
		return 0;
	subop = p[0];
	off = p[1];
	n = p[2];
	dest = p[3];
	if (off > BBSRAM_SIZE || n > BBSRAM_SIZE - off)
		return 0;
	if (subop == 0)
	{
		if (n && dest)
			queue_memory(dest, io->st.bbsram + off, n);
		queue_status(ST_BBSRAM, 0);
		return 1;
	}
	if (subop == 1)
	{
		if (n)
		{
			int copied = dest && fw_host_iop_read(dest, io->st.bbsram + off, n);
			if (!copied)
			{
				const u32 start = dest ? 3 : 4;
				const u32 inline_bytes = q > start ? (q - start) * 4 : 0;
				u32 i;
				if (n > inline_bytes)
					return 0;
				for (i = 0; i < n; i++)
					io->st.bbsram[off + i] = (u8)(p[start + i / 4] >> (24 - (i & 3) * 8));
			}
			/* The boot test's scratch write is not the game's data. */
			if (!(off == 0 && n == BBSRAM_VOLATILE_TEST))
			{
				io->st.m.bbsram_dirty = 1;
				save_bbsram(io);
			}
		}
		queue_status(ST_BBSRAM, 0);
		return 1;
	}
	return 0;
}

static int cmd_bootrom(p1io* io, const u32* p, u32 q)
{
	u32 subop, off, n, dest;
	if (q < 4)
		return 0;
	subop = p[0];
	off = p[1];
	n = p[2];
	dest = p[3];
	if (off <= BOOTROM_SIZE && n <= BOOTROM_SIZE - off && n && dest)
	{
		if (subop == 0)
			queue_memory(dest, io->st.bootrom + off, n);
		else if (subop == 1 && fw_host_iop_read(dest, io->st.bootrom + off, n))
		{
			io->st.m.bootrom_dirty = 1;
			save_bootrom(io);
			dallas_key_from_bootrom(io);
		}
	}
	queue_status(ST_BOOTROM, 0x41000000);
	return 1;
}

static int cmd_fsci(p1io* io, const u32* p, u32 q)
{
	const u32 subop = p[0];
	const u32 n = q > 1 ? p[1] : 0;
	const u32 dest = q > 2 ? p[2] : 0;
	if (subop == 0)
	{
		io->st.m.fsci_offset = 0;
		fsci_build(io);
	}
	else if (subop == 2 && n && dest && io->desc.io_mode != P1IO_MODE_POPN)
	{
		/* pop'n takes a MAC it parses for a card probe that worked, and
		 * skips the backup RAM's set-up: it gets none. */
		const u32 left = io->st.m.fsci_offset < FSCI_STREAM ? FSCI_STREAM - io->st.m.fsci_offset : 0;
		const u32 bytes = min_u32(min_u32(n, DBUF_WRITEB_MAX), left);
		if (fw_host_iop_write(dest, io->st.fsci + io->st.m.fsci_offset, bytes))
		{
			io->st.m.fsci_offset += bytes;
			queue_status(ST_FSCI, bswap32(bytes));
			return 1;
		}
	}
	queue_status(ST_FSCI, 0);
	return 1;
}

static int cmd_adpcm(p1io* io, const u32* p, u32 q)
{
	const u32 words = q ? min_u32(p[0] + 1, q - 1) : 0;
	u32 i, hi = 0, lo = 0;
	int have_hi = 0, have_lo = 0;
	for (i = 0; i < words; i++)
	{
		const u32 w = p[i + 1];
		switch (w >> 24)
		{
			case 0x00:
				if (!w)
					adpcm_request_stop(io);
				break;
			case 0x02:
				hi = w & 0xffff;
				have_hi = 1;
				break;
			case 0x03:
				lo = w & 0xffff;
				have_lo = 1;
				break;
			case 0x05:
				io->st.m.adpcm.vol_l = w & 0xffff;
				break;
			case 0x06:
				io->st.m.adpcm.vol_r = w & 0xffff;
				break;
			default:
				break;
		}
	}
	if (have_hi && have_lo)
		adpcm_start(io, (hi << 16) | lo);
	queue_status(ST_ADPCM, 0);
	return 1;
}

/* The network is offline: the board answers its RPCs with no peer. */
static int cmd_net(p1io* io, u32 command_offset, const u32* p, u32 q)
{
	u32 rel, ch, cmd, r[8], prop_n = 0;
	if (command_offset < CMD_NET_BASE)
		return 0;
	rel = command_offset - CMD_NET_BASE;
	ch = rel / CMD_NET_STRIDE;
	if (rel % CMD_NET_STRIDE || ch >= NET_CHANNELS)
		return 0;
	cmd = p[0];
	switch (cmd)
	{
		case 0: case 1: case 2: case 3: case 5: case 6: case 7: case 0x0b: case 0x0c:
		case 0x13: case 0x14: case 0x15: case 0x16: case 0x17: case 0x1d: case 0x1e:
			break;
		default:
			return 0;
	}
	if (cmd == 0x1e)
	{
		const u8* data = NULL;
		u32 n = 0;
		if (q < 4)
			return 0;
		switch (p[1])
		{
			case 0x101:
			{
				/* An address of its own on the cabinet's link. */
				u32 id = io->st.bbsram[BBSRAM_NETWORK_ID];
				if (id < 1 || id > 6)
					id = 1;
				io->st.m.net_prop[0] = 192;
				io->st.m.net_prop[1] = 168;
				io->st.m.net_prop[2] = 1;
				io->st.m.net_prop[3] = (u8)(10 + id);
				data = io->st.m.net_prop;
				n = 4;
				break;
			}
			case 1:    data = s_net_subnet; n = 4; break;
			case 3:    data = s_net_zero; n = 4; break;
			case 6:    data = s_net_zero; n = 4; break;
			case 0xf:  data = s_net_machine_id; n = sizeof(s_net_machine_id); break;
			case 0x2a: data = io->st.m.factory_mac; n = 6; break;
			case 0x36: data = s_net_zero; n = 4; break;
			default:   return 0;
		}
		prop_n = min_u32(p[3], n);
		fw_host_queue_bytes(0x1000, p[2], data, prop_n);
	}
	else if ((cmd == 0x14 || cmd == 0x15) && q < 4)
		return 0;

	memset(r, 0, sizeof(r));
	if (cmd == 7)
		r[1] = 1;
	else if (cmd == 0x13 || cmd == 0x14 || cmd == 0x15)
		r[1] = bswap32(q > 3 ? p[3] : 0);
	else if (cmd == 0x1e)
		r[1] = bswap32(prop_n);
	fw_host_queue_block(0xfffe, ST_NET_BASE + ch * ST_NET_STRIDE, r, 8);
	return 1;
}

static int cmd_jamma(p1io* io, u32 command_offset, const u32* p)
{
	p1io_meta* m = &io->st.m;
	u32 status;
	if (command_offset == CMD_JAMMA_INIT)
	{
		u32 init[JAMMA_REPORT_QUADS], attach[JAMMA_REPORT_QUADS], i;
		read_input(io);
		count_coins(io);
		status = jamma_status(io);
		m->jamma_dest = p[0];
		m->last_jamma = status;
		init[0] = 0xfe4b109c; init[1] = 0; init[2] = 0; init[3] = 0;
		init[4] = JAMMA_JVS_PRESENT; init[5] = 0x81000100; init[6] = 0; init[7] = 0x01020101;
		init[8] = status;
		attach[0] = 0x98062caa; attach[1] = 0; attach[2] = 0; attach[3] = 0;
		attach[4] = JAMMA_JVS_PRESENT; attach[5] = 0x80000000; attach[6] = 0; attach[7] = 0x01020101;
		attach[8] = status;
		fw_host_queue_block(0x1000, p[0], init, JAMMA_REPORT_QUADS);
		for (i = 0; i < 8; i++)
			fw_host_queue_block(0x1000, p[0], attach, JAMMA_REPORT_QUADS);
		return 1;
	}

	if (serial_mode(io))
	{
		const u32 prev = m->latch;
		m->latch = p[4] & 0xff;
		/* The memory card slot's line, on its falling edge. */
		if ((prev & 0x05) == 0x05 && (m->latch & 0x05) == 0x04)
			m->memcard_slot ^= 1;
	}
	if (m->jamma_dest != NO_JAMMA_DEST)
	{
		u32 report[JAMMA_REPORT_QUADS];
		read_input(io);
		count_coins(io);
		status = jamma_status(io);
		report[0] = bswap32(m->coin[0]);
		report[1] = bswap32(m->coin[1]);
		report[2] = 0;
		report[3] = 0;
		report[4] = bswap32(source_bits(io)) | (io->desc.io_mode != P1IO_MODE_POPN ? JAMMA_JVS_PRESENT : 0);
		report[5] = 0x80000000;
		report[6] = 0;
		report[7] = 0x01020101;
		report[8] = status;
		if (fw_host_iop_write(m->jamma_dest, report, sizeof(report)))
			m->last_jamma = status;
	}
	return 1;
}

static int cmd_storage(p1io* io, u32 command_offset, const u32* p, u32 q)
{
	const int ata = command_offset == CMD_ATA;
	const int wide = ata && q >= 5;
	const u32 subop = p[0];
	const u32 sector = wide ? p[2] : p[1];
	const u32 count = wide ? p[3] : p[2];
	const u32 dest = wide ? p[4] : p[3];
	const u32 device = wide ? p[1] : 0;
	const u32 st = ata ? ST_ATA : ST_CF;
	storage* s = command_storage(io, command_offset);

	if (subop == 0)
		return read_sectors(io, s, sector, count, dest, st);
	if (subop == 2)
		return write_sectors(s, sector, count, dest, st);
	if (!ata && subop == 4)
	{
		queue_status(ST_CF, 0);
		return 1;
	}
	if (ata && subop == 7)
	{
		/* The game's file system query: formatted once it has made one. */
		if (device == 0x14)
			io->st.m.formatted = 1;
		queue_status(ST_ATA, bswap32(device == 0x0a && !io->st.m.formatted ? 0xffffffd1u : 0));
		return 1;
	}
	return 0;
}

static int p1io_write(fw_device* dev, u64 offset, const u32* p, u32 q)
{
	p1io* io = (p1io*)dev;
	const u32 c = (u32)(offset & 0xfff);

	if (c == CMD_CF || c == CMD_ATA)
		return q >= 4 && cmd_storage(io, c, p, q);
	if (!q)
		return 0;
	if (cmd_net(io, c, p, q))
		return 1;
	switch (c)
	{
		case CMD_ADPCM:  return cmd_adpcm(io, p, q);
		case CMD_UART:   return cmd_uart(io, p, q);
		case CMD_DALLAS: return cmd_dallas(io, p, q);
		case CMD_BBSRAM: return cmd_bbsram(io, p, q);
		case CMD_BOOTROM: return cmd_bootrom(io, p, q);
		case CMD_FSCI:   return cmd_fsci(io, p, q);
		case CMD_JAMMA_INIT:
		case CMD_JAMMA_OUTPUT:
			return q >= 8 && cmd_jamma(io, c, p);
		default:
			return 0;
	}
}

static int p1io_read(fw_device* dev, u64 offset, u32* value)
{
	p1io* io = (p1io*)dev;
	if ((offset & ~0x3ffULL) == FW_CROM_BASE && !(offset & 3))
	{
		const u32 i = (u32)(offset - FW_CROM_BASE) >> 2;
		if (io->crom_words)
		{
			if (i >= io->crom_words)
				return 0;
			*value = io->crom[i];
			return 1;
		}
		if (i >= sizeof(s_crom) / sizeof(s_crom[0]))
			return 0;
		*value = s_crom[i];
		return 1;
	}
	/* Captured: the board's answers before its first commands. */
	if (offset == BOOT_READY_OFFSET)
	{
		*value = 0x01000000;
		return 1;
	}
	if (offset == RUNTIME_READY_OFFSET)
	{
		*value = 0;
		return 1;
	}
	return 0;
}

/* ---- the device --------------------------------------------------- */

/* What a machine reset leaves: the memories stay, the rest starts over. */
static void p1io_reset(fw_device* dev)
{
	p1io* io = (p1io*)dev;
	p1io_meta* m = &io->st.m;
	m->npending = 0;
	fw_host_clear();
	m->uart_len = 0;
	memset(m->net_prop, 0, sizeof(m->net_prop));
	m->fsci_offset = 0;
	fsci_build(io);
	adpcm_stop(io);
	m->adpcm.vol_l = m->adpcm.vol_r = 0;
	m->bind_state = 0;
	memset(m->key_pressed, 0, sizeof(m->key_pressed));
	m->key_head = m->key_count = 0;
	m->jamma_dest = NO_JAMMA_DEST;
	m->last_jamma = JAMMA_NEUTRAL;
	m->last_bind = 0;
	m->latch = 0;
	m->memcard_slot = 1;
	m->coin[0] = m->coin[1] = 0;
	m->formatted = 0;
	m->next_ready_valid = 0;
	m->next_ready = 0;
}

static size_t p1io_state_size(fw_device* dev)
{
	(void)dev;
	return sizeof(p1io_state);
}

static void p1io_state_write(fw_device* dev, u8* block)
{
	p1io* io = (p1io*)dev;
	io->st.m.version = P1IO_STATE_VERSION;
	memcpy(block, &io->st, sizeof(io->st));
}

static int p1io_state_read(fw_device* dev, const u8* block)
{
	p1io* io = (p1io*)dev;
	p1io_meta m;
	const adpcm_state* a = &m.adpcm;
	u8* data = NULL;
	u32 size = 0;

	memcpy(&m, block + offsetof(p1io_state, m), sizeof(m));
	if (m.version != P1IO_STATE_VERSION || m.fsci_offset > FSCI_STREAM || m.uart_len > UART_RX_LIMIT
		|| m.npending > PENDING_MAX || m.key_head >= KEY_EVENTS || m.key_count > KEY_EVENTS
		|| m.memcard_slot > 1 || m.dallas_loaded[0] > 1 || m.dallas_loaded[1] > 1)
		return 0;
	if (a->step[0] < 0 || a->step[0] > 48 || a->step[1] < 0 || a->step[1] > 48
		|| a->loop_step[0] < 0 || a->loop_step[0] > 48 || a->loop_step[1] < 0 || a->loop_step[1] > 48)
		return 0;
	if (a->playing)
	{
		/* The sound is read again from the disk, and must be the one the
		 * state was playing. */
		u8 header[ADPCM_HEADER_SIZE];
		if (!a->loop_saved && a->position > a->loop_start && a->looping)
			return 0;
		if (!adpcm_read(io, a->sector, a->from_cf, header, &data, &size) || size != a->size
			|| a->position >= size || a->loop_start > a->loop_end || a->loop_end > size)
		{
			free(data);
			return 0;
		}
	}
	memcpy(&io->st, block, sizeof(io->st));
	free(io->adpcm_data);
	io->adpcm_data = data;
	schedule_pending(io);
	return 1;
}

static void set_storage_paths(p1io* io)
{
	io->hdd.path = io->desc.hdd_image;
	io->hdd.is_cf = 0;
	io->cf.path = io->desc.cf_image;
	io->cf.is_cf = 1;
}

p1io* p1io_create(const py1_desc* d, p1io_input_source input)
{
	p1io* io = (p1io*)calloc(1, sizeof(p1io));
	if (!io)
		return NULL;
	io->desc = *d;
	io->input = input;
	set_storage_paths(io);
	io->dev.reset = p1io_reset;
	io->dev.read_quadlet = p1io_read;
	io->dev.write = p1io_write;
	io->dev.service = p1io_service;
	io->dev.mix = p1io_mix;
	io->dev.state_size = p1io_state_size;
	io->dev.state_write = p1io_state_write;
	io->dev.state_read = p1io_state_read;

	load_bootrom(io);
	load_bbsram(io);
	load_dallas(io);
	load_config_rom(io);
	p1io_reset(&io->dev);
	s_active = io;
	return io;
}

void p1io_destroy(p1io* io)
{
	if (!io)
		return;
	save_bootrom(io);
	save_bbsram(io);
	save_dallas(io);
	storage_close(&io->hdd);
	storage_close(&io->cf);
	free(io->adpcm_data);
	if (s_active == io)
		s_active = NULL;
	free(io);
}

fw_device* p1io_device(p1io* io)
{
	return &io->dev;
}

int p1io_memcard_slot(void)
{
	if (!s_active || !serial_mode(s_active))
		return -1;
	return s_active->desc.io_mode == P1IO_MODE_DOGSTATIONDX ? (int)s_active->st.m.memcard_slot : 1;
}

/* ---- input descriptors -------------------------------------------- */

#define D(port, id, desc) { port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_##id, desc }
#define SYSTEM D(0, L3, "Test"), D(0, R3, "Service"), D(0, SELECT, "Coin 1"), D(1, SELECT, "Coin 2")
#define JVS(port) \
	D(port, START, "Start"), D(port, UP, "Up"), D(port, DOWN, "Down"), D(port, LEFT, "Left"), \
	D(port, RIGHT, "Right"), D(port, B, "Button 1"), D(port, A, "Button 2"), D(port, Y, "Button 3"), \
	D(port, X, "Button 4"), D(port, L, "Button 5"), D(port, R, "Button 6")
#define DDR(port) \
	D(port, START, "Start"), D(port, UP, "Foot Up"), D(port, DOWN, "Foot Down"), D(port, LEFT, "Foot Left"), \
	D(port, RIGHT, "Foot Right"), D(port, X, "Foot Up"), D(port, B, "Foot Down"), D(port, Y, "Foot Left"), \
	D(port, A, "Foot Right"), D(port, L, "Select Left"), D(port, R, "Select Right")

static const struct retro_input_descriptor s_desc_jvs[] = { SYSTEM, JVS(0), JVS(1), { 0, 0, 0, 0, NULL } };
/* Perfect Pool: the panel, and the trackball on the first pad's mouse
 * and left stick (the USB side). */
static const struct retro_input_descriptor s_desc_ppool[] = {
	SYSTEM, JVS(0), JVS(1),
	{ 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Trackball X" },
	{ 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Trackball Y" },
	{ 0, 0, 0, 0, NULL }
};
static const struct retro_input_descriptor s_desc_ddr[] = { SYSTEM, DDR(0), DDR(1), { 0, 0, 0, 0, NULL } };
static const struct retro_input_descriptor s_desc_popn[] = {
	SYSTEM, D(0, START, "Start"),
	D(0, X, "Button (P1 Up)"), D(0, A, "Button (P1 Down)"), D(0, R, "Button (P1 Left)"),
	D(0, B, "Button (P1 Right)"), D(0, L, "Button (P1 Button 1)"), D(0, Y, "Button (P2 Up)"),
	D(0, R2, "Button (P2 Down)"), D(0, UP, "Button (P2 Left)"), D(0, L2, "Button (P2 Right)"),
	{ 0, 0, 0, 0, NULL }
};

#undef D
#undef SYSTEM
#undef JVS
#undef DDR

const struct retro_input_descriptor* p1io_input_descriptors(int io_mode)
{
	switch (io_mode)
	{
		case P1IO_MODE_EXTIO: return s_desc_ddr;
		case P1IO_MODE_POPN:  return s_desc_popn;
		case P1IO_MODE_PPOOL: return s_desc_ppool;
		default:              return s_desc_jvs;
	}
}
