/*  CDVD seek and read timing treats a DVD-Video disc as a DVD.
 *
 *  The callers already pick the DVD rotation and sector rates for it via
 *  cdvdIsDVD(), but the CLV/CAV position curve used the CD's 360000-sector
 *  surface, so past ~700 MB every DVD-Video sector timed as the outer edge
 *  and dual-layer discs lost their layer-1 offset. A DVD-Video disc must
 *  time exactly as a PS2 DVD does at the same sector.
 *
 *  Includes CDVD.c to reach its static timing functions; the build drops
 *  every section the test does not reach, so only the CDVD API pointer
 *  needs a stub.
 */

#include "../../pcsx2/CDVD/CDVD.c"

static u32 s_layer1;
static s32 s_dual;

static s32 CALLBACK stub_getDualInfo(s32* dualType, u32* layer1Start)
{
	*dualType    = s_dual;
	*layer1Start = s_layer1;
	return 1;
}

static CDVD_API s_api;
CDVD_API* CDVD = &s_api;

static int failures;

static void same_as_ps2dvd(const char* name, u8 spindle, u32 sector)
{
	u32 rot_v, rot_p, read_v, read_p;

	cdvd.SpindlCtrl   = spindle;
	cdvd.SeekToSector = sector;
	cdvd.Speed        = 4;

	cdvd.DiscType = CDVD_TYPE_DVDV;
	rot_v  = cdvdRotationTime(MODE_DVDROM);
	read_v = cdvdBlockReadTime(MODE_DVDROM);

	cdvd.DiscType = CDVD_TYPE_PS2DVD;
	rot_p  = cdvdRotationTime(MODE_DVDROM);
	read_p = cdvdBlockReadTime(MODE_DVDROM);

	if (rot_v != rot_p || read_v != read_p)
	{
		printf("  FAIL %-30s DVD-Video rot %u read %u, PS2 DVD rot %u read %u\n",
			name, (unsigned)rot_v, (unsigned)read_v, (unsigned)rot_p, (unsigned)read_p);
		failures++;
	}
}

int main(void)
{
	s_api.getDualInfo = stub_getDualInfo;

	s_dual   = 0;
	s_layer1 = 0xFFFFFFFFu;
	same_as_ps2dvd("CLV single layer, mid disc", 0, 1000000);
	same_as_ps2dvd("CAV single layer, mid disc", CDVD_SPINDLE_CAV, 1000000);

	s_dual   = 1;
	s_layer1 = 2000000;
	same_as_ps2dvd("CLV dual layer, layer 1", 0, 2100000);
	same_as_ps2dvd("CAV dual layer, layer 1", CDVD_SPINDLE_CAV, 2100000);

	if (failures)
	{
		printf("cdvdtime: %d failure(s)\n", failures);
		return 1;
	}
	printf("cdvdtime: all cases pass\n");
	return 0;
}
