/* What the program tests do to a handful of programs, written once against
 * the P_* operations the includer defines: GLProgram.c's C functions in
 * main.c, and the same against the C++ class it replaced when the
 * expected trace was made. The scenario's own choices come from its own
 * generator, the driver's answers from the stubs'. */

static u32 s_pick = 0x9E3779B9u;

static u32 pick(void)
{
	s_pick ^= s_pick << 13;
	s_pick ^= s_pick >> 17;
	s_pick ^= s_pick << 5;
	return s_pick;
}

static const char s_source[] = "void main() { gl_Position = vec4(0.0); } /* filler for lengths */";
static const char* const s_names[9] = {
	"u_source_rect", "u_target_rect", "u_source_size", "u_target_size", "u_target_resolution",
	"u_rcp_target_resolution", "u_source_resolution", "u_rcp_source_resolution", "u_time"};

#define NPROGS 4

static size_t source_length(void)
{
	return (pick() % 3) ? (size_t)(5 + pick() % 40) : 0;
}

static u32 run_scenario(u32 iterations)
{
	int counts[NPROGS] = {0, 0, 0, 0};
	unsigned char blob[100];
	gs_array_t bin;
	u32 it;
	int i;

	gs_array_init(&bin, 1);
	for (i = 0; i < (int)sizeof(blob); i++)
		blob[i] = (unsigned char)(i * 13 + 1);
	s_pick = 0x9E3779B9u;
	stubs_reset();
	P_RESET();

	for (it = 0; it < iterations; it++)
	{
		const int a  = (int)(pick() % NPROGS);
		const u32 op = pick() % 12;
		tr(0xAB000000u | (op << 8) | (u32)a);
		switch (op)
		{
			case 0:
				P_DESTROY(a);
				counts[a] = 0;
				break;
			case 1:
			{
				const size_t vl = source_length();
				const size_t fl = source_length();
				const int hint  = (int)(pick() & 1);
				P_DESTROY(a);
				counts[a] = 0;
				if (P_COMPILE(a, s_source, vl, s_source + 3, fl))
				{
					if (hint)
						P_HINT(a);
					tr(0x11u + (u32)P_LINK(a));
				}
				else
					tr(0x10u);
				break;
			}
			case 2:
			{
				const u32 len = 1 + pick() % 100;
				P_DESTROY(a);
				counts[a] = 0;
				tr((u32)P_FROMBIN(a, blob, len, 0x8000u + len));
				break;
			}
			case 3:
			{
				u32 fmt = 0;
				const int ok = P_GETBIN(a, &bin, &fmt);
				tr((u32)ok);
				if (ok)
				{
					size_t k;
					tr(fmt);
					tr((u32)bin.count);
					for (k = 0; k < bin.count; k++)
						tr(bin.items[k]);
				}
				break;
			}
			case 4:
			{
				const size_t vl = 1 + pick() % 40;
				const size_t fl = 1 + pick() % 40;
				P_DESTROY(a);
				counts[a] = 0;
				tr((u32)P_START(a, s_source, vl, s_source + 5, fl));
				break;
			}
			case 5:
				tr((u32)(P_POLL(a) + 7));
				break;
			case 6:
				if (counts[a] < 9)
				{
					tr((u32)P_REG(a, s_names[counts[a]]));
					counts[a]++;
				}
				break;
			case 7:
				if (counts[a] > 0)
				{
					const int idx = (int)(pick() % (u32)counts[a]);
					const u32 kind = pick() % 10;
					const u32 x = pick(), y = pick(), z = pick(), w = pick();
					float fv[4];
					s32 iv[4];
					fv[0] = (float)(s32)x / 3.0f;
					fv[1] = (float)(s32)y / 5.0f;
					fv[2] = (float)(s32)z / 7.0f;
					fv[3] = (float)(s32)w / 9.0f;
					iv[0] = (s32)x;
					iv[1] = (s32)y;
					iv[2] = (s32)z;
					iv[3] = (s32)w;
					switch (kind)
					{
						case 0: P_U1UI(a, idx, x); break;
						case 1: P_U3UI(a, idx, x, y, z); break;
						case 2: P_U4UI(a, idx, x, y, z, w); break;
						case 3: P_U1I(a, idx, (s32)x); break;
						case 4: P_U2I(a, idx, (s32)x, (s32)y); break;
						case 5: P_U1F(a, idx, fv[0]); break;
						case 6: P_U2IV(a, idx, iv); break;
						case 7: P_U4IV(a, idx, iv); break;
						case 8: P_U2FV(a, idx, fv); break;
						default: P_U4FV(a, idx, fv); break;
					}
				}
				break;
			case 8:
				P_BIND(a);
				break;
			case 9:
				P_RESET();
				break;
			case 10:
			{
				const int b = (int)(pick() % NPROGS);
				if (b != a)
				{
					P_MOVE(a, b);
					counts[a] = counts[b];
					counts[b] = 0;
				}
				break;
			}
			default:
				tr(0x50u + (u32)P_PENDING(a) * 2u + (u32)P_VALID(a));
				break;
		}
	}

	for (i = 0; i < NPROGS; i++)
		P_DESTROY(i);
	gs_array_free(&bin);
	tr(g_calls);
	return g_trace;
}
