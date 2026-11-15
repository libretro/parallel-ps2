/* The D3D12 shader cache (GS/Renderers/DX12/D3D12ShaderCache.c): blobs,
 * pipelines, snapshots, files and log as the C++ D3D12ShaderCache gave
 * them (the hash on the command line), scenario.h. Windows only: built
 * with MinGW, run under wine with its compiler, in a folder of its own. */
#define COBJMACROS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <direct.h>

#include "GS/Renderers/DX12/D3D12ShaderCache.h"
#include "fake.h"

retro_log_printf_t log_cb = fk_log;

typedef gs_d3d12_shader_cache* CACHE;
typedef ID3DBlob* SC_BLOB;
typedef gs_d3d12_cache_key KEY;
typedef gs_d3d12_cache_snapshot SNAP;
#define B_SIZE(b) ID3D10Blob_GetBufferSize(b)
#define B_DATA(b) ID3D10Blob_GetBufferPointer(b)
#define B_FREE(b) ID3D10Blob_Release(b)
static gs_d3d12_shader_cache* c_new(void)
{
	gs_d3d12_shader_cache* c = (gs_d3d12_shader_cache*)malloc(sizeof(*c));
	gs_d3d12_shader_cache_init(c);
	return c;
}
#define C_NEW() c_new()
#define C_DELETE(c) free(c)
#define C_OPEN(c, l, d, dir) gs_d3d12_shader_cache_open(c, l, d, dir)
#define C_CLOSE(c) gs_d3d12_shader_cache_close(c)
#define C_LEVEL(c) (c)->feature_level
#define C_DEBUG(c) (c)->debug
#define C_BLOB(c, t, s, n, m, e) gs_d3d12_shader_cache_blob(c, t, s, n, m, e)
#define C_GPSO(c, d, desc) (void*)gs_d3d12_shader_cache_graphics_pipeline(c, (ID3D12Device*)(d), desc)
#define C_CPSO(c, d, desc) (void*)gs_d3d12_shader_cache_compute_pipeline(c, (ID3D12Device*)(d), desc)
#define C_LIST(c) gs_d3d12_shader_cache_pipeline_list_path(c)
#define C_ADD_SHADER(c, k, b) gs_d3d12_shader_cache_add_shader(c, k, b)
#define C_ADD_PIPELINE(c, k, b) gs_d3d12_shader_cache_add_pipeline(c, k, b)
#define K_SHADER(k, t, s, n, m, e) gs_d3d12_shader_key(k, t, s, n, m, e)
#define K_GPSO(k, desc) gs_d3d12_graphics_pipeline_key(k, desc)
#define K_CPSO(k, desc) gs_d3d12_compute_pipeline_key(k, desc)
#define S_OPEN(c, s) gs_d3d12_cache_snapshot_open(c, s)
#define S_READ(s, k) gs_d3d12_cache_snapshot_read(s, k)
#define S_CLOSE(s) gs_d3d12_cache_snapshot_close(s)

#include "scenario.h"

static int fail(const char* what)
{
	printf("FAIL: %s\n", what);
	return 1;
}

int main(int argc, char** argv)
{
	unsigned long long h;
	gs_d3d12_shader_cache c;
	ID3DBlob* b;

	_mkdir(SC_DIR);
	scenario();
	h = fk_hash;
	printf("d3d12shadercache: %016llx\n", h);

	/* closed, and opened where no files can be: the index the files had
	 * is not used, and the shaders are compiled */
	gs_d3d12_shader_cache_init(&c);
	gs_d3d12_shader_cache_open(&c, D3D_FEATURE_LEVEL_11_0, 0, SC_DIR);
	if (!c.shaders.index.count || !c.pipelines.index.count)
		return fail("the files' entries were not read");
	gs_d3d12_shader_cache_close(&c);
	gs_d3d12_shader_cache_open(&c, D3D_FEATURE_LEVEL_11_0, 0, SC_DIR "\\missing");
	b = gs_d3d12_shader_cache_blob(&c, 0, sc_vs, sizeof(sc_vs) - 1, NULL, "vs_main");
	if (!b || c.shaders.index.count || c.pipelines.index.count)
		return fail("a reopened cache kept the index of the files it closed");
	ID3D10Blob_Release(b);
	gs_d3d12_shader_cache_close(&c);

	/* blobs that cannot be written (a folder in their place): the index
	 * made for them is closed, not left taking entries */
	_mkdir("d3d12dir.dir");
	_mkdir("d3d12dir.dir\\d3d12_shaders_sm50.bin");
	{
		FILE* f = fopen("d3d12dir.dir\\d3d12_shaders_sm50.bin\\keep", "wb");
		if (f)
			fclose(f);
	}
	gs_d3d12_shader_cache_init(&c);
	if (gs_d3d12_shader_cache_open(&c, D3D_FEATURE_LEVEL_11_0, 0, "d3d12dir.dir"))
		return fail("the cache opened without its blob file");
	if (c.shaders.index_file || c.shaders.blob_file)
		return fail("the index of blobs that could not be made was left open");
	gs_d3d12_shader_cache_close(&c);

	if (fk_live() != 0)
	{
		printf("FAIL: %d pipelines never released\n", fk_live());
		return 1;
	}
	if (argc > 1 && strtoull(argv[1], NULL, 16) != h)
	{
		printf("FAIL: the C++ D3D12ShaderCache gave %s\n", argv[1]);
		return 1;
	}
	return 0;
}
