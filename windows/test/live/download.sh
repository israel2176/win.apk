#!/usr/bin/env bash
# download.sh <apkeep> <out dir> -- the live test's APKs, fetched on the
# Windows runner that installs them (Git Bash). No Google account and no
# credentials: APKPure without a login through apkeep (the x86_64 build), and
# the repository secret APK_URLS (one direct link per line) when the user set
# one. Every link is masked before anything else and never printed. Nothing is
# uploaded or committed: the files stay on the runner. live.py checks each
# file with winapk-runtime --verify-apk and installs a Google app only when
# Google signed it.
#
# PACKAGES: space-separated packages (the workflow's input); GOOGLE_APPS:
# Google's Services Framework, Play services and Play Store, tried first.
set -e
APKEEP="$1"
OUT="$2"
# Before anything can print one: each link of the secret is masked.
if [ -n "$APK_URLS" ]; then
  while IFS= read -r u; do
    u="${u%$'\r'}"
    [ -n "$u" ] && echo "::add-mask::$u"
  done <<< "$APK_URLS"
fi
[[ "$PACKAGES $GOOGLE_APPS" =~ ^[A-Za-z0-9_.\ ]*$ ]] || { echo "::error::bad package list"; exit 1; }
here="$(cd "$(dirname "$0")" && pwd)"
PY=python3; command -v python3 > /dev/null 2>&1 && python3 -c 1 2> /dev/null || PY=python
native() { if command -v cygpath > /dev/null 2>&1; then cygpath -w "$1"; else echo "$1"; fi; }
mkdir -p "$OUT" dl
: > "$OUT/sources.txt"

for pkg in $GOOGLE_APPS $PACKAGES; do
  out="dl/$pkg/apkpure"
  [ -d "$out" ] && continue
  mkdir -p "$out"
  echo "::group::$pkg from APKPure (x86_64)"
  "$APKEEP" -a "$pkg" -d apk-pure -o "acknowledge_dangers=true,arch=x86_64" "$(native "$PWD/$out")" 2>&1 \
    | tr '\r' '\n' | grep -v '^\s*$' | tail -n 8 || true
  echo "::endgroup::"
  if [ -n "$(find "$out" -name '*.apk' -o -name '*.xapk' | head -1)" ]; then
    find "$out" -type f -printf "$pkg: apkpure %s bytes\t%P\n" >> "$OUT/sources.txt"
    "$PY" -I "$here/pack.py" "$pkg" "$out" "$OUT/apkpure"
  else
    echo "$pkg: NOT DOWNLOADED (apkpure)" >> "$OUT/sources.txt"
  fi
done

# The fallback: the user's own links. Saved as url-<n>.<ext>; live.py names
# each by the package --verify-apk reads in it. A failure says which line,
# never the link.
if [ -n "$APK_URLS" ]; then
  mkdir -p "$OUT/urls"
  n=0
  while IFS= read -r u; do
    u="${u%$'\r'}"
    [ -n "$u" ] || continue
    n=$((n + 1))
    path="${u%%[?#]*}"
    ext="${path##*.}"
    ext="$(echo "$ext" | tr 'A-Z' 'a-z')"
    case "$ext" in apk|apks|xapk|apkm) ;; *) ext=apk ;; esac
    if curl -fsSL --retry 2 -o "$OUT/urls/url-$n.$ext" "$u" > /dev/null 2>&1; then
      echo "APK_URLS line $n: $(wc -c < "$OUT/urls/url-$n.$ext") bytes (.$ext)" >> "$OUT/sources.txt"
    else
      rm -f "$OUT/urls/url-$n.$ext"
      echo "APK_URLS line $n: download FAILED" >> "$OUT/sources.txt"
    fi
  done <<< "$APK_URLS"
fi
cat "$OUT/sources.txt"
