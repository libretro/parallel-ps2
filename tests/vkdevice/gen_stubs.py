#!/usr/bin/env python3
"""Writes stubs_vk.c: a stand-in for each Vulkan entry point the core
loads (VKEntryPoints.inl), which logs its name and every argument through
what they point at (vk.xml's lengths), calls hook_<name> from stubs.c
where there is one, and makes the handles it returns.

    gen_stubs.py <vk.xml> <VKEntryPoints.inl> <stubs.c> <vulkan_core.h> > stubs_vk.c

A union is not logged inside a struct (what it holds depends on what the
struct is for: the hooks log those), and a struct stubs.c serialises
itself (ser_override_<name>) is left to it."""
import re
import sys
import xml.etree.ElementTree as ET

xml_path, inl_path, stubs_path, core_path = sys.argv[1:5]
root = ET.parse(xml_path).getroot()
core = open(core_path).read()
stubs = open(stubs_path).read()
hooks = set(re.findall(r'\bhook_(vk\w+)\s*\(', stubs))
overrides = set(re.findall(r'\bser_override_(Vk\w+)\s*\(', stubs))
entries = re.findall(r'ENTRY_POINT\((vk\w+),', open(inl_path).read())

SCALARS = {'uint8_t', 'uint16_t', 'uint32_t', 'uint64_t', 'int8_t', 'int16_t', 'int32_t', 'int64_t', 'size_t',
	'VkBool32', 'VkDeviceSize', 'VkDeviceAddress', 'VkSampleMask', 'VkFlags', 'VkFlags64', 'char', 'int'}
FLOATS = {'float', 'double'}

types = {}
handles = set()
enums = set()
for t in root.find('types'):
	cat = t.get('category')
	name = t.get('name') or (t.find('name').text if t.find('name') is not None else None)
	if not name:
		continue
	if cat == 'handle':
		handles.add(name)
		if t.get('alias'):
			continue
	elif cat in ('struct', 'union'):
		if t.get('alias'):
			types[name] = ('alias', t.get('alias'))
		else:
			types[name] = (cat, t)
	elif cat in ('enum', 'bitmask', 'basetype'):
		enums.add(name)


def resolve(name):
	while name in types and types[name][0] == 'alias':
		name = types[name][1]
	return name


def in_core(name):
	return re.search(r'\b(typedef (struct|union) %s\b|typedef \w+ %s;)' % (name, name), core) is not None


def parse_decl(m):
	"""(type, name, pointer depth, const, array, len, altlen)"""
	text = ''.join(m.itertext())
	t = m.find('type').text
	n = m.find('name').text
	tail = text[text.index(n) + len(n):]
	head = text[:text.index(n)]
	if m.find('comment') is not None:
		tail = tail.replace(m.find('comment').text or '', '')
	arr = re.match(r'\s*\[([^\]]+)\]', tail)
	depth = head.count('*')
	const = head.strip().startswith('const')
	return t, n, depth, const, (arr.group(1) if arr else None), m.get('len'), m.get('altlen')


def members(name):
	cat, t = types[resolve(name)]
	out = []
	for m in t.findall('member'):
		if m.get('api') and 'vulkan' not in m.get('api').split(','):
			continue
		out.append(parse_decl(m))
	return out


def len_expr(ln, alt, prefix):
	if alt:
		return re.sub(r'\b([a-zA-Z_]\w*)\b', lambda x: prefix + x.group(1) if not x.group(1).isupper() else x.group(1), alt)
	first = ln.split(',')[0]
	if first == 'null-terminated':
		return None
	return re.sub(r'\b([a-z]\w*)\b', lambda x: prefix + x.group(1), first.replace('-&gt;', '->'))


need = []
done = set()


def want(name):
	name = resolve(name)
	if name not in done and name in types:
		done.add(name)
		need.append(name)


def ser_value(t, n, depth, const, arr, ln, alt, expr, prefix, out, ind):
	"""Code logging one member or parameter whose value is expr."""
	tt = resolve(t)
	if n == 'pNext':
		out.append('%sser_pnext(%s);' % (ind, expr))
		return
	if arr:
		out.append('%slog_bytes(%s, sizeof(%s[0]) * (%s));' % (ind, expr, expr, arr))
		return
	if t.startswith('PFN_'):
		out.append('%slog_word(%s != NULL);' % (ind, expr))
		return
	if depth == 0:
		if tt in handles:
			out.append('%slog_handle((u64)(uintptr_t)%s);' % (ind, expr))
		elif tt in types:
			if types[tt][0] == 'union':
				out.append('%slog_word(0xC1EA2u); /* (%s: the hooks) */' % (ind, tt))
			else:
				want(tt)
				out.append('%sser_%s(&%s);' % (ind, tt, expr))
		elif tt in FLOATS:
			out.append('%slog_float((float)%s);' % (ind, expr))
		else:
			out.append('%slog_word((u64)%s);' % (ind, expr))
		return
	# a pointer
	count = len_expr(ln, alt, prefix) if (ln or alt) else None
	if t == 'char' and depth == 1:
		out.append('%sif (%s) log_string(%s); else log_word(0);' % (ind, expr, expr))
		return
	if t == 'char' and depth == 2:
		out.append('%sif (%s) { u32 i_; for (i_ = 0; i_ < (u32)(%s); i_++) log_string(%s[i_]); } else log_word(0);'
			% (ind, expr, count, expr))
		return
	if t == 'void':
		if count:
			out.append('%sif (%s) log_bytes(%s, (size_t)(%s)); else log_word(0);' % (ind, expr, expr, count))
		else:
			out.append('%slog_word(%s != NULL);' % (ind, expr))
		return
	if count is None:
		count = '1'
	out.append('%sif (%s)' % (ind, expr))
	out.append('%s{' % ind)
	out.append('%s\tu32 i_%d;' % (ind, len(ind)))
	out.append('%s\tfor (i_%d = 0; i_%d < (u32)(%s); i_%d++)' % (ind, len(ind), len(ind), count, len(ind)))
	el = '%s[i_%d]' % (expr, len(ind))
	sub = []
	ser_value(t, n + '[]', 0, const, None, None, None, el, prefix, sub, ind + '\t\t')
	out.extend(sub)
	out.append('%s}' % ind)
	out.append('%selse' % ind)
	out.append('%s\tlog_word(0);' % ind)


# The commands
cmds = {}
for c in root.find('commands'):
	if c.get('alias'):
		cmds[c.get('name')] = ('alias', c.get('alias'))
		continue
	if c.get('api') and 'vulkan' not in c.get('api').split(','):
		continue
	proto = c.find('proto')
	cmds[proto.find('name').text] = ('cmd', c)

stub_fns = []
for e in entries:
	c = cmds.get(e)
	if not c:
		continue
	if c[0] == 'alias':
		c = cmds[c[1]]
	c = c[1]
	ret = c.find('proto').find('type').text
	params = []
	for p in c.findall('param'):
		if p.get('api') and 'vulkan' not in p.get('api').split(','):
			continue
		params.append((parse_decl(p), ''.join(p.itertext()).strip()))
	if e in ('vkGetInstanceProcAddr', 'vkGetDeviceProcAddr'):
		continue
	sig = ', '.join(re.sub(r'\s+', ' ', d) for _, d in params)
	body = []
	body.append('\tlog_name("%s");' % e)
	outs = []
	for (t, n, depth, const, arr, ln, alt), _ in params:
		if depth > 0 and not const and not (t == 'void' and depth == 2):
			outs.append((t, n, depth, ln, alt))
			continue
		if depth == 2 and t == 'void':
			outs.append((t, n, depth, ln, alt))
			continue
		if resolve(t) in types and types[resolve(t)][0] == 'union' and depth == 1:
			body.append('\tif (%s) log_bytes(%s, sizeof(*%s)); else log_word(0);' % (n, n, n))
			continue
		ser_value(t, n, depth, const, arr, ln, alt, n, '', body, '\t')
	names = ', '.join(n for (t, n, *_), _ in params)
	if e.startswith('vkDestroy') or e == 'vkFreeMemory':
		kind = 'VkDeviceMemory' if e == 'vkFreeMemory' else 'Vk' + e[len('vkDestroy'):]
		for (t, n, depth, *_), _ in params:
			if resolve(t) == kind and depth == 0:
				body.append('\tkill_handle((u64)(uintptr_t)%s);' % n)
	if e in hooks:
		if ret == 'void':
			body.append('\thook_%s(%s);' % (e, names))
		else:
			body.append('\t{\n\t\t%s r_ = hook_%s(%s);' % (ret, e, names))
	# the handles it makes
	for t, n, depth, ln, alt in outs:
		tt = resolve(t)
		if tt in handles and depth == 1:
			cnt = len_expr(ln, alt, '') if (ln or alt) else '1'
			ptrs = [pn for (pt, pn, pd, *_), _ in params if pd > 0]
			if cnt in ptrs:
				cnt = '*' + cnt
			make = '' if e in hooks else '%s[i_] = (%s)(uintptr_t)new_handle("%s", (u64)(uintptr_t)%s); ' % (n, t, tt, params[0][0][1])
			body.append('\t%s{ u32 i_; for (i_ = 0; i_ < (u32)(%s); i_++) { %slog_handle((u64)(uintptr_t)%s[i_]); } }'
				% ('\t' if (e in hooks and ret != 'void') else '', cnt, make, n))
	if e in hooks and ret != 'void':
		body.append('\t\treturn r_;\n\t}')
	elif ret == 'VkResult':
		body.append('\treturn VK_SUCCESS;')
	elif ret != 'void':
		body.append('\treturn (%s)0;' % ret)
	stub_fns.append((e, ret, sig, body))

# The structs reached (more as their serializers are written), and the
# pNext structs extending them
ext = {}
for name, v in types.items():
	if v[0] == 'struct' and v[1].get('structextends') and in_core(name):
		for base in v[1].get('structextends').split(','):
			ext.setdefault(base, []).append(name)
ser_out = []
protos = []
i = 0
while i < len(need):
	name = need[i]
	i += 1
	for x in ext.get(name, []):
		want(x)
	if types[name][0] != 'struct':
		continue
	protos.append('static void ser_%s(const %s* p);' % (name, name))
	b = []
	if name in overrides:
		b.append('\tser_override_%s(p);' % name)
	else:
		for (t, n, depth, const, arr, ln, alt) in members(name):
			ser_value(t, n, depth, const, arr, ln, alt, 'p->' + n, 'p->', b, '\t')
	ser_out.append('static void ser_%s(const %s* p)\n{\n%s\n}\n' % (name, name, '\n'.join(b)))

stype = {}
for name in need:
	if types[name][0] != 'struct':
		continue
	for m in types[name][1].findall('member'):
		if m.get('values') and m.find('name').text == 'sType':
			stype[name] = m.get('values')

w = sys.stdout.write
w('/* Written by gen_stubs.py from vk.xml: do not edit */\n')
w('#include "stubs.h"\n\n')
w('\n'.join(protos) + '\n\n')
for name in sorted(overrides):
	w('void ser_override_%s(const %s* p);\n' % (name, name))
for e, ret, sig, body in stub_fns:
	if e in hooks:
		w('%s hook_%s(%s);\n' % (ret, e, sig))
w('\n')
w('static void ser_pnext(const void* p)\n{\n\tconst VkBaseInStructure* b = (const VkBaseInStructure*)p;\n')
w('\tif (!p)\n\t{\n\t\tlog_word(0);\n\t\treturn;\n\t}\n\tswitch ((int)b->sType)\n\t{\n')
seen = set()
for name in need:
	if name in stype and stype[name] not in seen and types[name][0] == 'struct' and types[name][1].get('structextends'):
		seen.add(stype[name])
		w('\t\tcase %s: ser_%s((const %s*)p); return;\n' % (stype[name], name, name))
w('\t\tdefault: break;\n\t}\n\tlog_word(b->sType);\n\tser_pnext(b->pNext);\n}\n\n')
w('\n'.join(ser_out) + '\n')
for e, ret, sig, body in stub_fns:
	w('static %s VKAPI_CALL stub_%s(%s)\n{\n%s\n}\n\n' % (ret, e, sig, '\n'.join(body)))
w('const stub_entry g_stub_entries[] = {\n')
for e, ret, sig, body in stub_fns:
	w('\t{"%s", (PFN_vkVoidFunction)stub_%s},\n' % (e, e))
w('\t{NULL, NULL}\n};\n')
