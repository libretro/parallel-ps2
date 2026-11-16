/* The GS block swizzle in C (GS/gs_block.h) against the hash the C++
 * GSBlock gave (the command line), in whichever instruction set this was
 * built for: scenario.h. */
#include "common/Pcsx2Types.h"
#include "GS/gs_block.h"
#include "common.h"

#define W32(i, al, m, d, s, p) gs_block_write_column32(i, al, m, d, s, p)
#define W16(i, al, d, s, p) gs_block_write_column16(i, al, d, s, p)
#define W8(i, al, d, s, p) gs_block_write_column8(i, al, d, s, p)
#define W4(i, al, d, s, p) gs_block_write_column4(i, al, d, s, p)
#define W32Y(y, al, m, d, s, p) gs_block_write_column32_y(y, al, m, d, s, p)
#define W16Y(y, al, d, s, p) gs_block_write_column16_y(y, al, d, s, p)
#define W8Y(y, al, d, s, p) gs_block_write_column8_y(y, al, d, s, p)
#define W4Y(y, al, d, s, p) gs_block_write_column4_y(y, al, d, s, p)
#define WB32(al, m, d, s, p) gs_block_write_block32(al, m, d, s, p)
#define WB16(al, d, s, p) gs_block_write_block16(al, d, s, p)
#define WB8(al, d, s, p) gs_block_write_block8(al, d, s, p)
#define WB4(al, d, s, p) gs_block_write_block4(al, d, s, p)
#define R32(i, s, d, p) gs_block_read_column32(i, s, d, p)
#define R16(i, s, d, p) gs_block_read_column16(i, s, d, p)
#define R8(i, s, d, p) gs_block_read_column8(i, s, d, p)
#define R4(i, s, d, p) gs_block_read_column4(i, s, d, p)
#define R32Y(y, s, d, p) gs_block_read_column32_y(y, s, d, p)
#define R16Y(y, s, d, p) gs_block_read_column16_y(y, s, d, p)
#define R8Y(y, s, d, p) gs_block_read_column8_y(y, s, d, p)
#define R4Y(y, s, d, p) gs_block_read_column4_y(y, s, d, p)
#define RB32(s, d, p) gs_block_read_block32(s, d, p)
#define RB16(s, d, p) gs_block_read_block16(s, d, p)
#define RB8(s, d, p) gs_block_read_block8(s, d, p)
#define RB4(s, d, p) gs_block_read_block4(s, d, p)
#define RB4P(s, d, p) gs_block_read_block4p(s, d, p)
#define RB8HP(s, d, p) gs_block_read_block8hp(s, d, p)
#define RB4HLP(s, d, p) gs_block_read_block4hlp(s, d, p)
#define RB4HHP(s, d, p) gs_block_read_block4hhp(s, d, p)
#define EB24(aem, s, d, p, t0) gs_block_expand_block24(aem, (const u32*)(s), d, p, t0)
#define EB16(aem, s, d, p, t0, t1) gs_block_expand_block16(aem, (const u16*)(s), d, p, t0, t1)
#define EB8_32(s, d, p, pal) gs_block_expand_block8_32(s, d, p, pal)
#define EB8_16(s, d, p, pal) gs_block_expand_block8_16(s, d, p, pal)
#define EB4_32(s, d, p, pal) gs_block_expand_block4_32(s, d, p, (const u64*)(pal))
#define EB4_16(s, d, p, pal) gs_block_expand_block4_16(s, d, p, (const u64*)(pal))
#define EB8H_32(s, d, p, pal) gs_block_expand_block8h_32((const u32*)(s), d, p, pal)
#define EB8H_16(s, d, p, pal) gs_block_expand_block8h_16((const u32*)(s), d, p, pal)
#define EB4HL_32(s, d, p, pal) gs_block_expand_block4hl_32((const u32*)(s), d, p, pal)
#define EB4HL_16(s, d, p, pal) gs_block_expand_block4hl_16((const u32*)(s), d, p, pal)
#define EB4HH_32(s, d, p, pal) gs_block_expand_block4hh_32((const u32*)(s), d, p, pal)
#define EB4HH_16(s, d, p, pal) gs_block_expand_block4hh_16((const u32*)(s), d, p, pal)
#define UW24(s, p, d) gs_block_unpack_and_write_block24(s, p, d)
#define UW8H(s, p, d) gs_block_unpack_and_write_block8h(s, p, d)
#define UW4HL(s, p, d) gs_block_unpack_and_write_block4hl(s, p, d)
#define UW4HH(s, p, d) gs_block_unpack_and_write_block4hh(s, p, d)
#define RE24(aem, s, d, p, t0) gs_block_read_and_expand_block24(aem, s, d, p, t0)
#define RE16(aem, s, d, p, t0, t1) gs_block_read_and_expand_block16(aem, s, d, p, t0, t1)
#define RE8_32(s, d, p, pal) gs_block_read_and_expand_block8_32(s, d, p, pal)
#define RE8_32HSW(s, d, p, pal) gs_block_read_and_expand_block8_32hsw(s, d, p, pal)
#define RE4_32(s, d, p, pal) gs_block_read_and_expand_block4_32(s, d, p, pal)
#define RE8H_32(s, d, p, pal) gs_block_read_and_expand_block8h_32(s, d, p, pal)
#define RE8H_32HSW(s, d, p, pal) gs_block_read_and_expand_block8h_32hsw(s, d, p, pal)
#define RE4HL_32(s, d, p, pal) gs_block_read_and_expand_block4hl_32(s, d, p, pal)
#define RE4HH_32(s, d, p, pal) gs_block_read_and_expand_block4hh_32(s, d, p, pal)

#include "scenario.h"

int main(int argc, char** argv)
{
	sc_init();
	sc_writes();
	sc_reads();
	sc_expands();
	sc_palettes();
	sc_unpacks();
	printf("gsblock: %016llx\n", sc_hash);
	if (argc > 1 && strtoull(argv[1], NULL, 16) != sc_hash)
	{
		printf("FAIL: the C++ GSBlock gave %s\n", argv[1]);
		return 1;
	}
	return 0;
}
