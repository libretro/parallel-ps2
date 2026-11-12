/* c89compat.h -- the surviving C++-side pieces after the veneer cut.
 *
 * Under PCSX2_C89_EMITTER, x86emitter.h includes x86types.h, this header,
 * c89ops.h -- NOT the reference emitter and NOT the
 * shim. This header carries the handful of names converted code still
 * spells the C++ way, each either pure metadata or emitting through the
 * C89 core:
 *
 *   - xInvertCond (a table on the condition enum)
 *   - xLoadFarAddr / xComplexAddress / xWriteImm64ToMem, with bodies on
 *     the C89 macros byte-matching the reference forms
 */
#pragma once
#define PCSX2_C89_COMPAT_HELPERS

#include "common/emitter/x86types.h"
#include "common/emitter/c89ops.h"

/* Static in C: a gnu89 plain inline is an external definition, so every
 * C translation unit including this would define the symbol again. */
#ifdef __cplusplus
inline
#else
static __inline
#endif
JccComparisonType xInvertCond(JccComparisonType src)
{
	return (src == Jcc_Unconditional) ? Jcc_Unconditional : (JccComparisonType)((int)src ^ 1);
}


