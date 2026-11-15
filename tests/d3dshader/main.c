/* D3D.c, the shader compiler's wrapper and the adapter and factory
 * helpers: what the C++ D3D namespace gave (the hash on the command
 * line), scenario.h. Windows only: built with MinGW, run under wine,
 * whose compiler is real enough to give blobs and errors. */
#define COBJMACROS
#include <stdio.h>
#include <stdlib.h>

#include "GS/Renderers/DX11/D3D.h"
#include "fake.h"

retro_log_printf_t log_cb = fk_log;

typedef ID3DBlob* SC_BLOB;
#define COMPILE(t, l, d, c, n, m, e) gs_d3d_compile_shader(t, l, d, c, n, m, e)
#define B_SIZE(b) ID3D10Blob_GetBufferSize(b)
#define B_DATA(b) ID3D10Blob_GetBufferPointer(b)
#define B_FREE(b) ID3D10Blob_Release(b)
#define VENDOR(a) gs_d3d_vendor_id((IDXGIAdapter1*)(a))

static int factory_ok(int debug)
{
	IDXGIFactory5* f = gs_d3d_create_factory(debug);
	if (!f)
		return 0;
	IDXGIFactory5_Release(f);
	return 1;
}
#define FACTORY_OK(d) factory_ok(d)

#include "scenario.h"

int main(int argc, char** argv)
{
	scenario();
	printf("d3dshader: %016llx\n", fk_hash);
	if (argc > 1 && strtoull(argv[1], NULL, 16) != fk_hash)
	{
		printf("FAIL: the C++ D3D namespace gave %s\n", argv[1]);
		return 1;
	}
	return 0;
}
