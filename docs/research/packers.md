# Packed and protected apps: what works, what does not, and why

Some apps ship their code packed or protected. Two kinds matter here:

- **Self-unpacking native libraries.** A library whose code is compressed or
  encrypted in the file and restored by a small loader stub when the library
  is loaded (UPX's `--android-shlib` is the common tool). The stub runs before
  any libc call and talks to the kernel itself.
- **App protectors.** A stub `Application` in Java loads a native library that
  decrypts the app's real DEX code (and often more native code) at run time,
  inspects the process (`/proc/self/maps`, `/proc/self/status`, the linker's
  and `app_process64`'s files, the auxiliary vector), and then hands the
  decrypted code to the runtime. Names seen in Win.apk's logs: `libjiagu*.so`,
  `libDexHelper*.so`, `libexec*.so`, `libshell*.so`, `libnesec.so`,
  `libsecexe.so`.

Win.apk's goal is that such an app runs as it would on a phone. It does not
defeat checks an app makes in order to refuse running; where the app's checks
see something a phone would not show, that is listed below as a difference.

## What a packed app meets first, in order

| Step | On a device | In Win.apk |
|---|---|---|
| The Java stub's `attachBaseContext` loads the protector's library | `System.loadLibrary` | Works (the loader picks the x86_64 build, else arm64) |
| The library decrypts its own `.text`, `.dynstr` and symbols in its constructors, with `mprotect` | Plain `mprotect` | Works: `dlsym` reads the symbol tables as the constructors left them (`windows/PORTING.md`, "linker"); a stack-guard load in decrypted code is patched as it faults (`fs_tls_patch.h`) |
| Raw system calls (`syscall` on x86_64, `svc #0` on arm64) instead of libc: `openat`/`read` of `/proc`, `mmap`/`mprotect`/`munmap`, `memfd_create`, `getpid` | The kernel answers | Works. **arm64**: answered by the bridge (`arm64_abi.cpp`). **x86_64**: each `SYSCALL` found is made `UD2` and answered by a handler (below) |
| `/proc/self/auxv`, `getauxval` | The kernel's auxiliary vector | Works: `/proc/self/auxv` holds the vector `getauxval` answers from, with the caller's CPU (below) |
| `/proc/self/maps`, `/system/bin/linker64`, `/system/bin/app_process64` | The process's mappings and files | Works: synthesized (`include/winapk/process_image.h`, `GuestLinker::proc_maps`), the guest libraries by their `/data/app/...` paths |
| `ptrace(PTRACE_TRACEME)`, `/proc/self/status` `TracerPid` | Refused / 0 on a device with no debugger | Works as on a device |
| The decrypted DEX goes to the runtime | ART: `DexFile`, `InMemoryDexClassLoader`, or ART's internal structures (`ArtMethod`, `DexFile` cookies, `libart.so` symbols found through `/proc/self/maps` and the file on disk) | **Public APIs work**: `DexClassLoader`, `PathClassLoader`, `InMemoryDexClassLoader`, `DexFile.loadDex`, `BaseDexClassLoader.addDexPath` convert the DEX to class files and load it. **ART's internals do not exist**: Win.apk runs Java on HotSpot, so there is no `libart.so`, no `ArtMethod` and no DEX cookie to fill in |

## Raw system calls in x86_64 code (`include/winapk/x86_syscall.h`)

On Windows, the `SYSCALL` instruction enters the Windows kernel, whose service
numbers are not Linux's. So the loader finds the `SYSCALL` instructions of
every x86_64 library and turns each into `UD2` (two bytes, as `SYSCALL`); a
vectored exception handler answers the illegal instruction at those sites as
the Linux kernel would: the result (`-errno` on failure) in `rax`, the return
address in `rcx`, the flags in `r11`, `errno` untouched. The calls are the
Bionic shim's functions under x86_64's numbering
(`windows/src/x86_syscall.cpp`, the table also behind the shim's `syscall()`). A number the shim does not have is
`-ENOSYS` and one log line:
`[win.apk] raw system call <n> (x86_64) at <address>: not implemented, -ENOSYS`.

How the instructions are found, without taking data for code:

- **At load**: by decoding instructions (prefixes, REX, VEX, EVEX, the one-,
  two- and three-byte maps, ModRM, SIB, displacements, immediates) from every
  entry point the library names: `DT_INIT`, `DT_INIT_ARRAY`, `DT_FINI(_ARRAY)`,
  its defined function symbols in `.dynsym` and `.symtab`, `e_entry`; and
  following direct jumps, branches and calls. Only bytes decoded as `SYSCALL`
  on such a path change.
- **Code written at run time** (a decrypted `.text`, an unpacked next stage),
  once a loaded library has been found to contain a `SYSCALL`: a guest `mmap` or `mprotect`
  asking for `PROT_EXEC` on private memory gets everything it asked for but
  execution. Its first run faults; the handler decodes from the faulting
  instruction, patches what it finds and makes the pages it decoded
  executable. Making the memory executable again after rewriting it starts
  that over.

Apps that never load a library with a `SYSCALL` are not changed at all: a
library with none loads byte for byte as in its file (the stack-guard patch
aside), no handler is installed, and memory a JIT makes executable is
executable at once. Only once a library with a `SYSCALL` is loaded
does the run-time scan start, and from then on each page a guest makes
executable takes one fault at its first run: about 5 microseconds a page
under Wine (`winapk-syscall-test` prints it: 2000 rounds of two fresh pages
made read-execute cost 4.1 microseconds a round with the scan off and 14.1
with it on). A JIT that toggles W^X on every write pays that per page per
toggle; one that keeps a writable and an executable mapping of one anonymous
file does not (shared file views are not scanned). The reverse is a limit: a
protector whose `SYSCALL`s are all in code it decrypts later, with none in
the file, is not scanned at run time.

An anonymous file (`memfd_create`) mapped `MAP_SHARED` with `PROT_EXEC` is
a view of the file, as on Linux, so code written through a second, writable
mapping of it runs.

Each x86_64 library in which the scan finds a `SYSCALL` logs one line at its load (a library with none logs nothing),
`[win.apk] <library>: SYSCALL scan <ms> ms, <n> instructions from <n> entries, <n> site(s)`.
Which addresses were decoded is one bit per byte of the code, so the scan
needs an eighth of the code's size and time linear in it.

What differs from a device:

- Code that reads its own instructions back (a checksum over `.text`) sees
  `0f 0b` where the file has `0f 05`, as it already saw `gs:` for `fs:` at the
  stack-guard loads.
- A `SYSCALL` reached only through an indirect jump (a jump table, a function
  pointer) into a page that is already executable is not found, and still
  enters the Windows kernel.
- Windows writes the exception record just below the stack pointer, where the
  System V ABI lets a leaf function keep up to 128 bytes (the red zone). A leaf
  that keeps locals there across an inline `SYSCALL` loses them. Moving `rsp`
  past the red zone before the fault would need the site's bytes rewritten as
  a jump to a thunk of its own (a `call` would itself write its return
  address into the red zone), placed within 2 GiB of each site, and it would
  still leave out sites whose call number is not set by the instruction just
  before them; not done.
- Every raw call costs an exception (microseconds); libc's functions do not.

Tested by `winapk-syscall-test` (`windows/test/x86_syscall_test.cpp`,
`windows/test/guest/syscall_guest.c`, `jit_guest.c`): the decoder against known encodings;
ordinary compiled code with no `SYSCALL` loaded byte for byte as in its file
with the run-time scan left off; JIT-style code (W^X rounds, a dual-mapped
anonymous file) before and after the scan is on; `0f 05` bytes that are no
instruction (jumped over, inside an immediate) left alone; then raw calls from a constructor, the kernel's register and error
conventions, raw `mmap`/`mprotect`/`munmap`, code written at run time,
across a page boundary, rewritten and made executable again, and
`/proc/self/auxv`. Under Wine a
`SYSCALL` that was not patched reaches the Linux kernel itself, so the test's
`getpid` and `getuid` checks compare with the shim's answers (Windows' process
id), which only a patched site gives.

## The auxiliary vector (`winapk::process_auxv`)

`/proc/self/auxv` (also `/proc/thread-self/auxv` and `/proc/<pid>/auxv`) and
`getauxval` give the same vector: `AT_PAGESZ` 4096, `AT_CLKTCK` 100,
`AT_PHDR`, `AT_PHENT`, `AT_PHNUM`, `AT_BASE`, `AT_ENTRY`, `AT_FLAGS`, the user
and group ids, `AT_SECURE` 0, `AT_RANDOM` (16 random bytes, made once),
`AT_HWCAP`, `AT_HWCAP2`, `AT_EXECFN` (`/system/bin/app_process64`),
`AT_PLATFORM`, and `AT_NULL` last. `getauxval` of a type it lacks is 0 with
`ENOENT`, as bionic's. The CPU entries are the caller's:

- x86_64 code: `AT_HWCAP` is CPUID leaf 1's EDX, as Linux reports it,
  `AT_HWCAP2` 0, `AT_PLATFORM` `x86_64`.
- arm64 code (its `getauxval`, `open`, `openat` and `fopen` imports and its raw
  `openat` bind to `arm64_abi.cpp`): `AT_HWCAP` `HWCAP_FP | HWCAP_ASIMD`,
  `AT_HWCAP2` 0, `AT_PLATFORM` `aarch64` (`arm64::kHwcap`), what a phone with
  no optional feature reports. The translator has no AES, PMULL or SHA
  instructions, and CRC32 and the LSE atomics, which it executes, are not
  claimed, so code choosing by them takes its plain ARMv8.0 path. An arm64
  library's IFUNC resolvers are given the same bits.

How it differs from a device:

- `AT_PHDR`, `AT_BASE` and `AT_ENTRY` name the `app_process64` and `linker64`
  images that `/proc/self/maps` lists, which are an ELF header with no program
  headers (`include/winapk/process_image.h`): `AT_PHNUM` is 0, `AT_PHDR` the
  byte after the header, and `AT_ENTRY` the image's base, where no code is.
- There is no `AT_SYSINFO_EHDR`: Win.apk has no vDSO, and libc's clock
  functions are the shim's.
- No `AT_MINSIGSTKSZ` or `AT_HWCAP3`.

Tested by `winapk-syscall-test` (x86_64: the file read by raw calls, `AT_NULL`
last, `AT_PAGESZ`, `AT_RANDOM`'s 16 bytes, `AT_PLATFORM`, every value the same
as `getauxval`'s) and `winapk-arm64-test` (arm64: the CPU entries, the file by
raw `openat` and by `fopen`, the values `getauxval` gives).

## UPX-packed libraries

Checked under Wine with a test library packed by UPX 4.2.2
(`upx --android-shlib`): the stub's raw calls are all answered
(`memfd_create`, `write`, `ftruncate`, `mmap` of the descriptor, `MAP_FIXED`
over the library's own range, `munmap`, `openat`/`read` of
`/proc/self/auxv`), its second stage is found and patched when it first runs,
and the library's code it restores is byte for byte the unpacked file's. The
library then still crashes when the stub hands over to it; the cause is not
found yet, so UPX-packed libraries are **not** listed as working. The packed
copy is not part of CI's tests.

## Dynamically loaded DEX

`DexClassLoader`, `PathClassLoader`, `DelegateLastClassLoader`,
`InMemoryDexClassLoader`, `DexFile`/`DexFile.loadDex` and
`BaseDexClassLoader.addDexPath` convert the DEX to class files on the spot
(`runtime/java/dalvik/system/MacApkDexConverter.java`) and load them, so a
protector that decrypts the app's DEX and gives it to one of these works.

Not yet: code added by changing a loader's `pathList.dexElements` by
reflection (an older protector's or a hot-fix framework's way: the elements of
a new `DexClassLoader`, or `DexPathList.makeDexElements`/`makePathElements`,
put into the app's `PathClassLoader`). `DexPathList` here holds the elements
for code that reads them, but the loader finds classes through its own list,
so classes added that way are not found (`runtime/java/dalvik/system/DexPathList.java`).

## What cannot work

Protectors that hand the decrypted code to ART's internals instead of its
public classes: they look up `libart.so` (in `/proc/self/maps` and on disk) and
call its functions, fill in an `ArtMethod`, or replace a `DexFile`'s cookie.
None of these exist on HotSpot. Such an app stops right after its protector's
library loads, with a `ClassNotFoundException` for its own classes
(`docs/APPS.md`, "Known issues"). Nothing in Win.apk can stand in for ART's
memory layout, and Win.apk does not try to bypass the protector instead. A
build of the app without the protector, where its maker offers one, runs.
