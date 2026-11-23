#!/usr/bin/env python3
# Adds a module to paraLLEl-GS's shader bank: its SPIR-V goes to the end
# of spirv_bank, its reflection record (shaderbank_reflect.py) to the end
# of reflection_bank, and a member and loader line to the Shaders struct.
#
#   shaderbank_add.py SHADERS_DIR NAME program|shader module.spv
#
# Once added, the module is one like the others: list it in
# shaderbank_build.sh, which then rebuilds and checks it.
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from shaderbank_reflect import reflect, serialize  # noqa: E402


def emit(values, per_line, fmt):
    lines = []
    for i in range(0, len(values), per_line):
        lines.append("\t" + " ".join(fmt % v + "," for v in values[i:i + per_line]))
    return "\n".join(lines) + "\n"


def main():
    if len(sys.argv) != 5 or sys.argv[3] not in ("program", "shader"):
        sys.exit(__doc__ if __doc__ else "usage: shaderbank_add.py SHADERS_DIR NAME program|shader module.spv")
    shaders, name, kind, spv = sys.argv[1:]
    bank_path = os.path.join(shaders, "slangmosh.hpp")
    iface_path = os.path.join(shaders, "slangmosh_iface.hpp")
    bank = open(bank_path).read()
    iface = open(iface_path).read()

    if re.search(r"this->%s = " % re.escape(name), bank):
        sys.exit("%s is in the bank already" % name)

    data = open(spv, "rb").read()
    words = list(struct.unpack("<%dI" % (len(data) // 4), data))
    record = serialize(reflect(words))

    head = "static const uint32_t spirv_bank[] =\n{\n"
    s0 = bank.index(head) + len(head)
    s1 = bank.index("};\n", s0)
    bank_words = [int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{8})u", bank[s0:s1])]

    rhead = "static const uint8_t reflection_bank[] =\n{\n"
    r0 = bank.index(rhead) + len(rhead)
    r1 = bank.index("};\n", r0)
    rbytes = [int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2}),", bank[r0:r1])]

    loader = "\tlayout.unserialize(reflection_bank + %d, %d);\n\tthis->%s = device.request_%s(spirv_bank + %d, %d, &layout);\n" % (
        len(rbytes), len(record), name, kind, len(bank_words), len(data))

    end = bank.index("}\n}\n", bank.index("Shaders<Program, Shader>::Shaders("))
    bank = (bank[:s0] + emit(bank_words + words, 8, "0x%08xu") + bank[s1:r0] +
            emit(rbytes + list(record), 32, "0x%02x") + bank[r1:end] + loader + bank[end:])

    # NAME may index an array member, upload_no8[1] say: the member is
    # declared, or grown, to hold it.
    m = re.match(r"(\w+)((?:\[\d+\])*)$", name)
    base, index = m.group(1), [int(x) for x in re.findall(r"\d+", m.group(2))]
    typ = "Program" if kind == "program" else "Shader"
    decl = re.search(r"\t%s %s((?:\[\d+\])*) = \{\};\n" % (typ, base), iface)
    if decl:
        dims = [max(int(x), i + 1) for x, i in zip(re.findall(r"\d+", decl.group(1)), index)]
        iface = iface.replace(decl.group(0), "\t%s %s%s = {};\n" % (typ, base, "".join("[%d]" % d for d in dims)), 1)
    else:
        anchor = "\tShaders() = default;\n"
        member = "\t%s %s%s = {};\n" % (typ, base, "".join("[%d]" % (i + 1) for i in index))
        iface = iface.replace(anchor, member + anchor, 1)

    open(bank_path, "w").write(bank)
    open(iface_path, "w").write(iface)
    print("added %s: %d bytes of SPIR-V, record at %d" % (name, len(data), len(rbytes)))


if __name__ == "__main__":
    main()
