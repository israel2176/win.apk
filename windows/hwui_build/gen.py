#!/usr/bin/env python3
"""Writes modules.cmake: one static library per Android.bp module wanted, for Windows x86_64 host
as Soong builds it (target windows + host sections), with the include directories its own and its
dependencies' export_*include_dirs give. A module listed in ANDROID_TOO also takes its android
target sections (HWUI's and Skia's GPU parts), minus the sources DROP matches."""
import glob
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bp  # noqa: E402

SRC = os.path.abspath(sys.argv[1])
OUT = sys.argv[2]
WANT = sys.argv[3].split(',')
ANDROID_TOO = set(sys.argv[4].split(',')) if len(sys.argv) > 4 and sys.argv[4] else set()
DROP = re.compile(sys.argv[5]) if len(sys.argv) > 5 and sys.argv[5] else None

tree = bp.Tree()
for root, dirs, files in os.walk(SRC):
    if 'Android.bp' in files:
        tree.load(os.path.join(root, 'Android.bp'))


def flat(name):
    targets = ('host', 'windows', 'not_darwin', 'not_linux_bionic')
    if name in ANDROID_TOO:
        targets = ('host', 'windows', 'android')
    return tree.flat(name, targets=targets)


EXPORTS = ('export_include_dirs', 'export_system_include_dirs')
DEP_LISTS = ('static_libs', 'whole_static_libs', 'shared_libs', 'header_libs')
REEXPORT = ('export_static_lib_headers', 'export_shared_lib_headers', 'export_header_lib_headers')


def exported(name, seen=None):
    """A module's exported include dirs, with those of the libraries it re-exports."""
    seen = seen or set()
    if name in seen or name not in tree.mods:
        return []
    seen.add(name)
    f = flat(name)
    dirs = []
    for k in EXPORTS:
        dirs += f.get(k, [])
    for k in REEXPORT:
        for d in f.get(k, []):
            dirs += exported(d, seen)
    if f.get('__type') in ('cc_library_headers',):
        for k in ('header_libs', 'static_libs', 'shared_libs'):
            for d in f.get(k, []):
                dirs += exported(d, seen)
    return dirs


# Where an AOSP tree path lives in the fetched sources.
TREE_MAP = [('external/skia', 'skia14'), ('frameworks/base/libs/hwui', 'hwui'), ('frameworks/base/core/jni', 'core_jni'),
            ('frameworks/base/libs/androidfw', 'androidfw'), ('frameworks/native/libs', 'native_libs'),
            ('frameworks/native/include', 'native_include'), ('external/', ''), ('system/core', 'system_core'),
            ('frameworks/minikin', 'minikin'), ('system/incremental_delivery', 'incremental_delivery')]


def aosp_path(d):
    for a, b in TREE_MAP:
        if d.startswith(a):
            return os.path.normpath(os.path.join(SRC, b + d[len(a):]))
    return os.path.normpath(os.path.join(SRC, d))


def expand(srcs, excl):
    out = []
    for s in srcs:
        if s.startswith(':'):
            continue
        if any(c in s for c in '*?'):
            out += sorted(glob.glob(s, recursive=True))
        else:
            out.append(s)
    ex = set()
    for e in excl:
        ex |= set(glob.glob(e, recursive=True)) if any(c in e for c in '*?') else {e}
    return [s for s in out if s not in ex and s.endswith(('.c', '.cc', '.cpp', '.S', '.cxx'))]


lines = []
missing = set()
i = 0
while i < len(WANT):
    n = WANT[i]
    i += 1
    if n in tree.mods:
        for w in flat(n).get('whole_static_libs', []):
            if w not in WANT:
                WANT.append(w)
for name in WANT:
    if name not in tree.mods:
        missing.add(name)
        continue
    f = flat(name)
    srcs = expand(f.get('srcs', []), f.get('exclude_srcs', []))
    if DROP:
        srcs = [s for s in srcs if not DROP.search(s)]
    if not srcs:
        continue
    inc = list(f.get('local_include_dirs', [])) + list(f.get('export_include_dirs', [])) + \
        list(f.get('export_system_include_dirs', []))
    inc += [aosp_path(d) for d in f.get('include_dirs', [])]
    for k in DEP_LISTS:
        for d in f.get(k, []):
            if d not in tree.mods:
                missing.add(d)
            inc += exported(d)
    inc.append(f['__dir'])
    seen = set()
    inc = [d for d in inc if not (d in seen or seen.add(d))]
    cflags = [c for c in f.get('cflags', []) if c not in ('-Werror',) and not c.startswith('-Werror')]
    lib = 'aosp_' + re.sub(r'[^A-Za-z0-9_]', '_', name)
    lines.append('# %s (%s)' % (name, os.path.relpath(f['__dir'], SRC)))
    lines.append('add_library(%s STATIC' % lib)
    lines += ['  "%s"' % s for s in srcs]
    lines.append(')')
    lines.append('target_include_directories(%s PRIVATE' % lib)
    lines += ['  "%s"' % d for d in inc]
    lines.append(')')
    q = lambda xs: ' '.join('"%s%s"' % ('SHELL:' if ' ' in x.strip() else '', x.strip().replace('"', '\\"')) for x in xs)
    if cflags:
        lines.append('target_compile_options(%s PRIVATE %s)' % (lib, q(cflags)))
    if f.get('cppflags'):
        lines.append('target_compile_options(%s PRIVATE $<$<COMPILE_LANGUAGE:CXX>:%s>)' %
                     (lib, ';'.join(c.replace('"', '\\"') for c in f['cppflags'])))
    if f.get('conlyflags'):
        lines.append('target_compile_options(%s PRIVATE $<$<COMPILE_LANGUAGE:C>:%s>)' %
                     (lib, ';'.join(c.replace('"', '\\"') for c in f['conlyflags'])))
    lines.append('set(%s_DEPS %s)' % (lib, ' '.join(f.get('static_libs', []) + f.get('whole_static_libs', []) +
                                                     f.get('shared_libs', []))))
    lines.append('')
open(OUT, 'w').write('\n'.join(lines))
print('modules:', len(WANT), 'missing deps:', ' '.join(sorted(missing)), file=sys.stderr)
