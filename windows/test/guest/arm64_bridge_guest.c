// arm64_bridge_guest.c -- the arm64 library test/arm64_test.cpp uses to check
// the guest <-> host boundary (arm64_guest.h).
//
// Built for aarch64-linux-android with plain clang and no NDK, like
// poc_guest.c: the libc functions are declared by hand and resolved by the
// loader against the Bionic shim, which is System V x86_64 code. Every call
// below therefore crosses from the interpreter into the host and, for the
// callbacks, back.

typedef unsigned long size_t;
typedef __builtin_va_list va_list;
#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_end(ap) __builtin_va_end(ap)

void* malloc(size_t);
void free(void*);
void* memcpy(void*, const void*, size_t);
void* memset(void*, int, size_t);
size_t strlen(const char*);
int strcmp(const char*, const char*);
int snprintf(char*, size_t, const char*, ...);
int vsnprintf(char*, size_t, const char*, va_list);
void qsort(void*, size_t, size_t, int (*)(const void*, const void*));
void* bsearch(const void*, const void*, size_t, size_t, int (*)(const void*, const void*));
int* __errno(void);
int __android_log_write(int prio, const char* tag, const char* text);
int __android_log_print(int prio, const char* tag, const char* fmt, ...);
int __cxa_atexit(void (*)(void*), void*, void*);
typedef long pthread_t;
typedef long jmp_buf[32];
int setjmp(jmp_buf);
void longjmp(jmp_buf, int) __attribute__((noreturn));
int pthread_create(pthread_t*, const void*, void* (*)(void*), void*);
int pthread_join(pthread_t, void**);
long strtol(const char*, char**, int);
double strtod(const char*, char**);
int vasprintf(char**, const char*, va_list);
void android_set_abort_message(const char*);
void openlog(const char*, int, int);
void syslog(int, const char*, ...);
void vsyslog(int, const char*, va_list);
void closelog(void);

#define EXPORT __attribute__((visibility("default")))

// ---- constructors and the stack protector -----------------------------------
static volatile int g_ctor_value;

__attribute__((constructor)) static void arm64_ctor(void) {
    g_ctor_value = 0x5150;
    __android_log_write(3, "WinApkArm64", "constructor ran in the interpreter");
}

EXPORT int arm64_ctor_ran(void) { return g_ctor_value; }

// Built with -fstack-protector-all: every function here reads the guard from
// TPIDR_EL0 slot 5. A local array keeps the check meaningful.
EXPORT int arm64_stack_guard(int seed) {
    volatile char buf[100];
    for (int i = 0; i < 100; ++i) buf[i] = (char)(seed + i);
    int sum = 0;
    for (int i = 0; i < 100; ++i) sum += buf[i];
    return sum;
}

// ---- plain libc calls ---------------------------------------------------------
EXPORT int arm64_libc(void) {
    const char* src = "hello from an Android arm64 library";
    size_t n = strlen(src) + 1;
    char* copy = (char*)malloc(n);
    if (!copy) return -1;
    memcpy(copy, src, n);
    int ok = strcmp(copy, src) == 0;
    free(copy);
    *__errno() = 34;
    ok = ok && *__errno() == 34;
    return ok ? (int)n : -2;
}

// ---- variadic calls and va_list -------------------------------------------------
// Ten integer-class arguments and doubles: under System V the 7th and later
// integer ones go on the stack, under AAPCS64 the 9th and later.
EXPORT int arm64_snprintf(char* out, int cap) {
    return snprintf(out, (size_t)cap, "%d %s %ld %.2f %c %u %x %lld %.1f %d %s %d", 1, "two", 3L, 4.5, '5', 6u, 0x7f,
                    8LL, 9.3, 10, "eleven", 12);
}

static int my_format(char* out, size_t cap, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out, cap, fmt, ap);
    va_end(ap);
    return n;
}

// The guest's own va_list (AAPCS64) handed to the host's vsnprintf.
EXPORT int arm64_vsnprintf(char* out, int cap) {
    return my_format(out, (size_t)cap, "%s|%d|%.3f|%ld|%d|%d|%d|%d|%d|%.1f|%s", "va", 1, 2.5, 3L, 4, 5, 6, 7, 8, 9.5,
                     "end");
}

EXPORT int arm64_log_print(void) {
    return __android_log_print(4, "WinApkArm64", "log line with %d and %s from the interpreter", 42, "args");
}

// ---- host -> guest callbacks ---------------------------------------------------------
static int compare_ints(const void* a, const void* b) {
    int x = *(const int*)a, y = *(const int*)b;
    return (x > y) - (x < y);
}

EXPORT int arm64_qsort(void) {
    int v[] = {42, 7, 19, 3, 88, 21, 1, 56, -4, 13};
    qsort(v, 10, sizeof(int), compare_ints);
    for (int i = 1; i < 10; ++i) {
        if (v[i - 1] > v[i]) return 0;
    }
    return v[0] * 1000 + v[9];  // -4 * 1000 + 88 = -3912
}

// bsearch with a guest comparator, for every key from -5 to 90: each of the
// ten values found at its index, nothing else found.
EXPORT int arm64_bsearch(void) {
    static const int v[] = {-4, 1, 3, 7, 13, 19, 21, 42, 56, 88};
    int r = 0;
    for (int key = -5; key <= 90; ++key) {
        const int* hit = bsearch(&key, v, 10, sizeof(int), compare_ints);
        if (hit && *hit != key) return -1;
        if (hit) r += (int)(hit - v) * 7 + 1;
    }
    return r;  // 7 * (0 + 1 + ... + 9) + 10 = 325
}

// A comparator that itself calls the host: guest -> host -> guest -> host.
static int compare_by_text(const void* a, const void* b) {
    char x[16], y[16];
    snprintf(x, sizeof x, "%05d", *(const int*)a);
    snprintf(y, sizeof y, "%05d", *(const int*)b);
    return strcmp(x, y);
}

EXPORT int arm64_nested(void) {
    int v[] = {300, 20, 1000, 4, 55};
    qsort(v, 5, sizeof(int), compare_by_text);
    return v[0] + v[4] * 10;  // 4 + 1000 * 10 = 10004
}

static void* thread_main(void* arg) {
    long n = (long)arg;
    long sum = 0;
    for (long i = 1; i <= n; ++i) sum += i * i;
    char buf[32];
    snprintf(buf, sizeof buf, "%ld", sum);  // the new thread calls the host too
    return (void*)strtol(buf, 0, 10);
}

EXPORT long arm64_threads(void) {
    pthread_t t[4];
    for (long i = 0; i < 4; ++i) {
        if (pthread_create(&t[i], 0, thread_main, (void*)(100 + i)) != 0) return -1;
    }
    long total = 0;
    for (int i = 0; i < 4; ++i) {
        void* r = 0;
        if (pthread_join(t[i], &r) != 0) return -2;
        total += (long)r;
    }
    return total;
}

static int g_exit_count;
static void on_exit_handler(void* arg) { g_exit_count += (int)(long)arg; }

EXPORT int arm64_register_atexit(void) { return __cxa_atexit(on_exit_handler, (void*)7, 0); }
EXPORT int arm64_exit_count(void) { return g_exit_count; }

// ---- arguments and results across the boundary, both ways ---------------------------
// Called by the host as a System V function pointer (through the execute-fault
// path or host_callable()).
EXPORT long arm64_many_args(long a, long b, long c, long d, long e, long f, long g, long h, long i, long j,
                            double x, double y) {
    return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * h + 9 * i + 10 * j + (long)(x * 100 + y * 1000);
}

// As many integer arguments as Flutter's FlutterJNI.nativeSetViewportMetrics
// (29: 21 of them on the stack), the last ones pointers like its int[].
EXPORT long arm64_jni_sized(long a0, long a1, long a2, long a3, long a4, long a5, long a6, long a7, long a8, long a9,
                            long a10, long a11, long a12, long a13, long a14, long a15, long a16, long a17, long a18,
                            long a19, long a20, long a21, long a22, long a23, long a24, long a25, long a26,
                            const long* p27, const long* p28) {
    long r = a0 + 2 * a1 + 3 * a2 + 4 * a3 + 5 * a4 + 6 * a5 + 7 * a6 + 8 * a7 + 9 * a8 + 10 * a9;
    r += 11 * a10 + 12 * a11 + 13 * a12 + 14 * a13 + 15 * a14 + 16 * a15 + 17 * a16 + 18 * a17 + 19 * a18;
    r += 20 * a19 + 21 * a20 + 22 * a21 + 23 * a22 + 24 * a23 + 25 * a24 + 26 * a25 + 27 * a26;
    return r + 28 * *p27 + 29 * *p28;
}

EXPORT double arm64_scale(double x, float y, int n) { return x * y * n; }

// Calls a host function pointer with the same shape.
EXPORT long arm64_call_host(long (*fn)(long, long, long, long, long, long, long, long, long, long, double, double)) {
    return fn(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 0.5, 0.25);
}

EXPORT double arm64_parse(const char* s) { return strtod(s, 0) * 2; }

// ---- setjmp/longjmp, as libpng and Lua use them for errors -----------------------------
static jmp_buf g_jmp;

static __attribute__((noinline)) void deep(int depth, volatile int* trail) {
    *trail = *trail * 10 + depth;
    if (depth == 3) longjmp(g_jmp, 42);
    deep(depth + 1, trail);
    *trail = -1;  // never reached
}

EXPORT int arm64_setjmp(void) {
    volatile int trail = 0;
    volatile long kept = 7;  // callee-saved across the jump
    int r = setjmp(g_jmp);
    if (r == 0) {
        kept = 9;
        deep(1, &trail);
        return -1;
    }
    char buf[16];
    snprintf(buf, sizeof buf, "%d", r);  // the stack is usable after the jump
    return (int)strtol(buf, 0, 10) * 10000 + trail * 10 + (int)kept;  // 42 * 10000 + 123 * 10 + 9
}

// ---- arm64 layouts the shim converts: fts's struct stat; and cleanup handlers ----------
struct a64_stat {  // Bionic arm64 (asm-generic): 128 bytes, st_size at 48
    unsigned long dev, ino;
    unsigned mode, nlink, uid, gid;
    unsigned long rdev, pad1;
    long size;
    int blksize, pad2;
    long blocks, times[6];
    unsigned unused[2];
};
int stat(const char*, struct a64_stat*);
typedef struct FTSENT {
    struct FTSENT *cycle, *parent, *link;
    long number;
    void* pointer;
    char *accpath, *path;
    int err, symfd;
    size_t pathlen, namelen;
    unsigned long ino, dev;
    unsigned nlink;
    short level;
    unsigned short info, flags, instr;
    struct a64_stat* statp;
    char name[1];
} FTSENT;
typedef struct FTS FTS;
FTS* fts_open(char* const*, int, int (*)(const FTSENT**, const FTSENT**));
FTSENT* fts_read(FTS*);
int fts_close(FTS*);

static int g_fts_compares;
static int fts_by_name(const FTSENT** a, const FTSENT** b) {
    ++g_fts_compares;
    return strcmp((*a)->name, (*b)->name);
}

// Walks `dir` (FTS_PHYSICAL, sorted by a guest comparator) and returns 1 when
// `name` was found with the size stat() gives it, in arm64's struct stat.
EXPORT int arm64_fts(const char* dir, const char* name) {
    char* roots[] = {(char*)dir, 0};
    FTS* fts = fts_open(roots, 0x10, fts_by_name);
    if (!fts) return -1;
    int found = 0;
    for (FTSENT* e; (e = fts_read(fts));) {
        if (e->level != 1 || strcmp(e->name, name) != 0) continue;
        struct a64_stat st;
        found = e->info == 8 && stat(e->path, &st) == 0 && st.size > 0 && e->statp->size == st.size &&
                (e->statp->mode & 0170000) == 0100000;
    }
    fts_close(fts);
    return found && g_fts_compares > 0;
}

struct cleanup_t { struct cleanup_t* prev; void (*routine)(void*); void* arg; };
void __pthread_cleanup_push(struct cleanup_t*, void (*)(void*), void*);
void __pthread_cleanup_pop(struct cleanup_t*, int);
void pthread_exit(void*) __attribute__((noreturn));
static int g_cleanup_sum;
static void cleanup_add(void* arg) { g_cleanup_sum = g_cleanup_sum * 10 + (int)(long)arg; }
static void* cleanup_thread(void* arg) {
    (void)arg;
    struct cleanup_t a, b;
    __pthread_cleanup_push(&a, cleanup_add, (void*)1);
    __pthread_cleanup_push(&b, cleanup_add, (void*)2);
    __pthread_cleanup_pop(&b, 1);
    pthread_exit((void*)3);
}
// 2 (popped with execute), then 1 (run by pthread_exit), then 3 joined: 213.
EXPORT int arm64_cleanup(void) {
    pthread_t t;
    void* r = 0;
    g_cleanup_sum = 0;
    if (pthread_create(&t, 0, cleanup_thread, 0) || pthread_join(t, &r)) return -1;
    return g_cleanup_sum * 10 + (int)(long)r;
}

// ---- libc++abi's abort_message, short of the abort ---------------------------------
// What NDK libc++abi does before abort() when a C++ exception goes uncaught:
// format the message, hand it to android_set_abort_message, then
// openlog("libc++abi", 0, 0), syslog, closelog. The shim's no-op stood for
// vsyslog too, so the bridge took openlog's third argument (0) for a va_list
// and the host crashed reading 0x18 (Bank Leumi, under Bugsnag).
static int abort_tail(const char* fmt, ...) {
    char* buffer = 0;
    va_list ap;
    va_start(ap, fmt);
    int n = vasprintf(&buffer, fmt, ap);
    va_end(ap);
    if (n < 0 || !buffer) return -1;
    android_set_abort_message(buffer);
    openlog("libc++abi", 0, 0);
    syslog(2, "%s", buffer);
    va_start(ap, fmt);
    vsyslog(2, fmt, ap);
    va_end(ap);
    closelog();
    int ok = strcmp(buffer, "terminating due to uncaught exception of type E: what") == 0;
    free(buffer);
    return ok ? 1 : -2;
}

EXPORT int arm64_abort_tail(void) {
    return abort_tail("terminating due to uncaught exception of type %s: %s", "E", "what");
}

// ---- raw system calls -------------------------------------------------------------
// Protection libraries (Leumi's libe689.so) make their calls as raw SVCs, by
// arm64's numbers, so that nothing hooking libc sees them; others go through
// syscall(). Each check is one bit of the result, so the test names a failure.
long syscall(long, ...);
int getpid(void);
int gettid(void);
int getuid(void);
struct a64_timespec { long tv_sec, tv_nsec; };
struct a64_utsname { char sysname[65], nodename[65], release[65], version[65], machine[65], domainname[65]; };

static long svc6(long nr, long a0, long a1, long a2, long a3, long a4, long a5) {
    register long x8 __asm__("x8") = nr;
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    register long x3 __asm__("x3") = a3;
    register long x4 __asm__("x4") = a4;
    register long x5 __asm__("x5") = a5;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory");
    return x0;
}

EXPORT int arm64_raw_syscalls(void) {
    int bits = 0;
    // SVC 222 mmap (1 MB, read/write, MAP_PRIVATE | MAP_ANONYMOUS), 226 mprotect, 215 munmap.
    char* p = (char*)svc6(222, 0, 1 << 20, 3, 0x22, -1, 0);
    if (p && (unsigned long)p < (unsigned long)-4096L) {
        p[0] = 1;
        p[(1 << 20) - 1] = 2;
        if (p[1 << 19] == 0) bits |= 1;
        if (svc6(226, (long)p, 65536, 1, 0, 0, 0) == 0) bits |= 2;
        if (svc6(215, (long)p, 1 << 20, 0, 0, 0, 0) == 0) bits |= 4;
    }
    // The same three through the syscall() import: six arguments, the sixth on the bridge's stack.
    char* q = (char*)syscall(222, 0L, 65536L, 3L, 0x22L, -1L, 0L);
    if (q && q != (char*)-1) {
        q[65535] = 5;
        if (syscall(226, q, 65536L, 1L) == 0 && syscall(215, q, 65536L) == 0) bits |= 8;
    }
    if (svc6(172, 0, 0, 0, 0, 0, 0) == getpid() && svc6(178, 0, 0, 0, 0, 0, 0) == gettid()) bits |= 16;
    if (svc6(174, 0, 0, 0, 0, 0, 0) == getuid()) bits |= 32;
    struct a64_timespec ts = {0, 1000000};
    if (svc6(101, (long)&ts, 0, 0, 0, 0, 0) == 0) bits |= 64;
    struct a64_utsname u;
    if (svc6(160, (long)&u, 0, 0, 0, 0, 0) == 0 && strcmp(u.sysname, "Linux") == 0) bits |= 128;
    // A call the shim does not have: -ENOSYS in x0, as a kernel without it answers.
    if (svc6(9999, 0, 0, 0, 0, 0, 0) == -38) bits |= 256;
    return bits;
}
