/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2010  PCSX2 Dev Team
 *
 *  PCSX2 is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU Lesser General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  PCSX2 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with PCSX2.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include <string.h>

#include "Common.h"
#include "Cache.h"
#include "vtlb.h"

/* The lower parts of a cache tag:
 * 31 - 12: the physical address cache tag.
 * 11 - 7: unused.
 * 6: dirty flag.
 * 5: valid flag.
 * 4: LRF flag - least recently filled flag.
 * 3: lock flag.
 * 2 - 0: unused. */

#define DIRTY_FLAG 0x40
#define VALID_FLAG 0x20
#define LRF_FLAG 0x10
#define LOCK_FLAG 0x8
#define ALL_FLAGS 0xFFF

typedef struct PCSX2_ALIGN(64) EECacheData
{
	u8 bytes[64];
} EECacheData;

typedef struct EECacheTag
{
	uptr rawValue;
} EECacheTag;

typedef struct EECacheSet
{
	EECacheTag tags[2];
	EECacheData data[2];
} EECacheSet;

/* One way of one set. */
typedef struct EECacheLine
{
	EECacheTag* tag;
	EECacheData* data;
	int set;
} EECacheLine;

static struct
{
	EECacheSet sets[64];
} cache;

static __fi EECacheLine cache_line(int set, int way)
{
	EECacheLine line;
	line.tag  = &cache.sets[set].tags[way];
	line.data = &cache.sets[set].data[way];
	line.set  = set;
	return line;
}

static void cache_line_writeback(EECacheLine line)
{
	uptr target;
	if (!((line.tag->rawValue & (DIRTY_FLAG | VALID_FLAG)) == (DIRTY_FLAG | VALID_FLAG)))
		return;

	target = (line.tag->rawValue & ~ALL_FLAGS) | (line.set << 6);

	*(EECacheData*)target = *line.data;
	line.tag->rawValue &= ~DIRTY_FLAG;
}

static void cache_line_load(EECacheLine line, uptr ppf)
{
	line.tag->rawValue &= ALL_FLAGS;
	line.tag->rawValue |= (ppf & ~ALL_FLAGS);
	memcpy(line.data, (void*)(ppf & ~(uptr)0x3F), sizeof(*line.data));
	line.tag->rawValue |=  VALID_FLAG;
	line.tag->rawValue &= ~DIRTY_FLAG;
}

/* Note the test: a way counts as a hit unless it is invalid AND holds
 * this tag, so way 0 nearly always answers. That is how this code has
 * always behaved; EnableEECache is never switched on in this core. */
static __fi bool cache_way_hit(const EECacheSet* set, uptr ppf, int checkWay, int* way)
{
	if (!(set->tags[checkWay].rawValue & VALID_FLAG) && (set->tags[checkWay].rawValue & ~ALL_FLAGS) == (ppf & ~ALL_FLAGS))
		return false;

	*way = checkWay;
	return true;
}

static bool findInCache(const EECacheSet* set, uptr ppf, int* way)
{
	return cache_way_hit(set, ppf, 0, way) || cache_way_hit(set, ppf, 1, way);
}

static int getFreeCache(u32 mem, int* way)
{
	const int setIdx = (mem >> 6) & 0x3F;
	EECacheSet* set    = &cache.sets[setIdx];
	vtlb_virt_t vmv  = vtlbdata.vmap[mem >> VTLB_PAGE_BITS];
	uptr ppf         = vtlb_virt_ptr(vmv, mem);

	if (!findInCache(set, ppf, way))
	{
		int newWay     = (set->tags[0].rawValue & LRF_FLAG) ^ (set->tags[1].rawValue & LRF_FLAG);
		EECacheLine line = cache_line(setIdx, newWay);
		*way           = newWay;

		cache_line_writeback(line);
		cache_line_load(line, ppf);
		line.tag->rawValue ^= LRF_FLAG;
	}

	return setIdx;
}

/* Cache line access. prepareCacheAccess was template<bool Write, int Bytes>
 * and the read/write bodies were template<typename Int>; every parameter is
 * a constant at each of the ten entry points below, so the Write flag and
 * the alignment mask folded away per instantiation. Written out, the two
 * variants differ by one OR of DIRTY_FLAG. */
static void* prepareCacheAccessRead(u32 mem, int bytes, int* way, int* idx)
{
	u32 aligned;

	*way = 0;
	*idx = getFreeCache(mem, way);
	aligned = mem & ~(bytes - 1);
	return &cache.sets[*idx].data[*way].bytes[aligned & 0x3f];
}

static void* prepareCacheAccessWrite(u32 mem, int bytes, int* way, int* idx)
{
	u32 aligned;

	*way = 0;
	*idx = getFreeCache(mem, way);
	cache.sets[*idx].tags[*way].rawValue |= DIRTY_FLAG;
	aligned = mem & ~(bytes - 1);
	return &cache.sets[*idx].data[*way].bytes[aligned & 0x3f];
}

#define CACHE_DEFINE_ACCESS(bits, type)                                        \
void writeCache##bits(u32 mem, type value)                                     \
{                                                                              \
	int way, idx;                                                              \
	void* addr = prepareCacheAccessWrite(mem, sizeof(type), &way, &idx);       \
	*(type*)addr = value;                                                      \
}                                                                              \
type readCache##bits(u32 mem)                                                  \
{                                                                              \
	int way, idx;                                                              \
	void* addr = prepareCacheAccessRead(mem, sizeof(type), &way, &idx);        \
	return *(type*)addr;                                                       \
}

CACHE_DEFINE_ACCESS(8,  u8)
CACHE_DEFINE_ACCESS(16, u16)
CACHE_DEFINE_ACCESS(32, u32)

/* 64-bit write takes its value by const value in the header. */
void writeCache64(u32 mem, const u64 value)
{
	int way, idx;
	void* addr = prepareCacheAccessWrite(mem, sizeof(u64), &way, &idx);
	*(u64*)addr = value;
}

u64 readCache64(u32 mem)
{
	int way, idx;
	void* addr = prepareCacheAccessRead(mem, sizeof(u64), &way, &idx);
	return *(u64*)addr;
}

void writeCache128(u32 mem, const mem128_t* value)
{
	int way, idx;
	void* addr = prepareCacheAccessWrite(mem, sizeof(mem128_t), &way, &idx);
	*(mem128_t*)addr = *value;
}

RETURNS_R128 readCache128(u32 mem)
{
	int way, idx;
	void* addr = prepareCacheAccessRead(mem, sizeof(mem128_t), &way, &idx);
	return r128_load(addr);
}

/* The line addr hits, if any; the set's way 1 only when way 0 misses. */
static bool cache_hit_line(u32 addr, EECacheLine* line)
{
	int way;
	const int index = (addr >> 6) & 0x3F;
	vtlb_virt_t vmv = vtlbdata.vmap[addr >> VTLB_PAGE_BITS];
	uptr ppf = vtlb_virt_ptr(vmv, addr);
	if (!findInCache(&cache.sets[index], ppf, &way))
		return false;
	*line = cache_line(index, way);
	return true;
}

static void cache_line_invalidate(EECacheLine line)
{
	line.tag->rawValue &= LRF_FLAG;
	memset(line.data, 0, sizeof(*line.data));
}

void intCACHE(void)
{
	const u32 addr  = cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_;
	const int index = (addr >> 6) & 0x3F;
	EECacheLine line;

	switch (_Rt_)
	{
		case 0x1a: /* DHIN (Data Cache Hit Invalidate) */
			if (cache_hit_line(addr, &line))
				cache_line_invalidate(line);
			break;

		case 0x18: /* DHWBIN (Data Cache Hit WriteBack with Invalidate) */
			if (cache_hit_line(addr, &line))
			{
				cache_line_writeback(line);
				cache_line_invalidate(line);
			}
			break;

		case 0x1c: /* DHWOIN (Data Cache Hit WriteBack Without Invalidate) */
			if (cache_hit_line(addr, &line))
				cache_line_writeback(line);
			break;

		case 0x16: /* DXIN (Data Cache Index Invalidate) */
			cache_line_invalidate(cache_line(index, addr & 0x1));
			break;

		case 0x11: /* DXLDT (Data Cache Load Data into TagLo) */
			memcpy(&cpuRegs.CP0.n.TagLo, &cache.sets[index].data[addr & 0x1].bytes[addr & 0x3C], 4);
			break;

		case 0x10: /* DXLTG (Data Cache Load Tag into TagLo) */
			line = cache_line(index, addr & 0x1);
			/* DXLTG demands that SYNC.L is called before this command, which
			 * forces the cache to write back, so presumably games are checking
			 * the cache has updated the memory. For speed, do it here. */
			cache_line_writeback(line);
			/* Our tags hold host addresses, not PS2 ones. */
			cpuRegs.CP0.n.TagLo = line.tag->rawValue & ALL_FLAGS;
			break;

		case 0x13: /* DXSDT (Data Cache Store 32bits from TagLo) */
			memcpy(&cache.sets[index].data[addr & 0x1].bytes[addr & 0x3C], &cpuRegs.CP0.n.TagLo, 4);
			break;

		case 0x12: /* DXSTG (Data Cache Store Tag from TagLo) */
			line = cache_line(index, addr & 0x1);
			line.tag->rawValue &= ~ALL_FLAGS;
			line.tag->rawValue |= (cpuRegs.CP0.n.TagLo & ALL_FLAGS);
			break;

		case 0x14: /* DXWBIN (Data Cache Index WriteBack Invalidate) */
			line = cache_line(index, addr & 0x1);
			cache_line_writeback(line);
			cache_line_invalidate(line);
			break;

		case 0x7: /* IXIN (Instruction Cache Index Invalidate): no instruction cache here */
		case 0xC: /* BFH (BTAC Flush): branch targets are not cached */
		default:
			break;
	}
}
