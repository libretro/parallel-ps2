/* VUops.c, the real VU interpreter built as C89, driven through its own
 * dispatch tables with no PS2 test images: the ops whose C++ forms it
 * replaced, and the gamefix it now reads from the hot settings word.
 *
 * ABS goes through applyUnaryFunction, once a template, now a function
 * pointer; CLIP compares sign-flipped words that were static_casts;
 * FTOI saturates exponent-255 inputs by their sign, which std::isnan and
 * std::signbit decided and a bit test now does. vuDouble's clamp of an
 * exponent-255 operand to the largest finite value follows
 * vu0ExtraOverflow through EMU_HOT_VU0_EXTRA_OVERFLOW: off, ESADD of
 * one sees a host NaN; on, it squares the largest float to infinity. */
#include <stdio.h>
#include <string.h>

#include "Common.h"
#include "VUops.h"
#include "Gif_Unit.h"
#include "MTVU.h"
#include "HotConfig.h"

/* What VUops.c reaches for on paths this never takes: XGKICK, the MTVU
 * handoff and the EE's event queue. */
PCSX2_ALIGN(16) VURegs vuRegs[2];
PCSX2_ALIGN(16) cpuRegisters cpuRegs;
PCSX2_ALIGN(__pagealignsize) u8 eeHw[PS2MEM_HARDWARE];
VU_Thread vu1Thread;
u32 emu_hot;
void CPU_INT(EE_EventType n, s32 ecycle) { (void)n; (void)ecycle; }
u32 gif_unit_get_packet_size(GIF_PATH pathIdx, u8* pMem, u32 offset, u32 size, bool flush)
{
	(void)pathIdx; (void)pMem; (void)offset; (void)size; (void)flush;
	return 0;
}
u32 gif_unit_transfer(GIF_TRANSFER_TYPE tranType, u8* pMem, u32 size)
{
	(void)tranType; (void)pMem; (void)size;
	return 0;
}

#define FT(r)   ((u32)(r) << 16)
#define FS(r)   ((u32)(r) << 11)
#define DEST(d) ((u32)(d) << 21)
#define FSF(f)  ((u32)(f) << 21)
#define XYZW 0xF
#define DEST_XZ 0xA /* x is bit 3, w bit 0 */
#define DEST_YW 0x5

#define OP_FTOI0 0x17C
#define OP_ABS   0x1FD
#define OP_CLIP  0x1FF
#define OP_ESADD 0x73C

static int failures;
static void check(int ok, const char* what)
{
	printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok)
		failures++;
}

static void upper(u32 code)
{
	vuRegs[0].code = code;
	VU0_UPPER_OPCODE[code & 0x3f]();
}

static void lower(u32 code)
{
	vuRegs[0].code = code;
	VU0_LOWER_OPCODE[code >> 25]();
}

static void set_vf(int r, u32 x, u32 y, u32 z, u32 w)
{
	vuRegs[0].VF[r].i.x = x;
	vuRegs[0].VF[r].i.y = y;
	vuRegs[0].VF[r].i.z = z;
	vuRegs[0].VF[r].i.w = w;
}

static u32 p_bits(void)
{
	u32 b;
	memcpy(&b, &vuRegs[0].p.F, sizeof(b));
	return b;
}

int main(void)
{
	VURegs* vu = &vuRegs[0];

	/* ABS: the sign bit only, NaN payloads kept, and only where the
	 * destination mask says; VF0 is never written. */
	set_vf(1, 0xbf800000, 0xc0000000, 0x80000000, 0xffc00001);
	set_vf(2, 0x11111111, 0x22222222, 0x33333333, 0x44444444);
	upper(OP_ABS | DEST(DEST_YW) | FT(2) | FS(1));
	check(vu->VF[2].i.x == 0x11111111 && vu->VF[2].i.y == 0x40000000
		&& vu->VF[2].i.z == 0x33333333 && vu->VF[2].i.w == 0x7fc00001,
		"ABS clears the sign of the masked fields only, NaN payload kept");
	upper(OP_ABS | DEST(DEST_XZ) | FT(2) | FS(1));
	check(vu->VF[2].i.x == 0x3f800000 && vu->VF[2].i.z == 0x00000000 && vu->VF[2].i.y == 0x40000000,
		"ABS of x and z, y left as it was");
	set_vf(0, 0, 0, 0, 0x3f800000);
	upper(OP_ABS | DEST(XYZW) | FT(0) | FS(1));
	check(vu->VF[0].i.w == 0x3f800000, "ABS to VF0 writes nothing");

	/* CLIP x, y, z against |w|: +x over, -y under, z inside. */
	vu->clipflag = 0;
	set_vf(3, 0x40000000, 0xc0000000, 0x3f000000, 0);
	set_vf(4, 0, 0, 0, 0xbf800000);
	upper(OP_CLIP | DEST(XYZW) | FT(4) | FS(3));
	check(vu->clipflag == (0x01 | 0x08), "CLIP: +x over, -y under, z within |w|");
	upper(OP_CLIP | DEST(XYZW) | FT(4) | FS(3));
	check(vu->clipflag == ((0x01 | 0x08) << 6 | 0x01 | 0x08), "CLIP shifts the last judgement up");
	/* A denormal w is the largest denormal: only normals clip. */
	set_vf(4, 0, 0, 0, 0x00000001);
	set_vf(3, 0x00400000, 0x00800000, 0, 0);
	vu->clipflag = 0;
	upper(OP_CLIP | DEST(XYZW) | FT(4) | FS(3));
	check(vu->clipflag == 0x04, "CLIP: a denormal w lets denormals through, not normals");

	/* FTOI0 saturates the exponent-255 patterns by sign. */
	set_vf(5, 0x7fffffff, 0xffffffff, 0x3fc00000, 0xcf800000);
	upper(OP_FTOI0 | DEST(XYZW) | FT(6) | FS(5));
	check(vu->VF[6].SL[0] == 2147483647 && vu->VF[6].SL[1] == -2147483647 - 1,
		"FTOI0: exponent 255 saturates by sign");
	check(vu->VF[6].SL[2] == 1 && vu->VF[6].SL[3] == -2147483647 - 1,
		"FTOI0: 1.5 truncates to 1, -2^32 saturates");

	/* vuDouble's clamp, from the hot word. */
	set_vf(7, 0x7f800001, 0, 0, 0);
	emu_hot = 0;
	lower(0x80000000u | OP_ESADD | FS(7));
	check((p_bits() & 0x7f800000) == 0x7f800000 && (p_bits() & 0x007fffff) != 0,
		"vu0ExtraOverflow off: ESADD of an exponent-255 x is a NaN");
	emu_hot = EMU_HOT_VU0_EXTRA_OVERFLOW;
	lower(0x80000000u | OP_ESADD | FS(7));
	check(p_bits() == 0x7f800000, "vu0ExtraOverflow on: x clamps, its square is infinity");

	printf(failures ? "vuops: %d failure(s)\n" : "vuops: ok\n", failures);
	return failures != 0;
}
