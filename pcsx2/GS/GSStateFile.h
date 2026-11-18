/* SPDX-License-Identifier: LGPL-3.0+ */
#ifndef GS_STATE_FILE_H
#define GS_STATE_FILE_H

/* The GS's part of a savestate: its registers, the vertex being built,
 * the transfer's position, local memory, the GIF paths and Q, in the
 * layout states of version GS_STATE_VERSION and before have. GSState
 * flushes, resets and derives what follows from them around these
 * (Freeze, Defrost). */

#include "common/Pcsx2Defs.h"
#include "GSRegs.h"
#include "GSDrawingContext.h"
#include "GSTransfer.h"
#include "Renderers/Common/gs_vertex.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GS_STATE_VERSION 8

/* The registers at the start of GSDrawingEnvironment */
typedef struct gs_env_regs
{
	GIFRegPRIM       PRIM;
	GIFRegPRMODE     PRMODE;
	GIFRegPRMODECONT PRMODECONT;
	GIFRegTEXCLUT    TEXCLUT;
	GIFRegSCANMSK    SCANMSK;
	GIFRegTEXA       TEXA;
	GIFRegFOGCOL     FOGCOL;
	GIFRegDIMX       DIMX;
	GIFRegDTHE       DTHE;
	GIFRegCOLCLAMP   COLCLAMP;
	GIFRegPABE       PABE;
	GIFRegBITBLTBUF  BITBLTBUF;
	GIFRegTRXDIR     TRXDIR;
	GIFRegTRXPOS     TRXPOS;
	GIFRegTRXREG     TRXREG;
} gs_env_regs;

/* Where each part lives */
typedef struct gs_state_parts
{
	gs_env_regs* env;
	gs_drawing_regs* ctx[2];
	union gs_vertex* v;
	gs_transfer* tr;
	u8* vm;          /* GS_LOCAL_MEMORY_SIZE bytes */
	GIFPath* path;   /* four */
	float* q;
} gs_state_parts;

u32 gs_state_size(void);
/* The state into data (gs_state_size bytes). The paths' tags take their
 * loops and registers back first, as a saved tag holds them. */
void gs_state_write(u8* data, const gs_state_parts* s);
/* A state's version: newer than GS_STATE_VERSION it cannot be read */
u32 gs_state_version(const u8* data);
/* The state read back, its version's layout; local memory a page at a
 * time, the pages that changed set in changed (GS_MAX_PAGES bits) */
void gs_state_read(const u8* data, const gs_state_parts* s, u32* changed);
/* What the register handlers would have made of the registers a state
 * brought back raw; scanmask_used latched as the SCANMSK handler does */
void gs_state_normalize(const gs_state_parts* s, u8* scanmask_used);

#ifdef __cplusplus
}
#endif

#endif
