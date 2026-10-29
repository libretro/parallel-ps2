/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2021  PCSX2 Dev Team
 *
 *  PCSX2 is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU Lesser General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  PCSX2 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with PCSX2.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

/* C89. */
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include <compat/strl.h>
#include <compat/fnmatch.h>
#include <file/file_path.h>

#include "common/Pcsx2Defs.h"
#include "../R3000A.h"
#include "../MemoryTypes.h"
#include "../IopMem.h"
#include "../IopHw.h"
#include "../IopDma.h"
#include "../R5900.h"
#include "../Counters.h"
#include "../Elfheader.h"
#include "../SaveState.h"
#include "../ps2/BiosTools.h"

#include "Ps1CD.h"
#include "CDVD.h"
#include "CDVD_host.h"
#include "CDVD_internal.h"
#include "cdvd_iso.h"
#include "IsoFS/iso9660.h"

/* TODO/FIXME - forward declaration */

/* This typically reflects the Sony-assigned serial code for the Disc, if one exists. */
/*  (examples:  SLUS-2113, etc). */
/* If the disc is homebrew then it probably won't have a valid serial; in which case */
/* this string will be empty. */
/* Bounded: the format is four letters, a separator, five digits. */
char DiscSerial[PCSX2_PATH_MAX];

cdvdStruct cdvd;

s64 PSXCLK = 36864000;

#define NVRAM_SIZE 1024
static u8 s_nvram[NVRAM_SIZE];
/* cdvd_nvram_store compares against a buffer CDVD_NVRAM_STORE_MAX long */
typedef char cdvd_nvram_fits[NVRAM_SIZE <= CDVD_NVRAM_STORE_MAX ? 1 : -1];

#define DEFAULT_MECHA_VERSION 0x00020603
static u32 s_mecha_version = 0;

/* EmuConfig.Speedhacks.fastCDVD, from cdvdApplySettings */
static bool s_fast_cdvd;

void cdvdApplySettings(bool fast_cdvd)
{
	s_fast_cdvd = fast_cdvd;
}

static void CDVDSECTORREADY_INT(u32 eCycle)
{
	if (psxRegs.interrupt & (1 << IopEvt_CdvdSectorReady))
		return;

	if (s_fast_cdvd)
	{
		if (eCycle < Cdvd_FullSeek_Cycles && eCycle > 1)
			eCycle *= 0.05f;
	}

	PSX_INT(IopEvt_CdvdSectorReady, eCycle);
}

static void CDVDREAD_INT(u32 eCycle)
{
	/* Give it an arbitary FAST value. Good for ~5000kb/s in ULE when copying a file from CDVD to HDD */
	/* Keep long seeks out though, as games may try to push dmas while seeking. (Tales of the Abyss) */
	if (s_fast_cdvd)
	{
		if (eCycle < Cdvd_FullSeek_Cycles && eCycle > 1)
			eCycle *= 0.05f;
	}

	PSX_INT(IopEvt_CdvdRead, eCycle);
}

static __fi void cdvdActionInterrupt_inl(void);

static void CDVD_INT(int eCycle)
{
	if (eCycle == 0)
		cdvdActionInterrupt_inl();
	else
		PSX_INT(IopEvt_Cdvd, eCycle);
}

/* Sets the cdvd IRQ and the reason for the IRQ, and signals the IOP for a branch */
/* test (which will cause the exception to be handled). */
static void cdvdSetIrq(uint id)
{
	/* Only assert the IOP interrupt on a rising edge of IntrStat; if the bit
	 * is already set the interrupt line is still pending and re-issuing would
	 * be a spurious double IRQ. */
	if (!(cdvd.IntrStat & id))
	{
		iopIntcIrq(2);
		psxSetNextBranchDelta(20);
	}

	cdvd.IntrStat       |= id;
	cdvd.AbortRequested  = false;
}

static int mg_BIToffset(u8* buffer)
{
	int i, ofs = 0x20;
	const int count = *(u16*)&buffer[0x1A];

	/* Prevent OOB read from unbounded loop on crafted MG data */
	if (count < 0 || count > ((65536 - 0x20) / 0x10))
		return -1;

	for (i = 0; i < count; i++)
		ofs += 0x10;

	/* ofs is used to index the 65536-byte mg_buffer below (buffer[ofs] and */
	/* the returned ofs+0x20). Guard every step so crafted MG data cannot */
	/* push the offset out of bounds. */
	if (*(u16*)&buffer[0x18] & 1)
	{
		if (ofs < 0 || ofs >= 65536)
			return -1;
		ofs += buffer[ofs];
	}
	if ((*(u16*)&buffer[0x18] & 0xF000) == 0)
		ofs += 8;

	if (ofs < 0 || (ofs + 0x20) > 65536)
		return -1;

	return ofs + 0x20;
}

const NVMLayout* getNvmLayout(void)
{
	return (nvmlayouts[1].biosVer <= BiosVersion) ? &nvmlayouts[1] : &nvmlayouts[0];
}

static void cdvdCreateNewNVM(void)
{
	/* Write NVM ILink area with dummy data (Age of Empires 2) */
	/* Also write language data defaulting to English (Guitar Hero 2) */
	/* Also write PStwo region defaults */

	static const u8 ILinkID_Data[8] = {0x00, 0xAC, 0xFF, 0xFF, 0xFF, 0xFF, 0xB9, 0x86};
	static const u8 ILinkID_checksum[2] = {0x00, 0x18};
	const NVMLayout* nvmLayout = getNvmLayout();

	memset(s_nvram, 0, sizeof(s_nvram));
	if (((BiosVersion >> 8) == 2) && ((BiosVersion & 0xff) != 10)) /* bios >= 200, except of 0x210 for PSX2 DESR */
		memcpy(&s_nvram[nvmLayout->regparams], PStwoRegionDefaults[BiosRegion], 12);

	memcpy(&s_nvram[nvmLayout->ilinkId], ILinkID_Data, sizeof(ILinkID_Data));
	if (nvmlayouts[1].biosVer <= BiosVersion)
		memcpy(&s_nvram[nvmLayout->ilinkId + 0x08], ILinkID_checksum, sizeof(ILinkID_checksum));

	/* Config sections first 16 bytes are generally blank expect the last byte which is PS1 mode stuff */
	/* So let's ignore that and just write the PS2 mode stuff */
	memcpy(&s_nvram[nvmLayout->config1 + 0x10], biosLangDefaults[BiosRegion], 16);
}


/* BIOS path with the extension swapped, into a caller buffer. */
static void cdvdBiosSiblingPath(char* out, size_t out_size, const char* ext)
{
	strlcpy(out, BiosPath, out_size);
	path_remove_extension(out);
	strlcat(out, ".", out_size);
	strlcat(out, ext, out_size);
}

void cdvdLoadNVRAM(void)
{
	/* One path buffer, the NVRAM's and then the MEC's. */
	char path[PCSX2_PATH_MAX];
	const char* nvmfile = path;
	const char* mecfile = path;
	RFILE *fp;
	cdvdBiosSiblingPath(path, sizeof(path), "nvm");
	fp = filestream_open(nvmfile, RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);
	if (!fp || filestream_read(fp, s_nvram, sizeof(s_nvram)) != (int64_t)sizeof(s_nvram))
	{
		log_cb(RETRO_LOG_WARN, "Failed to open or read NVRAM: %s\n", nvmfile);
		cdvdCreateNewNVM();
	}
	else
	{
		/* Verify NVRAM is sane. */
		static const u8 zero[16] = {0};
		const NVMLayout* nvmLayout = getNvmLayout();

		log_cb(RETRO_LOG_INFO, "Reading NVRAM file: %s\n", nvmfile);

		if (memcmp(&s_nvram[nvmLayout->config1 + 0x10], zero, 16) == 0 ||
			(((BiosVersion >> 8) == 2) && ((BiosVersion & 0xff) != 10) &&
				(memcmp(&s_nvram[nvmLayout->regparams], zero, 12) == 0)))
		{
			log_cb(RETRO_LOG_WARN, "Language or Region Parameters missing, filling in defaults\n");
			cdvdCreateNewNVM();
		}
	}

	/* The NVRAM handle is done with here, before fp is reused. */
	if (fp)
		filestream_close(fp);

	cdvdBiosSiblingPath(path, sizeof(path), "mec");
	fp = filestream_open(mecfile, RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);
	if (!fp || filestream_read(fp, &s_mecha_version, sizeof(s_mecha_version)) != (int64_t)sizeof(s_mecha_version))
	{
		s_mecha_version = DEFAULT_MECHA_VERSION;
		log_cb(RETRO_LOG_ERROR, "Failed to open or read MEC file at %s, creating default.\n", mecfile);
		/* Closed before the file is replaced: Windows will not rename
		 * over a file that is still open. */
		if (fp)
			filestream_close(fp);
		fp = NULL;
		if (cdvd_nvram_store(mecfile, &s_mecha_version, sizeof(s_mecha_version)) < 0)
			log_cb(RETRO_LOG_ERROR, "Failed to write MEC file. Check your BIOS setup/permission settings.\n");
	}
	if (fp)
		filestream_close(fp);
}

void cdvdSaveNVRAM(void)
{
	char nvmfile[PCSX2_PATH_MAX];
	cdvdBiosSiblingPath(nvmfile, sizeof(nvmfile), "nvm");
	switch (cdvd_nvram_store(nvmfile, s_nvram, NVRAM_SIZE))
	{
		case 0:
			log_cb(RETRO_LOG_INFO, "NVRAM has not changed, not writing to disk.\n");
			break;
		case 1:
			log_cb(RETRO_LOG_INFO, "NVRAM saved to %s.\n", nvmfile);
			break;
		default:
			log_cb(RETRO_LOG_ERROR, "Failed to save NVRAM to %s\n", nvmfile);
			break;
	}
}

static void cdvdReadNVM(u8* dst, int offset, int bytes)
{
	int to_read = bytes;
	if ((offset + bytes) > ((int)(sizeof(s_nvram))))
	{
		log_cb(RETRO_LOG_WARN, "CDVD: Out of bounds NVRAM read: offset=%d, bytes=%d\n", offset, bytes);
		to_read = pcsx2_max_i(((int)(sizeof(s_nvram))) - offset, 0);
		memset(dst + to_read, 0, bytes - to_read);
	}

	if (to_read > 0)
		memcpy(dst, &s_nvram[offset], to_read);
}

static void cdvdWriteNVM(const u8* src, int offset, int bytes)
{
	int to_write = bytes;
	if ((offset + bytes) > ((int)(sizeof(s_nvram))))
	{
		log_cb(RETRO_LOG_WARN, "CDVD: Out of bounds NVRAM write: offset=%d, bytes=%d\n", offset, bytes);
		to_write = pcsx2_max_i(((int)(sizeof(s_nvram))) - offset, 0);
	}

	if (to_write > 0)
		memcpy(&s_nvram[offset], src, to_write);
}

void cdvdReadLanguageParams(u8* config)
{
	cdvdReadNVM(config, getNvmLayout()->config1 + 0xF, 16);
}

static s32 cdvdReadConfig(u8* config)
{
	const NVMLayout* nvmLayout;
	/* make sure its in read mode */
	if (cdvd.CReadWrite != 0)
	{
		config[0] = 0x80;
		memset(&config[1], 0x00, 15);
		return 1;
	}
	/* check if block index is in bounds */
	else if (cdvd.CBlockIndex >= cdvd.CNumBlocks)
		return 1;
	else if (
		   ((cdvd.COffset == 0) && (cdvd.CBlockIndex >= 4))
		|| ((cdvd.COffset == 1) && (cdvd.CBlockIndex >= 2))
		|| ((cdvd.COffset == 2) && (cdvd.CBlockIndex >= 7)))
	{
		memset(config, 0, 16);
		return 0;
	}

	/* get config data */
	nvmLayout = getNvmLayout();
	switch (cdvd.COffset)
	{
		case 0:
			cdvdReadNVM(config, nvmLayout->config0 + ((cdvd.CBlockIndex++) * 16), 16);
			break;
		case 2:
			cdvdReadNVM(config, nvmLayout->config2 + ((cdvd.CBlockIndex++) * 16), 16);
			break;
		default:
			{
				cdvdReadNVM(config, nvmLayout->config1 + (cdvd.CBlockIndex * 16), 16);
				if (cdvd.CBlockIndex == 1 && (NoOSD || host_option_fast_boot()))
				{
					/* HACK: Set the "initialized" flag when fast booting, otherwise some games crash (e.g. Jak 1). */
					config[2] |= 0x80;
				}

				cdvd.CBlockIndex++;
			}
			break;
	}
	return 0;
}

static s32 cdvdWriteConfig(const u8* config)
{
	const NVMLayout* nvmLayout;
	/* make sure its in write mode && the block index is in bounds */
	if ((cdvd.CReadWrite != 1) || (cdvd.CBlockIndex >= cdvd.CNumBlocks))
		return 1;
	else if (
		((cdvd.COffset == 0) && (cdvd.CBlockIndex >= 4)) ||
		((cdvd.COffset == 1) && (cdvd.CBlockIndex >= 2)) ||
		((cdvd.COffset == 2) && (cdvd.CBlockIndex >= 7)))
		return 0;

	/* get config data */
	nvmLayout = getNvmLayout();
	switch (cdvd.COffset)
	{
		case 0:
			cdvdWriteNVM(config, nvmLayout->config0 + ((cdvd.CBlockIndex++) * 16), 16);
			break;
		case 2:
			cdvdWriteNVM(config, nvmLayout->config2 + ((cdvd.CBlockIndex++) * 16), 16);
			break;
		default:
			cdvdWriteNVM(config, nvmLayout->config1 + ((cdvd.CBlockIndex++) * 16), 16);
			break;
	}
	return 0;
}

static void _reloadElfInfo(const char* elfpath)
{
	u32 crc, entry;

	/* Now's a good time to reload the ELF info... */
	if (!strcmp(elfpath, LastELF))
		return;
	if (!elf_identify_path(elfpath, &crc, &entry))
		return;

	ElfCRC   = crc;
	ElfEntry = entry;
	strlcpy(LastELF, elfpath, PCSX2_PATH_MAX);

	log_cb(RETRO_LOG_INFO, "ELF (%s) Game CRC = 0x%08X, EntryPoint = 0x%08X\n", LastELF, ElfCRC, ElfEntry);

	/* Note: Do not load game database info here.  This code is generic and called from */
	/* BIOS key encryption as well as eeloadReplaceOSDSYS.  The first is actually still executing */
	/* BIOS code, and patches and cheats should not be applied yet.  (they are applied when */
	/* eeGameStarting is invoked, which is when the VM starts executing the actual game ELF */
	/* binary). */
}

/* cdrom:\SCES_123.45;1 -> SCES-12345, in place, no allocation.
 *
 * The rules are unchanged: take the text after the last backslash, or
 * failing that after the last colon, or the whole path; drop the version
 * suffix from ';'; require it to match one of the two expected shapes;
 * then remove the dot, turn '_' into '-', and upper-case the rest. An
 * input that does not match yields an empty string, as before. */
static void ExecutablePathToSerial(char* out, size_t out_size, const char* path)
{
	const char* start;
	const char* sep;
	char*       semi;
	char*       r;
	char*       w;

	out[0] = '\0';
	if (!path)
		return;

	sep   = strrchr(path, '\\');
	start = sep ? sep + 1 : NULL;
	if (!start)
	{
		sep   = strrchr(path, ':');
		start = sep ? sep + 1 : path;
	}
	strlcpy(out, start, out_size);

	semi = strchr(out, ';');
	if (semi)
		*semi = '\0';

	if (   rl_fnmatch("????_???.??*", out, 0) != 0
	    && rl_fnmatch("????" "-???.??*", out, 0) != 0) /* split: trigraph */
	{
		out[0] = '\0';
		return;
	}

	for (r = out, w = out; *r; r++)
	{
		if (*r == '.')
			continue;              /* SCES_123.45 -> SCES_12345 */
		if (*r == '_')
			*w++ = '-';
		else
			*w++ = (char)toupper((unsigned char)*r);
	}
	*w = '\0';
}

void cdvdReloadElfInfo(const char* elfoverride)
{
	char      elfpath[PCSX2_PATH_MAX];
	const u32 disc_type = GetPS2ElfName(elfpath, sizeof(elfpath));
	ExecutablePathToSerial(DiscSerial, PCSX2_PATH_MAX, elfpath);
	rcntRefreshBootTiming();

	/* Use the serial from the disc (if any), and the ELF CRC of the override. */
	if (elfoverride && elfoverride[0])
	{
		_reloadElfInfo(elfoverride);
		return;
	}

	/* PCSX2 currently only recognizes *.elf executables in proper PS2 format. */
	/* To support different PSX titles in the console title and for savestates, this code bypasses all the detection, */
	/* simply using the exe name, stripped of problematic characters. */
	if (disc_type == 1)
		return;

	/* Isn't a disc we recognize? */
	if (disc_type == 0)
		return;

	/* Recognized and PS2 (BOOT2).  Good job, user. */
	_reloadElfInfo(elfpath);
}


static void cdvdReadKey(u8 arg0, u16 arg1, u32 arg2, u8* key)
{
	s32 numbers = 0, letters = 0;
	u32 key_0_3;
	u8 key_4, key_14;

	cdvdReloadElfInfo(NULL);

	/* clear key values */
	memset(key, 0, 16);

	if (DiscSerial[0])
	{
		/* convert the number characters to a real 32 bit number */
		int i;
		for (i = 5; i < 10 && DiscSerial[i] >= '0' && DiscSerial[i] <= '9'; i++)
			numbers = numbers * 10 + (DiscSerial[i] - '0'); /* the leading digits, as from_chars */

		/* combine the lower 7 bits of each char */
		/* to make the 4 letters fit into a single u32 */
		letters =         (s32)((DiscSerial[3] & 0x7F) << 0)  |
				  (s32)((DiscSerial[2] & 0x7F) << 7)  |
				  (s32)((DiscSerial[1] & 0x7F) << 14) |
				  (s32)((DiscSerial[0] & 0x7F) << 21);
	}

	/* calculate magic numbers */
	key_0_3 = ((numbers & 0x1FC00) >> 10) | ((0x01FFFFFF & letters) << 7); /* numbers = 7F  letters = FFFFFF80 */
	key_4   = ((numbers & 0x0001F) << 3)  | ((0x0E000000 & letters) >> 25);   /* numbers = F8  letters = 07 */
	key_14  = ((numbers & 0x003E0) >> 2)  | 0x04;                            /* numbers = F8  extra   = 04  unused = 03 */

	/* store key values */
	key[0] = (key_0_3 & 0x000000FF) >> 0;
	key[1] = (key_0_3 & 0x0000FF00) >> 8;
	key[2] = (key_0_3 & 0x00FF0000) >> 16;
	key[3] = (key_0_3 & 0xFF000000) >> 24;
	key[4] = key_4;

	switch (arg2)
	{
		case 75:
			key[14] = key_14;
			key[15] = 0x05;
			break;

			/*      case 3075: */
			/*          key[15] = 0x01; */
			/*          break; */

		case 4246:
			/* 0x0001F2F707 = sector 0x0001F2F7  dec 0x07 */
			key[0] = 0x07;
			key[1] = 0xF7;
			key[2] = 0xF2;
			key[3] = 0x01;
			key[4] = 0x00;
			key[15] = 0x01;
			break;

		default:
			key[15] = 0x01;
			break;
	}
}

static s32 cdvdReadSubQ(s32 lsn, cdvdSubQ* subq)
{
	s32 ret = CDVD->readSubQ(lsn, subq);
	if (ret != -1)
		return ret;
	return 0x80;
}

static void cdvdDetectDisk(void)
{
	cdvd.DiscType = DoCDVDdetectDiskType();

	if (cdvd.DiscType != 0)
	{
		cdvdTD td;
		CDVD->getTD(0, &td);
		cdvd.MaxSector = td.lsn;
	}
}

#define cdvdUpdateStatus(NewStatus) \
{ \
	cdvd.Status        = NewStatus; \
	cdvd.StatusSticky |= NewStatus; \
}

/* We don't really use the MECHA bit but Cold Fear will kick back to the BIOS if it's not set */
#define cdvdUpdateReady(NewReadyStatus) cdvd.Ready = ((NewReadyStatus) | (CDVD_DRIVE_MECHA_INIT | CDVD_DRIVE_DEV9CON))

s32 cdvdCtrlTrayOpen(void)
{
	if (cdvd.Status & CDVD_STATUS_TRAY_OPEN)
		return 0x80;

	/* If we switch using a source change we need to pretend it's a new disc */
	if (CDVDsys_GetSourceType() == CDVD_SOURCE_DISC)
	{
		cdvdNewDiskCB();
	}
	else
	{
		cdvdDetectDisk();
		cdvdUpdateStatus(CDVD_STATUS_TRAY_OPEN);
		cdvdUpdateReady(0);
		cdvd.Spinning = false;
		cdvdSetIrq(1 << Irq_Eject);
	}

	return 0; /* needs to be 0 for success according to homebrew test "CDVD" */
}

s32 cdvdCtrlTrayClose(void)
{
	if (!(cdvd.Status & CDVD_STATUS_TRAY_OPEN))
		return 0x80;

	if (!g_GameStarted && g_SkipBiosHack)
	{
		cdvdUpdateReady(CDVD_DRIVE_READY);
		cdvdUpdateStatus(CDVD_STATUS_PAUSE);
		cdvd.Spinning               = true;
		cdvd.Tray.trayState         = CDVD_DISC_ENGAGED;
		cdvd.Tray.cdvdActionSeconds = 0;
	}
	else
	{
		cdvdUpdateReady(CDVD_DRIVE_BUSY);
		cdvdUpdateStatus(CDVD_STATUS_STOP);
		cdvd.Spinning               = false;
		cdvd.Tray.trayState         = CDVD_DISC_DETECTING;
		cdvd.Tray.cdvdActionSeconds = 3;
	}
	cdvdDetectDisk();

	return 0; /* needs to be 0 for success according to homebrew test "CDVD" */
}

static bool cdvdIsDVD(void)
{
	if (               cdvd.DiscType == CDVD_TYPE_DETCTDVDS 
			|| cdvd.DiscType == CDVD_TYPE_DETCTDVDD 
			|| cdvd.DiscType == CDVD_TYPE_PS2DVD 
			|| cdvd.DiscType == CDVD_TYPE_DVDV)
		return true;
	return false;
}

static int cdvdTrayStateDetecting(void)
{
	if (cdvd.Tray.trayState != CDVD_DISC_DETECTING)
	{
		if (cdvdIsDVD())
		{
			u32 layer1Start = 0;
			s32 dualType    = 0;
			CDVD->getDualInfo(&dualType, &layer1Start);

			if (dualType > 0)
				return CDVD_TYPE_DETCTDVDD;
			return CDVD_TYPE_DETCTDVDS;
		}

		if (cdvd.DiscType != CDVD_TYPE_NODISC)
			return CDVD_TYPE_DETCTCD;
	}
	return CDVD_TYPE_DETCT; /* Detecting any kind of disc existing */
}

/* Where the head sits across the disc surface, in tenths of a permille */
/* (0 = innermost, 6000 = outermost), which is the position term both */
/* latency curves are built from. */
/* */
/* SeekToSector is copied straight out of the guest's N-command parameter */
/* buffer, so it is bounded by nothing: games read past the end of the */
/* disc, and reads are issued while DiscType is still one of the */
/* transient detection values, where numSectors is a CD's 360000 rather */
/* than a DVD's 2298496 and every DVD-sized sector is off the end of the */
/* modelled surface.  The float curve this replaced turned those into a */
/* negative speed factor and a negative cycle count, which the u32 cast */
/* made meaningless but harmless.  The integer form divides by the */
/* factor, so an unclamped position is an unsigned wrap and then a */
/* division by zero - a SIGFPE the float version could not produce. */
/* Clamped to the surface, which is the only domain the curve was ever */
/* defined on. */
static u64 cdvdSurfacePosPtm(u32 numSectors, u32 offset)
{
	const u64 pos = (u64)(cdvd.SeekToSector - offset) * 6000ULL / numSectors;
	return (pos > 6000ULL) ? 6000ULL : pos;
}

/* Drive speed as a divisor.  Set to 1/2/4/12/24 by every N command that */
/* starts a transfer and to 4 by reset, so zero is not reachable today; */
/* taken as a floor of 1 anyway, so that no path through these two */
/* functions can divide by zero regardless of what sets it later. */
static u64 cdvdDriveSpeed(void)
{
	return cdvd.Speed ? (u64)cdvd.Speed : 1ULL;
}

static u32 cdvdRotationTime(CDVD_MODE_TYPE mode)
{
	/* All-integer: these cycle counts are scheduled into the emulated */
	/* event system, so they must not depend on host FP behavior (the */
	/* mips platform branch builds with -ffast-math).  The CLV */
	/* position-dependent speed factor is carried in exact tenths of a */
	/* permille (0.40..1.40 -> 4000..14000), the drive speed caps in */
	/* exact tenths (10.3x/1.6x -> 103/16), and each result is one u64 */
	/* division.  Verified against the old float math across the full */
	/* disc surface for every speed: within 0.013% - noise against a */
	/* latency model whose rate constants are themselves estimates. */
	const u64 rot = (mode == MODE_CDROM) ? CD_MAX_ROTATION_X1 : DVD_MAX_ROTATION_X1;
	u32 layer1Start = 0;
	s32 dualType    = 0;
	u32 numSectors  = 360000; /* Pretty much every CD format */
	u32 offset      = 0;
	u64 ss_ptm, cap_x10, speed_x10;

	/* CAV rotation is constant (minimum speed to maintain exact speed on outer edge) */
	if (cdvd.SpindlCtrl & CDVD_SPINDLE_CAV)
	{
		/* cycles for one rotation = PSXCLK * 60s / (RPM * speed) */
		return (u32)((u64)PSXCLK * 60ULL / (rot * cdvdDriveSpeed()));
	}

	/* CLV adjusts its speed based on where it is on the disc, so we can take the max RPM and use the sector to work it out */
	/* Sector counts are taken from google for Single layer, Dual layer DVD's and for 700MB CD's */
	switch (cdvd.DiscType)
	{
		case CDVD_TYPE_DETCTDVDS:
		case CDVD_TYPE_PS2DVD:
		case CDVD_TYPE_DETCTDVDD:
			numSectors      = 2298496;
			/* Layer 1 needs an offset as it goes back to the middle of the disc */
			CDVD->getDualInfo(&dualType, &layer1Start);
			if (cdvd.SeekToSector >= layer1Start)
				offset = layer1Start;
			break;
		default: /* Pretty much every CD format */
			break;
	}

	/* CLV speeds are reversed, so the centre is the fastest position. */
	/* sectorSpeed = 1.40 - 0.60 * position, in tenths of a permille, */
	/* so 8000..14000 across the surface and never zero. */
	ss_ptm    = 14000ULL - cdvdSurfacePosPtm(numSectors, offset);
	cap_x10   = (mode == MODE_CDROM) ? 103ULL : 16ULL;
	speed_x10 = pcsx2_min_u64(cdvdDriveSpeed() * 10ULL, cap_x10);
	return (u32)((u64)PSXCLK * 60ULL * 10ULL * 10000ULL / (rot * speed_x10 * ss_ptm));
}

static uint cdvdBlockReadTime(CDVD_MODE_TYPE mode)
{
	const u64 sps = (mode == MODE_CDROM) ? CD_SECTORS_PERSECOND : DVD_SECTORS_PERSECOND;
	u64 cap_x10, speed_x10;

	/* CAV Read speed is roughly 41% in the centre full speed on outer edge. I imagine it's more logarithmic than this */
	if (cdvd.SpindlCtrl & CDVD_SPINDLE_CAV)
	{
		u32 numSectors = 360000; /* Pretty much every CD format */
		u32 offset = 0;
		u64 ss_ptm;

		/* Sector counts are taken from google for Single layer, Dual layer DVD's and for 700MB CD's */
		if (       (cdvd.DiscType == CDVD_TYPE_DETCTDVDS)
			|| (cdvd.DiscType == CDVD_TYPE_PS2DVD)
			|| (cdvd.DiscType == CDVD_TYPE_DETCTDVDD))
		{
			u32 layer1Start  = 0;
			s32 dualType     = 0;
			numSectors       = 2298496;
			/* Layer 1 needs an offset as it goes back to the middle of the disc */
			CDVD->getDualInfo(&dualType, &layer1Start);
			if (cdvd.SeekToSector >= layer1Start)
				offset = layer1Start;
		}

		/* sectorSpeed = 0.40 + 0.60 * position, in tenths of a permille */
		/* (0.40 is the "base" inner track speed), so 4000..10000. */
		ss_ptm = 4000ULL + cdvdSurfacePosPtm(numSectors, offset);
		return (u32)((u64)PSXCLK * 10000ULL / (sps * cdvdDriveSpeed() * ss_ptm));
	}

	/* CLV Read Speed is constant */
	cap_x10   = (mode == MODE_CDROM) ? 103ULL : 16ULL;
	speed_x10 = pcsx2_min_u64(cdvdDriveSpeed() * 10ULL, cap_x10);
	return (u32)((u64)PSXCLK * 10ULL / (sps * speed_x10));
}

void cdvdReset(void)
{
	memset(&cdvd, 0, sizeof(cdvd));

	cdvd.DiscType = CDVD_TYPE_NODISC;
	cdvd.Spinning = false;

	cdvd.sDataIn = 0x40;
	cdvdUpdateReady(CDVD_DRIVE_READY);
	cdvdUpdateStatus(CDVD_STATUS_TRAY_OPEN);
	cdvd.Speed = 4;
	cdvd.BlockSize = 2064;
	cdvd.Action = cdvdAction_None;
	cdvd.ReadTime = cdvdBlockReadTime(MODE_DVDROM);
	cdvd.RotSpeed = cdvdRotationTime(MODE_DVDROM);

	{
		/* CDVD internally uses GMT+9.  If you think the time's wrong, you're wrong. */
		/* Set up your time zone and winter/summer in the BIOS.  No PS2 BIOS I know of features automatic DST. */
		const time_t utc_time  = time(NULL);
		const time_t gmt9_time = (utc_time + (60 * 60 * 9));
		struct tm curtime;
		memset(&curtime, 0, sizeof(curtime));
#ifdef _WIN32
		gmtime_s(&curtime, &gmt9_time);
#else
		gmtime_r(&gmt9_time, &curtime);
#endif
		cdvd.RTC.second = (u8)curtime.tm_sec;
		cdvd.RTC.minute = (u8)curtime.tm_min;
		cdvd.RTC.hour = (u8)curtime.tm_hour;
		cdvd.RTC.day = (u8)curtime.tm_mday;
		cdvd.RTC.month = (u8)curtime.tm_mon + 1; /* WX returns Jan as "0" */
		cdvd.RTC.year = (u8)(curtime.tm_year - 100); /* offset from 2000 */
	}

	g_GameStarted = false;
	g_GameLoading = false;
	/* g_SkipBiosHack is set by psxHwReset, before this runs */

	cdvdCtrlTrayClose();
}

bool cdvdFreeze(SaveStateBase *s)
{
	if (!(SaveState_FreezeTag(s, "cdvd")))
		return false;

	SaveState_Freeze(s, cdvd);
	if (!SaveState_IsOkay(s))
		return false;

	if (SaveState_IsLoading(s))
	{
		/* mg_size and mg_maxsize bound each other in sceMgWriteData --
		 * "if (mg_size + SCMDParamCnt > mg_maxsize)" -- so with both
		 * taken from the state the check is self-consistent and decides
		 * nothing. mg_size is also the length of the move in
		 * sceMgReadData. Neither can exceed the buffer they describe. */
		if (cdvd.mg_maxsize < 0 || cdvd.mg_maxsize > (int)sizeof(cdvd.mg_buffer))
			cdvd.mg_maxsize = (int)sizeof(cdvd.mg_buffer);
		if (cdvd.mg_size < 0 || cdvd.mg_size > cdvd.mg_maxsize)
			cdvd.mg_size = 0;

		/* The result cursor walks a 16-byte buffer. Every runtime
		 * assignment to the count is a literal 1..16, so the bound holds
		 * by construction and is never enforced -- and what follows
		 * SCMDResultBuff in this struct is the CDVD key material, handed
		 * back a byte at a time through the register read. */
		if (cdvd.SCMDResultCnt > sizeof(cdvd.SCMDResultBuff))
			cdvd.SCMDResultCnt = 0;
		if (cdvd.SCMDResultPos > cdvd.SCMDResultCnt)
			cdvd.SCMDResultPos = cdvd.SCMDResultCnt;
		if (cdvd.SCMDParamCnt > sizeof(cdvd.SCMDParamBuff))
			cdvd.SCMDParamCnt = 0;
		if (cdvd.SCMDParamPos > cdvd.SCMDParamCnt)
			cdvd.SCMDParamPos = cdvd.SCMDParamCnt;

		/* Make sure the Cdvd source has the expected track loaded into the buffer. */
		/* If cdvd.SeekCompleted is cleared it means we need to load the SeekToSector (ie, a */
		/* seek is in progress!) */

		if (cdvd.Reading)
			cdvd.ReadErr = DoCDVDreadTrack(cdvd.SeekCompleted ? cdvd.CurrentSector : cdvd.SeekToSector, cdvd.ReadMode);
	}

	return true;
}

void cdvdNewDiskCB(void)
{
	DoCDVDresetDiskTypeCache();
	cdvdDetectDisk();

	/* If not ejected but we've swapped source pretend it got ejected */
	if ((g_GameStarted || !g_SkipBiosHack) && cdvd.Tray.trayState != CDVD_DISC_EJECT)
	{
		cdvdUpdateStatus(CDVD_STATUS_TRAY_OPEN);
		cdvdUpdateReady(CDVD_DRIVE_BUSY);
		cdvd.Tray.trayState = CDVD_DISC_EJECT;
		cdvd.Spinning = false;
		cdvdSetIrq(1 << Irq_Eject);
		/* If it really got ejected, the DVD Reader will report Type 0, so no need to simulate ejection */
		if (cdvd.DiscType > 0)
			cdvd.Tray.cdvdActionSeconds = 3;
	}
	else if (cdvd.DiscType > 0)
	{
		cdvdUpdateReady(CDVD_DRIVE_BUSY);
		cdvdUpdateStatus(CDVD_STATUS_SEEK);
		cdvd.Spinning = true;
		cdvd.Tray.trayState = CDVD_DISC_DETECTING;
		cdvd.Tray.cdvdActionSeconds = 3;
	}
}

static void mechaDecryptBytes(u32 madr, int size)
{
	int shiftAmount = (cdvd.decSet >> 4) & 7;
	int doXor = (cdvd.decSet) & 1;
	int doShift = (cdvd.decSet) & 2;
	int i;

	u8* curval = &iopMem->Main[madr & 0x1fffff];
	for (i = 0; i < size; ++i, ++curval)
	{
		if (doXor)
			*curval ^= cdvd.Key[4];
		if (doShift)
			*curval = (*curval >> shiftAmount) | (*curval << (8 - shiftAmount));
	}
}

/* Whether DMA3 takes a whole block now. */
static bool cdvdDmaReady(void)
{
	const s32 bcr = (HW_DMA3_BCR_H16 * HW_DMA3_BCR_L16) * 4;
	return bcr >= cdvd.BlockSize && (HW_DMA3_CHCR & 0x01000000);
}

/* Where the sector's data begins in a block, and how many bytes of it the
 * drive's read mode yields; the rest of the block is the drive's to fill. */
static u32 cdvdBlockDataOffset(void)
{
	return cdvd.BlockSize == 2064 ? 12 : 0;
}

static u32 cdvdModeBytes(int mode)
{
	switch (mode)
	{
		case CDVD_MODE_2352:
		case CDVD_MODE_2368: return 2352; /* no image carries subchannel Q */
		case CDVD_MODE_2340: return 2340;
		case CDVD_MODE_2328: return 2328;
		case CDVD_MODE_2048: return 2048;
		default:             return 0;
	}
}

/* The block of the sector in src goes to IOP memory through DMA3; src is
 * NULL when the sector's data is there already (cdvdReadSectorFetch). */
static int cdvdReadSector(const u8* src)
{
	u8* mdest;

	if (!cdvdDmaReady())
	{
		if (HW_DMA3_CHCR & 0x01000000)
		{
			HW_DMA3_CHCR &= ~0x01000000;
			psxDmaInterrupt(3);
		}
		return -1;
	}

	/* DMAs use physical addresses (air) */
	mdest = &iopMem->Main[HW_DMA3_MADR & 0x1fffff];

	/* if raw dvd sector 'fill in the blanks' */
	if (cdvd.BlockSize == 2064)
	{
		/* get info on dvd type and layer1 start */
		s32 layerNum    = 0;
		u32 layer1Start = 0;
		s32 dualType    = 0;
		u32 lsn         = cdvd.CurrentSector;

		CDVD->getDualInfo(&dualType, &layer1Start);

		if ((dualType == 1) && (lsn >= layer1Start))
		{
			/* dual layer ptp disc */
			layerNum = 1;
			lsn = lsn - layer1Start + 0x30000;
		}
		else if ((dualType == 2) && (lsn >= layer1Start))
		{
			/* dual layer otp disc */
			layerNum = 1;
			lsn = ~(layer1Start + 0x30000 - 1);
		}
		else
		{
			/* Assuming the other dualType is 0, */
			/* single layer disc, or on first layer of dual layer disc. */
			layerNum = 0;
			lsn += 0x30000;
		}

		mdest[0] = 0x20 | layerNum;
		mdest[1] = (u8)(lsn >> 16);
		mdest[2] = (u8)(lsn >> 8);
		mdest[3] = (u8)(lsn);

		/* sector IED (not calculated at present) */
		mdest[4] = 0;
		mdest[5] = 0;

		/* sector CPR_MAI (not calculated at present) */
		mdest[6] = 0;
		mdest[7] = 0;
		mdest[8] = 0;
		mdest[9] = 0;
		mdest[10] = 0;
		mdest[11] = 0;

		/* normal 2048 bytes of sector data */
		if (src)
			memcpy(&mdest[12], src, 2048);

		/* 4 bytes of edc (not calculated at present) */
		mdest[2060] = 0;
		mdest[2061] = 0;
		mdest[2062] = 0;
		mdest[2063] = 0;
	}
	else if (cdvd.BlockSize == 2368)
	{
		/* 2352 bytes of sector and 16 of subchannel Q, which no image
		 * carries: the Q bytes are zero. */
		if (src)
			memcpy(mdest, src, 2352);
		memset(&mdest[2352], 0, 16);
	}
	else if (src)
		memcpy(mdest, src, cdvd.BlockSize);

	/* decrypt sector's bytes */
	if (cdvd.decSet)
		mechaDecryptBytes(HW_DMA3_MADR, cdvd.BlockSize);

	/* Added a clear after memory write .. never seemed to be necessary before but *should* */
	/* be more correct. (air) */
	psxCpu->Clear(HW_DMA3_MADR, cdvd.BlockSize / 4);

	HW_DMA3_BCR_H16 -= (cdvd.BlockSize / (HW_DMA3_BCR_L16 * 4));
	HW_DMA3_MADR += cdvd.BlockSize;

	if (!HW_DMA3_BCR_H16)
	{
		if (HW_DMA3_CHCR & 0x01000000)
		{
			HW_DMA3_CHCR &= ~0x01000000;
			psxDmaInterrupt(3);
		}
	}

	return 0;
}

static __fi void cdvdActionInterrupt_inl(void)
{
	u8 ready_status = CDVD_DRIVE_READY;
	if (cdvd.AbortRequested)
	{
		cdvd.Error = 0x1; /* Abort Error */
		ready_status |= CDVD_DRIVE_ERROR;
		cdvdUpdateReady(ready_status);
		cdvdUpdateStatus(CDVD_STATUS_PAUSE);
		cdvd.WaitingDMA = false;
		cdvd.nextSectorsBuffered = 0;
		psxRegs.interrupt &= ~(1 << IopEvt_CdvdSectorReady);
		psxRegs.interrupt &= ~(1 << IopEvt_Cdvd); /* Stop any current reads */
	}

	switch (cdvd.Action)
	{
		case cdvdAction_Seek:
			cdvd.Spinning = true;
			cdvdUpdateReady(ready_status);
			cdvd.CurrentSector = cdvd.SeekToSector;
			cdvdUpdateStatus(CDVD_STATUS_PAUSE);
			CDVDSECTORREADY_INT(cdvd.ReadTime);
			break;

		case cdvdAction_Standby:
			cdvd.Spinning = true; /* check (rama) */
			cdvdUpdateReady(ready_status);
			cdvd.CurrentSector = cdvd.SeekToSector;
			cdvdUpdateStatus(CDVD_STATUS_PAUSE);
			cdvd.nextSectorsBuffered = 0;
			CDVDSECTORREADY_INT(cdvd.ReadTime);
			break;

		case cdvdAction_Stop:
			cdvd.Spinning = false;
			cdvdUpdateReady(ready_status);
			cdvd.CurrentSector = 0;
			cdvdUpdateStatus(CDVD_STATUS_STOP);
			break;

		case cdvdAction_Error:
			cdvdUpdateReady(CDVD_DRIVE_READY | CDVD_DRIVE_ERROR);
			cdvdUpdateStatus(CDVD_STATUS_PAUSE);
			break;
	}
	
	cdvd.Action = cdvdAction_None;
	cdvdSetIrq((1 << Irq_CommandComplete));
}

void cdvdActionInterrupt(void)
{
	cdvdActionInterrupt_inl();
}

void cdvdSectorReady(void)
{
	if (cdvd.nextSectorsBuffered < 16)
		cdvd.nextSectorsBuffered++;

	if (cdvd.nextSectorsBuffered < 16)
		CDVDSECTORREADY_INT(cdvd.ReadTime);
	else if (!cdvd.Reading)
		cdvdUpdateStatus(CDVD_STATUS_PAUSE);
}

void cdvdReadInterrupt(void)
{
	bool direct = false; /* the sector went straight to IOP memory */

	/* A read command whose last sector has already been transferred only owes */
	/* the guest its completion interrupt.  Raise it on its own, slightly after */
	/* the DMA finished, and leave every hardware flag in the finished state. */
	/* Launch-era cdvdman watches the DMA counters to track transfer progress */
	/* and only treats the command-complete interrupt as final once the data */
	/* has visibly arrived.  On hardware the DMA always ends comfortably ahead */
	/* of the completion interrupt; delivering both in the same instant lets a */
	/* thread that sampled the counters mid-poll conclude that nothing was */
	/* transferred, and the driver never issues another command. */
	if (cdvd.Reading && cdvd.SectorCnt <= 0)
	{
		cdvd.Reading = 0;
		/* Setting the data ready flag fixes a black screen loading issue in */
		/* Street Fighter EX3 (NTSC-J version). */
		cdvdSetIrq((1 << Irq_CommandComplete));
		cdvdUpdateReady(CDVD_DRIVE_READY);
		if (cdvd.nextSectorsBuffered < 16)
		{
			cdvdUpdateStatus(CDVD_STATUS_READ);
		}
		else
		{
			cdvdUpdateStatus(CDVD_STATUS_PAUSE);
		}
		return;
	}
	cdvdUpdateReady(CDVD_DRIVE_BUSY);
	cdvdUpdateStatus(CDVD_STATUS_READ);
	cdvd.WaitingDMA = false;

	if (!cdvd.SeekCompleted)
	{
		/* Seeking finished.  Process the track we requested before, and */
		/* then schedule another CDVD read int for when the block read finishes. */

		/* NOTE: The first CD track was read when the seek was initiated, so no need */
		/* to call CDVDReadTrack here. */

		cdvd.Spinning        = true;
		cdvd.CurrentRetryCnt = 0;
		cdvd.Reading         = 1;
		cdvd.SeekCompleted   = 1;
		cdvd.CurrentSector   = cdvd.SeekToSector;
	}

	if (cdvd.AbortRequested)
	{
		/* Code in the CDVD controller suggest there is an alignment thing with DVD's but this seems to just break stuff (Auto Modellista). */
		/* Needs more investigation */
		/* if (!cdvdIsDVD() || !(cdvd.CurrentSector & 0xF)) */
		{
			log_cb(RETRO_LOG_WARN, "Read Abort\n");
			cdvd.Error = 0x1; /* Abort Error */
			cdvdUpdateReady(CDVD_DRIVE_READY | CDVD_DRIVE_ERROR);
			cdvdUpdateStatus(CDVD_STATUS_PAUSE);
			cdvd.WaitingDMA = false;
			cdvd.nextSectorsBuffered = 0;
			psxRegs.interrupt &= ~(1 << IopEvt_CdvdSectorReady);
			cdvdSetIrq((1 << Irq_CommandComplete));
			return;
		}
	}

	if (cdvd.CurrentSector >= cdvd.MaxSector)
	{
		cdvd.Error = 0x32; /* Outermost track reached during playback */
		cdvdUpdateReady(CDVD_DRIVE_READY | CDVD_DRIVE_ERROR);
		cdvdUpdateStatus(CDVD_STATUS_PAUSE);
		cdvd.WaitingDMA = false;
		cdvdSetIrq((1 << Irq_CommandComplete));
		return;
	}

	if (cdvd.Reading)
	{
		if (cdvd.ReadErr == 0)
		{
			/* A block the DMA takes at once is fetched where it goes;
			 * one it does not yet waits in cdr.Transfer for a later
			 * try. */
			u8* dest = cdr.Transfer;
			if (cdvd.SectorCnt > 0 && cdvd.nextSectorsBuffered && cdvdDmaReady()
					&& cdvdBlockDataOffset() + cdvdModeBytes(cdvd.ReadMode) <= (u32)cdvd.BlockSize)
				dest = &iopMem->Main[HW_DMA3_MADR & 0x1fffff] + cdvdBlockDataOffset();
			while ((cdvd.ReadErr = DoCDVDgetBuffer(dest)), cdvd.ReadErr == -2) { }
			direct = cdvd.ReadErr == 0 && dest != cdr.Transfer;
		}

		if (cdvd.ReadErr == -1)
		{
			cdvd.CurrentRetryCnt++;

			if (cdvd.CurrentRetryCnt <= cdvd.RetryCntMax)
			{
				cdvd.ReadErr = DoCDVDreadTrack(cdvd.CurrentSector, cdvd.ReadMode);
				CDVDREAD_INT(cdvd.ReadTime);
			}

			return;
		}

		cdvd.Reading = false;
	}

	if (cdvd.SectorCnt > 0 && cdvd.nextSectorsBuffered)
	{
		if (cdvdReadSector(direct ? NULL : cdr.Transfer) == -1)
		{
			/* This means that the BCR/DMA hasn't finished yet, and rather than fire off the */
			/* sector-finished notice too early (which might overwrite game data) we delay a */
			/* bit and try to read the sector again later. */
			/* An arbitrary delay of some number of cycles probably makes more sense here, */
			/* but for now it's based on the cdvd.ReadTime value. -- air */
			cdvdUpdateStatus(CDVD_STATUS_PAUSE);
			cdvd.WaitingDMA = true;
			return;
		}

		cdvd.nextSectorsBuffered--;
		CDVDSECTORREADY_INT(cdvd.ReadTime);

		cdvd.CurrentSector++;
		cdvd.SeekToSector++;

		if (--cdvd.SectorCnt <= 0)
		{
			/* The sector data has just landed via DMA.  Let the completion */
			/* interrupt arrive on its own a moment later, the way the real */
			/* drive orders things (see the note at the top of this function). */
			CDVDREAD_INT(1024);
			/* Timing issues on command end */
			/* Star Ocean (1.1 Japan) expects the DMA to end and interrupt at least 128 or more cycles before the CDVD command ends. */
			/* However the time required seems to increase slowly, so delaying the end of the command is not the solution. */
			/* cdvd.Status = CDVD_STATUS_PAUSE; // Needed here but could be smth else than Pause (rama) */
			/* All done! :D */
			return;
		}
	}
	else
	{
		if (cdvd.SectorCnt <= 0)
		{
			cdvdSetIrq((1 << Irq_CommandComplete));

			cdvdUpdateReady(CDVD_DRIVE_READY);
			cdvdUpdateStatus(CDVD_STATUS_PAUSE);
			return;
		}
		if (cdvd.nextSectorsBuffered)
			CDVDREAD_INT((cdvd.BlockSize / 4) * 12);
		else
			CDVDREAD_INT(psxRemainingCycles(IopEvt_CdvdSectorReady) + ((cdvd.BlockSize / 4) * 12));
		return;
	}

	cdvd.CurrentRetryCnt = 0;
	cdvd.Reading = 1;
	cdvd.ReadErr = DoCDVDreadTrack(cdvd.CurrentSector, cdvd.ReadMode);
	if (cdvd.nextSectorsBuffered)
		CDVDREAD_INT((cdvd.BlockSize / 4) * 12);
	else
		CDVDREAD_INT(psxRemainingCycles(IopEvt_CdvdSectorReady) + ((cdvd.BlockSize / 4) * 12));
}

/* Returns the number of IOP cycles until the event completes. */
static uint cdvdStartSeek(uint newsector, CDVD_MODE_TYPE mode, bool transition_to_CLV)
{
	uint delta;
	uint seektime = 0;
	bool isSeeking = false;
	int drive_speed_change_cycles = 0;
	int old_rotspeed;

	cdvd.SeekToSector = newsector;
	delta = abs(((s32)(cdvd.SeekToSector - cdvd.CurrentSector)));

	cdvdUpdateReady(CDVD_DRIVE_BUSY);
	cdvd.Reading = 1;
	cdvd.SeekCompleted = 0;
	/* Okay so let's explain this, since people keep messing with it in the past and just poking it. */
	/* So when the drive is spinning, bit 0x2 is set on the Status, and bit 0x8 is set when the drive is not reading. */
	/* So In the case where it's seeking to data it will be Spinning (0x2) not reading (0x8) and Seeking (0x10, but because seeking is also spinning 0x2 is also set)) */
	/* Update - Apparently all that was rubbish and some games don't like it. WRC was the one in this scenario which hated SEEK |ZPAUSE, so just putting it back to pause for now. */
	/* We should really run some tests for this behaviour. */
	old_rotspeed = cdvd.RotSpeed;
	cdvd.RotSpeed = cdvdRotationTime(mode);

	cdvd.ReadTime = cdvdBlockReadTime(mode);

	if (cdvd.Spinning && transition_to_CLV)
	{
		/* A rough cycles-per-RPM-change cost based on 333ms for a full */
		/* spin up.  The old float constant 0.054950495049505 is exactly */
		/* the rational 333/6060 (= 111/2020); with rpm = PSXCLK*60 / */
		/* rotation_cycles this reduces to one exact u64 expression. */
		/* A rotation time of zero is not reachable now that the position */
		/* factor is clamped, but these two divisions are the second half */
		/* of the same trap and cost nothing to close. */
		const u64 old_rpm = old_rotspeed ? ((u64)PSXCLK * 60ULL / (u64)old_rotspeed) : 0ULL;
		const u64 new_rpm = cdvd.RotSpeed ? ((u64)PSXCLK * 60ULL / (u64)cdvd.RotSpeed) : 0ULL;
		const u64 d_rpm   = (new_rpm > old_rpm) ? (new_rpm - old_rpm) : (old_rpm - new_rpm);
		drive_speed_change_cycles = (int)((u64)PSXCLK * d_rpm * 111ULL / 2020000ULL);
		cdvd.nextSectorsBuffered = 0;
		psxRegs.interrupt &= ~(1 << IopEvt_CdvdSectorReady);
	}
	cdvdUpdateStatus(CDVD_STATUS_SEEK);

	if (!cdvd.Spinning)
	{
		seektime = PSXCLK / 3; /* 333ms delay */
		cdvd.Spinning = true;
		cdvd.nextSectorsBuffered = 0;
		CDVDSECTORREADY_INT(seektime + cdvd.ReadTime);
	}
	else if ((tbl_ContigiousSeekDelta[mode] == 0) || (delta >= tbl_ContigiousSeekDelta[mode]))
	{
		/* Select either Full or Fast seek depending on delta: */
		cdvd.nextSectorsBuffered = 0;
		psxRegs.interrupt &= ~(1 << IopEvt_CdvdSectorReady);

		if (delta >= tbl_FastSeekDelta[mode]) /* Full Seek */
			seektime = Cdvd_FullSeek_Cycles;
		else
			seektime = Cdvd_FastSeek_Cycles;
		isSeeking = true;
	}
	else if(!drive_speed_change_cycles)
	{
		/* if delta > 0 it will read a new sector so the readInterrupt will account for this. */
		
		isSeeking = false;

		if (cdvd.Action != cdvdAction_Seek)
		{
			if (delta == 0)
			{
				/* cdvd.Status = CDVD_STATUS_PAUSE; */
				cdvdUpdateStatus(CDVD_STATUS_READ);
				cdvd.SeekCompleted = 1; /* Note: 1, not 0, as implied by the next comment. Need to look into this. --arcum42 */
				cdvd.Reading = 1; /* We don't need to wait for it to read a sector as it's already queued up, or we adjust for it here. */
				cdvd.CurrentRetryCnt = 0;

				/* setting SeekCompleted to 0 skips the seek logic, which means the next call to */
				/* cdvdReadInterrupt will load a block.  So make sure it's properly scheduled */
				/* based on sector read speeds: */

				if (!cdvd.nextSectorsBuffered)/* Buffering time hasn't completed yet so cancel it and simulate the remaining time */
				{
					if (psxRegs.interrupt & (1 << IopEvt_CdvdSectorReady))
						seektime = psxRemainingCycles(IopEvt_CdvdSectorReady) + ((cdvd.BlockSize / 4) * 12);
					else
						delta = 1; /* Forces it to use the rotational delay since we have no sectors buffered and it isn't buffering any. */
				}
				else
					return (cdvd.BlockSize / 4) * 12;
			}
			else
			{
				if (delta >= cdvd.nextSectorsBuffered)
				{
					cdvd.nextSectorsBuffered = 0;
					psxRegs.interrupt &= ~(1 << IopEvt_CdvdSectorReady);
				}
				else
					cdvd.nextSectorsBuffered -= delta;
			}
		}
	}

	seektime += drive_speed_change_cycles;

	/* Only do this on reads, the seek kind of accounts for this and then it reads the sectors after */
	if ((delta || cdvd.Action == cdvdAction_Seek) && !isSeeking && !cdvd.nextSectorsBuffered)
	{
		const u32 rotationalLatency = cdvdRotationTime(((CDVD_MODE_TYPE)(cdvdIsDVD()))) / 2; /* Half it to average the rotational latency. */
		if (cdvd.Action == cdvdAction_Seek)
		{
			seektime += rotationalLatency;
			cdvd.nextSectorsBuffered = 0;
			psxRegs.interrupt &= ~(1 << IopEvt_CdvdSectorReady);
		}
		else
		{
			seektime += rotationalLatency + cdvd.ReadTime;
			CDVDSECTORREADY_INT(seektime);
			seektime += (cdvd.BlockSize / 4) * 12;
		}
	}
	else if (!isSeeking) /* Not seeking but we have buffered stuff, need to just account for DMA time (and kick the read DMA if it's not running for some reason. */
	{
		if (!(psxRegs.interrupt & (1 << IopEvt_CdvdSectorReady)))
		{
			seektime += cdvd.ReadTime;
			CDVDSECTORREADY_INT(seektime);
		}
		seektime += (cdvd.BlockSize / 4) * 12;
	}
	else /* We're seeking, so kick off the buffering after the seek finishes. */
	{
		CDVDSECTORREADY_INT(seektime);
	}

	return seektime;
}

void cdvdUpdateTrayState(void)
{
	if (cdvd.Tray.cdvdActionSeconds > 0)
	{
		if (--cdvd.Tray.cdvdActionSeconds == 0)
		{
			switch (cdvd.Tray.trayState)
			{
				case CDVD_DISC_OPEN:
					cdvdCtrlTrayOpen();
					if (cdvd.DiscType > 0 || CDVDsys_GetSourceType() == CDVD_SOURCE_NODISC)
					{
						cdvd.Tray.cdvdActionSeconds = 3;
						cdvd.Tray.trayState = CDVD_DISC_EJECT;
					}

				break;
				case CDVD_DISC_EJECT:
					cdvdCtrlTrayClose();
					break;
				case CDVD_DISC_DETECTING:
					cdvd.Tray.trayState = CDVD_DISC_SEEKING;
					cdvdUpdateStatus(CDVD_STATUS_SEEK);
					cdvd.Tray.cdvdActionSeconds = 2;
					/* If we're swapping disc, reload the elf, patches etc to reflect the new disc. */
					if (g_GameStarted)
					{
						cdvdReloadElfInfo(NULL);
						vm_game_starting_on_cpu_thread();
					}
					break;
				case CDVD_DISC_SEEKING:
					cdvd.Spinning = true;
					/* fallthrough */
				case CDVD_DISC_ENGAGED:
					cdvd.Tray.trayState = CDVD_DISC_ENGAGED;
					cdvdUpdateReady(CDVD_DRIVE_READY);
					cdvdUpdateStatus(CDVD_STATUS_PAUSE);
					break;
			}
		}
	}
}

void cdvdVsync(void)
{
	static u8 monthmap[13] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

	cdvd.RTCcount++;
	/* Exact integer compare on the milli-hertz grid; identical boundary */
	/* to the old double compare (n * 1000 < mHz  <=>  n < mHz / 1000.0). */
	if (cdvd.RTCcount * 1000u < GetVerticalFrequencyMilliHz())
		return;
	cdvd.RTCcount = 0;

	cdvdUpdateTrayState();

	cdvd.RTC.second++;
	if (cdvd.RTC.second < 60)
		return;
	cdvd.RTC.second = 0;

	cdvd.RTC.minute++;
	if (cdvd.RTC.minute < 60)
		return;
	cdvd.RTC.minute = 0;

	cdvd.RTC.hour++;
	if (cdvd.RTC.hour < 24)
		return;
	cdvd.RTC.hour = 0;

	cdvd.RTC.day++;
	if (cdvd.RTC.day <= (cdvd.RTC.month == 2 && cdvd.RTC.year % 4 == 0 ? 29 : monthmap[cdvd.RTC.month - 1]))
		return;
	cdvd.RTC.day = 1;

	cdvd.RTC.month++;
	if (cdvd.RTC.month <= 12)
		return;
	cdvd.RTC.month = 1;

	cdvd.RTC.year++;
	if (cdvd.RTC.year < 100)
		return;
	cdvd.RTC.year = 0;
}

static INLINE u8 cdvdRead18(void) /* SDATAOUT */
{
	u8 ret = 0;

	if (((cdvd.sDataIn & 0x40) == 0) && (cdvd.SCMDResultPos < cdvd.SCMDResultCnt))
	{
		cdvd.SCMDResultPos++;
		if (cdvd.SCMDResultPos >= cdvd.SCMDResultCnt)
			cdvd.sDataIn |= 0x40;
		ret = cdvd.SCMDResultBuff[cdvd.SCMDResultPos - 1];
	}

	return ret;
}

u8 cdvdRead(u8 key)
{
	switch (key)
	{
		case 0x04: /* NCOMMAND */
			return cdvd.nCommand;

		case 0x05: /* N-READY */
			return cdvd.Ready;

		case 0x06: /* ERROR */
		{
			u8 ret = cdvd.Error;
			cdvd.Error = 0;
			return ret;
		}
		case 0x07: /* BREAK */
			return 0;

		case 0x08: /* INTR_STAT */
			return cdvd.IntrStat;

		case 0x0A: /* STATUS */
			return cdvd.Status;

		case 0x0B: /* STATUS STICKY */
			return cdvd.StatusSticky;
		case 0x0C: /* CRT MINUTE */
			return itob((u8)(cdvd.CurrentSector / (60 * 75)));

		case 0x0D: /* CRT SECOND */
			return itob((u8)((cdvd.CurrentSector / 75) % 60) + 2);

		case 0x0E: /* CRT FRAME */
			return itob((u8)(cdvd.CurrentSector % 75));

		case 0x0F: /* TYPE */
			if (cdvd.Tray.trayState == CDVD_DISC_ENGAGED)
				return cdvd.DiscType;
			return (cdvd.Tray.trayState <= CDVD_DISC_SEEKING) ? cdvdTrayStateDetecting() : 0; /* Detecting Disc / No Disc */

		case 0x13: /* SPEED */
		{
			u8 speedCtrl = cdvd.SpindlCtrl & 0x3F;

			if (speedCtrl == 0)
				speedCtrl = cdvdIsDVD() ? 3 : 5;

			if (cdvdIsDVD())
				speedCtrl += 0xF;
			else
				speedCtrl--;

			if (cdvd.Tray.trayState != CDVD_DISC_ENGAGED || cdvd.Spinning == false)
				speedCtrl = 0;

			return speedCtrl;
		}


		case 0x15: /* RSV */
			return 0x0; /*  PSX DESR related, but confirmed to be 0 on normal PS2 */

		case 0x16: /* SCOMMAND */
			return cdvd.sCommand;

		case 0x17: /* SREADY */
			return cdvd.sDataIn;

		case 0x18:
			return cdvdRead18();

		case 0x20:
		case 0x21:
		case 0x22:
		case 0x23:
		case 0x24:
		{
			int temp = key - 0x20;
			return cdvd.Key[temp];
		}
		case 0x28:
		case 0x29:
		case 0x2A:
		case 0x2B:
		case 0x2C:
		{
			int temp = key - 0x23;
			return cdvd.Key[temp];
		}

		case 0x30:
		case 0x31:
		case 0x32:
		case 0x33:
		case 0x34:
		{
			int temp = key - 0x26;
			return cdvd.Key[temp];
		}

		case 0x38: /* valid parts of key data (first and last are valid) */
			return cdvd.Key[15];

		case 0x39: /* KEY-XOR */
			return cdvd.KeyXor;

		case 0x3A: /* DEC_SET */
			return cdvd.decSet;

		default:
			/* note: notify the console since this is a potentially serious emulation problem: */
			/* return -1 (all bits set) instead of 0, improves chances of the software being happy */
			break;
	}
	return -1;
}

static bool cdvdReadErrorHandler(void)
{
	if (cdvd.SectorCnt <= 0)
	{
		cdvd.Error = 0x21; /* Number of read sectors abnormal */
		return false;
	}

	if (cdvd.SeekToSector >= cdvd.MaxSector)
	{
		/* Probably should be 0x20 (bad LSN) but apparently Silent Hill 2 Black Ribbon has a fade at the end of the first trailer */
		/* And the only way you can throw an error and it still does that is to use 0x30 (Read error), anything else it skips the fade. */
		/* This'll do for now but needs investigation */
		cdvd.Error = 0x30; /* Problem occurred during read */
		return false;
	}

	return true;
}

static bool cdvdCommandErrorHandler(void)
{
	static u8 cdvdParamLength[16] = { 0, 0, 0, 0, 0, 4, 11, 11, 11, 1, 255, 255, 7, 2, 11, 1 };

	if (cdvd.nCommand > N_CD_NOP) /* Command needs a disc, so check the tray is closed */
	{
		if ((cdvd.Status & CDVD_STATUS_TRAY_OPEN) || (cdvd.DiscType == CDVD_TYPE_NODISC))
		{
			cdvd.Error = (cdvd.DiscType == CDVD_TYPE_NODISC) ? 0x12 : 0x11; /* No Disc Tray is open */
			cdvd.Ready |= CDVD_DRIVE_ERROR;
			cdvdSetIrq((1 << Irq_CommandComplete));
			return false;
		}
	}

	if (cdvd.NCMDParamCnt != cdvdParamLength[cdvd.nCommand] && cdvdParamLength[cdvd.nCommand] != 255)
	{
		cdvd.Error = 0x22; /* Invalid parameter for command */
		cdvd.Ready |= CDVD_DRIVE_ERROR;
		cdvdSetIrq((1 << Irq_CommandComplete));
		return false;
	}

	if (cdvd.nCommand > N_CD_CHG_SPDL_CTRL)
	{
		cdvd.Error = 0x10; /* Unsupported Command */
		cdvd.Ready |= CDVD_DRIVE_ERROR;
		cdvdSetIrq((1 << Irq_CommandComplete));
		return false;
	}

	return true;
}

static void cdvdWrite04(u8 rt) /* NCOMMAND */
{
	/* the read commands' */
	u32  oldSpindleCtrl;
	bool ParamError;

	if (!(cdvd.Ready & CDVD_DRIVE_READY))
	{
		cdvd.Error = 0x13; /* Not Ready */
		cdvd.Ready |= CDVD_DRIVE_ERROR;
		cdvdSetIrq((1 << Irq_CommandComplete));
		cdvd.NCMDParamPos = 0;
		cdvd.NCMDParamCnt = 0;
		return;
	}

	cdvd.nCommand = rt;
	cdvd.AbortRequested = false;

	if (!cdvdCommandErrorHandler())
	{
		cdvd.NCMDParamPos = 0;
		cdvd.NCMDParamCnt = 0;
		return;
	}

	switch (rt)
	{
		case N_CD_RESET: /* CdSync */
			cdvdUpdateReady(CDVD_DRIVE_READY);
			cdvd.SCMDParamPos = 0;
			cdvd.SCMDParamCnt = 0;
			cdvdUpdateStatus(CDVD_STATUS_STOP);
			cdvd.Spinning = false;
			memset(cdvd.SCMDResultBuff, 0, sizeof(cdvd.SCMDResultBuff));
			cdvdSetIrq((1 << Irq_CommandComplete));
			break;

		case N_CD_STANDBY: /* CdStandby */

			/* Seek to sector zero.  The cdvdStartSeek function will simulate */
			/* spinup times if needed. */
			CDVD_INT(cdvdStartSeek(0, ((CDVD_MODE_TYPE)(cdvdIsDVD())), false));
			/* Might not seek, but makes sense since it does move to the inner most track */
			/* It's only temporary until the interrupt anyway when it sets itself ready */
			cdvdUpdateStatus(CDVD_STATUS_SEEK);
			cdvd.Action = cdvdAction_Standby;
			break;

		case N_CD_STOP: /* CdStop */
			cdvdUpdateReady(CDVD_DRIVE_BUSY);
			cdvd.nextSectorsBuffered = 0;
			psxRegs.interrupt &= ~(1 << IopEvt_CdvdSectorReady);
			cdvdUpdateStatus(CDVD_STATUS_SPIN);
			PSX_INT(IopEvt_Cdvd, PSXCLK / 6); /* 166ms delay? */
			cdvd.Action = cdvdAction_Stop;
			break;

		case N_CD_PAUSE: /* CdPause */
			/* A few games rely on PAUSE setting the Status correctly. */
			/* However we should probably stop any read in progress too, just to be safe */
			psxRegs.interrupt &= ~(1 << IopEvt_Cdvd);
			cdvdUpdateReady(CDVD_DRIVE_READY);
			cdvdSetIrq((1 << Irq_CommandComplete));
			/* After Pausing needs to buffer the next sector */
			cdvdUpdateStatus(CDVD_STATUS_PAUSE);
			cdvd.nextSectorsBuffered = 0;
			CDVDSECTORREADY_INT(cdvd.ReadTime);
			break;

		case N_CD_SEEK: /* CdSeek */
		{
			u32 sector;
			cdvd.Action = cdvdAction_Seek; /* Have to do this first, the StartSeek relies on it */
			/* The parameter buffer is bytes; the sector is read out of it as
			 * the read commands do, not through a pointer that the buffer's
			 * place in the struct need not align. */
			memcpy(&sector, cdvd.NCMDParamBuff + 0, sizeof(u32));
			CDVD_INT(cdvdStartSeek(sector, ((CDVD_MODE_TYPE)(cdvdIsDVD())), false));
			cdvdUpdateStatus(CDVD_STATUS_SEEK);
			break;
		}

		case N_CD_READ: /* CdRead */
		{
			/* Assign the seek to sector based on cdvd.Param[0]-[3], and the number of  sectors based on cdvd.Param[4]-[7]. */
			memcpy(&cdvd.SeekToSector, cdvd.NCMDParamBuff + 0, sizeof(u32));
			memcpy(&cdvd.SectorCnt,    cdvd.NCMDParamBuff + 4, sizeof(u32));
			cdvd.RetryCntMax   = (cdvd.NCMDParamBuff[8] == 0) ? 0x100 : cdvd.NCMDParamBuff[8];
			oldSpindleCtrl = cdvd.SpindlCtrl;

			if (cdvd.NCMDParamBuff[9] & 0x3F)
				cdvd.SpindlCtrl = cdvd.NCMDParamBuff[9];
			else
				cdvd.SpindlCtrl = (cdvd.NCMDParamBuff[9] & 0x80) | (cdvdIsDVD() ? 3 : 5); /* Max speed for DVD/CD */

			ParamError = false;

			switch (cdvd.SpindlCtrl & CDVD_SPINDLE_SPEED)
			{
				case 1: /* x1 */
					cdvd.Speed = 1;
					break;
				case 2: /* x2 */
					cdvd.Speed = 2;
					break;
				case 3: /* x4 */
					cdvd.Speed = 4;
					break;
				case 4: /* x12 */
					if (cdvdIsDVD())
						ParamError = true;
					else
						cdvd.Speed = 12;
					break;
				case 5: /* x24 */
					if (cdvdIsDVD())
						ParamError = true;
					else
						cdvd.Speed = 24;
					break;
				default:
					ParamError = true;
					break;
			}

			if (cdvdIsDVD() && cdvd.NCMDParamBuff[10] != 0)
				ParamError = true;
			else
			{
				switch (cdvd.NCMDParamBuff[10])
				{
					case 2:
						cdvd.ReadMode = CDVD_MODE_2340;
						cdvd.BlockSize = 2340;
						break;
					case 1:
						cdvd.ReadMode = CDVD_MODE_2328;
						cdvd.BlockSize = 2328;
						break;
					case 0:
						cdvd.ReadMode = CDVD_MODE_2048;
						cdvd.BlockSize = 2048;
						break;
					default:
						ParamError = true;
						break;
				}
			}

			if (ParamError)
			{
				cdvd.SpindlCtrl = oldSpindleCtrl;
				cdvd.Error = 0x22; /* Invalid Parameter */
				cdvd.Action = cdvdAction_Error;
				cdvdUpdateStatus(CDVD_STATUS_SEEK);
				cdvdUpdateReady(CDVD_DRIVE_BUSY);
				CDVD_INT(cdvd.BlockSize * 12);
				break;
			}

			if (!cdvdReadErrorHandler())
			{
				cdvd.Action = cdvdAction_Error;
				cdvdUpdateStatus(CDVD_STATUS_SEEK);
				cdvdUpdateReady(CDVD_DRIVE_BUSY);
				CDVD_INT(cdvdRotationTime((CDVD_MODE_TYPE)cdvdIsDVD()));
				break;
			}

			CDVDREAD_INT(cdvdStartSeek(cdvd.SeekToSector, ((CDVD_MODE_TYPE)(cdvdIsDVD())), !(cdvd.SpindlCtrl & CDVD_SPINDLE_CAV) && (oldSpindleCtrl & CDVD_SPINDLE_CAV)));

			/* Read-ahead by telling CDVD about the track now. */
			/* This helps improve performance on actual from-cd emulation */
			/* (ie, not using the hard drive) */
			cdvd.ReadErr = DoCDVDreadTrack(cdvd.SeekToSector, cdvd.ReadMode);

			/* Set the reading block flag.  If a seek is pending then SeekCompleted will */
			/* take priority in the handler anyway.  If the read is contiguous then */
			/* this'll skip the seek delay. */
			cdvd.Reading = 1;
			break;
		}
		case N_CD_READ_CDDA: /* CdReadCDDA */
		case N_CD_READ_XCDDA: /* CdReadXCDDA */
		{
			if (cdvdIsDVD())
			{
				cdvd.Error = 0x14; /* Invalid for current disc type */
				cdvdUpdateReady(CDVD_DRIVE_READY | CDVD_DRIVE_ERROR);
				cdvdSetIrq((1 << Irq_CommandComplete));
				return;
			}
			/* Assign the seek to sector based on cdvd.Param[0]-[3], and the number of  sectors based on cdvd.Param[4]-[7]. */
			memcpy(&cdvd.SeekToSector, cdvd.NCMDParamBuff + 0, sizeof(u32));
			memcpy(&cdvd.SectorCnt,    cdvd.NCMDParamBuff + 4, sizeof(u32));
			cdvd.RetryCntMax   = (cdvd.NCMDParamBuff[8] == 0) ? 0x100 : cdvd.NCMDParamBuff[8];

			oldSpindleCtrl = cdvd.SpindlCtrl;

			if (cdvd.NCMDParamBuff[9] & 0x3F)
				cdvd.SpindlCtrl = cdvd.NCMDParamBuff[9];
			else
				cdvd.SpindlCtrl = (cdvd.NCMDParamBuff[9] & 0x80) | 5; /* Max speed for CD */

			ParamError = false;

			switch (cdvd.SpindlCtrl & CDVD_SPINDLE_SPEED)
			{
				case 1: /* x1 */
					cdvd.Speed = 1;
					break;
				case 2: /* x2 */
					cdvd.Speed = 2;
					break;
				case 3: /* x4 */
					cdvd.Speed = 4;
					break;
				case 4: /* x12 */
					cdvd.Speed = 12;
					break;
				case 5: /* x24 */
					cdvd.Speed = 24;
					break;
				default:
					ParamError = true;
					break;
			}

			switch (cdvd.NCMDParamBuff[10])
			{
				case 1:
					cdvd.ReadMode = CDVD_MODE_2368;
					cdvd.BlockSize = 2368;
					break;
				case 0:
					cdvd.ReadMode = CDVD_MODE_2352;
					cdvd.BlockSize = 2352;
					break;
				default:
					ParamError = true;
					break;
			}

			if (ParamError)
			{
				cdvd.SpindlCtrl = oldSpindleCtrl;
				cdvd.Error = 0x22; /* Invalid Parameter */
				cdvd.Action = cdvdAction_Error;
				cdvdUpdateStatus(CDVD_STATUS_SEEK);
				cdvdUpdateReady(CDVD_DRIVE_BUSY);
				CDVD_INT(cdvd.BlockSize * 12);
				break;
			}

			CDVDREAD_INT(cdvdStartSeek(cdvd.SeekToSector, MODE_CDROM, !(cdvd.SpindlCtrl& CDVD_SPINDLE_CAV) && (oldSpindleCtrl& CDVD_SPINDLE_CAV)));

			/* Read-ahead by telling CDVD about the track now. */
			/* This helps improve performance on actual from-cd emulation */
			/* (ie, not using the hard drive) */
			cdvd.ReadErr = DoCDVDreadTrack(cdvd.SeekToSector, cdvd.ReadMode);

			/* Set the reading block flag.  If a seek is pending then SeekCompleted will */
			/* take priority in the handler anyway.  If the read is contiguous then */
			/* this'll skip the seek delay. */
			cdvd.Reading = 1;
			break;
		}
		case N_DVD_READ: /* DvdRead */
		{
			if (!cdvdIsDVD())
			{
				cdvd.Error = 0x14; /* Invalid for current disc type */
				cdvdUpdateReady(CDVD_DRIVE_READY | CDVD_DRIVE_ERROR);
				cdvdSetIrq((1 << Irq_CommandComplete));
				return;
			}
			/* Assign the seek to sector based on cdvd.Param[0]-[3], and the number of  sectors based on cdvd.Param[4]-[7]. */
			memcpy(&cdvd.SeekToSector, cdvd.NCMDParamBuff + 0, sizeof(u32));
			memcpy(&cdvd.SectorCnt,    cdvd.NCMDParamBuff + 4, sizeof(u32));

			oldSpindleCtrl = cdvd.SpindlCtrl;

			if (cdvd.NCMDParamBuff[8] == 0)
				cdvd.RetryCntMax = 0x100;
			else
				cdvd.RetryCntMax = cdvd.NCMDParamBuff[8];

			if (cdvd.NCMDParamBuff[9] & 0x3F)
				cdvd.SpindlCtrl = cdvd.NCMDParamBuff[9];
			else
				cdvd.SpindlCtrl = (cdvd.NCMDParamBuff[9] & 0x80) | 3; /* Max speed for DVD */

			ParamError = false;

			switch (cdvd.SpindlCtrl & CDVD_SPINDLE_SPEED)
			{
				case 1: /* x1 */
					cdvd.Speed = 1;
					break;
				case 2: /* x2 */
					cdvd.Speed = 2;
					break;
				case 3: /* x4 */
					cdvd.Speed = 4;
					break;
				default:
					ParamError = true;
					break;
			}

			if (cdvd.NCMDParamBuff[10] != 0)
				ParamError = true;

			cdvd.ReadMode = CDVD_MODE_2048;
			cdvd.BlockSize = 2064;

			if (ParamError)
			{
				cdvd.SpindlCtrl = oldSpindleCtrl;
				cdvd.Error      = 0x22; /* Invalid Parameter */
				cdvd.Action     = cdvdAction_Error;
				cdvdUpdateStatus(CDVD_STATUS_SEEK);
				cdvdUpdateReady(CDVD_DRIVE_BUSY);
				CDVD_INT(cdvd.BlockSize * 12);
				break;
			}

			if (!cdvdReadErrorHandler())
			{
				cdvd.Action = cdvdAction_Error;
				cdvdUpdateStatus(CDVD_STATUS_SEEK);
				cdvdUpdateReady(CDVD_DRIVE_BUSY);
				CDVD_INT(cdvdRotationTime((CDVD_MODE_TYPE)cdvdIsDVD()));
				break;
			}

			CDVDREAD_INT(cdvdStartSeek(cdvd.SeekToSector, MODE_DVDROM, !(cdvd.SpindlCtrl & CDVD_SPINDLE_CAV) && (oldSpindleCtrl& CDVD_SPINDLE_CAV)));

			/* Read-ahead by telling CDVD about the track now. */
			/* This helps improve performance on actual from-cd emulation */
			/* (ie, not using the hard drive) */
			cdvd.ReadErr = DoCDVDreadTrack(cdvd.SeekToSector, cdvd.ReadMode);

			/* Set the reading block flag.  If a seek is pending then SeekCompleted will */
			/* take priority in the handler anyway.  If the read is contiguous then */
			/* this'll skip the seek delay. */
			cdvd.Reading = 1;
			break;
		}
		case N_CD_GET_TOC: /* CdGetToc & cdvdman_call19 */
			/* Param[0] is 0 for CdGetToc and any value for cdvdman_call19 */
			CDVD->getTOC(&iopMem->Main[HW_DMA3_MADR & 0x1fffff]);
			cdvdSetIrq((1 << Irq_CommandComplete));
			HW_DMA3_CHCR &= ~0x01000000;
			psxDmaInterrupt(3);
			cdvdUpdateReady(CDVD_DRIVE_READY);
			/* After reading the TOC it needs to go back to buffer the next sector */
			cdvdUpdateStatus(CDVD_STATUS_PAUSE);
			cdvd.nextSectorsBuffered = 0;
			CDVDSECTORREADY_INT(cdvd.ReadTime);
			break;

		case N_CD_READ_KEY: /* CdReadKey */
			{
				u8 arg0 = cdvd.NCMDParamBuff[0];
				u16 arg1 = cdvd.NCMDParamBuff[1] | (cdvd.NCMDParamBuff[2] << 8);
				u32 arg2 = cdvd.NCMDParamBuff[3] | (cdvd.NCMDParamBuff[4] << 8) | (cdvd.NCMDParamBuff[5] << 16) | (cdvd.NCMDParamBuff[6] << 24);
				cdvdReadKey(arg0, arg1, arg2, cdvd.Key);
				cdvd.KeyXor = 0x00;
				cdvdSetIrq((1 << Irq_CommandComplete));
				/* After reading the key it needs to go back to buffer the next sector */
				cdvdUpdateStatus(CDVD_STATUS_PAUSE);
				cdvdUpdateReady(CDVD_DRIVE_READY);
				cdvd.nextSectorsBuffered = 0;
				CDVDSECTORREADY_INT(cdvd.ReadTime);
			}
			break;
		case N_CD_NOP: /* CdNop_ */
			cdvdUpdateReady(CDVD_DRIVE_READY);
			/* fallthrough */
		case N_CD_CHG_SPDL_CTRL: /* CdChgSpdlCtrl */
		default: /* Should be unreachable, handled in the error handler earlier */
			cdvdSetIrq((1 << Irq_CommandComplete));
			break;
	}
	cdvd.NCMDParamPos = 0;
	cdvd.NCMDParamCnt = 0;
}

static INLINE void cdvdWrite05(u8 rt) /* NDATAIN */
{
	if (cdvd.NCMDParamPos >= 16)
	{
		cdvd.NCMDParamPos = 0;
		cdvd.NCMDParamCnt = 0;
	}

	cdvd.NCMDParamBuff[cdvd.NCMDParamPos++] = rt;
	cdvd.NCMDParamCnt++;
}

static INLINE void cdvdWrite07(u8 rt) /* BREAK */
{
	/* If we're already in a Ready state or already Breaking, then do nothing: */
	if (!(cdvd.Ready & CDVD_DRIVE_BUSY) || cdvd.AbortRequested)
		return;

	cdvd.AbortRequested = true;
}

static void cdvdWrite16(u8 rt) /* SCOMMAND */
{
	/* 	cdvdTN	diskInfo; */
	/* 	cdvdTD	trackInfo; */
	/* 	int i, lbn, type, min, sec, frm, address; */
	int address;
	u8 tmp;
	cdvd.sCommand = rt;
	memset(cdvd.SCMDResultBuff, 0, sizeof(cdvd.SCMDResultBuff));

	switch (rt)
	{
		/* 		case 0x01: // GetDiscType - from cdvdman (0:1) */
		/* 			SetResultSize(1); */
		/* 			cdvd.Result[0] = 0; */
		/* 			break; */

		case 0x02: /* CdReadSubQ  (0:11) */
			cdvd.SCMDResultCnt  = 11;
			cdvd.SCMDResultPos  = 0;
			cdvd.sDataIn       &= ~0x40;
			memset(&cdvd.SCMDResultBuff[0], 0, 11);
			cdvd.SCMDResultBuff[0] = cdvdReadSubQ(cdvd.CurrentSector, (cdvdSubQ*)&cdvd.SCMDResultBuff[1]);
			break;

		case 0x03: /* Mecacon-command */
			switch (cdvd.SCMDParamBuff[0])
			{
				case 0x00: /* get mecha version (1:4) */
					cdvd.SCMDResultCnt      = 4;
					cdvd.SCMDResultPos      = 0;
					cdvd.sDataIn           &= ~0x40;
					memset(&cdvd.SCMDResultBuff[0], 0, 4);
					memcpy(&cdvd.SCMDResultBuff[0], &s_mecha_version, sizeof(u32));
					break;
				case 0x30:
					cdvd.SCMDResultCnt      = 2;
					cdvd.SCMDResultPos      = 0;
					cdvd.sDataIn           &= ~0x40;
					cdvd.SCMDResultBuff[0]  = cdvd.Status;
					cdvd.SCMDResultBuff[1]  = (cdvd.Status & 0x1) ? 8 : 0;
					break;
				case 0x44: /* write console ID (9:1) */
					cdvd.SCMDResultCnt      = 1;
					cdvd.SCMDResultPos      = 0;
					cdvd.sDataIn           &= ~0x40;
					memset(&cdvd.SCMDResultBuff[0], 0, 1);
					cdvdWriteNVM(&cdvd.SCMDParamBuff[1], getNvmLayout()->consoleId, 8);
					break;

				case 0x45: /* read console ID (1:9) */
					cdvd.SCMDResultCnt      = 9;
					cdvd.SCMDResultPos      = 0;
					cdvd.sDataIn           &= ~0x40;
					memset(&cdvd.SCMDResultBuff[0], 0, 9);
					cdvdReadNVM(&cdvd.SCMDResultBuff[1], getNvmLayout()->consoleId, 8);
					break;

				case 0xFD: /* _sceCdReadRenewalDate (1:6) BCD */
					cdvd.SCMDResultCnt      = 6;
					cdvd.SCMDResultPos      = 0;
					cdvd.sDataIn           &= ~0x40;
					cdvd.SCMDResultBuff[0]  = 0;
					cdvd.SCMDResultBuff[1]  = 0x04; /* year */
					cdvd.SCMDResultBuff[2]  = 0x12; /* month */
					cdvd.SCMDResultBuff[3]  = 0x10; /* day */
					cdvd.SCMDResultBuff[4]  = 0x01; /* hour */
					cdvd.SCMDResultBuff[5]  = 0x30; /* min */
					break;

				case 0xEF: /* read console temperature (1:3) */
					   /* This returns a fixed value of 30.5 C */
					cdvd.SCMDResultCnt      = 3;
					cdvd.SCMDResultPos      = 0;
					cdvd.sDataIn           &= ~0x40;
					cdvd.SCMDResultBuff[0]  = 0; /* returns 0 on success */
					cdvd.SCMDResultBuff[1]  = 0x0F; /* last 8 bits for integer */
					cdvd.SCMDResultBuff[2]  = 0x05; /* leftmost bit for integer, other 7 bits for decimal place */
					break;

				default:
					cdvd.SCMDResultCnt      = 1;
					cdvd.SCMDResultPos      = 0;
					cdvd.sDataIn           &= ~0x40;
					cdvd.SCMDResultBuff[0]  = 0x81;
					break;
			}
			break;

		case 0x05: /* CdTrayReqState (0:1) - resets the tray open detection */
			   /* log_cb(RETRO_LOG_WARN, "CdTrayReqState. cdvd.Status = %d\n", cdvd.Status); */
			   /* This function sets the Sticky tray flag to the same value as Status for detecting change */
			cdvd.StatusSticky = cdvd.Status & CDVD_STATUS_TRAY_OPEN;

			cdvd.SCMDResultCnt      = 1;
			cdvd.SCMDResultPos      = 0;
			cdvd.sDataIn           &= ~0x40;
			cdvd.SCMDResultBuff[0]  = 0; /* Could be a bit to say it's busy, but actual function is unknown, it expects 0 to continue. */
			break;

		case 0x06: /* CdTrayCtrl  (1:1) */
			cdvd.SCMDResultCnt      = 1;
			cdvd.SCMDResultPos      = 0;
			cdvd.sDataIn           &= ~0x40;
			if (cdvd.SCMDParamBuff[0] == 0)
				cdvd.SCMDResultBuff[0] = cdvdCtrlTrayOpen();
			else
				cdvd.SCMDResultBuff[0] = cdvdCtrlTrayClose();
			break;

		case 0x08: /* CdReadRTC (0:8) */
			cdvd.SCMDResultCnt      = 8;
			cdvd.SCMDResultPos      = 0;
			cdvd.sDataIn           &= ~0x40;
			cdvd.SCMDResultBuff[0]  = 0;
			cdvd.SCMDResultBuff[1]  = itob(cdvd.RTC.second); /* Seconds */
			cdvd.SCMDResultBuff[2]  = itob(cdvd.RTC.minute); /* Minutes */
			cdvd.SCMDResultBuff[3]  = itob(cdvd.RTC.hour);   /* Hours */
			cdvd.SCMDResultBuff[4]  = 0;                     /* Nothing */
			cdvd.SCMDResultBuff[5]  = itob(cdvd.RTC.day);    /* Day */
			cdvd.SCMDResultBuff[6]  = itob(cdvd.RTC.month);  /* Month */
			cdvd.SCMDResultBuff[7]  = itob(cdvd.RTC.year);   /* Year */
			break;

		case 0x09: /* sceCdWriteRTC (7:1) */
			cdvd.SCMDResultCnt      = 1;
			cdvd.SCMDResultPos      = 0;
			cdvd.sDataIn           &= ~0x40;
			cdvd.SCMDResultBuff[0]  = 0;
			cdvd.RTC.pad            = 0;

			cdvd.RTC.second         = btoi(cdvd.SCMDParamBuff[cdvd.SCMDParamPos - 7]);
			cdvd.RTC.minute         = btoi(cdvd.SCMDParamBuff[cdvd.SCMDParamPos - 6]) % 60;
			cdvd.RTC.hour           = btoi(cdvd.SCMDParamBuff[cdvd.SCMDParamPos - 5]) % 24;
			cdvd.RTC.day            = btoi(cdvd.SCMDParamBuff[cdvd.SCMDParamPos - 3]);
			cdvd.RTC.month          = btoi(cdvd.SCMDParamBuff[cdvd.SCMDParamPos - 2] & 0x7f);
			cdvd.RTC.year           = btoi(cdvd.SCMDParamBuff[cdvd.SCMDParamPos - 1]);
			break;

		case 0x0A: /* sceCdReadNVM (2:3) */
			address = (cdvd.SCMDParamBuff[0] << 8) | cdvd.SCMDParamBuff[1];

			if (address < 512)
			{
				cdvd.SCMDResultCnt  = 3;
				cdvd.SCMDResultPos  = 0;
				cdvd.sDataIn       &= ~0x40;
				memset(&cdvd.SCMDResultBuff[0], 0, 3);
				cdvdReadNVM(&cdvd.SCMDResultBuff[1], address * 2, 2);
				/* swap bytes around */
				tmp = cdvd.SCMDResultBuff[1];
				cdvd.SCMDResultBuff[1] = cdvd.SCMDResultBuff[2];
				cdvd.SCMDResultBuff[2] = tmp;
			}
			else
			{
				cdvd.SCMDResultCnt      = 1;
				cdvd.SCMDResultPos      = 0;
				cdvd.sDataIn           &= ~0x40;
				cdvd.SCMDResultBuff[0]  = 0xff;
			}
			break;

		case 0x0B: /* sceCdWriteNVM (4:1) */
			cdvd.SCMDResultCnt  = 1;
			cdvd.SCMDResultPos  = 0;
			cdvd.sDataIn       &= ~0x40;
			address = (cdvd.SCMDParamBuff[0] << 8) | cdvd.SCMDParamBuff[1];

			if (address < 512)
			{
				/* swap bytes around */
				tmp = cdvd.SCMDParamBuff[2];
				cdvd.SCMDParamBuff[2] = cdvd.SCMDParamBuff[3];
				cdvd.SCMDParamBuff[3] = tmp;
				cdvdWriteNVM(&cdvd.SCMDParamBuff[2], address * 2, 2);
			}
			else
				cdvd.SCMDResultBuff[0] = 0xff;
			break;

			/* 		case 0x0C: // sceCdSetHDMode (1:1) */
			/* 			break; */


		case 0x0F: /* sceCdPowerOff (0:1)- Call74 from Xcdvdman */
			vm_request_stop();
			break;

		case 0x12: /* sceCdReadILinkId (0:9) */
			cdvd.SCMDResultCnt  = 9;
			cdvd.SCMDResultPos  = 0;
			cdvd.sDataIn       &= ~0x40;
			memset(&cdvd.SCMDResultBuff[0], 0, 9);
			cdvdReadNVM(&cdvd.SCMDResultBuff[1], getNvmLayout()->ilinkId, 8);
			if ((!cdvd.SCMDResultBuff[3]) && (!cdvd.SCMDResultBuff[4])) /* nvm file is missing correct iLinkId, return hardcoded one */
			{
				cdvd.SCMDResultBuff[0] = 0x00;
				cdvd.SCMDResultBuff[1] = 0x00;
				cdvd.SCMDResultBuff[2] = 0xAC;
				cdvd.SCMDResultBuff[3] = 0xFF;
				cdvd.SCMDResultBuff[4] = 0xFF;
				cdvd.SCMDResultBuff[5] = 0xFF;
				cdvd.SCMDResultBuff[6] = 0xFF;
				cdvd.SCMDResultBuff[7] = 0xB9;
				cdvd.SCMDResultBuff[8] = 0x86;
			}
			break;

		case 0x13: /* sceCdWriteILinkID (8:1) */
			cdvd.SCMDResultCnt  = 1;
			cdvd.SCMDResultPos  = 0;
			cdvd.sDataIn       &= ~0x40;
			memset(&cdvd.SCMDResultBuff[0], 0, 1);
			cdvdWriteNVM(&cdvd.SCMDResultBuff[1], getNvmLayout()->ilinkId, 8);
			break;

		case 0x14: /* CdCtrlAudioDigitalOut (1:1) */
			   /* parameter can be 2, 0, ... */
			cdvd.SCMDResultCnt  = 1;
			cdvd.SCMDResultPos  = 0;
			cdvd.sDataIn       &= ~0x40;
			cdvd.SCMDResultBuff[0] = 0; /* 8 is a flag; not used */
			break;

		case 0x15: /* sceCdForbidDVDP (0:1) */
			cdvd.SCMDResultCnt  = 1;
			cdvd.SCMDResultPos  = 0;
			cdvd.sDataIn       &= ~0x40;
			cdvd.SCMDResultBuff[0] = 5;
			break;

		case 0x16: /* AutoAdjustCtrl - from cdvdman (1:1) */
			cdvd.SCMDResultCnt  = 1;
			cdvd.SCMDResultPos  = 0;
			cdvd.sDataIn       &= ~0x40;
			cdvd.SCMDResultBuff[0] = 0;
			break;

		case 0x17: /* CdReadModelNumber (1:9) - from xcdvdman */
			cdvd.SCMDResultCnt  = 9;
			cdvd.SCMDResultPos  = 0;
			cdvd.sDataIn       &= ~0x40;
			memset(&cdvd.SCMDResultBuff[0], 0, 9);
			cdvdReadNVM(&cdvd.SCMDResultBuff[1], getNvmLayout()->modelNum + cdvd.SCMDResultBuff[0], 8);
			break;

		case 0x18: /* CdWriteModelNumber (9:1) - from xcdvdman */
			cdvd.SCMDResultCnt  = 1;
			cdvd.SCMDResultPos  = 0;
			cdvd.sDataIn       &= ~0x40;
			cdvd.SCMDResultBuff[0] = 0;
			cdvdWriteNVM(&cdvd.SCMDParamBuff[1], getNvmLayout()->modelNum + cdvd.SCMDResultBuff[0], 8);
			break;

			/* 		case 0x19: // sceCdForbidRead (0:1) - from xcdvdman */
			/* 			break; */

		case 0x1A: /* sceCdBootCertify (4:1)//(4:16 in psx?) */
			/* on input there are 4 bytes: 1;?10;J;C for 18000; 1;60;E;C for 39002 from ROMVER */
			cdvd.SCMDResultCnt  = 1;
			cdvd.SCMDResultPos  = 0;
			cdvd.sDataIn       &= ~0x40;
			cdvd.SCMDResultBuff[0] = 1; /* i guess that means okay */
			break;

		case 0x1B: /* sceCdCancelPOffRdy (0:1) - Call73 from Xcdvdman (1:1) */
			cdvd.SCMDResultCnt  = 1;
			cdvd.SCMDResultPos  = 0;
			cdvd.sDataIn       &= ~0x40;
			cdvd.SCMDResultBuff[0] = 0;
			break;

		case 0x1C: /* sceCdBlueLEDCtl (1:1) - Call72 from Xcdvdman */
			cdvd.SCMDResultCnt  = 1;
			cdvd.SCMDResultPos  = 0;
			cdvd.sDataIn       &= ~0x40;
			cdvd.SCMDResultBuff[0] = 0;
			break;

			/* 		case 0x1D: // cdvdman_call116 (0:5) - In V10 Bios */
			/* 			break; */

		case 0x1E: /* sceRemote2Read (0:5) - // 00 14 AA BB CC -> remote key code */
			cdvd.SCMDResultCnt  = 5;
			cdvd.SCMDResultPos  = 0;
			cdvd.sDataIn       &= ~0x40;
			cdvd.SCMDResultBuff[0] = 0x00;
			cdvd.SCMDResultBuff[1] = 0x14;
			cdvd.SCMDResultBuff[2] = 0x00;
			cdvd.SCMDResultBuff[3] = 0x00;
			cdvd.SCMDResultBuff[4] = 0x00;
			break;

			/* 		case 0x1F: // sceRemote2_7 (2:1) - cdvdman_call117 */
			/* 			break; */

		case 0x20: /* sceRemote2_6 (0:3)	// 00 01 00 */
			cdvd.SCMDResultCnt  = 3;
			cdvd.SCMDResultPos  = 0;
			cdvd.sDataIn       &= ~0x40;
			cdvd.SCMDResultBuff[0] = 0x00;
			cdvd.SCMDResultBuff[1] = 0x01;
			cdvd.SCMDResultBuff[2] = 0x00;
			break;

			/* 		case 0x21: // sceCdWriteWakeUpTime (8:1) */
			/* 			break; */

		case 0x22: /* sceCdReadWakeUpTime (0:10) */
			cdvd.SCMDResultCnt  = 10;
			cdvd.SCMDResultPos  = 0;
			cdvd.sDataIn       &= ~0x40;
			cdvd.SCMDResultBuff[0] = 0;
			cdvd.SCMDResultBuff[1] = 0;
			cdvd.SCMDResultBuff[2] = 0;
			cdvd.SCMDResultBuff[3] = 0;
			cdvd.SCMDResultBuff[4] = 0;
			cdvd.SCMDResultBuff[5] = 0;
			cdvd.SCMDResultBuff[6] = 0;
			cdvd.SCMDResultBuff[7] = 0;
			cdvd.SCMDResultBuff[8] = 0;
			cdvd.SCMDResultBuff[9] = 0;
			break;

		case 0x24: /* sceCdRCBypassCtrl (1:1) - In V10 Bios */
			   /* FIXME: because PRId<0x23, the bit 0 of sio2 don't get updated 0xBF808284 */
			cdvd.SCMDResultCnt  = 1;
			cdvd.SCMDResultPos  = 0;
			cdvd.sDataIn       &= ~0x40;
			cdvd.SCMDResultBuff[0] = 0;
			break;

			/* 		case 0x25: // cdvdman_call120 (1:1) - In V10 Bios */
			/* 			break; */

			/* 		case 0x26: // cdvdman_call128 (0,3) - In V10 Bios */
			/* 			break; */

		case 0x27: /* GetPS1BootParam (0:13) - called only by China region PS2 models */

			/* Return Disc Serial which is passed to PS1DRV and later used to find matching config. */
			cdvd.SCMDResultCnt  = 13;
			cdvd.SCMDResultPos  = 0;
			cdvd.sDataIn       &= ~0x40;
			cdvd.SCMDResultBuff[0] = 0;
			cdvd.SCMDResultBuff[1] = DiscSerial[0];
			cdvd.SCMDResultBuff[2] = DiscSerial[1];
			cdvd.SCMDResultBuff[3] = DiscSerial[2];
			cdvd.SCMDResultBuff[4] = DiscSerial[3];
			cdvd.SCMDResultBuff[5] = DiscSerial[4];
			cdvd.SCMDResultBuff[6] = DiscSerial[5];
			cdvd.SCMDResultBuff[7] = DiscSerial[6];
			cdvd.SCMDResultBuff[8] = DiscSerial[7];
			cdvd.SCMDResultBuff[9] = DiscSerial[9]; /* Skipping dot here is required. */
			cdvd.SCMDResultBuff[10] = DiscSerial[10];
			cdvd.SCMDResultBuff[11] = DiscSerial[11];
			cdvd.SCMDResultBuff[12] = DiscSerial[12];
			break;

			/* 		case 0x28: // cdvdman_call150 (1:1) - In V10 Bios */
			/* 			break; */

		case 0x29: /* sceCdNoticeGameStart (1:1) */
			cdvd.SCMDResultCnt  = 1;
			cdvd.SCMDResultPos  = 0;
			cdvd.sDataIn       &= ~0x40;
			cdvd.SCMDResultBuff[0] = 0;
			break;

			/* 		case 0x2C: //sceCdXBSPowerCtl (2:2) */
			/* 			break; */

			/* 		case 0x2D: //sceCdXLEDCtl (2:2) */
			/* 			break; */

			/* 		case 0x2E: //sceCdBuzzerCtl (0:1) */
			/* 			break; */

			/* 		case 0x2F: //cdvdman_call167 (16:1) */
			/* 			break; */

			/* 		case 0x30: //cdvdman_call169 (1:9) */
			/* 			break; */

		case 0x31: /* sceCdSetMediumRemoval (1:1) */
			cdvd.SCMDResultCnt  = 1;
			cdvd.SCMDResultPos  = 0;
			cdvd.sDataIn       &= ~0x40;
			cdvd.SCMDResultBuff[0] = 0;
			break;

		case 0x32: /* sceCdGetMediumRemoval (0:2) */
			cdvd.SCMDResultCnt  = 2;
			cdvd.SCMDResultPos  = 0;
			cdvd.sDataIn       &= ~0x40;
			cdvd.SCMDResultBuff[0] = 0;
			cdvd.SCMDResultBuff[1] = 0;
			break;

			/* 		case 0x33: //sceCdXDVRPReset (1:1) */
			/* 			break; */

		case 0x36: /* cdvdman_call189 [__sceCdReadRegionParams - made up name] (0:15) i think it is 16, not 15 */
			cdvd.SCMDResultCnt  = 15;
			cdvd.SCMDResultPos  = 0;
			cdvd.sDataIn       &= ~0x40;
			memset(&cdvd.SCMDResultBuff[0], 0, 15);

			memcpy(&cdvd.SCMDResultBuff[1], &s_mecha_version, sizeof(u32));
			cdvdReadNVM(&cdvd.SCMDResultBuff[3], getNvmLayout()->regparams, 8);
			cdvd.SCMDResultBuff[1] = 1 << cdvd.SCMDResultBuff[1]; /* encryption zone; see offset 0x1C in encrypted headers */
								      /* //////////////////////////////////////// */
			cdvd.SCMDResultBuff[2] = 0; /* ?? */
						/* 			cdvd.Result[3] == ROMVER[4] == *0xBFC7FF04 */
						/* 			cdvd.Result[4] == OSDVER[4] == CAP			Jjpn, Aeng, Eeng, Heng, Reng, Csch, Kkor? */
						/* 			cdvd.Result[5] == OSDVER[5] == small */
						/* 			cdvd.Result[6] == OSDVER[6] == small */
						/* 			cdvd.Result[7] == OSDVER[7] == small */
						/* 			cdvd.Result[8] == VERSTR[0x22] == *0xBFC7FF52 */
						/* 			cdvd.Result[9] == DVDID						J U O E A R C M */
						/* 			cdvd.Result[10]== 0;					//?? */
			cdvd.SCMDResultBuff[11] = 0; /* ?? */
			cdvd.SCMDResultBuff[12] = 0; /* ?? */
						 /* //////////////////////////////////////// */
			cdvd.SCMDResultBuff[13] = 0; /* 0xFF - 77001 */
			cdvd.SCMDResultBuff[14] = 0; /* ?? */
			break;

		case 0x37: /* called from EECONF [sceCdReadMAC - made up name] (0:9) */
			cdvd.SCMDResultCnt  = 9;
			cdvd.SCMDResultPos  = 0;
			cdvd.sDataIn       &= ~0x40;
			memset(&cdvd.SCMDResultBuff[0], 0, 9);
			cdvdReadNVM(&cdvd.SCMDResultBuff[1], getNvmLayout()->mac, 8);
			break;

		case 0x38: /* used to fix the MAC back after accidentally trashed it :D [sceCdWriteMAC - made up name] (8:1) */
			cdvd.SCMDResultCnt  = 1;
			cdvd.SCMDResultPos  = 0;
			cdvd.sDataIn       &= ~0x40;
			cdvd.SCMDResultBuff[0] = 0;
			cdvdWriteNVM(&cdvd.SCMDResultBuff[0], getNvmLayout()->mac, 8);
			break;

		case 0x3E: /* [__sceCdWriteRegionParams - made up name] (15:1) [Florin: hum, i was expecting 14:1] */
			cdvd.SCMDResultCnt  = 1;
			cdvd.SCMDResultPos  = 0;
			cdvd.sDataIn       &= ~0x40;
			cdvd.SCMDResultBuff[0] = 0;
			cdvdWriteNVM(&cdvd.SCMDParamBuff[2], getNvmLayout()->regparams, 8);
			break;

		case 0x40: /* CdOpenConfig (3:1) */
			cdvd.SCMDResultCnt  = 1;
			cdvd.SCMDResultPos  = 0;
			cdvd.sDataIn       &= ~0x40;
			cdvd.CReadWrite = cdvd.SCMDParamBuff[0];
			cdvd.COffset = cdvd.SCMDParamBuff[1];
			cdvd.CNumBlocks = cdvd.SCMDParamBuff[2];
			cdvd.CBlockIndex = 0;
			cdvd.SCMDResultBuff[0] = 0;
			break;

		case 0x41: /* CdReadConfig (0:16) */
			cdvd.SCMDResultCnt  = 16;
			cdvd.SCMDResultPos  = 0;
			cdvd.sDataIn       &= ~0x40;
			memset(&cdvd.SCMDResultBuff[0], 0, 16);
			cdvdReadConfig(&cdvd.SCMDResultBuff[0]);
			break;

		case 0x42: /* CdWriteConfig (16:1) */
			cdvd.SCMDResultCnt  = 1;
			cdvd.SCMDResultPos  = 0;
			cdvd.sDataIn       &= ~0x40;
			cdvd.SCMDResultBuff[0] = 0;
			cdvdWriteConfig(&cdvd.SCMDParamBuff[0]);
			break;

		case 0x43: /* CdCloseConfig (0:1) */
			cdvd.SCMDResultCnt  = 1;
			cdvd.SCMDResultPos  = 0;
			cdvd.sDataIn       &= ~0x40;
			cdvd.CReadWrite = 0;
			cdvd.COffset = 0;
			cdvd.CNumBlocks = 0;
			cdvd.CBlockIndex = 0;
			cdvd.SCMDResultBuff[0] = 0;
			break;

		case 0x80: /* secrman: __mechacon_auth_0x80 */
		case 0x81: /* secrman: __mechacon_auth_0x81 */
			cdvd.SCMDResultCnt  = 1;
			cdvd.SCMDResultPos  = 0;
			cdvd.sDataIn       &= ~0x40;
			cdvd.mg_datatype = 0; /* data */
			cdvd.SCMDResultBuff[0] = 0;
			break;

		case 0x82: /* secrman: __mechacon_auth_0x82 */
		case 0x83: /* secrman: __mechacon_auth_0x83 */
			cdvd.SCMDResultCnt  = 1;
			cdvd.SCMDResultPos  = 0;
			cdvd.sDataIn       &= ~0x40;
			cdvd.SCMDResultBuff[0] = 0;
			break;

		case 0x84: /* secrman: __mechacon_auth_0x84 */
			cdvd.SCMDResultCnt  = 13;
			cdvd.SCMDResultPos  = 0;
			cdvd.sDataIn       &= ~0x40;
			cdvd.SCMDResultBuff[0] = 0;

			cdvd.SCMDResultBuff[1] = 0x21;
			cdvd.SCMDResultBuff[2] = 0xdc;
			cdvd.SCMDResultBuff[3] = 0x31;
			cdvd.SCMDResultBuff[4] = 0x96;
			cdvd.SCMDResultBuff[5] = 0xce;
			cdvd.SCMDResultBuff[6] = 0x72;
			cdvd.SCMDResultBuff[7] = 0xe0;
			cdvd.SCMDResultBuff[8] = 0xc8;

			cdvd.SCMDResultBuff[9] = 0x69;
			cdvd.SCMDResultBuff[10] = 0xda;
			cdvd.SCMDResultBuff[11] = 0x34;
			cdvd.SCMDResultBuff[12] = 0x9b;
			break;

		case 0x85: /* secrman: __mechacon_auth_0x85 */
			cdvd.SCMDResultCnt  = 13;
			cdvd.SCMDResultPos  = 0;
			cdvd.sDataIn       &= ~0x40;
			cdvd.SCMDResultBuff[0] = 0;

			cdvd.SCMDResultBuff[1] = 0xeb;
			cdvd.SCMDResultBuff[2] = 0x01;
			cdvd.SCMDResultBuff[3] = 0xc7;
			cdvd.SCMDResultBuff[4] = 0xa9;

			cdvd.SCMDResultBuff[5] = 0x3f;
			cdvd.SCMDResultBuff[6] = 0x9c;
			cdvd.SCMDResultBuff[7] = 0x5b;
			cdvd.SCMDResultBuff[8] = 0x19;
			cdvd.SCMDResultBuff[9] = 0x31;
			cdvd.SCMDResultBuff[10] = 0xa0;
			cdvd.SCMDResultBuff[11] = 0xb3;
			cdvd.SCMDResultBuff[12] = 0xa3;
			break;

		case 0x86: /* secrman: __mechacon_auth_0x86 */
		case 0x87: /* secrman: __mechacon_auth_0x87 */
			cdvd.SCMDResultCnt  = 1;
			cdvd.SCMDResultPos  = 0;
			cdvd.sDataIn       &= ~0x40;
			cdvd.SCMDResultBuff[0] = 0;
			break;

		case 0x8D: /* sceMgWriteData */
			cdvd.SCMDResultCnt  = 1;
			cdvd.SCMDResultPos  = 0;
			cdvd.sDataIn       &= ~0x40;
			if (cdvd.mg_size + cdvd.SCMDParamCnt > cdvd.mg_maxsize)
				cdvd.SCMDResultBuff[0] = 0x80;
			else
			{
				memcpy(&cdvd.mg_buffer[cdvd.mg_size], cdvd.SCMDParamBuff, cdvd.SCMDParamCnt);
				cdvd.mg_size += cdvd.SCMDParamCnt;
				cdvd.SCMDResultBuff[0] = 0; /* 0 complete ; 1 busy ; 0x80 error */
			}
			break;

		case 0x8E: /* sceMgReadData */
			{
				u8 _siz = pcsx2_min_i(16, cdvd.mg_size);
				cdvd.SCMDResultCnt  = _siz;
				cdvd.SCMDResultPos  = 0;
				cdvd.sDataIn       &= ~0x40;
				memset(&cdvd.SCMDResultBuff[0], 0, _siz);
				memcpy(&cdvd.SCMDResultBuff[0], &cdvd.mg_buffer[0], cdvd.SCMDResultCnt);
				cdvd.mg_size -= cdvd.SCMDResultCnt;
				memcpy(&cdvd.mg_buffer[0], &cdvd.mg_buffer[cdvd.SCMDResultCnt], cdvd.mg_size);
			}
			break;

		case 0x88: /* secrman: __mechacon_auth_0x88	//for now it is the same; so, fall;) */
		case 0x8F: /* secrman: __mechacon_auth_0x8F */
			cdvd.SCMDResultCnt  = 1;
			cdvd.SCMDResultPos  = 0;
			cdvd.sDataIn       &= ~0x40;
			if (cdvd.mg_datatype == 1) /* header data */
			{
				int bit_ofs;

				if ((cdvd.mg_maxsize != cdvd.mg_size) || (cdvd.mg_size < 0x20) || (cdvd.mg_size != *(u16*)&cdvd.mg_buffer[0x14]))
				{
					cdvd.SCMDResultBuff[0] = 0x80;
					break;
				}

				bit_ofs = mg_BIToffset(&cdvd.mg_buffer[0]);

				/* bit_ofs is derived from guest-controlled MG data. It indexes
				 * mg_buffer at [bit_ofs - 0x20 .. bit_ofs + 7]; bound it so a
				 * crafted buffer cannot under/overflow mg_buffer. */
				if (bit_ofs < 0x20 || (bit_ofs + 8) > (int)sizeof(cdvd.mg_buffer))
				{
					cdvd.SCMDResultBuff[0] = 0x80;
					break;
				}

				memcpy(&cdvd.mg_kbit[0], &cdvd.mg_buffer[bit_ofs - 0x20], 0x10);
				memcpy(&cdvd.mg_kcon[0], &cdvd.mg_buffer[bit_ofs - 0x10], 0x10);

				if ((cdvd.mg_buffer[bit_ofs + 5] || cdvd.mg_buffer[bit_ofs + 6] || cdvd.mg_buffer[bit_ofs + 7]) ||
						(cdvd.mg_buffer[bit_ofs + 4] * 16 + bit_ofs + 8 + 16 != *(u16*)&cdvd.mg_buffer[0x14]))
				{
					cdvd.SCMDResultBuff[0] = 0x80;
					break;
				}
			}
			cdvd.SCMDResultBuff[0] = 0; /* 0 complete ; 1 busy ; 0x80 error */
			break;

		case 0x90: /* sceMgWriteHeaderStart */
			cdvd.SCMDResultCnt  = 1;
			cdvd.SCMDResultPos  = 0;
			cdvd.sDataIn       &= ~0x40;
			cdvd.mg_size = 0;
			cdvd.mg_datatype = 1; /* header data */

			cdvd.SCMDResultBuff[0] = 0; /* 0 complete ; 1 busy ; 0x80 error */
			break;

		case 0x91: /* sceMgReadBITLength */
			{
				int bit_ofs, copy_len;
				cdvd.SCMDResultCnt  = 3;
				cdvd.SCMDResultPos  = 0;
				cdvd.sDataIn       &= ~0x40;
				bit_ofs = mg_BIToffset(&cdvd.mg_buffer[0]);

				/* bit_ofs and the copy length are derived from the
				 * guest-controlled MG buffer; validate both against the
				 * buffer bounds before copying so a crafted buffer cannot
				 * trigger an out-of-bounds read (host memory disclosure). */
				if (bit_ofs < 0 || (bit_ofs + 4) >= (int)sizeof(cdvd.mg_buffer))
				{
					cdvd.SCMDResultBuff[0] = 0x80;
					cdvd.SCMDResultBuff[1] = 0;
					cdvd.SCMDResultBuff[2] = 0;
					break;
				}

				copy_len = 8 + 16 * ((int)(cdvd.mg_buffer[bit_ofs + 4]));
				if (copy_len < 0 || (bit_ofs + copy_len) > (int)sizeof(cdvd.mg_buffer))
				{
					cdvd.SCMDResultBuff[0] = 0x80;
					cdvd.SCMDResultBuff[1] = 0;
					cdvd.SCMDResultBuff[2] = 0;
					break;
				}

				memcpy(&cdvd.mg_buffer[0], &cdvd.mg_buffer[bit_ofs], ((size_t)(copy_len)));

				cdvd.mg_maxsize = 0; /* don't allow any write */
				cdvd.mg_size = 8 + 16 * cdvd.mg_buffer[4]; /* new offset, i just moved the data */

				cdvd.SCMDResultBuff[0] = (cdvd.mg_datatype == 1) ? 0 : 0x80; /* 0 complete ; 1 busy ; 0x80 error */
				cdvd.SCMDResultBuff[1] = (cdvd.mg_size >> 0) & 0xFF;
				cdvd.SCMDResultBuff[2] = (cdvd.mg_size >> 8) & 0xFF;
				break;
			}
		case 0x92: /* sceMgWriteDatainLength */
			cdvd.SCMDResultCnt     = 1;
			cdvd.SCMDResultPos     = 0;
			cdvd.sDataIn          &= ~0x40;
			cdvd.mg_size           = 0;
			cdvd.mg_datatype       = 0; /* data (encrypted) */
			cdvd.mg_maxsize        = cdvd.SCMDParamBuff[0] | (((int)cdvd.SCMDParamBuff[1]) << 8);
			cdvd.SCMDResultBuff[0] = 0; /* 0 complete ; 1 busy ; 0x80 error */
			break;

		case 0x93: /* sceMgWriteDataoutLength */
			cdvd.SCMDResultCnt  = 1;
			cdvd.SCMDResultPos  = 0;
			cdvd.sDataIn       &= ~0x40;
			if (((cdvd.SCMDParamBuff[0] | (((int)cdvd.SCMDParamBuff[1]) << 8)) == cdvd.mg_size) && (cdvd.mg_datatype == 0))
			{
				cdvd.mg_maxsize = 0; /* don't allow any write */
				cdvd.SCMDResultBuff[0] = 0; /* 0 complete ; 1 busy ; 0x80 error */
			}
			else
				cdvd.SCMDResultBuff[0] = 0x80;
			break;

		case 0x94: /* sceMgReadKbit - read first half of BIT key */
			cdvd.SCMDResultCnt  = 9;
			cdvd.SCMDResultPos  = 0;
			cdvd.sDataIn       &= ~0x40;
			memset(&cdvd.SCMDResultBuff[0], 0, 9);
			memcpy(&cdvd.SCMDResultBuff[1], cdvd.mg_kbit, 8);
			break;

		case 0x95: /* sceMgReadKbit2 - read second half of BIT key */
			cdvd.SCMDResultCnt  = 9;
			cdvd.SCMDResultPos  = 0;
			cdvd.sDataIn       &= ~0x40;
			memset(&cdvd.SCMDResultBuff[0], 0, 9);
			memcpy(&cdvd.SCMDResultBuff[1], cdvd.mg_kbit + 8, 8);
			break;

		case 0x96: /* sceMgReadKcon - read first half of content key */
			cdvd.SCMDResultCnt  = 9;
			cdvd.SCMDResultPos  = 0;
			cdvd.sDataIn       &= ~0x40;
			memset(&cdvd.SCMDResultBuff[0], 0, 9);
			memcpy(&cdvd.SCMDResultBuff[1], cdvd.mg_kcon, 8);
			break;

		case 0x97: /* sceMgReadKcon2 - read second half of content key */
			cdvd.SCMDResultCnt  = 9;
			cdvd.SCMDResultPos  = 0;
			cdvd.sDataIn       &= ~0x40;
			memset(&cdvd.SCMDResultBuff[0], 0, 9);
			memcpy(&cdvd.SCMDResultBuff[1], cdvd.mg_kcon + 8, 8);
			break;

		default:
			cdvd.SCMDResultCnt  = 1;
			cdvd.SCMDResultPos  = 0;
			cdvd.sDataIn       &= ~0x40;
			cdvd.SCMDResultBuff[0] = 0x80; /* 0 complete ; 1 busy ; 0x80 error */
			break;
	} /* end switch */

	cdvd.SCMDParamPos = 0;
	cdvd.SCMDParamCnt = 0;
}

static INLINE void cdvdWrite17(u8 rt) /* SDATAIN */
{
	if (cdvd.SCMDParamPos >= 16)
	{
		cdvd.SCMDParamPos = 0;
		cdvd.SCMDParamCnt = 0;
	}

	cdvd.SCMDParamBuff[cdvd.SCMDParamPos++] = rt;
	cdvd.SCMDParamCnt++;
}

void cdvdWrite(u8 key, u8 rt)
{
	switch (key)
	{
		case 0x04:
			cdvdWrite04(rt);
			break;
		case 0x05:
			cdvdWrite05(rt);
			break;
		case 0x06:
			cdvd.HowTo = rt;
			break;
		case 0x07:
			cdvdWrite07(rt);
			break;
		case 0x08:
			cdvd.IntrStat &= ~rt;
			break;
		case 0x0A: /* STATUS */
			break;
		case 0x0F: /* TYPE */
			break;
		case 0x14:
			/* Rama Or WISI guessed that "2" literally meant 2x but we can get */
			/* 0x02 or 0xfe for "Standard" or "Fast" it appears. It is unsure what those values are meant to be */
			/* Tests with ref suggest this register is write only? - Weirdbeard */
			break;
		case 0x16:
			cdvdWrite16(rt);
			break;
		case 0x17:
			cdvdWrite17(rt);
			break;
		case 0x18: /* SDATAOUT */
			break;
		case 0x3A: /* DEC-SET */
			cdvd.decSet = rt;
			break;
		default:
			break;
	}
}
