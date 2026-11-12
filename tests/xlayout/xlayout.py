#!/usr/bin/env python3
"""Compare struct, union and enum layouts between two objects' DWARF.

usage: xlayout.py <readelf-dump-C> <readelf-dump-C++>

Each input is the output of `readelf --debug-dump=info` on an object
built from the same headers, once by the C front end and once by the C++
one. Every aggregate and enum both sides name is compared by size, and
aggregates member by member (name and byte offset). Exits 1 on any
difference.

Names are the tag, or for an anonymous type the typedef that names it,
since `typedef struct { ... } X;` is the C spelling of `struct X { ... };`.
Types only one side has (C++ classes, templates) are not compared.
"""
import re
import sys

DIE = re.compile(r'^\s*<(\d+)><([0-9a-f]+)>: Abbrev Number: \d+ \((DW_TAG_\w+)\)')
ATTR = re.compile(r'^\s*<[0-9a-f]+>\s+(DW_AT_\w+)\s*:\s*(.*)$')
KINDS = ('DW_TAG_structure_type', 'DW_TAG_union_type', 'DW_TAG_enumeration_type', 'DW_TAG_class_type')


def parse(path):
    dies = {}
    order = []
    stack = []
    cur = None
    with open(path, errors='replace') as f:
        for line in f:
            m = DIE.match(line)
            if m:
                depth, off, tag = int(m.group(1)), int(m.group(2), 16), m.group(3)
                cur = {'tag': tag, 'depth': depth, 'off': off, 'attrs': {}, 'kids': []}
                while stack and stack[-1]['depth'] >= depth:
                    stack.pop()
                if stack:
                    stack[-1]['kids'].append(cur)
                stack.append(cur)
                dies[off] = cur
                order.append(cur)
                continue
            m = ATTR.match(line)
            if m and cur is not None:
                cur['attrs'][m.group(1)] = m.group(2).strip()
    return dies, order


def name_of(d):
    v = d['attrs'].get('DW_AT_name')
    if v is None:
        return None
    # "(indirect string, offset: 0x..): name" or a plain name
    return v.split(': ')[-1].strip()


def ref(v):
    m = re.search(r'<0x([0-9a-f]+)>', v or '')
    return int(m.group(1), 16) if m else None


def num(v):
    if v is None:
        return None
    m = re.match(r'(-?\d+|0x[0-9a-f]+)', v)
    return int(m.group(1), 0) if m else None


def collect(path):
    dies, order = parse(path)
    named = {}
    # typedef names for anonymous aggregates
    alias = {}
    for d in order:
        if d['tag'] == 'DW_TAG_typedef':
            t = ref(d['attrs'].get('DW_AT_type'))
            if t in dies and name_of(dies[t]) is None:
                alias.setdefault(t, name_of(d))
    for d in order:
        if d['tag'] not in KINDS:
            continue
        if 'DW_AT_declaration' in d['attrs']:
            continue
        n = name_of(d) or alias.get(d['off'])
        if not n or d['depth'] != 1:
            continue
        kind = 'enum' if d['tag'] == 'DW_TAG_enumeration_type' else 'aggregate'
        size = num(d['attrs'].get('DW_AT_byte_size'))
        members = []
        if kind == 'aggregate':
            for k in d['kids']:
                if k['tag'] != 'DW_TAG_member':
                    continue
                if 'DW_AT_external' in k['attrs'] or 'DW_AT_declaration' in k['attrs']:
                    continue  # C++ static data member
                loc = num(k['attrs'].get('DW_AT_data_member_location')) or 0
                bit = k['attrs'].get('DW_AT_data_bit_offset')
                members.append((name_of(k) or '<anon>', loc, bit))
        # First definition wins; a later one with the same name is a
        # different scope (a struct local to an inline function) and the
        # front ends agree on which comes first.
        named.setdefault((kind, n), (size, members))
    return named


def main():
    c = collect(sys.argv[1])
    cxx = collect(sys.argv[2])
    both = sorted(set(c) & set(cxx))
    bad = 0
    for key in both:
        (cs, cm), (xs, xm) = c[key], cxx[key]
        if cs != xs:
            print('  %s %s: size %s in C, %s in C++' % (key[0], key[1], cs, xs))
            bad += 1
            continue
        if cm != xm:
            cd = dict((m[0], m[1:]) for m in cm)
            xd = dict((m[0], m[1:]) for m in xm)
            diffs = [n for n in sorted(set(cd) | set(xd)) if cd.get(n) != xd.get(n)]
            print('  %s %s: members differ: %s' % (key[0], key[1],
                  ', '.join('%s C=%s C++=%s' % (n, cd.get(n), xd.get(n)) for n in diffs[:6])))
            bad += 1
    print('%d types named on both sides compared, %d differ' % (len(both), bad))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
