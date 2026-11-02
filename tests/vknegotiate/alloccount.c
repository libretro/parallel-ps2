/* Preloaded into tests/vknegotiate (LD_PRELOAD), with VN_ALLOC_WINDOW:
 * counts the heap allocations the core itself makes between
 * vn_alloc_begin and vn_alloc_end, which the frontend calls at the frames
 * the window names, then prints the count and where the most came from.
 *
 * "The core itself": the first frame on the stack inside the core is
 * reached only through the C and C++ runtimes (malloc, operator new). An
 * allocation the Vulkan driver makes while the core calls into it is the
 * driver's, and lavapipe allocates for every command it records, which a
 * real driver takes from its command pool; those are not counted. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <execinfo.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AC_SITES 64

static void* (*ac_real_malloc)(size_t);
static void* (*ac_real_calloc)(size_t, size_t);
static void* (*ac_real_realloc)(void*, size_t);
static void* (*ac_real_memalign)(size_t, size_t);
static int (*ac_real_posix_memalign)(void**, size_t, size_t);
static void* (*ac_real_aligned_alloc)(size_t, size_t);

/* dlsym itself may allocate before the real calloc is known. */
static char ac_boot[16384];
static size_t ac_boot_used;

static int ac_counting;
static const char* ac_core; /* VN_ALLOC_CORE: the core's path */
static __thread int ac_inside;
static unsigned long ac_count;
static struct
{
	void* site;
	void* caller;
	unsigned long n;
} ac_site[AC_SITES];

static int ac_runtime(const char* path)
{
	return strstr(path, "libc.so") || strstr(path, "libstdc++") || strstr(path, "libgcc")
		|| strstr(path, "alloccount") || strstr(path, "ld-linux");
}

static void ac_note(void)
{
	void* bt[16];
	int n, i;
	if (!__atomic_load_n(&ac_counting, __ATOMIC_ACQUIRE) || ac_inside)
		return;
	ac_inside = 1;
	n = backtrace(bt, 16);
	for (i = 1; i < n; i++)
	{
		Dl_info di;
		if (!dladdr(bt[i], &di) || !di.dli_fname)
			break;
		if (ac_runtime(di.dli_fname))
			continue;
		if (ac_core && strstr(di.dli_fname, ac_core))
		{
			unsigned k;
			__atomic_add_fetch(&ac_count, 1, __ATOMIC_RELAXED);
			/* The site and its caller: the first is often the core's
			 * own operator new or aligned-allocation wrapper. */
			void* caller = (i + 1 < n) ? bt[i + 1] : NULL;
			for (k = 0; k < AC_SITES; k++)
				if (!ac_site[k].site || (ac_site[k].site == bt[i] && ac_site[k].caller == caller))
				{
					ac_site[k].site   = bt[i];
					ac_site[k].caller = caller;
					ac_site[k].n++;
					break;
				}
		}
		break;
	}
	ac_inside = 0;
}

void vn_alloc_begin(void)
{
	ac_core = getenv("VN_ALLOC_CORE");
	__atomic_store_n(&ac_count, 0, __ATOMIC_RELAXED);
	memset(ac_site, 0, sizeof(ac_site));
	__atomic_store_n(&ac_counting, 1, __ATOMIC_RELEASE);
}

void vn_alloc_end(void)
{
	unsigned k;
	__atomic_store_n(&ac_counting, 0, __ATOMIC_RELEASE);
	printf("alloccount: %lu core allocations\n", ac_count);
	for (k = 0; k < AC_SITES && ac_site[k].site; k++)
	{
		Dl_info di;
		/* The core's symbols are hidden: an offset for addr2line. */
		if (dladdr(ac_site[k].site, &di) && di.dli_fbase)
			printf("alloccount:   %lu from core+0x%lx, called from core+0x%lx\n", ac_site[k].n,
				(unsigned long)((uintptr_t)ac_site[k].site - (uintptr_t)di.dli_fbase),
				(unsigned long)((uintptr_t)ac_site[k].caller - (uintptr_t)di.dli_fbase));
		else
			printf("alloccount:   %lu from %p\n", ac_site[k].n, ac_site[k].site);
	}
	fflush(stdout);
}

#define AC_RESOLVE(var, name) \
	do { if (!var) *(void**)&var = dlsym(RTLD_NEXT, name); } while (0)

void* malloc(size_t n)
{
	void* p;
	AC_RESOLVE(ac_real_malloc, "malloc");
	p = ac_real_malloc(n);
	ac_note();
	return p;
}

void* calloc(size_t a, size_t b)
{
	static int resolving;
	void* p;
	if (!ac_real_calloc)
	{
		if (resolving)
		{
			p = ac_boot + ac_boot_used;
			ac_boot_used += (a * b + 15) & ~(size_t)15;
			return p; /* static storage: already zero */
		}
		resolving = 1;
		AC_RESOLVE(ac_real_calloc, "calloc");
		resolving = 0;
	}
	p = ac_real_calloc(a, b);
	ac_note();
	return p;
}

void* realloc(void* o, size_t n)
{
	void* p;
	AC_RESOLVE(ac_real_realloc, "realloc");
	p = ac_real_realloc(o, n);
	ac_note();
	return p;
}

void* memalign(size_t a, size_t n)
{
	void* p;
	AC_RESOLVE(ac_real_memalign, "memalign");
	p = ac_real_memalign(a, n);
	ac_note();
	return p;
}

int posix_memalign(void** out, size_t a, size_t n)
{
	int r;
	AC_RESOLVE(ac_real_posix_memalign, "posix_memalign");
	r = ac_real_posix_memalign(out, a, n);
	ac_note();
	return r;
}

void* aligned_alloc(size_t a, size_t n)
{
	void* p;
	AC_RESOLVE(ac_real_aligned_alloc, "aligned_alloc");
	p = ac_real_aligned_alloc(a, n);
	ac_note();
	return p;
}

void free(void* p)
{
	static void (*real_free)(void*);
	if ((char*)p >= ac_boot && (char*)p < ac_boot + sizeof(ac_boot))
		return;
	AC_RESOLVE(real_free, "free");
	real_free(p);
}
