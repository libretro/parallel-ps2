#!/usr/bin/env python3
# The Vulkan renderer's SPIR-V, compiled ahead of time.
#
# The GLSL under pcsx2/GS/Renderers/Vulkan/shaders/ is never seen by the
# build: this script compiles it into pcsx2/GS/Renderers/Vulkan/VKShaders.inc,
# which is committed, and the renderer makes its shader modules straight
# from those words. No shader compiler is linked into the core.
#
#   python3 tools/vk_spirv.py update [--glslang PATH]
#       recompile everything; needs glslangValidator (the Vulkan SDK, or
#       the glslang-tools package), from PATH or --glslang
#   python3 tools/vk_spirv.py check
#       fail unless VKShaders.inc is what update wrote for the sources as
#       they are now: their hash and its own words' hash both match. Needs
#       no compiler; CI runs it (tests/vkspirv).
#
# What is compiled:
#  - tfx.glsl: the draw shaders. Every selector field is a specialization
#    constant, so a module serves every selector with its interface: the
#    vertex shader with and without VS_EXPAND, the fragment shader for
#    each way of reading the target (none, RT_SUBPASS, RT_FETCH). The
#    constants' IDs are written out as an enum (VKSC_*), so the C++ cannot
#    disagree with the shader about them. What the renderer drops from a
#    module's words (VKSpirvPatch.c) is checked to be droppable: the
#    colour varying's one Flat member decoration; in a fragment module
#    the second colour output, gl_PrimitiveID and gl_FragDepth, each
#    declared once and only loaded or stored, the geometry capability and
#    the depth replacing mode.
#  - convert.glsl, interlace.glsl, merge.glsl, vr.glsl: the utility
#    shaders, a module an entry point (each compiled with <entry>=main).
#
# Debug instructions (names, source text, lines) are stripped.

import argparse
import hashlib
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VK = ROOT / "pcsx2" / "GS" / "Renderers" / "Vulkan"
SRC = VK / "shaders"
VR_BODY = ROOT / "pcsx2" / "GS" / "Renderers" / "Common" / "vr_glsl_body.h"
OUT = VK / "VKShaders.inc"

INPUTS = [SRC / "tfx.glsl", SRC / "convert.glsl", SRC / "interlace.glsl",
          SRC / "merge.glsl", SRC / "vr.glsl", VR_BODY, Path(__file__).resolve()]

# The fragment modules, in the order of their index (see VKShaders.inc)
RT_AXIS = [("", None), ("subpass", "RT_SUBPASS"), ("fetch", "RT_FETCH")]

# SPIR-V
MAGIC = 0x07230203
OP_ENTRY_POINT = 15
OP_EXECUTION_MODE = 16
OP_CAPABILITY = 17
OP_VARIABLE = 59
OP_LOAD = 61
OP_STORE = 62
OP_DECORATE = 71
OP_MEMBER_DECORATE = 72
DECORATION_FLAT = 14
DECORATION_BUILTIN = 11
DECORATION_INDEX = 32
BUILTIN_PRIMITIVE_ID = 7
BUILTIN_FRAG_DEPTH = 22
MODE_DEPTH_REPLACING = 12
CAP_GEOMETRY = 2
DEBUG_OPS = {2, 3, 4, 5, 6, 7, 8, 317, 330}  # source, names, lines, processed


def read(p):
    return p.read_text(encoding="utf-8")


def source_hash():
    h = hashlib.sha256()
    for p in INPUTS:
        h.update(p.relative_to(ROOT).as_posix().encode())
        h.update(b"\0")
        # line endings as committed on any host
        h.update(read(p).replace("\r\n", "\n").encode("utf-8"))
        h.update(b"\0")
    return h.hexdigest()


def vr_body():
    # VR_GLSL_BODY: a C macro of string literals, one a line
    text = read(VR_BODY)
    m = re.search(r"#define VR_GLSL_BODY\s*\\\n(.*?)(?:\n\s*\n|\Z)", text, re.S)
    if not m:
        sys.exit("error: no VR_GLSL_BODY in " + str(VR_BODY))
    out = []
    for lit in re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1)):
        out.append(bytes(lit, "utf-8").decode("unicode_escape"))
    return "".join(out)


def words_of(blob):
    if len(blob) % 4:
        sys.exit("error: SPIR-V not a whole number of words")
    w = list(struct.unpack("<%dI" % (len(blob) // 4), blob))
    if not w or w[0] != MAGIC:
        sys.exit("error: not SPIR-V")
    return w


def instructions(w):
    i = 5
    while i < len(w):
        n = w[i] >> 16
        if n == 0 or i + n > len(w):
            sys.exit("error: malformed SPIR-V")
        yield i, n, w[i] & 0xFFFF
        i += n


def strip_debug(w):
    out = w[:5]
    for i, n, op in instructions(w):
        if op not in DEBUG_OPS:
            out.extend(w[i:i + n])
    return out


def droppable(w, what):
    # the second colour output, gl_PrimitiveID and gl_FragDepth: each one
    # variable, used only as the pointer of a load or a store; the
    # geometry capability and depth replacing there to go with them
    want = {(DECORATION_INDEX, 1): "o_col1", (DECORATION_BUILTIN, BUILTIN_PRIMITIVE_ID): "gl_PrimitiveID",
            (DECORATION_BUILTIN, BUILTIN_FRAG_DEPTH): "gl_FragDepth"}
    ids = {}
    caps, modes = set(), set()
    for i, n, op in instructions(w):
        if op == OP_DECORATE and n >= 4 and (w[i + 2], w[i + 3]) in want:
            name = want[(w[i + 2], w[i + 3])]
            if name in ids:
                sys.exit("error: %s: %s decorated twice" % (what, name))
            ids[name] = w[i + 1]
        elif op == OP_CAPABILITY:
            caps.add(w[i + 1])
        elif op == OP_EXECUTION_MODE:
            modes.add(w[i + 2])
    if len(ids) != 3:
        sys.exit("error: %s: missing %s" % (what, sorted(set(want.values()) - set(ids))))
    if CAP_GEOMETRY not in caps or MODE_DEPTH_REPLACING not in modes:
        sys.exit("error: %s: no geometry capability or depth replacing mode" % what)
    var = set(ids.values())
    for i, n, op in instructions(w):
        operands = w[i + 1:i + n]
        if op in (OP_DECORATE, OP_ENTRY_POINT):
            continue
        if op == OP_VARIABLE:
            operands = [w[i + 1]] + w[i + 3:i + n]  # all but its own id
        elif op == OP_LOAD:
            operands = w[i + 1:i + 3] + w[i + 4:i + n]  # all but the pointer
        elif op == OP_STORE:
            operands = w[i + 2:i + n]  # the value
        if var.intersection(operands):
            sys.exit("error: %s: a dropped variable is used by opcode %d" % (what, op))


def flat_decoration(w):
    # the colour's, on its member of the VSOutput block (gl_PrimitiveID's is
    # an OpDecorate of a variable, and stays)
    found = []
    for i, n, op in instructions(w):
        if op == OP_MEMBER_DECORATE and n >= 4 and w[i + 3] == DECORATION_FLAT:
            found.append((i, n))
    if len(found) != 1:
        sys.exit("error: a TFX module has %d Flat decorations, not the colour's one" % len(found))


class Compiler:
    def __init__(self, glslang):
        self.glslang = glslang
        self.tmp = Path(tempfile.mkdtemp(prefix="vk_spirv"))

    def close(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def compile(self, text, stage, defines, what):
        src = self.tmp / "in.glsl"
        spv = self.tmp / "out.spv"
        src.write_text(text, encoding="utf-8")
        cmd = [self.glslang, "--target-env", "vulkan1.0", "-S", stage, "-V", str(src), "-o", str(spv)]
        cmd += ["-D" + d for d in defines]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            sys.stderr.write("glslangValidator failed for %s:\n%s%s" % (what, r.stdout, r.stderr))
            sys.exit(1)
        return strip_debug(words_of(spv.read_bytes()))


def spec_ids(text):
    vs, ps = [], []
    for m in re.finditer(r"layout\(constant_id\s*=\s*(\d+)\)\s*const\s+\w+\s+(\w+)", text):
        (vs if m.group(2).startswith("VS_") else ps).append((int(m.group(1)), m.group(2)))
    for lst in (vs, ps):
        ids = [i for i, _ in lst]
        if ids != list(range(len(ids))):
            sys.exit("error: specialization constant IDs not 0..n-1 in order")
    return vs, ps


def entries(text):
    # the entry points a utility source guards with #ifdef <entry>
    return [m.group(1) for m in re.finditer(r"^void (ps_\w+)\(\)", text, re.M)]


def emit_array(out, name, w):
    out.append("static const u32 %s[%d] = {" % (name, len(w)))
    for i in range(0, len(w), 8):
        out.append("\t" + ", ".join("0x%08x" % x for x in w[i:i + 8]) + ",")
    out.append("};")


def generate(glslang):
    c = Compiler(glslang)
    try:
        tfx = read(SRC / "tfx.glsl")
        vs_ids, ps_ids = spec_ids(tfx)
        mods = []   # (name, words)

        for expand in (0, 1):
            name = "vkspv_tfx_vs" + ("_expand" if expand else "")
            w = c.compile(tfx, "vert", ["VERTEX_SHADER"] + (["VS_EXPAND"] if expand else []), name)
            flat_decoration(w)
            mods.append((name, w))
        fs_names = []
        for rt_name, rt_def in RT_AXIS:
            name = "vkspv_tfx_fs" + ("_" + rt_name if rt_name else "")
            w = c.compile(tfx, "frag", ["FRAGMENT_SHADER"] + ([rt_def] if rt_def else []), name)
            flat_decoration(w)
            droppable(w, name)
            mods.append((name, w))
            fs_names.append(name)

        util_vs = []
        for src in ("convert", "interlace", "merge"):
            name = "vkspv_%s_vs" % src
            mods.append((name, c.compile(read(SRC / (src + ".glsl")), "vert", ["VERTEX_SHADER"], name)))
            util_vs.append(name)

        def util_fs(src, text, entry):
            name = "vkspv_%s_%s" % (src, entry)
            mods.append((name, c.compile(text, "frag", ["FRAGMENT_SHADER", entry + "=main"], name)))
            return name

        convert = read(SRC / "convert.glsl")
        conv = [(e, util_fs("convert", convert, e)) for e in entries(convert)]
        stencil = [util_fs("convert", convert, "ps_stencil_image_init_%d" % i) for i in range(4)]
        interlace = read(SRC / "interlace.glsl")
        inter = [util_fs("interlace", interlace, e) for e in entries(interlace)]
        merge = read(SRC / "merge.glsl")
        merg = [util_fs("merge", merge, e) for e in entries(merge)]
        vr = read(SRC / "vr.glsl").replace("// @VR_GLSL_BODY", vr_body())
        vrs = [util_fs("vr", vr, e) for e in ("ps_vr_range", "ps_vr_eye")]
        if [e for e in entries(interlace)] != ["ps_main%d" % i for i in range(len(inter))] \
                or [e for e in entries(merge)] != ["ps_main0", "ps_main1"]:
            sys.exit("error: interlace/merge entries are not ps_main0..n in order")
    finally:
        c.close()

    out = []
    out.append("/* The Vulkan renderer's SPIR-V. Written by tools/vk_spirv.py from")
    out.append(" * the GLSL under shaders/; do not edit: change that and run")
    out.append(" *   python3 tools/vk_spirv.py update")
    out.append(" * (tests/vkspirv fails until it is run). */")
    out.append("")
    out.append("/* These shaders, for what was made with them to be told from what other")
    out.append(" * shaders made (the first word of the sources' hash) */")
    out.append("#define VKSPV_VERSION 0x%su" % source_hash()[:8])
    out.append("")
    out.append("/* Specialization constant IDs: tfx.glsl's VS_ and PS_ fields */")
    out.append("enum")
    out.append("{")
    for i, n in vs_ids:
        out.append("\tVKSC_%s = %d," % (n, i))
    out.append("\tVKSC_VS_COUNT = %d," % len(vs_ids))
    for i, n in ps_ids:
        out.append("\tVKSC_%s = %d," % (n, i))
    out.append("\tVKSC_PS_COUNT = %d" % len(ps_ids))
    out.append("};")
    out.append("")
    for name, w in mods:
        emit_array(out, name, w)
        out.append("")

    def ref(n):
        return "{%s, %d}" % (n, len(dict(mods)[n]))

    out.append("/* Each constant a u32 at 4 * its ID */")
    for stage, ids in (("vs", vs_ids), ("fs", ps_ids)):
        out.append("static const VkSpecializationMapEntry vkspv_tfx_%s_spec_map[%d] = {" % (stage, len(ids)))
        for i, _ in ids:
            out.append("\t{%d, %d, 4}," % (i, 4 * i))
        out.append("};")
    out.append("/* The TFX vertex shaders: [VS_EXPAND] */")
    out.append("static const VKSpirv vkspv_tfx_vs_mod[2] = {%s, %s};" %
               (ref("vkspv_tfx_vs"), ref("vkspv_tfx_vs_expand")))
    out.append("/* The TFX fragment shaders: [how the target is read]: not at all,")
    out.append(" * as an input attachment, as a sampled image */")
    out.append("static const VKSpirv vkspv_tfx_fs_mod[%d] = {%s};" % (len(fs_names), ", ".join(ref(n) for n in fs_names)))
    out.append("/* The utility vertex shaders: convert, interlace, merge */")
    out.append("static const VKSpirv vkspv_util_vs_mod[3] = {%s};" % ", ".join(ref(n) for n in util_vs))
    out.append("/* convert.glsl's entries, by name (shaderName()) */")
    out.append("static const VKSpirvNamed vkspv_convert_mod[%d] = {" % len(conv))
    for e, n in conv:
        out.append("\t{\"%s\", %s}," % (e, ref(n)))
    out.append("};")
    out.append("static const VKSpirv vkspv_stencil_init_mod[4] = {%s};" % ", ".join(ref(n) for n in stencil))
    out.append("static const VKSpirv vkspv_interlace_mod[%d] = {%s};" % (len(inter), ", ".join(ref(n) for n in inter)))
    out.append("static const VKSpirv vkspv_merge_mod[2] = {%s};" % ", ".join(ref(n) for n in merg))
    out.append("/* range, eye */")
    out.append("static const VKSpirv vkspv_vr_mod[2] = {%s};" % ", ".join(ref(n) for n in vrs))
    body = "\n".join(out) + "\n"
    head = "/* source sha256 %s */\n/* output sha256 %s */\n" % (
        source_hash(), hashlib.sha256(body.encode()).hexdigest())
    return head + body


def check():
    if not OUT.exists():
        print("FAIL: %s missing; run: python3 tools/vk_spirv.py update" % OUT.relative_to(ROOT))
        return 1
    text = OUT.read_text(encoding="utf-8").replace("\r\n", "\n")
    m = re.match(r"/\* source sha256 ([0-9a-f]{64}) \*/\n/\* output sha256 ([0-9a-f]{64}) \*/\n", text)
    if not m:
        print("FAIL: %s has no hashes; run: python3 tools/vk_spirv.py update" % OUT.relative_to(ROOT))
        return 1
    ok = True
    if m.group(1) != source_hash():
        print("FAIL: the shader sources changed since VKShaders.inc was written;")
        print("      run: python3 tools/vk_spirv.py update")
        ok = False
    if m.group(2) != hashlib.sha256(text[m.end():].encode()).hexdigest():
        print("FAIL: VKShaders.inc was edited by hand; run: python3 tools/vk_spirv.py update")
        ok = False
    if ok:
        print("ok: VKShaders.inc matches its sources")
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description="The Vulkan renderer's SPIR-V, compiled ahead of time.")
    ap.add_argument("mode", choices=("update", "check"))
    ap.add_argument("--glslang", help="glslangValidator to use (default: from PATH)")
    a = ap.parse_args()
    if a.mode == "check":
        return check()
    glslang = a.glslang or shutil.which("glslangValidator")
    if not glslang:
        sys.exit("error: glslangValidator not found; install the Vulkan SDK or glslang-tools, or pass --glslang")
    OUT.write_bytes(generate(glslang).encode("utf-8"))
    print("wrote %s" % OUT.relative_to(ROOT))
    return 0


if __name__ == "__main__":
    sys.exit(main())
