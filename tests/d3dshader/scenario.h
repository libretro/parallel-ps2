/* What the renderers ask of D3D.c, with picked values: HLSL compiled for
 * each kind of shader at each feature level, debug and not, with and
 * without macros, sources that fail and that warn; the vendor of adapters
 * of every vendor id the renderers tell apart; a factory. The hash takes
 * each blob's bytes and every line logged. Through the C functions
 * (main.c) and the C++ D3D namespace they replaced (the oracle). */

static const char sc_vs[] =
	"#ifndef SCALE\n#define SCALE 2\n#endif\n"
	"float4 main(float4 p : POSITION) : SV_Position { return p * SCALE; }\n";
static const char sc_ps[] =
	"Texture2D t : register(t0); SamplerState s : register(s0);\n"
	"float4 main(float4 p : SV_Position, float2 uv : TEXCOORD0) : SV_Target { return t.Sample(s, uv) * p.x; }\n";
static const char sc_cs[] =
	"RWTexture2D<float4> o : register(u0);\n"
	"[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) { o[id.xy] = float4(id.x, id.y, 0, 1); }\n";
static const char sc_bad[] = "float4 main( : SV_Position { return 1; }\n";
static const char sc_warn[] =
	"float4 main(float4 p : POSITION) : SV_Position { float2 a = p; return float4(a, 0, 1); }\n";

static void sc_blob(SC_BLOB b)
{
	fk_hu(b != 0);
	if (b)
	{
		fk_hu(B_SIZE(b));
		fk_h(B_DATA(b), B_SIZE(b));
		B_FREE(b);
	}
}

static void scenario(void)
{
	static const D3D_FEATURE_LEVEL levels[4] = {
		D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_11_1};
	static const D3D_SHADER_MACRO macros[2] = {{"SCALE", "3"}, {NULL, NULL}};
	static const unsigned vendors[9] = {0x10DE, 0x1002, 0x1022, 0x163C, 0x8086, 0x8087, 0x5143, 0, 0xFFFF};
	unsigned l, d, i;

	for (l = 0; l < 4; l++)
		for (d = 0; d < 2; d++)
		{
			sc_blob(COMPILE(0, levels[l], d, sc_vs, sizeof(sc_vs) - 1, NULL, "main"));
			sc_blob(COMPILE(0, levels[l], d, sc_vs, sizeof(sc_vs) - 1, macros, "main"));
			sc_blob(COMPILE(1, levels[l], d, sc_ps, sizeof(sc_ps) - 1, NULL, "main"));
			sc_blob(COMPILE(2, levels[l], d, sc_cs, sizeof(sc_cs) - 1, NULL, "main"));
			sc_blob(COMPILE(0, levels[l], d, sc_bad, sizeof(sc_bad) - 1, NULL, "main"));
			sc_blob(COMPILE(0, levels[l], d, sc_warn, sizeof(sc_warn) - 1, NULL, "main"));
			sc_blob(COMPILE(0, levels[l], d, sc_vs, sizeof(sc_vs) - 1, NULL, "nothing"));
		}
	/* a level the renderers do not name: the newest model */
	sc_blob(COMPILE(1, (D3D_FEATURE_LEVEL)0xc000, 0, sc_ps, sizeof(sc_ps) - 1, NULL, "main"));

	for (i = 0; i < 9; i++)
		fk_hu(VENDOR(fk_adapter(vendors[i], 1)));
	fk_hu(VENDOR(fk_adapter(0x10DE, 0)));

	fk_hu(FACTORY_OK(0));
}
