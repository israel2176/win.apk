#!/usr/bin/env python3
"""A small Android.bp (Blueprint) reader and resolver: the modules of the given files, with their
defaults applied and the target/arch sections chosen for one variant (here Windows x86_64 host,
optionally with a module's android sections too), as Soong would flatten them."""
import os
import re
import sys

TOKEN = re.compile(r'\s+|//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|`[^`]*`|[A-Za-z_][A-Za-z0-9_\-.]*|-?\d+|[{}\[\]:,=+()]|\+=',
                   re.S)


def tokens(text):
    pos = 0
    out = []
    while pos < len(text):
        if text.startswith('+=', pos):
            out.append('+=')
            pos += 2
            continue
        m = TOKEN.match(text, pos)
        if not m:
            raise SyntaxError('bad char %r at %d' % (text[pos], pos))
        t = m.group(0)
        pos = m.end()
        if t.strip() and not t.startswith('//') and not t.startswith('/*'):
            out.append(t)
    return out


class Parser:
    def __init__(self, text, variables):
        self.t = tokens(text)
        self.i = 0
        self.vars = variables

    def peek(self):
        return self.t[self.i] if self.i < len(self.t) else None

    def next(self):
        tok = self.t[self.i]
        self.i += 1
        return tok

    def expect(self, tok):
        got = self.next()
        if got != tok:
            raise SyntaxError('expected %s got %s near %s' % (tok, got, self.t[self.i - 5:self.i + 5]))

    def value(self):
        v = self.atom()
        while self.peek() == '+':
            self.next()
            v = add(v, self.atom())
        return v

    def atom(self):
        tok = self.next()
        if tok.startswith('"'):
            return bytes(tok[1:-1], 'utf-8').decode('unicode_escape')
        if tok.startswith('`'):
            return tok[1:-1]
        if tok == '[':
            items = []
            while self.peek() != ']':
                items.append(self.value())
                if self.peek() == ',':
                    self.next()
            self.next()
            return items
        if tok == '{':
            return self.props('}')
        if tok in ('true', 'false'):
            return tok == 'true'
        if re.match(r'-?\d+$', tok):
            return int(tok)
        if tok == 'select':
            raise SyntaxError('select() unsupported')
        return self.vars.get(tok, [])

    def props(self, end):
        d = {}
        while self.peek() != end:
            k = self.next()
            if self.peek() in (':', '='):
                self.next()
            d[k] = self.value()
            if self.peek() == ',':
                self.next()
        self.next()
        return d

    def file(self):
        mods = []
        while self.peek() is not None:
            tok = self.next()
            if self.peek() == '=':
                self.next()
                self.vars[tok] = self.value()
            elif self.peek() == '+=':
                self.next()
                self.vars[tok] = add(self.vars.get(tok, []), self.value())
            elif self.peek() == '{':
                self.next()
                p = self.props('}')
                p['__type'] = tok
                mods.append(p)
            else:
                raise SyntaxError('unexpected %s' % tok)
        return mods


def add(a, b):
    if isinstance(a, list) and isinstance(b, list):
        return a + b
    if isinstance(a, str) and isinstance(b, str):
        return a + b
    if isinstance(a, dict) and isinstance(b, dict):
        return merge(a, b)
    if a in (None, []):
        return b
    return b


def merge(a, b):
    """Soong's property merge: lists append, maps merge recursively, scalars: b wins."""
    out = dict(a)
    for k, v in b.items():
        if k in out:
            if isinstance(out[k], list) and isinstance(v, list):
                out[k] = out[k] + v
            elif isinstance(out[k], dict) and isinstance(v, dict):
                out[k] = merge(out[k], v)
            else:
                out[k] = v
        else:
            out[k] = v
    return out


class Tree:
    """Every module of the given Android.bp files, by name, with the directory it is in."""

    def __init__(self):
        self.mods = {}
        self.defaults = {}

    def load(self, path, root=None):
        text = open(path, encoding='utf-8').read()
        d = os.path.dirname(os.path.abspath(path))
        for m in Parser(text, {}).file():
            m['__dir'] = d
            name = m.get('name')
            if not name:
                continue
            if m['__type'].endswith('_defaults') or m['__type'] == 'cc_defaults':
                self.defaults[name] = m
            self.mods.setdefault(name, m)

    def flat(self, name, targets=('windows', 'host', 'not_windows_never'), arch='x86_64'):
        """The module's properties for one variant: defaults first (each in its own directory),
        then target/arch sections, srcs and include dirs made absolute."""
        m = self.mods[name]
        return self._flat(m, targets, arch)

    def _flat(self, m, targets, arch):
        acc = {}
        for dn in m.get('defaults', []):
            if dn in self.defaults:
                acc = merge(acc, self._flat(self.defaults[dn], targets, arch))
        own = absolutize(dict((k, v) for k, v in m.items() if k not in ('defaults',)), m['__dir'])
        tgt = own.pop('target', {})
        ar = own.pop('arch', {})
        mlib = own.pop('multilib', {})
        for t in targets:
            if t in tgt:
                own = merge(own, absolutize(tgt[t], m['__dir']))
        if arch in ar:
            own = merge(own, absolutize(ar[arch], m['__dir']))
        if 'lib64' in mlib:
            own = merge(own, absolutize(mlib['lib64'], m['__dir']))
        return merge(acc, own)


PATH_PROPS = ('srcs', 'exclude_srcs', 'local_include_dirs', 'export_include_dirs', 'include_dirs_local')


def absolutize(p, d):
    p = dict(p)
    for k in PATH_PROPS:
        if k in p and isinstance(p[k], list):
            p[k] = [x if x.startswith(':') else os.path.normpath(os.path.join(d, x)) for x in p[k]]
    for k in ('target', 'arch', 'multilib'):
        pass
    return p


if __name__ == '__main__':
    t = Tree()
    for f in sys.argv[2:]:
        t.load(f)
    import json
    print(json.dumps(t.flat(sys.argv[1]), indent=1, default=str))
