/* The D3D11 shader cache (GS/Renderers/DX11/D3D11ShaderCache.c): blobs,
 * shaders, files and log as the C++ D3D11ShaderCache gave them (the hash
 * on the command line), scenario.h. Windows only: built with MinGW, run
 * under wine with its compiler, in a folder of its own. */
#define COBJMACROS
#include <stdio.h>
#include <stdlib.h>
#include <direct.h>

#include "GS/Renderers/DX11/D3D11ShaderCache.h"
#include "fake.h"

retro_log_printf_t log_cb = fk_log;

typedef gs_d3d11_shader_cache* CACHE;
typedef ID3DBlob* SC_BLOB;
#define B_SIZE(b) ID3D10Blob_GetBufferSize(b)
#define B_DATA(b) ID3D10Blob_GetBufferPointer(b)
#define B_FREE(b) ID3D10Blob_Release(b)
static gs_d3d11_shader_cache* c_new(void)
{
	gs_d3d11_shader_cache* c = (gs_d3d11_shader_cache*)malloc(sizeof(*c));
	gs_d3d11_shader_cache_init(c);
	return c;
}
#define C_NEW() c_new()
#define C_DELETE(c) free(c)
#define C_OPEN(c, l, d, dir) gs_d3d11_shader_cache_open(c, l, d, dir)
#define C_CLOSE(c) gs_d3d11_shader_cache_close(c)
#define C_LEVEL(c) (c)->feature_level
#define C_BLOB(c, t, s, n, m, e) gs_d3d11_shader_cache_blob(c, t, s, n, m, e)
#define C_VS(c, d, s, n, m, e) gs_d3d11_shader_cache_vertex_shader(c, (ID3D11Device*)(d), s, n, m, e)
#define C_PS(c, d, s, n, m, e) gs_d3d11_shader_cache_pixel_shader(c, (ID3D11Device*)(d), s, n, m, e)
#define C_CS(c, d, s, n, m, e) gs_d3d11_shader_cache_compute_shader(c, (ID3D11Device*)(d), s, n, m, e)
#define C_VS_LAYOUT(c, d, vs, il, l, ln, s, n, m, e) gs_d3d11_shader_cache_vertex_shader_and_layout(c, \
	(ID3D11Device*)(d), (ID3D11VertexShader**)(vs), (ID3D11InputLayout**)(il), l, ln, s, n, m, e)

#include "scenario.h"

int main(int argc, char** argv)
{
	unsigned long long h;
	_mkdir(SC_DIR);
	scenario();
	h = fk_hash;
	printf("d3d11shadercache: %016llx\n", h);
	/* closed, and opened where no files can be: the index the files had
	 * is not used, and the shaders are compiled */
	{
		gs_d3d11_shader_cache c;
		ID3DBlob* b;
		gs_d3d11_shader_cache_init(&c);
		gs_d3d11_shader_cache_open(&c, D3D_FEATURE_LEVEL_11_0, 0, SC_DIR);
		gs_d3d11_shader_cache_close(&c);
		gs_d3d11_shader_cache_open(&c, D3D_FEATURE_LEVEL_11_0, 0, SC_DIR "\\missing");
		b = gs_d3d11_shader_cache_blob(&c, 0, sc_vs, sizeof(sc_vs) - 1, NULL, "vs_main");
		if (!b || c.index.count != 0)
		{
			printf("FAIL: a reopened cache kept the index of the files it closed\n");
			return 1;
		}
		ID3D10Blob_Release(b);
		gs_d3d11_shader_cache_close(&c);
	}
	if (fk_live() != 0)
	{
		printf("FAIL: %d shaders or layouts never released\n", fk_live());
		return 1;
	}
	if (argc > 1 && strtoull(argv[1], NULL, 16) != h)
	{
		printf("FAIL: the C++ D3D11ShaderCache gave %s\n", argv[1]);
		return 1;
	}
	return 0;
}
