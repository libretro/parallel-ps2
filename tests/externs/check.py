"""Every variable is declared with the type it is defined with.

Usage: check.py <tree root>

A variable declared `extern` in one file and defined in another links on
ELF whatever the two say its type is, because a C++ variable's symbol
there is its bare name; MSVC's symbol for it spells the type out, so a
mismatch is an unresolved external there and only there. (A unique_ptr
declaration of a variable defined as a raw pointer built everywhere but
Windows.) This collects every file-scope `extern T name;` and every
file-scope definition `T name;` / `T name = ...;` in the core's sources
and headers, and fails on a name whose declarations and definition do
not agree on the type.

The parse is line-based and deliberately narrow: one declarator per
line, starting in column 0 (or one tab in, for declarations inside an
extern "C" block). That is how every global in the core is written.
"""
import os
import re
import sys

SKIP_DIRS = ("3rdparty", os.path.join("libretro", "libretro-common"),
             os.path.join("pcsx2", "GS", "parallel-gs", "Granite"), "tests")
EXTS = (".c", ".cpp", ".h", ".hpp", ".inl")

QUAL = r"(?:(?:alignas|PCSX2_ALIGN|__declspec)\s*\([^)]*\)\s*)*"
DECL = re.compile(r"^\t?" + QUAL + r"extern\s+" + QUAL +
                  r"(?P<type>(?!\"C\")[A-Za-z_][\w:<>,\s\*&]*?)\s*(?<![\w:])(?P<name>[A-Za-z_]\w*)\s*(?P<arr>\[[^\]]*\])?\s*;")
DEFN = re.compile(r"^" + QUAL +
                  r"(?P<type>(?!(?:return|typedef|extern|static|using|namespace|template|goto|case|else|delete|throw)\b)[A-Za-z_][\w:<>,\s\*&]*?)\s*(?<![\w:])(?P<name>[A-Za-z_]\w*)\s*(?P<arr>\[[^\]]*\])?\s*(?:=[^;]*)?;")


def norm(t, arr):
    t = re.sub(r"\bconstexpr\b", "const", t)
    t = re.sub(r"\b(struct|union|enum|volatile)\b", " ", t)
    t = re.sub(r"\s*([\*&<>,])\s*", r"\1", t)
    t = re.sub(r"\s+", " ", t).strip()
    return t + ("[]" if arr is not None else "")


def split_top(t):
    out, depth, cur = [], 0, ""
    for ch in t:
        if ch == "<":
            depth += 1
        elif ch == ">":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur)
            cur = ""
        else:
            cur += ch
    out.append(cur)
    return out


def top_level_comma(t):
    return len(split_top(t)) > 1


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def main():
    root = sys.argv[1]
    decls = {}
    defs = {}
    for top in ("pcsx2", "common", "libretro"):
        for d, dirs, files in os.walk(os.path.join(root, top)):
            rel = os.path.relpath(d, root)
            if any(rel == s or rel.startswith(s + os.sep) for s in SKIP_DIRS):
                dirs[:] = []
                continue
            for f in files:
                if not f.endswith(EXTS):
                    continue
                path = os.path.join(d, f)
                text = strip_comments(open(path, encoding="utf-8", errors="replace").read())
                depth = 0
                for no, line in enumerate(text.split("\n"), 1):
                    at_file_scope = depth == 0
                    m = DECL.match(line)
                    if m and "(" not in m.group("type"):
                        where = "%s:%d" % (os.path.relpath(path, root), no)
                        t = m.group("type")
                        names = [m.group("name")]
                        if top_level_comma(t):
                            # extern T a, b, c;
                            parts = split_top(t + " " + m.group("name"))
                            first = re.match(r"(.*?)(?<![\w:])([A-Za-z_]\w*)$", parts[0].strip())
                            t = first.group(1)
                            names = [first.group(2)] + [x.strip() for x in parts[1:]]
                        for n in names:
                            decls.setdefault(n, []).append((norm(t, m.group("arr")), where))
                    elif at_file_scope and f.endswith((".c", ".cpp")):
                        m = DEFN.match(line)
                        if m and "(" not in line.split("=")[0]:
                            where = "%s:%d" % (os.path.relpath(path, root), no)
                            t = m.group("type")
                            names = [m.group("name")]
                            if top_level_comma(t):
                                # T a, b, c;
                                parts = split_top(t + " " + m.group("name"))
                                first = re.match(r"(.*?)(?<![\w:])([A-Za-z_]\w*)$", parts[0].strip())
                                t = first.group(1)
                                names = [first.group(2)] + [x.strip() for x in parts[1:]]
                            for n in names:
                                defs.setdefault(n, []).append((norm(t, m.group("arr")), where))
                    # Braces opened by extern "C" and namespaces do not
                    # make a scope a global can't be at.
                    if re.match(r'^\s*(extern\s+"C"|namespace\b)', line) and "{" in line:
                        continue
                    depth += line.count("{") - line.count("}")
                    if depth < 0:
                        depth = 0
    bad = 0
    for name, ds in sorted(decls.items()):
        if name not in defs or len(defs[name]) != 1:
            continue
        dtype, dwhere = defs[name][0]
        for t, where in ds:
            if t != dtype:
                bad += 1
                print("%s: extern %s %s; but %s defines it as %s" % (where, t, name, dwhere, dtype))
    checked = sum(1 for n in decls if n in defs and len(defs[n]) == 1)
    print("%d variables declared extern checked against their definitions, %d mismatches" % (checked, bad))
    return 1 if bad or checked == 0 else 0


if __name__ == "__main__":
    sys.exit(main())
