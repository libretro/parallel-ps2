#!/bin/sh
# Rebuild paraLLEl-GS shader bank modules from their GLSL, or check that
# every module in the bank is what its source builds to. Run by hand.
#
#   shaderbank_build.sh check [NAME ...]   rebuild and compare, all by default
#   shaderbank_build.sh build NAME ...     rebuild and splice into the bank
#
# NAME is the bank's table field: clut_write, upload[1], ubershader[0][1].
# The bank is pcsx2/GS/parallel-gs/gs/shaders/slangmosh.hpp; the sources
# sit beside it. Each module records the toolchain its shipped blob was
# built with, and is only ever rebuilt with that one, so a rebuild of an
# unchanged source gives the shipped bytes back:
#
#   pinned   upstream's slangmosh path: shaderc -O, Vulkan 1.1, SPIR-V 1.3,
#            no debug info, STAGE_<stage>=1 defined. glslc from shaderc
#            42c364eb27982ecfc9e00e384df205730e65b90c built with glslang
#            8f79344bdb5c151b450462e35283a731c4a0f158, SPIRV-Tools
#            556c7ca95c6a309278c6cee98129d573827a05b4 and SPIRV-Headers
#            00898b201b4153d7198c3e0134dbf953c83bbfd7 (Granite 9e32ac62's
#            pins): clone each at that commit, the last three into
#            shaderc's third_party/{glslang,spirv-tools,spirv-headers},
#            then
#              cmake -S shaderc -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
#                -DSHADERC_SKIP_TESTS=ON -DSHADERC_SKIP_EXAMPLES=ON \
#                -DSHADERC_SKIP_COPYRIGHT_CHECK=ON -DSPIRV_SKIP_TESTS=ON
#              ninja -C build glslc_exe
#            and point PINNED_GLSLC at build/glslc/glslc.
#   glslc    glslc -O, Vulkan 1.1: shaderc 2023.8 as Ubuntu 24.04 ships it
#            (package glslc 2023.8-1build1). GLSLC overrides the path.
#   glslang  glslangValidator -V, Vulkan 1.1, then spirv-opt -O: glslang
#            15.1.0 and SPIRV-Tools 2025.1 as Ubuntu 24.04 ships them
#            (glslang-tools 15.1.0-2~ubuntu0.24.04.2, spirv-tools
#            2025.1~rc1-1~ubuntu0.24.04.2). "glslang-strip" adds
#            spirv-opt --strip-debug.
#
# The Granite modules (sharpen_*, upscale_*, blit) come from Granite's own
# assets at 9e32ac62; set GRANITE to such a checkout to include them.
# A toolchain that is missing or a different version skips its modules;
# check fails on any module that builds to other bytes.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
SHADERS="$ROOT/pcsx2/GS/parallel-gs/gs/shaders"
BANK="$SHADERS/slangmosh.hpp"
GLSLC=${GLSLC:-glslc}
MODE=$1
[ $# -gt 0 ] && shift
case $MODE in
	check) ;;
	build) [ $# -gt 0 ] || { echo "build needs module names"; exit 1; } ;;
	*) echo "usage: $0 check [NAME ...] | build NAME ..."; exit 1 ;;
esac

# name|source|stage|defines|toolchain
modules() {
	cat <<'EOF'
binning|binning.comp|compute||pinned
upload[0]|upload.comp|compute|SUPER_SAMPLED=0|pinned
upload[1]|upload.comp|compute|SUPER_SAMPLED=1|pinned
clut_write|clut_write.comp|compute||pinned
vram_copy|vram_copy.comp|compute||pinned
triangle_setup|triangle_setup.comp|compute||glslc
single_sample_heuristic|single_sample_heuristic.comp|compute||pinned
ubershader[0][0]|ubershader.comp|compute|FEEDBACK_COLOR=0 FEEDBACK_DEPTH=0|glslc
ubershader[1][0]|ubershader.comp|compute|FEEDBACK_COLOR=1 FEEDBACK_DEPTH=0|glslc
ubershader[0][1]|ubershader.comp|compute|FEEDBACK_COLOR=0 FEEDBACK_DEPTH=1|glslc
ubershader[1][1]|ubershader.comp|compute|FEEDBACK_COLOR=1 FEEDBACK_DEPTH=1|glslc
quad|quad.vert|vertex||pinned
sample_circuit[0]|sample_circuit.frag|fragment|PROMOTED=0|glslang
sample_circuit[1]|sample_circuit.frag|fragment|PROMOTED=1|glslang
blit_circuit|blit_circuit.frag|fragment||pinned
weave|weave.frag|fragment||glslang-strip
sharpen_vert|@post/ffx-fsr/sharpen.vert|vertex||pinned
sharpen_frag|@post/ffx-fsr/sharpen.frag|fragment||pinned
upscale_vert|@post/ffx-fsr/upscale.vert|vertex||pinned
upscale_frag|@post/ffx-fsr/upscale.frag|fragment||pinned
blit|@blit.frag|fragment||pinned
extwrite|extwrite.comp|compute||pinned
qword_clear|qword_clear.comp|compute||pinned
sampler_feedback|sampler_feedback.comp|compute||pinned
ui_vert|ui.vert|vertex||pinned
ui_frag[0][0]|ui.frag|fragment|TEX=0 LINEAR=0|pinned
ui_frag[1][0]|ui.frag|fragment|TEX=1 LINEAR=0|pinned
ui_frag[0][1]|ui.frag|fragment|TEX=0 LINEAR=1|pinned
ui_frag[1][1]|ui.frag|fragment|TEX=1 LINEAR=1|pinned
EOF
}

# Prints why a toolchain cannot be used, nothing when it can.
toolchain_missing() {
	case $1 in
	pinned)
		[ -n "$PINNED_GLSLC" ] && [ -x "$PINNED_GLSLC" ] || { echo "PINNED_GLSLC not set"; return; }
		v=$("$PINNED_GLSLC" --version 2>/dev/null)
		case $v in *42c364eb27982ecfc9e00e384df205730e65b90c*) ;; *) echo "PINNED_GLSLC is not shaderc 42c364e"; return ;; esac
		case $v in *8f79344bdb5c151b450462e35283a731c4a0f158*) ;; *) echo "PINNED_GLSLC is not built with glslang 8f79344"; return ;; esac
		case $v in *556c7ca95c6a309278c6cee98129d573827a05b4*) ;; *) echo "PINNED_GLSLC is not built with SPIRV-Tools 556c7ca"; return ;; esac ;;
	glslc)
		case $("$GLSLC" --version 2>/dev/null) in "shaderc 2023.8"*) ;; *) echo "$GLSLC is not shaderc 2023.8" ;; esac ;;
	glslang|glslang-strip)
		case $(glslangValidator --version 2>/dev/null) in *"Glslang Version: 11:15.1.0"*) ;; *) echo "glslangValidator is not 15.1.0"; return ;; esac
		case $(spirv-opt --version 2>&1) in "SPIRV-Tools v2025.1 "*) ;; *) echo "spirv-opt is not SPIRV-Tools 2025.1" ;; esac ;;
	esac
}

# compile SOURCE STAGE "DEFINES" TOOLCHAIN OUT
compile() {
	src=$1; stage=$2; defs=""; inc="-I$SHADERS"
	for d in $3; do defs="$defs -D$d"; done
	case $src in
	@*)
		[ -n "$GRANITE" ] || { echo "GRANITE not set" >&2; return 1; }
		src="$GRANITE/assets/shaders/${src#@}"; inc="-I$GRANITE/assets/shaders/inc" ;;
	*) src="$SHADERS/$src" ;;
	esac
	case $4 in
	pinned)
		case $stage in compute) sd=STAGE_COMPUTE ;; vertex) sd=STAGE_VERTEX ;; *) sd=STAGE_FRAGMENT ;; esac
		"$PINNED_GLSLC" -O --target-env=vulkan1.1 --target-spv=spv1.3 -fshader-stage=$stage \
			-D$sd=1 $defs $inc "$src" -o "$5" ;;
	glslc)
		"$GLSLC" -O --target-env=vulkan1.1 -fshader-stage=$stage $defs $inc "$src" -o "$5" ;;
	glslang|glslang-strip)
		# glslangValidator wants the include extension spelled out.
		sed '/^#version/a #extension GL_GOOGLE_include_directive : require' "$src" > "$TMP/src.$stage"
		case $stage in compute) st=comp ;; vertex) st=vert ;; *) st=frag ;; esac
		glslangValidator -V --target-env vulkan1.1 -S $st $defs $inc "$TMP/src.$stage" -o "$TMP/raw.spv" > "$TMP/log" ||
			{ cat "$TMP/log" >&2; return 1; }
		spirv-opt -O "$TMP/raw.spv" -o "$5"
		if [ "$4" = glslang-strip ]; then
			spirv-opt --strip-debug "$5" -o "$TMP/stripped.spv"
			mv "$TMP/stripped.spv" "$5"
		fi ;;
	esac
}

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
${CC:-cc} -std=c89 -pedantic -Wall -O2 -o "$TMP/splice" "$DIR/pgs_bank_splice.c"

wanted() {
	[ $# -eq 0 ] && return 0
	for w in "$@"; do [ "$w" = "$NAME" ] && return 0; done
	return 1
}

if [ "$MODE" = build ]; then
	args=""
	for want in "$@"; do
		line=$(modules | grep -F "$want|" | head -n 1)
		[ -n "$line" ] && [ "${line%%|*}" = "$want" ] || { echo "no module $want"; exit 1; }
		IFS='|' read -r NAME SRC STAGE DEFS TC <<EOF
$line
EOF
		why=$(toolchain_missing "$TC")
		[ -z "$why" ] || { echo "$NAME: $why"; exit 1; }
		n=$(echo "$NAME" | tr -c 'A-Za-z0-9\n' '_')
		compile "$SRC" "$STAGE" "$DEFS" "$TC" "$TMP/$n.spv"
		args="$args $NAME=$TMP/$n.spv"
		echo "built $NAME ($TC)"
	done
	"$TMP/splice" "$BANK" "$TMP/bank.hpp" $args > /dev/null
	mv "$TMP/bank.hpp" "$BANK"
	echo "spliced into $BANK"
	exit 0
fi

fail=0
modules > "$TMP/list"
while IFS='|' read -r NAME SRC STAGE DEFS TC; do
	wanted "$@" || continue
	why=$(toolchain_missing "$TC")
	case $SRC in @*) [ -n "$GRANITE" ] || why="GRANITE not set" ;; esac
	if [ -n "$why" ]; then
		printf '  skip    %-24s %s\n' "$NAME" "$why"
		continue
	fi
	"$TMP/splice" -x "$BANK" "$NAME=$TMP/shipped.spv" > /dev/null
	if ! compile "$SRC" "$STAGE" "$DEFS" "$TC" "$TMP/built.spv" 2> "$TMP/err"; then
		printf '  FAIL    %-24s does not compile: %s\n' "$NAME" "$(head -n 1 "$TMP/err")"
		fail=1
	elif cmp -s "$TMP/shipped.spv" "$TMP/built.spv"; then
		printf '  same    %-24s %s\n' "$NAME" "$TC"
	else
		printf '  DIFFER  %-24s %s\n' "$NAME" "$TC"
		fail=1
	fi
done < "$TMP/list"
[ $fail -eq 0 ] && echo "PASS: every checked module is what its source builds to" ||
	{ echo "FAIL: the bank does not match its sources"; exit 1; }
