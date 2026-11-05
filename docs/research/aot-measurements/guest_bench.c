// The guest side of aotbench: arm64_bench.c with a bump allocator in the
// guest itself, so the measured benchmarks need no host calls.
typedef unsigned long size_t;
static unsigned char heap[16u << 20] __attribute__((aligned(16)));
static size_t used;
__attribute__((visibility("hidden"))) void* malloc(size_t n) {
    void* p = heap + used;
    used += (n + 15) & ~(size_t)15;
    return used <= sizeof heap ? p : 0;
}
__attribute__((visibility("hidden"))) void free(void* p) { (void)p; used = 0; }
#define BENCH_HOST
void* memcpy(void*, const void*, size_t);
size_t strlen(const char*);
void qsort(void*, size_t, size_t, int (*)(const void*, const void*));
#include "arm64_bench.c"

// Direct calls (BL): within one translation they are C calls; to a function
// in another DLL they go through aot_rt.branch and the lookup.
__attribute__((noinline)) u64 callee_a(u64 h) { return (h ^ (h >> 31)) * 0x9e3779b97f4a7c15ull; }
__attribute__((noinline)) u64 callee_b(u64 h) { return h + (h << 7) + 1; }
BENCH_EXPORT u64 bench_calls(u64 n, u64 seed, u64 c) {
    u64 h = seed + c;
    for (u64 i = 0; i < n; ++i) h = callee_b(callee_a(h)) ^ i;
    return h;
}
