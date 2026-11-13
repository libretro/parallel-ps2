/* The texture format math (GS/Renderers/Common/GSTextureFormat.c) gives
 * what GSTexture's statics and GSDownloadTexture's transfer methods gave,
 * over a sweep of every format and the sizes around block edges (the hash
 * below was taken from them); and the stride copy copies what it should,
 * packed or not. And the GL storage of each format (GLTextureFormat.c),
 * as GSTextureOGL's constructor chose it, flag by flag. */

#include <stdio.h>
#include <string.h>

#include "GS/Renderers/Common/GSTextureFormat.h"
#include "GS/Renderers/OpenGL/GLTextureFormat.h"

#define F_BLOCK_BYTES(f)             gs_texture_block_bytes(f)
#define F_BLOCK_SIZE(f)              gs_texture_block_size(f)
#define F_COMPRESSED(f)              gs_texture_is_compressed(f)
#define F_UPLOAD_PITCH(f, w)         gs_texture_upload_pitch(f, w)
#define F_ROW_LENGTH(f, p)           gs_texture_row_length(f, p)
#define F_UPLOAD_SIZE(f, h, p)       gs_texture_upload_size(f, h, p)
#define F_TRANSFER_PITCH(f, w, a)    gs_texture_transfer_pitch(f, w, a)
#define F_BUFFER_SIZE(f, w, h, a)    gs_texture_buffer_size(f, w, h, a)

static void transfer(unsigned f, u32 pitch, u32 x, u32 y, u32 w, u32 h, u32* off, u32* size, u32* rows)
{
	const gs_texture_transfer t = gs_texture_transfer_of(f, pitch, x, y, w, h);
	*off  = t.offset;
	*size = t.size;
	*rows = t.rows;
}
#define F_TRANSFER(f, p, x, y, w, h, o, s, r) transfer(f, p, x, y, w, h, o, s, r)

#include "sweep.h"

/* GSTexture's and GSDownloadTexture's results over the sweep */
#define EXPECTED 0x954b8182u

static int s_failed;

static void check(int ok, const char* what)
{
	if (!ok)
	{
		printf("FAIL: %s\n", what);
		s_failed = 1;
	}
}

static void stride_copy(void)
{
	unsigned char src[16 * 9], dst[24 * 9], want[24 * 9];
	size_t i, r;
	for (i = 0; i < sizeof(src); i++)
		src[i] = (unsigned char)(i * 7 + 3);

	/* packed alike: one copy of everything */
	memset(dst, 0, sizeof(dst));
	gs_stride_memcpy(dst, 16, src, 16, 16, 9);
	check(!memcmp(dst, src, sizeof(src)), "a packed copy copies every row");

	/* 10 of 16 bytes a row into rows 24 apart: the rest untouched */
	memset(dst, 0xEE, sizeof(dst));
	memset(want, 0xEE, sizeof(want));
	for (r = 0; r < 9; r++)
		memcpy(want + r * 24, src + r * 16, 10);
	gs_stride_memcpy(dst, 24, src, 16, 10, 9);
	check(!memcmp(dst, want, sizeof(dst)), "a strided copy copies each row's part and nothing between");

	/* none */
	memset(dst, 0xEE, sizeof(dst));
	gs_stride_memcpy(dst, 24, src, 16, 10, 0);
	check(dst[0] == 0xEE, "no rows, no copy");
}

/* GSTextureOGL's constructor's switch, as it was */
static const struct
{
	unsigned format;
	int colclip_float, depth_only;
	GLenum internal_format, format_gl, type;
	u32 shift;
} s_gl_expected[] = {
	{GS_TEX_INVALID, 0, 0, 0, 0, 0, 0},
	{GS_TEX_COLOR, 1, 1, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 2},
	{GS_TEX_COLOR_CLIP, 0, 0, GL_RGBA16, GL_RGBA, GL_UNSIGNED_SHORT, 3},
	{GS_TEX_COLOR_CLIP, 1, 0, GL_RGBA32F, GL_RGBA, GL_FLOAT, 4},
	{GS_TEX_DEPTH_STENCIL, 0, 0, GL_DEPTH32F_STENCIL8, GL_DEPTH_STENCIL, GL_FLOAT_32_UNSIGNED_INT_24_8_REV, 3},
	{GS_TEX_DEPTH_STENCIL, 1, 1, GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, GL_FLOAT, 2},
	{GS_TEX_UNORM8, 0, 1, GL_R8, GL_RED, GL_UNSIGNED_BYTE, 0},
	{GS_TEX_UINT16, 1, 0, GL_R16UI, GL_RED_INTEGER, GL_UNSIGNED_SHORT, 1},
	{GS_TEX_UINT32, 0, 0, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, 2},
	{GS_TEX_PRIMID, 0, 0, GL_R32F, GL_RED, GL_INT, 2},
	{GS_TEX_BC1, 0, 0, GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, GL_UNSIGNED_BYTE, 1},
	{GS_TEX_BC2, 0, 0, GL_COMPRESSED_RGBA_S3TC_DXT3_EXT, GL_COMPRESSED_RGBA_S3TC_DXT3_EXT, GL_UNSIGNED_BYTE, 1},
	{GS_TEX_BC3, 1, 1, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, GL_UNSIGNED_BYTE, 1},
	{GS_TEX_BC7, 0, 0, GL_COMPRESSED_RGBA_BPTC_UNORM_ARB, GL_COMPRESSED_RGBA_BPTC_UNORM_ARB, GL_UNSIGNED_BYTE, 1},
	{200, 0, 0, 0, 0, 0, 0},
};

static void gl_formats(void)
{
	size_t i;
	for (i = 0; i < sizeof(s_gl_expected) / sizeof(s_gl_expected[0]); i++)
	{
		const gs_gl_texture_format f = gs_gl_texture_format_of(s_gl_expected[i].format,
			s_gl_expected[i].colclip_float, s_gl_expected[i].depth_only);
		if (f.internal_format != s_gl_expected[i].internal_format || f.format != s_gl_expected[i].format_gl
			|| f.type != s_gl_expected[i].type || f.shift != s_gl_expected[i].shift)
		{
			printf("FAIL: GL storage of format %u (colclip float %d, depth only %d)\n", s_gl_expected[i].format,
				s_gl_expected[i].colclip_float, s_gl_expected[i].depth_only);
			s_failed = 1;
		}
	}
	/* where GL's texel is what the format math says (not ColorClip as
	 * float, nor depth with stencil): the upload pitch agrees */
	for (i = GS_TEX_COLOR; i <= GS_TEX_PRIMID; i++)
	{
		const gs_gl_texture_format f = gs_gl_texture_format_of((unsigned)i, 0, 1);
		if (i != GS_TEX_COLOR_CLIP && (1u << f.shift) != gs_texture_block_bytes((unsigned)i))
		{
			printf("FAIL: format %u's GL texel is not its block\n", (unsigned)i);
			s_failed = 1;
		}
	}
}

int main(int argc, char** argv)
{
	const u32 h = sweep();
	printf("format math %08x\n", (unsigned)h);
	if (argc > 1 && !strcmp(argv[1], "--print"))
		return 0;
	check(h == EXPECTED, "the format math matches GSTexture's");
	check(gs_texture_block_bytes(GS_TEX_FORMATS) == 1 && gs_texture_block_bytes(200) == 1,
		"a format past the table reads as one byte, not past it");
	stride_copy();
	gl_formats();
	if (s_failed)
		return 1;
	printf("gstexfmt: ok\n");
	return 0;
}
