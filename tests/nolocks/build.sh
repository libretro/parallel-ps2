#!/bin/sh
# No locks in the core: threads hand work over through atomics, SPSC
# rings and eventcounts, never a mutex, a condition variable or a
# critical section. This fails on any of them in the core's own sources.
# libretro-common is synced from RetroArch and its primitives are
# allowed to use what they like underneath. The negative case checks a
# stray lock is caught.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)

PATTERN='\bPGS::(mutex|lock_guard|unique_lock|condition_variable)\b|\bRWSpinLock|\bslock_(new|lock|unlock|try_lock|free)\b|\bscond_(new|wait|signal|broadcast|free)\b|\bslock_t\b|\bscond_t\b|std::(recursive_|shared_|timed_)?mutex\b|std::(lock_guard|unique_lock|scoped_lock|condition_variable)\b|\bpthread_(mutex|cond|rwlock)_|\b(Enter|Leave|Initialize)CriticalSection\b|\bCRITICAL_SECTION\b|\bSRWLOCK\b|\bAcquireSRWLock|\bSLockGuard\b'

scan() {
	grep -rnE "$PATTERN" "$@" --include='*.c' --include='*.cpp' --include='*.h' --include='*.hpp' --include='*.inl' \
		| grep -v '/libretro-common/' \
		| grep -v "^$DIR/" || true
}

echo "== locks in the core =="
HITS=$(scan "$ROOT/pcsx2" "$ROOT/libretro" "$ROOT/common" "$ROOT/tests")
if [ -n "$HITS" ]; then
	echo "$HITS" | sed "s|^$ROOT/||; s/^/  FAIL: /"
	echo "  hand the work over through atomics, an SPSC ring or an eventcount instead"
	exit 1
fi
echo "  none"

echo "== negative: a stray lock is caught =="
mkdir -p "$DIR/.stray"
printf 'static slock_t* s_lock;\nvoid f(void) { slock_lock(s_lock); }\n' > "$DIR/.stray/stray.c"
if grep -rnE "$PATTERN" "$DIR/.stray" > /dev/null; then
	echo "  caught"
else
	rm -rf "$DIR/.stray"
	echo "  FAIL: the scan missed a lock"
	exit 1
fi
rm -rf "$DIR/.stray"
echo "nolocks: ok"
