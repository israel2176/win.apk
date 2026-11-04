#!/bin/bash
# speed.sh <hb dir> : code speed of each compile setting, ratio to native (median of 5)
set -e
# AOT: winapk-aot built with winapk-aot-measure.patch. <hb dir>: guest.so, native, aotbench.
H=$1; cd "$H"
declare -A N=([compute]=20000000 [float]=2000 [indirect]=20000000 [memory]=20 [calls]=50000000)
declare -A A
for s in bench_compute bench_float bench_indirect bench_memory bench_calls callee_a callee_b; do
  A[$s]=$(nm guest.so | awk -v s=$s '$3==s {print $1}' | sed 's/^0*//'); done
med() { sort -n | awk '{a[NR]=$1} END {print a[int((NR+1)/2)]}'; }
run() { # label dlls
  local line="$1"
  for b in compute float indirect memory calls; do
    local t=$(for i in 1 2 3 4 5; do ./aotbench guest.so "$2" ${N[$b]} $b=${A[bench_$b]} | awk '{print $2}'; done | med)
    local nt=$(for i in 1 2 3 4 5; do ./native ${N[$b]} $b | awk '{print $2}'; done | med)
    line="$line $(awk -v a=$t -v n=$nt 'BEGIN {printf "%.2f", a/n}')"
  done
  echo "$line"
}
FLAGS="-march=native -fno-strict-aliasing -w -fdeclspec -fPIC -shared"
"${AOT:?set AOT}" guest.so whole.c 1 2>/dev/null
echo "setting compute float indirect memory calls (x native)"
for o in "-O0" "-O1" "-O1 -mllvm -fast-isel" "-O2" "-O3"; do
  clang $o $FLAGS -o w.so whole.c; run "[${o// /_}]" ./w.so; done
# a base DLL plus a delta DLL holding the callees: calls cross DLLs
WINAPK_AOT_EXCLUDE=${A[callee_a]},${A[callee_b]} "${AOT:?set AOT}" guest.so base.c 1 2>/dev/null
WINAPK_AOT_ONLY=${A[callee_a]},${A[callee_b]} "${AOT:?set AOT}" guest.so delta.c 1 2>/dev/null
clang -O1 $FLAGS -o base.so base.c; clang -O1 $FLAGS -o delta.so delta.c
run "[-O1_base+delta]" ./base.so,./delta.so
