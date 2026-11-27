// jit_guest.c -- code written at run time the way a JIT writes it, for
// winapk-syscall-test. No SYSCALL instruction anywhere: only libc's mmap,
// mprotect and memfd_create. The test runs these before and after a library
// with raw system calls is loaded (include/winapk/x86_syscall.h), and they must
// give the same answers both times.

typedef unsigned long size_t;
typedef long off_t;

void* mmap(void* addr, size_t len, int prot, int flags, int fd, off_t off);
int mprotect(void* addr, size_t len, int prot);
int munmap(void* addr, size_t len);
int memfd_create(const char* name, unsigned flags);
int ftruncate(int fd, off_t len);
int close(int fd);

#define EXPORT __attribute__((visibility("default")))
#define PROT_R 1
#define PROT_W 2
#define PROT_X 4
#define MAP_SHARED 1
#define MAP_PRIVATE 2
#define MAP_ANON 0x20
#define MAP_FAILED ((void*)-1)

typedef int (*AddFn)(int);

// lea eax, [rdi + k]; ret
static void emit_add(unsigned char* at, signed char k) {
    at[0] = 0x8d;
    at[1] = 0x47;
    at[2] = (unsigned char)k;
    at[3] = 0xc3;
}

// Fresh pages, written, made read-execute, run many times; then W^X: back to
// read-write, rewritten, read-execute again, `rounds` times, each run checked.
// 1 when every answer was right.
EXPORT long jit_wx(int rounds, int calls) {
    unsigned char* p = (unsigned char*)mmap(0, 8192, PROT_R | PROT_W, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) return -1;
    for (int r = 0; r < rounds; ++r) {
        if (r && mprotect(p, 8192, PROT_R | PROT_W) != 0) return -2;
        const signed char k = (signed char)(r % 100);
        emit_add(p + 64, k);
        emit_add(p + 4096 + 32, (signed char)(k + 1));  // the second page too
        if (mprotect(p, 8192, PROT_R | PROT_X) != 0) return -3;
        AddFn a = (AddFn)(p + 64), b = (AddFn)(p + 4096 + 32);
        for (int i = 0; i < calls; ++i) {
            if (a(i) != i + k || b(i) != i + k + 1) return -4;
        }
    }
    munmap(p, 8192);
    return 1;
}

// One anonymous file mapped twice: written through a read-write mapping, run
// through a read-execute one, rewritten through the first and run again.
EXPORT long jit_dual_mapped(int rounds) {
    int fd = memfd_create("jit", 0);
    if (fd < 0) return -1;
    if (ftruncate(fd, 4096) != 0) return -2;
    unsigned char* w = (unsigned char*)mmap(0, 4096, PROT_R | PROT_W, MAP_SHARED, fd, 0);
    unsigned char* x = (unsigned char*)mmap(0, 4096, PROT_R | PROT_X, MAP_SHARED, fd, 0);
    close(fd);
    if (w == MAP_FAILED || x == MAP_FAILED) return -3;
    if (w == x) return -4;
    for (int r = 0; r < rounds; ++r) {
        const signed char k = (signed char)(r % 50 + 3);
        emit_add(w + 128, k);
        __builtin___clear_cache((char*)x, (char*)x + 4096);
        if (((AddFn)(x + 128))(10) != 10 + k) return -5;
    }
    munmap(w, 4096);
    munmap(x, 4096);
    return 1;
}
