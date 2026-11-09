"""pack.py <package> <download dir> <out dir>: the package's download as one
file Win.apk installs: a single APK as <package>.apk, an APKPure .xapk as is,
a split set as <package>.apks (base.apk and the splits at the root, the layout
Win.apk's AppInstall reads like an .apkm)."""
import os
import shutil
import sys
import zipfile

pkg, src, out = sys.argv[1:4]
os.makedirs(out, exist_ok=True)
files = []
for root, _, names in os.walk(src):
    for n in names:
        files.append(os.path.join(root, n))
xapk = [f for f in files if f.endswith(".xapk")]
apks = [f for f in files if f.endswith(".apk")]
if xapk:
    shutil.copy(xapk[0], os.path.join(out, pkg + ".xapk"))
elif len(apks) == 1:
    shutil.copy(apks[0], os.path.join(out, pkg + ".apk"))
elif apks:
    def has_dex(f):
        try:
            return "classes.dex" in zipfile.ZipFile(f).namelist()
        except Exception:
            return False
    named = [f for f in apks if os.path.basename(f) in (pkg + ".apk", "base.apk")]
    base = named[0] if named else max((f for f in apks if has_dex(f)), key=os.path.getsize)
    with zipfile.ZipFile(os.path.join(out, pkg + ".apks"), "w", zipfile.ZIP_STORED) as z:
        z.write(base, "base.apk")
        for f in apks:
            if f != base:
                z.write(f, os.path.basename(f))
    print(f"{pkg}: base {os.path.basename(base)} + {len(apks) - 1} splits -> {pkg}.apks")
