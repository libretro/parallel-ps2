#!/bin/sh
# The core against standard libraries other than the one it is developed
# on. A type only libstdc++ has (__gnu_cxx::__normal_iterator, spelled out
# where an iterator was meant) built on Linux and broke the Android,
# macOS and Windows builds, which use libc++ and MSVC's library.
#
# Two checks: no source names a standard library's internals (reserved
# names: __gnu_cxx, std::__x, std::_X, the libraries' own macros), which
# catches MSVC's as well as libstdc++'s; and, where clang and libc++ are
# installed, every C++ unit of the CMake build compiles against libc++.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)

echo "== standard library internals named in the core =="
HITS=$(cd "$ROOT" && git grep -nE '__gnu_cxx|std::__[a-z]|std::_[A-Z]|__cxx11|_LIBCPP_|_GLIBCXX' -- \
	pcsx2 common libretro \
	':!libretro/libretro-common' ':!pcsx2/GS/parallel-gs/Granite' || true)
if [ -n "$HITS" ]; then
	echo "$HITS"
	echo "stdlib: FAIL: name the type the standard gives (std::vector<T>::iterator), not a library's own"
	exit 1
fi
echo "  none"

echo "== every C++ unit against libc++ =="
printf '#include <vector>\nint main(void) { std::vector<int> v; return (int)v.size(); }\n' > "$DIR/.probe.cpp"
if ! command -v cmake >/dev/null 2>&1 || ! command -v clang++ >/dev/null 2>&1 \
	|| ! clang++ -stdlib=libc++ -fsyntax-only "$DIR/.probe.cpp" >/dev/null 2>&1; then
	rm -f "$DIR/.probe.cpp"
	echo "  skipped: needs cmake, clang and libc++ (libc++-dev)"
	exit 0
fi
rm -f "$DIR/.probe.cpp"
B="$DIR/.build"
rm -rf "$B" && mkdir -p "$B"
(cd "$B" && CC=clang CXX=clang++ CXXFLAGS="-stdlib=libc++" LDFLAGS="-stdlib=libc++" \
	cmake -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCMAKE_BUILD_TYPE=Release "$ROOT" >"$B/configure.log" 2>&1) \
	|| { cat "$B/configure.log"; exit 1; }
STATUS=0
python3 "$DIR/sweep.py" "$B/compile_commands.json" || STATUS=$?
rm -rf "$B"
[ $STATUS -eq 0 ] && echo "stdlib: ok" || echo "stdlib: FAIL"
exit $STATUS
