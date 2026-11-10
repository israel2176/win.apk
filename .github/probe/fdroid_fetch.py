"""Fetch one F-Droid app for the app probe: fdroid_fetch.py <package> <versionCode|latest> <out.apk>.

"latest" asks F-Droid for the package's versions and takes the newest one with
x86_64 native code or none (an app with one build per CPU has several
version codes), else the one F-Droid suggests."""
import json
import sys
import urllib.request
import zipfile


def get(url, path):
    req = urllib.request.Request(url, headers={"User-Agent": "app-probe"})
    with urllib.request.urlopen(req, timeout=300) as r, open(path, "wb") as f:
        while True:
            block = r.read(1 << 20)
            if not block:
                break
            f.write(block)


def fetch(pkg, code, out):
    for base in ("repo", "archive"):
        try:
            get(f"https://f-droid.org/{base}/{pkg}_{code}.apk", out)
            return True
        except Exception as e:  # the other folder, or the next version
            print(f"  {base}/{pkg}_{code}.apk: {e}")
    return False


def abis(path):
    names = zipfile.ZipFile(path).namelist()
    return sorted({n.split("/")[1] for n in names if n.startswith("lib/") and n.count("/") >= 2})


pkg, version, out = sys.argv[1:4]
codes = [version]
if version == "latest":
    req = urllib.request.Request(f"https://f-droid.org/api/v1/packages/{pkg}", headers={"User-Agent": "app-probe"})
    info = json.load(urllib.request.urlopen(req, timeout=60))
    codes = [str(p["versionCode"]) for p in info["packages"][:6]]
    codes.append(str(info["suggestedVersionCode"]))
for i, code in enumerate(codes):
    if not fetch(pkg, code, out):
        continue
    a = abis(out)
    print(f"{pkg} {code}: native code {a or 'none'}")
    if not a or "x86_64" in a or i == len(codes) - 1:
        print(f"VERSION={code}")
        sys.exit(0)
sys.exit(1)
