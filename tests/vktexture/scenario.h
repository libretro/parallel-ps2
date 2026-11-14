/* What GSDeviceVK and the renderers do with textures, with picked
 * values: made of every type and format (one too big to place), uploads
 * that fit the stream buffer and ones that take a staging buffer, maps,
 * mipmaps, layout changes, clears, linked framebuffers, read-backs, and
 * destroyed now or deferred. Through the C textures (main.c) and the C++
 * GSTextureVK (the oracle): the device sees the same. */
#define SC_TEX 10
#define SC_DL 3

static unsigned char sc_data[1536 * 1024];

static void sc_state(TEX t)
{
	if (!t)
	{
		HU(0x0);
		return;
	}
	HU(T_LAYOUT(t)); HU(T_STATE(t)); HU(T_NEEDS_MIPS(t)); HN(T_IMAGE(t)); HN(T_VIEW(t));
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
		TEX t = T_CREATE(3, 1, 64, 1024, 1);
		HU(T_MAP(t, 0, 0, &bits, &pitch));
		rc[0] = 0; rc[1] = 0; rc[2] = 64; rc[3] = 1023;
		HU(T_MAP(t, rc, 0, &bits, &pitch));
		T_UNMAP(t);
		sc_state(t);
		T_FREE(t);
	}

	/* uploads that fill the stream buffer in one command buffer:
	 * the later ones have to submit it to make room */
	{
		int rc[4], k;
		TEX t = T_CREATE(3, 1, 64, 1024, 1);
		rc[0] = 0; rc[1] = 0; rc[2] = 64; rc[3] = 400;
		for (k = 0; k < 8; k++)
			HU(T_UPDATE(t, rc, sc_data + 64 * k, 64 * 8, 0));
		sc_state(t);
		T_FREE(t);
	}

	for (round = 0; round < 4000; round++)
	{
		const unsigned op = R(16);
		const unsigned k  = R(SC_TEX);
		TEX t             = tex[k];

		if (R(40) == 0)
			fk_execute(0);
		if (R(60) == 0)
			fk_stencil = !fk_stencil;

		if (op == 0 || !t)
		{
			unsigned type, format, w, h, levels, big;
			if (t)
			{
				T_FREE(t);
				tex[k] = 0;
			}
			type   = 1 + R(4);
			format = 1 + R(11);
			big    = R(30);
			w      = big == 0 ? 3500 : 1 + R(big < 4 ? 600 : 96);
			h      = 1 + R(big < 4 ? 400 : 64);
			levels = type == 3 ? 1 + R(4) : 1;
			if (type == 2)
				format = 3;
			tex[k] = T_CREATE(type, format, w, h, levels);
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
				if ((size_t)pitch * (size_t)(rc[3] - rc[1]) > sizeof(sc_data))
					break;
				if (R(3) == 0)
					T_SET_USE(t, fk_fence);
				ok = T_UPDATE(t, rc, sc_data + R(64), pitch, layer);
				HU(ok);
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
				ok = T_MAP(t, use_rect ? rc : 0, layer, &bits, &pitch);
				HU(ok);
				if (ok)
				{
					int rows = use_rect ? rc[3] - rc[1] : T_H(t);
					HU((unsigned)pitch);
					memset(bits, (int)(round & 0xff), (size_t)pitch * (size_t)(rows > 4 ? 4 : rows));
					T_UNMAP(t);
				}
				break;
			}
			case 5:
				T_MIPMAP(t);
				/* (as the device does once it has) */
				T_SET_NEEDS_MIPS(t, 0);
				break;
			case 6: case 7:
			{
				const unsigned layout = R(14);
				if (R(2))
					T_TRANSITION(t, layout);
				else
					T_TRANSITION_CMD(t, layout);
				break;
			}
			case 8:
			{
				const unsigned a = R(14), b = R(14), start = R(2), n = 1 + R(2);
				T_LEVELS(t, start, n, a, b);
				break;
			}
			case 9:
			{
				const unsigned c = R(0xffffff) * 256u + R(256);
				const unsigned how = R(3);
				if (how == 0)
					T_SET_CLEAR_COLOR(t, c);
				else if (how == 1)
					T_SET_CLEAR_DEPTH(t, (float)R(1000) / 999.0f);
				else
					T_SET_STATE(t, R(3));
				if (R(2))
					T_COMMIT_CLEAR(t);
				else
					T_COMMIT_CLEAR_CMD(t);
				break;
			}
			case 10: case 11:
			{
				/* a framebuffer: alone, or a target with a depth buffer */
				const unsigned j = R(SC_TEX), fb_loop = R(2), linked = R(2);
				TEX d = tex[j];
				VkFramebuffer fb;
				if (!(T_TYPE(t) == 1 || T_TYPE(t) == 2))
					break;
				if (linked && d && d != t && T_TYPE(d) == 2 && T_TYPE(t) == 1 && T_W(d) >= T_W(t) && T_H(d) >= T_H(t))
					fb = T_FB(t, d, fb_loop);
				else
					fb = T_FB(t, 0, fb_loop);
				HN(fb);
				break;
			}
			case 12:
			{
				const unsigned defer = R(2);
				T_DESTROY(t, defer);
				break;
			}
			case 13: case 14:
			{
				/* a read-back */
				const unsigned d = R(SC_DL);
				int src[4], drc[4], use_tp, level;
				if (!dl[d] || R(8) == 0)
				{
					unsigned w = 1 + R(256), h = 1 + R(128), fmt = 1 + R(7);
					if (dl[d])
						D_FREE(dl[d]);
					dl[d] = D_CREATE(w, h, fmt);
					HU(dl[d] != 0);
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
				use_tp = (int)R(2);
				level  = (int)R(2);
				D_COPY(dl[d], drc, t, src, level, use_tp);
				HU(D_PITCH(dl[d]));
				if (R(2))
				{
					if (R(3) == 0)
						fk_execute(R(2));
					D_FLUSH(dl[d]);
					HU(D_MAP(dl[d], drc));
					D_UNMAP(dl[d]);
				}
				break;
			}
			default:
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
