/* The C++ GSBlock the C replaced, through the same scenario: built
 * against that tree (see build.sh for how the hash was taken) */
#include "common/Pcsx2Defs.h"
#include "GS/GSRegs.h"
#include "GS/GSBlock.h"
#include "common.h"
using namespace CURRENT_ISA;

#define W32(i, al, m, d, s, p) GSBlock::WriteColumn32<i, al, m>(d, s, p)
#define W16(i, al, d, s, p) GSBlock::WriteColumn16<i, al>(d, s, p)
#define W8(i, al, d, s, p) GSBlock::WriteColumn8<i, al>(d, s, p)
#define W4(i, al, d, s, p) GSBlock::WriteColumn4<i, al>(d, s, p)
#define W32Y(y, al, m, d, s, p) GSBlock::WriteColumn32<al, m>(y, d, s, p)
#define W16Y(y, al, d, s, p) GSBlock::WriteColumn16<al>(y, d, s, p)
#define W8Y(y, al, d, s, p) GSBlock::WriteColumn8<al>(y, d, s, p)
#define W4Y(y, al, d, s, p) GSBlock::WriteColumn4<al>(y, d, s, p)
#define WB32(al, m, d, s, p) GSBlock::WriteBlock32<al, m>(d, s, p)
#define WB16(al, d, s, p) GSBlock::WriteBlock16<al>(d, s, p)
#define WB8(al, d, s, p) GSBlock::WriteBlock8<al>(d, s, p)
#define WB4(al, d, s, p) GSBlock::WriteBlock4<al>(d, s, p)
#define R32(i, s, d, p) GSBlock::ReadColumn32<i>(s, d, p)
#define R16(i, s, d, p) GSBlock::ReadColumn16<i>(s, d, p)
#define R8(i, s, d, p) GSBlock::ReadColumn8<i>(s, d, p)
#define R4(i, s, d, p) GSBlock::ReadColumn4<i>(s, d, p)
#define R32Y(y, s, d, p) GSBlock::ReadColumn32(y, s, d, p)
#define R16Y(y, s, d, p) GSBlock::ReadColumn16(y, s, d, p)
#define R8Y(y, s, d, p) GSBlock::ReadColumn8(y, s, d, p)
#define R4Y(y, s, d, p) GSBlock::ReadColumn4(y, s, d, p)
#define RB32(s, d, p) GSBlock::ReadBlock32(s, d, p)
#define RB16(s, d, p) GSBlock::ReadBlock16(s, d, p)
#define RB8(s, d, p) GSBlock::ReadBlock8(s, d, p)
#define RB4(s, d, p) GSBlock::ReadBlock4(s, d, p)
#define RB4P(s, d, p) GSBlock::ReadBlock4P(s, d, p)
#define RB8HP(s, d, p) GSBlock::ReadBlock8HP(s, d, p)
#define RB4HLP(s, d, p) GSBlock::ReadBlock4HLP(s, d, p)
#define RB4HHP(s, d, p) GSBlock::ReadBlock4HHP(s, d, p)
static GIFRegTEXA sc_texa(u32 t0, u32 t1)
{
	GIFRegTEXA t;
	t.U64 = 0;
	t.TA0 = t0;
	t.TA1 = t1;
	return t;
}
#define EB24(aem, s, d, p, t0) GSBlock::ExpandBlock24<aem>((const u32*)(s), d, p, sc_texa(t0, 0))
#define EB16(aem, s, d, p, t0, t1) GSBlock::ExpandBlock16<aem>((const u16*)(s), d, p, sc_texa(t0, t1))
#define EB8_32(s, d, p, pal) GSBlock::ExpandBlock8_32(s, d, p, pal)
#define EB8_16(s, d, p, pal) GSBlock::ExpandBlock8_16(s, d, p, pal)
#define EB4_32(s, d, p, pal) GSBlock::ExpandBlock4_32(s, d, p, (const u64*)(pal))
#define EB4_16(s, d, p, pal) GSBlock::ExpandBlock4_16(s, d, p, (const u64*)(pal))
#define EB8H_32(s, d, p, pal) GSBlock::ExpandBlock8H_32((u32*)(s), d, p, pal)
#define EB8H_16(s, d, p, pal) GSBlock::ExpandBlock8H_16((u32*)(s), d, p, pal)
#define EB4HL_32(s, d, p, pal) GSBlock::ExpandBlock4HL_32((u32*)(s), d, p, pal)
#define EB4HL_16(s, d, p, pal) GSBlock::ExpandBlock4HL_16((u32*)(s), d, p, pal)
#define EB4HH_32(s, d, p, pal) GSBlock::ExpandBlock4HH_32((u32*)(s), d, p, pal)
#define EB4HH_16(s, d, p, pal) GSBlock::ExpandBlock4HH_16((u32*)(s), d, p, pal)
#define UW24(s, p, d) GSBlock::UnpackAndWriteBlock24(s, p, d)
#define UW8H(s, p, d) GSBlock::UnpackAndWriteBlock8H(s, p, d)
#define UW4HL(s, p, d) GSBlock::UnpackAndWriteBlock4HL(s, p, d)
#define UW4HH(s, p, d) GSBlock::UnpackAndWriteBlock4HH(s, p, d)
#define RE24(aem, s, d, p, t0) GSBlock::ReadAndExpandBlock24<aem>(s, d, p, sc_texa(t0, 0))
#define RE16(aem, s, d, p, t0, t1) GSBlock::ReadAndExpandBlock16<aem>(s, d, p, sc_texa(t0, t1))
#define RE8_32(s, d, p, pal) GSBlock::ReadAndExpandBlock8_32(s, d, p, pal)
#define RE8_32HSW(s, d, p, pal) GSBlock::ReadAndExpandBlock8_32HSW(s, d, p, pal)
#define RE4_32(s, d, p, pal) GSBlock::ReadAndExpandBlock4_32(s, d, p, pal)
#define RE8H_32(s, d, p, pal) GSBlock::ReadAndExpandBlock8H_32(s, d, p, pal)
#define RE8H_32HSW(s, d, p, pal) GSBlock::ReadAndExpandBlock8H_32HSW(s, d, p, pal)
#define RE4HL_32(s, d, p, pal) GSBlock::ReadAndExpandBlock4HL_32(s, d, p, pal)
#define RE4HH_32(s, d, p, pal) GSBlock::ReadAndExpandBlock4HH_32(s, d, p, pal)

#include "scenario.h"

int main()
{
	sc_init();
	sc_writes();
	sc_reads();
	sc_expands();
	sc_palettes();
	sc_unpacks();
	printf("%016llx\n", sc_hash);
	return 0;
}
