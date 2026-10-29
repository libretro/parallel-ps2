#!/bin/sh
# Read the paraLLEl-GS shader bank, and identify a module from its source.
#
# shaders/slangmosh.hpp is a precompiled SPIR-V bank -- thirty modules in
# one uint32 array -- and it ships name-stripped, so it says nothing about
# what the GPU does with a given input. This splits it into one .spv per
# module and disassembles them when spirv-dis is installed.
#
# The GLSL beside it is upstream paraLLEl-GS
# (https://github.com/Arntzen-Software/parallel-gs) at bb437b1, the
# revision whose bank this one started as, plus the fork's own changes.
# shaderbank_build.sh rebuilds each module from it with the toolchain its
# blob was built with and checks that every module comes out byte for
# byte; use that to change a shader.
#
# The table at the end of the bank names each module by word offset, and
# the split below numbers modules in bank order, not by name. To identify
# a module from a source built some other way, recompile it and match it
# against every module:
#
#   ./shaderbank.sh /tmp/bank ../../pcsx2/GS/parallel-gs/gs/shaders
#
# The match is on instruction mix, with debug info stripped from both
# sides, and it reports how far off the nearest module is. Treat a few
# percent as the same shader built by another compiler and more as a
# different shader; the id bound alone is not enough to tell them apart,
# since two modules here collide on it.
#
# What the shaders do with a degenerate Q was read out of the bank like
# this; the note at the head of pgs_vertex_kernels.h says what was found.
# Needs: apt install spirv-tools glslang-tools
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
BANK=$ROOT/pcsx2/GS/parallel-gs/gs/shaders/slangmosh.hpp
OUT=${1:-${TMPDIR:-/tmp}/pgs-shaders}
SRC=$2

[ -f "$BANK" ] || { echo "no shader bank at $BANK"; exit 1; }
mkdir -p "$OUT"

python3 - "$BANK" "$OUT" <<'PY'
import re, sys, os

bank, out = sys.argv[1], sys.argv[2]
src = open(bank).read()
m = re.search(r'spirv_bank\[\] =\s*\{(.*?)\n\};', src, re.S)
if not m:
    sys.exit("could not find spirv_bank[] in %s" % bank)
W = [int(x, 16) for x in re.findall(r'0x([0-9a-fA-F]{8})u', m.group(1))]

# Every module starts with the magic, but the magic can also appear inside
# another module's constant data, so each candidate has to look like a
# header: version 1.x, schema 0, a plausible id bound.
starts = []
for i, w in enumerate(W):
    if w != 0x07230203 or i + 5 > len(W):
        continue
    ver, bound, schema = W[i + 1], W[i + 3], W[i + 4]
    if (ver >> 16) & 0xff == 1 and schema == 0 and 0 < bound < 1000000:
        starts.append(i)
starts.append(len(W))

def entry_name(w):
    """OpEntryPoint's name, when the module kept it."""
    i = 5
    while i < len(w):
        op, ln = w[i] & 0xffff, w[i] >> 16
        if ln == 0:
            return ""
        if op == 15 and ln > 3:
            b = b''.join(w[j].to_bytes(4, 'little') for j in range(i + 3, i + ln))
            return b.split(b'\0')[0].decode('utf8', 'replace')
        i += ln
    return ""

print("%d modules" % (len(starts) - 1))
print("  %-3s %-9s %-9s %s" % ("idx", "bound", "bytes", "entry"))
for k in range(len(starts) - 1):
    mod = W[starts[k]:starts[k + 1]]
    path = os.path.join(out, "%02d.spv" % k)
    open(path, 'wb').write(b''.join(w.to_bytes(4, 'little') for w in mod))
    print("  %-3d %-9d %-9d %s" % (k, mod[3], len(mod) * 4, entry_name(mod) or "(stripped)"))
PY

if command -v spirv-dis >/dev/null 2>&1; then
	for f in "$OUT"/*.spv; do
		spirv-dis --no-header "$f" > "${f%.spv}.dis" 2>/dev/null || true
	done
	echo
	echo "disassembled to $OUT/*.dis"
else
	echo
	echo "spirv-dis not installed (apt install spirv-tools); .spv only"
fi

if [ -n "$SRC" ] && [ -d "$SRC" ]; then
	echo
	echo "=== recompiling $SRC and matching each against the bank ==="
	command -v glslangValidator >/dev/null 2>&1 ||
		{ echo "glslangValidator not installed (apt install glslang-tools)"; exit 1; }
	for s in "$SRC"/*.comp "$SRC"/*.frag "$SRC"/*.vert; do
		[ -f "$s" ] || continue
		b=$(basename "$s"); stage=comp
		case $s in *.frag) stage=frag ;; *.vert) stage=vert ;; esac
		if ! glslangValidator --target-env vulkan1.3 -S $stage -I"$SRC" \
		     -P'#extension GL_GOOGLE_include_directive : require' \
		     -DFEEDBACK_COLOR=0 -DFEEDBACK_DEPTH=0 \
		     -o "$OUT/re.spv" "$s" >"$OUT/re.log" 2>&1; then
			printf '  %-26s does not compile against these headers\n' "$b"
			continue
		fi
		# Debug info off both sides: glslang keeps names that the bank
		# was stripped of, and they would swamp the comparison.
		command -v spirv-opt >/dev/null 2>&1 &&
			spirv-opt --strip-debug -O "$OUT/re.spv" -o "$OUT/re.spv" >/dev/null 2>&1
		python3 - "$OUT" "$b" <<'PY'
import sys, os, glob, re, collections, subprocess

out, name = sys.argv[1], sys.argv[2]

def mix(path):
    """Instruction histogram, debug and decoration aside."""
    try:
        dis = subprocess.run(["spirv-dis", "--no-header", path],
                             capture_output=True, text=True).stdout
    except FileNotFoundError:
        return None
    c = collections.Counter()
    for line in dis.splitlines():
        m = re.search(r"= (Op\w+)|^\s+(Op\w+)", line)
        if not m:
            continue
        op = m.group(1) or m.group(2)
        if op.startswith(("OpName", "OpMemberName", "OpSource", "OpString")):
            continue
        c[op] += 1
    return c

want = mix(os.path.join(out, "re.spv"))
if want is None:
    print("  %-26s spirv-dis not installed; cannot match" % name)
    sys.exit(0)
total = sum(want.values())

best = None
for f in sorted(glob.glob(os.path.join(out, "[0-9][0-9].spv"))):
    got = mix(f)
    dist = sum(abs(want[o] - got[o]) for o in set(want) | set(got))
    if best is None or dist < best[0]:
        best = (dist, os.path.basename(f)[:2], sum(got.values()))

dist, idx, got_total = best
pct = 100.0 * dist / total if total else 0.0
verdict = ("same shader, toolchain residue" if pct < 5.0
           else "NOT this shader" if pct > 25.0
           else "close -- check by hand")
print("  %-26s -> module %s  %5.1f%% apart (%d vs %d instructions)  %s"
      % (name, idx, pct, total, got_total, verdict))
PY
	done
	rm -f "$OUT/re.spv" "$OUT/re.log"
fi

echo
echo "PASS: shader bank split"
