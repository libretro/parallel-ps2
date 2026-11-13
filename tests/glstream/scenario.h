/* What the stream buffer tests do to a few buffers, written once against
 * the S_* operations the includer defines: GLStreamBuffer.c's C functions
 * in main.c, and the same against the C++ classes it replaced when the
 * expected trace was made. Buffers are made on every kind of driver
 * (buffer_storage core, ARB, EXT or none; NVIDIA or not; allocations that
 * fail), then mapped, written, unmapped and bound at random alignments
 * and sizes, wrapping the storage buffers many times over. */

typedef struct map_t
{
	void* pointer;
	u32 buffer_offset;
	u32 index_aligned;
	u32 space_aligned;
} map_t;

static u32 s_pick = 0x2545F491u;

static u32 pick(void)
{
	s_pick ^= s_pick << 13;
	s_pick ^= s_pick >> 17;
	s_pick ^= s_pick << 5;
	return s_pick;
}

#define NBUFS 3

static const GLenum s_targets[4] = {GL_ARRAY_BUFFER, GL_ELEMENT_ARRAY_BUFFER, GL_UNIFORM_BUFFER, GL_PIXEL_UNPACK_BUFFER};
static const u32 s_sizes[4] = {4096, 8192, 65536, 16384};
static const u32 s_aligns[6] = {1, 2, 4, 24, 64, 256};
static const char* const s_vendors[3] = {"NVIDIA Corporation", "ARM", "AMD"};

static u32 run_scenario(u32 iterations)
{
	u32 it;
	int i;
	s_pick = 0x2545F491u;
	stubs_reset();
	g_trace_sync_deletes = 1;

	for (it = 0; it < iterations; it++)
	{
		const int a  = (int)(pick() % NBUFS);
		const u32 op = pick() % 16;
		tr(0xAB000000u | (op << 8) | (u32)a);
		if (op == 0)
		{
			const u32 caps      = pick() % 4;
			const char* vendor  = s_vendors[pick() % 3];
			const int fail_st   = (pick() % 6) == 0;
			const int fail_data = (pick() % 8) == 0;
			const GLenum target = s_targets[pick() % 4];
			const u32 size      = s_sizes[pick() % 4];
			GLAD_GL_VERSION_4_4        = caps == 1;
			GLAD_GL_ARB_buffer_storage = caps == 2;
			GLAD_GL_EXT_buffer_storage = caps == 3;
			g_vendor       = vendor;
			g_fail_storage = fail_st;
			g_fail_data    = fail_data;
			g_trace_sync_deletes = 0;
			S_DESTROY(a);
			g_trace_sync_deletes = 1;
			tr((u32)S_CREATE(a, target, size));
			g_fail_storage = g_fail_data = 0;
		}
		else if (!S_VALID(a))
			tr(0xDEADu);
		else if (op <= 11)
		{
			const u32 align = s_aligns[pick() % 6];
			const u32 chunk = S_CHUNK(a);
			const u32 min_size = pick() % (chunk + 1);
			const u32 used = min_size ? pick() % (min_size + 1) : 0;
			const unsigned char fill = (unsigned char)pick();
			map_t m;
			S_MAP(a, align, min_size, &m);
			tr(m.buffer_offset);
			tr(m.index_aligned);
			tr(m.space_aligned);
			memset(m.pointer, fill, min_size);
			S_UNMAP(a, used);
		}
		else if (op <= 13)
			tr(S_CHUNK(a));
		else if (op == 14)
			S_BIND(a);
		else
			S_UNBIND(a);
	}

	g_trace_sync_deletes = 0;
	for (i = 0; i < NBUFS; i++)
		S_DESTROY(i);
	g_trace_sync_deletes = 1;
	tr(g_calls);
	return g_trace;
}
