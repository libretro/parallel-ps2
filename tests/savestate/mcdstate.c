/*  Memory card protocol state coverage.
 *
 *  A memory card access is several SIO commands long: a sector address,
 *  then reads or writes that each advance a transfer address, all against
 *  the terminator MCMAN set when it probed the card. That state lives in
 *  struct _mcd (Sio.h), one per port and slot, and for PS1 cards in
 *  PS1MemoryCardState (MemoryCardProtocol.h). A state saved between two of
 *  those commands has to carry it, or the load resumes the access at
 *  whatever address happens to be live.
 *
 *  This reads both struct declarations and checks that every member is
 *  frozen by FreezeMcds (Sio.c) or mcp_freeze_ps1 (MemoryCardProtocol.c),
 *  so a member added later without a line in the freeze shows up here.
 *  _mcd's port and slot are fixed by the array position and are exempt.
 *
 *  Usage: tests/savestate/mcdstate <Sio.h> <Sio.c>
 *                                  <MemoryCardProtocol.h> <MemoryCardProtocol.c>
 */

#include <ctype.h>
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

/* [*s, *e) of the text from start up to the first end after it. */
static int span(const char* src, const char* start, const char* end, const char** s, const char** e)
{
	*s = strstr(src, start);
	*e = *s ? strstr(*s + strlen(start), end) : NULL;
	return *s && *e;
}

/* Is "<prefix><name>" followed by a non-identifier character somewhere in
 * [s, e)? */
static int mentions(const char* s, const char* e, const char* prefix, const char* name, size_t n)
{
	size_t pl = strlen(prefix);
	const char* p = s;

	while ((p = strstr(p, prefix)) && p < e)
	{
		const char* q = p + pl;
		if (!strncmp(q, name, n) && !(isalnum((unsigned char)q[n]) || q[n] == '_'))
			return 1;
		p++;
	}
	return 0;
}

/* For each member declared in [ds, de), check it against [fs, fe). Members
 * are the last identifier before '[', '=' or ';' on a declaration line.
 * Returns the number missing; *count gets the number of members seen. */
static int check_members(const char* what, const char* ds, const char* de, const char* fs,
	const char* fe, const char* prefix, const char* const* exempt, int* count)
{
	const char* p = ds;
	int missing = 0;

	*count = 0;
	while (p < de)
	{
		const char* le = strchr(p, '\n');
		const char* stop;
		const char* q;
		const char* n_end;
		const char* n;
		int i, skip = 0;

		if (!le || le > de)
			le = de;
		/* the code part of the line */
		for (stop = p; stop < le; stop++)
			if (*stop == ';' || *stop == '=' || *stop == '[' || (stop[0] == '/' && stop[1] == '/'))
				break;
		if (stop >= le || *stop == '/')
		{
			p = le + 1;
			continue;
		}
		q = stop;
		while (q > p && isspace((unsigned char)q[-1]))
			q--;
		n_end = q;
		while (q > p && (isalnum((unsigned char)q[-1]) || q[-1] == '_'))
			q--;
		n = q;
		if (n == n_end || q == p) /* no type before the name: not a declaration */
		{
			p = le + 1;
			continue;
		}
		for (i = 0; exempt && exempt[i]; i++)
			if (strlen(exempt[i]) == (size_t)(n_end - n) && !strncmp(exempt[i], n, (size_t)(n_end - n)))
				skip = 1;
		if (!skip)
		{
			(*count)++;
			if (!mentions(fs, fe, prefix, n, (size_t)(n_end - n)))
			{
				printf("  %s: %.*s is not frozen\n", what, (int)(n_end - n), n);
				missing++;
			}
		}
		p = le + 1;
	}
	return missing;
}

int main(int argc, char** argv)
{
	static const char* const kMcdExempt[] = {"port", "slot", NULL};
	char *sio_h, *sio_c, *mcp_h, *mcp_c;
	const char *ds, *de, *fs, *fe;
	int bad = 0, n_mcd = 0, n_ps1 = 0;

	if (argc != 5)
	{
		fprintf(stderr, "usage: %s <Sio.h> <Sio.c> <MemoryCardProtocol.h> <MemoryCardProtocol.c>\n",
			argv[0]);
		return 2;
	}
	sio_h = read_file(argv[1]);
	sio_c = read_file(argv[2]);
	mcp_h = read_file(argv[3]);
	mcp_c = read_file(argv[4]);
	if (!sio_h || !sio_c || !mcp_h || !mcp_c)
	{
		fprintf(stderr, "cannot read the inputs\n");
		bad = -1;
		goto done;
	}

	if (!span(sio_h, "struct _mcd\n{", "\n}", &ds, &de) ||
	    !span(sio_c, "static bool FreezeMcds(", "\n}\n", &fs, &fe))
	{
		printf("  _mcd or FreezeMcds not found -- has it moved?\n");
		bad++;
	}
	else
		bad += check_members("_mcd", ds, de, fs, fe, "m->", kMcdExempt, &n_mcd);

	if (!span(mcp_h, "struct PS1MemoryCardState\n{", "\n}", &ds, &de) ||
	    !span(mcp_c, "bool mcp_freeze_ps1(", "\n}\n", &fs, &fe))
	{
		printf("  PS1MemoryCardState or mcp_freeze_ps1 not found -- has it moved?\n");
		bad++;
	}
	else
		bad += check_members("PS1MemoryCardState", ds, de, fs, fe, "ps1McState.", NULL, &n_ps1);

	printf("  %d _mcd and %d PS1MemoryCardState members checked\n", n_mcd, n_ps1);
	if (!bad && (!n_mcd || !n_ps1))
	{
		printf("  no members found -- has the declaration changed shape?\n");
		bad++;
	}

done:
	free(sio_h);
	free(sio_c);
	free(mcp_h);
	free(mcp_c);
	if (bad < 0)
		return 2;
	printf(bad ? "FAIL\n" : "PASS\n");
	return bad ? 1 : 0;
}
