/* Selectors at random, and what VKPipelineKey.h makes of each, for
 * check.py to hold against the SPIR-V the core is built with. One line a
 * selector:
 *
 *   P <54 constants> | <54 constants of its key> | <reads the target> <its key does> <dropped>
 *   V <expand> <4 constants> | <4 constants of its key>
 *
 * Fails (exit 1) on a key that is not its own key. The selectors are any
 * bits at all: specialization constants take any value, and a key must
 * hold for every one. Half the fields are drawn from 0 more often, so the
 * rules' conditions come out both ways. */

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <vulkan/vulkan_core.h>

#include "GS/Renderers/Common/GSDevice.h"

typedef struct VKSpirv
{
	const u32* words;
	u32 count;
} VKSpirv;
typedef struct VKSpirvNamed
{
	const char* name;
	VKSpirv spv;
} VKSpirvNamed;
#include "VKShaders.inc"
#include "VKPipelineKey.h"

static unsigned long long s_rng;

static u32 rnd(u32 n)
{
	s_rng ^= s_rng << 13;
	s_rng ^= s_rng >> 7;
	s_rng ^= s_rng << 17;
	return (u32)((s_rng >> 11) % n);
}

/* a field of `bits` bits: zero half the time */
static u32 field(u32 bits)
{
	return rnd(2) ? 0u : rnd(1u << bits);
}

static void print(const char* tag, const u32* v, u32 n)
{
	u32 i;
	(void)tag;
	for (i = 0; i < n; i++)
		printf(" %u", v[i]);
}

int main(int argc, char** argv)
{
	const u32 count = (argc > 1) ? (u32)atoi(argv[1]) : 1000;
	u32 i;
	s_rng = (argc > 2) ? strtoull(argv[2], NULL, 0) : 0x9E3779B97F4A7C15ull;
	if (!s_rng)
		s_rng = 1;

	for (i = 0; i < count; i++)
	{
		GSHWDrawConfig::PSSelector ps, key, again;
		u32 a[VKSC_PS_COUNT], b[VKSC_PS_COUNT];

		ps.key_lo = 0;
		ps.key_hi = 0;
		ps.fst = field(1); ps.wms = field(2); ps.wmt = field(2); ps.adjs = field(1); ps.adjt = field(1);
		ps.aem_fmt = field(2); ps.pal_fmt = field(2); ps.dst_fmt = field(2); ps.depth_fmt = field(2);
		ps.channel = field(3); ps.urban_chaos_hle = field(1); ps.tales_of_abyss_hle = field(1);
		ps.aem = field(1); ps.tfx = rnd(3) ? rnd(5) : 4; ps.tcc = field(1); ps.atst = static_cast<GSShader::PS_ATST>(field(3));
		ps.afail = static_cast<GSShader::PS_AFAIL>(field(3)); ps.fog = field(1); ps.blend_hw = field(3); ps.a_masked = field(1);
		ps.fba = field(1); ps.ltf = field(1); ps.automatic_lod = field(1); ps.manual_lod = field(1);
		ps.colclip = field(1); ps.date = field(3); ps.tcoffsethack = field(1); ps.region_rect = field(1);
		ps.blend_a = field(2); ps.blend_b = field(2); ps.blend_c = field(2); ps.blend_d = field(2);
		ps.blend_mix = field(2); ps.round_inv = field(1); ps.fixed_one_a = field(1); ps.shuffle = field(1);
		ps.shuffle_same = field(1); ps.process_ba = field(2); ps.process_rg = field(2);
		ps.shuffle_across = field(1); ps.real16src = field(1); ps.write_rg = field(1); ps.fbmask = field(1);
		ps.colclip_hw = field(1); ps.rta_correction = field(1); ps.rta_source_correction = field(1);
		ps.dither = field(2); ps.dither_adjust = field(1); ps.pabe = field(1); ps.scanmsk = field(2);
		ps.sample_map = field(1); ps.native_taps = field(1); ps.tex_is_fb = field(1); ps.no_color = field(1);
		ps.no_color1 = field(1); ps.iip = field(1); ps.zclamp = field(1);

		key = ps;
		VKCanonicalPS(key);
		again = key;
		VKCanonicalPS(again);
		if (memcmp(&again, &key, sizeof(key)))
		{
			printf("FAIL: selector %u: its key is not its own key\n", i);
			return 1;
		}
		VKTFXSpecPS(ps, a);
		VKTFXSpecPS(key, b);
		printf("P");
		print("", a, VKSC_PS_COUNT);
		printf(" |");
		print("", b, VKSC_PS_COUNT);
		printf(" | %d %d %u\n", VKTFXReadsRT(ps) ? 1 : 0, VKTFXReadsRT(key) ? 1 : 0, VKTFXFragmentDrop(true, key));
	}
	for (i = 0; i < count / 4; i++)
	{
		GSHWDrawConfig::VSSelector vs, key, again;
		const u32 topology = rnd(3);
		u32 a[VKSC_VS_COUNT], b[VKSC_VS_COUNT];

		vs.key = 0;
		vs.fst = field(1); vs.tme = field(1); vs.iip = field(1); vs.point_size = field(1);
		vs.expand = static_cast<GSHWDrawConfig::VSExpand>(rnd(2) ? 0 : rnd(4));
		key = vs;
		VKCanonicalVS(key, topology);
		again = key;
		VKCanonicalVS(again, topology);
		if (again.key != key.key)
		{
			printf("FAIL: vertex selector %u: its key is not its own key\n", i);
			return 1;
		}
		VKTFXSpecVS(vs, a);
		VKTFXSpecVS(key, b);
		/* The point size is the pipeline's to drop, not the shader's: it
		 * is written all the same, and only points drawn as points read
		 * it. So the shaders are compared with the key's, and the key
		 * keeps it for those. */
		a[VKSC_VS_POINT_SIZE] = b[VKSC_VS_POINT_SIZE];
		if (topology == static_cast<u32>(GSHWDrawConfig::Topology::Point) && vs.expand == GSHWDrawConfig::VSExpand::None
			&& vs.point_size && !key.point_size)
		{
			printf("FAIL: vertex selector %u: points lost their size\n", i);
			return 1;
		}
		printf("V %d", vs.expand != GSHWDrawConfig::VSExpand::None ? 1 : 0);
		print("", a, VKSC_VS_COUNT);
		printf(" |");
		print("", b, VKSC_VS_COUNT);
		printf("\n");
	}
	return 0;
}
