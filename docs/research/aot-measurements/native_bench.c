// The native reference for aotbench: the same benchmarks compiled for the host.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define BENCH_HOST
#include "arm64_bench.c"
__attribute__((noinline)) u64 callee_a(u64 h) { return (h ^ (h >> 31)) * 0x9e3779b97f4a7c15ull; }
__attribute__((noinline)) u64 callee_b(u64 h) { return h + (h << 7) + 1; }
u64 bench_calls(u64 n, u64 seed, u64 c) { u64 h = seed + c; for (u64 i = 0; i < n; ++i) h = callee_b(callee_a(h)) ^ i; return h; }
static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}
int main(int argc, char** argv) {
    u64 n = strtoull(argv[1], 0, 0); const char* only = argc > 2 ? argv[2] : 0;
    struct { const char* name; u64 (*f)(u64, u64, u64); } b[] = {
        {"compute", bench_compute}, {"float", bench_float}, {"indirect", bench_indirect}, {"memory", bench_memory}, {"calls", bench_calls}};
    for (int i = 0; i < 5; ++i) { if (only && strcmp(only, b[i].name)) continue;
        double t = now();
        u64 r = b[i].f(n, 12345, 7);
        printf("%s %.4f %016llx\n", b[i].name, now() - t, (unsigned long long)r);
    }
    return 0;
}
