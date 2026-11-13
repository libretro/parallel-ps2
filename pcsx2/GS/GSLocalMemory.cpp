/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2021 PCSX2 Dev Team
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

#include <memalign.h>
#include <stdlib.h>

#include "GS.h"
#include "GSLocalMemory.h"
#include "GSExtra.h"

template <typename Fn>
static void foreachBlock(const GSOffset& off, GSLocalMemory* mem, const GSVector4i& r, u8* dst, int dstpitch, int bpp, Fn&& fn)
{
	GSOffset::BNHelper bn = off.bnMulti(r.left, r.top);
	int right = r.right >> off.blockShiftX();
	int bottom = r.bottom >> off.blockShiftY();

	int offset = dstpitch << off.blockShiftY();
	int xAdd = (1 << off.blockShiftX()) * (bpp / 8);

	for (; bn.blkY() < bottom; bn.nextBlockY(), dst += offset)
	{
		for (int x = 0; bn.blkX() < right; bn.nextBlockX(), x += xAdd)
		{
			const u8* src = mem->BlockPtr(bn.value());
			u8* read_dst = dst + x;
			fn(read_dst, src);
		}
	}
}

//

constexpr GSSwizzleInfo GSLocalMemory::swizzle32;
constexpr GSSwizzleInfo GSLocalMemory::swizzle32Z;
constexpr GSSwizzleInfo GSLocalMemory::swizzle16;
constexpr GSSwizzleInfo GSLocalMemory::swizzle16S;
constexpr GSSwizzleInfo GSLocalMemory::swizzle16Z;
constexpr GSSwizzleInfo GSLocalMemory::swizzle16SZ;
constexpr GSSwizzleInfo GSLocalMemory::swizzle8;
constexpr GSSwizzleInfo GSLocalMemory::swizzle4;

//

GSLocalMemory::psm_t GSLocalMemory::m_psm[64];
GSLocalMemory::readImage GSLocalMemory::m_readImageX;

//

GSLocalMemory::GSLocalMemory()
	: m_clut(this)
{
	m_vm8 = (u8*)GSAllocateWrappedMemory(m_vmsize, 4);
	memset(m_vm8, 0, m_vmsize);

	gs_flat_map_init(&m_po4map, sizeof(u64), sizeof(GSPixelOffset4*));
	gs_flat_map_init(&m_p2tmap, sizeof(u64), sizeof(GSPage2TileMap*));

	MULTI_ISA_SELECT(GSLocalMemoryPopulateFunctions)(*this);

	for (int i = 0; i < 64; i++)
	{
		psm_t& psm = m_psm[i];
		static_cast<gs_psm_attr&>(psm) = gs_psm[i];
		psm.rp = &GSLocalMemory::ReadPixel32;
		psm.rpa = &GSLocalMemory::ReadPixel32;
		psm.wp = &GSLocalMemory::WritePixel32;
		psm.wpa = &GSLocalMemory::WritePixel32;
		psm.rt = &GSLocalMemory::ReadTexel32;
		psm.rta = &GSLocalMemory::ReadTexel32;
		psm.wfa = &GSLocalMemory::WritePixel32;
	}

	m_psm[PSMCT24].rp = &GSLocalMemory::ReadPixel24;
	m_psm[PSMCT16].rp = &GSLocalMemory::ReadPixel16;
	m_psm[PSMCT16S].rp = &GSLocalMemory::ReadPixel16S;
	m_psm[PSMT8].rp = &GSLocalMemory::ReadPixel8;
	m_psm[PSMT4].rp = &GSLocalMemory::ReadPixel4;
	m_psm[PSMT8H].rp = &GSLocalMemory::ReadPixel8H;
	m_psm[PSMT4HL].rp = &GSLocalMemory::ReadPixel4HL;
	m_psm[PSMT4HH].rp = &GSLocalMemory::ReadPixel4HH;
	m_psm[PSMZ32].rp = &GSLocalMemory::ReadPixel32Z;
	m_psm[PSMZ24].rp = &GSLocalMemory::ReadPixel24Z;
	m_psm[PSMZ16].rp = &GSLocalMemory::ReadPixel16Z;
	m_psm[PSMZ16S].rp = &GSLocalMemory::ReadPixel16SZ;

	m_psm[PSMCT24].rpa = &GSLocalMemory::ReadPixel24;
	m_psm[PSMCT16].rpa = &GSLocalMemory::ReadPixel16;
	m_psm[PSMCT16S].rpa = &GSLocalMemory::ReadPixel16;
	m_psm[PSMT8].rpa = &GSLocalMemory::ReadPixel8;
	m_psm[PSMT4].rpa = &GSLocalMemory::ReadPixel4;
	m_psm[PSMT8H].rpa = &GSLocalMemory::ReadPixel8H;
	m_psm[PSMT4HL].rpa = &GSLocalMemory::ReadPixel4HL;
	m_psm[PSMT4HH].rpa = &GSLocalMemory::ReadPixel4HH;
	m_psm[PSMZ32].rpa = &GSLocalMemory::ReadPixel32;
	m_psm[PSMZ24].rpa = &GSLocalMemory::ReadPixel24;
	m_psm[PSMZ16].rpa = &GSLocalMemory::ReadPixel16;
	m_psm[PSMZ16S].rpa = &GSLocalMemory::ReadPixel16;

	m_psm[PSMCT32].wp = &GSLocalMemory::WritePixel32;
	m_psm[PSMCT24].wp = &GSLocalMemory::WritePixel24;
	m_psm[PSMCT16].wp = &GSLocalMemory::WritePixel16;
	m_psm[PSMCT16S].wp = &GSLocalMemory::WritePixel16S;
	m_psm[PSMT8].wp = &GSLocalMemory::WritePixel8;
	m_psm[PSMT4].wp = &GSLocalMemory::WritePixel4;
	m_psm[PSMT8H].wp = &GSLocalMemory::WritePixel8H;
	m_psm[PSMT4HL].wp = &GSLocalMemory::WritePixel4HL;
	m_psm[PSMT4HH].wp = &GSLocalMemory::WritePixel4HH;
	m_psm[PSMZ32].wp = &GSLocalMemory::WritePixel32Z;
	m_psm[PSMZ24].wp = &GSLocalMemory::WritePixel24Z;
	m_psm[PSMZ16].wp = &GSLocalMemory::WritePixel16Z;
	m_psm[PSMZ16S].wp = &GSLocalMemory::WritePixel16SZ;

	m_psm[PSMCT32].wpa = &GSLocalMemory::WritePixel32;
	m_psm[PSMCT24].wpa = &GSLocalMemory::WritePixel24;
	m_psm[PSMCT16].wpa = &GSLocalMemory::WritePixel16;
	m_psm[PSMCT16S].wpa = &GSLocalMemory::WritePixel16;
	m_psm[PSMT8].wpa = &GSLocalMemory::WritePixel8;
	m_psm[PSMT4].wpa = &GSLocalMemory::WritePixel4;
	m_psm[PSMT8H].wpa = &GSLocalMemory::WritePixel8H;
	m_psm[PSMT4HL].wpa = &GSLocalMemory::WritePixel4HL;
	m_psm[PSMT4HH].wpa = &GSLocalMemory::WritePixel4HH;
	m_psm[PSMZ32].wpa = &GSLocalMemory::WritePixel32;
	m_psm[PSMZ24].wpa = &GSLocalMemory::WritePixel24;
	m_psm[PSMZ16].wpa = &GSLocalMemory::WritePixel16;
	m_psm[PSMZ16S].wpa = &GSLocalMemory::WritePixel16;

	m_psm[PSMCT24].rt = &GSLocalMemory::ReadTexel24;
	m_psm[PSMCT16].rt = &GSLocalMemory::ReadTexel16;
	m_psm[PSMCT16S].rt = &GSLocalMemory::ReadTexel16S;
	m_psm[PSMT8].rt = &GSLocalMemory::ReadTexel8;
	m_psm[PSMT4].rt = &GSLocalMemory::ReadTexel4;
	m_psm[PSMT8H].rt = &GSLocalMemory::ReadTexel8H;
	m_psm[PSMT4HL].rt = &GSLocalMemory::ReadTexel4HL;
	m_psm[PSMT4HH].rt = &GSLocalMemory::ReadTexel4HH;
	m_psm[PSMZ32].rt = &GSLocalMemory::ReadTexel32Z;
	m_psm[PSMZ24].rt = &GSLocalMemory::ReadTexel24Z;
	m_psm[PSMZ16].rt = &GSLocalMemory::ReadTexel16Z;
	m_psm[PSMZ16S].rt = &GSLocalMemory::ReadTexel16SZ;

	m_psm[PSMCT24].rta = &GSLocalMemory::ReadTexel24;
	m_psm[PSMCT16].rta = &GSLocalMemory::ReadTexel16;
	m_psm[PSMCT16S].rta = &GSLocalMemory::ReadTexel16;
	m_psm[PSMT8].rta = &GSLocalMemory::ReadTexel8;
	m_psm[PSMT4].rta = &GSLocalMemory::ReadTexel4;
	m_psm[PSMT8H].rta = &GSLocalMemory::ReadTexel8H;
	m_psm[PSMT4HL].rta = &GSLocalMemory::ReadTexel4HL;
	m_psm[PSMT4HH].rta = &GSLocalMemory::ReadTexel4HH;
	m_psm[PSMZ24].rta = &GSLocalMemory::ReadTexel24;
	m_psm[PSMZ16].rta = &GSLocalMemory::ReadTexel16;
	m_psm[PSMZ16S].rta = &GSLocalMemory::ReadTexel16;

	m_psm[PSMCT24].wfa = &GSLocalMemory::WritePixel24;
	m_psm[PSMCT16].wfa = &GSLocalMemory::WriteFrame16;
	m_psm[PSMCT16S].wfa = &GSLocalMemory::WriteFrame16;
	m_psm[PSMZ24].wfa = &GSLocalMemory::WritePixel24;
	m_psm[PSMZ16].wfa = &GSLocalMemory::WriteFrame16;
	m_psm[PSMZ16S].wfa = &GSLocalMemory::WriteFrame16;
}

GSLocalMemory::~GSLocalMemory()
{
	if (m_vm8)
		GSFreeWrappedMemory(m_vm8, m_vmsize, 4);

	gs_local_memory_map_free(&m_po4map);
	gs_local_memory_map_free(&m_p2tmap);
}

u32 GSLocalMemory::IsPageAlignedMasked(u32 psm, const GSVector4i& rc)
{
	const psm_t& psm_s = m_psm[psm];
	const GSVector4i pgmsk = GSVector4i(psm_s.pgs).xyxy() - GSVector4i(1);
	return ((rc & pgmsk) == GSVector4i::zero()).mask();
}

bool GSLocalMemory::IsPageAligned(u32 psm, const GSVector4i& rc)
{
	const psm_t& psm_s = m_psm[psm];
	const GSVector4i pgmsk = GSVector4i(psm_s.pgs).xyxy() - GSVector4i(1);
	return (rc & pgmsk).eq(GSVector4i::zero());
}

u32 GSLocalMemory::GetStartBlockAddress(u32 bp, u32 bw, u32 psm, GSVector4i rect)
{
	return gs_psm_start_block(bp, bw, psm, rect.x, rect.y);
}

u32 GSLocalMemory::GetEndBlockAddress(u32 bp, u32 bw, u32 psm, GSVector4i rect)
{
	return gs_psm_end_block(bp, bw, psm, rect.z, rect.w);
}

u32 GSLocalMemory::GetUnwrappedEndBlockAddress(u32 bp, u32 bw, u32 psm, GSVector4i rect)
{
	return gs_psm_end_block_unwrapped(bp, bw, psm, rect.z, rect.w);
}

GSVector4i GSLocalMemory::GetRectForPageOffset(u32 base_bp, u32 offset_bp, u32 bw, u32 psm)
{
	const u32 page_offset = (offset_bp - base_bp) >> 5;
	const GSVector2i& pgs = m_psm[psm].pgs;
	const u32 valid_bw = pcsx2_max_i(1U, bw);
	const GSVector2i page_offset_xy = GSVector2i(page_offset % valid_bw, page_offset / pcsx2_max_i(1U, valid_bw));
	return GSVector4i(pgs * page_offset_xy).xyxy() + GSVector4i::loadh(pgs);
}

bool GSLocalMemory::HasOverlap(const u32 src_bp, const u32 src_bw, const u32 src_psm, const GSVector4i src_rect
							, const u32 dst_bp, const u32 dst_bw, const u32 dst_psm, const GSVector4i dst_rect)
{
	const int s[4] = {src_rect.x, src_rect.y, src_rect.z, src_rect.w};
	const int d[4] = {dst_rect.x, dst_rect.y, dst_rect.z, dst_rect.w};
	return gs_psm_has_overlap(src_bp, src_bw, src_psm, s, dst_bp, dst_bw, dst_psm, d) != 0;
}

///////////////////

void GSLocalMemory::ReadTexture(const GSOffset& off, const GSVector4i& r, u8* dst, int dstpitch, const GIFRegTEXA& TEXA)
{
	const psm_t& psm = m_psm[off.psm()];

	readTexel rt = psm.rt;
	readTexture rtx = psm.rtx;

	if (r.width() < psm.bs.x || r.height() < psm.bs.y || (r.left & (psm.bs.x - 1)) || (r.top & (psm.bs.y - 1)) || (r.right & (psm.bs.x - 1)) || (r.bottom & (psm.bs.y - 1)))
	{
		GIFRegTEX0 TEX0;

		TEX0.TBP0 = off.bp();
		TEX0.TBW = off.bw();
		TEX0.PSM = off.psm();

		GSVector4i cr = r.ralign<Align_Inside>(psm.bs);

		bool aligned = ((size_t)(dst + (cr.left - r.left) * sizeof(u32)) & 0xf) == 0;

		if (cr.rempty() || !aligned)
		{
			// TODO: expand r to block size, read into temp buffer
			for (int y = r.top; y < r.bottom; y++, dst += dstpitch)
			{
				for (int x = r.left, i = 0; x < r.right; x++, i++)
					((u32*)dst)[i] = (this->*rt)(x, y, TEX0, TEXA);
			}
		}
		else
		{
			u8* crdst = dst;

			for (int y = r.top; y < cr.top; y++, dst += dstpitch)
			{
				for (int x = r.left, i = 0; x < r.right; x++, i++)
					((u32*)dst)[i] = (this->*rt)(x, y, TEX0, TEXA);
			}

			for (int y = cr.top; y < cr.bottom; y++, dst += dstpitch)
			{
				for (int x = r.left, i = 0; x < cr.left; x++, i++)
				{
					((u32*)dst)[i] = (this->*rt)(x, y, TEX0, TEXA);
				}

				for (int x = cr.right, i = x - r.left; x < r.right; x++, i++)
				{
					((u32*)dst)[i] = (this->*rt)(x, y, TEX0, TEXA);
				}
			}

			for (int y = cr.bottom; y < r.bottom; y++, dst += dstpitch)
			{
				for (int x = r.left, i = 0; x < r.right; x++, i++)
				{
					((u32*)dst)[i] = (this->*rt)(x, y, TEX0, TEXA);
				}
			}

			if (!cr.rempty())
			{
				crdst += dstpitch * (cr.top - r.top);
				crdst += sizeof(u32) * (cr.left - r.left);
				rtx(*this, off, cr, crdst, dstpitch, TEXA);
			}
		}
	}
	else
		rtx(*this, off, r, dst, dstpitch, TEXA);
}
