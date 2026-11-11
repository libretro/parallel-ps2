#!/bin/sh
# emu_hot (pcsx2/HotConfig.h) mirrors the EE settings the hot paths test
# -- vtlb's memory access paths, the interpreter's branches, the event
# test -- so they read one word instead of calling into EmuConfig. A
# mirror is only right while every write to what it mirrors is followed
# by emu_config_sync_hot. This keeps that true at the source level:
#
#   1. CPU, speedhack and gamefix settings, or EmuConfig as a whole, are
#      written only inside the functions that resync afterwards
#      (VMManager::LoadSettings and the ApplyGameFixes it calls, and the
#      two Pcsx2Config.c functions in HotConfig.h);
#   2. LoadSettings calls emu_config_sync_hot after ApplyGameFixes.
#
# A write anywhere else leaves the hot paths on the old value: a game
# fix that never takes effect, or vtlb taking the recompiler's path with
# the interpreter running. The negative case checks a stray write is
# caught.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)

ALLOWED='VMManager::LoadSettings|VMManager::ApplyGameFixes|emu_config_sync_hot|emu_config_disable_fastmem'

# file:line:function for every write to a mirrored setting.
writes() {
	awk '
	/^[A-Za-z_].*[A-Za-z_0-9:]+[ \t]*\([^;]*$/ { cand = $0 }
	/^\{/ { fn = cand; sub(/\(.*/, "", fn); n = split(fn, w, /[ \t*&]+/); fn = w[n] }
	/^\}/ { fn = "" }
	/^[ \t]*(\/\/|\/\*|\*)/ { next }
	/EmuConfig(\.(Cpu|Speedhacks|Gamefixes)[A-Za-z_.]*)?[ \t]*(=[^=]|\|=|&=)/ ||
	/GameDatabase_ApplyGameFixes\([^,]*,[ \t]*EmuConfig/ ||
	/EmuConfig\.Gamefixes\.(Set|DisableAll)\(/ {
		print FILENAME ":" FNR ":" (fn == "" ? "<file scope>" : fn)
	}' "$@"
}

echo "== writes to the mirrored settings, outside a resync =="
FILES=$(find "$ROOT/pcsx2" "$ROOT/libretro" -name '*.cpp' -not -path '*/libretro-common/*' -not -path '*/parallel-gs/*' | sort)
BAD=$(writes $FILES | grep -vE ":($ALLOWED)\$" || true)
if [ -n "$BAD" ]; then
	echo "$BAD" | sed "s|^$ROOT/||; s/^/  FAIL: /"
	echo "  each needs emu_config_sync_hot() after it, or to move into LoadSettings"
	exit 1
fi
echo "  none"

echo "== LoadSettings resyncs after the game fixes =="
awk '/^void VMManager::LoadSettings\(\)/ { inb = 1 } inb && /ApplyGameFixes\(\)/ { af = NR }
	inb && /emu_config_sync_hot\(\)/ { sy = NR } inb && /^\}/ { exit }
	END { exit !(af && sy > af) }' "$ROOT/pcsx2/VMManager.cpp" ||
	{ echo "  FAIL: VMManager::LoadSettings must call emu_config_sync_hot() after ApplyGameFixes()"; exit 1; }
echo "  ok"

echo "== negative: a stray write is caught =="
printf 'void SomethingElse()\n{\n\tEmuConfig.Speedhacks.WaitLoop = true;\n}\n' > "$DIR/.stray.cpp"
if writes "$DIR/.stray.cpp" | grep -vqE ":($ALLOWED)\$"; then
	echo "  caught"
else
	rm -f "$DIR/.stray.cpp"
	echo "  FAIL: the audit missed a write in SomethingElse"
	exit 1
fi
rm -f "$DIR/.stray.cpp"
echo "hotconfig: ok"
