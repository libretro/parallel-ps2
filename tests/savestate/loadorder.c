/*  Savestate load order audit.
 *
 *  A state is applied block by block: registers, then the subsystems, then
 *  memory, SPU2, PAD and GS. One that is rejected partway has already
 *  replaced everything before the failing block, and a reader in error
 *  zero-fills every block after it, so carrying on runs the CPUs on wiped
 *  memory. retro_unserialize therefore snapshots the running machine with
 *  state_write before it calls state_read on the frontend's data, and on a
 *  failed read puts the snapshot back with state_read.
 *
 *  A state this session wrote (the frontend says it stays in this
 *  instance, and it is this session's size) cannot be rejected partway,
 *  so state_load_needs_rollback lets it load without the copy, and
 *  only such a state keeps the compiled code its memory did not change.
 *
 *  This checks that shape in libretro/main.cpp.
 *
 *  Usage: tests/savestate/loadorder <libretro/main.cpp>
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

static const char* within(const char* s, const char* e, const char* needle)
{
	const char* p = strstr(s, needle);
	return (p && p < e) ? p : NULL;
}

int main(int argc, char** argv)
{
	char* src;
	const char *s, *e, *snap, *load, *restore;
	int bad = 0;

	if (argc != 2)
	{
		fprintf(stderr, "usage: %s <libretro/main.cpp>\n", argv[0]);
		return 2;
	}
	src = read_file(argv[1]);
	if (!src)
	{
		fprintf(stderr, "cannot read %s\n", argv[1]);
		return 2;
	}

	s = strstr(src, "bool retro_unserialize(");
	e = s ? strstr(s, "\n}\n") : NULL;
	if (!s || !e)
	{
		printf("  retro_unserialize not found -- has it moved?\nFAIL\n");
		free(src);
		return 1;
	}
	snap = within(s, e, "state_write(");
	load = within(s, e, "state_read(data");
	restore = load ? within(load + 1, e, "state_read(") : NULL;

	if (!snap || !load || snap > load)
	{
		printf("  the running machine is not snapshotted before the load\n");
		bad++;
	}
	if (!restore)
	{
		printf("  a failed load does not restore the snapshot\n");
		bad++;
	}
	/* The copy is skipped only for a state this session wrote: the
	 * frontend says it stays in this instance and it is this session's
	 * size. Anything else - including no answer - keeps it. */
	{
		const char* g = strstr(src, "static bool state_load_needs_rollback(");
		const char* ge = g ? strstr(g, "\n}\n") : NULL;
		const char* c = strstr(src, "static bool state_same_instance(");
		const char* ce = c ? strstr(c, "\n}\n") : NULL;
		if (!g || !ge || !c || !ce
		 || !within(c, ce, "RETRO_ENVIRONMENT_GET_SAVESTATE_CONTEXT")
		 || !within(c, ce, "return false;")
		 || !within(c, ce, "ctx == RETRO_SAVESTATE_CONTEXT_RUNAHEAD_SAME_INSTANCE")
		 || !within(g, ge, "!state_same_instance()")
		 || !within(g, ge, "size != retro_serialize_size()"))
		{
			printf("  the rollback copy is not kept for every state but this session's own\n");
			bad++;
		}
		if (!within(s, e, "const bool same_instance = !state_load_needs_rollback(size);")
		 || !within(s, e, "if (!same_instance)")
		 || (snap && within(s, e, "if (!same_instance)") > snap))
		{
			printf("  the snapshot is not taken whenever state_load_needs_rollback asks\n");
			bad++;
		}
		/* Compiled code is kept only through the same gate, never on the
		 * restore of the rollback copy. */
		if (!within(s, e, "state_read(data, size, same_instance)")
		 || !within(s, e, "state_read(backup, backup_size, false)"))
		{
			printf("  compiled code is kept for a state other than this session's own\n");
			bad++;
		}
	}
	/* state_read takes FreezeInternals' answer and returns on a refusal
	 * before the first memory block is read. */
	{
		const char* r = strstr(src, "static bool state_read(");
		const char* re = r ? strstr(r, "\n}\n") : NULL;
		const char* fi = r ? within(r, re, "SaveState_FreezeInternals(&loadme)") : NULL;
		const char* mem = fi ? within(fi, re, "SaveState_FreezeMem(&loadme") : NULL;
		const char* chk = fi ? within(fi, re, "if (!ok)") : NULL;
		const char* ret = chk ? within(chk, re, "return false;") : NULL;
		if (!fi || !mem || !chk || !ret || ret > mem
		 || !within(r, fi, "ok = "))
		{
			printf("  state_read does not stop at a rejected internals block\n");
			bad++;
		}
	}

	free(src);
	printf(bad ? "FAIL\n" : "PASS\n");
	return bad ? 1 : 0;
}
