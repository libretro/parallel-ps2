/*  arm64 recompiler register-file width audit.
 *
 *  The arm64 EE and IOP recompilers reach cpuRegs and psxRegs fields
 *  through RegsField(), and the register named in the access sets its
 *  width: a w register
 *  moves 32 bits, an x register 64. Nothing ties that choice to the
 *  field's declared type, so a 32-bit access to a u64 field compiles
 *  and runs. A narrow store drops the carry out of the low word -- the
 *  EE clock is a u64, and a block tail that adds its cycles through a
 *  w register sends the clock back 2^32 cycles at the crossing, after
 *  which every timer derives a count from a start cycle in the future.
 *  A wide store to a u32 field overwrites the field after it.
 *
 *  This reads the field types out of a register-file struct declaration
 *  and checks every a64_ldr/a64_str through RegsField(&<var>.<field>) in a
 *  recompiler against them. Only top-level fields are checked; accesses
 *  into nested members (CP0.n.Status, GPR.r[n]) are skipped.
 *
 *  Usage: tests/ee/cyclewidth <header> <struct> <var> <recompiler.cpp>
 *    e.g. R5900.h cpuRegisters cpuRegs recR5900_arm64.c
 *         R3000A.h psxRegisters psxRegs recR3000A_arm64.c
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FIELDS 64
#define NAME_LEN   64

struct field
{
	char name[NAME_LEN];
	int  bits;
};

static struct field fields[MAX_FIELDS];
static int nfields;

static char* read_file(const char* path)
{
	FILE* f = fopen(path, "rb");
	long n;
	char* buf;

	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	buf = (char*)malloc((size_t)n + 1);
	if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n)
	{
		free(buf);
		buf = NULL;
	}
	if (buf)
		buf[n] = '\0';
	fclose(f);
	return buf;
}

static int type_bits(const char* t, size_t n)
{
	if ((n == 3 && !strncmp(t, "u64", 3)) || (n == 3 && !strncmp(t, "s64", 3)))
		return 64;
	if ((n == 3 && !strncmp(t, "u32", 3)) || (n == 3 && !strncmp(t, "s32", 3)) ||
	    (n == 3 && !strncmp(t, "int", 3)))
		return 32;
	return 0;
}

/* Collect "type name[, name...];" declarations of the scalar widths we
 * know, from the body of the named struct. Lines with any other leading
 * type (GPRregs, CP0regs, ...) are nested members and skipped. */
static int parse_fields(const char* hdr, const char* tag)
{
	char open[96], close[96];
	const char* start;
	const char* end;
	const char* p;

	if (strlen(tag) > 64)
		return 0;
	sprintf(open, "typedef struct %s", tag);
	sprintf(close, "} %s;", tag);
	start = strstr(hdr, open);
	if (!start)
		return 0;
	end = strstr(start, close);
	if (!end)
		return 0;
	p = strchr(start, '{');
	if (!p || p > end)
		return 0;
	p++;

	while (p < end)
	{
		const char* line_end = strchr(p, '\n');
		const char* q = p;
		const char* t;
		int bits;

		if (!line_end || line_end > end)
			line_end = end;
		while (q < line_end && isspace((unsigned char)*q))
			q++;
		t = q;
		while (q < line_end && (isalnum((unsigned char)*q) || *q == '_'))
			q++;
		bits = type_bits(t, (size_t)(q - t));
		if (bits)
		{
			/* names up to the ';', stopping at a comment */
			while (q < line_end && *q != ';' && !(q[0] == '/' && q[1] == '/'))
			{
				const char* n;
				while (q < line_end && !(isalpha((unsigned char)*q) || *q == '_'))
				{
					if (*q == ';' || (q[0] == '/' && q[1] == '/'))
						break;
					if (*q == '[')
						while (q < line_end && *q != ']')
							q++;
					q++;
				}
				if (q >= line_end || *q == ';' || (q[0] == '/' && q[1] == '/'))
					break;
				n = q;
				while (q < line_end && (isalnum((unsigned char)*q) || *q == '_'))
					q++;
				if (nfields < MAX_FIELDS && (size_t)(q - n) < NAME_LEN)
				{
					memcpy(fields[nfields].name, n, (size_t)(q - n));
					fields[nfields].name[q - n] = '\0';
					fields[nfields].bits = bits;
					nfields++;
				}
			}
		}
		p = line_end + 1;
	}
	return nfields;
}

static int field_bits(const char* name, size_t n)
{
	int i;
	for (i = 0; i < nfields; i++)
		if (strlen(fields[i].name) == n && !strncmp(fields[i].name, name, n))
			return fields[i].bits;
	return 0;
}

int main(int argc, char** argv)
{
	char needle[96];
	size_t needle_len;
	const char* tag;
	const char* var;
	char* hdr;
	char* src;
	const char* p;
	int checked = 0, bad = 0, wide_fields = 0, i;

	if (argc != 5 || strlen(argv[3]) > 64)
	{
		fprintf(stderr, "usage: %s <header> <struct> <var> <recompiler.cpp>\n", argv[0]);
		return 2;
	}
	tag = argv[2];
	var = argv[3];
	sprintf(needle, "RegsField(&%s.", var);
	needle_len = strlen(needle);
	hdr = read_file(argv[1]);
	src = read_file(argv[4]);
	if (!hdr || !src)
	{
		fprintf(stderr, "cannot read %s\n", !hdr ? argv[1] : argv[4]);
		free(hdr);
		free(src);
		return 2;
	}
	if (!parse_fields(hdr, tag))
	{
		fprintf(stderr, "no %s fields found in %s\n", tag, argv[1]);
		free(hdr);
		free(src);
		return 2;
	}
	for (i = 0; i < nfields; i++)
		if (fields[i].bits == 64)
			wide_fields++;

	for (p = strstr(src, needle); p; p = strstr(p + 1, needle))
	{
		const char* name = p + needle_len;
		const char* ne = name;
		const char* ls = p;
		const char* op;
		const char* r;
		int bits, line = 1;
		const char* c;

		while (isalnum((unsigned char)*ne) || *ne == '_')
			ne++;
		if (*ne != ')') /* nested member: CP0.n.Status, GPR.r[n] */
			continue;
		bits = field_bits(name, (size_t)(ne - name));
		if (!bits)
			continue;

		while (ls > src && ls[-1] != '\n')
			ls--;
		/* in a comment: not code */
		for (c = ls; c + 1 < p; c++)
			if (c[0] == '/' && (c[1] == '/' || c[1] == '*'))
				break;
		if (c + 1 < p)
			continue;
		/* the nearest load or store opening before the operand; its
		 * register follows the emitter argument */
		op = NULL;
		for (c = p; c - ls >= 8 && !op; c--)
			if (!strncmp(c - 8, "a64_ldr(", 8) || !strncmp(c - 8, "a64_str(", 8))
				op = c - 8;
		if (!op)
			continue; /* not a load or store: an address computation */
		r = op + 8;
		while (r < p && *r != ',')
			r++;
		if (r < p)
			r++;
		while (*r == ' ')
			r++;
		/* A64_W(n)/A64_X(n), or a register variable named w.../x... */
		if (!strncmp(r, "A64_W(", 6))
			r = "w";
		else if (!strncmp(r, "A64_X(", 6))
			r = "x";
		for (c = src; c < p; c++)
			if (*c == '\n')
				line++;
		checked++;

		if ((*r == 'w' && bits == 64) || (*r == 'x' && bits == 32))
		{
			printf("  line %d: %c-register access to %d-bit %s.%.*s\n", line, *r, bits, var,
				(int)(ne - name), name);
			bad++;
		}
	}

	printf("  %d %s fields (%d of them 64-bit), %d accesses checked\n", nfields, var, wide_fields,
		checked);
	free(hdr);
	free(src);
	if (!checked)
	{
		printf("FAIL: nothing to check -- has the declaration or the access helper moved?\n");
		return 1;
	}
	if (bad)
	{
		printf("FAIL: %d access(es) of the wrong width\n", bad);
		return 1;
	}
	printf("PASS\n");
	return 0;
}
