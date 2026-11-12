/* pcsx2/DEV9/DEV9.c, the real unit built as C89: the EEPROM a game reads
 * the console's MAC address from. An eeprom.dat in the working directory
 * is mapped over the built-in one when it holds all 64 bytes; anything
 * else -- no file, an empty or short one, a FIFO -- leaves the built-in
 * EEPROM in place. A short file used to be mapped anyway and faulted on
 * the first read past its end, a FIFO's failed map was taken as the
 * EEPROM, and every boot leaked the file and its mapping. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <libretro.h>
#ifdef _WIN32
#include <direct.h>
#define chdir _chdir
#define mkdir_dir(p) _mkdir(p)
#else
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#define mkdir_dir(p) mkdir(p, 0755)
#endif

#include "DEV9/DEV9.h"
#include "Config.h"
#include "DEV9/ATA/ATA.h"

/* What DEV9.c, flash.c and smap.c reach outside themselves. */
Pcsx2Config EmuConfig;
struct EmuFoldersData EmuFolders;
char BiosPath[16];
static void RETRO_CALLCONV quiet(enum retro_log_level level, const char* fmt, ...) { (void)level; (void)fmt; }
retro_log_printf_t log_cb = quiet;
void dev9Irq(int cycles) { (void)cycles; }

static ata_state_t* the_ata;
ata_state_t* ata_new(void) { the_ata = (ata_state_t*)calloc(1, 64); return the_ata; }
void ata_free(ata_state_t* ata) { free(ata); }
int ata_open(ata_state_t* ata, const char* hddPath, uint64_t size_sectors) { (void)ata; (void)hddPath; (void)size_sectors; return -1; }
void ata_close(ata_state_t* ata) { (void)ata; }
bool ata_load_hdd_id(ata_state_t* ata, const char* path) { (void)ata; (void)path; return false; }
void ata_hard_reset(ata_state_t* ata) { (void)ata; }
uint16_t ata_read16(ata_state_t* ata, uint32_t addr) { (void)ata; (void)addr; return 0; }
void ata_write16(ata_state_t* ata, uint32_t addr, uint16_t value) { (void)ata; (void)addr; (void)value; }
void ata_async(ata_state_t* ata, uint32_t cycles) { (void)ata; (void)cycles; }
void ata_read_dma8_mem(ata_state_t* ata, uint8_t* pMem, int size) { (void)ata; (void)pMem; (void)size; }
void ata_write_dma8_mem(ata_state_t* ata, uint8_t* pMem, int size) { (void)ata; (void)pMem; (void)size; }

void tx_put(const void* head, int head_len, const void* tail, int tail_len) { (void)head; (void)head_len; (void)tail; (void)tail_len; }
void ad_reset(void) { }
bool net_rx_deliver(void) { return false; }
void InitNet(void) { }
void ReconfigureLiveNet(const struct Pcsx2Config* old_config) { (void)old_config; }
void TermNet(void) { }

static int failures;
static void check(int ok, const char* what)
{
	printf("  %s: %s\n", ok ? "ok" : "FAIL", what);
	if (!ok)
		failures++;
}

/* The built-in EEPROM's first word, as a game reads it. */
#define BUILTIN_WORD0 0x6D76

/* A fresh directory to hold this case's eeprom.dat, made current. */
static void enter_case(const char* name)
{
	mkdir_dir(name);
	if (chdir(name) != 0)
	{
		printf("  FAIL: cannot enter %s\n", name);
		exit(1);
	}
}

static void leave_case(void)
{
	if (chdir("..") != 0)
		exit(1);
}

static void write_file(const char* path, const unsigned char* bytes, size_t size)
{
	FILE* f = fopen(path, "wb");
	if (!f)
	{
		printf("  FAIL: cannot write %s\n", path);
		exit(1);
	}
	if (size)
		fwrite(bytes, 1, size, f);
	fclose(f);
}

/* Every word of the EEPROM, read the way the bit-banged reader does:
 * one u16 at a time across all 32. A map that is not the EEPROM's
 * faults here. */
static unsigned sum_eeprom(void)
{
	unsigned sum = 0;
	int i;
	for (i = 0; i < 32; i++)
		sum += dev9.eeprom[i];
	return sum;
}

/* One boot and shutdown: what the EEPROM's first word is, after reading
 * all of it. */
static unsigned boot_word0(void)
{
	unsigned word0;
	if (DEV9init() != 0)
	{
		printf("  FAIL: DEV9init\n");
		exit(1);
	}
	sum_eeprom();
	word0 = dev9.eeprom[0];
	DEV9shutdown();
	return word0;
}

#ifdef __linux__
static int open_fds(void)
{
	int n = 0;
	DIR* d = opendir("/proc/self/fd");
	struct dirent* e;
	if (!d)
		return -1;
	while ((e = readdir(d)) != NULL)
		n++;
	closedir(d);
	return n;
}
#endif

int main(void)
{
	unsigned char full[64];
	int i;

	mkdir_dir("dev9eeprom_work");
	if (chdir("dev9eeprom_work") != 0)
		return 1;

	printf("no eeprom.dat\n");
	enter_case("none");
	remove("eeprom.dat");
	check(boot_word0() == BUILTIN_WORD0, "the built-in EEPROM");
	leave_case();

	printf("an empty eeprom.dat\n");
	enter_case("empty");
	write_file("eeprom.dat", full, 0);
	check(boot_word0() == BUILTIN_WORD0, "the built-in EEPROM, read whole without a fault");
	leave_case();

	printf("a short eeprom.dat\n");
	enter_case("short");
	memset(full, 0x11, sizeof(full));
	write_file("eeprom.dat", full, 10);
	check(boot_word0() == BUILTIN_WORD0, "the built-in EEPROM, read whole without a fault");
	leave_case();

#if !defined(_WIN32)
	printf("a FIFO named eeprom.dat\n");
	enter_case("fifo");
	remove("eeprom.dat");
	if (mkfifo("eeprom.dat", 0644) == 0)
		check(boot_word0() == BUILTIN_WORD0, "the built-in EEPROM, read whole without a fault");
	else
		printf("  skipped: no FIFOs here\n");
	remove("eeprom.dat");
	leave_case();
#endif

	printf("a whole eeprom.dat\n");
	enter_case("whole");
	for (i = 0; i < 64; i++)
		full[i] = (unsigned char)(0xA0 + i);
	write_file("eeprom.dat", full, sizeof(full));
	check(boot_word0() == (unsigned)(full[0] | (full[1] << 8)), "the file's EEPROM");
#ifdef __linux__
	{
		int before = open_fds();
		for (i = 0; i < 16; i++)
			boot_word0();
		check(before >= 0 && open_fds() == before, "sixteen boots leave no file or mapping open");
	}
#endif
	leave_case();

	if (failures)
	{
		printf("%d FAILED\n", failures);
		return 1;
	}
	printf("all passed\n");
	return 0;
}
