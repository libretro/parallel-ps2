/* What GSDevice12 asks of its shader cache, with picked values: shaders
 * of each kind and graphics and compute pipelines built into new files,
 * read back from them in the next run, added to; the driver turning its
 * old blobs down, failing a pipeline or its blob; a snapshot taken for
 * the warm-up thread, read, and added to behind it; the files found
 * truncated, of another version, missing their blobs; each feature
 * level's and the debug files. The hash takes every blob, key, pipeline
 * call and line logged, and the files after each run. Through the C
 * cache (main.c) and the C++ D3D12ShaderCache (the oracle). */

#define SC_DIR "d3d12cache.dir"
#define SC_SH_IDX SC_DIR "\\d3d12_shaders_sm50.idx"
#define SC_SH_BIN SC_DIR "\\d3d12_shaders_sm50.bin"
#define SC_PL_IDX SC_DIR "\\d3d12_pipelines_sm50.idx"
#define SC_PL_BIN SC_DIR "\\d3d12_pipelines_sm50.bin"

static const char sc_vs[] =
	"#ifndef SCALE\n#define SCALE 2\n#endif\n"
	"float4 vs_main(float4 p : POSITION, float2 t : TEXCOORD0) : SV_Position { return p * SCALE + t.xyxy; }\n"
	"float4 vs_other(float4 p : POSITION) : SV_Position { return p.wzyx; }\n";
static const char sc_ps[] =
	"Texture2D t : register(t0); SamplerState s : register(s0);\n"
	"float4 ps_main(float4 p : SV_Position) : SV_Target { return t.Sample(s, p.xy) * p.x; }\n"
	"float4 ps_flat(float4 p : SV_Position) : SV_Target { return float4(1, 0, 1, 1); }\n";
static const char sc_cs[] =
	"RWTexture2D<float4> o : register(u0);\n"
	"[numthreads(8, 8, 1)] void cs_main(uint3 id : SV_DispatchThreadID) { o[id.xy] = float4(id.x, id.y, 0, 1); }\n";
static const char sc_bad[] = "float4 ps_main( : SV_Target { return 1; }\n";

static const D3D_SHADER_MACRO sc_macros3[] = {{"SCALE", "3"}, {NULL, NULL}};
static const D3D12_INPUT_ELEMENT_DESC sc_layout[2] = {
	{"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
	{"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};

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

static void sc_key(const KEY* k) { fk_h(k, sizeof(*k)); }

static void sc_pso(void* pso)
{
	fk_hu(pso != 0);
	fk_release(pso);
}

/* a graphics pipeline of the two shaders, told apart by @variant */
static void sc_graphics(D3D12_GRAPHICS_PIPELINE_STATE_DESC* d, SC_BLOB vs, SC_BLOB ps, unsigned variant)
{
	memset(d, 0, sizeof(*d));
	d->VS.pShaderBytecode                      = B_DATA(vs);
	d->VS.BytecodeLength                       = B_SIZE(vs);
	d->PS.pShaderBytecode                      = B_DATA(ps);
	d->PS.BytecodeLength                       = B_SIZE(ps);
	d->InputLayout.pInputElementDescs          = sc_layout;
	d->InputLayout.NumElements                 = 2;
	d->SampleMask                              = 0xFFFFFFFFu;
	d->RasterizerState.FillMode                = D3D12_FILL_MODE_SOLID;
	d->RasterizerState.CullMode                = D3D12_CULL_MODE_NONE;
	d->PrimitiveTopologyType                   = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	d->NumRenderTargets                        = 1 + (variant >> 2);
	d->RTVFormats[0]                           = (variant & 2) ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
	d->BlendState.RenderTarget[0].BlendEnable  = (variant & 1) ? TRUE : FALSE;
	d->BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	d->SampleDesc.Count                        = 1;
}

/* every kind of entry, some twice, and the warm-up thread's snapshot */
static void sc_use(CACHE c, void* dev)
{
	D3D12_GRAPHICS_PIPELINE_STATE_DESC g;
	D3D12_COMPUTE_PIPELINE_STATE_DESC cd;
	SC_BLOB vs, ps, cs;
	KEY k;
	SNAP snap;
	const char* list;

	sc_blob(C_BLOB(c, 0, sc_vs, sizeof(sc_vs) - 1, NULL, "vs_main"));
	sc_blob(C_BLOB(c, 0, sc_vs, sizeof(sc_vs) - 1, sc_macros3, "vs_main"));
	sc_blob(C_BLOB(c, 1, sc_ps, sizeof(sc_ps) - 1, NULL, "ps_flat"));
	sc_blob(C_BLOB(c, 1, sc_ps, sizeof(sc_ps) - 1, NULL, "ps_flat"));
	sc_blob(C_BLOB(c, 1, sc_bad, sizeof(sc_bad) - 1, NULL, "ps_main"));
	vs = C_BLOB(c, 0, sc_vs, sizeof(sc_vs) - 1, NULL, "vs_main");
	ps = C_BLOB(c, 1, sc_ps, sizeof(sc_ps) - 1, NULL, "ps_main");
	cs = C_BLOB(c, 2, sc_cs, sizeof(sc_cs) - 1, NULL, "cs_main");
	fk_hu(vs && ps && cs);
	if (!vs || !ps || !cs)
		return;

	/* pipelines, from the driver's blobs where the files have them */
	sc_graphics(&g, vs, ps, 0);
	sc_pso(C_GPSO(c, dev, &g));
	sc_pso(C_GPSO(c, dev, &g));
	K_GPSO(&k, &g);
	sc_key(&k);
	sc_graphics(&g, vs, ps, 3);
	sc_pso(C_GPSO(c, dev, &g));
	memset(&cd, 0, sizeof(cd));
	cd.CS.pShaderBytecode = B_DATA(cs);
	cd.CS.BytecodeLength  = B_SIZE(cs);
	sc_pso(C_CPSO(c, dev, &cd));
	K_CPSO(&k, &cd);
	sc_key(&k);

	/* the warm-up thread's view: what was there when it was taken */
	fk_hu(S_OPEN(c, &snap) != 0);
	fk_hu(snap.num_shaders);
	fk_hu(snap.num_pipelines);
	if (snap.shaders)
		fk_h(snap.shaders, snap.num_shaders * sizeof(*snap.shaders));
	if (snap.pipelines)
		fk_h(snap.pipelines, snap.num_pipelines * sizeof(*snap.pipelines));
	K_SHADER(&k, 0, sc_vs, sizeof(sc_vs) - 1, sc_macros3, "vs_main");
	sc_key(&k);
	sc_blob(S_READ(&snap, &k));
	K_SHADER(&k, 1, sc_bad, sizeof(sc_bad) - 1, NULL, "ps_main");
	sc_blob(S_READ(&snap, &k));
	sc_graphics(&g, vs, ps, 3);
	K_GPSO(&k, &g);
	sc_blob(S_READ(&snap, &k));
	K_CPSO(&k, &cd);
	sc_blob(S_READ(&snap, &k));
	/* added behind it: the cache takes them, the snapshot does not see
	 * them; a key the cache has is left as it is */
	K_SHADER(&k, 0, sc_vs, sizeof(sc_vs) - 1, NULL, "vs_other");
	C_ADD_SHADER(c, &k, ps);
	sc_blob(S_READ(&snap, &k));
	sc_blob(C_BLOB(c, 0, sc_vs, sizeof(sc_vs) - 1, NULL, "vs_other"));
	K_SHADER(&k, 1, sc_ps, sizeof(sc_ps) - 1, NULL, "ps_flat");
	C_ADD_SHADER(c, &k, vs);
	sc_blob(C_BLOB(c, 1, sc_ps, sizeof(sc_ps) - 1, NULL, "ps_flat"));
	sc_graphics(&g, vs, ps, 2);
	K_GPSO(&k, &g);
	C_ADD_PIPELINE(c, &k, cs);
	sc_blob(S_READ(&snap, &k));
	C_ADD_PIPELINE(c, &k, NULL);
	S_CLOSE(&snap);
	fk_hu(snap.shaders == 0 && snap.pipelines == 0 && snap.num_shaders == 0);

	list = C_LIST(c);
	fk_hu(list && *list);
	if (list)
		fk_h(list, strlen(list));

	/* the driver failing a pipeline, and the blob of one */
	fk_fail_create = 1;
	sc_graphics(&g, vs, ps, 4);
	sc_pso(C_GPSO(c, dev, &g));
	fk_fail_create      = 0;
	fk_fail_cached_blob = 1;
	sc_graphics(&g, vs, ps, 5);
	sc_pso(C_GPSO(c, dev, &g));
	fk_fail_cached_blob = 0;

	B_FREE(vs);
	B_FREE(ps);
	B_FREE(cs);
}

/* (a cache of its own each run, as each device has: the C++ cache kept
 * its index through a close, checked apart) */
static void sc_run(void* dev, D3D_FEATURE_LEVEL fl, int debug, const char* dir)
{
	CACHE c = C_NEW();
	fk_hu(C_OPEN(c, fl, debug, dir) != 0);
	fk_hu((unsigned)C_LEVEL(c));
	fk_hu(C_DEBUG(c) != 0);
	sc_use(c, dev);
	C_CLOSE(c);
	C_DELETE(c);
}

static void sc_hash_files(void)
{
	fk_hash_file(SC_SH_IDX);
	fk_hash_file(SC_SH_BIN);
	fk_hash_file(SC_PL_IDX);
	fk_hash_file(SC_PL_BIN);
}

static void scenario(void)
{
	void* dev = fk_device();

	/* no cache: everything built */
	sc_run(dev, D3D_FEATURE_LEVEL_11_0, 0, NULL);
	/* new files, then read back and added to */
	sc_run(dev, D3D_FEATURE_LEVEL_11_0, 0, SC_DIR);
	sc_hash_files();
	sc_run(dev, D3D_FEATURE_LEVEL_11_0, 0, SC_DIR);
	sc_hash_files();
	/* a new driver: the pipeline files begun again */
	fk_driver_changed = 1;
	sc_run(dev, D3D_FEATURE_LEVEL_11_0, 0, SC_DIR);
	fk_driver_changed = 0;
	sc_hash_files();
	sc_run(dev, D3D_FEATURE_LEVEL_11_0, 0, SC_DIR);
	sc_hash_files();
	/* a key an index has twice: the first stands */
	fk_dup_key(SC_SH_IDX);
	fk_dup_key(SC_PL_IDX);
	sc_run(dev, D3D_FEATURE_LEVEL_11_0, 0, SC_DIR);
	sc_hash_files();
	/* an index cut in the middle of an entry: made anew */
	fk_truncate(SC_SH_IDX, 4 + 64 + 30);
	sc_run(dev, D3D_FEATURE_LEVEL_11_0, 0, SC_DIR);
	sc_hash_files();
	/* an entry past the end of the blobs */
	fk_poke(SC_PL_IDX, 4 + 56, 0x7FFFFFF0u);
	sc_run(dev, D3D_FEATURE_LEVEL_11_0, 0, SC_DIR);
	sc_hash_files();
	/* another version */
	fk_poke(SC_SH_IDX, 0, 1u);
	fk_poke(SC_PL_IDX, 0, 1u);
	sc_run(dev, D3D_FEATURE_LEVEL_11_0, 0, SC_DIR);
	sc_hash_files();
	/* the blobs gone */
	remove(SC_SH_BIN);
	remove(SC_PL_BIN);
	sc_run(dev, D3D_FEATURE_LEVEL_11_0, 0, SC_DIR);
	sc_hash_files();
	/* each level's files, and the debug ones */
	sc_run(dev, D3D_FEATURE_LEVEL_10_0, 0, SC_DIR);
	sc_run(dev, D3D_FEATURE_LEVEL_10_1, 1, SC_DIR);
	sc_run(dev, D3D_FEATURE_LEVEL_11_1, 0, SC_DIR);
	fk_hash_file(SC_DIR "\\d3d12_shaders_sm40.idx");
	fk_hash_file(SC_DIR "\\d3d12_pipelines_sm41_debug.bin");
	fk_hash_file(SC_DIR "\\d3d12_shaders_unk.idx");
	/* a folder that is not there: no files, everything still built */
	sc_run(dev, D3D_FEATURE_LEVEL_11_0, 0, SC_DIR "\\missing");
}
