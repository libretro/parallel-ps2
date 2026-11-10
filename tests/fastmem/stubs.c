/* What vtlb.c links against that the table never reaches. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Common.h"
#include "vtlb.h"
#include "Cache.h"
#include "COP0.h"
#include "HotConfig.h"
#include "VU.h"

static void unreached(const char* who)
{
	fprintf(stderr, "fastmemtable: stub %s reached\n", who);
	abort();
}

u32 emu_hot;
void emu_config_disable_fastmem(void) { unreached("emu_config_disable_fastmem"); }
R5900cpu intCpu;
R5900cpu* Cpu = &intCpu;
cpuRegisters cpuRegs;
tlbs tlb[48];
EEVM_MemoryAllocMess* eeMem;
IopVM_MemoryAllocMess* iopMem;
VURegs vuRegs[2];
static void log_stub(enum retro_log_level level, const char* fmt, ...) { (void)level; (void)fmt; }
retro_log_printf_t log_cb = log_stub;

void UnmapTLB(const tlbs* t, int i) { (void)t; (void)i; unreached("UnmapTLB"); }
void cpuTlbMiss(u32 addr, u32 bd, u32 excode) { (void)addr; (void)bd; (void)excode; unreached("cpuTlbMiss"); }
void vtlb_DynBackpatchLoadStore(uptr code_address, u32 code_size, u32 guest_pc, u32 guest_addr,
	u32 gpr_bitmask, u32 fpr_bitmask, u8 address_register, u8 data_register,
	u8 size_in_bits, int is_signed, int is_load, int is_fpr) { unreached("vtlb_DynBackpatchLoadStore"); }

u8 readCache8(u32 mem) { (void)mem; unreached("readCache8"); return 0; }
u16 readCache16(u32 mem) { (void)mem; unreached("readCache16"); return 0; }
u32 readCache32(u32 mem) { (void)mem; unreached("readCache32"); return 0; }
u64 readCache64(u32 mem) { (void)mem; unreached("readCache64"); return 0; }
RETURNS_R128 readCache128(u32 mem) { r128 v; (void)mem; unreached("readCache128"); memset(&v, 0, sizeof(v)); return v; }
void writeCache8(u32 mem, u8 value) { (void)mem; (void)value; unreached("writeCache8"); }
void writeCache16(u32 mem, u16 value) { (void)mem; (void)value; unreached("writeCache16"); }
void writeCache32(u32 mem, u32 value) { (void)mem; (void)value; unreached("writeCache32"); }
void writeCache64(u32 mem, const u64 value) { (void)mem; (void)value; unreached("writeCache64"); }
void writeCache128(u32 mem, const mem128_t* value) { (void)mem; (void)value; unreached("writeCache128"); }

#include <faulthandler.h>
#include "VirtualMemory.h"
#include "Memory.h"
#include <memmap.h>

/* vtlb_Core_Alloc's side of the VM: the bump allocator hands out zeroed
 * memory, and the fastmem area is a window at a fixed address nothing
 * dereferences -- its mappings and protections are recorded, not made. */
static SysMainMemory s_vm;
SysMainMemory* GetVmMemory(void) { return &s_vm; }
u8* vmm_bump_alloc(VirtualMemoryBumpAllocator* b, size_t size) { (void)b; return (u8*)calloc(1, size); }
bool retro_faulthandler_install(retro_fault_handler_t handler) { (void)handler; return true; }
void retro_faulthandler_remove(retro_fault_handler_t handler) { (void)handler; }

#define FAKE_FASTMEM_BASE ((unsigned char*)(uintptr_t)0x700000000000ull)
memshm_area_t* memshm_area_create(size_t len) { (void)len; return (memshm_area_t*)(uintptr_t)1; }
void memshm_area_free(memshm_area_t* area) { (void)area; }
unsigned char* memshm_area_base(const memshm_area_t* area) { (void)area; return FAKE_FASTMEM_BASE; }
unsigned char* memshm_area_map(memshm_area_t* area, void* handle, size_t offset, void* addr, size_t size, int prot)
{ (void)area; (void)handle; (void)offset; (void)size; (void)prot; return (unsigned char*)addr; }
bool memshm_area_unmap(memshm_area_t* area, void* addr, size_t size) { (void)area; (void)addr; (void)size; return true; }

#ifdef _WIN32
/* host_mmap's Windows path (HostMem.h); memmap.c is not linked, as its
 * shared-memory area is stubbed above. */
#include <windows.h>
void* memreserve_at(void* hint, size_t len) { return VirtualAlloc(hint, len, MEM_RESERVE, PAGE_NOACCESS); }
bool memcommit(void* addr, size_t len) { return VirtualAlloc(addr, len, MEM_COMMIT, PAGE_READWRITE) != NULL; }
void memrelease(void* addr, size_t len) { (void)len; VirtualFree(addr, 0, MEM_RELEASE); }
#endif
