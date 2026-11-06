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
#      what ran, and again only once the profile has grown by a quarter.
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
"$CXX" "${FLAGS[@]}" -o winapk-runtime.exe worker.cpp -lshell32
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

echo
if [ "$failures" -eq 0 ]; then echo "OK"; else echo "FAILED: $failures"; fi
exit "$failures"
