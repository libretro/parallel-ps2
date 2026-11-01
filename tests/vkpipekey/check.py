#!/usr/bin/env python3
# Holds pcsx2/GS/Renderers/Vulkan/VKPipelineKey.h to the SPIR-V the core is
# built with (VKShaders.inc), no GPU or compiler needed.
#
# A module is specialized here as a driver would: the specialization
# constants take a selector's values, the constant expressions over them
# are folded, and a branch on a constant goes one way only. What the
# entry point can then reach - the functions, the blocks, the
# instructions, with every value that came from a constant printed as
# the value - is what the selector runs. For each selector from
# selectors.cpp (read on stdin):
#
#  - it and its key run the same instructions (the TFX fragment module
#    that samples its target, which reads everything; the vertex modules
#    likewise);
#  - a selector VKTFXReadsRT says reads no target never loads it, nor does
#    its key, so the module without the target is enough for both;
#  - a key whose primitive ID VKTFXFragmentDrop drops never loads it.
#
# The constant folding covers the operations glslang makes of tfx.glsl's
# constant expressions; anything else it meets with constant operands it
# leaves unfolded, which can only make two selectors look different, never
# the same.

import re
import sys
from pathlib import Path

INC = Path(__file__).resolve().parent.parent.parent / "pcsx2" / "GS" / "Renderers" / "Vulkan" / "VKShaders.inc"

# opcodes
OP_ENTRY_POINT = 15
OP_TYPE_BOOL, OP_TYPE_INT = 20, 21
OP_CONST_TRUE, OP_CONST_FALSE, OP_CONST = 41, 42, 43
OP_SPEC_TRUE, OP_SPEC_FALSE, OP_SPEC, OP_SPEC_OP = 48, 49, 50, 52
OP_FUNCTION, OP_FUNCTION_END, OP_CALL = 54, 56, 57
OP_VARIABLE, OP_LOAD, OP_STORE = 59, 61, 62
OP_DECORATE = 71
OP_PHI, OP_LOOP_MERGE, OP_SELECTION_MERGE = 245, 246, 247
OP_LABEL, OP_BRANCH, OP_BRANCH_COND, OP_SWITCH = 248, 249, 250, 251
TERMINATORS = {249, 250, 251, 252, 253, 254, 255}
DECO_SPEC_ID, DECO_BUILTIN, DECO_SET, DECO_BINDING = 1, 11, 34, 33

M32 = 0xFFFFFFFF


def s32(v):
    v &= M32
    return v - (1 << 32) if v & 0x80000000 else v


# the operations folded: opcode -> (operand count, function of the values)
FOLD = {
    83: (1, lambda a: a),                                   # CopyObject
    113: (1, lambda a: a), 114: (1, lambda a: a),          # UConvert, SConvert
    124: (1, lambda a: a),                                  # Bitcast (int <-> int)
    126: (1, lambda a: (-a) & M32),                         # SNegate
    128: (2, lambda a, b: (a + b) & M32),                   # IAdd
    130: (2, lambda a, b: (a - b) & M32),                   # ISub
    132: (2, lambda a, b: (a * b) & M32),                   # IMul
    164: (2, lambda a, b: a == b), 165: (2, lambda a, b: a != b),   # LogicalEqual, NotEqual
    166: (2, lambda a, b: a or b), 167: (2, lambda a, b: a and b),  # LogicalOr, And
    168: (1, lambda a: not a),                              # LogicalNot
    169: (3, lambda c, a, b: a if c else b),                # Select
    170: (2, lambda a, b: a == b), 171: (2, lambda a, b: a != b),   # IEqual, INotEqual
    172: (2, lambda a, b: (a & M32) > (b & M32)),           # UGreaterThan
    173: (2, lambda a, b: s32(a) > s32(b)),                 # SGreaterThan
    174: (2, lambda a, b: (a & M32) >= (b & M32)),
    175: (2, lambda a, b: s32(a) >= s32(b)),
    176: (2, lambda a, b: (a & M32) < (b & M32)),
    177: (2, lambda a, b: s32(a) < s32(b)),
    178: (2, lambda a, b: (a & M32) <= (b & M32)),
    179: (2, lambda a, b: s32(a) <= s32(b)),
    194: (2, lambda a, b: (a & M32) >> (b & 31)),           # ShiftRightLogical
    195: (2, lambda a, b: (s32(a) >> (b & 31)) & M32),      # ShiftRightArithmetic
    196: (2, lambda a, b: (a << (b & 31)) & M32),           # ShiftLeftLogical
    197: (2, lambda a, b: (a | b) & M32), 198: (2, lambda a, b: (a ^ b) & M32),
    199: (2, lambda a, b: (a & b) & M32), 200: (1, lambda a: (~a) & M32),
}


def load_modules():
    text = INC.read_text()
    mods = {}
    for m in re.finditer(r"static const u32 (\w+)\[\d+\] = \{(.*?)\};", text, re.S):
        mods[m.group(1)] = [int(x, 16) for x in re.findall(r"0x[0-9a-f]+", m.group(2))]
    return mods


class Module:
    def __init__(self, w):
        self.insts = []        # (op, operands)
        i = 5
        while i < len(w):
            n = w[i] >> 16
            self.insts.append((w[i] & 0xFFFF, w[i + 1:i + n]))
            i += n
        self.spec_id = {}      # result id -> spec id
        self.builtin = {}      # builtin -> variable
        self.binding = {}      # (set, binding) -> variable
        sets, binds = {}, {}
        self.defs = {}         # result id -> (op, operands)
        self.def_block = {}    # result id -> the block it is in
        self.consts = {}       # result id -> value of a plain constant
        self.spec = []         # spec constant definitions, in order
        self.functions = {}    # id -> list of (label, [insts])
        self.entry = None
        types = {}
        fn = None
        block = None
        for op, o in self.insts:
            if op == OP_DECORATE:
                if o[1] == DECO_SPEC_ID:
                    self.spec_id[o[0]] = o[2]
                elif o[1] == DECO_BUILTIN:
                    self.builtin[o[2]] = o[0]
                elif o[1] == DECO_SET:
                    sets[o[0]] = o[2]
                elif o[1] == DECO_BINDING:
                    binds[o[0]] = o[2]
            elif op == OP_ENTRY_POINT:
                self.entry = o[1]
            elif op in (OP_TYPE_BOOL, OP_TYPE_INT):
                types[o[0]] = op
            elif op in (OP_CONST_TRUE, OP_CONST_FALSE):
                self.consts[o[1]] = op == OP_CONST_TRUE
            elif op == OP_CONST and len(o) == 3:
                # an int, or a float's bits (only ever selected between)
                self.consts[o[1]] = o[2]
            elif op in (OP_SPEC_TRUE, OP_SPEC_FALSE, OP_SPEC, OP_SPEC_OP):
                self.spec.append((op, o))
            elif op == OP_FUNCTION:
                fn = o[1]
                self.functions[fn] = []
            elif op == OP_FUNCTION_END:
                fn = None
            elif op == OP_LABEL and fn is not None:
                block = (o[0], [])
                self.functions[fn].append(block)
            elif fn is not None and block is not None:
                block[1].append((op, o))
            if fn is not None and (op in FOLD or op == OP_PHI):
                self.defs[o[1]] = (op, o)
                self.def_block[o[1]] = block[0] if block else None
        for var, s in sets.items():
            if var in binds:
                self.binding[(s, binds[var])] = var

    def specialize(self, values):
        """values: spec id -> value. Returns the listing of what runs."""
        known = {}       # id -> value, for what came from a specialization constant
        for op, o in self.spec:
            rid = o[1]
            sid = self.spec_id.get(rid)
            if op in (OP_SPEC_TRUE, OP_SPEC_FALSE):
                v = values[sid] != 0 if sid in values else (op == OP_SPEC_TRUE)
            elif op == OP_SPEC:
                v = values[sid] & M32 if sid in values else o[2]
            else:
                inner, args = o[2], o[3:]
                if inner not in FOLD or FOLD[inner][0] != len(args):
                    sys.exit("check.py: a constant expression of opcode %d is not folded here" % inner)
                vals = [self.value(a, known, None) for a in args]
                if any(x is None for x in vals):
                    sys.exit("check.py: a constant expression over something not constant")
                v = FOLD[inner][1](*vals)
            known[rid] = v

        listing = []
        done, todo = set(), [self.entry]
        while todo:
            f = todo.pop(0)
            if f in done:
                continue
            done.add(f)
            blocks = self.functions[f]
            reach, edges = self.reachable(blocks, known)
            for label, insts in blocks:
                if label not in reach:
                    continue
                listing.append(("block", label))
                for op, o in insts:
                    if op in (OP_SELECTION_MERGE, OP_LOOP_MERGE):
                        continue
                    if op == OP_CALL:
                        todo.append(o[2])
                    if op == OP_BRANCH_COND:
                        c = self.value(o[0], known, edges)
                        if c is not None:
                            listing.append(("branch", o[1] if c else o[2]))
                            continue
                    listing.append((op,) + tuple(("v", known[x]) if x in known else x for x in o))
        return listing

    def value(self, i, known, edges):
        """the constant value of i, folding what it can; None if not one.
        edges: the branches taken, for a phi (None: none known)"""
        if i in known:
            return known[i]
        if i in self.consts:
            return self.consts[i]
        d = self.defs.get(i)
        if d is None:
            return None
        op, o = d
        if op in FOLD and FOLD[op][0] == len(o) - 2:
            vals = [self.value(a, known, edges) for a in o[2:]]
            if any(x is None for x in vals):
                return None
            v = FOLD[op][1](*vals)
            # a value of the selector's: printed as one wherever it is used
            if any(a in known for a in o[2:]):
                known[i] = v
            return v
        if op == OP_PHI and edges is not None:
            vals = set()
            here = self.def_block.get(i)
            for k in range(2, len(o), 2):
                if (o[k + 1], here) in edges:
                    v = self.value(o[k], known, edges)
                    if v is None:
                        return None
                    vals.add(v)
            if len(vals) == 1:
                return vals.pop()
        return None

    def reachable(self, blocks, known):
        # a phi is folded over the branches the pass before found taken;
        # passes until they hold, and if they never do the union of the
        # last two (which only adds blocks)
        by_label = dict(blocks)
        reach, taken = set(), set()
        for _ in range(8):
            before, before_taken = set(reach), set(taken)
            new = {blocks[0][0]}
            taken = set()
            todo = [blocks[0][0]]
            while todo:
                lab = todo.pop()
                insts = by_label[lab]
                if not insts:
                    continue
                op, o = insts[-1]
                nxt = []
                if op == OP_BRANCH:
                    nxt = [o[0]]
                elif op == OP_BRANCH_COND:
                    c = self.value(o[0], known, before_taken)
                    nxt = [o[1], o[2]] if c is None else [o[1] if c else o[2]]
                elif op == OP_SWITCH:
                    sel = self.value(o[0], known, before_taken)
                    cases = list(zip(o[2::2], o[3::2]))
                    if sel is None:
                        nxt = [o[1]] + [t for _, t in cases]
                    else:
                        nxt = [t for lit, t in cases if lit == sel] or [o[1]]
                for t in nxt:
                    taken.add((lab, t))
                    if t not in new:
                        new.add(t)
                        todo.append(t)
            reach = new
            if reach == before and taken == before_taken:
                return reach, taken
        return reach | before, taken | before_taken


def loads(listing, var):
    return any(e[0] == OP_LOAD and len(e) > 3 and e[3] == var for e in listing)


def main():
    mods = load_modules()
    fs = Module(mods["vkspv_tfx_fs_fetch"])
    vs = [Module(mods["vkspv_tfx_vs"]), Module(mods["vkspv_tfx_vs_expand"])]
    rt = fs.binding.get((2, 0))
    primid = fs.builtin.get(7)
    if rt is None or primid is None:
        sys.exit("check.py: no target or primitive ID in the fragment module")
    failed = 0
    n_ps = n_vs = 0
    for line in sys.stdin:
        part = line.split("|")
        if line.startswith("FAIL"):
            print("  " + line.rstrip())
            failed += 1
            continue
        if line.startswith("P"):
            n_ps += 1
            a = list(map(int, part[0].split()[1:]))
            b = list(map(int, part[1].split()))
            reads_a, reads_b, drop = map(int, part[2].split())
            la = fs.specialize(dict(enumerate(a)))
            lb = fs.specialize(dict(enumerate(b)))
            what = []
            if la != lb:
                diff = [i for i in range(len(a)) if a[i] != b[i]]
                what.append("its key (constants %s zeroed) runs other instructions" % diff)
            if not reads_a and loads(la, rt):
                what.append("it reads the target VKTFXReadsRT says it does not")
            if not reads_b and loads(lb, rt):
                what.append("its key reads the target VKTFXReadsRT says it does not")
            if (drop & 2) and loads(lb, primid):
                what.append("its key loads the primitive ID it drops")
            if what:
                failed += 1
                if failed <= 10:
                    print("  FAIL: selector %s: %s" % (" ".join(map(str, a)), "; ".join(what)))
        elif line.startswith("V"):
            n_vs += 1
            head = part[0].split()
            expand = int(head[1])
            a = list(map(int, head[2:]))
            b = list(map(int, part[1].split()))
            if vs[expand].specialize(dict(enumerate(a))) != vs[expand].specialize(dict(enumerate(b))):
                failed += 1
                if failed <= 10:
                    print("  FAIL: vertex selector %s: its key runs other instructions" % " ".join(map(str, a)))
    if failed or not n_ps or not n_vs:
        print("  %d of %d selectors failed" % (failed, n_ps + n_vs))
        return 1
    print("  ok: %d pixel and %d vertex selectors run as their keys do" % (n_ps, n_vs))
    return 0


if __name__ == "__main__":
    sys.exit(main())
