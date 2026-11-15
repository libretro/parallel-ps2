/* What GSDevice12 and the renderers do with textures, with picked
 * values: made of every type and format with the views each takes (some
 * too wide to place, some past a full descriptor heap), uploads that fit
 * the stream buffer and ones that take a staging buffer, maps, mipmaps,
 * state changes, clears, read-backs, and destroyed now or deferred.
 * Through the C textures (main.c) and the C++ GSTexture12 (the oracle):
 * the device sees the same. */
#define SC_TEX 12
#define SC_DL 3

static unsigned char sc_data[1536 * 1024];
static unsigned sc_r = 0x12345678u;

static unsigned R(unsigned n)
{
	sc_r ^= sc_r << 13;
	sc_r ^= sc_r >> 17;
	sc_r ^= sc_r << 5;
	return n ? sc_r % n : 0;
}

/* DXGI formats as the device picks them, by GS format */
static const unsigned sc_dxgi[13] = {0, 28, 10, 40, 61, 57, 42, 39, 71, 74, 77, 98, 28};

static void sc_state(TEX t)
{
	if (!t)
	{
		fk_hu(0x0);
		return;
	}
	fk_hu(T_STATE(t)); fk_hu(T_RSTATE(t)); fk_hu(T_NEEDS_MIPS(t)); fk_hu(fk_resource_id(T_RES(t)));
	fk_hu(T_SRV(t)); fk_hu(T_WRITE(t)); fk_hu(T_UAV(t));
}

static TEX sc_create(unsigned type, unsigned format, int w, int h, int levels)
{
	const unsigned dx = sc_dxgi[format];
	return T_CREATE(type, format, w, h, levels, dx, R(8) ? dx : 0, type == 1 ? dx : 0, type == 2 ? 40 : 0,
		type == 4 ? dx : 0);
}

static void scenario(void)
{
	TEX tex[SC_TEX];
	DTEX dl[SC_DL];
	unsigned round, i;

	for (i = 0; i < sizeof(sc_data); i++)
		sc_data[i] = (unsigned char)(i * 131 + (i >> 9));
	for (i = 0; i < SC_TEX; i++)
		tex[i] = 0;
	for (i = 0; i < SC_DL; i++)
		dl[i] = 0;

	/* a map of exactly half the stream buffer, which is refused, and one
	 * a row less */
	{
		unsigned char* bits = 0;
		int pitch = 0, rc[4];
		TEX t = sc_create(3, 1, 64, 1024, 1);
		fk_hu(T_MAP(t, 0, 0, &bits, &pitch));
		rc[0] = 0; rc[1] = 0; rc[2] = 64; rc[3] = 1023;
		fk_hu(T_MAP(t, rc, 0, &bits, &pitch));
		T_UNMAP(t);
		sc_state(t);
		T_FREE(t);
	}

	/* an upload of exactly half the stream buffer, which goes through
	 * it, and one a row more, which takes a buffer of its own */
	{
		int rc[4];
		TEX t = sc_create(3, 1, 64, 1025, 1);
		rc[0] = 0; rc[1] = 0; rc[2] = 64; rc[3] = 1024;
		fk_hu(T_UPDATE(t, rc, sc_data, 64 * 4, 0));
		rc[3] = 1025;
		fk_hu(T_UPDATE(t, rc, sc_data, 64 * 4, 0));
		sc_state(t);
		T_FREE(t);
	}

	/* uploads that fill the stream buffer in one command list: the
	 * later ones have to execute it to make room */
	{
		int rc[4], k;
		TEX t = sc_create(3, 1, 64, 1024, 1);
		rc[0] = 0; rc[1] = 0; rc[2] = 64; rc[3] = 400;
		for (k = 0; k < 8; k++)
			fk_hu(T_UPDATE(t, rc, sc_data + 64 * k, 64 * 4, 0));
		sc_state(t);
		T_FREE(t);
	}

	for (round = 0; round < 6000; round++)
	{
		const unsigned op = R(16);
		const unsigned k  = R(SC_TEX);
		TEX t             = tex[k];

		if (R(30) == 0)
			fk_execute(R(2));

		if (op == 0 || !t)
		{
			unsigned type, format, w, h, levels, big;
			if (t)
			{
				if (R(3) == 0)
				{
					T_DESTROY(t, R(2));
					sc_state(t);
				}
				T_FREE(t);
				tex[k] = 0;
			}
			type   = 1 + R(4);
			format = 1 + R(12);
			big    = R(30);
			w      = big == 0 ? 2500 : 1 + R(big < 4 ? 600 : 96);
			h      = 1 + R(big < 4 ? 400 : 64);
			levels = type == 3 ? 1 + R(4) : 1;
			if (type == 2)
				format = 3;
			tex[k] = sc_create(type, format, (int)w, (int)h, (int)levels);
			sc_state(tex[k]);
			continue;
		}

		switch (op)
		{
			case 1: case 2: case 3:
			{
				/* an upload: a part, or the whole, of a level */
				int rc[4], pitch, layer, whole, ok;
				whole = R(4) == 0;
				layer = (int)R(3);
				rc[0] = whole ? 0 : (int)R((unsigned)T_W(t));
				rc[1] = whole ? 0 : (int)R((unsigned)T_H(t));
				rc[2] = whole ? T_W(t) : rc[0] + 1 + (int)R((unsigned)(T_W(t) - rc[0]));
				rc[3] = whole ? T_H(t) : rc[1] + 1 + (int)R((unsigned)(T_H(t) - rc[1]));
				pitch = (rc[2] - rc[0]) * 4 + (int)R(3) * 4;
				if ((size_t)pitch * (size_t)(rc[3] - rc[1] + 4) > sizeof(sc_data) - 64)
					break;
				if (R(3) == 0)
					T_SET_USE(t, fk_fence);
				ok = T_UPDATE(t, rc, sc_data + R(64), pitch, layer);
				fk_hu((unsigned)ok);
				break;
			}
			case 4:
			{
				/* a map, written, and unmapped */
				int rc[4], use_rect, layer, ok, pitch;
				unsigned char* bits = 0;
				use_rect = (int)R(2);
				layer    = (int)R(2);
				rc[0] = (int)R((unsigned)T_W(t));
				rc[1] = (int)R((unsigned)T_H(t));
				rc[2] = rc[0] + 1 + (int)R((unsigned)(T_W(t) - rc[0]));
				rc[3] = rc[1] + 1 + (int)R((unsigned)(T_H(t) - rc[1]));
				if (R(3) == 0)
					T_SET_USE(t, fk_fence);
				ok = T_MAP(t, use_rect ? rc : 0, layer, &bits, &pitch);
				fk_hu((unsigned)ok);
				if (ok)
				{
					int rows = use_rect ? rc[3] - rc[1] : T_H(t);
					fk_hu((unsigned)pitch);
					memset(bits, (int)(round & 0xff), (size_t)pitch * (size_t)(rows > 4 ? 4 : rows));
					T_UNMAP(t);
				}
				break;
			}
			case 5:
				T_MIPMAP(t);
				T_SET_NEEDS_MIPS(t, 0);
				break;
			case 6:
			{
				static const unsigned states[6] = {0, 0x4, 0x8, 0x10, 0x40, 0x400};
				T_TRANSITION(t, states[R(6)]);
				break;
			}
			case 7:
				T_TRANSITION_LEVEL(t, (int)R(3), T_RSTATE(t), 0x800);
				T_TRANSITION_LEVEL(t, (int)R(3), 0x800, T_RSTATE(t));
				break;
			case 8:
				if (T_TYPE(t) == 2)
					T_SET_CLEAR_DEPTH(t, (float)R(1000) / 999.0f);
				else if (T_TYPE(t) == 1)
					T_SET_CLEAR_COLOR(t, R(0xFFFFFFFFu) ^ (R(256) << 24));
				break;
			case 9:
				if (T_TYPE(t) <= 2)
				{
					if (R(2))
						T_COMMIT_CLEAR(t);
					else
						T_COMMIT_CLEAR_CMD(t);
				}
				break;
			case 10:
				T_SET_USE(t, R(2) ? fk_fence : 0);
				break;
			case 11: case 12:
			{
				/* a read-back of a part of the texture */
				unsigned d = R(SC_DL);
				int src[4], drc[4], ok;
				if (!dl[d] || R(10) == 0)
				{
					if (dl[d])
						D_FREE(dl[d]);
					dl[d] = D_CREATE(16 + R(200), 16 + R(150), T_FORMAT(t));
					fk_hu(dl[d] != 0);
					if (!dl[d])
						break;
				}
				if (D_FORMAT(dl[d]) != T_FORMAT(t) || T_FORMAT(t) >= 9)
					break;
				src[0] = (int)R((unsigned)T_W(t));
				src[1] = (int)R((unsigned)T_H(t));
				src[2] = src[0] + 1 + (int)R((unsigned)(T_W(t) - src[0]));
				src[3] = src[1] + 1 + (int)R((unsigned)(T_H(t) - src[1]));
				if ((unsigned)(src[2] - src[0]) > D_W(dl[d]) || (unsigned)(src[3] - src[1]) > D_H(dl[d]))
					break;
				drc[0] = (int)R(D_W(dl[d]) - (unsigned)(src[2] - src[0]) + 1);
				drc[1] = (int)R(D_H(dl[d]) - (unsigned)(src[3] - src[1]) + 1);
				drc[2] = drc[0] + src[2] - src[0];
				drc[3] = drc[1] + src[3] - src[1];
				D_COPY(dl[d], drc, t, src, 0, (int)R(2));
				fk_hu(D_PITCH(dl[d]));
				if (R(2))
				{
					/* (the copy's list done already, or not) */
					if (R(2))
						fk_execute(1);
					D_FLUSH(dl[d]);
					ok = D_MAP(dl[d], drc);
					fk_hu((unsigned)ok);
					if (ok)
					{
						fk_h(D_PTR(dl[d]), 16);
						if (R(2))
							D_UNMAP(dl[d]);
					}
				}
				break;
			}
			default:
				if (R(4) == 0)
					fk_wait(fk_fence - 1 > fk_completed ? fk_completed + 1 : fk_completed);
				break;
		}
		sc_state(t);
	}

	for (i = 0; i < SC_TEX; i++)
		if (tex[i])
			T_FREE(tex[i]);
	for (i = 0; i < SC_DL; i++)
		if (dl[i])
			D_FREE(dl[i]);
}
