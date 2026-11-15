/* What GSDevice11 asks of its shader cache, with picked values: shaders
 * of each kind compiled into new files, read back from them in the next
 * run, added to, and the files found truncated, of another version,
 * missing their blobs; each feature level's and the debug files; device
 * calls that fail. The hash takes every blob, every shader and layout
 * made, every line logged and the files after each run. Through the C
 * cache (main.c) and the C++ D3D11ShaderCache (the oracle). */

#define SC_DIR "d3d11cache.dir"
#define SC_IDX SC_DIR "\\d3d_shaders_sm50.idx"
#define SC_BIN SC_DIR "\\d3d_shaders_sm50.bin"

static const char sc_vs[] =
	"#ifndef SCALE\n#define SCALE 2\n#endif\n"
	"float4 vs_main(float4 p : POSITION, float2 t : TEXCOORD0) : SV_Position { return p * SCALE + t.xyxy; }\n"
	"float4 vs_other(float4 p : POSITION) : SV_Position { return p.wzyx; }\n";
static const char sc_ps[] =
	"Texture2D t : register(t0); SamplerState s : register(s0);\n"
	"float4 ps_main(float4 p : SV_Position, float2 uv : TEXCOORD0) : SV_Target { return t.Sample(s, uv) * p.x; }\n"
	"float4 ps_flat(float4 p : SV_Position) : SV_Target { return float4(1, 0, 1, 1); }\n";
static const char sc_cs[] =
	"RWTexture2D<float4> o : register(u0);\n"
	"[numthreads(8, 8, 1)] void cs_main(uint3 id : SV_DispatchThreadID) { o[id.xy] = float4(id.x, id.y, 0, 1); }\n";
static const char sc_bad[] = "float4 ps_main( : SV_Target { return 1; }\n";

static const D3D_SHADER_MACRO sc_macros3[] = {{"SCALE", "3"}, {NULL, NULL}};
static const D3D_SHADER_MACRO sc_macros4[] = {{"SCALE", "4"}, {"UNUSED", "1"}, {NULL, NULL}};
static const D3D11_INPUT_ELEMENT_DESC sc_layout[2] = {
	{"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
	{"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0}};

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

/* every kind of shader, some twice */
static void sc_use(CACHE c, void* dev)
{
	void *vs = 0, *il = 0;
	sc_blob(C_BLOB(c, 0, sc_vs, sizeof(sc_vs) - 1, NULL, "vs_main"));
	sc_blob(C_BLOB(c, 0, sc_vs, sizeof(sc_vs) - 1, sc_macros3, "vs_main"));
	sc_blob(C_BLOB(c, 0, sc_vs, sizeof(sc_vs) - 1, sc_macros4, "vs_main"));
	sc_blob(C_BLOB(c, 1, sc_ps, sizeof(sc_ps) - 1, NULL, "ps_main"));
	sc_blob(C_BLOB(c, 1, sc_ps, sizeof(sc_ps) - 1, NULL, "ps_main"));
	sc_blob(C_BLOB(c, 1, sc_bad, sizeof(sc_bad) - 1, NULL, "ps_main"));
	/* (the same text and entry point, another kind) */
	sc_blob(C_BLOB(c, 2, sc_cs, sizeof(sc_cs) - 1, NULL, "cs_main"));
	fk_release(C_VS(c, dev, sc_vs, sizeof(sc_vs) - 1, NULL, "vs_other"));
	fk_release(C_PS(c, dev, sc_ps, sizeof(sc_ps) - 1, NULL, "ps_flat"));
	fk_release(C_CS(c, dev, sc_cs, sizeof(sc_cs) - 1, NULL, "cs_main"));
	fk_release(C_PS(c, dev, sc_bad, sizeof(sc_bad) - 1, NULL, "ps_main"));
	fk_hu((unsigned)C_VS_LAYOUT(c, dev, &vs, &il, sc_layout, 2, sc_vs, sizeof(sc_vs) - 1, NULL, "vs_main"));
	fk_hu(vs != 0); fk_hu(il != 0);
	fk_release(vs);
	fk_release(il);
	vs = 0; il = 0;
	/* the device refusing */
	fk_fail_layout = 1;
	fk_hu((unsigned)C_VS_LAYOUT(c, dev, &vs, &il, sc_layout, 2, sc_vs, sizeof(sc_vs) - 1, NULL, "vs_main"));
	fk_hu(vs != 0);
	fk_fail_layout = 0;
	fk_fail_vs     = 1;
	fk_hu(C_VS(c, dev, sc_vs, sizeof(sc_vs) - 1, NULL, "vs_main") != 0);
	fk_hu((unsigned)C_VS_LAYOUT(c, dev, &vs, &il, sc_layout, 2, sc_vs, sizeof(sc_vs) - 1, NULL, "vs_main"));
	fk_fail_vs = 0;
}

/* (a cache of its own each run, as each device has: the C++ cache kept
 * its index through a close, checked apart) */
static void sc_run(void* dev, D3D_FEATURE_LEVEL fl, int debug, const char* dir)
{
	CACHE c = C_NEW();
	fk_hu((unsigned)C_OPEN(c, fl, debug, dir));
	fk_hu((unsigned)C_LEVEL(c));
	sc_use(c, dev);
	C_CLOSE(c);
	C_DELETE(c);
}

static void scenario(void)
{
	void* dev = fk_device();

	/* no cache: every shader compiled */
	sc_run(dev, D3D_FEATURE_LEVEL_11_0, 0, NULL);
	/* new files, then read back and added to */
	sc_run(dev, D3D_FEATURE_LEVEL_11_0, 0, SC_DIR);
	fk_hash_file(SC_IDX);
	fk_hash_file(SC_BIN);
	sc_run(dev, D3D_FEATURE_LEVEL_11_0, 0, SC_DIR);
	fk_hash_file(SC_IDX);
	fk_hash_file(SC_BIN);
	/* a key the index has twice: the first stands */
	fk_dup_key(SC_IDX);
	sc_run(dev, D3D_FEATURE_LEVEL_11_0, 0, SC_DIR);
	fk_hash_file(SC_IDX);
	/* an index cut in the middle of an entry: made anew */
	fk_truncate(SC_IDX, 4 + 64 + 30);
	sc_run(dev, D3D_FEATURE_LEVEL_11_0, 0, SC_DIR);
	fk_hash_file(SC_IDX);
	fk_hash_file(SC_BIN);
	/* an entry past the end of the blobs */
	fk_poke(SC_IDX, 4 + 56, 0x7FFFFFF0u);
	sc_run(dev, D3D_FEATURE_LEVEL_11_0, 0, SC_DIR);
	fk_hash_file(SC_IDX);
	/* another version */
	fk_poke(SC_IDX, 0, 1u);
	sc_run(dev, D3D_FEATURE_LEVEL_11_0, 0, SC_DIR);
	fk_hash_file(SC_IDX);
	/* the blobs gone */
	remove(SC_BIN);
	sc_run(dev, D3D_FEATURE_LEVEL_11_0, 0, SC_DIR);
	fk_hash_file(SC_IDX);
	fk_hash_file(SC_BIN);
	/* each level's files, and the debug ones */
	sc_run(dev, D3D_FEATURE_LEVEL_10_0, 0, SC_DIR);
	sc_run(dev, D3D_FEATURE_LEVEL_10_1, 1, SC_DIR);
	sc_run(dev, D3D_FEATURE_LEVEL_11_1, 0, SC_DIR);
	fk_hash_file(SC_DIR "\\d3d_shaders_sm40.idx");
	fk_hash_file(SC_DIR "\\d3d_shaders_sm41_debug.bin");
	fk_hash_file(SC_DIR "\\d3d_shaders_unk.idx");
	/* a folder that is not there: no files, the shaders still made */
	sc_run(dev, D3D_FEATURE_LEVEL_11_0, 0, SC_DIR "\\missing");
}
