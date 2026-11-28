// syscall_guest.c -- raw system calls in x86_64 guest code, for winapk-syscall-test.
//
// The SYSCALL instructions here are what a self-unpacking library or a
// protector does without libc (include/winapk/x86_syscall.h): each one must
// reach the Bionic shim as the kernel's call. It also has what UPX needs to
// pack it (--android-shlib: DT_INIT and enough code to shrink), for checking a
// self-unpacking library by hand (docs/research/packers.md).

typedef unsigned long size_t;
typedef long ssize_t;

int getpid(void);
int pipe(int fds[2]);
int close(int fd);
ssize_t write(int fd, const void* buf, size_t n);
void* memcpy(void* d, const void* s, size_t n);
int* __errno(void);

#define EXPORT __attribute__((visibility("default")))

static long raw0(long nr) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(nr) : "rcx", "r11", "memory");
    return r;
}
static long raw3(long nr, long a, long b, long c) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(nr), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return r;
}
static long raw6(long nr, long a, long b, long c, long d, long e, long f) {
    long r;
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    register long r9 __asm__("r9") = f;
    __asm__ volatile("syscall" : "=a"(r) : "a"(nr), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return r;
}

// What SYSCALL leaves in rcx (the next instruction) and r11 (the flags), as the kernel does.
EXPORT long syscall_rcx_is_return(void) {
    long rcx, here;
    __asm__ volatile(
        "mov $39, %%eax\n\t"
        "syscall\n\t"
        "1: lea 1b(%%rip), %1\n\t"
        "mov %%rcx, %0\n\t"
        : "=r"(rcx), "=r"(here)
        :
        : "rax", "rcx", "r11", "memory");
    return rcx == here;
}

// Bytes 0f 05 that are not a SYSCALL instruction, which the loader must leave
// alone: two jumped over (data in the middle of code), and two inside a mov's
// immediate. syscall_decoy runs the code around them and returns 0x50f.
__asm__(".text\n"
        ".globl syscall_decoy\n"
        ".type syscall_decoy, @function\n"
        "syscall_decoy:\n"
        "    jmp 1f\n"
        ".globl syscall_decoy_skipped\n"
        "syscall_decoy_skipped:\n"
        "    .byte 0x0f, 0x05\n"
        "1:\n"
        ".globl syscall_decoy_immediate\n"
        "syscall_decoy_immediate:\n"
        "    movl $0x050f, %eax\n"
        "    ret\n"
        ".size syscall_decoy, . - syscall_decoy\n");

static int g_ctor_ran;
__attribute__((constructor)) static void ctor(void) { g_ctor_ran = (int)raw0(39) == getpid(); }

// UPX needs DT_INIT to hook its loader stub into.
EXPORT void _init(void) {}

EXPORT int syscall_ctor_ran(void) { return g_ctor_ran; }

EXPORT long syscall_getpid(void) { return raw0(39) == getpid(); }

// write(2) by raw call into a pipe made by libc, and read(2) back by raw call.
EXPORT long syscall_pipe_roundtrip(void) {
    int fds[2];
    if (pipe(fds) != 0) return -100;
    long w = raw3(1, fds[1], (long)"packed", 6);
    char buf[8] = {0};
    long r = raw3(0, fds[0], (long)buf, sizeof buf);
    close(fds[0]);
    close(fds[1]);
    if (w != 6) return -1;
    if (r != 6) return -2;
    return buf[0] == 'p' && buf[5] == 'd';
}

// The kernel's error convention: -errno in rax, errno untouched.
EXPORT long syscall_errors(void) {
    *__errno() = 0;
    if (raw3(3, -1, 0, 0) != -9) return -1;            // close(-1): -EBADF
    if (raw0(100000) != -38) return -2;                // an unknown number: -ENOSYS
    if (*__errno() != 0) return -3;
    char buf[512];
    long n = raw3(79, (long)buf, sizeof buf, 0);       // getcwd: the length with its NUL
    if (n <= 1 || buf[n - 1] != 0) return -4;
    return 1;
}

// mmap, mprotect and munmap by raw call, and code written at run time with a
// SYSCALL in it: mov eax, 39; syscall; ret.
EXPORT long syscall_runtime_code(void) {
    static const unsigned char code[] = {0xb8, 0x27, 0x00, 0x00, 0x00, 0x0f, 0x05, 0xc3};
    long p = raw6(9, 0, 8192, 3 /* RW */, 0x22 /* PRIVATE|ANONYMOUS */, -1, 0);
    if (p < 0 && p > -4096) return -1;
    memcpy((void*)(p + 4096 - 4), code, sizeof code);  // across the page boundary
    if (raw3(10, p, 8192, 5 /* R|X */) != 0) return -2;
    long (*fn)(void) = (long (*)(void))(p + 4096 - 4);
    long pid = fn();
    // Written again, made executable again: found again.
    if (raw3(10, p, 8192, 3) != 0) return -3;
    ((unsigned char*)p)[4096 - 4 + 1] = 0x66;          // mov eax, 102 (getuid)
    if (raw3(10, p, 8192, 5) != 0) return -4;
    long uid = fn();
    if (raw3(11, p, 8192, 0) != 0) return -5;
    if (pid != getpid()) return -6;
    if (uid < 0) return -7;
    return 1;
}

// Code that copies itself and runs the copy, as a UPX stub moves its decompressor into memory it maps:
// the copy carries the SYSCALL the scan made a trap, and must still be the kernel's call.
// syscall_copy_src is mov eax, 39; syscall; ret, with bytes around it the copy keeps.
__asm__(".text\n"
        ".p2align 4\n"
        "syscall_copy_before: .byte 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc\n"
        ".globl syscall_copy_src\n"
        ".type syscall_copy_src, @function\n"
        "syscall_copy_src:\n"
        "  mov $39, %eax\n"
        "  syscall\n"
        "  ret\n"
        "  .byte 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc\n");
long syscall_copy_src(void);

EXPORT long syscall_copied_code(void) {
    const unsigned char* src = (const unsigned char*)syscall_copy_src - 16;
    long p = raw6(9, 0, 4096, 3 /* RW */, 0x22 /* PRIVATE|ANONYMOUS */, -1, 0);
    if (p < 0 && p > -4096) return -1;
    memcpy((void*)(p + 256), src, 16 + 8 + 8);
    if (raw3(10, p, 4096, 5 /* R|X */) != 0) return -2;
    long pid = ((long (*)(void))(p + 256 + 16))();
    raw3(11, p, 4096, 0);
    return pid == getpid() ? 1 : -3;
}

// An instruction that starts at the end of a page already run and ends in the next page, made
// executable at the same time but not run yet: the fault names the second page, not rip's.
EXPORT long syscall_cross_page(void) {
    long p = raw6(9, 0, 8192, 3, 0x22, -1, 0);
    if (p < 0 && p > -4096) return -1;
    unsigned char* a = (unsigned char*)p;
    a[0] = 0xc3;                                                    // ret: runs page A alone
    static const unsigned char code[] = {0xb8, 0x27, 0x00, 0x00, 0x00, 0x0f, 0x05, 0xc3};  // mov eax, 39 ...
    memcpy(a + 4096 - 3, code, sizeof code);                        // ... its immediate in page B
    if (raw3(10, p, 8192, 5) != 0) return -2;
    ((void (*)(void))a)();
    long pid = ((long (*)(void))(a + 4096 - 3))();
    raw3(11, p, 8192, 0);
    return pid == getpid() ? 1 : -3;
}

// /proc/self/auxv, read by raw calls as a loader stub with no libc reads it: (type, value) pairs ending
// in AT_NULL, AT_PAGESZ 4096, AT_RANDOM pointing at 16 readable bytes, AT_PLATFORM "x86_64", and every
// value the one getauxval gives. One bit per check, so the test names a failure.
unsigned long getauxval(unsigned long type);

EXPORT long syscall_auxv(void) {
    unsigned long v[128];
    long fd = raw3(2, (long)"/proc/self/auxv", 0 /* O_RDONLY */, 0);
    if (fd < 0) return -1;
    long n = 0, r;
    while (n < (long)sizeof v && (r = raw3(0, fd, (long)v + n, (long)sizeof v - n)) > 0) n += r;
    raw3(3, fd, 0, 0);
    if (n <= 0 || n % 16 != 0) return -2;
    long pairs = n / 16, bits = 0;
    if (v[2 * (pairs - 1)] == 0 && v[2 * (pairs - 1) + 1] == 0) bits |= 1;  // AT_NULL last
    int mismatches = 0, pagesz = 0, random = 0, platform = 0;
    for (long i = 0; i + 1 < pairs; ++i) {
        const unsigned long type = v[2 * i], value = v[2 * i + 1];
        if (type == 0) break;
        if (getauxval(type) != value) ++mismatches;
        if (type == 6 && value == 4096) pagesz = 1;  // AT_PAGESZ
        if (type == 25 && value) {                    // AT_RANDOM: 16 bytes, readable
            const volatile unsigned char* b = (const unsigned char*)value;
            unsigned sum = 0;
            for (int k = 0; k < 16; ++k) sum += b[k];
            random = sum != 0;  // read without a fault, and filled (all 16 zero: one chance in 2^128)
        }
        if (type == 15 && value) {  // AT_PLATFORM
            const char* p = (const char*)value;
            platform = p[0] == 'x' && p[1] == '8' && p[2] == '6' && p[3] == '_' && p[4] == '6' && p[5] == '4' && !p[6];
        }
    }
    if (mismatches == 0) bits |= 2;
    if (pagesz) bits |= 4;
    if (random) bits |= 8;
    if (platform) bits |= 16;
    *__errno() = 0;
    if (getauxval(0x7fff) == 0 && *__errno() == 2) bits |= 32;  // a type it lacks: 0, ENOENT
    return bits;
}

// Something for UPX to compress: it packs the executable segment and refuses a
// library that would not shrink, so a few dozen small functions.
#define FILL(n)                                                                    \
    __attribute__((noinline)) static unsigned fill##n(unsigned a) {                \
        unsigned s = 0;                                                            \
        for (unsigned i = 0; i < a; ++i) s += (i * n) ^ (s >> 3);                  \
        return s + n * 7;                                                          \
    }
#define FILL10(n) FILL(n##0) FILL(n##1) FILL(n##2) FILL(n##3) FILL(n##4) FILL(n##5) FILL(n##6) FILL(n##7) FILL(n##8) FILL(n##9)
FILL10(1) FILL10(2) FILL10(3) FILL10(4) FILL10(5) FILL10(6)
static unsigned (*const kFill[])(unsigned) = {
    fill10, fill11, fill12, fill13, fill14, fill15, fill16, fill17, fill18, fill19,
    fill20, fill21, fill22, fill23, fill24, fill25, fill26, fill27, fill28, fill29,
    fill30, fill31, fill32, fill33, fill34, fill35, fill36, fill37, fill38, fill39,
    fill40, fill41, fill42, fill43, fill44, fill45, fill46, fill47, fill48, fill49,
    fill50, fill51, fill52, fill53, fill54, fill55, fill56, fill57, fill58, fill59,
    fill60, fill61, fill62, fill63, fill64, fill65, fill66, fill67, fill68, fill69,
};

// The same answer whether or not the library was packed (and only once the constructor has run).
EXPORT unsigned syscall_checksum(unsigned a) {
    unsigned sum = 0;
    for (unsigned i = 0; i < sizeof kFill / sizeof kFill[0]; ++i) sum = sum * 31 + kFill[i](a);
    return sum + (unsigned)g_ctor_ran;
}
