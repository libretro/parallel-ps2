/* The sweep both sides run: every function of the format math over every
 * format, the widths, heights and pitches around block edges up to past a
 * page, every pitch alignment the renderers use, and rectangles at odd
 * offsets. F_* are the includer's: the C functions here, the C++ class
 * statics and download texture methods when the expected hash was made. */

static u32 s_h = 2166136261u;

static void mix(u32 v)
{
	int i;
	for (i = 0; i < 4; i++)
	{
		s_h ^= (v >> (i * 8)) & 0xFF;
		s_h *= 16777619u;
	}
}

static u32 sweep(void)
{
	static const u32 aligns[5] = {1, 2, 4, 64, 256};
	unsigned f;
	u32 a, i, j;
	s_h = 2166136261u;
	for (f = 0; f < 12; f++)
	{
		mix(F_BLOCK_BYTES(f));
		mix(F_BLOCK_SIZE(f));
		mix((u32)F_COMPRESSED(f));
		for (i = 0; i < 1100; i += (i < 70 ? 1 : 13))
		{
			mix(F_UPLOAD_PITCH(f, i));
			mix(F_ROW_LENGTH(f, i));
			for (j = 0; j < 40; j += 3)
				mix(F_UPLOAD_SIZE(f, j, i));
			for (a = 0; a < 5; a++)
			{
				mix(F_TRANSFER_PITCH(f, i, aligns[a]));
				mix(F_BUFFER_SIZE(f, i, i / 3 + 1, aligns[a]));
			}
		}
		for (i = 0; i < 200; i += 7)
		{
			for (j = 0; j < 200; j += 11)
			{
				u32 off, size, rows;
				F_TRANSFER(f, 4096 + j, i, j, (i * 3) % 97 + 1, (j * 5) % 61 + 1, &off, &size, &rows);
				mix(off);
				mix(size);
				mix(rows);
			}
		}
	}
	return s_h;
}
