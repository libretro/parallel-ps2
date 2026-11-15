/* What GSDevice11 and the renderers do with textures, with picked
 * values: made of every type and format (some too wide to make), updated
 * in parts, their views asked for in any order, mipmaps, maps (which
 * D3D11 refuses), read-backs, and freed. Through the C textures (main.c)
 * and the C++ GSTexture11 (the oracle): the device sees the same. */
#define SC_TEX 10
#define SC_DL 3

static unsigned char sc_data[256 * 1024];
static unsigned sc_r = 0x2468ACE1u;

static unsigned R(unsigned n)
{
	sc_r ^= sc_r << 13;
	sc_r ^= sc_r >> 17;
	sc_r ^= sc_r << 5;
	return n ? sc_r % n : 0;
}

static void sc_state(TEX t)
{
	if (!t)
	{
		fk_hu(0);
		return;
	}
	fk_hu(T_W(t)); fk_hu(T_H(t)); fk_hu(T_LEVELS(t)); fk_hu(T_NEEDS_MIPS(t)); fk_hu(fk_id(T_TEXTURE(t)));
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

	for (round = 0; round < 6000; round++)
	{
		const unsigned op = R(12);
		const unsigned k  = R(SC_TEX);
		TEX t             = tex[k];

		if (op == 0 || !t)
		{
			unsigned type, format, w, h, levels;
			if (t)
			{
				T_FREE(t);
				tex[k] = 0;
			}
			type   = 1 + R(4);
			format = 1 + R(11);
			w      = R(40) == 0 ? 5000 : 1 + R(700);
			h      = 1 + R(500);
			levels = type == 3 ? 1 + R(4) : 1;
			if (type == 2)
				format = 3;
			tex[k] = T_CREATE(type, format, (int)w, (int)h, (int)levels);
			sc_state(tex[k]);
			continue;
		}

		switch (op)
		{
			case 1: case 2:
			{
				int rc[4], layer, ok;
				layer = (int)R(3);
				rc[0] = (int)R((unsigned)T_W(t));
				rc[1] = (int)R((unsigned)T_H(t));
				rc[2] = rc[0] + 1 + (int)R((unsigned)(T_W(t) - rc[0]));
				rc[3] = rc[1] + 1 + (int)R((unsigned)(T_H(t) - rc[1]));
				ok = T_UPDATE(t, rc, sc_data + R(1024), (rc[2] - rc[0]) * 4 + (int)R(4) * 4, layer);
				fk_hu((unsigned)ok);
				break;
			}
			case 3:
			{
				unsigned char* bits = 0;
				int pitch = 0;
				fk_hu((unsigned)T_MAP(t, &bits, &pitch));
				T_UNMAP(t);
				break;
			}
			case 4:
				T_MIPMAP(t);
				T_SET_NEEDS_MIPS(t, 0);
				break;
			case 5: fk_hu(fk_id(T_SRV(t))); break;
			case 6: if (T_TYPE(t) == 1 || T_TYPE(t) == 3) fk_hu(fk_id(T_RTV(t))); break;
			case 7: if (T_TYPE(t) == 2) fk_hu(fk_id(T_DSV(t))); break;
			case 8: if (T_TYPE(t) == 4) fk_hu(fk_id(T_UAV(t))); break;
			default:
			{
				/* a read-back of a part of the texture */
				unsigned d = R(SC_DL);
				int src[4], drc[4], ok;
				if (!dl[d] || R(10) == 0)
				{
					if (dl[d])
						D_FREE(dl[d]);
					dl[d] = D_CREATE(16 + R(500), 16 + R(300), T_FORMAT(t));
					fk_hu(dl[d] != 0);
					if (!dl[d])
						break;
				}
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
				if (R(3))
				{
					if (R(2))
						D_FLUSH(dl[d]);
					ok = D_MAP(dl[d], drc);
					fk_hu((unsigned)ok);
					if (ok)
					{
						fk_hu(D_PITCH(dl[d]));
						fk_h(D_PTR(dl[d]), 16);
						if (R(2))
							D_UNMAP(dl[d]);
					}
				}
				break;
			}
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
