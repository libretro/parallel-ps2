/* The header is C: this unit includes it under -std=c89 -pedantic and
 * gives main.cpp a C entry point into it, so the C build is the one
 * checked as well as the C++ one. */
#include "ParseNumber.h"

int parsenumber_c89_u32(const char* s, size_t n, int base, uint32_t* out)
{
	return parse_number_u32(s, n, base, out);
}

int parsenumber_c89_s32(const char* s, size_t n, int base, int32_t* out)
{
	return parse_number_s32(s, n, base, out);
}
