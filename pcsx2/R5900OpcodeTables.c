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

/* Every R5900 opcode table. */

#include "R5900OpcodeTables.h"
#include "R5900.h"

#include "x86/iR5900AritImm.h"
#include "x86/iR5900Arit.h"
#include "x86/iR5900MultDiv.h"
#include "x86/iR5900Shift.h"
#include "x86/iR5900Branch.h"
#include "x86/iR5900Jump.h"
#include "x86/iR5900LoadStore.h"
#include "x86/iR5900Move.h"
#include "x86/iMMI.h"
#include "x86/iCOP0.h"
#include "x86/iFPU.h"

/* arm64 has no table-driven recompiler: its recompiler slots stay NULL. */
#if defined(ARCH_ARM64)
#define REC_OPCODE_PTR(fn) NULL
#else
#define REC_OPCODE_PTR(fn) fn
#endif

/* Each opcode name below is a macro for its table entry, so the tables
 * read as rows of mnemonics: the interpreter handler is int plus the
 * name, the recompiler handler rec plus the name. The names are only
 * macros from here to the end of this file. */
#define OP_ENTRY(name, cycles, flags) \
	{ #name, cycles, flags, NULL, int##name, REC_OPCODE_PTR(rec##name) }

#define OP_CLASS(name) \
	{ #name, 0, 0, Class_##name, NULL, NULL }

/* Cycle ratios. Still a work in progress. */
enum
{
	CYC_DEFAULT = 9,
	CYC_BRANCH = 11,
	CYC_COP_DEFAULT = 7,
	CYC_MULT = 2 * 8,
	CYC_DIV = 14 * 8,
	CYC_MMI_MULT = 3 * 8,
	CYC_MMI_DIV = 22 * 8,
	CYC_MMI_DEFAULT = 14,
	CYC_FPU_MULT = 4 * 8,
	CYC_STORE = 14,
	CYC_LOAD = 14
};

#define Unknown OP_ENTRY(Unknown, CYC_DEFAULT, 0)
#define MMI_Unknown OP_ENTRY(MMI_Unknown, CYC_DEFAULT, 0)
#define COP0_Unknown OP_ENTRY(COP0_Unknown, CYC_DEFAULT, 0)
#define COP1_Unknown OP_ENTRY(COP1_Unknown, CYC_DEFAULT, 0)

/* Class Subset Opcodes */
/* (not really opcodes, but rather entire subsets of other opcode classes) */

#define SPECIAL OP_CLASS(SPECIAL)
#define REGIMM OP_CLASS(REGIMM)
#define MMI OP_CLASS(MMI)
#define MMI0 OP_CLASS(MMI0)
#define MMI2 OP_CLASS(MMI2)
#define MMI1 OP_CLASS(MMI1)
#define MMI3 OP_CLASS(MMI3)

#define COP0 OP_CLASS(COP0)
#define COP1 OP_CLASS(COP1)

/* Misc Junk */

#define COP2 OP_ENTRY(COP2, CYC_DEFAULT, 0)

#define CACHE OP_ENTRY(CACHE, CYC_DEFAULT, 0)
#define PREF OP_ENTRY(PREF, CYC_DEFAULT, 0)
#define SYSCALL OP_ENTRY(SYSCALL, CYC_DEFAULT, IS_BRANCH|BRANCHTYPE_SYSCALL)
#define BREAK OP_ENTRY(BREAK, CYC_DEFAULT, 0)
#define SYNC OP_ENTRY(SYNC, CYC_DEFAULT, 0)

/* Branch/Jump Opcodes */

#define J OP_ENTRY(J, CYC_DEFAULT, IS_BRANCH|BRANCHTYPE_JUMP)
#define JAL OP_ENTRY(JAL, CYC_DEFAULT, IS_BRANCH|BRANCHTYPE_JUMP|IS_LINKED)
#define JR OP_ENTRY(JR, CYC_DEFAULT, IS_BRANCH|BRANCHTYPE_REGISTER)
#define JALR OP_ENTRY(JALR, CYC_DEFAULT, IS_BRANCH|BRANCHTYPE_REGISTER|IS_LINKED)

#define BEQ OP_ENTRY(BEQ, CYC_BRANCH, IS_BRANCH|BRANCHTYPE_BRANCH|CONDTYPE_EQ)
#define BNE OP_ENTRY(BNE, CYC_BRANCH, IS_BRANCH|BRANCHTYPE_BRANCH|CONDTYPE_NE)
#define BLEZ OP_ENTRY(BLEZ, CYC_BRANCH, IS_BRANCH|BRANCHTYPE_BRANCH|CONDTYPE_LEZ)
#define BGTZ OP_ENTRY(BGTZ, CYC_BRANCH, IS_BRANCH|BRANCHTYPE_BRANCH|CONDTYPE_GTZ)
#define BEQL OP_ENTRY(BEQL, CYC_BRANCH, IS_BRANCH|BRANCHTYPE_BRANCH|CONDTYPE_EQ|IS_LIKELY)
#define BNEL OP_ENTRY(BNEL, CYC_BRANCH, IS_BRANCH|BRANCHTYPE_BRANCH|CONDTYPE_NE|IS_LIKELY)
#define BLEZL OP_ENTRY(BLEZL, CYC_BRANCH, IS_BRANCH|BRANCHTYPE_BRANCH|CONDTYPE_LEZ|IS_LIKELY)
#define BGTZL OP_ENTRY(BGTZL, CYC_BRANCH, IS_BRANCH|BRANCHTYPE_BRANCH|CONDTYPE_GTZ|IS_LIKELY)
#define BLTZ OP_ENTRY(BLTZ, CYC_BRANCH, IS_BRANCH|BRANCHTYPE_BRANCH|CONDTYPE_LTZ)
#define BGEZ OP_ENTRY(BGEZ, CYC_BRANCH, IS_BRANCH|BRANCHTYPE_BRANCH|CONDTYPE_GEZ)
#define BLTZL OP_ENTRY(BLTZL, CYC_BRANCH, IS_BRANCH|BRANCHTYPE_BRANCH|CONDTYPE_LTZ|IS_LIKELY)
#define BGEZL OP_ENTRY(BGEZL, CYC_BRANCH, IS_BRANCH|BRANCHTYPE_BRANCH|CONDTYPE_GEZ|IS_LIKELY)
#define BLTZAL OP_ENTRY(BLTZAL, CYC_BRANCH, IS_BRANCH|BRANCHTYPE_BRANCH|CONDTYPE_LTZ|IS_LINKED)
#define BGEZAL OP_ENTRY(BGEZAL, CYC_BRANCH, IS_BRANCH|BRANCHTYPE_BRANCH|CONDTYPE_GEZ|IS_LINKED)
#define BLTZALL OP_ENTRY(BLTZALL, CYC_BRANCH, IS_BRANCH|BRANCHTYPE_BRANCH|CONDTYPE_LTZ|IS_LINKED|IS_LIKELY)
#define BGEZALL OP_ENTRY(BGEZALL, CYC_BRANCH, IS_BRANCH|BRANCHTYPE_BRANCH|CONDTYPE_GEZ|IS_LINKED|IS_LIKELY)

#define TGEI OP_ENTRY(TGEI, CYC_BRANCH, 0)
#define TGEIU OP_ENTRY(TGEIU, CYC_BRANCH, 0)
#define TLTI OP_ENTRY(TLTI, CYC_BRANCH, 0)
#define TLTIU OP_ENTRY(TLTIU, CYC_BRANCH, 0)
#define TEQI OP_ENTRY(TEQI, CYC_BRANCH, 0)
#define TNEI OP_ENTRY(TNEI, CYC_BRANCH, 0)
#define TGE OP_ENTRY(TGE, CYC_BRANCH, 0)
#define TGEU OP_ENTRY(TGEU, CYC_BRANCH, 0)
#define TLT OP_ENTRY(TLT, CYC_BRANCH, 0)
#define TLTU OP_ENTRY(TLTU, CYC_BRANCH, 0)
#define TEQ OP_ENTRY(TEQ, CYC_BRANCH, 0)
#define TNE OP_ENTRY(TNE, CYC_BRANCH, 0)

/* Arithmetic */

#define MULT OP_ENTRY(MULT, CYC_MULT, 0)
#define MULTU OP_ENTRY(MULTU, CYC_MULT, 0)
#define MULT1 OP_ENTRY(MULT1, CYC_MULT, 0)
#define MULTU1 OP_ENTRY(MULTU1, CYC_MULT, 0)
#define MADD OP_ENTRY(MADD, CYC_MULT, 0)
#define MADDU OP_ENTRY(MADDU, CYC_MULT, 0)
#define MADD1 OP_ENTRY(MADD1, CYC_MULT, 0)
#define MADDU1 OP_ENTRY(MADDU1, CYC_MULT, 0)
#define DIV OP_ENTRY(DIV, CYC_DIV, 0)
#define DIVU OP_ENTRY(DIVU, CYC_DIV, 0)
#define DIV1 OP_ENTRY(DIV1, CYC_DIV, 0)
#define DIVU1 OP_ENTRY(DIVU1, CYC_DIV, 0)

#define ADDI OP_ENTRY(ADDI, CYC_DEFAULT, IS_ALU|ALUTYPE_ADDI)
#define ADDIU OP_ENTRY(ADDIU, CYC_DEFAULT, IS_ALU|ALUTYPE_ADDI)
#define DADDI OP_ENTRY(DADDI, CYC_DEFAULT, IS_ALU|ALUTYPE_ADDI|IS_64BIT)
#define DADDIU OP_ENTRY(DADDIU, CYC_DEFAULT, IS_ALU|ALUTYPE_ADDI|IS_64BIT)
#define DADD OP_ENTRY(DADD, CYC_DEFAULT, IS_ALU|ALUTYPE_ADD|IS_64BIT)
#define DADDU OP_ENTRY(DADDU, CYC_DEFAULT, IS_ALU|ALUTYPE_ADD|IS_64BIT)
#define DSUB OP_ENTRY(DSUB, CYC_DEFAULT, IS_ALU|ALUTYPE_SUB|IS_64BIT)
#define DSUBU OP_ENTRY(DSUBU, CYC_DEFAULT, IS_ALU|ALUTYPE_SUB|IS_64BIT)
#define ADD OP_ENTRY(ADD, CYC_DEFAULT, IS_ALU|ALUTYPE_ADD)
#define ADDU OP_ENTRY(ADDU, CYC_DEFAULT, IS_ALU|ALUTYPE_ADD)
#define SUB OP_ENTRY(SUB, CYC_DEFAULT, IS_ALU|ALUTYPE_SUB)
#define SUBU OP_ENTRY(SUBU, CYC_DEFAULT, IS_ALU|ALUTYPE_SUB)

#define ANDI OP_ENTRY(ANDI, CYC_DEFAULT, 0)
#define ORI OP_ENTRY(ORI, CYC_DEFAULT, 0)
#define XORI OP_ENTRY(XORI, CYC_DEFAULT, 0)
#define AND OP_ENTRY(AND, CYC_DEFAULT, 0)
#define OR OP_ENTRY(OR, CYC_DEFAULT, 0)
#define XOR OP_ENTRY(XOR, CYC_DEFAULT, 0)
#define NOR OP_ENTRY(NOR, CYC_DEFAULT, 0)
#define SLTI OP_ENTRY(SLTI, CYC_DEFAULT, 0)
#define SLTIU OP_ENTRY(SLTIU, CYC_DEFAULT, 0)
#define SLT OP_ENTRY(SLT, CYC_DEFAULT, 0)
#define SLTU OP_ENTRY(SLTU, CYC_DEFAULT, 0)
#define LUI OP_ENTRY(LUI, CYC_DEFAULT, 0)
#define SLL OP_ENTRY(SLL, CYC_DEFAULT, 0)
#define SRL OP_ENTRY(SRL, CYC_DEFAULT, 0)
#define SRA OP_ENTRY(SRA, CYC_DEFAULT, 0)
#define SLLV OP_ENTRY(SLLV, CYC_DEFAULT, 0)
#define SRLV OP_ENTRY(SRLV, CYC_DEFAULT, 0)
#define SRAV OP_ENTRY(SRAV, CYC_DEFAULT, 0)
#define MOVZ OP_ENTRY(MOVZ, CYC_DEFAULT, IS_ALU|ALUTYPE_CONDMOVE|CONDTYPE_EQ)
#define MOVN OP_ENTRY(MOVN, CYC_DEFAULT, IS_ALU|ALUTYPE_CONDMOVE|CONDTYPE_NE)
#define DSLLV OP_ENTRY(DSLLV, CYC_DEFAULT, 0)
#define DSRLV OP_ENTRY(DSRLV, CYC_DEFAULT, 0)
#define DSRAV OP_ENTRY(DSRAV, CYC_DEFAULT, 0)
#define DSLL OP_ENTRY(DSLL, CYC_DEFAULT, 0)
#define DSRL OP_ENTRY(DSRL, CYC_DEFAULT, 0)
#define DSRA OP_ENTRY(DSRA, CYC_DEFAULT, 0)
#define DSLL32 OP_ENTRY(DSLL32, CYC_DEFAULT, 0)
#define DSRL32 OP_ENTRY(DSRL32, CYC_DEFAULT, 0)
#define DSRA32 OP_ENTRY(DSRA32, CYC_DEFAULT, 0)

#define MFHI OP_ENTRY(MFHI, CYC_DEFAULT, 0)
#define MTHI OP_ENTRY(MTHI, CYC_DEFAULT, 0)
#define MFLO OP_ENTRY(MFLO, CYC_DEFAULT, 0)
#define MTLO OP_ENTRY(MTLO, CYC_DEFAULT, 0)
#define MFSA OP_ENTRY(MFSA, CYC_DEFAULT, 0)
#define MTSA OP_ENTRY(MTSA, CYC_DEFAULT, 0)
#define MTSAB OP_ENTRY(MTSAB, CYC_DEFAULT, 0)
#define MTSAH OP_ENTRY(MTSAH, CYC_DEFAULT, 0)
#define MFHI1 OP_ENTRY(MFHI1, CYC_DEFAULT, 0)
#define MTHI1 OP_ENTRY(MTHI1, CYC_DEFAULT, 0)
#define MFLO1 OP_ENTRY(MFLO1, CYC_DEFAULT, 0)
#define MTLO1 OP_ENTRY(MTLO1, CYC_DEFAULT, 0)

/* Loads! */

#define LDL OP_ENTRY(LDL, CYC_LOAD, IS_MEMORY|IS_LOAD|MEMTYPE_DWORD|IS_LEFT)
#define LDR OP_ENTRY(LDR, CYC_LOAD, IS_MEMORY|IS_LOAD|MEMTYPE_DWORD|IS_RIGHT)
#define LQ OP_ENTRY(LQ, CYC_LOAD, IS_MEMORY|IS_LOAD|MEMTYPE_QWORD)
#define LB OP_ENTRY(LB, CYC_LOAD, IS_MEMORY|IS_LOAD|MEMTYPE_BYTE)
#define LH OP_ENTRY(LH, CYC_LOAD, IS_MEMORY|IS_LOAD|MEMTYPE_HALF)
#define LWL OP_ENTRY(LWL, CYC_LOAD, IS_MEMORY|IS_LOAD|MEMTYPE_WORD|IS_LEFT)
#define LW OP_ENTRY(LW, CYC_LOAD, IS_MEMORY|IS_LOAD|MEMTYPE_WORD)
#define LBU OP_ENTRY(LBU, CYC_LOAD, IS_MEMORY|IS_LOAD|MEMTYPE_BYTE)
#define LHU OP_ENTRY(LHU, CYC_LOAD, IS_MEMORY|IS_LOAD|MEMTYPE_HALF)
#define LWR OP_ENTRY(LWR, CYC_LOAD, IS_MEMORY|IS_LOAD|MEMTYPE_WORD|IS_RIGHT)
#define LWU OP_ENTRY(LWU, CYC_LOAD, IS_MEMORY|IS_LOAD|MEMTYPE_WORD)
#define LWC1 OP_ENTRY(LWC1, CYC_LOAD, IS_MEMORY|IS_LOAD|MEMTYPE_WORD)
#define LQC2 OP_ENTRY(LQC2, CYC_LOAD, IS_MEMORY|IS_LOAD|MEMTYPE_QWORD)
#define LD OP_ENTRY(LD, CYC_LOAD, IS_MEMORY|IS_LOAD|MEMTYPE_DWORD)

/* Stores! */

#define SQ OP_ENTRY(SQ, CYC_STORE, IS_MEMORY|IS_STORE|MEMTYPE_QWORD)
#define SB OP_ENTRY(SB, CYC_STORE, IS_MEMORY|IS_STORE|MEMTYPE_BYTE)
#define SH OP_ENTRY(SH, CYC_STORE, IS_MEMORY|IS_STORE|MEMTYPE_HALF)
#define SWL OP_ENTRY(SWL, CYC_STORE, IS_MEMORY|IS_STORE|MEMTYPE_WORD|IS_LEFT)
#define SW OP_ENTRY(SW, CYC_STORE, IS_MEMORY|IS_STORE|MEMTYPE_WORD)
#define SDL OP_ENTRY(SDL, CYC_STORE, IS_MEMORY|IS_STORE|MEMTYPE_DWORD|IS_LEFT)
#define SDR OP_ENTRY(SDR, CYC_STORE, IS_MEMORY|IS_STORE|MEMTYPE_DWORD|IS_RIGHT)
#define SWR OP_ENTRY(SWR, CYC_STORE, IS_MEMORY|IS_STORE|MEMTYPE_WORD|IS_RIGHT)
#define SWC1 OP_ENTRY(SWC1, CYC_STORE, IS_MEMORY|IS_STORE|MEMTYPE_WORD)
#define SQC2 OP_ENTRY(SQC2, CYC_STORE, IS_MEMORY|IS_STORE|MEMTYPE_QWORD)
#define SD OP_ENTRY(SD, CYC_STORE, IS_MEMORY|IS_STORE|MEMTYPE_DWORD)

/* Multimedia Instructions! */

#define PLZCW OP_ENTRY(PLZCW, CYC_MMI_DEFAULT, 0)
#define PMFHL OP_ENTRY(PMFHL, CYC_MMI_DEFAULT, 0)
#define PMTHL OP_ENTRY(PMTHL, CYC_MMI_DEFAULT, 0)
#define PSLLH OP_ENTRY(PSLLH, CYC_MMI_DEFAULT, 0)
#define PSRLH OP_ENTRY(PSRLH, CYC_MMI_DEFAULT, 0)
#define PSRAH OP_ENTRY(PSRAH, CYC_MMI_DEFAULT, 0)
#define PSLLW OP_ENTRY(PSLLW, CYC_MMI_DEFAULT, 0)
#define PSRLW OP_ENTRY(PSRLW, CYC_MMI_DEFAULT, 0)
#define PSRAW OP_ENTRY(PSRAW, CYC_MMI_DEFAULT, 0)

#define PADDW OP_ENTRY(PADDW, CYC_MMI_DEFAULT, 0)
#define PADDH OP_ENTRY(PADDH, CYC_MMI_DEFAULT, 0)
#define PADDB OP_ENTRY(PADDB, CYC_MMI_DEFAULT, 0)
#define PADDSW OP_ENTRY(PADDSW, CYC_MMI_DEFAULT, 0)
#define PADDSH OP_ENTRY(PADDSH, CYC_MMI_DEFAULT, 0)
#define PADDSB OP_ENTRY(PADDSB, CYC_MMI_DEFAULT, 0)
#define PADDUW OP_ENTRY(PADDUW, CYC_MMI_DEFAULT, 0)
#define PADDUH OP_ENTRY(PADDUH, CYC_MMI_DEFAULT, 0)
#define PADDUB OP_ENTRY(PADDUB, CYC_MMI_DEFAULT, 0)
#define PSUBW OP_ENTRY(PSUBW, CYC_MMI_DEFAULT, 0)
#define PSUBH OP_ENTRY(PSUBH, CYC_MMI_DEFAULT, 0)
#define PSUBB OP_ENTRY(PSUBB, CYC_MMI_DEFAULT, 0)
#define PSUBSW OP_ENTRY(PSUBSW, CYC_MMI_DEFAULT, 0)
#define PSUBSH OP_ENTRY(PSUBSH, CYC_MMI_DEFAULT, 0)
#define PSUBSB OP_ENTRY(PSUBSB, CYC_MMI_DEFAULT, 0)
#define PSUBUW OP_ENTRY(PSUBUW, CYC_MMI_DEFAULT, 0)
#define PSUBUH OP_ENTRY(PSUBUH, CYC_MMI_DEFAULT, 0)
#define PSUBUB OP_ENTRY(PSUBUB, CYC_MMI_DEFAULT, 0)

#define PCGTW OP_ENTRY(PCGTW, CYC_MMI_DEFAULT, 0)
#define PMAXW OP_ENTRY(PMAXW, CYC_MMI_DEFAULT, 0)
#define PMAXH OP_ENTRY(PMAXH, CYC_MMI_DEFAULT, 0)
#define PCGTH OP_ENTRY(PCGTH, CYC_MMI_DEFAULT, 0)
#define PCGTB OP_ENTRY(PCGTB, CYC_MMI_DEFAULT, 0)
#define PEXTLW OP_ENTRY(PEXTLW, CYC_MMI_DEFAULT, 0)
#define PEXTLH OP_ENTRY(PEXTLH, CYC_MMI_DEFAULT, 0)
#define PEXTLB OP_ENTRY(PEXTLB, CYC_MMI_DEFAULT, 0)
#define PEXT5 OP_ENTRY(PEXT5, CYC_MMI_DEFAULT, 0)
#define PPACW OP_ENTRY(PPACW, CYC_MMI_DEFAULT, 0)
#define PPACH OP_ENTRY(PPACH, CYC_MMI_DEFAULT, 0)
#define PPACB OP_ENTRY(PPACB, CYC_MMI_DEFAULT, 0)
#define PPAC5 OP_ENTRY(PPAC5, CYC_MMI_DEFAULT, 0)

#define PABSW OP_ENTRY(PABSW, CYC_MMI_DEFAULT, 0)
#define PABSH OP_ENTRY(PABSH, CYC_MMI_DEFAULT, 0)
#define PCEQW OP_ENTRY(PCEQW, CYC_MMI_DEFAULT, 0)
#define PMINW OP_ENTRY(PMINW, CYC_MMI_DEFAULT, 0)
#define PMINH OP_ENTRY(PMINH, CYC_MMI_DEFAULT, 0)
#define PADSBH OP_ENTRY(PADSBH, CYC_MMI_DEFAULT, 0)
#define PCEQH OP_ENTRY(PCEQH, CYC_MMI_DEFAULT, 0)
#define PCEQB OP_ENTRY(PCEQB, CYC_MMI_DEFAULT, 0)
#define PEXTUW OP_ENTRY(PEXTUW, CYC_MMI_DEFAULT, 0)
#define PEXTUH OP_ENTRY(PEXTUH, CYC_MMI_DEFAULT, 0)
#define PEXTUB OP_ENTRY(PEXTUB, CYC_MMI_DEFAULT, 0)
#define PSLLVW OP_ENTRY(PSLLVW, CYC_MMI_DEFAULT, 0)
#define PSRLVW OP_ENTRY(PSRLVW, CYC_MMI_DEFAULT, 0)

#define QFSRV OP_ENTRY(QFSRV, CYC_MMI_DEFAULT, 0)

#define PMADDH OP_ENTRY(PMADDH, CYC_MMI_MULT, 0)
#define PHMADH OP_ENTRY(PHMADH, CYC_MMI_MULT, 0)
#define PMSUBH OP_ENTRY(PMSUBH, CYC_MMI_MULT, 0)
#define PHMSBH OP_ENTRY(PHMSBH, CYC_MMI_MULT, 0)
#define PMULTH OP_ENTRY(PMULTH, CYC_MMI_MULT, 0)
#define PMADDW OP_ENTRY(PMADDW, CYC_MMI_MULT, 0)
#define PMSUBW OP_ENTRY(PMSUBW, CYC_MMI_MULT, 0)
#define PMFHI OP_ENTRY(PMFHI, CYC_MMI_MULT, 0)
#define PMFLO OP_ENTRY(PMFLO, CYC_MMI_MULT, 0)
#define PMULTW OP_ENTRY(PMULTW, CYC_MMI_MULT, 0)
#define PMADDUW OP_ENTRY(PMADDUW, CYC_MMI_MULT, 0)
#define PMULTUW OP_ENTRY(PMULTUW, CYC_MMI_MULT, 0)
#define PDIVUW OP_ENTRY(PDIVUW, CYC_MMI_DIV, 0)
#define PDIVW OP_ENTRY(PDIVW, CYC_MMI_DIV, 0)
#define PDIVBW OP_ENTRY(PDIVBW, CYC_MMI_DIV, 0)

#define PINTH OP_ENTRY(PINTH, CYC_MMI_DEFAULT, 0)
#define PCPYLD OP_ENTRY(PCPYLD, CYC_MMI_DEFAULT, 0)
#define PAND OP_ENTRY(PAND, CYC_MMI_DEFAULT, 0)
#define PXOR OP_ENTRY(PXOR, CYC_MMI_DEFAULT, 0)
#define PEXEH OP_ENTRY(PEXEH, CYC_MMI_DEFAULT, 0)
#define PREVH OP_ENTRY(PREVH, CYC_MMI_DEFAULT, 0)
#define PEXEW OP_ENTRY(PEXEW, CYC_MMI_DEFAULT, 0)
#define PROT3W OP_ENTRY(PROT3W, CYC_MMI_DEFAULT, 0)

#define PSRAVW OP_ENTRY(PSRAVW, CYC_MMI_DEFAULT, 0)
#define PMTHI OP_ENTRY(PMTHI, CYC_MMI_DEFAULT, 0)
#define PMTLO OP_ENTRY(PMTLO, CYC_MMI_DEFAULT, 0)
#define PINTEH OP_ENTRY(PINTEH, CYC_MMI_DEFAULT, 0)
#define PCPYUD OP_ENTRY(PCPYUD, CYC_MMI_DEFAULT, 0)
#define POR OP_ENTRY(POR, CYC_MMI_DEFAULT, 0)
#define PNOR OP_ENTRY(PNOR, CYC_MMI_DEFAULT, 0)
#define PEXCH OP_ENTRY(PEXCH, CYC_MMI_DEFAULT, 0)
#define PCPYH OP_ENTRY(PCPYH, CYC_MMI_DEFAULT, 0)
#define PEXCW OP_ENTRY(PEXCW, CYC_MMI_DEFAULT, 0)

/* COP0 Instructions */

#define COP0_C0 OP_CLASS(COP0_C0)
#define COP0_BC0 OP_CLASS(COP0_BC0)

#define MFC0 OP_ENTRY(MFC0, CYC_COP_DEFAULT, 0)
#define MTC0 OP_ENTRY(MTC0, CYC_COP_DEFAULT, 0)

#define BC0F OP_ENTRY(BC0F, CYC_BRANCH, IS_BRANCH | BRANCHTYPE_BC0 | CONDTYPE_EQ)
#define BC0T OP_ENTRY(BC0T, CYC_BRANCH, IS_BRANCH | BRANCHTYPE_BC0 | CONDTYPE_NE)
#define BC0FL OP_ENTRY(BC0FL, CYC_BRANCH, IS_BRANCH | BRANCHTYPE_BC0 | CONDTYPE_EQ | IS_LIKELY)
#define BC0TL OP_ENTRY(BC0TL, CYC_BRANCH, IS_BRANCH | BRANCHTYPE_BC0 | CONDTYPE_NE | IS_LIKELY)

#define TLBR OP_ENTRY(TLBR, CYC_COP_DEFAULT, 0)
#define TLBWI OP_ENTRY(TLBWI, CYC_COP_DEFAULT, 0)
#define TLBWR OP_ENTRY(TLBWR, CYC_COP_DEFAULT, 0)
#define TLBP OP_ENTRY(TLBP, CYC_COP_DEFAULT, 0)
#define ERET OP_ENTRY(ERET, CYC_COP_DEFAULT, IS_BRANCH|BRANCHTYPE_ERET)
#define EI OP_ENTRY(EI, CYC_COP_DEFAULT, 0)
#define DI OP_ENTRY(DI, CYC_COP_DEFAULT, 0)

/* COP1 Instructions! */

#define COP1_BC1 OP_CLASS(COP1_BC1)
#define COP1_S OP_CLASS(COP1_S)
#define COP1_W OP_CLASS(COP1_W) /* contains CVT_S instruction *only* */

#define MFC1 OP_ENTRY(MFC1, CYC_COP_DEFAULT, 0)
#define CFC1 OP_ENTRY(CFC1, CYC_COP_DEFAULT, 0)
#define MTC1 OP_ENTRY(MTC1, CYC_COP_DEFAULT, 0)
#define CTC1 OP_ENTRY(CTC1, CYC_COP_DEFAULT, 0)

#define BC1F OP_ENTRY(BC1F, CYC_BRANCH, IS_BRANCH|BRANCHTYPE_BC1|CONDTYPE_EQ)
#define BC1T OP_ENTRY(BC1T, CYC_BRANCH, IS_BRANCH|BRANCHTYPE_BC1|CONDTYPE_NE)
#define BC1FL OP_ENTRY(BC1FL, CYC_BRANCH, IS_BRANCH|BRANCHTYPE_BC1|CONDTYPE_EQ|IS_LIKELY)
#define BC1TL OP_ENTRY(BC1TL, CYC_BRANCH, IS_BRANCH|BRANCHTYPE_BC1|CONDTYPE_NE|IS_LIKELY)

#define ADD_S OP_ENTRY(ADD_S, CYC_COP_DEFAULT, 0)
#define ADDA_S OP_ENTRY(ADDA_S, CYC_COP_DEFAULT, 0)
#define SUB_S OP_ENTRY(SUB_S, CYC_COP_DEFAULT, 0)
#define SUBA_S OP_ENTRY(SUBA_S, CYC_COP_DEFAULT, 0)

#define ABS_S OP_ENTRY(ABS_S, CYC_COP_DEFAULT, 0)
#define MOV_S OP_ENTRY(MOV_S, CYC_COP_DEFAULT, 0)
#define NEG_S OP_ENTRY(NEG_S, CYC_COP_DEFAULT, 0)
#define MAX_S OP_ENTRY(MAX_S, CYC_COP_DEFAULT, 0)
#define MIN_S OP_ENTRY(MIN_S, CYC_COP_DEFAULT, 0)

#define MUL_S OP_ENTRY(MUL_S, CYC_FPU_MULT, 0)
#define DIV_S OP_ENTRY(DIV_S, 6*8, 0)
#define SQRT_S OP_ENTRY(SQRT_S, 6*8, 0)
#define RSQRT_S OP_ENTRY(RSQRT_S, 8*8, 0)
#define MULA_S OP_ENTRY(MULA_S, CYC_FPU_MULT, 0)
#define MADD_S OP_ENTRY(MADD_S, CYC_FPU_MULT, 0)
#define MSUB_S OP_ENTRY(MSUB_S, CYC_FPU_MULT, 0)
#define MADDA_S OP_ENTRY(MADDA_S, CYC_FPU_MULT, 0)
#define MSUBA_S OP_ENTRY(MSUBA_S, CYC_FPU_MULT, 0)

#define C_F OP_ENTRY(C_F, CYC_COP_DEFAULT, 0)
#define C_EQ OP_ENTRY(C_EQ, CYC_COP_DEFAULT, 0)
#define C_LT OP_ENTRY(C_LT, CYC_COP_DEFAULT, 0)
#define C_LE OP_ENTRY(C_LE, CYC_COP_DEFAULT, 0)

#define CVT_S OP_ENTRY(CVT_S, CYC_COP_DEFAULT, 0)
#define CVT_W OP_ENTRY(CVT_W, CYC_COP_DEFAULT, 0)

const OPCODE tbl_Standard[64] =
{
	SPECIAL,       REGIMM,        J,             JAL,     BEQ,           BNE,     BLEZ,  BGTZ,
	ADDI,          ADDIU,         SLTI,          SLTIU,   ANDI,          ORI,     XORI,  LUI,
	COP0,          COP1,          COP2,          Unknown, BEQL,          BNEL,    BLEZL, BGTZL,
	DADDI,         DADDIU,        LDL,           LDR,     MMI,           Unknown, LQ,    SQ,
	LB,            LH,            LWL,           LW,      LBU,           LHU,     LWR,   LWU,
	SB,            SH,            SWL,           SW,      SDL,           SDR,     SWR,   CACHE,
	Unknown,       LWC1,          Unknown,       PREF,    Unknown,       Unknown, LQC2,  LD,
	Unknown,       SWC1,          Unknown,       Unknown, Unknown,       Unknown, SQC2,  SD
};

static const OPCODE tbl_Special[64] =
{
	SLL,      Unknown,  SRL,      SRA,      SLLV,    Unknown, SRLV,    SRAV,
	JR,       JALR,     MOVZ,     MOVN,     SYSCALL, BREAK,   Unknown, SYNC,
	MFHI,     MTHI,     MFLO,     MTLO,     DSLLV,   Unknown, DSRLV,   DSRAV,
	MULT,     MULTU,    DIV,      DIVU,     Unknown, Unknown, Unknown, Unknown,
	ADD,      ADDU,     SUB,      SUBU,     AND,     OR,      XOR,     NOR,
	MFSA,     MTSA,     SLT,      SLTU,     DADD,    DADDU,   DSUB,    DSUBU,
	TGE,      TGEU,     TLT,      TLTU,     TEQ,     Unknown, TNE,     Unknown,
	DSLL,     Unknown,  DSRL,     DSRA,     DSLL32,  Unknown, DSRL32,  DSRA32
};

static const OPCODE tbl_RegImm[32] = {
	BLTZ,   BGEZ,   BLTZL,      BGEZL,   Unknown, Unknown, Unknown, Unknown,
	TGEI,   TGEIU,  TLTI,       TLTIU,   TEQI,    Unknown, TNEI,    Unknown,
	BLTZAL, BGEZAL, BLTZALL,    BGEZALL, Unknown, Unknown, Unknown, Unknown,
	MTSAB,  MTSAH , Unknown,    Unknown, Unknown, Unknown, Unknown, Unknown,
};

static const OPCODE tbl_MMI[64] =
{
	MADD,               MADDU,           MMI_Unknown,          MMI_Unknown,          PLZCW,            MMI_Unknown,       MMI_Unknown,          MMI_Unknown,
	MMI0,      MMI2,   MMI_Unknown,          MMI_Unknown,          MMI_Unknown,      MMI_Unknown,       MMI_Unknown,          MMI_Unknown,
	MFHI1,              MTHI1,           MFLO1,                MTLO1,                MMI_Unknown,      MMI_Unknown,       MMI_Unknown,          MMI_Unknown,
	MULT1,              MULTU1,          DIV1,                 DIVU1,                MMI_Unknown,      MMI_Unknown,       MMI_Unknown,          MMI_Unknown,
	MADD1,              MADDU1,          MMI_Unknown,          MMI_Unknown,          MMI_Unknown,      MMI_Unknown,       MMI_Unknown,          MMI_Unknown,
	MMI1,      MMI3,   MMI_Unknown,          MMI_Unknown,          MMI_Unknown,      MMI_Unknown,       MMI_Unknown,          MMI_Unknown,
	PMFHL,              PMTHL,           MMI_Unknown,          MMI_Unknown,          PSLLH,            MMI_Unknown,       PSRLH,                PSRAH,
	MMI_Unknown,        MMI_Unknown,     MMI_Unknown,          MMI_Unknown,          PSLLW,            MMI_Unknown,       PSRLW,                PSRAW,
};

static const OPCODE tbl_MMI0[32] =
{
	PADDW,         PSUBW,         PCGTW,          PMAXW,
	PADDH,         PSUBH,         PCGTH,          PMAXH,
	PADDB,         PSUBB,         PCGTB,          MMI_Unknown,
	MMI_Unknown,   MMI_Unknown,   MMI_Unknown,    MMI_Unknown,
	PADDSW,        PSUBSW,        PEXTLW,         PPACW,
	PADDSH,        PSUBSH,        PEXTLH,         PPACH,
	PADDSB,        PSUBSB,        PEXTLB,         PPACB,
	MMI_Unknown,   MMI_Unknown,   PEXT5,          PPAC5,
};

static const OPCODE tbl_MMI1[32] =
{
	MMI_Unknown,   PABSW,         PCEQW,         PMINW,
	PADSBH,        PABSH,         PCEQH,         PMINH,
	MMI_Unknown,   MMI_Unknown,   PCEQB,         MMI_Unknown,
	MMI_Unknown,   MMI_Unknown,   MMI_Unknown,   MMI_Unknown,
	PADDUW,        PSUBUW,        PEXTUW,        MMI_Unknown,
	PADDUH,        PSUBUH,        PEXTUH,        MMI_Unknown,
	PADDUB,        PSUBUB,        PEXTUB,        QFSRV,
	MMI_Unknown,   MMI_Unknown,   MMI_Unknown,   MMI_Unknown,
};

static const OPCODE tbl_MMI2[32] =
{
	PMADDW,        MMI_Unknown,   PSLLVW,        PSRLVW,
	PMSUBW,        MMI_Unknown,   MMI_Unknown,   MMI_Unknown,
	PMFHI,         PMFLO,         PINTH,         MMI_Unknown,
	PMULTW,        PDIVW,         PCPYLD,        MMI_Unknown,
	PMADDH,        PHMADH,        PAND,          PXOR,
	PMSUBH,        PHMSBH,        MMI_Unknown,   MMI_Unknown,
	MMI_Unknown,   MMI_Unknown,   PEXEH,         PREVH,
	PMULTH,        PDIVBW,        PEXEW,         PROT3W,
};

static const OPCODE tbl_MMI3[32] =
{
	PMADDUW,       MMI_Unknown,   MMI_Unknown,   PSRAVW,
	MMI_Unknown,   MMI_Unknown,   MMI_Unknown,   MMI_Unknown,
	PMTHI,         PMTLO,         PINTEH,        MMI_Unknown,
	PMULTUW,       PDIVUW,        PCPYUD,        MMI_Unknown,
	MMI_Unknown,   MMI_Unknown,   POR,           PNOR,
	MMI_Unknown,   MMI_Unknown,   MMI_Unknown,   MMI_Unknown,
	MMI_Unknown,   MMI_Unknown,   PEXCH,         PCPYH,
	MMI_Unknown,   MMI_Unknown,   PEXCW,         MMI_Unknown,
};

static const OPCODE tbl_COP0[32] =
{
	MFC0,         COP0_Unknown, COP0_Unknown, COP0_Unknown, MTC0,         COP0_Unknown, COP0_Unknown, COP0_Unknown,
	COP0_BC0, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown,
	COP0_C0,  COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown,
	COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown,
};

static const OPCODE tbl_COP0_BC0[32] =
{
	BC0F,         BC0T,         BC0FL,        BC0TL,        COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown,
	COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown,
	COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown,
	COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown,
};

static const OPCODE tbl_COP0_C0[64] =
{
	COP0_Unknown, TLBR,         TLBWI,        COP0_Unknown, COP0_Unknown, COP0_Unknown, TLBWR,        COP0_Unknown,
	TLBP,         COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown,
	COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown,
	ERET,         COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown,
	COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown,
	COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown,
	COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown,
	EI,           DI,           COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown
};

static const OPCODE tbl_COP1[32] =
{
	MFC1,         COP1_Unknown, CFC1,         COP1_Unknown, MTC1,         COP1_Unknown, CTC1,         COP1_Unknown,
	COP1_BC1, COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_Unknown,
	COP1_S,   COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_W, COP1_Unknown, COP1_Unknown, COP1_Unknown,
	COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_Unknown,
};

static const OPCODE tbl_COP1_BC1[32] =
{
	BC1F,         BC1T,         BC1FL,        BC1TL,        COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_Unknown,
	COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_Unknown,
	COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_Unknown,
	COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_Unknown,
};

static const OPCODE tbl_COP1_S[64] =
{
	ADD_S,       SUB_S,       MUL_S,       DIV_S,       SQRT_S,      ABS_S,       MOV_S,       NEG_S,
	COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,
	COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,RSQRT_S,     COP1_Unknown,
	ADDA_S,      SUBA_S,      MULA_S,      COP1_Unknown,MADD_S,      MSUB_S,      MADDA_S,     MSUBA_S,
	COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,CVT_W,       COP1_Unknown,COP1_Unknown,COP1_Unknown,
	MAX_S,       MIN_S,       COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,
	C_F,         COP1_Unknown,C_EQ,        COP1_Unknown,C_LT,        COP1_Unknown,C_LE,        COP1_Unknown,
	COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,
};

static const OPCODE tbl_COP1_W[64] =
{
	COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,
	COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,
	COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,
	COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,
	CVT_S,       COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,
	COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,
	COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,
	COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,
};

const OPCODE* Class_SPECIAL(u32 op) { return &tbl_Special[op & 0x3F]; }
const OPCODE* Class_REGIMM(u32 op)  { return &tbl_RegImm[(op >> 16) & 0x1F]; }

const OPCODE* Class_MMI(u32 op)  { return &tbl_MMI[op & 0x3F]; }
const OPCODE* Class_MMI0(u32 op) { return &tbl_MMI0[(op >> 6) & 0x1F]; }
const OPCODE* Class_MMI1(u32 op) { return &tbl_MMI1[(op >> 6) & 0x1F]; }
const OPCODE* Class_MMI2(u32 op) { return &tbl_MMI2[(op >> 6) & 0x1F]; }
const OPCODE* Class_MMI3(u32 op) { return &tbl_MMI3[(op >> 6) & 0x1F]; }

const OPCODE* Class_COP0(u32 op) { return &tbl_COP0[(op >> 21) & 0x1F]; }
const OPCODE* Class_COP0_BC0(u32 op) { return &tbl_COP0_BC0[(op >> 16) & 0x03]; }
const OPCODE* Class_COP0_C0(u32 op) { return &tbl_COP0_C0[op & 0x3F]; }

const OPCODE* Class_COP1(u32 op) { return &tbl_COP1[(op >> 21) & 0x1F]; }
const OPCODE* Class_COP1_BC1(u32 op) { return &tbl_COP1_BC1[(op >> 16) & 0x1F]; }
const OPCODE* Class_COP1_S(u32 op) { return &tbl_COP1_S[op & 0x3F]; }
const OPCODE* Class_COP1_W(u32 op) { return &tbl_COP1_W[op & 0x3F]; }

void (*Int_COP2PrintTable[32])(void) = {
    COP2_Unknown, QMFC2,        CFC2,         COP2_Unknown, COP2_Unknown, QMTC2,        CTC2,         COP2_Unknown,
    COP2_BC2,     COP2_Unknown, COP2_Unknown, COP2_Unknown, COP2_Unknown, COP2_Unknown, COP2_Unknown, COP2_Unknown,
    COP2_SPECIAL, COP2_SPECIAL, COP2_SPECIAL, COP2_SPECIAL, COP2_SPECIAL, COP2_SPECIAL, COP2_SPECIAL, COP2_SPECIAL,
	COP2_SPECIAL, COP2_SPECIAL, COP2_SPECIAL, COP2_SPECIAL, COP2_SPECIAL, COP2_SPECIAL, COP2_SPECIAL, COP2_SPECIAL,
};

void (*Int_COP2BC2PrintTable[32])(void) = {
    BC2F,         BC2T,         BC2FL,        BC2TL,        COP2_Unknown, COP2_Unknown, COP2_Unknown, COP2_Unknown,
    COP2_Unknown, COP2_Unknown, COP2_Unknown, COP2_Unknown, COP2_Unknown, COP2_Unknown, COP2_Unknown, COP2_Unknown,
    COP2_Unknown, COP2_Unknown, COP2_Unknown, COP2_Unknown, COP2_Unknown, COP2_Unknown, COP2_Unknown, COP2_Unknown,
    COP2_Unknown, COP2_Unknown, COP2_Unknown, COP2_Unknown, COP2_Unknown, COP2_Unknown, COP2_Unknown, COP2_Unknown,
};

void (*Int_COP2SPECIAL1PrintTable[64])(void) =
{
 VADDx,       VADDy,       VADDz,       VADDw,       VSUBx,        VSUBy,        VSUBz,        VSUBw,
 VMADDx,      VMADDy,      VMADDz,      VMADDw,      VMSUBx,       VMSUBy,       VMSUBz,       VMSUBw,
 VMAXx,       VMAXy,       VMAXz,       VMAXw,       VMINIx,       VMINIy,       VMINIz,       VMINIw,
 VMULx,       VMULy,       VMULz,       VMULw,       VMULq,        VMAXi,        VMULi,        VMINIi,
 VADDq,       VMADDq,      VADDi,       VMADDi,      VSUBq,        VMSUBq,       VSUBi,        VMSUBi,
 VADD,        VMADD,       VMUL,        VMAX,        VSUB,         VMSUB,        VOPMSUB,      VMINI,
 VIADD,       VISUB,       VIADDI,      COP2_Unknown,VIAND,        VIOR,         COP2_Unknown, COP2_Unknown,
 VCALLMS,     VCALLMSR,    COP2_Unknown,COP2_Unknown,COP2_SPECIAL2,COP2_SPECIAL2,COP2_SPECIAL2,COP2_SPECIAL2,
};

void (*Int_COP2SPECIAL2PrintTable[128])(void) =
{
 VADDAx      ,VADDAy      ,VADDAz      ,VADDAw      ,VSUBAx      ,VSUBAy      ,VSUBAz      ,VSUBAw,
 VMADDAx     ,VMADDAy     ,VMADDAz     ,VMADDAw     ,VMSUBAx     ,VMSUBAy     ,VMSUBAz     ,VMSUBAw,
 VITOF0      ,VITOF4      ,VITOF12     ,VITOF15     ,VFTOI0      ,VFTOI4      ,VFTOI12     ,VFTOI15,
 VMULAx      ,VMULAy      ,VMULAz      ,VMULAw      ,VMULAq      ,VABS        ,VMULAi      ,VCLIPw,
 VADDAq      ,VMADDAq     ,VADDAi      ,VMADDAi     ,VSUBAq      ,VMSUBAq     ,VSUBAi      ,VMSUBAi,
 VADDA       ,VMADDA      ,VMULA       ,COP2_Unknown,VSUBA       ,VMSUBA      ,VOPMULA     ,VNOP,
 VMOVE       ,VMR32       ,COP2_Unknown,COP2_Unknown,VLQI        ,VSQI        ,VLQD        ,VSQD,
 VDIV        ,VSQRT       ,VRSQRT      ,VWAITQ      ,VMTIR       ,VMFIR       ,VILWR       ,VISWR,
 VRNEXT      ,VRGET       ,VRINIT      ,VRXOR       ,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,
 COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,
 COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,
 COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,
 COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,
 COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,
 COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,
 COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,
};
