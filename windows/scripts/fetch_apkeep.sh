#!/usr/bin/env bash
# fetch_apkeep.sh -- apkeep (EFF, MIT), the command-line downloader the
# Google services setup's Download runs: it fetches Google Play services and
# the Play Store from APKPure, x86_64 variant, into the setup's own folder, and
# the launcher installs a file only once `--verify-apk` says Google signed it.
#
#   windows/scripts/fetch_apkeep.sh [bin dir]
#
# Built from source, not taken from a release: apkeep 1.1.0 downloads the
# first file APKPure lists for an app, and for "x86_64" APKPure lists x86 and
# x86_64 builds alike (it reads the ABIs as what the device can run), so Play
# services came as an x86-only build Win.apk cannot run. The source is the
# fork's branch for EFForg/apkeep#258 and #259 (pick the variant that really
# holds the requested ABI), pinned by commit, with windows/scripts/
# apkeep-patches/ applied (Play services' version names hold spaces and
# parentheses, which that branch did not read). Its dependencies are the
# ones its Cargo.lock pins (--locked).
#
# Needs cargo (rustup's). On Windows (MSYS2) it builds for
# x86_64-pc-windows-msvc with the Visual Studio tools rustup finds; elsewhere
# it cross-compiles x86_64-pc-windows-gnu with mingw-w64. The result is kept
# in thirdparty/apkeep/ (not in git) under a key of the commit and the
# patches, so a rebuild does not build it again. With a bin dir it is copied
# to <bin>/tools/apkeep.exe and its licence to
# <bin>/licenses/apkeep-LICENSE.txt.

set -euo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
REPO=https://github.com/israel2176/apkeep
COMMIT=d325ad56b52bb3fae48eb45c77a9456fff9ddd36  # fix-apkpure-arch-variant-selection
PATCHES=("$HERE"/apkeep-patches/*.patch)
DIR="$HERE/../../thirdparty/apkeep"
KEY=$( (echo "$COMMIT"; echo opt-z-lto-strip; cat "${PATCHES[@]}") | sha256sum | cut -c1-16)
EXE="$DIR/apkeep-$KEY.exe"

mkdir -p "$DIR"
if [[ ! -f "$EXE" || ! -f "$DIR/LICENSE-$KEY" ]]; then
    echo "[apkeep] building apkeep ${COMMIT:0:12} + ${#PATCHES[@]} patch(es)"
    cargo=$(command -v cargo || true)
    [[ -n "$cargo" ]] || [[ ! -x "$HOME/.cargo/bin/cargo" ]] || cargo="$HOME/.cargo/bin/cargo"
    if [[ -z "$cargo" && -n "${USERPROFILE:-}" ]] && command -v cygpath >/dev/null; then
        c="$(cygpath -u "$USERPROFILE")/.cargo/bin/cargo.exe"
        [[ -x "$c" ]] && cargo="$c"
    fi
    [[ -n "$cargo" ]] || { echo "[apkeep] cargo not found: install Rust (rustup)" >&2; exit 1; }
    SRC="$DIR/src"
    rm -rf "$SRC"
    git init -q "$SRC"
    # The source as committed, LF: Git for Windows' core.autocrlf would check it out with CRLF, and the
    # patches (LF) would not apply.
    git -C "$SRC" config core.autocrlf false
    git -C "$SRC" fetch -q --depth 1 "$REPO" "$COMMIT"
    git -C "$SRC" -c advice.detachedHead=false checkout -q FETCH_HEAD
    for p in "${PATCHES[@]}"; do tr -d '\r' < "$p" | git -C "$SRC" apply -; done
    # Built for size: it only downloads a file now and then, and it ships in every install
    # (windows/README.md, "Footprint").
    export CARGO_PROFILE_RELEASE_OPT_LEVEL=z CARGO_PROFILE_RELEASE_LTO=true \
        CARGO_PROFILE_RELEASE_CODEGEN_UNITS=1 CARGO_PROFILE_RELEASE_STRIP=true
    case "$(uname -s)" in
        MINGW*|MSYS*|CYGWIN*)
            target=x86_64-pc-windows-msvc
            # MSYS2's compiler variables would point the C parts (aws-lc, zstd) at clang with MSVC's target;
            # the cc crate finds Visual Studio's cl by itself.
            "$cargo" --version
            (cd "$SRC" && env -u CC -u CXX -u AR -u CFLAGS -u CXXFLAGS -u LDFLAGS \
                "$cargo" build -q --release --locked --target "$target")
            ;;
        *)
            target=x86_64-pc-windows-gnu
            rustup target add "$target" >/dev/null 2>&1 || true
            (cd "$SRC" && CC_x86_64_pc_windows_gnu=x86_64-w64-mingw32-gcc-posix \
                CXX_x86_64_pc_windows_gnu=x86_64-w64-mingw32-g++-posix \
                AR_x86_64_pc_windows_gnu=x86_64-w64-mingw32-ar \
                CARGO_TARGET_X86_64_PC_WINDOWS_GNU_LINKER=x86_64-w64-mingw32-gcc-posix \
                "$cargo" build -q --release --locked --target "$target")
            ;;
    esac
    cp -f "$SRC/target/$target/release/apkeep.exe" "$EXE.part"
    cp -f "$SRC/LICENSE" "$DIR/LICENSE-$KEY"
    mv "$EXE.part" "$EXE"
    rm -rf "$SRC"
fi
if [[ -n "${1:-}" ]]; then
    mkdir -p "$1/tools" "$1/licenses"
    cp -f "$EXE" "$1/tools/apkeep.exe"
    cp -f "$DIR/LICENSE-$KEY" "$1/licenses/apkeep-LICENSE.txt"
    echo "[apkeep] apkeep.exe (${COMMIT:0:12} + patches, $KEY) in $1/tools"
fi
