#!/usr/bin/env bash
# run_aot_worker_wine.sh -- the background arm64 translation worker
# (host/runtime_main.cpp, "arm64 translation, in the background") under Wine,
# with a stand-in for clang (fakeclang.cpp) that takes FAKECLANG_MS per part
# and logs when each one runs. Checks, on generated arm64 libraries:
#
#   1. a whole translation: parts of kAotPartInsns, the stamp, the shared
#      cache (one file with the app's DLL), the work folder gone;
#   2. the worker killed half way: none of its compilers outlives it (the job
#      object), and the next run compiles only the parts not done;
#   3. a part that always fails: waits of 10, 40, 160 minutes, then no more
#      tries; each retry compiles only that part;
#   4. a library over kAotFullBelow: nothing until it has a profile, then only
#      what ran, and again only once the profile has grown by a quarter;
#   5. four apps' workers on the same library at once, one shared cache, with
#      hard links and with copies: one good entry, every app's DLL sealed;
#   6. a worker killed at each step of a write (WINAPK_AOT_FAULT: half a seal,
#      a cache entry under its temporary name, before and after the rename
#      into place, half a stamp): the next run finishes, nothing half made
#      is taken for a translation;
#   7. a DLL cut short under a "full" stamp (a power loss): translated again,
#      and the cache entry it shares is not handed on;
#   8. a cache entry cut short: not taken, deleted, compiled again;
#   9. a library dated in the future (extracted while the clock was ahead):
#      translated once, not at every pass;
#  10. temporary files left by killed writers: deleted once an hour old.
#
# Scenarios 5-10 are aot_files.h's rules in the real worker; test/
# aot_cache_test.cpp checks the same rules on Windows in CI.
#
#   windows/test/aot_worker/run_aot_worker_wine.sh
#
# Needs wine64, python3, clang with the aarch64 target, lld and llvm-strip
# (the guest libraries), and a mingw-w64 C++ compiler (CXX, default
# x86_64-w64-mingw32-g++-posix).

set -euo pipefail
HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
WIN_DIR="$(cd -- "$HERE/../.." >/dev/null 2>&1 && pwd)"
OUT="${WINAPK_BUILD:-$WIN_DIR/build}/aot-worker-test"
WINE="${WINE:-$(command -v wine64 || command -v wine || echo /usr/lib/wine/wine64)}"
CXX="${CXX:-x86_64-w64-mingw32-g++-posix}"
export WINEDEBUG=-all
rm -rf "$OUT" && mkdir -p "$OUT"
cd "$OUT"

python3 -I "$HERE/gen_worker_test.py" "$WIN_DIR/host/runtime_main.cpp" worker.cpp
FLAGS=(-std=c++17 -O1 -municode -static -DUNICODE -D_UNICODE -I"$WIN_DIR/include")
"$CXX" "${FLAGS[@]}" -o winapk-runtime.exe worker.cpp "$WIN_DIR/src/arm64/aot_files.cpp" -lshell32
"$CXX" "${FLAGS[@]}" -o fakeclang.exe "$HERE/fakeclang.cpp"
"$CXX" -std=c++17 -O2 -static -I"$WIN_DIR/include" -o winapk-aot.exe "$WIN_DIR/tools/arm64_aot_main.cpp"

# Guest libraries, stripped as release builds are (their functions are found
# through .eh_frame_hdr): libguest.so, about 100,000 instructions (several
# parts, translated whole), and libbig.so, over kAotFullBelow (translated
# where its profile says code ran).
guest() {  # guest <functions> <name>
    python3 -I "$HERE/gen_guest.py" "$1" > "$2.c"
    clang --target=aarch64-linux-android29 -O0 -fPIC -funwind-tables -shared -nostdlib -fuse-ld=lld \
        -Wl,--eh-frame-hdr -o "$2.so" "$2.c"
    llvm-strip --strip-all "$2.so"
}
guest 400 libguest
guest 3200 libbig

failures=0
check() {  # check <what> <command...>
    local what="$1"
    shift
    if "$@"; then echo "ok   $what"; else echo "FAIL $what"; failures=$((failures + 1)); fi
}
win() { echo "Z:$1" | tr / '\\'; }
setup() {  # setup <test dir> <library>: a library folder with that library
    rm -rf "$1" && mkdir -p "$1/lib/arm64" "$1/cache" && cp "$2" "$1/lib/arm64/"
    : > "$1/clang.log"
    FAKECLANG_LOG="$(win "$PWD/$1/clang.log")"
    export FAKECLANG_LOG
}
worker() { "$WINE" ./winapk-runtime.exe --aot-translate "$(win "$PWD/$1/lib/arm64")" --cache "$(win "$PWD/$1/cache")"; }
needs_work() { "$WINE" ./winapk-runtime.exe "$(win "$PWD/$1/lib/arm64/$2")" | tr -d '\r'; }
stamp() { cat "$1/lib/arm64/$2.aot.stamp"; }
check_dll() { "$WINE" ./winapk-runtime.exe --check "$(win "$PWD/$1")" | tr -d '\r'; }
compiles() { grep -c '^start' "$1/clang.log" || true; }
count() { ps -eo comm | grep -c "^$1" || true; }
WINAPK_AOT_CLANG="$(win "$PWD/fakeclang.exe")"
export WINAPK_AOT_CLANG WINAPK_AOT_JOBS=3

echo "== 1. a whole translation"
setup t1 libguest.so
FAKECLANG_MS=200 worker t1 > t1/log 2>&1
parts=$(compiles t1)
check "several parts ($parts compiles)" test "$parts" -ge 4
most=$(sort -k3,3n -k1,1r t1/clang.log | awk '$1 == "start" {n++} $1 == "end" || $1 == "fail" {n--} n > m {m = n} END {print m + 0}')
check "at most WINAPK_AOT_JOBS at once ($most)" test "$most" -le 3
check "stamp full ($(stamp t1 libguest.so))" grep -q '^full 0 ' t1/lib/arm64/libguest.so.aot.stamp
check "the DLL in place" test -f t1/lib/arm64/libguest.so.aot.dll
check "the shared cache has it" test "$(ls t1/cache | wc -l)" -eq 1
check "as the same file as the app's DLL (a hard link)" test "$(stat -c %i t1/cache/*.aot.dll)" = \
    "$(stat -c %i t1/lib/arm64/libguest.so.aot.dll)"
check "the work folder is gone" test -z "$(ls -A t1/lib/arm64/.aot-tmp)"
check "nothing left to do" test "$(needs_work t1 libguest.so)" = 0

echo "== 2. killed half way"
setup t2 libguest.so
FAKECLANG_MS=3000 worker t2 > t2/log 2>&1 &
for _ in $(seq 120); do  # until some parts are done and others are compiling
    [ "$(grep -c '^end' t2/clang.log || true)" -ge 3 ] && [ "$(count fakeclang)" -ge 1 ] && break
    sleep 0.5
done
running=$(count fakeclang)
for p in $(ps -eo pid,comm | awk '$2 ~ /^winapk-runtime/ {print $1}'); do kill -9 "$p"; done
wait || true
sleep 3
check "compilers were running ($running)" test "$running" -ge 1
check "no compiler outlives the worker ($(count fakeclang) left)" test "$(count fakeclang)" -eq 0
check "no .tmp left behind" test -z "$(find t2/lib/arm64/.aot-tmp -name '*.tmp')"
check "work is left" test "$(needs_work t2 libguest.so)" = 1
: > t2/clang.log
FAKECLANG_MS=200 worker t2 > t2/log2 2>&1
check "the next run goes on ($(grep -o '[0-9]* of [0-9]* parts were compiled before' t2/log2 || true))" \
    grep -q 'parts were compiled before' t2/log2
check "and compiles fewer than all ($(compiles t2) of $parts)" test "$(compiles t2)" -lt "$parts"
check "stamp full" grep -q '^full 0 ' t2/lib/arm64/libguest.so.aot.stamp

echo "== 3. a part that always fails"
setup t3 libguest.so
for round in 1 2 3 4; do
    : > t3/clang.log
    FAKECLANG_MS=50 FAKECLANG_FAIL=".c.2.c" worker t3 > t3/log 2>&1 || true
    s=$(stamp t3 libguest.so)
    check "round $round: stamp wait, $round failure(s) ($s)" test "$(echo "$s" | awk '{print $1, $4}')" = "wait $round"
    if [ "$round" -lt 4 ]; then
        wait_s=$(($(echo "$s" | awk '{print $5}') - $(date +%s)))
        want=$((600 << (2 * (round - 1))))
        check "round $round: waits $((want / 60)) minutes" test "$wait_s" -gt $((want - 120)) -a "$wait_s" -le "$want"
        check "round $round: not tried before that" test "$(needs_work t3 libguest.so)" = 0
        if [ "$round" -gt 1 ]; then
            check "round $round: only the failing part again ($(compiles t3))" test "$(compiles t3)" -eq 1
        fi
        echo "$s" | awk '{$5 = 0; print}' > t3/lib/arm64/libguest.so.aot.stamp  # as if the wait were over
    fi
done
check "after 4 failures: no more tries" test "$(needs_work t3 libguest.so)" = 0
check "and no work kept" test -z "$(ls -A t3/lib/arm64/.aot-tmp)"

echo "== 4. a library over kAotFullBelow"
setup t4 libbig.so
SO="$(win "$PWD/t4/lib/arm64/libbig.so")"
P=t4/lib/arm64/libbig.so.aot.profile
hash=$("$WINE" ./winapk-aot.exe --hash "$SO" | tr -d '\r')
"$WINE" ./winapk-aot.exe "$SO" "$(win "$PWD/all.c")" 2> /dev/null || true
grep -o '{0x[0-9a-f]*ull' all.c | sed 's/{0x//; s/ull//' > entries.txt  # every function the translator finds
check "the stripped library's functions are found ($(wc -l < entries.txt))" test "$(wc -l < entries.txt)" -ge 3200
FAKECLANG_MS=50 worker t4 > t4/log0 2>&1
check "no profile yet: waits for one" grep -q 'waiting for its profile' t4/log0
{ echo "winapk-aot-profile 1 $hash"; head -1000 entries.txt; } > "$P"
check "a profile: work to do" test "$(needs_work t4 libbig.so)" = 1
FAKECLANG_MS=50 worker t4 > t4/log 2>&1
check "translated where it ran" grep -q 'profile: 1000 of' t4/log
check "stamp profile 1000 ($(stamp t4 libbig.so))" grep -q '^profile 1000 ' t4/lib/arm64/libbig.so.aot.stamp
sed -n 1001,1200p entries.txt >> "$P"
check "+20%: not again yet" test "$(needs_work t4 libbig.so)" = 0
sed -n 1201,1300p entries.txt >> "$P"
check "+30%: again" test "$(needs_work t4 libbig.so)" = 1

echo "== 5. four apps' workers on one library at once"
for mode in links copies; do
    rm -rf t5 && mkdir -p t5/cache
    for a in 1 2 3 4; do mkdir -p "t5/app$a/lib/arm64" && cp libguest.so "t5/app$a/lib/arm64/"; done
    : > t5/clang.log
    FAKECLANG_LOG="$(win "$PWD/t5/clang.log")"
    fault=""
    [ "$mode" = copies ] && fault=nolink
    for a in 1 2 3 4; do
        WINAPK_AOT_FAULT="$fault" FAKECLANG_MS=300 FAKECLANG_LOG="$FAKECLANG_LOG" "$WINE" ./winapk-runtime.exe \
            --aot-translate "$(win "$PWD/t5/app$a/lib/arm64")" --cache "$(win "$PWD/t5/cache")" > "t5/log$a" 2>&1 &
    done
    wait
    good=0
    for a in 1 2 3 4; do
        [ "$(check_dll "t5/app$a/lib/arm64/libguest.so.aot.dll")" = sealed ] &&
            grep -q '^full 0 ' "t5/app$a/lib/arm64/libguest.so.aot.stamp" &&
            [ "$(needs_work "t5/app$a" libguest.so)" = 0 ] && good=$((good + 1))
    done
    check "$mode: every app's DLL whole, stamped full, nothing left to do ($good of 4)" test "$good" -eq 4
    check "$mode: one entry in the cache, whole" test "$(ls t5/cache | wc -l)" -eq 1 -a \
        "$(check_dll t5/cache/*.aot.dll)" = sealed
    check "$mode: no temporary file left" test -z "$(find t5 -name '*.tmp')"
done

echo "== 6. killed at each step of a write"
for point in seal cache-put install installed write; do
    setup t6 libguest.so
    code=0
    WINAPK_AOT_FAULT="kill:$point" FAKECLANG_MS=20 worker t6 > t6/log 2>&1 || code=$?
    check "kill:$point ends the worker there (exit code $code)" test "$code" -eq 99
    dll=$(check_dll t6/lib/arm64/libguest.so.aot.dll)
    check "kill:$point: the app's DLL is whole or none ($dll)" test "$dll" = sealed -o "$dll" = missing
    check "kill:$point: work is left" test "$(needs_work t6 libguest.so)" = 1
    for f in t6/cache/*.aot.dll; do
        [ -e "$f" ] && check "kill:$point: the cache entry is whole" test "$(check_dll "$f")" = sealed
    done
    FAKECLANG_MS=20 worker t6 > t6/log2 2>&1
    check "kill:$point: the next run finishes ($(stamp t6 libguest.so))" grep -q '^full 0 ' \
        t6/lib/arm64/libguest.so.aot.stamp
    check "kill:$point: with a whole DLL" test "$(check_dll t6/lib/arm64/libguest.so.aot.dll)" = sealed
    check "kill:$point: nothing left to do" test "$(needs_work t6 libguest.so)" = 0
done

echo "== 7. a DLL cut short under a full stamp"
setup t7 libguest.so
FAKECLANG_MS=20 worker t7 > t7/log 2>&1
truncate -s -10 t7/lib/arm64/libguest.so.aot.dll  # a hard link: the cache's entry too
check "the stamp says full" grep -q '^full 0 ' t7/lib/arm64/libguest.so.aot.stamp
check "work to do" test "$(needs_work t7 libguest.so)" = 1
: > t7/clang.log
FAKECLANG_MS=20 worker t7 > t7/log2 2>&1
check "the cache's entry is not taken" test -z "$(grep 'taken from the shared cache' t7/log2)"
check "compiled again ($(compiles t7) compiles)" test "$(compiles t7)" -ge 1
check "a whole DLL" test "$(check_dll t7/lib/arm64/libguest.so.aot.dll)" = sealed
check "nothing left to do" test "$(needs_work t7 libguest.so)" = 0

echo "== 8. a cache entry cut short"
setup t8 libguest.so
cp t1/cache/*.aot.dll t8/cache/
entry=$(ls t8/cache/*.aot.dll)
truncate -s 10 "$entry"
FAKECLANG_MS=20 worker t8 > t8/log 2>&1
check "not taken" test -z "$(grep 'taken from the shared cache' t8/log)"
check "compiled instead ($(compiles t8) compiles)" test "$(compiles t8)" -ge 1
check "the app's DLL is not the cut-short one ($(stat -c %s t8/lib/arm64/libguest.so.aot.dll) bytes)" \
    test "$(stat -c %s t8/lib/arm64/libguest.so.aot.dll)" -gt 10
check "a whole one in the cache now" test "$(check_dll "$entry")" = sealed

echo "== 9. a library dated in the future"
setup t9 libguest.so
cp t1/cache/*.aot.dll t9/cache/  # as a second app: from the cache
touch -d '+1 day' t9/lib/arm64/libguest.so
FAKECLANG_MS=20 worker t9 > t9/log 2>&1
taken=$(grep -c 'taken from the shared cache' t9/log || true)
check "translated once, not at every pass ($taken)" test "$taken" -eq 1
check "nothing left to do" test "$(needs_work t9 libguest.so)" = 0

echo "== 10. temporary files left by killed writers"
setup t10 libguest.so
for f in t10/cache/x.aot.dll.1a2b-0.tmp t10/lib/arm64/libguest.so.aot.stamp.1a2b-0.tmp; do echo partial > "$f"; done
touch t10/cache/fresh.aot.dll.1a2c-0.tmp
touch -d '-2 hours' t10/cache/x.aot.dll.1a2b-0.tmp t10/lib/arm64/libguest.so.aot.stamp.1a2b-0.tmp
FAKECLANG_MS=20 worker t10 > t10/log 2>&1
check "the stale ones are deleted" test ! -e t10/cache/x.aot.dll.1a2b-0.tmp -a \
    ! -e t10/lib/arm64/libguest.so.aot.stamp.1a2b-0.tmp
check "a fresh one is left (its writer may be at it)" test -e t10/cache/fresh.aot.dll.1a2c-0.tmp

echo
if [ "$failures" -eq 0 ]; then echo "OK"; else echo "FAILED: $failures"; fi
exit "$failures"
