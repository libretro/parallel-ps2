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
#pragma once

#include "common/Pcsx2Defs.h"

#ifdef __cplusplus
extern "C" {
#endif

enum Syscall
{
	SetGsCrt = 2,
	ExecPS2 = 7,
	SetVTLBRefillHandler = 13,
	StartThread = 34,
	ChangeThreadPriority = 41,
	SetOsdConfigParam = 74,
	GetOsdConfigParam = 75,
	SetOsdConfigParam2 = 110,
	GetOsdConfigParam2 = 111,
	sysPrintOut = 117,
	sceSifSetDma = 119,
	Deci2Call = 124
};

extern  void (*Int_COP2PrintTable[32])(void);
extern  void (*Int_COP2BC2PrintTable[32])(void);
extern  void (*Int_COP2SPECIAL1PrintTable[64])(void);
extern  void (*Int_COP2SPECIAL2PrintTable[128])(void);

void COP2_BC2(void);
void COP2_SPECIAL(void);
void COP2_SPECIAL2(void);
void COP2_Unknown(void);

/* reserve the lower 8 bits for opcode specific types */
/* which of these are actually used depends on the opcode */
/* flags further below */
#define MEMTYPE_MASK         (0x07 << 0)
#define MEMTYPE_BYTE         (0x01 << 0)
#define MEMTYPE_HALF         (0x02 << 0)
#define MEMTYPE_WORD         (0x03 << 0)
#define MEMTYPE_DWORD        (0x04 << 0)
#define MEMTYPE_QWORD        (0x05 << 0)

#define CONDTYPE_MASK        (0x07 << 0)
#define CONDTYPE_EQ          (0x01 << 0)
#define CONDTYPE_NE          (0x02 << 0)
#define CONDTYPE_LEZ         (0x03 << 0)
#define CONDTYPE_GTZ         (0x04 << 0)
#define CONDTYPE_LTZ         (0x05 << 0)
#define CONDTYPE_GEZ         (0x06 << 0)

#define BRANCHTYPE_MASK      (0x0F << 3)
#define BRANCHTYPE_JUMP      (0x01 << 3)
#define BRANCHTYPE_BRANCH    (0x02 << 3)
#define BRANCHTYPE_SYSCALL   (0x03 << 3)
#define BRANCHTYPE_ERET      (0x04 << 3)
#define BRANCHTYPE_REGISTER  (0x05 << 3)
#define BRANCHTYPE_BC1       (0x06 << 3)
#define BRANCHTYPE_BC0       (0x08 << 3)

#define ALUTYPE_MASK         (0x07 << 3)
#define ALUTYPE_ADD          (0x01 << 3)
#define ALUTYPE_ADDI         (0x02 << 3)
#define ALUTYPE_SUB          (0x03 << 3)
#define ALUTYPE_CONDMOVE     (0x04 << 3)

#define IS_LOAD         0x00000100
#define IS_STORE        0x00000200
#define IS_BRANCH       0x00000400
#define IS_LINKED       0x00001000
#define IS_LIKELY       0x00002000
#define IS_MEMORY       0x00004000
#define IS_CONDMOVE     0x00010000
#define IS_ALU          0x00020000
#define IS_64BIT        0x00040000
#define IS_LEFT         0x00080000
#define IS_RIGHT        0x00100000

/* Recompiler handlers the tables name directly. */
void recNULL(void);
void recUnknown(void);
void recMMI_Unknown(void);
void recCOP0_Unknown(void);
void recCOP1_Unknown(void);
void recCOP2(void);
void recCACHE(void);
void recPREF(void);
void recSYSCALL(void);
void recBREAK(void);
void recSYNC(void);
void recMFSA(void);
void recMTSA(void);
void recMTSAB(void);
void recMTSAH(void);
void recTGE(void);
void recTGEU(void);
void recTLT(void);
void recTLTU(void);
void recTEQ(void);
void recTNE(void);
void recTGEI(void);
void recTGEIU(void);
void recTLTI(void);
void recTLTIU(void);
void recTEQI(void);
void recTNEI(void);

/* The opcode table entry for every EE instruction and coprocessor op.
 * getsubclass is set on the entries that index a further table. */
typedef struct OPCODE
{
	const char Name[16];
	u8 cycles;
	u32 flags;
	const struct OPCODE* (*getsubclass)(u32 op);
	void (*interpret)(void);
	void (*recompile)(void);
} OPCODE;

/* The instruction at cpuRegs.pc, and the one encoded by op. */
const OPCODE* R5900_GetCurrentInstruction(void);
const OPCODE* R5900_GetInstruction(u32 op);

extern const OPCODE tbl_Standard[64];

const OPCODE* Class_SPECIAL(u32 op);
const OPCODE* Class_REGIMM(u32 op);
const OPCODE* Class_MMI(u32 op);
const OPCODE* Class_MMI0(u32 op);
const OPCODE* Class_MMI1(u32 op);
const OPCODE* Class_MMI2(u32 op);
const OPCODE* Class_MMI3(u32 op);
const OPCODE* Class_COP0(u32 op);
const OPCODE* Class_COP0_BC0(u32 op);
const OPCODE* Class_COP0_C0(u32 op);
const OPCODE* Class_COP1(u32 op);
const OPCODE* Class_COP1_BC1(u32 op);
const OPCODE* Class_COP1_S(u32 op);
const OPCODE* Class_COP1_W(u32 op);

/* Interpreter op handlers, one per encoding: int plus the mnemonic, as
 * the recompiler's are rec plus the mnemonic. */
void intCOP2(void);
void intUnknown(void);
void intMMI_Unknown(void);
void intCOP0_Unknown(void);
void intCOP1_Unknown(void);
int intCPCOND0(void);
void intJ(void);
void intJAL(void);
void intBEQ(void);
void intBNE(void);
void intBLEZ(void);
void intBGTZ(void);
void intADDI(void);
void intADDIU(void);
void intSLTI(void);
void intSLTIU(void);
void intANDI(void);
void intORI(void);
void intXORI(void);
void intLUI(void);
void intBEQL(void);
void intBNEL(void);
void intBLEZL(void);
void intBGTZL(void);
void intDADDI(void);
void intDADDIU(void);
void intLDL(void);
void intLDR(void);
void intLB(void);
void intLH(void);
void intLWL(void);
void intLW(void);
void intLBU(void);
void intLHU(void);
void intLWR(void);
void intLWU(void);
void intSB(void);
void intSH(void);
void intSWL(void);
void intSW(void);
void intSDL(void);
void intSDR(void);
void intSWR(void);
void intCACHE(void);
void intLWC1(void);
void intPREF(void);
void intLQC2(void);
void intLD(void);
void intSQC2(void);
void intSD(void);
void intLQ(void);
void intSQ(void);
void intSWC1(void);

/* Special Opcodes */
void intSLL(void);
void intSRL(void);
void intSRA(void);
void intSLLV(void);
void intSRLV(void);
void intSRAV(void);
void intJR(void);
void intJALR(void);
void intSYSCALL(void);
void intBREAK(void);
void intSYNC(void);
void intMFHI(void);
void intMTHI(void);
void intMFLO(void);
void intMTLO(void);
void intDSLLV(void);
void intDSRLV(void);
void intDSRAV(void);
void intMULT(void);
void intMULTU(void);
void intDIV(void);
void intDIVU(void);
void intADD(void);
void intADDU(void);
void intSUB(void);
void intSUBU(void);
void intAND(void);
void intOR(void);
void intXOR(void);
void intNOR(void);
void intSLT(void);
void intSLTU(void);
void intDADD(void);
void intDADDU(void);
void intDSUB(void);
void intDSUBU(void);
void intTGE(void);
void intTGEU(void);
void intTLT(void);
void intTLTU(void);
void intTEQ(void);
void intTNE(void);
void intDSLL(void);
void intDSRL(void);
void intDSRA(void);
void intDSLL32(void);
void intDSRL32(void);
void intDSRA32(void);
void intMOVZ(void);
void intMOVN(void);
void intMFSA(void);
void intMTSA(void);

/* Regimm Opcodes */
void intBLTZ(void);
void intBGEZ(void);
void intBLTZL(void);
void intBGEZL(void);
void intTGEI(void);
void intTGEIU(void);
void intTLTI(void);
void intTLTIU(void);
void intTEQI(void);
void intTNEI(void);
void intBLTZAL(void);
void intBGEZAL(void);
void intBLTZALL(void);
void intBGEZALL(void);
void intMTSAB(void);
void intMTSAH(void);

/* Mmi Opcodes */
void intMADD(void);
void intMADDU(void);
void intMADD1(void);
void intMADDU1(void);
void intMFHI1(void);
void intMTHI1(void);
void intMFLO1(void);
void intMTLO1(void);
void intMULT1(void);
void intMULTU1(void);
void intDIV1(void);
void intDIVU1(void);
void intPLZCW(void);
void intPMFHL(void);
void intPMTHL(void);
void intPSLLH(void);
void intPSRLH(void);
void intPSRAH(void);
void intPSLLW(void);
void intPSRLW(void);
void intPSRAW(void);

/* Mmi0 Opcodes */
void intPADDW(void);
void intPSUBW(void);
void intPCGTW(void);
void intPMAXW(void);
void intPADDH(void);
void intPSUBH(void);
void intPCGTH(void);
void intPMAXH(void);
void intPADDB(void);
void intPSUBB(void);
void intPCGTB(void);
void intPADDSW(void);
void intPSUBSW(void);
void intPEXTLW(void);
void intPPACW(void);
void intPADDSH(void);
void intPSUBSH(void);
void intPEXTLH(void);
void intPPACH(void);
void intPADDSB(void);
void intPSUBSB(void);
void intPEXTLB(void);
void intPPACB(void);
void intPEXT5(void);
void intPPAC5(void);

/* Mmi1 Opcodes */
void intPABSW(void);
void intPCEQW(void);
void intPMINW(void);
void intPADSBH(void);
void intPABSH(void);
void intPCEQH(void);
void intPMINH(void);
void intPCEQB(void);
void intPADDUW(void);
void intPSUBUW(void);
void intPEXTUW(void);
void intPADDUH(void);
void intPSUBUH(void);
void intPEXTUH(void);
void intPADDUB(void);
void intPSUBUB(void);
void intPEXTUB(void);
void intQFSRV(void);

/* Mmi2 Opcodes */
void intPMADDW(void);
void intPSLLVW(void);
void intPSRLVW(void);
void intPMSUBW(void);
void intPMFHI(void);
void intPMFLO(void);
void intPINTH(void);
void intPMULTW(void);
void intPDIVW(void);
void intPCPYLD(void);
void intPMADDH(void);
void intPHMADH(void);
void intPAND(void);
void intPXOR(void);
void intPMSUBH(void);
void intPHMSBH(void);
void intPEXEH(void);
void intPREVH(void);
void intPMULTH(void);
void intPDIVBW(void);
void intPEXEW(void);
void intPROT3W(void);

/* Mmi3 Opcodes */
void intPMADDUW(void);
void intPSRAVW(void);
void intPMTHI(void);
void intPMTLO(void);
void intPINTEH(void);
void intPMULTUW(void);
void intPDIVUW(void);
void intPCPYUD(void);
void intPOR(void);
void intPNOR(void);
void intPEXCH(void);
void intPCPYH(void);
void intPEXCW(void);

/* Cop0 Opcodes */
void intMFC0(void);
void intMTC0(void);
void intBC0F(void);
void intBC0T(void);
void intBC0FL(void);
void intBC0TL(void);
void intTLBR(void);
void intTLBWI(void);
void intTLBWR(void);
void intTLBP(void);
void intERET(void);
void intDI(void);
void intEI(void);

/* Cop1 Opcodes - Floating Point Unit */
void intMFC1(void);
void intCFC1(void);
void intMTC1(void);
void intCTC1(void);
void intBC1F(void);
void intBC1T(void);
void intBC1FL(void);
void intBC1TL(void);
void intADD_S(void);
void intSUB_S(void);
void intMUL_S(void);
void intDIV_S(void);
void intSQRT_S(void);
void intABS_S(void);
void intMOV_S(void);
void intNEG_S(void);
void intRSQRT_S(void);
void intADDA_S(void);
void intSUBA_S(void);
void intMULA_S(void);
void intMADD_S(void);
void intMSUB_S(void);
void intMADDA_S(void);
void intMSUBA_S(void);
void intCVT_W(void);
void intMAX_S(void);
void intMIN_S(void);
void intC_F(void);
void intC_EQ(void);
void intC_LT(void);
void intC_LE(void);
void intCVT_S(void);

/*  */
/* Cop2 - (VU0) */
/*  */
void QMFC2(void);
void CFC2(void);
void QMTC2(void);
void CTC2(void);
void BC2F(void);
void BC2T(void);
void BC2FL(void);
void BC2TL(void);
void VCALLMS(void);
void VCALLMSR(void);
/* Special 1 VU0 Table */
void VADDx(void);
void VADDy(void);
void VADDz(void);
void VADDw(void);
void VSUBx(void);
void VSUBy(void);
void VSUBz(void);
void VSUBw(void);
void VMADDx(void);
void VMADDy(void);
void VMADDz(void);
void VMADDw(void);
void VMSUBx(void);
void VMSUBy(void);
void VMSUBz(void);
void VMSUBw(void);
void VMAXx(void);
void VMAXy(void);
void VMAXz(void);
void VMAXw(void);
void VMINIx(void);
void VMINIy(void);
void VMINIz(void);
void VMINIw(void);
void VMULx(void);
void VMULy(void);
void VMULz(void);
void VMULw(void);
void VMULq(void);
void VMAXi(void);
void VMULi(void);
void VMINIi(void);
void VADDq(void);
void VMADDq(void);
void VADDi(void);
void VMADDi(void);
void VSUBq(void);
void VMSUBq(void);
void VSUBi(void);
void VMSUBi(void);
void VADD(void);
void VMADD(void);
void VMUL(void);
void VMAX(void);
void VSUB(void);
void VMSUB(void);
void VOPMSUB(void);
void VMINI(void);
void VIADD(void);
void VISUB(void);
void VIADDI(void);
void VIAND(void);
void VIOR(void);

/* Special2 VU0 Table */
void VADDAx(void);
void VADDAy(void);
void VADDAz(void);
void VADDAw(void);
void VSUBAx(void);
void VSUBAy(void);
void VSUBAz(void);
void VSUBAw(void);
void VMADDAx(void);
void VMADDAy(void);
void VMADDAz(void);
void VMADDAw(void);
void VMSUBAx(void);
void VMSUBAy(void);
void VMSUBAz(void);
void VMSUBAw(void);
void VITOF0(void);
void VITOF4(void);
void VITOF12(void);
void VITOF15(void);
void VFTOI0(void);
void VFTOI4(void);
void VFTOI12(void);
void VFTOI15(void);
void VMULAx(void);
void VMULAy(void);
void VMULAz(void);
void VMULAw(void);
void VMULAq(void);
void VABS(void);
void VMULAi(void);
void VCLIPw(void);
void VADDAq(void);
void VMADDAq(void);
void VADDAi(void);
void VMADDAi(void);
void VSUBAq(void);
void VMSUBAq(void);
void VSUBAi(void);
void VMSUBAi(void);
void VADDA(void);
void VMADDA(void);
void VMULA(void);
void VSUBA(void);
void VMSUBA(void);
void VOPMULA(void);
void VNOP(void);
void VMOVE(void);
void VMR32(void);
void VLQI(void);
void VSQI(void);
void VLQD(void);
void VSQD(void);
void VDIV(void);
void VSQRT(void);
void VRSQRT(void);
void VWAITQ(void);
void VMTIR(void);
void VMFIR(void);
void VILWR(void);
void VISWR(void);
void VRNEXT(void);
void VRGET(void);
void VRINIT(void);
void VRXOR(void);

#ifdef __cplusplus
}
#endif
