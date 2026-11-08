/*  Memory card terminator audit.
 *
 *  MCMAN tracks a per-card terminator byte. It sets one with command 0x27
 *  and reads it back with 0x28; newer revisions set 0x5a and check the
 *  reply byte for it, so:
 *
 *    SetTerminator  stores the new terminator and echoes that same value;
 *    GetTerminator  answers 0x2b followed by the card's terminator twice;
 *    AuthF3         (authentication reset) returns the card to the
 *                   default terminator before replying.
 *
 *  This is upstream PCSX2 c3bafa2a4 less its eject signalling, which this
 *  core does with auto-eject ticks instead. The three handlers are a few
 *  lines each in MemoryCardProtocol.c; this reads them and checks that
 *  shape: in SetTerminator the store precedes a final push of mcd->term,
 *  GetTerminator pushes mcd->term twice, and AuthF3 assigns mcd->term
 *  before the_2b_terminator.
 *
 *  Usage: tests/memcard/termaudit <MemoryCardProtocol.c>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* Body of `void mcp_<name>(void)` as [*s, *e), 0 if absent. */
static int body(const char* src, const char* name, const char** s, const char** e)
{
	char sig[96];

	sprintf(sig, "void mcp_%s(void)", name);
	*s = strstr(src, sig);
	*e = *s ? strstr(*s, "\n}\n") : NULL;
	return *s && *e;
}

/* Last occurrence of needle in [s, e), or NULL. */
static const char* last_in(const char* s, const char* e, const char* needle)
{
	const char* last = NULL;
	const char* p = s;

	while ((p = strstr(p, needle)) && p < e)
	{
		last = p;
		p++;
	}
	return last;
}

static int count_in(const char* s, const char* e, const char* needle)
{
	int n = 0;
	const char* p = s;

	while ((p = strstr(p, needle)) && p < e)
	{
		n++;
		p++;
	}
	return n;
}

static int verdict(const char* what, int ok)
{
	printf("  %-14s %s\n", what, ok ? "ok" : "WRONG");
	return ok ? 0 : 1;
}

int main(int argc, char** argv)
{
	char* src;
	const char *s, *e, *store, *push;
	int bad = 0;

	if (argc != 2)
	{
		fprintf(stderr, "usage: %s <MemoryCardProtocol.c>\n", argv[0]);
		return 2;
	}
	src = read_file(argv[1]);
	if (!src)
	{
		fprintf(stderr, "cannot read %s\n", argv[1]);
		return 2;
	}

	if (!body(src, "set_terminator", &s, &e))
		bad += verdict("SetTerminator", 0);
	else
	{
		store = strstr(s, "mcd->term =");
		push = last_in(s, e, "OUT(");
		bad += verdict("SetTerminator", store && store < e && push && store < push &&
		                                    !strncmp(push, "OUT(mcd->term);", 15));
	}

	if (!body(src, "get_terminator", &s, &e))
		bad += verdict("GetTerminator", 0);
	else
		bad += verdict("GetTerminator", count_in(s, e, "OUT(mcd->term);") == 2);

	if (!body(src, "auth_f3", &s, &e))
		bad += verdict("AuthF3", 0);
	else
	{
		store = strstr(s, "mcd->term =");
		push = strstr(s, "the_2b_terminator(");
		bad += verdict("AuthF3", store && push && store < push && push < e);
	}

	free(src);
	if (bad)
	{
		printf("FAIL\n");
		return 1;
	}
	printf("PASS\n");
	return 0;
}
