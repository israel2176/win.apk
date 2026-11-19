#!/usr/bin/env python3
"""What the screens sweep needs from an APK's manifest, read with aapt2.

    manifest_info.py <aapt2> <apk> <manifest out>

Prints shell assignments (PACKAGE, THEME, THEME_WHY, MIN_SDK, TARGET_SDK, RTL)
and writes a minimal text AndroidManifest.xml for Robolectric: the package, its
SDK levels and android:supportsRtl (without which Android lays nothing out right
to left), and no application class, so the oracle runs none of the app's code
but its views'.

THEME is the theme most of the app's activities wear (an activity without one
wears the application's): the launcher activity is often a splash screen whose
theme no other screen has.
"""
import collections
import re
import subprocess
import sys

ANDROID = 'http://schemas.android.com/apk/res/android:'


class Node:
    def __init__(self, name, depth):
        self.name, self.depth, self.attrs, self.children = name, depth, {}, []


def parse(text):
    root = Node('', -1)
    stack = [root]
    for line in text.splitlines():
        m = re.match(r'^(\s*)([EA]): (.*)$', line)
        if not m:
            continue
        depth, kind, rest = len(m.group(1)), m.group(2), m.group(3)
        if kind == 'E':
            while stack[-1].depth >= depth:
                stack.pop()
            node = Node(rest.split(' ')[0], depth)
            stack[-1].children.append(node)
            stack.append(node)
        else:
            a = re.match(r'^(\S+?)(?:\(0x[0-9a-f]+\))?=(.*)$', rest)
            if not a:
                continue
            key = a.group(1).replace(ANDROID, 'android:')
            value = a.group(2)
            q = re.match(r'^"(.*?)"', value)
            if q:
                value = q.group(1)
            stack[-1].attrs[key] = value
    return root


def find(node, name):
    if node.name == name:
        yield node
    for c in node.children:
        yield from find(c, name)


def is_launcher(node):
    for f in node.children:
        if f.name != 'intent-filter':
            continue
        actions = {c.attrs.get('android:name') for c in f.children if c.name == 'action'}
        cats = {c.attrs.get('android:name') for c in f.children if c.name == 'category'}
        if 'android.intent.action.MAIN' in actions and 'android.intent.category.LAUNCHER' in cats:
            return True
    return False


def res(value):
    """@0x7f12000d -> 0x7f12000d."""
    return value[1:] if value and value.startswith('@0x') else None


def main():
    aapt2, apk, manifest_out = sys.argv[1:4]
    text = subprocess.run([aapt2, 'dump', 'xmltree', '--file', 'AndroidManifest.xml', apk],
                          check=True, capture_output=True, text=True).stdout
    tree = parse(text)
    manifest = next(find(tree, 'manifest'))
    package = manifest.attrs['package']
    sdk = next(find(tree, 'uses-sdk'), None)
    min_sdk = sdk.attrs.get('android:minSdkVersion', '21') if sdk else '21'
    target_sdk = sdk.attrs.get('android:targetSdkVersion', min_sdk) if sdk else min_sdk
    app = next(find(tree, 'application'), None)
    app_theme = res(app.attrs.get('android:theme')) if app else None
    rtl = bool(app) and app.attrs.get('android:supportsRtl', 'false').lower() in ('true', '0xffffffff', '-1')

    activities = {a.attrs.get('android:name'): a for a in find(tree, 'activity')}
    worn = collections.Counter()
    for a in activities.values():
        t = res(a.attrs.get('android:theme')) or app_theme
        if t:
            worn[t] += 1
    launcher = None
    for kind in ('activity', 'activity-alias'):
        for a in find(tree, kind):
            if is_launcher(a) and (launcher is None or a.attrs.get('android:enabled') != 'false'):
                launcher = activities.get(a.attrs.get('android:targetActivity'), a) if kind != 'activity' else a
    theme, why = None, ''
    if worn:
        theme, n = worn.most_common(1)[0]
        why = f'worn by {n} of {len(activities)} activities'
    elif launcher is not None and res(launcher.attrs.get('android:theme')):
        theme, why = res(launcher.attrs['android:theme']), 'the launcher activity\'s'
    elif app_theme:
        theme, why = app_theme, 'the application\'s'

    with open(manifest_out, 'w', encoding='utf-8') as f:
        f.write('<?xml version="1.0" encoding="utf-8"?>\n'
                '<manifest xmlns:android="http://schemas.android.com/apk/res/android"\n'
                f'    package="{package}">\n'
                f'    <uses-sdk android:minSdkVersion="{min_sdk}" android:targetSdkVersion="{target_sdk}"/>\n'
                f'    <application android:supportsRtl="{"true" if rtl else "false"}"/>\n'
                '</manifest>\n')
    print(f'PACKAGE={package}')
    print(f'THEME={theme or ""}')
    print(f"THEME_WHY='{why}'")
    print(f'MIN_SDK={min_sdk}')
    print(f'TARGET_SDK={target_sdk}')
    print(f'RTL={1 if rtl else 0}')


if __name__ == '__main__':
    main()
