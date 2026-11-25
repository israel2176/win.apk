#!/usr/bin/env bash
# fetch.sh: AOSP 14 (android-14.0.0_r1) sources of HWUI and its closure, as gitiles archives.
set -u
S=${1:?work dir}/src
TAG=android-14.0.0_r1
get() {  # get <project> <dest> [subdir]
  local url="https://android.googlesource.com/$1/+archive/refs/tags/$TAG${3:+/$3}.tar.gz"
  [ -d "$S/$2" ] && return 0
  mkdir -p "$S/$2.tmp"
  if curl -sS --retry 3 -o "$S/$2.tgz" "$url" && tar -xzf "$S/$2.tgz" -C "$S/$2.tmp"; then
    mv "$S/$2.tmp" "$S/$2"; rm -f "$S/$2.tgz"; echo "ok $2"
  else
    echo "FAIL $2 ($url)"; rm -rf "$S/$2.tmp"
  fi
}
get platform/frameworks/base hwui libs/hwui &
get platform/frameworks/base core_jni core/jni &
get platform/frameworks/minikin minikin &
get platform/external/harfbuzz_ng harfbuzz_ng &
wait
get platform/external/freetype freetype &
get platform/external/icu icu4c icu4c &
get platform/external/libpng libpng &
get platform/external/libjpeg-turbo libjpeg-turbo &
wait
get platform/external/webp libwebp &
get platform/external/giflib giflib &
get platform/external/expat expat &
get platform/external/zlib zlib &
wait
get platform/system/core system_core &
get platform/system/libbase libbase &
get platform/system/logging logging &
get platform/external/fmtlib fmtlib &
wait
get platform/libnativehelper libnativehelper &
get platform/frameworks/native native_libs libs &
get platform/frameworks/native native_include include &
wait
