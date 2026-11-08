/* The TOOL registers in IOP hardware page 3 as a COH-H (arcade) board's
 * BIOS reads them: config switches at 0x1f803100 with only bit 3 set (no
 * CD-ROM drive), board id 0x7c at 0x1f803204. A console reads the
 * switches all high, as before.
 *
 * Includes IopHwRead.c; the build drops what the reads never reach. */
#include "../../pcsx2/ps2/Iop/IopHwRead.c"

#include <stdio.h>

bool BiosIsArcade;
PCSX2_ALIGN(__pagealignsize) u8 iopHw[PS2MEM_IOP_HARDWARE];

static int failures;

static void check(bool ok, const char* what)
{
	printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok)
		failures++;
}

int main(void)
{
	BiosIsArcade = false;
	check(iopHwRead8_Page3(0x1f803100) == 0xFF, "console: TOOL config switches read all high");

	BiosIsArcade = true;
	check(iopHwRead8_Page3(0x1f803100) == 0x08, "COH-H: TOOL config switches read 0x08, no CD-ROM drive");
	check(iopHwRead8_Page3(0x1f803204) == 0x7c, "COH-H: TOOL board id reads 0x7c");

	printf(failures ? "iopcoh: %d failure(s)\n" : "iopcoh: ok\n", failures);
	return failures != 0;
}
