/* What the shader cache tests do, written once against the S_* operations
 * the includer defines: GLShaderCache.c's C functions in main.c, and the
 * same against the C++ class it replaced when the expected trace was
 * made. Programs are asked for over and over from a set of sources, the
 * cache is closed and opened again, and between runs its files are
 * damaged as a crash or another version would leave them: a torn last
 * entry, another version, an entry past the blob's end, no blob file.
 * The trace takes in every answer, the GL calls (the stubs' trace) and,
 * at the end, the bytes of both files. */

#include <stdio.h>
#include <string.h>

static u32 s_pick = 0x51ED270Bu;

static u32 pick(void)
{
	s_pick ^= s_pick << 13;
	s_pick ^= s_pick >> 17;
	s_pick ^= s_pick << 5;
	return s_pick;
}

static char s_index_path[1024];
static char s_blob_path[1024];

static void trace_file(const char* path)
{
	FILE* f = fopen(path, "rb");
	int ch;
	u32 n = 0;
	if (!f)
	{
		tr(0xF11E0000u);
		return;
	}
	while ((ch = fgetc(f)) != EOF)
	{
		tr((u32)ch);
		n++;
	}
	fclose(f);
	tr(n);
}

static void damage(u32 how)
{
	FILE* f;
	switch (how)
	{
		case 0: /* a torn last entry */
			if ((f = fopen(s_index_path, "ab")))
			{
				fwrite("torn!!!", 1, 7, f);
				fclose(f);
			}
			break;
		case 1: /* another version */
			if ((f = fopen(s_index_path, "r+b")))
			{
				const u32 v = 57u;
				fwrite(&v, 4, 1, f);
				fclose(f);
			}
			break;
		case 2: /* the first entry's offset past the blob */
			if ((f = fopen(s_index_path, "r+b")))
			{
				const u32 off = 0x7FFFFF00u; /* (past the blob without wrapping) */
				if (fseek(f, 4 + 40, SEEK_SET) == 0)
					fwrite(&off, 4, 1, f);
				fclose(f);
			}
			break;
		default: /* no blob */
			remove(s_blob_path);
			break;
	}
}

static const char s_text[] = "#version 330\nvoid main() { /* the program's own text, cut to length */ }\n"
	"uniform vec4 a; uniform vec4 b; uniform vec4 c; uniform vec4 d; uniform vec4 e;";

#define NPROGS 4

static u32 run_scenario(const char* dir, u32 iterations)
{
	u32 it;
	int i;
	sprintf(s_index_path, "%s/gl_programs.idx", dir);
	sprintf(s_blob_path, "%s/gl_programs.bin", dir);
	remove(s_index_path);
	remove(s_blob_path);
	s_pick = 0x51ED270Bu;
	stubs_reset();

	S_INIT();
	tr((u32)S_OPEN(0, dir));
	for (it = 0; it < iterations; it++)
	{
		const u32 op = pick() % 40;
		tr(0xAB000000u | op);
		if (op == 0)
		{
			const int gles = (int)(pick() & 1);
			S_CLOSE();
			tr((u32)S_OPEN(gles, dir));
		}
		else if (op == 1)
		{
			const u32 how = pick() % 4;
			S_CLOSE();
			damage(how);
			tr((u32)S_OPEN(0, dir));
		}
		else
		{
			const int p     = (int)(pick() % NPROGS);
			const u32 v     = pick() % 16;
			const size_t vl = (v % 5) ? 20 + v * 7 : 0;
			const size_t fl = 30 + (pick() % 16) * 5;
			S_DESTROY(p);
			tr((u32)S_GET(p, s_text, vl, s_text + 1, fl));
			tr((u32)S_PROGRAM_ID(p));
		}
	}
	S_CLOSE();
	for (i = 0; i < NPROGS; i++)
		S_DESTROY(i);
	trace_file(s_index_path);
	trace_file(s_blob_path);
	S_FREE();
	tr(g_calls);
	return g_trace;
}
