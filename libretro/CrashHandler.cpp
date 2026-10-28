#include "CrashHandler.h"

#if defined(__unix__) || defined(__ANDROID__) || defined(__APPLE__)

#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <ucontext.h>

#if defined(__linux__) || defined(__ANDROID__)
#include <sys/prctl.h>
#endif
#if defined(__ANDROID__)
#include <android/log.h>
#endif

namespace
{
	/*
	 * The signals worth reporting. SIGSEGV and SIGBUS are the interesting ones,
	 * but they arrive here only after fastmem has declined them - see the
	 * header. The rest are taken directly.
	 */
	const int kSignals[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP };
	constexpr size_t kSignalCount = sizeof(kSignals) / sizeof(kSignals[0]);

	struct sigaction s_old_actions[kSignalCount];
	bool s_installed = false;

	void Emit(const char* line)
	{
		/* Two destinations on purpose: logcat is where an Android crash is
		 * actually readable, and stderr is what reaches a desktop terminal and
		 * anything capturing the core's output. */
#if defined(__ANDROID__)
		__android_log_write(ANDROID_LOG_ERROR, "lrps2", line);
#endif
		const size_t len = strlen(line);
		ssize_t written = write(STDERR_FILENO, line, len);
		written = write(STDERR_FILENO, "\n", 1);
		(void)written;
	}

	/*
	 * The one question a crash in a recompiled core turns on: was the program
	 * counter in generated code or in the core's own text? A backtrace cannot
	 * say - JIT pages carry no unwind information, so the unwinder stops at the
	 * signal frame - but /proc/self/maps can, because generated code lives in an
	 * anonymous mapping while compiled code lives in the .so.
	 */
	void DescribeAddress(const char* what, unsigned long long addr)
	{
		char line[640];
		if (addr == 0)
		{
			snprintf(line, sizeof(line), "  %s in null", what);
			Emit(line);
			return;
		}

		FILE* maps = fopen("/proc/self/maps", "r");
		if (!maps)
		{
			snprintf(line, sizeof(line), "  %s in unknown (no /proc/self/maps)", what);
			Emit(line);
			return;
		}

		char entry[512];
		bool found = false;
		while (fgets(entry, sizeof(entry), maps))
		{
			unsigned long long start = 0, end = 0;
			if (sscanf(entry, "%llx-%llx", &start, &end) != 2)
				continue;
			if (addr < start || addr >= end)
				continue;

			size_t n = strlen(entry);
			while (n > 0 && (entry[n - 1] == '\n' || entry[n - 1] == ' '))
				entry[--n] = '\0';
			snprintf(line, sizeof(line), "  %s in %s (+0x%llx)", what, entry, addr - start);
			found = true;
			break;
		}
		fclose(maps);

		if (!found)
			snprintf(line, sizeof(line), "  %s in unmapped", what);
		Emit(line);
	}

	void CurrentThreadName(char* out, size_t size)
	{
		out[0] = '\0';
#if defined(__linux__) || defined(__ANDROID__)
		char name[32] = {};
		if (prctl(PR_GET_NAME, name, 0, 0, 0) == 0)
			snprintf(out, size, "%s", name);
#endif
		if (out[0] == '\0')
			snprintf(out, size, "unnamed");
	}

	const char* SignalName(int sig)
	{
		switch (sig)
		{
			case SIGSEGV: return "SIGSEGV";
			case SIGBUS:  return "SIGBUS";
			case SIGILL:  return "SIGILL";
			case SIGFPE:  return "SIGFPE";
			case SIGABRT: return "SIGABRT";
			case SIGTRAP: return "SIGTRAP";
			default:      return "signal";
		}
	}

	void ChainToPrevious(int sig, siginfo_t* info, void* ctx)
	{
		for (size_t i = 0; i < kSignalCount; i++)
		{
			if (kSignals[i] != sig)
				continue;

			const struct sigaction& sa = s_old_actions[i];
			if (sa.sa_flags & SA_SIGINFO)
				sa.sa_sigaction(sig, info, ctx);
			else if (sa.sa_handler == SIG_DFL)
			{
				/* Restoring the default and re-raising would come back here,
				 * since the handler is reinstalled; abort is what the default
				 * action would do anyway. */
				signal(sig, SIG_DFL);
				raise(sig);
				abort();
			}
			else if (sa.sa_handler != SIG_IGN)
				sa.sa_handler(sig);
			return;
		}
		abort();
	}

	void Handler(int sig, siginfo_t* info, void* ctx)
	{
		/* A second fault while reporting the first would loop; let the second
		 * one take the default action instead. */
		static volatile sig_atomic_t s_reporting = 0;
		if (s_reporting)
		{
			signal(sig, SIG_DFL);
			raise(sig);
			return;
		}
		s_reporting = 1;

		char line[640];
		char thread[64];
		CurrentThreadName(thread, sizeof(thread));

		const unsigned long long fault = (unsigned long long)(info ? (uintptr_t)info->si_addr : 0);

		snprintf(line, sizeof(line), "[CrashHandler] %s (code %d) on thread \"%s\"",
			SignalName(sig), info ? info->si_code : 0, thread);
		Emit(line);

#if defined(__aarch64__) && (defined(__linux__) || defined(__ANDROID__))
		ucontext_t* uc = (ucontext_t*)ctx;
		const unsigned long long pc = uc ? (unsigned long long)uc->uc_mcontext.pc : 0;
		const unsigned long long lr = uc ? (unsigned long long)uc->uc_mcontext.regs[30] : 0;
		const unsigned long long sp = uc ? (unsigned long long)uc->uc_mcontext.sp : 0;
		const unsigned long long fp = uc ? (unsigned long long)uc->uc_mcontext.regs[29] : 0;

		snprintf(line, sizeof(line), "  fault=0x%llx pc=0x%llx lr=0x%llx sp=0x%llx fp=0x%llx",
			fault, pc, lr, sp, fp);
		Emit(line);

		DescribeAddress("pc   ", pc);
		DescribeAddress("lr   ", lr);
		if (fault != pc)
			DescribeAddress("fault", fault);
#else
		snprintf(line, sizeof(line), "  fault=0x%llx", fault);
		Emit(line);
		DescribeAddress("fault", fault);
		(void)ctx;
#endif

		Emit("  (pc inside the core's .so is a bug in compiled code; pc inside an "
		     "anonymous mapping means it was executing recompiled code)");

		s_reporting = 0;
		ChainToPrevious(sig, info, ctx);
	}
} // namespace

void CrashHandler_Install(void)
{
	if (s_installed)
		return;
	s_installed = true;

	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_SIGINFO | SA_NODEFER;
	sa.sa_sigaction = Handler;

	for (size_t i = 0; i < kSignalCount; i++)
		sigaction(kSignals[i], &sa, &s_old_actions[i]);
}

#else

void CrashHandler_Install(void) {}

#endif
