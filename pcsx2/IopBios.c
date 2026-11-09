/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2022  PCSX2 Dev Team
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

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <time.h> /* localtime */
#include <sys/stat.h>

#include "common/Pcsx2Defs.h"
#include <compat/strl.h>
#include <file/file_path.h>
#include <streams/file_stream.h>
#ifdef _WIN32
#include <encodings/utf.h>
#endif

#include "HostFS.h"
#include "Common.h"
#include "R3000A.h"
#include "R5900.h" /* for g_GameStarted */
#include "IopMem.h"
#include "IopBios.h"

#if !defined(S_ISREG) && defined(S_IFMT) && defined(S_IFREG)
#define S_ISREG(m) (((m)&S_IFMT) == S_IFREG)
#endif
#if !defined(S_ISDIR) && defined(S_IFMT) && defined(S_IFDIR)
#define S_ISDIR(m) (((m)&S_IFMT) == S_IFDIR)
#endif

typedef struct
{
	uint32_t mode;
	uint32_t attr;
	uint32_t size;
	uint8_t ctime[8];
	uint8_t atime[8];
	uint8_t mtime[8];
	uint32_t hisize;
} fio_stat_t;
typedef struct
{
	fio_stat_t _fioStat;
	/** Number of subs (main) / subpart number (sub) */
	uint32_t private_0;
	uint32_t private_1;
	uint32_t private_2;
	uint32_t private_3;
	uint32_t private_4;
	/** Sector start.  */
	uint32_t private_5;
} fxio_stat_t;

typedef struct
{
	fio_stat_t stat;
	char name[256];
	uint32_t unknown;
} fio_dirent_t;

typedef struct
{
	fxio_stat_t stat;
	char name[256];
	uint32_t unknown;
} fxio_dirent_t;

static char hostRoot[PCSX2_PATH_MAX];

/* Lexical canonicalise: a port of the since-removed Path::Canonicalize,
 * checked against it in tests/path/canon_test.c over that table and
 * 200,000 generated paths before the original was deleted. */
static void iop_canonicalize(char *s)
{
	char       *comps[128];
	int         ncomp   = 0;
	int         total   = 0;
	char       *w;
	int         i;
	char       *starts[128];
	int         nstart  = 0;

	/* Split on the native separator, including the empty leading
	 * component for an absolute path (the rule the since-removed
	 * Path::SplitNativePath followed). */
	{
		size_t start = 0, pos = 0;
		const size_t len = strlen(s);

		while (pos < len)
		{
			if (s[pos] != '/')
			{
				pos++;
				continue;
			}
			if (pos != start || pos == 0)
			{
				s[pos] = '\0';
				if (nstart < 128)
					starts[nstart++] = s + start;
			}
			pos++;
			start = pos;
		}
		if (start != pos && nstart < 128)
			starts[nstart++] = s + start;
	}
	total = nstart;

	for (i = 0; i < total; i++)
	{
		char *c = starts[i];

		if (!strcmp(c, "."))
		{
			if (total == 1)
				comps[ncomp++] = c;
			continue;
		}
		if (!strcmp(c, ".."))
		{
			if (ncomp > 0)
				ncomp--;
			else if (ncomp < 128)
				comps[ncomp++] = c;
			continue;
		}
		if (ncomp < 128)
			comps[ncomp++] = c;
	}

	w = s;
	for (i = 0; i < ncomp; i++)
	{
		const size_t len = strlen(comps[i]);

		if (i)
			*w++ = '/';
		memmove(w, comps[i], len);
		w += len;
	}
	*w = '\0';
}



void Hle_SetElfPath(const char* elfFileName)
{
	strlcpy(hostRoot, elfFileName, sizeof(hostRoot));
	path_basedir(hostRoot);
	/* path_basedir leaves a trailing separator; the sandbox comparison
	 * below expects the root without one. */
	{
		const size_t n = strlen(hostRoot);
		if (n > 1 && hostRoot[n - 1] == FS_OSPATH_SEPARATOR_CHARACTER)
			hostRoot[n - 1] = '\0';
	}
	log_cb(RETRO_LOG_INFO, "HLE Host: Set 'host:' root path to: %s\n\n", hostRoot);
}

void Hle_ClearElfPath(void)
{
	hostRoot[0] = '\0';
}

#define v0 (psxRegs.GPR.n.v0)
#define a0 (psxRegs.GPR.n.a0)
#define a1 (psxRegs.GPR.n.a1)
#define a2 (psxRegs.GPR.n.a2)
#define a3 (psxRegs.GPR.n.a3)
#define sp (psxRegs.GPR.n.sp)
#define ra (psxRegs.GPR.n.ra)
#define pc (psxRegs.pc)

/* Guest strings into caller buffers. */
#define Ra0_BUF(buf) iopMemReadStringBuf((buf), (int)sizeof(buf), a0, 65536)
#define Ra1_BUF(buf) iopMemReadStringBuf((buf), (int)sizeof(buf), a1, 65536)

/* Stat values differ between iomanX and ioman. These values have been
 * taken from the PS2SDK, specifically iox_stat.h. */
typedef struct fio_stat_flags
{
	/* Access flags: execute, write, read */
	int IXOTH;
	int IWOTH;
	int IROTH;

	/* File mode flags: symlink, regular file, directory */
	int IFLNK;
	int IFREG;
	int IFDIR;
} fio_stat_flags;

static const fio_stat_flags ioman_stat = {
	0x01,
	0x02,
	0x04,
	0x08,
	0x10,
	0x20,
};

static const fio_stat_flags iomanx_stat = {
	0x01,
	0x02,
	0x04,
	0x4000,
	0x2000,
	0x1000,
};

/* Resolve a guest path under hostRoot, or yield an empty string if it
 * escapes. Writes into the caller's buffer; no allocation.
 *
 * The HLE calls run on the IOP's thread, one at a time and never
 * nested, so their path buffers here and below are static rather than
 * several KB of stack each. */
static void host_path(char* out, size_t out_size, const char* path, int allow_open_host_root)
{
	static char   native[PCSX2_PATH_MAX];
	static char   canonical[PCSX2_PATH_MAX];
	size_t root_len;

	out[0] = '\0';
	strlcpy(native, path, sizeof(native));
	iop_canonicalize(native);

	root_len = strlen(hostRoot);
	if (root_len == 0)
		return;

	if (!strncmp(native, hostRoot, root_len))
		strlcpy(out, native, out_size);
	else
		pcsx2_path_join(out, out_size, hostRoot, native);

	/* Double-check that it falls within the directory of the elf.
	 * Not a real sandbox, but emulators shouldn't be treated as such.
	 * Don't run untrusted code! */
	strlcpy(canonical, out, sizeof(canonical));
	iop_canonicalize(canonical);

	/* Opening the root of host (`host:.` or `host:`) is allowed as a
	 * directory open but not as a file open. */
	if (!allow_open_host_root || strcmp(canonical, hostRoot))
	{
		if (   strlen(canonical) <= root_len
		    || strncmp(canonical, hostRoot, root_len)
		    || canonical[root_len] != FS_OSPATH_SEPARATOR_CHARACTER)
		{
			log_cb(RETRO_LOG_ERROR, "IopHLE: Denying access to path outside of ELF directory. Requested path: '{%s}', Resolved path: '{%s}', ELF directory: '{%s}'\n",
				path, out, hostRoot);
			out[0] = '\0';
		}
	}
}

/* This is a workaround for GHS on *NIX platforms
 * Whenever a program splits directories with a backslash (ulaunchelf)
 * the directory is considered non-existant */
static __fi void clean_path(char* out, size_t out_size, const char* path)
{
	char* p;

	strlcpy(out, path, out_size);
	for (p = out; *p; p++)
	{
		if (*p == '\\')
			*p = '/';
	}
}

/* What the fio stat HLE needs of a host file: libretro's VFS stat (size
 * and a directory flag) cannot carry the mode bits and the three times. */
typedef struct host_file_stat
{
	unsigned mode;    /* the host's st_mode */
	u32      size;    /* st_size as the guest's 32-bit field takes it */
	time_t   ctime_;
	time_t   atime_;
	time_t   mtime_;
} host_file_stat;

#ifdef _WIN32
static bool host_stat_file(const char* path, host_file_stat* st)
{
	struct _stat64 st64;
	wchar_t* wpath;
	if (path[0] == '\0')
		return false;
	wpath = utf8_to_utf16_string_alloc(path);
	if (!wpath)
		return false;
	if (_wstat64(wpath, &st64) != 0)
	{
		free(wpath);
		return false;
	}
	free(wpath);
	st->mode = st64.st_mode;
	/* The 64-bit size saturates at the 32-bit signed size struct stat
	 * carries on Windows, which this went through before. */
	st->size = (st64.st_size > 0x7fffffff) ? 0x7fffffffu : (u32)st64.st_size;
	st->ctime_ = (time_t)st64.st_ctime;
	st->atime_ = (time_t)st64.st_atime;
	st->mtime_ = (time_t)st64.st_mtime;
	return true;
}
#else
static bool host_stat_file(const char* path, host_file_stat* st)
{
	struct stat s;
	if (path[0] == '\0')
		return false;
	if (stat(path, &s) != 0)
		return false;
	st->mode = (unsigned)s.st_mode;
	st->size = (u32)s.st_size;
	st->ctime_ = s.st_ctime;
	st->atime_ = s.st_atime;
	st->mtime_ = s.st_mtime;
	return true;
}
#endif

/* A host time into the guest's 8-byte form; nothing when the host cannot
 * break the time down. */
static void host_time(uint8_t out[8], const time_t* t)
{
	const struct tm* lt = localtime(t);
	if (!lt)
		return;
	out[6] = (unsigned char)lt->tm_year;
	out[5] = (unsigned char)lt->tm_mon + 1;
	out[4] = (unsigned char)lt->tm_mday;
	out[3] = (unsigned char)lt->tm_hour;
	out[2] = (unsigned char)lt->tm_min;
	out[1] = (unsigned char)lt->tm_sec;
}

static int host_stat(const char* path, fio_stat_t* host_stats, const fio_stat_flags* flags)
{
	host_file_stat file_stats;
	char file_path[PCSX2_PATH_MAX];

	host_path(file_path, sizeof(file_path), path, 1);

	if (!host_stat_file(file_path, &file_stats))
		return -IOP_ENOENT;

	host_stats->size = file_stats.size;
	host_stats->hisize = 0;

	/* Convert the mode. */
	host_stats->mode = (file_stats.mode & (flags->IROTH | flags->IWOTH | flags->IXOTH));
#ifndef _WIN32
	if (S_ISLNK(file_stats.mode))
		host_stats->mode |= flags->IFLNK;
#endif
	if (S_ISREG(file_stats.mode))
		host_stats->mode |= flags->IFREG;
	if (S_ISDIR(file_stats.mode))
		host_stats->mode |= flags->IFDIR;

	host_time(host_stats->ctime, &file_stats.ctime_);
	host_time(host_stats->atime, &file_stats.atime_);
	host_time(host_stats->mtime, &file_stats.mtime_);

	return 0;
}

static int host_statx(const char* path, fxio_stat_t* host_stats)
{
	return host_stat(path, &host_stats->_fioStat, &iomanx_stat);
}

/* ---- host: files --------------------------------------------------------
 *
 * host: I/O goes through the libretro VFS rather than open(2). The
 * frontend can then serve these paths itself -- on Android a content://
 * URI is not something open(2) can take at all. A file is its RFILE. */

/* TODO: sandbox option, other permissions */
static int host_file_open(RFILE** file, const char* full_path, s32 flags)
{
	const char* colon = strchr(full_path, ':');
	char file_path[PCSX2_PATH_MAX];
	unsigned vfs_mode;
	RFILE* fp;

	host_path(file_path, sizeof(file_path), colon ? colon + 1 : full_path, 0);

	switch (flags & IOP_O_RDWR)
	{
		case IOP_O_WRONLY:
			vfs_mode = RETRO_VFS_FILE_ACCESS_WRITE;
			break;
		case IOP_O_RDWR:
			vfs_mode = RETRO_VFS_FILE_ACCESS_READ_WRITE;
			break;
		case IOP_O_RDONLY:
		default:
			vfs_mode = RETRO_VFS_FILE_ACCESS_READ;
			break;
	}

	/* O_TRUNC is the VFS default for a writable open; without O_TRUNC the
	 * existing contents have to be kept, and O_APPEND needs them kept as
	 * well before the seek to the end below. */
	if ((vfs_mode & RETRO_VFS_FILE_ACCESS_WRITE) &&
	    !(flags & IOP_O_TRUNC))
		vfs_mode |= RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING;

	/* A writable VFS open creates the file, unless it is to keep the
	 * contents: that one opens only a file that exists. IOP_O_CREAT
	 * without IOP_O_TRUNC therefore drops the keeping when there is
	 * nothing to keep, or the open fails where it should create. A
	 * read-only open of a missing file fails below either way. */
	if ((vfs_mode & RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING) &&
	    (flags & IOP_O_CREAT) && !path_is_valid(file_path))
		vfs_mode &= ~RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING;

	fp = filestream_open(file_path, vfs_mode,
			RETRO_VFS_FILE_ACCESS_HINT_NONE);
	if (!fp)
		return -IOP_ENOENT;

	if (flags & IOP_O_APPEND)
		filestream_seek(fp, 0, RETRO_VFS_SEEK_POSITION_END);

	*file = fp;
	return 0;
}

static int host_file_lseek(RFILE* fp, s32 offset, s32 whence)
{
	int seek_position;

	switch (whence)
	{
		case IOP_SEEK_SET:
			seek_position = RETRO_VFS_SEEK_POSITION_START;
			break;
		case IOP_SEEK_CUR:
			seek_position = RETRO_VFS_SEEK_POSITION_CURRENT;
			break;
		case IOP_SEEK_END:
			seek_position = RETRO_VFS_SEEK_POSITION_END;
			break;
		default:
			return -IOP_EIO;
	}

	/* lseek answers the new position. filestream_seek answers 0 for
	 * success, which this used to hand the guest as the position. */
	if (filestream_seek(fp, offset, seek_position) < 0)
		return -IOP_EIO;
	return (int)filestream_tell(fp);
}

/* ---- host: directories -------------------------------------------------- */

typedef struct HostDir
{
	FILESYSTEM_FIND_RESULTS results; /* owned */
	size_t pos;
	char basedir[PCSX2_PATH_MAX];
} HostDir;

static int host_dir_open(HostDir** dir, const char* full_path)
{
	const char* colon = strchr(full_path, ':');
	static char path[PCSX2_PATH_MAX];
	FILESYSTEM_FIND_RESULTS results;
	HostDir* d;

	host_path(path, sizeof(path), colon ? colon + 1 : full_path, 1);

	if (!path_is_directory(path))
		return -IOP_ENOENT; /* Should return ENOTDIR if path is a file? */

	memset(&results, 0, sizeof(results));
	FileSystem_FindFiles(path, "*", FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_FOLDERS | FILESYSTEM_FIND_RELATIVE_PATHS | FILESYSTEM_FIND_HIDDEN_FILES, &results);

	d = (HostDir*)malloc(sizeof(*d));
	if (!d)
	{
		FileSystem_FreeFindResults(&results);
		return -IOP_ENOMEM;
	}
	d->results = results;
	d->pos     = 0;
	strlcpy(d->basedir, path, sizeof(d->basedir));
	*dir = d;
	return 0;
}

static void host_dir_close(HostDir* dir)
{
	FileSystem_FreeFindResults(&dir->results);
	free(dir);
}

/* The next entry's name, and its stat when the joined path fits: a
 * truncated join names some other file, and an empty stat beats a stat of
 * the wrong path. */
static int host_dir_read(HostDir* dir, char* name, size_t name_size, fio_stat_t* stat, fxio_stat_t* statx)
{
	static char joined[PCSX2_PATH_MAX];
	const char* entry;
	size_t base_len, entry_len;

	if (dir->pos == dir->results.count)
		return 0;

	entry = dir->results.items[dir->pos].FileName;
	strlcpy(name, entry, name_size);

	base_len  = strlen(dir->basedir);
	entry_len = strlen(entry);
	if (base_len + 1 + entry_len < sizeof(joined))
	{
		memcpy(joined, dir->basedir, base_len);
		joined[base_len] = '/';
		memcpy(joined + base_len + 1, entry, entry_len + 1);
		if (statx)
			host_statx(joined, statx);
		else
			host_stat(joined, stat, &ioman_stat);
	}

	dir->pos++;
	return 1;
}

/* ---- ioman ---------------------------------------------------------------- */

#define IOMAN_FIRSTFD 0x100
#define IOMAN_MAXFDS  0x100

enum
{
	FILE_FREE,
	FILE_FILE,
	FILE_DIR
};

typedef struct filedesc
{
	int type;
	RFILE* file;
	HostDir* dir;
} filedesc;

static filedesc ioman_fds[IOMAN_MAXFDS];
static int ioman_openfds;

static int ioman_freefdcount(void)
{
	return IOMAN_MAXFDS - ioman_openfds;
}

static void ioman_fd_close(filedesc* fd)
{
	switch (fd->type)
	{
		case FILE_FILE:
			filestream_close(fd->file);
			fd->file = NULL;
			break;
		case FILE_DIR:
			host_dir_close(fd->dir);
			fd->dir = NULL;
			break;
		default:
			return;
	}

	fd->type = FILE_FREE;
	ioman_openfds--;
}

/* The open file or directory behind a guest fd, or NULL when the fd is out
 * of range or holds the other kind. */
static RFILE* ioman_getfile(int fd)
{
	fd -= IOMAN_FIRSTFD;
	if (fd < 0 || fd >= IOMAN_MAXFDS || ioman_fds[fd].type != FILE_FILE)
		return NULL;
	return ioman_fds[fd].file;
}

static HostDir* ioman_getdir(int fd)
{
	fd -= IOMAN_FIRSTFD;
	if (fd < 0 || fd >= IOMAN_MAXFDS || ioman_fds[fd].type != FILE_DIR)
		return NULL;
	return ioman_fds[fd].dir;
}

/* A free fd for file or dir (one of them set); closes it when there is
 * none. */
static int ioman_allocfd(RFILE* file, HostDir* dir)
{
	int i;
	for (i = 0; i < IOMAN_MAXFDS; i++)
	{
		if (ioman_fds[i].type == FILE_FREE)
		{
			ioman_fds[i].type = file ? FILE_FILE : FILE_DIR;
			ioman_fds[i].file = file;
			ioman_fds[i].dir  = dir;
			ioman_openfds++;
			return IOMAN_FIRSTFD + i;
		}
	}

	if (file)
		filestream_close(file);
	else
		host_dir_close(dir);
	return -IOP_EMFILE;
}

static void ioman_freefd(int fd)
{
	fd -= IOMAN_FIRSTFD;

	if (fd < 0 || fd >= IOMAN_MAXFDS)
		return;

	ioman_fd_close(&ioman_fds[fd]);
}

void irxIomanReset(void)
{
	int i;
	for (i = 0; i < IOMAN_MAXFDS; i++)
		ioman_fd_close(&ioman_fds[i]);
}

/* "host:", or "host0:" through "host9...:" -- the digits between the name
 * and the colon are a unit number and are ignored. */
static int is_host(const char* path)
{
	size_t i;

	if (strncmp(path, "host", 4))
		return 0;

	for (i = 4; path[i] >= '0' && path[i] <= '9'; i++)
		;
	if (path[i] != ':')
		return 0;

	return (!g_GameStarted || emu_config_host_fs());
}

static int ioman_open_HLE(void)
{
	RFILE* file = NULL;
	static char path_raw[PCSX2_PATH_MAX];
	static char path[PCSX2_PATH_MAX];
	s32 flags;
	int err;

	Ra0_BUF(path_raw);
	clean_path(path, sizeof(path), path_raw);
	flags = a1;

	if (!is_host(path))
		return 0;

	if (!ioman_freefdcount())
	{
		v0 = -IOP_EMFILE;
		pc = ra;
		return 1;
	}

	err = host_file_open(&file, path, flags);
	if (err != 0 || !file)
		v0 = err ? err : -IOP_EIO;
	else
		v0 = ioman_allocfd(file, NULL);

	pc = ra;
	return 1;
}

static int ioman_close_HLE(void)
{
	s32 fd = a0;

	if (!ioman_getfile(fd))
		return 0;

	ioman_freefd(fd);
	v0 = 0;
	pc = ra;
	return 1;
}

static int ioman_dopen_HLE(void)
{
	HostDir* dir = NULL;
	static char path_raw[PCSX2_PATH_MAX];
	static char path[PCSX2_PATH_MAX];
	int err;

	Ra0_BUF(path_raw);
	clean_path(path, sizeof(path), path_raw);

	if (!is_host(path))
		return 0;

	err = host_dir_open(&dir, path);
	if (err != 0 || !dir)
		v0 = err ? err : -IOP_EIO;
	else
		v0 = ioman_allocfd(NULL, dir);

	pc = ra;
	return 1;
}

static int ioman_dclose_HLE(void)
{
	s32 fd = a0;

	if (!ioman_getdir(fd))
		return 0;

	ioman_freefd(fd);
	v0 = 0;
	pc = ra;
	return 1;
}

/* Bytes into guest memory one at a time, as the guest's own stores would
 * land: a range may run over RAM, a mirror or a register. */
static void guest_write(u32 data, const void* src, u32 count)
{
	const u8* b = (const u8*)src;
	u32 i;
	for (i = 0; i < count; i++)
		iopMemWrite8(data + i, b[i]);
}

static int ioman_dread(bool iomanX)
{
	s32 fh = a0;
	u32 data = a1;
	HostDir* dir = ioman_getdir(fh);

	if (!dir)
		return 0;

	/* Cleared, so an entry whose stat fails or whose name is short hands
	 * the guest zeros rather than whatever the stack held. */
	if (iomanX)
	{
		fxio_dirent_t ent;
		memset(&ent, 0, sizeof(ent));
		v0 = host_dir_read(dir, ent.name, sizeof(ent.name), NULL, &ent.stat);
		guest_write(data, &ent, sizeof(ent));
	}
	else
	{
		fio_dirent_t ent;
		memset(&ent, 0, sizeof(ent));
		v0 = host_dir_read(dir, ent.name, sizeof(ent.name), &ent.stat, NULL);
		guest_write(data, &ent, sizeof(ent));
	}

	pc = ra;
	return 1;
}

static int ioman_dread_HLE(void)
{
	return ioman_dread(false);
}

static int ioman_dreadx_HLE(void)
{
	return ioman_dread(true);
}

static int ioman_getStat(bool iomanx)
{
	static char path_raw[PCSX2_PATH_MAX];
	static char path[PCSX2_PATH_MAX];
	static char full_path[PCSX2_PATH_MAX];
	const char* colon;
	u32 data;

	Ra0_BUF(path_raw);
	clean_path(path, sizeof(path), path_raw);
	data = a1;

	if (!is_host(path))
		return 0;

	colon = strchr(path, ':');
	host_path(full_path, sizeof(full_path), colon ? colon + 1 : path, 1);
	/* Cleared, as a failed stat leaves it unwritten. */
	if (iomanx)
	{
		fxio_stat_t st;
		memset(&st, 0, sizeof(st));
		v0 = host_statx(full_path, &st);
		guest_write(data, &st, sizeof(st));
	}
	else
	{
		fio_stat_t st;
		memset(&st, 0, sizeof(st));
		v0 = host_stat(full_path, &st, &ioman_stat);
		guest_write(data, &st, sizeof(st));
	}
	pc = ra;
	return 1;
}

static int ioman_getStat_HLE(void)
{
	return ioman_getStat(false);
}

static int ioman_getStatx_HLE(void)
{
	return ioman_getStat(true);
}

static int ioman_lseek_HLE(void)
{
	s32 fd = a0;
	s32 offset = a1;
	s32 whence = a2;
	RFILE* file = ioman_getfile(fd);

	if (!file)
		return 0;

	v0 = host_file_lseek(file, offset, whence);
	pc = ra;
	return 1;
}

static int ioman_remove_HLE(void)
{
	static char full_path_raw[PCSX2_PATH_MAX];
	static char full_path[PCSX2_PATH_MAX];
	static char file_path[PCSX2_PATH_MAX];
	const char* colon;
	bool succeeded;

	Ra0_BUF(full_path_raw);
	clean_path(full_path, sizeof(full_path), full_path_raw);

	if (!is_host(full_path))
		return 0;

	colon = strchr(full_path, ':');
	host_path(file_path, sizeof(file_path), colon ? colon + 1 : full_path, 0);
	/* The PS2 kernel's remove refuses directories; keep that split here,
	 * since filestream_delete itself would take an empty directory out.
	 * Guard and delete go through the same VFS, so both see the same
	 * filesystem. */
	succeeded = !path_is_directory(file_path) &&
		filestream_delete(file_path) == 0;
	if (!succeeded)
		log_cb(RETRO_LOG_WARN, "IOPHLE remove_HLE failed for '%s'\n", file_path);
	v0 = succeeded ? 0 : -IOP_EIO;
	pc = ra;
	/* Handled, as mkdir and rmdir are. It used to answer 0 after setting
	 * the return address, so the caller went on into the guest's own
	 * remove from there, with its result overwriting this one. */
	return 1;
}

static int ioman_mkdir_HLE(void)
{
	static char full_path_raw[PCSX2_PATH_MAX];
	static char full_path[PCSX2_PATH_MAX];
	static char folder_path[PCSX2_PATH_MAX];
	const char* colon;
	bool succeeded;

	Ra0_BUF(full_path_raw);
	clean_path(full_path, sizeof(full_path), full_path_raw);

	if (!is_host(full_path))
		return 0;

	colon = strchr(full_path, ':');
	/* NOTE: Don't allow creating the ELF directory. */
	host_path(folder_path, sizeof(folder_path), colon ? colon + 1 : full_path, 0);
	succeeded = path_mkdir(folder_path);
	if (!succeeded)
		log_cb(RETRO_LOG_WARN, "IOPHLE mkdir_HLE failed for '%s'\n", folder_path);
	v0 = succeeded ? 0 : -IOP_EIO;
	pc = ra;
	return 1;
}

static int ioman_rmdir_HLE(void)
{
	static char full_path_raw[PCSX2_PATH_MAX];
	static char full_path[PCSX2_PATH_MAX];
	static char folder_path[PCSX2_PATH_MAX];
	const char* colon;
	bool succeeded;

	Ra0_BUF(full_path_raw);
	clean_path(full_path, sizeof(full_path), full_path_raw);

	if (!is_host(full_path))
		return 0;

	colon = strchr(full_path, ':');
	/* NOTE: Don't allow removing the elf directory itself. */
	host_path(folder_path, sizeof(folder_path), colon ? colon + 1 : full_path, 0);
	/* rmdir's inverse guard: only a directory may go. */
	succeeded = path_is_directory(folder_path) &&
		filestream_delete(folder_path) == 0;
	if (!succeeded)
		log_cb(RETRO_LOG_WARN, "IOPHLE rmdir_HLE failed for '%s'\n", folder_path);
	v0 = succeeded ? 0 : -IOP_EIO;
	pc = ra;
	return 1;
}

/* read and write move through this, a chunk at a time: the guest's count
 * is its own, and a buffer of that size every call -- up to 4 GiB -- was
 * an allocation the guest chose. */
static u8 s_io_chunk[64 * 1024];

static int ioman_read_HLE(void)
{
	s32 fd = a0;
	u32 data = a1;
	u32 count = a2;
	RFILE* file = ioman_getfile(fd);
	u32 total = 0;
	int failed = 0;

	if (!file)
		return 0;

	while (total < count)
	{
		const u32 want = (count - total < (u32)sizeof(s_io_chunk)) ? count - total : (u32)sizeof(s_io_chunk);
		const int64_t r = filestream_read(file, s_io_chunk, want);
		if (r < 0)
		{
			failed = 1;
			break;
		}
		guest_write(data + total, s_io_chunk, (u32)r);
		total += (u32)r;
		if ((u32)r < want)
			break;
	}

	v0 = (failed && total == 0) ? (u32)-IOP_EIO : total;
	pc = ra;
	return 1;
}

static int ioman_write_HLE(void)
{
	s32 fd = a0;
	u32 data = a1;
	u32 count = a2;
	RFILE* file;
	u32 total = 0;
	int failed = 0;

	if (fd == 1) /* stdout */
	{
		static char s[PCSX2_PATH_MAX];
		Ra1_BUF(s);
		pc = ra;
		v0 = a2;
		return 1;
	}

	file = ioman_getfile(fd);
	if (!file)
		return 0;

	while (total < count)
	{
		const u32 want = (count - total < (u32)sizeof(s_io_chunk)) ? count - total : (u32)sizeof(s_io_chunk);
		int64_t w;
		u32 i;
		for (i = 0; i < want; i++)
			s_io_chunk[i] = iopMemRead8(data + total + i);
		w = filestream_write(file, s_io_chunk, want);
		if (w < 0)
		{
			failed = 1;
			break;
		}
		total += (u32)w;
		if ((u32)w < want)
			break;
	}

	v0 = (failed && total == 0) ? (u32)-IOP_EIO : total;
	pc = ra;
	return 1;
}

/* ---- the other modules --------------------------------------------------- */

static int sysmem_Kprintf_HLE(void)
{
	/* Emulate the expected Kprintf functionality: */
	iopMemWrite32(sp, a0);
	iopMemWrite32(sp + 4, a1);
	iopMemWrite32(sp + 8, a2);
	iopMemWrite32(sp + 12, a3);
	pc = ra;

	return 1;
}

static int loadcore_RegisterLibraryEntries_HLE(void)
{
	return 0;
}

static void loadcore_RegisterLibraryEntries_DEBUG(void)
{
}

static void intrman_RegisterIntrHandler_DEBUG(void)
{
}

static void sifcmd_sceSifRegisterRpc_DEBUG(void)
{
}

/* ---- the import tables --------------------------------------------------- */

void irxImportName(u32 table, u32* name0, u32* name1)
{
	u8 raw[8];
	u32 w0 = 0, w1 = 0;
	int i, end = 8;
	for (i = 0; i < 8; i++)
		raw[i] = iopMemRead8(table + 12 + (u32)i);
	for (i = 0; i < 8; i++)
		if (raw[i] == 0) { end = i; break; }
	for (i = 0; i < 4 && i < end; i++)
		w0 |= (u32)raw[i] << (8 * i);
	for (i = 4; i < 8 && i < end; i++)
		w1 |= (u32)raw[i] << (8 * (i - 4));
	*name0 = w0;
	*name1 = w1;
}

u32 irxImportTableAddr(u32 entrypc)
{
	u32 i;

	i = entrypc - 0x18;
	while (entrypc - i < 0x2000)
	{
		if (iopMemRead32(i) == 0x41e00000)
			return i;
		i -= 4;
	}

	return 0;
}

/* The two words of an 8-byte module name, as irxImportName reads them:
 * little-endian, zero past the name. */
#define IRX_W(c0, c1, c2, c3) \
	((u32)(unsigned char)(c0) | ((u32)(unsigned char)(c1) << 8) \
	 | ((u32)(unsigned char)(c2) << 16) | ((u32)(unsigned char)(c3) << 24))

#define IRX_SYSMEM0   IRX_W('s', 'y', 's', 'm')
#define IRX_SYSMEM1   IRX_W('e', 'm', 0, 0)
#define IRX_LOADCORE0 IRX_W('l', 'o', 'a', 'd')
#define IRX_LOADCORE1 IRX_W('c', 'o', 'r', 'e')
#define IRX_IOMAN0    IRX_W('i', 'o', 'm', 'a')
#define IRX_IOMAN1    IRX_W('n', 0, 0, 0)
#define IRX_IOMANX1   IRX_W('n', 'x', 0, 0)
#define IRX_INTRMAN0  IRX_W('i', 'n', 't', 'r')
#define IRX_INTRMAN1  IRX_W('m', 'a', 'n', 0)
#define IRX_SIFCMD0   IRX_W('s', 'i', 'f', 'c')
#define IRX_SIFCMD1   IRX_W('m', 'd', 0, 0)

irxHLE irxImportHLE(u32 name0, u32 name1, u16 index)
{
	if (name0 == IRX_SYSMEM0 && name1 == IRX_SYSMEM1)
	{
		if (index == 14)
			return sysmem_Kprintf_HLE;
	}
	else if (name0 == IRX_LOADCORE0 && name1 == IRX_LOADCORE1)
	{
		/* For grabbing the thread list from thbase */
		if (index == 6)
			return loadcore_RegisterLibraryEntries_HLE;
	}
	/* ioman and iomanX are mostly compatible, excluding the stat
	 * structures */
	else if (name0 == IRX_IOMAN0 && (name1 == IRX_IOMAN1 || name1 == IRX_IOMANX1))
	{
		const bool use_ioman = name1 == IRX_IOMAN1;
		switch (index)
		{
			case 4:  return ioman_open_HLE;
			case 5:  return ioman_close_HLE;
			case 6:  return ioman_read_HLE;
			case 7:  return ioman_write_HLE;
			case 8:  return ioman_lseek_HLE;
			case 10: return ioman_remove_HLE;
			case 11: return ioman_mkdir_HLE;
			case 12: return ioman_rmdir_HLE;
			case 13: return ioman_dopen_HLE;
			case 14: return ioman_dclose_HLE;
			case 15: return use_ioman ? ioman_dread_HLE : ioman_dreadx_HLE;
			case 16: return use_ioman ? ioman_getStat_HLE : ioman_getStatx_HLE;
			default: break;
		}
	}
	return NULL;
}

irxDEBUG irxImportDebug(u32 name0, u32 name1, u16 index)
{
	if (name0 == IRX_LOADCORE0 && name1 == IRX_LOADCORE1)
	{
		if (index == 6)
			return loadcore_RegisterLibraryEntries_DEBUG;
	}
	else if (name0 == IRX_INTRMAN0 && name1 == IRX_INTRMAN1)
	{
		if (index == 4)
			return intrman_RegisterIntrHandler_DEBUG;
	}
	else if (name0 == IRX_SIFCMD0 && name1 == IRX_SIFCMD1)
	{
		if (index == 17)
			return sifcmd_sceSifRegisterRpc_DEBUG;
	}
	return NULL;
}

int irxImportExec(u32 import_table, u16 index)
{
	u32 n0, n1;
	irxHLE hle;
	irxDEBUG debug;

	if (!import_table)
		return 0;

	irxImportName(import_table, &n0, &n1);
	hle   = irxImportHLE(n0, n1, index);
	debug = irxImportDebug(n0, n1, index);

	if (debug)
		debug();

	if (hle)
		return hle();
	return 0;
}

/* Per-stub cache of the import resolution. psxJ runs the module-import
 * stub's hook on every execution: a backward scan of up to 0x2000 bytes
 * for the table magic and a compare chain on the name. The resolution
 * depends only on the table magic, the 8-byte name and the index, so it is
 * cached per stub pc and those inputs revalidated with three RAM reads per
 * hit. A module loaded over the stub rewrites name or magic and fails the
 * revalidation, which re-resolves. A stub with no table is not cached:
 * there is nothing stable to revalidate against.
 *
 * The cache is a fixed open-addressing table: a multiply-shift hash,
 * linear probing, no allocation. A stub pc is 4-byte aligned, so the hash
 * starts from pc >> 2. A full probe window overwrites its first slot --
 * this is a cache, and a lost entry is a re-resolve, never a wrong answer.
 * Empty is stub 0, the IOP's reset vector and never a J stub. */
typedef struct JStubCacheEntry
{
	u32 stub;         /* key: the stub pc; 0 = empty                   */
	u32 table;
	u32 name0, name1; /* raw words as read: revalidation compares raw */
	u16 index;
	irxHLE hle;
	irxDEBUG debug;
} JStubCacheEntry;

/* 1024 slots for a few hundred stubs at most: low load, short probes. */
#define JSTUB_CACHE_BITS  10
#define JSTUB_CACHE_SIZE  (1 << JSTUB_CACHE_BITS)
#define JSTUB_CACHE_PROBE 8
static JStubCacheEntry s_jstub_cache[JSTUB_CACHE_SIZE];

static __fi u32 jstub_hash(u32 stub)
{
	/* Fibonacci hashing: the 32-bit golden-ratio multiplier spreads the
	 * sequential stub addresses of one import table across the slots; the
	 * top bits are the best-mixed ones. */
	return ((stub >> 2) * 0x9E3779B9u) >> (32 - JSTUB_CACHE_BITS);
}

/* The slot holding stub, or NULL. */
static __fi JStubCacheEntry* jstub_find(u32 stub)
{
	u32 i = jstub_hash(stub);
	u32 n;
	for (n = 0; n < JSTUB_CACHE_PROBE; n++)
	{
		JStubCacheEntry* e = &s_jstub_cache[(i + n) & (JSTUB_CACHE_SIZE - 1)];
		if (e->stub == stub)
			return e;
		if (e->stub == 0)
			return NULL;
	}
	return NULL;
}

/* The slot to write stub into: its own if present, else the first empty in
 * the window, else the window's first slot (evict). */
static __fi JStubCacheEntry* jstub_slot(u32 stub)
{
	u32 i = jstub_hash(stub);
	u32 n;
	for (n = 0; n < JSTUB_CACHE_PROBE; n++)
	{
		JStubCacheEntry* e = &s_jstub_cache[(i + n) & (JSTUB_CACHE_SIZE - 1)];
		if (e->stub == stub || e->stub == 0)
			return e;
	}
	return &s_jstub_cache[i & (JSTUB_CACHE_SIZE - 1)];
}

int irxImportExecCached(u32 stubpc, u16 index)
{
	JStubCacheEntry* e = jstub_find(stubpc);
	u32 table;

	if (e)
	{
		if (e->index == index &&
			iopMemRead32(e->table) == 0x41e00000 &&
			iopMemRead32(e->table + 12) == e->name0 &&
			iopMemRead32(e->table + 16) == e->name1)
		{
			if (e->debug)
				e->debug();
			if (e->hle)
				return e->hle();
			return 0;
		}
		/* Stale: fall through and overwrite this slot below. */
	}
	table = irxImportTableAddr(stubpc);
	if (!table)
		return 0;
	if (!e)
		e = jstub_slot(stubpc);
	e->stub  = stubpc;
	e->table = table;
	e->name0 = iopMemRead32(table + 12);
	e->name1 = iopMemRead32(table + 16);
	e->index = index;
	{
		u32 n0, n1;
		irxImportName(table, &n0, &n1);
		e->hle   = irxImportHLE(n0, n1, index);
		e->debug = irxImportDebug(n0, n1, index);
	}
	if (e->debug)
		e->debug();
	if (e->hle)
		return e->hle();
	return 0;
}
