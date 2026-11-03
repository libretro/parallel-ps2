/* parse_number_* against std::from_chars, which the patch, game database
 * and core-option readers used before: for every run of text, both agree
 * on whether there is a number and, when there is, on its value. Runs are
 * fixed edge cases (range limits, signs, partial runs, bases) plus
 * generated ones drawn from digits, letters, '-', '+', ' ' and 'x'. The
 * C89 build of the header (c89.c) is held to the same answers. */
#include <charconv>
#include <cstdio>
#include <cstring>
#include <string>
#include "ParseNumber.h"

extern "C" int parsenumber_c89_u32(const char* s, size_t n, int base, uint32_t* out);
extern "C" int parsenumber_c89_s32(const char* s, size_t n, int base, int32_t* out);

static int s_fails;
static long s_checks;

template <typename T>
static bool reference(const std::string& s, int base, T* out)
{
	T v{};
	const std::from_chars_result r = std::from_chars(s.data(), s.data() + s.size(), v, base);
	if (r.ec != std::errc())
		return false;
	*out = v;
	return true;
}

template <typename T, typename F>
static void check(const char* what, F fn, const std::string& s, int base)
{
	T want = 0, got = 0;
	const bool ok_want = reference<T>(s, base, &want);
	const bool ok_got  = fn(s.data(), s.size(), base, &got) != 0;
	s_checks++;
	if (ok_want != ok_got || (ok_want && want != got))
	{
		if (s_fails++ < 20)
			printf("  FAIL %s base %d \"%s\": from_chars %s %lld, parse_number %s %lld\n",
				what, base, s.c_str(), ok_want ? "ok" : "none", (long long)want,
				ok_got ? "ok" : "none", (long long)got);
	}
}

static void check_all(const std::string& s, int base)
{
	check<uint64_t>("u64", parse_number_u64, s, base);
	check<uint32_t>("u32", parse_number_u32, s, base);
	check<uint8_t>("u8", parse_number_u8, s, base);
	check<int32_t>("s32", parse_number_s32, s, base);
	check<uint32_t>("c89 u32", parsenumber_c89_u32, s, base);
	check<int32_t>("c89 s32", parsenumber_c89_s32, s, base);
}

int main()
{
	static const char* fixed[] = {
		"", "0", "1", "-", "-0", "+1", " 1", "1 ", "12ab", "ab12", "0x10", "00000000000000000000001",
		"255", "256", "-1", "4294967295", "4294967296", "2147483647", "2147483648",
		"-2147483648", "-2147483649", "18446744073709551615", "18446744073709551616",
		"99999999999999999999999", "ff", "FF", "fF", "1g", "zz", "ZZ", "--1", "-+1",
		"FFFFFFFF", "100000000", "7fffffff", "80000000", "-80000000", "-80000001",
		"ffffffffffffffff", "10000000000000000", "002b8a5f", "Default",
	};
	static const int bases[] = {2, 8, 10, 16, 36};
	for (const char* f : fixed)
		for (int b : bases)
			check_all(f, b);

	/* Generated runs, from a fixed seed so a failure reproduces. */
	static const char alphabet[] = "0123456789abcdefzABCDEFZ-+ x";
	uint32_t seed = 0x2545F491u;
	for (int i = 0; i < 200000; i++)
	{
		std::string s;
		seed = seed * 1664525u + 1013904223u;
		const int len = (int)(seed >> 27) % 24;
		for (int j = 0; j < len; j++)
		{
			seed = seed * 1664525u + 1013904223u;
			/* mostly digits, so long in-range and overflowing values are common */
			const uint32_t r = seed >> 24;
			s += (r < 200) ? alphabet[r % 10] : alphabet[r % (sizeof(alphabet) - 1)];
		}
		seed = seed * 1664525u + 1013904223u;
		check_all(s, bases[(seed >> 28) % 5]);
	}

	/* Floats: none where from_chars-era code had none, the value otherwise. */
	{
		float f = 0;
		s_checks += 4;
		if (!parse_number_float("1.5", 3, &f) || f != 1.5f) { printf("  FAIL float 1.5\n"); s_fails++; }
		if (!parse_number_float("-0.25x", 6, &f) || f != -0.25f) { printf("  FAIL float -0.25x\n"); s_fails++; }
		f = 7;
		if (parse_number_float("abc", 3, &f) || f != 7) { printf("  FAIL float abc: should be none, unchanged\n"); s_fails++; }
		if (parse_number_float("", 0, &f) || f != 7) { printf("  FAIL float empty\n"); s_fails++; }
	}

	/* Booleans: the words and their leading parts, as before; else none. */
	{
		static const struct { const char* s; int ok; bool v; } cases[] = {
			{"true", 1, true}, {"TRUE", 1, true}, {"yes", 1, true}, {"on", 1, true}, {"1", 1, true},
			{"enabled", 1, true}, {"t", 1, true}, {"false", 1, false}, {"no", 1, false}, {"off", 1, false},
			{"0", 1, false}, {"disabled", 1, false}, {"f", 1, false}, {"maybe", 0, false}, {"2", 0, false},
			{"truex", 0, false},
		};
		for (const auto& c : cases)
		{
			bool v = !c.v;
			const int ok = parse_number_bool(c.s, strlen(c.s), &v);
			s_checks++;
			if (ok != c.ok || (ok && v != c.v))
			{
				printf("  FAIL bool \"%s\"\n", c.s);
				s_fails++;
			}
		}
	}

	printf("parsenumber: %ld checks, %d failures\n", s_checks, s_fails);
	return s_fails ? 1 : 0;
}
