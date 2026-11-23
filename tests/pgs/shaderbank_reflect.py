#!/usr/bin/env python3
# Granite's reflection of a paraLLEl-GS SPIR-V module: the 348-byte
# ResourceLayout record shaders/slangmosh.hpp keeps beside each module
# (magic "GRA" and version 7, then four DescriptorSetLayouts and the
# input, output, push constant, spec constant and bindless masks).
#
#   shaderbank_reflect.py module.spv        prints the record as hex
#   shaderbank_reflect.py --check BANK      reflects every module in the
#                                           bank, compares with its record
#
# A module added to the bank gets its record from here; --check holds
# this to what slangmosh wrote for the modules it built.
import struct
import sys

NUM_SETS = 4
NUM_BINDINGS = 32
UNSIZED = 0x7F
VERSION = 7

OP_DECORATE, OP_MEMBER_DECORATE = 71, 72
OP_TYPE_INT, OP_TYPE_FLOAT, OP_TYPE_VECTOR, OP_TYPE_MATRIX = 21, 22, 23, 24
OP_TYPE_IMAGE, OP_TYPE_SAMPLER, OP_TYPE_SAMPLED_IMAGE = 25, 26, 27
OP_TYPE_ARRAY, OP_TYPE_RUNTIME_ARRAY, OP_TYPE_STRUCT, OP_TYPE_POINTER = 28, 29, 30, 32
OP_CONSTANT, OP_VARIABLE, OP_ENTRY_POINT = 43, 59, 15
DEC_SPEC_ID, DEC_BLOCK, DEC_BUFFER_BLOCK, DEC_BUILTIN = 1, 2, 3, 11
DEC_LOCATION, DEC_BINDING, DEC_SET, DEC_OFFSET = 30, 33, 34, 35
DEC_ARRAY_STRIDE, DEC_MATRIX_STRIDE = 6, 7
SC_UNIFORM_CONSTANT, SC_INPUT, SC_UNIFORM, SC_OUTPUT = 0, 1, 2, 3
SC_PUSH_CONSTANT, SC_STORAGE_BUFFER = 9, 12
DIM_BUFFER, DIM_SUBPASS = 5, 6


def reflect(words):
    assert words[0] == 0x07230203
    types = {}
    consts = {}
    decor = {}
    member_offsets = {}
    member_dec = {}
    variables = []
    interface = set()
    i = 5
    while i < len(words):
        op = words[i] & 0xFFFF
        n = words[i] >> 16
        a = words[i + 1:i + n]
        if op == OP_ENTRY_POINT:
            name_end = 2
            while True:
                w = a[name_end]
                name_end += 1
                if (w >> 24) == 0 or (w & 0xFF) == 0 or ((w >> 8) & 0xFF) == 0 or ((w >> 16) & 0xFF) == 0:
                    break
            interface.update(a[name_end:])
        elif op == OP_DECORATE:
            decor.setdefault(a[0], {})[a[1]] = a[2] if len(a) > 2 else True
        elif op == OP_MEMBER_DECORATE:
            member_dec.setdefault(a[0], {}).setdefault(a[1], {})[a[2]] = a[3] if len(a) > 3 else True
            if a[2] == DEC_OFFSET:
                member_offsets.setdefault(a[0], {})[a[1]] = a[3]
        elif op in (OP_TYPE_INT, OP_TYPE_FLOAT):
            types[a[0]] = (op, a[1])
        elif op in (OP_TYPE_VECTOR, OP_TYPE_MATRIX):
            types[a[0]] = (op, a[1], a[2])
        elif op == OP_TYPE_IMAGE:
            types[a[0]] = (op, a[1], a[2], a[6])
        elif op == OP_TYPE_SAMPLER:
            types[a[0]] = (op,)
        elif op == OP_TYPE_SAMPLED_IMAGE:
            types[a[0]] = (op, a[1])
        elif op == OP_TYPE_ARRAY:
            types[a[0]] = (op, a[1], a[2])
        elif op == OP_TYPE_RUNTIME_ARRAY:
            types[a[0]] = (op, a[1])
        elif op == OP_TYPE_STRUCT:
            types[a[0]] = (op, list(a[1:]))
        elif op == OP_TYPE_POINTER:
            types[a[0]] = (op, a[1], a[2])
        elif op == OP_CONSTANT:
            consts[a[1]] = a[2]
        elif op == OP_VARIABLE:
            variables.append((a[0], a[1], a[2]))
        i += n

    def size_of(t):
        ty = types[t]
        if ty[0] in (OP_TYPE_INT, OP_TYPE_FLOAT):
            return ty[1] // 8
        if ty[0] == OP_TYPE_VECTOR:
            return size_of(ty[1]) * ty[2]
        if ty[0] == OP_TYPE_MATRIX:
            stride = None
            return size_of(ty[1]) * ty[2] if stride is None else stride * ty[2]
        if ty[0] == OP_TYPE_ARRAY:
            stride = decor.get(t, {}).get(DEC_ARRAY_STRIDE)
            return stride * consts[ty[2]] if stride else size_of(ty[1]) * consts[ty[2]]
        if ty[0] == OP_TYPE_STRUCT:
            offs = member_offsets.get(t, {})
            end = 0
            for m, mt in enumerate(ty[1]):
                end = max(end, offs.get(m, 0) + size_of(mt))
            return end
        raise ValueError("size of type %d" % t)

    layout = {
        "sets": [{k: 0 for k in ("sampled_image", "storage_image", "uniform_buffer", "storage_buffer",
                                 "rtas", "sampled_texel_buffer", "storage_texel_buffer",
                                 "input_attachment", "sampler", "separate_image", "fp",
                                 "immutable_sampler")} for _ in range(NUM_SETS)],
        "meta": [[0] * NUM_BINDINGS for _ in range(NUM_SETS)],
        "input": 0, "output": 0, "push": 0, "spec": 0, "bindless": 0,
    }
    for target, d in decor.items():
        if DEC_SPEC_ID in d:
            layout["spec"] |= 1 << d[DEC_SPEC_ID]

    for ptr, var, sc in variables:
        pointee = types[ptr][2]
        d = decor.get(var, {})
        if sc in (SC_INPUT, SC_OUTPUT):
            if var not in interface or DEC_LOCATION not in d:
                continue
            st = types[pointee]
            count = consts[st[2]] if st[0] == OP_TYPE_ARRAY else 1
            for k in range(count):
                layout["input" if sc == SC_INPUT else "output"] |= 1 << (d[DEC_LOCATION] + k)
            continue
        if sc == SC_PUSH_CONSTANT:
            layout["push"] = size_of(pointee)
            continue
        if sc not in (SC_UNIFORM_CONSTANT, SC_UNIFORM, SC_STORAGE_BUFFER):
            continue
        s, b = d[DEC_SET], d[DEC_BINDING]
        size = 1
        base = pointee
        if types[base][0] == OP_TYPE_ARRAY:
            size = consts[types[base][2]]
            base = types[base][1]
        elif types[base][0] == OP_TYPE_RUNTIME_ARRAY:
            size = UNSIZED
            base = types[base][1]
            layout["bindless"] |= 1 << s
        ty = types[base]
        sets = layout["sets"][s]
        bit = 1 << b
        if sc == SC_STORAGE_BUFFER or (sc == SC_UNIFORM and DEC_BUFFER_BLOCK in decor.get(base, {})):
            sets["storage_buffer"] |= bit
        elif sc == SC_UNIFORM:
            sets["uniform_buffer"] |= bit
        elif ty[0] == OP_TYPE_SAMPLER:
            sets["sampler"] |= bit
        elif ty[0] in (OP_TYPE_SAMPLED_IMAGE, OP_TYPE_IMAGE):
            img = types[ty[1]] if ty[0] == OP_TYPE_SAMPLED_IMAGE else ty
            fp = types[img[1]][0] == OP_TYPE_FLOAT
            dim, sampled = img[2], img[3]
            if dim == DIM_SUBPASS:
                sets["input_attachment"] |= bit
            elif dim == DIM_BUFFER:
                sets["sampled_texel_buffer" if sampled == 1 else "storage_texel_buffer"] |= bit
            elif sampled == 2:
                sets["storage_image"] |= bit
            elif ty[0] == OP_TYPE_SAMPLED_IMAGE:
                sets["sampled_image"] |= bit
            else:
                sets["separate_image"] |= bit
            if fp and size != UNSIZED:
                sets["fp"] |= bit
        layout["meta"][s][b] = size
    return layout


def serialize(layout):
    out = struct.pack("<4H", ord("G"), ord("R"), ord("A"), VERSION)
    order = ("sampled_image", "storage_image", "uniform_buffer", "storage_buffer", "rtas",
             "sampled_texel_buffer", "storage_texel_buffer", "input_attachment", "sampler",
             "separate_image", "fp", "immutable_sampler")
    for s in range(NUM_SETS):
        out += struct.pack("<12I", *(layout["sets"][s][k] for k in order))
        out += bytes(layout["meta"][s])
    out += struct.pack("<5I", layout["input"], layout["output"], layout["push"], layout["spec"],
                       layout["bindless"])
    return out


def bank_modules(path):
    import re
    text = open(path).read()
    words = [int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{8})u", text[:text.index("reflection_bank")])]
    rbytes = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2}),", text[text.index("reflection_bank"):text.index("template <typename Program")]))
    mods = []
    for m in re.finditer(r"layout\.unserialize\(reflection_bank \+ (\d+), (\d+)\);\s*this->(\S+) = device\.request_\w+\(spirv_bank \+ (\d+), (\d+),", text):
        roff, rsize, name, off, size = int(m.group(1)), int(m.group(2)), m.group(3), int(m.group(4)), int(m.group(5))
        mods.append((name, words[off:off + size // 4], rbytes[roff:roff + rsize]))
    return mods


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "--check":
        bad = 0
        for name, words, rec in bank_modules(sys.argv[2]):
            got = serialize(reflect(words))
            if got != rec:
                bad += 1
                diffs = [k for k in range(len(rec)) if k >= len(got) or got[k] != rec[k]]
                print("%s: differs at bytes %s" % (name, diffs[:16]))
        print("shaderbank_reflect: %s" % ("ok" if not bad else "FAIL"))
        sys.exit(1 if bad else 0)
    data = open(sys.argv[1], "rb").read()
    words = list(struct.unpack("<%dI" % (len(data) // 4), data))
    print(serialize(reflect(words)).hex())


if __name__ == "__main__":
    main()
