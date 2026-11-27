/*
 * exec_guest.c -- the Android program winapk-exec-test starts as a child process (an x86_64 PIE
 * executable, built by build_guest.sh with no NDK: its _start is crtbegin's, below, calling
 * __libc_init as Android's does).
 *
 * It writes "argc=<n>", each argument, and the variable WINAPK_EXEC_TEST to stdout, a line to
 * stderr, then copies stdin to stdout upper-cased, and exits with 7 (or with the number its first
 * argument after argv[0] gives, "exit=N").
 */
typedef unsigned long size_t;
typedef long ssize_t;

extern ssize_t write(int fd, const void* buf, size_t n);
extern ssize_t read(int fd, void* buf, size_t n);
extern char* getenv(const char* name);
extern size_t strlen(const char* s);
extern void __libc_init(void* raw_args, void (*onexit)(void), int (*slingshot)(int, char**, char**),
                        void* structors);

static void put(int fd, const char* s) { write(fd, s, strlen(s)); }

static int number(const char* s) {
    int n = 0;
    while (*s >= '0' && *s <= '9') n = n * 10 + (*s++ - '0');
    return n;
}

int main(int argc, char** argv, char** envp) {
    (void)envp;
    char digits[4] = {(char)('0' + argc % 10), '\n', 0, 0};
    put(1, "argc=");
    put(1, digits);
    for (int i = 0; i < argc; ++i) {
        put(1, "argv:");
        put(1, argv[i]);
        put(1, "\n");
    }
    const char* v = getenv("WINAPK_EXEC_TEST");
    put(1, "env:");
    put(1, v ? v : "(none)");
    put(1, "\n");
    put(2, "to stderr\n");
    char buf[256];
    ssize_t n;
    while ((n = read(0, buf, sizeof buf)) > 0) {
        for (ssize_t i = 0; i < n; ++i) {
            if (buf[i] >= 'a' && buf[i] <= 'z') buf[i] = (char)(buf[i] - 'a' + 'A');
        }
        write(1, buf, (size_t)n);
    }
    if (argc > 1 && argv[1][0] == 'e' && argv[1][1] == 'x' && argv[1][4] == '=') return number(argv[1] + 5);
    return 7;
}

static void* structors[3];

__attribute__((used)) static void start_main(void* raw_args) { __libc_init(raw_args, 0, main, structors); }

__asm__(
    ".text\n"
    ".globl _start\n"
    "_start:\n"
    "    xorl %ebp, %ebp\n"
    "    movq %rsp, %rdi\n"
    "    andq $-16, %rsp\n"
    "    call start_main\n"
    "    hlt\n");
