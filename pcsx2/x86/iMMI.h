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

/* btw, nice box ya got there ! */
/*********************************************************
*   MMI opcodes                                          *
*                                                        *
*********************************************************/
#pragma once

#ifdef __cplusplus
extern "C" {
#endif


	/* These are instructions contained the MMI "opcode space" but are not */
	/* actually MMI instructions.  They are just specialized versions of standard */
	/* instructions that "fit" into the second pipeline of the EE. */

	void recMADD1(void);
	void recMADDU1(void);
	void recMADD(void);
	void recMADDU(void);

	void recMTHI1(void);
	void recMTLO1(void);
	void recMFHI1(void);
	void recMFLO1(void);
	void recMULT1(void);
	void recMULTU1(void);
	void recDIV1(void);
	void recDIVU1(void);


	void recPLZCW(void);
	void recMMI0(void);
	void recMMI1(void);
	void recMMI2(void);
	void recMMI3(void);
	void recPMFHL(void);
	void recPMTHL(void);
	void recPMAXW(void);
	void recPMINW(void);
	void recPPACW(void);
	void recPEXTLH(void);
	void recPPACH(void);
	void recPEXTLB(void);
	void recPPACB(void);
	void recPEXT5(void);
	void recPPAC5(void);
	void recPABSW(void);
	void recPADSBH(void);
	void recPABSH(void);
	void recPADDUW(void);
	void recPSUBUW(void);
	void recPSUBUH(void);
	void recPEXTUH(void);
	void recPSUBUB(void);
	void recPEXTUB(void);
	void recQFSRV(void);
	void recPMADDW(void);
	void recPSLLVW(void);
	void recPSRLVW(void);
	void recPMSUBW(void);
	void recPINTH(void);
	void recPMULTW(void);
	void recPDIVW(void);
	void recPMADDH(void);
	void recPHMADH(void);
	void recPMSUBH(void);
	void recPHMSBH(void);
	void recPEXEH(void);
	void recPREVH(void);
	void recPMULTH(void);
	void recPDIVBW(void);
	void recPEXEW(void);
	void recPROT3W(void);
	void recPMADDUW(void);
	void recPSRAVW(void);
	void recPINTEH(void);
	void recPMULTUW(void);
	void recPDIVUW(void);
	void recPEXCH(void);
	void recPEXCW(void);

	void recPSRLH(void);
	void recPSRLW(void);
	void recPSRAH(void);
	void recPSRAW(void);
	void recPSLLH(void);
	void recPSLLW(void);
	void recPMAXH(void);
	void recPCGTB(void);
	void recPCGTH(void);
	void recPCGTW(void);
	void recPADDSB(void);
	void recPADDSH(void);
	void recPADDSW(void);
	void recPSUBSB(void);
	void recPSUBSH(void);
	void recPSUBSW(void);
	void recPADDB(void);
	void recPADDH(void);
	void recPADDW(void);
	void recPSUBB(void);
	void recPSUBH(void);
	void recPSUBW(void);
	void recPEXTLW(void);
	void recPEXTUW(void);
	void recPMINH(void);
	void recPCEQB(void);
	void recPCEQH(void);
	void recPCEQW(void);
	void recPADDUB(void);
	void recPADDUH(void);
	void recPMFHI(void);
	void recPMFLO(void);
	void recPAND(void);
	void recPXOR(void);
	void recPCPYLD(void);
	void recPNOR(void);
	void recPMTHI(void);
	void recPMTLO(void);
	void recPCPYUD(void);
	void recPOR(void);
	void recPCPYH(void);

#ifdef __cplusplus
}
#endif
