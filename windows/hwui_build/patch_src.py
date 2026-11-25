#!/usr/bin/env python3
"""The source changes the AOSP 14 HWUI closure needs to build for Windows with clang on MinGW-w64
(libstdc++, not Soong's libc++). Each is a literal replacement, applied once (idempotent)."""
import os
import sys

SRC = os.path.abspath(sys.argv[1])

EDITS = [
    # libstdc++'s allocator_traits rebinds through __replace_first_arg, which a non-type template
    # parameter defeats; libc++ falls back on the allocator itself. Spell the rebind out.
    ('native_libs/ui/include/ui/FatVector.h',
     '    typedef T value_type; // needed to implement std::allocator\n',
     '    typedef T value_type; // needed to implement std::allocator\n'
     '    template <class U>\n    struct rebind {\n        typedef InlineStdAllocator<U, SIZE> other;\n    };\n'),
]

bad = 0
for rel, old, new in EDITS:
    p = os.path.join(SRC, rel)
    s = open(p, encoding='utf-8').read()
    if new in s:
        continue
    if s.count(old) != 1:
        print('patch_src: %s: the text to replace is not there once' % rel)
        bad += 1
        continue
    open(p, 'w', encoding='utf-8').write(s.replace(old, new))
sys.exit(1 if bad else 0)
