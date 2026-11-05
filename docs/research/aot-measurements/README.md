# Tools for the AOT measurements

The scripts behind [`../aot-measurements.md`](../aot-measurements.md). They run
on Linux x86-64 with clang 20 and lld, and do not change Win.apk's own code:
the translator is a patched copy.

| File | What |
|---|---|
| `winapk-aot-measure.patch` | Against `windows/tools/arm64_aot_main.cpp`. Adds `WINAPK_AOT_ONLY` / `WINAPK_AOT_EXCLUDE` (comma-separated hex vaddrs: translate only / all but these functions), `WINAPK_AOT_DUMP=<file>` (one line per function: vaddr, instructions, hash of its C) and `WINAPK_AOT_EH=1` (add every function start in `.eh_frame_hdr`) |
| `winapk-aot-site-cache.patch` | On top of the first: `CALL_SITE` also remembers a translated target (set once per call site), through a new `AotRuntime::lookup` |
| `aotbench.cpp` | Loads an arm64 `.so` (relative relocations only), runs functions on the interpreter (`src/arm64/arm64_cpu.cpp`, `arm64_simd.cpp`) or on one or more translations compiled to host `.so` files; `AOTBENCH_LOOKUP=1` sets `AotRuntime::lookup`, `AOTBENCH_FASTMAP=1` puts a 4096-entry direct-mapped table in front of the function map |
| `guest_bench.c`, `native_bench.c` | `windows/test/guest/arm64_bench.c` as the guest library (with a guest-side allocator, and a direct-call benchmark) and as its native reference |
| `speed.sh` | Code speed per compile setting, ratio to native, median of 5 |
| `matrix.py` | Compile cost per setting on a fixed random sample of a library's functions |
| `reuse.py` | How much of a pass must be compiled again as a profile grows, per way of cutting parts |
| `fdecount.py` | Function starts listed in `.eh_frame_hdr` |
| `measure.py` | Time and peak memory of compiling C files one by one |
| `aot_flags_windows.ps1` | On Windows, with an installed Win.apk: the `-O1` / `-O2` / `-O2 -mllvm -fast-isel` comparison of `matrix.py` with the bundled clang, on the arm64 libraries of the installed apps (`aot-measurements.md`, section 8, step 1). Writes `winapk-aot-flags.csv` on the desktop |

Build:

```bash
patch -o arm64_aot_x.cpp windows/tools/arm64_aot_main.cpp docs/research/aot-measurements/winapk-aot-measure.patch
clang++ -std=c++17 -O2 -Iwindows/include -o winapk-aot-x arm64_aot_x.cpp
clang++ -std=c++17 -O2 -mcx16 -Iwindows/include -Iwindows/src/arm64 -o aotbench \
    docs/research/aot-measurements/aotbench.cpp windows/src/arm64/arm64_cpu.cpp windows/src/arm64/arm64_simd.cpp -ldl -latomic
clang --target=aarch64-linux-android29 -ffreestanding -O2 -fPIC -fvisibility=hidden -shared -nostdlib -fuse-ld=lld \
    -Wl,-z,now -Iwindows/test/guest -o guest.so docs/research/aot-measurements/guest_bench.c
clang -O2 -Iwindows/test/guest -o native docs/research/aot-measurements/native_bench.c
```

A translation for `aotbench` is compiled as a Linux shared object:
`clang <flags> -march=native -fno-strict-aliasing -w -fdeclspec -fPIC -shared -o x.so x.c`.
`matrix.py` compiles for `--target=x86_64-w64-windows-gnu -ffreestanding`, as the worker does.
