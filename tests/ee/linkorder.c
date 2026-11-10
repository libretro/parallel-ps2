/*  arm64 recompiler JALR ordering audit.
 *
 *  `jalr rd, rs` may name one register for both, and then the jump goes
 *  to the old rs: the interpreters read the target before the link is
 *  written (JALR() in Interpreter.c, psxJALR in R3000AInterpreter.cpp,
 *  the latter scored against the console's "jalr: rs/rd match" case in
 *  ps2autotests tests/cpu/iop/branchdelay). Both arm64 recompilers emit
 *  the link and the target read in one shared EmitBranch, so the order of
 *  two lines there is the whole of the semantics: emit the link first and
 *  the target read sees the return address, which turns the call into a
 *  jump to the instruction after its delay slot.
 *
 *  The linking BxxZAL branches are the opposite case and are left alone:
 *  the interpreters write r31 before reading rs there, deliberately.
 *
 *  For each file given, this finds EmitBranch and checks that the JR/JALR
 *  target read (`if (is_jr) ... LoadGpr(`) comes before the link store
 *  (`if (link > 0) ... StoreGpr(`).
 *
 *  Usage: tests/ee/linkorder <recompiler.cpp>...
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

/* First line in [from, end) holding both a and b, or NULL. */
static const char* find_line(const char* from, const char* end, const char* a, const char* b)
{
	const char* p = from;

	while (p < end)
	{
		const char* e = strchr(p, '\n');
		const char* x;
		if (!e || e > end)
			e = end;
		x = strstr(p, a);
		if (x && x < e)
		{
			const char* y = strstr(x, b);
			if (y && y < e)
				return p;
		}
		p = e + 1;
	}
	return NULL;
}

static int line_of(const char* src, const char* at)
{
	int line = 1;
	for (; src < at; src++)
		if (*src == '\n')
			line++;
	return line;
}

static int check(const char* path)
{
	char* src = read_file(path);
	const char* fn;
	const char* end;
	const char* jr;
	const char* link;
	int rc = 0;

	if (!src)
	{
		printf("  %s: cannot read\n", path);
		return 1;
	}
	fn = strstr(src, "bool EmitBranch(");
	end = fn ? strstr(fn + 1, "\n\t}\n") : NULL;
	if (!fn || !end)
	{
		printf("  %s: EmitBranch not found -- has it moved?\n", path);
		free(src);
		return 1;
	}
	jr = find_line(fn, end, "if (is_jr)", "LoadGpr(");
	link = find_line(fn, end, "if (link > 0)", "StoreGpr(");
	if (!jr || !link)
	{
		printf("  %s: no %s in EmitBranch -- has it moved?\n", path, !jr ? "JR target read" : "link store");
		rc = 1;
	}
	else if (jr > link)
	{
		printf("  %s:%d: the JR/JALR target is read after the link store at line %d\n", path,
			line_of(src, jr), line_of(src, link));
		rc = 1;
	}
	else
		printf("  %s: target read (line %d) before link store (line %d)\n", path, line_of(src, jr),
			line_of(src, link));
	free(src);
	return rc;
}

int main(int argc, char** argv)
{
	int i, bad = 0;

	if (argc < 2)
	{
		fprintf(stderr, "usage: %s <recompiler.cpp>...\n", argv[0]);
		return 2;
	}
	for (i = 1; i < argc; i++)
		bad += check(argv[i]);
	if (bad)
	{
		printf("FAIL\n");
		return 1;
	}
	printf("PASS\n");
	return 0;
}
