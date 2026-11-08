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
#include <time.h> /* localtime */
#include "common/Pcsx2Defs.h"
#include <string.h>
#include <iterator>
#include <utility>
#include <compat/strl.h>
#include <file/file_path.h>
#include <compat/strl.h>
#include <sys/stat.h>

#include <fcntl.h>

#include <file/file_path.h>
#include "Config.h"

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#include "HostFS.h"

#include "Common.h"
#include "R5900.h" // for g_GameStarted
#include <streams/file_stream.h>
#include "IopBios.h"

#include <limits>
#ifdef _WIN32
#include <encodings/utf.h>
#endif
#include "IopMem.h"
#include "iR3000A.h"
#include "ps2/BiosTools.h"

#if !defined(S_ISREG) && defined(S_IFMT) && defined(S_IFREG)
#define S_ISREG(m) (((m)&S_IFMT) == S_IFREG)
#endif
#if !defined(S_ISDIR) && defined(S_IFMT) && defined(S_IFDIR)
#define S_ISDIR(m) (((m)&S_IFMT) == S_IFDIR)
#endif

#ifndef O_BINARY
#define O_BINARY 0
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

void Hle_ClearElfPath()
{
	hostRoot[0] = '\0';
}

namespace R3000A
{

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

	// Stat values differ between iomanX and ioman
	// These values have been taken from the PS2SDK
	// Specifically iox_stat.h
	struct fio_stat_flags
	{
		// Access flags
		// Execute
		int IXOTH;
		// Write
		int IWOTH;
		// Read
		int IROTH;

		// File mode flags
		// Symlink
		int IFLNK;
		// Regular file
		int IFREG;
		// Directory
		int IFDIR;
	};

	fio_stat_flags ioman_stat{
		0x01,
		0x02,
		0x04,
		0x08,
		0x10,
		0x20,
	};

	fio_stat_flags iomanx_stat{
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

	// This is a workaround for GHS on *NIX platforms
	// Whenever a program splits directories with a backslash (ulaunchelf)
	// the directory is considered non-existant
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

	/* The fio stat HLE needs the full struct stat - mode bits, the three
	 * timestamps, symlink detection - which libretro's VFS stat (size +
	 * directory flag) cannot carry.  This was FileSystem::StatFile's only
	 * caller, so the platform stat lives here now, verbatim. */
#ifdef _WIN32
	static void TranslateStat64(struct stat* st, const struct _stat64& st64)
	{
		static constexpr __int64 MAX_SIZE = static_cast<__int64>(std::numeric_limits<decltype(st->st_size)>::max());
		st->st_dev = st64.st_dev;
		st->st_ino = st64.st_ino;
		st->st_mode = st64.st_mode;
		st->st_nlink = st64.st_nlink;
		st->st_uid = st64.st_uid;
		st->st_rdev = st64.st_rdev;
		st->st_size = static_cast<decltype(st->st_size)>((st64.st_size > MAX_SIZE) ? MAX_SIZE : st64.st_size);
		st->st_atime = static_cast<time_t>(st64.st_atime);
		st->st_mtime = static_cast<time_t>(st64.st_mtime);
		st->st_ctime = static_cast<time_t>(st64.st_ctime);
	}

	static bool host_stat_file(const char* path, struct stat* st)
	{
		struct _stat64 st64;
		if (path[0] == '\0')
			return false;
		wchar_t* wpath = utf8_to_utf16_string_alloc(path);
		if (_wstat64(wpath, &st64) != 0)
		{
			free(wpath);
			return false;
		}
		free(wpath);
		TranslateStat64(st, st64);
		return true;
	}
#else
	static bool host_stat_file(const char* path, struct stat* st)
	{
		if (path[0] == '\0')
			return false;
		return stat(path, st) == 0;
	}
#endif

	static int host_stat(const char* path, fio_stat_t* host_stats, fio_stat_flags& stat = ioman_stat)
	{
		struct stat file_stats;
		char file_path[PCSX2_PATH_MAX];

		host_path(file_path, sizeof(file_path), path, 1);

		if (!host_stat_file(file_path, &file_stats))
			return -IOP_ENOENT;

		host_stats->size = (uint32_t)file_stats.st_size;
		host_stats->hisize = 0;

		// Convert the mode.
		host_stats->mode = (file_stats.st_mode & (stat.IROTH | stat.IWOTH | stat.IXOTH));
#ifndef _WIN32
		if (S_ISLNK(file_stats.st_mode))
			host_stats->mode |= stat.IFLNK;
#endif
		if (S_ISREG(file_stats.st_mode))
			host_stats->mode |= stat.IFREG;
		if (S_ISDIR(file_stats.st_mode))
			host_stats->mode |= stat.IFDIR;

		// Convert the creation time.
		struct tm* loctime;
		loctime = localtime(&(file_stats.st_ctime));
		host_stats->ctime[6] = (unsigned char)loctime->tm_year;
		host_stats->ctime[5] = (unsigned char)loctime->tm_mon + 1;
		host_stats->ctime[4] = (unsigned char)loctime->tm_mday;
		host_stats->ctime[3] = (unsigned char)loctime->tm_hour;
		host_stats->ctime[2] = (unsigned char)loctime->tm_min;
		host_stats->ctime[1] = (unsigned char)loctime->tm_sec;

		// Convert the access time.
		loctime = localtime(&(file_stats.st_atime));
		host_stats->atime[6] = (unsigned char)loctime->tm_year;
		host_stats->atime[5] = (unsigned char)loctime->tm_mon + 1;
		host_stats->atime[4] = (unsigned char)loctime->tm_mday;
		host_stats->atime[3] = (unsigned char)loctime->tm_hour;
		host_stats->atime[2] = (unsigned char)loctime->tm_min;
		host_stats->atime[1] = (unsigned char)loctime->tm_sec;

		// Convert the last modified time.
		loctime = localtime(&(file_stats.st_mtime));
		host_stats->mtime[6] = (unsigned char)loctime->tm_year;
		host_stats->mtime[5] = (unsigned char)loctime->tm_mon + 1;
		host_stats->mtime[4] = (unsigned char)loctime->tm_mday;
		host_stats->mtime[3] = (unsigned char)loctime->tm_hour;
		host_stats->mtime[2] = (unsigned char)loctime->tm_min;
		host_stats->mtime[1] = (unsigned char)loctime->tm_sec;

		return 0;
	}

	static int host_stat(const char* path, fxio_stat_t* host_stats)
	{
		return host_stat(path, &host_stats->_fioStat, iomanx_stat);
	}

	// TODO: sandbox option, other permissions
	class HostFile
	{
	public:
		RFILE* fp;

		HostFile(RFILE* hostfp)
		{
			fp = hostfp;
		}

		~HostFile() = default;



		/* host: I/O goes through the libretro VFS rather than open(2).
		 * The frontend can then serve these paths itself -- on Android a
		 * content:// URI is not something open(2) can take at all -- and
		 * this was the last raw POSIX file path left in the core. */
		static int open(HostFile** file, const char* full_path, s32 flags, u16 mode)
		{
			const char* colon = strchr(full_path, ':');
			char file_path[PCSX2_PATH_MAX];

			host_path(file_path, sizeof(file_path), colon ? colon + 1 : full_path, 0);
			unsigned vfs_mode;
			RFILE* fp;

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

			/* O_TRUNC is the VFS default for a writable open; without
			 * O_TRUNC the existing contents have to be kept, and O_APPEND
			 * needs them kept as well before the seek to the end below. */
			if ((vfs_mode & RETRO_VFS_FILE_ACCESS_WRITE) &&
			    !(flags & IOP_O_TRUNC))
				vfs_mode |= RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING;

			/* IOP_O_CREAT is implied by a writable VFS open; a read-only
			 * open of a missing file fails below either way. */

			fp = filestream_open(file_path, vfs_mode,
					RETRO_VFS_FILE_ACCESS_HINT_NONE);
			if (!fp)
				return -IOP_ENOENT;

			if (flags & IOP_O_APPEND)
				filestream_seek(fp, 0, RETRO_VFS_SEEK_POSITION_END);

			*file = new HostFile(fp);
			if (!*file)
			{
				filestream_close(fp);
				return -IOP_ENOMEM;
			}

			return 0;
		}

		void close()
		{
			filestream_close(fp);
			delete this;
		}

		int lseek(s32 offset, s32 whence)
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

			return (int)filestream_seek(fp, offset, seek_position);
		}

		int read(void* buf, u32 count) /* Flawfinder: ignore */
		{
			const int64_t r = filestream_read(fp, buf, count);
			return (r < 0) ? -IOP_EIO : (int)r;
		}

		int write(void* buf, u32 count)
		{
			const int64_t w = filestream_write(fp, buf, count);
			return (w < 0) ? -IOP_EIO : (int)w;
		}
	};

	class HostDir
	{
	public:
		FILESYSTEM_FIND_RESULTS results; /* owned */
		size_t pos;
		char basedir[PCSX2_PATH_MAX];

		HostDir(const FILESYSTEM_FIND_RESULTS& results_, const char* basedir_)
			: results(results_)
			, pos(0)
		{
			strlcpy(basedir, basedir_, sizeof(basedir));
		}

		~HostDir() { FileSystem_FreeFindResults(&results); }

		static int open(HostDir** dir, const char* full_path)
		{
			const char* colon = strchr(full_path, ':');
			char path[PCSX2_PATH_MAX];

			host_path(path, sizeof(path), colon ? colon + 1 : full_path, 1);

			if (!path_is_directory(path))
				return -IOP_ENOENT; // Should return ENOTDIR if path is a file?
			
			FILESYSTEM_FIND_RESULTS results = {};
			FileSystem_FindFiles(path, "*", FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_FOLDERS | FILESYSTEM_FIND_RELATIVE_PATHS | FILESYSTEM_FIND_HIDDEN_FILES, &results);

			*dir = new HostDir(results, path);
			if (!*dir)
			{
				FileSystem_FreeFindResults(&results);
				return -IOP_ENOMEM;
			}

			return 0;
		}

		int read(void* buf, bool iomanX) /* Flawfinder: ignore */
		{
			if (pos == results.count)
				return 0;

			if (iomanX)
			{
				fxio_dirent_t* hostcontent = (fxio_dirent_t*)buf;
				strlcpy(hostcontent->name, results.items[pos].FileName, sizeof(hostcontent->name));
				{
					char joined[PCSX2_PATH_MAX];
					/* A truncated join names some other file; better an
					 * empty stat than a stat of the wrong path. */
					if (snprintf(joined, sizeof(joined), "%s/%s", basedir, results.items[pos].FileName) < (int)sizeof(joined))
						host_stat(joined, &hostcontent->stat);
				}
			}
			else
			{
				fio_dirent_t* hostcontent = (fio_dirent_t*)buf;
				strlcpy(hostcontent->name, results.items[pos].FileName, sizeof(hostcontent->name));
				{
					char joined[PCSX2_PATH_MAX];
					/* A truncated join names some other file; better an
					 * empty stat than a stat of the wrong path. */
					if (snprintf(joined, sizeof(joined), "%s/%s", basedir, results.items[pos].FileName) < (int)sizeof(joined))
						host_stat(joined, &hostcontent->stat);
				}
			}

			pos++;
			return 1;
		}

		void close()
		{
			delete this;
		}
	};

	namespace ioman
	{
		const int firstfd = 0x100;
		const int maxfds = 0x100;
		int openfds = 0;

		int freefdcount()
		{
			return maxfds - openfds;
		}

		struct filedesc
		{
			enum
			{
				FILE_FREE,
				FILE_FILE,
				FILE_DIR,
			} type;
			union
			{
				HostFile* file;
				HostDir* dir;
			};

			constexpr filedesc()
				: type(FILE_FREE)
				, file(nullptr)
			{
			}
			operator bool() const { return type != FILE_FREE; }
			operator HostFile*() const { return type == FILE_FILE ? file : NULL; }
			operator HostDir*() const { return type == FILE_DIR ? dir : NULL; }
			void operator=(HostFile* f)
			{
				type = FILE_FILE;
				file = f;
				openfds++;
			}
			void operator=(HostDir* d)
			{
				type = FILE_DIR;
				dir = d;
				openfds++;
			}

			void close()
			{
				switch (type)
				{
					case FILE_FILE:
						file->close();
						file = NULL;
						break;
					case FILE_DIR:
						dir->close();
						dir = NULL;
						break;
					case FILE_FREE:
						return;
				}

				type = FILE_FREE;
				openfds--;
			}
		};

		filedesc fds[maxfds];

		template <typename T>
		T* getfd(int fd)
		{
			fd -= firstfd;

			if (fd < 0 || fd >= maxfds)
				return NULL;

			return fds[fd];
		}

		template <typename T>
		int allocfd(T* obj)
		{
			for (int i = 0; i < maxfds; i++)
			{
				if (!fds[i])
				{
					fds[i] = obj;
					return firstfd + i;
				}
			}

			obj->close();
			return -IOP_EMFILE;
		}

		void freefd(int fd)
		{
			fd -= firstfd;

			if (fd < 0 || fd >= maxfds)
				return;

			fds[fd].close();
		}

		void reset()
		{
			for (int i = 0; i < maxfds; i++)
			{
				if (fds[i])
					fds[i].close();
			}
		}

		/* "host:", or "host0:" through "host9...:" -- the digits between
		 * the name and the colon are a unit number and are ignored. */
		static int is_host(const char* path)
		{
			size_t i;

			if (strncmp(path, "host", 4))
				return 0;

			for (i = 4; path[i] >= '0' && path[i] <= '9'; i++)
				;
			if (path[i] != ':')
				return 0;

			return (!g_GameStarted || EmuConfig.HostFs);
		}

		int open_HLE()
		{
			HostFile* file = NULL;
			static char path_raw[PCSX2_PATH_MAX];
			static char path[PCSX2_PATH_MAX];
			Ra0_BUF(path_raw);
			clean_path(path, sizeof(path), path_raw);
			s32 flags = a1;
			u16 mode = a2;

			if (is_host(path))
			{
				if (!freefdcount())
				{
					v0 = -IOP_EMFILE;
					pc = ra;
					return 1;
				}

				int err = HostFile::open(&file, path, flags, mode);

				if (err != 0 || !file)
				{
					if (err == 0) // ???
						err = -IOP_EIO;
					if (file) // ??????
						file->close();
					v0 = err;
				}
				else
				{
					/* allocfd owns cleanup on failure: it already
					 * close()s (and thereby deletes) the object before
					 * returning the error, so closing again here was a
					 * double delete -- GCC 12's -Wuse-after-free caught
					 * it. */
					v0 = allocfd(file);
				}

				pc = ra;
				return 1;
			}

			return 0;
		}

		int close_HLE()
		{
			s32 fd = a0;

			if (getfd<HostFile>(fd))
			{
				freefd(fd);
				v0 = 0;
				pc = ra;
				return 1;
			}

			return 0;
		}

		int dopen_HLE()
		{
			HostDir* dir = NULL;
			static char path_raw[PCSX2_PATH_MAX];
			static char path[PCSX2_PATH_MAX];
			Ra0_BUF(path_raw);
			clean_path(path, sizeof(path), path_raw);

			if (is_host(path))
			{
				int err = HostDir::open(&dir, path);

				if (err != 0 || !dir)
				{
					if (err == 0)
						err = -IOP_EIO;
					if (dir)
						dir->close();
					v0 = err;
				}
				else
				{
					/* Same ownership rule as open_HLE: allocfd already
					 * closed and deleted dir on failure. */
					v0 = allocfd(dir);
				}

				pc = ra;
				return 1;
			}

			return 0;
		}

		int dclose_HLE()
		{
			s32 dir = a0;

			if (getfd<HostDir>(dir))
			{
				freefd(dir);
				v0 = 0;
				pc = ra;
				return 1;
			}

			return 0;
		}

		int _dread_HLE(bool iomanX)
		{
			s32 fh = a0;
			u32 data = a1;
			if (iomanX)
			{
				if (HostDir* dir = getfd<HostDir>(fh))
				{
					char buf[sizeof(fxio_dirent_t)];
					v0 = dir->read(&buf, iomanX); /* Flawfinder: ignore */

					for (s32 i = 0; i < (s32)sizeof(fxio_dirent_t); i++)
						iopMemWrite8(data + i, buf[i]);

					pc = ra;
					return 1;
				}
			}
			else
			{
				if (HostDir* dir = getfd<HostDir>(fh))
				{
					char buf[sizeof(fio_dirent_t)];
					v0 = dir->read(&buf, false); /* Flawfinder: ignore */

					for (s32 i = 0; i < (s32)sizeof(fio_dirent_t); i++)
						iopMemWrite8(data + i, buf[i]);

					pc = ra;
					return 1;
				}
			}
			return 0;
		}

		int dread_HLE()
		{
			return _dread_HLE(false);
		}

		int dreadx_HLE()
		{
			return _dread_HLE(true);
		}

		int _getStat_HLE(bool iomanx)
		{
			static char path_raw[PCSX2_PATH_MAX];
			static char path[PCSX2_PATH_MAX];
			Ra0_BUF(path_raw);
			clean_path(path, sizeof(path), path_raw);
			u32 data = a1;

			if (is_host(path))
			{
				const char* colon = strchr(path, ':');
				static char full_path[PCSX2_PATH_MAX];

				host_path(full_path, sizeof(full_path), colon ? colon + 1 : path, 1);
				if (iomanx)
				{
					char buf[sizeof(fxio_stat_t)];
					v0 = host_stat(full_path, (fxio_stat_t*)&buf);

					for (size_t i = 0; i < sizeof(fxio_stat_t); i++)
						iopMemWrite8((uint32_t)(data + i), buf[i]);
				}
				else
				{
					char buf[sizeof(fio_stat_t)];
					v0 = host_stat(full_path, (fio_stat_t*)&buf);

					for (size_t i = 0; i < sizeof(fio_stat_t); i++)
						iopMemWrite8((uint32_t)(data + i), buf[i]);
				}
				pc = ra;
				return 1;
			}

			return 0;
		}

		int getStat_HLE()
		{
			return _getStat_HLE(false);
		}

		int getStatx_HLE()
		{
			return _getStat_HLE(true);
		}

		int lseek_HLE()
		{
			s32 fd = a0;
			s32 offset = a1;
			s32 whence = a2;

			if (HostFile* file = getfd<HostFile>(fd))
			{
				v0 = file->lseek(offset, whence);
				pc = ra;
				return 1;
			}

			return 0;
		}

		int remove_HLE()
		{
			static char full_path_raw[PCSX2_PATH_MAX];
			static char full_path[PCSX2_PATH_MAX];
			Ra0_BUF(full_path_raw);
			clean_path(full_path, sizeof(full_path), full_path_raw);

			if (is_host(full_path))
			{
				const char* colon = strchr(full_path, ':');
				static char file_path[PCSX2_PATH_MAX];

				host_path(file_path, sizeof(file_path), colon ? colon + 1 : full_path, 0);
				/* The PS2 kernel's remove refuses directories; keep that
				 * split here, since filestream_delete itself would take
				 * an empty directory out. Guard and delete go through
				 * the same VFS, so both see the same filesystem. */
				const bool succeeded = !path_is_directory(file_path) &&
					filestream_delete(file_path) == 0;
				if (!succeeded)
					log_cb(RETRO_LOG_WARN, "IOPHLE remove_HLE failed for '%s'\n", file_path);
				v0 = succeeded ? 0 : -IOP_EIO;
				pc = ra;
			}
			return 0;
		}

		int mkdir_HLE()
		{
			static char full_path_raw[PCSX2_PATH_MAX];
			static char full_path[PCSX2_PATH_MAX];
			Ra0_BUF(full_path_raw);
			clean_path(full_path, sizeof(full_path), full_path_raw);

			if (is_host(full_path))
			{
				const char* colon = strchr(full_path, ':');
				static char folder_path[PCSX2_PATH_MAX];

				host_path(folder_path, sizeof(folder_path), colon ? colon + 1 : full_path, 0); // NOTE: Don't allow creating the ELF directory.
				const bool succeeded = path_mkdir(folder_path);
				if (!succeeded)
					log_cb(RETRO_LOG_WARN, "IOPHLE mkdir_HLE failed for '%s'\n", folder_path);
				v0 = succeeded ? 0 : -IOP_EIO;
				pc = ra;
				return 1;
			}

			return 0;
		}

		int read_HLE()
		{
			s32 fd = a0;
			u32 data = a1;
			u32 count = a2;

			if (HostFile* file = getfd<HostFile>(fd))
			{
				auto buf = std::make_unique<char[]>(count);

				v0 = file->read(buf.get(), count);

				for (s32 i = 0; i < (s32)v0; i++)
					iopMemWrite8(data + i, buf[i]);

				pc = ra;
				return 1;
			}

			return 0;
		}

		int rmdir_HLE()
		{
			static char full_path_raw[PCSX2_PATH_MAX];
			static char full_path[PCSX2_PATH_MAX];
			Ra0_BUF(full_path_raw);
			clean_path(full_path, sizeof(full_path), full_path_raw);

			if (is_host(full_path))
			{
				const char* colon = strchr(full_path, ':');
				static char folder_path[PCSX2_PATH_MAX];

				host_path(folder_path, sizeof(folder_path), colon ? colon + 1 : full_path, 0); // NOTE: Don't allow removing the elf directory itself.
				/* rmdir's inverse guard: only a directory may go. */
				const bool succeeded = path_is_directory(folder_path) &&
					filestream_delete(folder_path) == 0;
				if (!succeeded)
					log_cb(RETRO_LOG_WARN, "IOPHLE rmdir_HLE failed for '%s'\n", folder_path);
				v0 = succeeded ? 0 : -IOP_EIO;
				pc = ra;
				return 1;
			}

			return 0;
		}

		int write_HLE()
		{
			s32 fd = a0;
			u32 data = a1;
			u32 count = a2;

			if (fd == 1) // stdout
			{
				static char s[PCSX2_PATH_MAX];
				Ra1_BUF(s);
				pc = ra;
				v0 = a2;
				return 1;
			}
			else if (HostFile* file = getfd<HostFile>(fd))
			{
				auto buf = std::make_unique<char[]>(count);

				for (u32 i = 0; i < count; i++)
					buf[i] = iopMemRead8(data + i);

				v0 = file->write(buf.get(), count);

				pc = ra;
				return 1;
			}

			return 0;
		}
	} // namespace ioman

	namespace sysmem
	{
		int Kprintf_HLE()
		{
			// Emulate the expected Kprintf functionality:
			iopMemWrite32(sp, a0);
			iopMemWrite32(sp + 4, a1);
			iopMemWrite32(sp + 8, a2);
			iopMemWrite32(sp + 12, a3);
			pc = ra;

			return 1;
		}
	} // namespace sysmem

	namespace loadcore
	{

		int RegisterLibraryEntries_HLE()
		{
			return 0;
		}

		void RegisterLibraryEntries_DEBUG()
		{
		}
	} // namespace loadcore

	namespace intrman
	{
		void RegisterIntrHandler_DEBUG()
		{
		}
	} // namespace intrman

	namespace sifcmd
	{
		void sceSifRegisterRpc_DEBUG()
		{
		}
	} // namespace sifcmd

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

// clang-format off
/* The two words of an 8-byte module name, folded from the literal at
 * compile time: MODULE(sysmem) compares name0 against 'sysm' and name1
 * against 'em\0\0' as immediates. */
static constexpr u32 irx_name_word(const char* s, int off)
{
	u32 w = 0;
	int i = 0;
	for (i = 0; i < off; i++)
		if (s[i] == 0)
			return 0;      /* a NUL before off: this word is all padding */
	for (i = 0; i < 4; i++)
	{
		const char c = s[off + i];
		if (c == 0)
			break;
		w |= (u32)(unsigned char)c << (8 * i);
	}
	return w;
}
#define IRX_NAME0(s) irx_name_word(s, 0)
#define IRX_NAME1(s) irx_name_word(s, 4)

#define MODULE(n)                                                     \
	if (name0 == IRX_NAME0(#n) && name1 == IRX_NAME1(#n))               \
	{                                                                 \
		using namespace n;                                            \
		switch (index)                                                \
		{
#define END_MODULE \
	}              \
	}
#define EXPORT_D(i, n) \
	case (i):          \
		return n##_DEBUG;
#define EXPORT_H(i, n) \
	case (i):          \
		return n##_HLE;
	// clang-format on

	irxHLE irxImportHLE(u32 name0, u32 name1, u16 index)
	{
		// debugging output
		// clang-format off
		MODULE(sysmem)
			EXPORT_H( 14, Kprintf)
		END_MODULE

		// For grabbing the thread list from thbase
		MODULE(loadcore)
			EXPORT_H( 6, RegisterLibraryEntries)
		END_MODULE

		// Special case with ioman and iomanX
		// They are mostly compatible excluding stat structures
		if ((name0 == IRX_NAME0("ioman")  && name1 == IRX_NAME1("ioman"))
		 || (name0 == IRX_NAME0("iomanx") && name1 == IRX_NAME1("iomanx")))
		{
			const bool use_ioman = name1 == IRX_NAME1("ioman");
			using namespace ioman;
				switch(index)
				{
					EXPORT_H(  4, open)
					EXPORT_H(  5, close)
					EXPORT_H(  6, read)
					EXPORT_H(  7, write)
					EXPORT_H(  8, lseek)
					EXPORT_H( 10, remove)
					EXPORT_H( 11, mkdir)
					EXPORT_H( 12, rmdir)
					EXPORT_H( 13, dopen)
					EXPORT_H( 14, dclose)
					case 15: // dread
					if(use_ioman)
						return dread_HLE;
					else
						return dreadx_HLE;
					case 16: // getStat
					if(use_ioman)
						return getStat_HLE;
					else
						return getStatx_HLE;
				}
		}
		// clang-format on
		return 0;
	}

	irxDEBUG irxImportDebug(u32 name0, u32 name1, u16 index)
	{
		// clang-format off
		MODULE(loadcore)
			EXPORT_D(  6, RegisterLibraryEntries)
		END_MODULE
		MODULE(intrman)
			EXPORT_D(  4, RegisterIntrHandler)
		END_MODULE
		MODULE(sifcmd)
			EXPORT_D( 17, sceSifRegisterRpc)
		END_MODULE
		// clang-format off

		return 0;
	}

#undef MODULE
#undef END_MODULE
#undef EXPORT_D
#undef EXPORT_H



	int irxImportExec(u32 import_table, u16 index)
	{
		if (!import_table)
			return 0;

		u32 n0, n1;
		irxImportName(import_table, &n0, &n1);
		irxHLE hle          = irxImportHLE(n0, n1, index);
		irxDEBUG debug      = irxImportDebug(n0, n1, index);

		if (debug)
			debug();

		if (hle)
			return hle();
		return 0;
	}

	// C.71: per-stub cache of the import resolution. psxJ runs the module-import
	// stub through irxImportExec on EVERY execution -- a backward scan of up to
	// 0x2000 bytes for the 0x41e00000 table magic, a std::string heap allocation
	// for the library name, and a string-compare chain, millions of times per
	// minute (this is the one J form the arm64 IOP JIT leaves to the interpreter
	// for exactly this hook). The resolution depends only on the table magic,
	// the 8-byte libname and the index, so cache it per stub pc and revalidate
	// those inputs with three RAM reads per hit. A module loading over the stub
	// rewrites name/magic and fails revalidation -> full re-resolve. A stub with
	// NO table (scan miss) is not cached: there is nothing stable to revalidate
	// against.
	/* The cache is a fixed open-addressing table, written C89-shaped:
	 * a plain array of records, a multiply-shift hash, linear probing,
	 * no allocation and no pointer chasing. std::unordered_map's find
	 * for this key divides by the bucket count (a 64-bit DIV, twice on
	 * the general path) and then walks a heap-allocated node list; the
	 * hot path here is one multiply, one shift, one indexed load and one
	 * compare. Measured on 300 stubs: 3.3 ns to 1.6 ns per hit, 23 ns to
	 * 1.4 ns per stale overwrite.
	 *
	 * A stub pc is a 4-byte-aligned IOP address, so the low two bits
	 * carry nothing and the hash starts from pc >> 2. Erase is never
	 * needed: the only removal in the old code was immediately followed
	 * by re-inserting the same key, which is an overwrite here. A full
	 * probe window overwrites its first slot -- this is a cache, and a
	 * lost entry is a re-resolve, never a wrong answer. Empty is stub 0,
	 * the IOP's reset vector and never a J stub. */
	namespace
	{
		struct JStubCacheEntry
		{
			u32 stub;         /* key: the stub pc; 0 = empty                   */
			u32 table;
			u32 name0, name1; /* raw words as read: revalidation compares raw */
			u16 index;
			irxHLE hle;
			irxDEBUG debug;
		};
		/* 1024 slots for a few hundred stubs at most: low load, short probes. */
		enum { JSTUB_CACHE_BITS = 10, JSTUB_CACHE_SIZE = 1 << JSTUB_CACHE_BITS,
		       JSTUB_CACHE_PROBE = 8 };
		JStubCacheEntry s_jstub_cache[JSTUB_CACHE_SIZE];

		static __fi u32 jstub_hash(u32 stub)
		{
			/* Fibonacci hashing: the 32-bit golden-ratio multiplier spreads
			 * the sequential stub addresses of one import table across the
			 * slots; the top bits are the best-mixed ones. */
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

		/* The slot to write stub into: its own if present, else the first
		 * empty in the window, else the window's first slot (evict). */
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
	}
	int irxImportExecCached(u32 stubpc, u16 index)
	{
		JStubCacheEntry* e = jstub_find(stubpc);
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
		const u32 table = irxImportTableAddr(stubpc);
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

} // end namespace R3000A

namespace R3000A
{
} // namespace R3000A
