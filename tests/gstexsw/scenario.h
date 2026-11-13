/* What the SW texture tests do, against the T_* operations the includer
 * defines (the C ops here, the C++ class when the expected hash was
 * made): textures of every type and format at odd sizes, updates inside,
 * across and outside them, maps with and without a rectangle, a second
 * map before the unmap; the hash takes in every answer, the pitch and
 * the texels. */

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

static unsigned char s_src[64 * 64 * 4];

static u32 run_scenario(void)
{
	u32 n, i;
	s_h    = 2166136261u;
	s_pick = 0x1234567u;
	for (i = 0; i < sizeof(s_src); i++)
		s_src[i] = (unsigned char)(i * 31 + 7);

	for (n = 0; n < 300; n++)
	{
		const unsigned type   = 1 + pick() % 4;
		const unsigned format = pick() % 12;
		const int w           = 1 + (int)(pick() % 70);
		const int h           = 1 + (int)(pick() % 40);
		T_TEX tex             = T_CREATE(type, w, h, format);
		u32 k;
		mix((u32)T_PITCH(tex));
		for (k = 0; k < 12; k++)
		{
			const int x  = (int)(pick() % 80) - 5;
			const int y  = (int)(pick() % 50) - 5;
			const int rw = (int)(pick() % 40);
			const int rh = (int)(pick() % 30);
			const int sp = 64 * 4;
			mix((u32)T_UPDATE(tex, x, y, x + rw, y + rh, s_src, sp));
		}
		for (k = 0; k < 3; k++)
		{
			const int use_rect = (int)(pick() & 1);
			const int x        = (int)(pick() % (u32)w);
			const int y        = (int)(pick() % (u32)h);
			u8* bits = NULL;
			int pitch = 0;
			const int ok = T_MAP(tex, use_rect, x, y, &bits, &pitch);
			mix((u32)ok);
			if (ok)
			{
				mix((u32)(bits - T_DATA(tex)));
				mix((u32)pitch);
				mix((u32)T_MAP(tex, 0, 0, 0, &bits, &pitch)); /* mapped already */
				T_UNMAP(tex);
			}
		}
		for (i = 0; i < (u32)(T_PITCH(tex) * h); i++)
			mix(T_DATA(tex)[i]);
		T_FREE(tex);
	}
	return s_h;
}
