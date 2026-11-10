/* The interpreter's execution stages survive a cancelled instruction.
 *
 * The EE interpreter runs a boot in stages: from reset until the BIOS's
 * EELOAD main, then until the game's entry point, then the game, each
 * stage watching for the addresses that end it. A load or store at a
 * misaligned address cancels its instruction by jumping back to the top
 * of the execution loop, and the loop must carry on in the stage it was
 * in: a game resumed in the reset stage has its jumps through EELOAD's
 * main taken for a new boot, and on arm64 it would lose the recompiler,
 * which runs in the game stage only.
 *
 * Driven on the real Interpreter.c and R5900OpcodeImpl.c: a boot that
 * reaches EELOAD main once, enters the game, does a misaligned load, then
 * jumps through EELOAD main again on its way out. EELOAD main must have
 * been hooked once, and the cancelled load must have left its register
 * as it was.
 * C89. */
#include <stdio.h>

int            eestage_init(void);
void           eestage_free(void);
unsigned char* eestage_ram(void);
void           eestage_execute(unsigned pc, unsigned elf_entry);
int            eestage_hooks(void);
int            eestage_started(void);
int            eestage_exited(void);
unsigned       eestage_gpr(int n);

#define EELOAD_START 0x82000u
#define EELOAD_MAIN  0x82100u
#define ELF_ENTRY    0x100000u

#define T0 8
#define T1 9
#define T3 11
#define T4 12
#define S0 16

#define OP_J(t)            (0x08000000u | (((t) >> 2) & 0x3FFFFFFu))
#define OP_JAL(t)          (0x0C000000u | (((t) >> 2) & 0x3FFFFFFu))
#define OP_LUI(rt, i)      (0x3C000000u | ((unsigned)(rt) << 16) | ((i) & 0xFFFFu))
#define OP_ORI(rt, rs, i)  (0x34000000u | ((unsigned)(rs) << 21) | ((unsigned)(rt) << 16) | ((i) & 0xFFFFu))
#define OP_LW(rt, o, b)    (0x8C000000u | ((unsigned)(b) << 21) | ((unsigned)(rt) << 16) | ((o) & 0xFFFFu))
#define OP_BNE(rs, rt, o)  (0x14000000u | ((unsigned)(rs) << 21) | ((unsigned)(rt) << 16) | ((o) & 0xFFFFu))

static void put(unsigned addr, unsigned op)
{
	unsigned char *p = eestage_ram() + addr;
	p[0] = (unsigned char)op;
	p[1] = (unsigned char)(op >> 8);
	p[2] = (unsigned char)(op >> 16);
	p[3] = (unsigned char)(op >> 24);
}

int main(void)
{
	int failures = 0;

	if (!eestage_init())
	{
		printf("eestage: out of memory\n");
		return 2;
	}

	/* EELOAD: its main is found from the JAL at +0x9c; reset runs into
	 * EELOAD_START from below and then jumps to main. */
	put(EELOAD_START + 0x00, OP_J(EELOAD_MAIN));
	put(EELOAD_START + 0x9c, OP_JAL(EELOAD_MAIN));
	/* main: on the way in (s0 clear) on to the game; on the way out, the
	 * exit load. */
	put(EELOAD_MAIN + 0x00, OP_BNE(S0, 0, 3));
	put(EELOAD_MAIN + 0x08, OP_J(ELF_ENTRY));
	put(EELOAD_MAIN + 0x10, OP_LW(T3, 0, T4));
	/* The game: a misaligned load, then back out through EELOAD main. */
	put(ELF_ENTRY + 0x00, OP_LUI(T0, 0x0020));
	put(ELF_ENTRY + 0x04, OP_LUI(T4, 0x01F0));
	put(ELF_ENTRY + 0x08, OP_ORI(T1, 0, 0x1234));
	put(ELF_ENTRY + 0x0c, OP_ORI(S0, 0, 1));
	put(ELF_ENTRY + 0x10, OP_LW(T1, 1, T0));
	put(ELF_ENTRY + 0x14, OP_J(EELOAD_MAIN));

	eestage_execute(EELOAD_START - 0x10, ELF_ENTRY);

	if (!eestage_exited())
	{
		printf("  FAIL: the program did not reach its exit\n");
		failures++;
	}
	if (!eestage_started())
	{
		printf("  FAIL: the game stage was never entered\n");
		failures++;
	}
	if (eestage_hooks() != 1)
	{
		printf("  FAIL: EELOAD main was hooked %d times; after the cancelled load the game ran in the reset stage\n",
				eestage_hooks());
		failures++;
	}
	if (eestage_gpr(T1) != 0x1234)
	{
		printf("  FAIL: the cancelled load wrote its register (%08x)\n", eestage_gpr(T1));
		failures++;
	}
	eestage_free();
	printf(failures ? "ee stages: FAILED (%d)\n" : "ee stages: ok\n", failures);
	return failures != 0;
}
