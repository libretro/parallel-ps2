
/*  EE opcode table audit.
 *
 *  The companion to tests/iop/tabaudit.c, for the larger EE tables. Every
 *  other test in this tree calls the op implementations directly, so
 *  nothing checks that R5900OpcodeTables.cpp points each encoding at the
 *  right one. A swapped pair there passes every test and breaks every
 *  game.
 *
 *  Eight tables, 352 entries: the primary table, SPECIAL, REGIMM, the MMI
 *  class table and its four subtables. Compared against the R5900
 *  encoding, so there is a published right answer rather than a
 *  plausible-looking one.
 *
 *  Unimplemented slots are compared as Unknown or MMI_Unknown rather than
 *  skipped, since an op landing in a reserved slot is the way these
 *  tables drift.
 *
 *  There is deliberately no second audit here for the EE recompiler, the
 *  way tests/iop/tabaudit.c has one for rpsxBSC and friends. The EE has
 *  no separate recompiler table to drift: OPCODE in R5900OpcodeTables.h
 *  carries both an interpret and a recompile pointer, and MakeOpcode
 *  fills them from one name token, as Interpreter::OpcodeImpl::name and
 *  ::rec##name. A slot cannot hold the interpreter for one op and the
 *  emitter for another. The IOP needed its own audit precisely because it
 *  does keep two tables, in R3000AOpcodeTables.c and iR3000Atables.cpp,
 *  which can disagree.
 *
 *  The arm64 EE recompiler is a third dispatch path and is not covered by
 *  either audit. It decodes with nested switch statements rather than
 *  tables -- roughly 190 case labels across switches on op, funct and rs
 *  in arm64/recR5900_arm64.cpp -- so there is no initialiser to read and
 *  compare. A misplaced case there is the same class of bug as a swapped
 *  table entry, and nothing here would catch it.
 *
 *  Usage: tests/ee/tabaudit <path-to-R5900OpcodeTables.cpp>
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/*  EE opcode table audit -- reference tables, from the R5900 encoding.
 *  Generated alongside tests/iop/tabaudit.c; see that file for why the
 *  dispatch needs checking separately from the implementations.
 */

static const char* const kStandard[64] = {
	"SPECIAL", "REGIMM", "J", "JAL",
	"BEQ", "BNE", "BLEZ", "BGTZ",
	"ADDI", "ADDIU", "SLTI", "SLTIU",
	"ANDI", "ORI", "XORI", "LUI",
	"COP0", "COP1", "COP2", "Unknown",
	"BEQL", "BNEL", "BLEZL", "BGTZL",
	"DADDI", "DADDIU", "LDL", "LDR",
	"MMI", "Unknown", "LQ", "SQ",
	"LB", "LH", "LWL", "LW",
	"LBU", "LHU", "LWR", "LWU",
	"SB", "SH", "SWL", "SW",
	"SDL", "SDR", "SWR", "CACHE",
	"Unknown", "LWC1", "Unknown", "PREF",
	"Unknown", "Unknown", "LQC2", "LD",
	"Unknown", "SWC1", "Unknown", "Unknown",
	"Unknown", "Unknown", "SQC2", "SD",
};

static const char* const kSpecial[64] = {
	"SLL", "Unknown", "SRL", "SRA",
	"SLLV", "Unknown", "SRLV", "SRAV",
	"JR", "JALR", "MOVZ", "MOVN",
	"SYSCALL", "BREAK", "Unknown", "SYNC",
	"MFHI", "MTHI", "MFLO", "MTLO",
	"DSLLV", "Unknown", "DSRLV", "DSRAV",
	"MULT", "MULTU", "DIV", "DIVU",
	"Unknown", "Unknown", "Unknown", "Unknown",
	"ADD", "ADDU", "SUB", "SUBU",
	"AND", "OR", "XOR", "NOR",
	"MFSA", "MTSA", "SLT", "SLTU",
	"DADD", "DADDU", "DSUB", "DSUBU",
	"TGE", "TGEU", "TLT", "TLTU",
	"TEQ", "Unknown", "TNE", "Unknown",
	"DSLL", "Unknown", "DSRL", "DSRA",
	"DSLL32", "Unknown", "DSRL32", "DSRA32",
};

static const char* const kRegImm[32] = {
	"BLTZ", "BGEZ", "BLTZL", "BGEZL",
	"Unknown", "Unknown", "Unknown", "Unknown",
	"TGEI", "TGEIU", "TLTI", "TLTIU",
	"TEQI", "Unknown", "TNEI", "Unknown",
	"BLTZAL", "BGEZAL", "BLTZALL", "BGEZALL",
	"Unknown", "Unknown", "Unknown", "Unknown",
	"MTSAB", "MTSAH", "Unknown", "Unknown",
	"Unknown", "Unknown", "Unknown", "Unknown",
};

static const char* const kMMI[64] = {
	"MADD", "MADDU", "MMI_Unknown", "MMI_Unknown",
	"PLZCW", "MMI_Unknown", "MMI_Unknown", "MMI_Unknown",
	"MMI0", "MMI2", "MMI_Unknown", "MMI_Unknown",
	"MMI_Unknown", "MMI_Unknown", "MMI_Unknown", "MMI_Unknown",
	"MFHI1", "MTHI1", "MFLO1", "MTLO1",
	"MMI_Unknown", "MMI_Unknown", "MMI_Unknown", "MMI_Unknown",
	"MULT1", "MULTU1", "DIV1", "DIVU1",
	"MMI_Unknown", "MMI_Unknown", "MMI_Unknown", "MMI_Unknown",
	"MADD1", "MADDU1", "MMI_Unknown", "MMI_Unknown",
	"MMI_Unknown", "MMI_Unknown", "MMI_Unknown", "MMI_Unknown",
	"MMI1", "MMI3", "MMI_Unknown", "MMI_Unknown",
	"MMI_Unknown", "MMI_Unknown", "MMI_Unknown", "MMI_Unknown",
	"PMFHL", "PMTHL", "MMI_Unknown", "MMI_Unknown",
	"PSLLH", "MMI_Unknown", "PSRLH", "PSRAH",
	"MMI_Unknown", "MMI_Unknown", "MMI_Unknown", "MMI_Unknown",
	"PSLLW", "MMI_Unknown", "PSRLW", "PSRAW",
};

static const char* const kMMI0[32] = {
	"PADDW", "PSUBW", "PCGTW", "PMAXW",
	"PADDH", "PSUBH", "PCGTH", "PMAXH",
	"PADDB", "PSUBB", "PCGTB", "MMI_Unknown",
	"MMI_Unknown", "MMI_Unknown", "MMI_Unknown", "MMI_Unknown",
	"PADDSW", "PSUBSW", "PEXTLW", "PPACW",
	"PADDSH", "PSUBSH", "PEXTLH", "PPACH",
	"PADDSB", "PSUBSB", "PEXTLB", "PPACB",
	"MMI_Unknown", "MMI_Unknown", "PEXT5", "PPAC5",
};

static const char* const kMMI1[32] = {
	"MMI_Unknown", "PABSW", "PCEQW", "PMINW",
	"PADSBH", "PABSH", "PCEQH", "PMINH",
	"MMI_Unknown", "MMI_Unknown", "PCEQB", "MMI_Unknown",
	"MMI_Unknown", "MMI_Unknown", "MMI_Unknown", "MMI_Unknown",
	"PADDUW", "PSUBUW", "PEXTUW", "MMI_Unknown",
	"PADDUH", "PSUBUH", "PEXTUH", "MMI_Unknown",
	"PADDUB", "PSUBUB", "PEXTUB", "QFSRV",
	"MMI_Unknown", "MMI_Unknown", "MMI_Unknown", "MMI_Unknown",
};

static const char* const kMMI2[32] = {
	"PMADDW", "MMI_Unknown", "PSLLVW", "PSRLVW",
	"PMSUBW", "MMI_Unknown", "MMI_Unknown", "MMI_Unknown",
	"PMFHI", "PMFLO", "PINTH", "MMI_Unknown",
	"PMULTW", "PDIVW", "PCPYLD", "MMI_Unknown",
	"PMADDH", "PHMADH", "PAND", "PXOR",
	"PMSUBH", "PHMSBH", "MMI_Unknown", "MMI_Unknown",
	"MMI_Unknown", "MMI_Unknown", "PEXEH", "PREVH",
	"PMULTH", "PDIVBW", "PEXEW", "PROT3W",
};

static const char* const kMMI3[32] = {
	"PMADDUW", "MMI_Unknown", "MMI_Unknown", "PSRAVW",
	"MMI_Unknown", "MMI_Unknown", "MMI_Unknown", "MMI_Unknown",
	"PMTHI", "PMTLO", "PINTEH", "MMI_Unknown",
	"PMULTUW", "PDIVUW", "PCPYUD", "MMI_Unknown",
	"MMI_Unknown", "MMI_Unknown", "POR", "PNOR",
	"MMI_Unknown", "MMI_Unknown", "MMI_Unknown", "MMI_Unknown",
	"MMI_Unknown", "MMI_Unknown", "PEXCH", "PCPYH",
	"MMI_Unknown", "MMI_Unknown", "PEXCW", "MMI_Unknown",
};

/* Pull one table's initialiser out of the file and split it on commas,
 * skipping the // comments the MMI tables carry inline. */
static int read_table(const char* src, const char* decl, char out[][32], int n)
{
	const char* p = strstr(src, decl);
	const char* end;
	int count = 0;
	if (!p) return -1;
	p = strchr(p, '{');
	if (!p) return -1;
	end = strchr(p, '}');
	if (!end) return -1;
	p++;
	while (p < end && count < n)
	{
		const char* q;
		int len;
		while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == ',')) p++;
		if (p >= end) break;
		if (p[0] == '/' && p[1] == '/')
		{ while (p < end && *p != '\n') p++; continue; }
		q = p;
		while (q < end && *q != ',' && *q != ' ' && *q != '\t' && *q != '\n') q++;
		len = (int)(q - p);
		if (len > 31) len = 31;
		memcpy(out[count], p, (size_t)len);
		out[count][len] = '\0';
		count++;
		p = q;
	}
	return count;
}

static int check(const char* what, char got[][32], int got_n,
                 const char* const* want, int want_n)
{
	int i, bad = 0;
	if (got_n != want_n)
	{ printf("  %s: read %d entries, expected %d\n", what, got_n, want_n); return 1; }
	for (i = 0; i < want_n; i++)
		if (strcmp(got[i], want[i]))
		{ printf("  %s[%d]: table has %s, the encoding says %s\n",
		         what, i, got[i], want[i]); bad++; }
	printf("%-14s %3d/%-3d entries match the R5900 encoding\n",
	       what, want_n - bad, want_n);
	return bad;
}

int main(int argc, char** argv)
{
	const char* path = (argc > 1) ? argv[1] : "R5900OpcodeTables.cpp";
	FILE* f = fopen(path, "rb");
	char* src;
	long len;
	static char buf[64][32];
	int bad = 0, n, i;

	const struct { const char* decl; const char* const* want; int n; } kAll[] = {
		{ "tbl_Standard[64]", kStandard, 64 },
		{ "tbl_Special[64]",  kSpecial,  64 },
		{ "tbl_RegImm[32]",   kRegImm,   32 },
		{ "tbl_MMI[64]",      kMMI,      64 },
		{ "tbl_MMI0[32]",     kMMI0,     32 },
		{ "tbl_MMI1[32]",     kMMI1,     32 },
		{ "tbl_MMI2[32]",     kMMI2,     32 },
		{ "tbl_MMI3[32]",     kMMI3,     32 },
	};

	if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }
	fseek(f, 0, SEEK_END); len = ftell(f); fseek(f, 0, SEEK_SET);
	src = (char*)malloc((size_t)len + 1);
	if (!src || fread(src, 1, (size_t)len, f) != (size_t)len) { fclose(f); return 2; }
	src[len] = '\0';
	fclose(f);

	for (i = 0; i < (int)(sizeof(kAll)/sizeof(kAll[0])); i++)
	{
		n = read_table(src, kAll[i].decl, buf, kAll[i].n);
		if (n < 0) { printf("  could not read %s\n", kAll[i].decl); bad++; continue; }
		bad += check(kAll[i].decl, buf, n, kAll[i].want, kAll[i].n);
	}

	free(src);
	printf("ee tabaudit: %s\n", bad ? "MISMATCHES FOUND" : "dispatch matches the encoding");
	return bad != 0;
}
