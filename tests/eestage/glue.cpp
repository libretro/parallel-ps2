/* What the real Interpreter.c and R5900Opcode*.c need around them for
 * tests/eestage, and C entry points for main.c. The EE sees a flat 32 MB
 * of RAM at every segment; a load from EESTAGE_EXIT ends execution. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Common.h"
#include "R5900.h"
#include "Elfheader.h"
#include "ps2/BiosTools.h"
#include "vtlb.h"

#define EESTAGE_RAM  (32u * 1024u * 1024u)
#define EESTAGE_EXIT 0x01F00000u

alignas(16) cpuRegisters cpuRegs;
R5900cpu* Cpu = &intCpu;
retro_atomic_int_t eeEventTestIsActive;
bool g_SkipBiosHack;
extern "C" { bool g_GameStarted; bool g_GameLoading; s32 EEsCycle; u64 EEoCycle; }
u32 g_eeloadMain, g_eeloadExec;
u32 ElfEntry = 0xFFFFFFFF;
bool NoOSD;
extern "C" { bool AllowParams1; bool AllowParams2; u32 BiosChecksum; }
BiosDebugInformation CurrentBiosInformation;

static void eestage_log(enum retro_log_level level, const char* fmt, ...)
{
	va_list ap;
	(void)level;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}
extern "C" retro_log_printf_t log_cb = eestage_log;

static u8* s_ram;
static int s_hooks;
static int s_exited;

extern "C" unsigned char* eestage_ram(void) { return s_ram; }
extern "C" int eestage_hooks(void) { return s_hooks; }
extern "C" int eestage_started(void) { return g_GameStarted; }
extern "C" int eestage_exited(void) { return s_exited; }
extern "C" unsigned eestage_gpr(int n) { return cpuRegs.GPR.r[n].UL[0]; }

extern "C" int eestage_init(void)
{
	s_ram = (u8*)calloc(1, EESTAGE_RAM);
	return s_ram != NULL;
}

extern "C" void eestage_free(void) { free(s_ram); }

/* Runs from pc until the program loads from EESTAGE_EXIT. */
extern "C" void eestage_execute(unsigned pc, unsigned elf_entry)
{
	memset(&cpuRegs, 0, sizeof(cpuRegs));
	cpuRegs.pc     = pc;
	ElfEntry       = elf_entry;
	g_GameStarted  = false;
	g_GameLoading  = false;
	g_eeloadMain   = 0;
	g_eeloadExec   = 0;
	s_hooks        = 0;
	s_exited       = 0;
	intCpu.Reset();
	intCpu.Execute();
}

/* The BIOS's EELOAD main: the game is about to be loaded. */
void eeloadHook(void) { s_hooks++; g_GameLoading = true; }
void eeloadHook2(void) { }
void eeGameStarting(void) { g_GameStarted = true; }
void _cpuEventTest_Shared(void) { }

static u8* ram_at(u32 mem, u32 size)
{
	const u32 a = mem & (EESTAGE_RAM - 1);
	if (a + size > EESTAGE_RAM)
		abort();
	return s_ram + a;
}

mem8_t vtlb_memRead8(u32 mem) { return *ram_at(mem, 1); }
mem16_t vtlb_memRead16(u32 mem) { u16 v; memcpy(&v, ram_at(mem, 2), 2); return v; }
mem32_t vtlb_memRead32(u32 mem)
{
	u32 v;
	if ((mem & 0x1FFFFFFF) == EESTAGE_EXIT)
	{
		s_exited = 1;
		Cpu->ExitExecution();
		return 0;
	}
	memcpy(&v, ram_at(mem, 4), 4);
	return v;
}
mem64_t vtlb_memRead64(u32 mem) { u64 v; memcpy(&v, ram_at(mem, 8), 8); return v; }
RETURNS_R128 vtlb_memRead128(u32 mem) { r128 v; memcpy(&v, ram_at(mem, 16), 16); return v; }
void vtlb_memWrite8(u32 mem, mem8_t value) { *ram_at(mem, 1) = value; }
void vtlb_memWrite16(u32 mem, mem16_t value) { memcpy(ram_at(mem, 2), &value, 2); }
void vtlb_memWrite32(u32 mem, mem32_t value) { memcpy(ram_at(mem, 4), &value, 4); }
void vtlb_memWrite64(u32 mem, mem64_t value) { memcpy(ram_at(mem, 8), &value, 8); }
void TAKES_R128 vtlb_memWrite128(u32 mem, r128 value) { memcpy(ram_at(mem, 16), &value, 16); }
void* vtlb_GetPhyPtr(u32 paddr) { return ram_at(paddr, 1); }
