/* Parse a number out of a run of text (pointer and length): zero when
 * the run does not start with one. As std::from_chars did: no leading
 * whitespace or '+', a '-' only for the signed forms, no base prefix,
 * letters for digits past 9 in either case, and the parse stops at the
 * first character that is not a digit - "12ab" in base 10 is 12. A
 * value out of the type's range is no value. Used by the core-option
 * parsing and the patch and game database readers. C89. */

#ifndef PCSX2_PARSENUMBER_H
#define PCSX2_PARSENUMBER_H

#include <stddef.h>
#include <string.h>
#include <stdint.h>
#include <boolean.h>
#include <retro_inline.h>
#include <string/rstrtod.h>   /* rstrtof_len */

/* Case-insensitive compare of a bounded run, by the platform's name. */
#ifndef Strncasecmp
#ifdef _MSC_VER
#define Strncasecmp(s1, s2, n) _strnicmp(s1, s2, n)
#else
#include <strings.h>
#define Strncasecmp(s1, s2, n) strncasecmp(s1, s2, n)
#endif
#endif

/* The digits at the start of s[0..n) in base, into *out; zero when there
 * are none or the value passes max. */
static INLINE int parse_number_digits(const char* s, size_t n, int base, uint64_t max, uint64_t* out)
{
	uint64_t value = 0;
	size_t i  = 0;
	for (; i < n; i++)
	{
		const char c = s[i];
		int digit;
		if (c >= '0' && c <= '9')
			digit = c - '0';
		else if (c >= 'a' && c <= 'z')
			digit = c - 'a' + 10;
		else if (c >= 'A' && c <= 'Z')
			digit = c - 'A' + 10;
		else
			break;
		if (digit >= base)
			break;
		if (value > (max - (uint64_t)digit) / (uint64_t)base)
			return 0;
		value = value * (uint64_t)base + (uint64_t)digit;
	}
	if (i == 0)
		return 0;
	*out = value;
	return 1;
}

static INLINE int parse_number_u64(const char* s, size_t n, int base, uint64_t* out)
{
	return parse_number_digits(s, n, base, ~(uint64_t)0, out);
}

static INLINE int parse_number_u32(const char* s, size_t n, int base, uint32_t* out)
{
	uint64_t v;
	if (!parse_number_digits(s, n, base, 0xFFFFFFFFu, &v))
		return 0;
	*out = (uint32_t)v;
	return 1;
}

static INLINE int parse_number_u8(const char* s, size_t n, int base, uint8_t* out)
{
	uint64_t v;
	if (!parse_number_digits(s, n, base, 0xFFu, &v))
		return 0;
	*out = (uint8_t)v;
	return 1;
}

static INLINE int parse_number_s32(const char* s, size_t n, int base, int32_t* out)
{
	uint64_t v;
	if (n && s[0] == '-')
	{
		if (!parse_number_digits(s + 1, n - 1, base, 0x80000000u, &v))
			return 0;
		*out = (int32_t)(0 - (int64_t)v);
		return 1;
	}
	if (!parse_number_digits(s, n, base, 0x7FFFFFFFu, &v))
		return 0;
	*out = (int32_t)v;
	return 1;
}

/* A float at the start of the run; zero when there is none. */
static INLINE int parse_number_float(const char* s, size_t n, float* out)
{
	size_t used = 0;
	const float value = rstrtof_len(s, n, &used);
	if (used == 0)
		return 0;
	*out = value;
	return 1;
}

/* "true"/"yes"/"on"/"1"/"enabled" and their negatives, matched as the
 * leading part of each word, as before; nothing else. */
static INLINE int parse_number_bool(const char* s, size_t n, bool* out)
{
	if (Strncasecmp("true", s, n) == 0 || Strncasecmp("yes", s, n) == 0 ||
		Strncasecmp("on", s, n) == 0 || Strncasecmp("1", s, n) == 0 ||
		Strncasecmp("enabled", s, n) == 0)
	{
		*out = true;
		return 1;
	}
	if (Strncasecmp("false", s, n) == 0 || Strncasecmp("no", s, n) == 0 ||
		Strncasecmp("off", s, n) == 0 || Strncasecmp("0", s, n) == 0 ||
		Strncasecmp("disabled", s, n) == 0)
	{
		*out = false;
		return 1;
	}
	return 0;
}

#endif
