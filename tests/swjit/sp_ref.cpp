/* The xbyak setup generator as the core built it (ref/), for one tier:
 * compiled once per tier with that tier's flags, as the Makefile does. */
#include "ref/GSSetupPrimCodeGenerator.h"
#include "GS/Renderers/SW/GSSwJitLayout.h"

#define SWJ_CAT2(a, b) a##b
#define SWJ_CAT(a, b) SWJ_CAT2(a, b)

extern "C" size_t SWJ_CAT(ref_setup_prim_, MULTI_ISA_UNSHARED_COMPILATION)(u64 key, void* code, size_t room)
{
	MULTI_ISA_UNSHARED_COMPILATION::GSSetupPrimCodeGenerator cg(key, code, room);
	return cg.getSize();
}
