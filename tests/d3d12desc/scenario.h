/* What the descriptor heap tests do, against the operations the includer
 * defines (the C functions, or the C++ classes when the expected hash
 * was made): heaps taken and given back one at a time, a run allocator
 * filled and reset, and sampler groups looked up, forgotten and reset.
 * The hash takes every answer and every handle; the device's calls go in
 * from the includer's fake. The per-descriptor heaps are never asked for
 * more than they hold here: the C++ class handed out descriptors past the
 * heap's end then, which the includer checks apart. */

static u32 s_h;
static u32 s_pick;

static void mix(u32 v)
{
	int i;
	for (i = 0; i < 4; i++)
	{
		s_h ^= (v >> (i * 8)) & 0xFF;
		s_h *= 16777619u;
	}
}

static u32 pick(void)
{
	s_pick ^= s_pick << 13;
	s_pick ^= s_pick >> 17;
	s_pick ^= s_pick << 5;
	return s_pick;
}

static void mix_handle(const HANDLE* h)
{
	mix(H_INDEX(h));
	mix((u32)H_CPU(h));
	mix((u32)(H_CPU(h) >> 32));
	mix((u32)H_GPU(h));
	mix((u32)(H_GPU(h) >> 32));
}

#define SC_LIVE 2048

/* one manager: num descriptors, allocations and frees at random */
static void sc_manager(MANAGER* m, u32 type, u32 num, int visible, u32 rounds)
{
	static HANDLE live[SC_LIVE];
	u32 nlive = 0, r;

	mix((u32)M_CREATE(m, type, num, visible));
	for (r = 0; r < rounds; r++)
	{
		const u32 op = pick() % 8;
		if (op < 4 && nlive < num && nlive < SC_LIVE)
		{
			const int ok = M_ALLOC(m, &live[nlive]);
			mix(0xA0u | (u32)ok);
			mix_handle(&live[nlive]);
			if (ok)
				nlive++;
		}
		else if (op < 7 && nlive)
		{
			const u32 i = pick() % nlive;
			if (pick() & 1)
			{
				M_FREE(m, &live[i]);
				mix_handle(&live[i]);
				/* (a cleared handle freed again is nothing) */
				M_FREE(m, &live[i]);
			}
			else
				M_FREE_INDEX(m, H_INDEX(&live[i]));
			live[i] = live[--nlive];
		}
	}
	M_DESTROY(m);
}

static void sc_allocator(ALLOCATOR* a, u32 num, u32 rounds)
{
	u32 r;
	mix((u32)A_CREATE(a, 0, num));
	for (r = 0; r < rounds; r++)
	{
		const u32 op = pick() % 16;
		if (op == 0)
			A_RESET(a);
		else
		{
			HANDLE h;
			const int ok = A_ALLOC(a, 1 + pick() % 40, &h);
			mix(0xB0u | (u32)ok);
			if (ok)
				mix_handle(&h);
		}
	}
	A_DESTROY(a);
}

static void sc_samplers(SAMPLERS* s, MANAGER* m, u32 num, u32 rounds)
{
	static HANDLE cpu[64];
	u32 r, i;

	mix((u32)M_CREATE(m, 1, 64, 0));
	for (i = 0; i < 64; i++)
		mix((u32)M_ALLOC(m, &cpu[i]));
	mix((u32)S_CREATE(s, num));
	for (r = 0; r < rounds; r++)
	{
		const u32 op = pick() % 64;
		HANDLE gpu;
		if (op == 0)
			S_INVALIDATE(s);
		else if (op == 1 || (op < 4 && S_SHOULD_RESET(s)))
		{
			mix(0x5E7u | ((u32)S_SHOULD_RESET(s) << 12));
			S_RESET(s);
		}
		else if (op < 34)
		{
			const int ok = S_SINGLE(s, &gpu, &cpu[pick() % 64]);
			mix(0xC0u | (u32)ok);
			if (ok)
				mix_handle(&gpu);
		}
		else
		{
			HANDLE pair[2];
			int ok;
			pair[0] = cpu[pick() % 64];
			pair[1] = cpu[pick() % 64];
			ok = S_GROUP(s, &gpu, pair);
			mix(0xD0u | (u32)ok);
			if (ok)
				mix_handle(&gpu);
		}
	}
	S_DESTROY(s);
	M_DESTROY(m);
}

static u32 run_scenario(u32 seed)
{
	s_h    = 2166136261u;
	s_pick = 0x9E3779B9u ^ seed;
	/* an RTV heap that is no whole number of 32s or of 1024s, a large
	 * shader-visible one, a run allocator, and sampler groups in a heap
	 * smaller than and as large as the device's */
	sc_manager(&s_m0, 2, 37, 0, 4000);
	sc_manager(&s_m1, 0, 3000, 1, 40000);
	sc_allocator(&s_a, 500, 20000);
	sc_samplers(&s_s, &s_m2, 300, 20000);
	/* (another: the class kept its groups through a destroy, the C
	 * forgets them, checked apart) */
	sc_samplers(&s_s2, &s_m2, 2048, 60000);
	return s_h;
}
