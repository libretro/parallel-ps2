/* The selector where GSSwJit.h reads it (GS_SEL_*): each field of the C++
 * GSScanlineSelector, set alone, read back through its macro. The
 * generators' own bytes are the other drivers'. */
#include <stdio.h>

#include <libretro.h>

extern "C" { retro_log_printf_t log_cb; }

#include "GS/Renderers/SW/GSScanlineEnvironment.h"
#include "GS/Renderers/SW/GSSwJit.h"

int main()
{
	GSScanlineSelector s;
	int fails = 0;
#define SEL(field, macro, v) \
	s.key = 0; \
	s.field = v; \
	if (macro(s.key) != v || s.key != ((u64)v << __builtin_ctzll(s.key))) \
	{ \
		printf("  FAIL: selector." #field " is not where " #macro " reads it\n"); \
		fails++; \
	}
	SEL(fpsm, GS_SEL_FPSM, 3) SEL(zpsm, GS_SEL_ZPSM, 3) SEL(ztst, GS_SEL_ZTST, 3) SEL(atst, GS_SEL_ATST, 7)
	SEL(afail, GS_SEL_AFAIL, 3) SEL(iip, GS_SEL_IIP, 1) SEL(tfx, GS_SEL_TFX, 7) SEL(tcc, GS_SEL_TCC, 1)
	SEL(fst, GS_SEL_FST, 1) SEL(ltf, GS_SEL_LTF, 1) SEL(tlu, GS_SEL_TLU, 1) SEL(fge, GS_SEL_FGE, 1)
	SEL(date, GS_SEL_DATE, 1) SEL(abe, GS_SEL_ABE, 1) SEL(aba, GS_SEL_ABA, 3) SEL(abb, GS_SEL_ABB, 3)
	SEL(abc, GS_SEL_ABC, 3) SEL(abd, GS_SEL_ABD, 3) SEL(pabe, GS_SEL_PABE, 1) SEL(aa1, GS_SEL_AA1, 1)
	SEL(fwrite, GS_SEL_FWRITE, 1) SEL(fb, GS_SEL_FB, 3) SEL(rfb, GS_SEL_RFB, 1) SEL(zwrite, GS_SEL_ZWRITE, 1)
	SEL(zb, GS_SEL_ZB, 3) SEL(ztest, GS_SEL_ZTEST, 1) SEL(zoverflow, GS_SEL_ZOVERFLOW, 1)
	SEL(zclamp, GS_SEL_ZCLAMP, 1) SEL(wms, GS_SEL_WMS, 3) SEL(wmt, GS_SEL_WMT, 3) SEL(datm, GS_SEL_DATM, 1)
	SEL(colclamp, GS_SEL_COLCLAMP, 1) SEL(fba, GS_SEL_FBA, 1) SEL(dthe, GS_SEL_DTHE, 1)
	SEL(prim, GS_SEL_PRIM, 3) SEL(edge, GS_SEL_EDGE, 1) SEL(tw, GS_SEL_TW, 7) SEL(lcm, GS_SEL_LCM, 1)
	SEL(mmin, GS_SEL_MMIN, 3) SEL(notest, GS_SEL_NOTEST, 1) SEL(zequal, GS_SEL_ZEQUAL, 1)
	SEL(hires, GS_SEL_HIRES, 1) SEL(tw_hi, GS_SEL_TW_HI, 1)
	printf("swjit selector: %d fields misplaced\n", fails);
	return fails != 0;
}
