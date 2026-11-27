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
| Raw system calls (`syscall` on x86_64, `svc #0` on arm64) instead of libc: `openat`/`read` of `/proc`, `mmap`/`mprotect`/`munmap`, `memfd_create`, `getpid` | The kernel answers | **arm64**: answered by the bridge (`arm64_abi.cpp`). **x86_64**: answered since this change (below). Before it, an x86_64 `SYSCALL` entered the Windows kernel with Linux's number |
| `/proc/self/auxv`, `getauxval` | The kernel's auxiliary vector | **Since this change**: `/proc/self/auxv` exists and holds the vector `getauxval` answers from (`winapk::process_auxv`, `windows/src/linker.cpp`); before, the open failed and `getauxval` knew only `AT_PAGESZ` |
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
(`windows/src/x86_syscall.cpp`, the table also behind the shim's `syscall()`,
which used to answer only 17 numbers). A number the shim does not have is
`-ENOSYS` and one log line:
`[win.apk] raw system call <n> (x86_64) at <address>: not implemented, -ENOSYS`.

How the instructions are found, without taking data for code:

- **At load**: by decoding instructions (prefixes, REX, VEX, EVEX, the one-,
  two- and three-byte maps, ModRM, SIB, displacements, immediates) from every
  entry point the library names: `DT_INIT`, `DT_INIT_ARRAY`, `DT_FINI(_ARRAY)`,
  its defined function symbols in `.dynsym` and `.symtab`, `e_entry`; and
  following direct jumps, branches and calls. Only bytes decoded as `SYSCALL`
  on such a path change.
- **Code written at run time** (a decrypted `.text`, an unpacked next stage):
  while x86_64 guest code runs in the process, a guest `mmap` or `mprotect`
  asking for `PROT_EXEC` on private memory gets everything it asked for but
  execution. Its first run faults; the handler decodes from the faulting
  instruction, patches what it finds and makes the pages it decoded
  executable. Making the memory executable again after rewriting it starts
  that over. Processes with only arm64 libraries never take this path.

What differs from a device:

- Code that reads its own instructions back (a checksum over `.text`) sees
  `0f 0b` where the file has `0f 05`, as it already saw `gs:` for `fs:` at the
  stack-guard loads.
- A `SYSCALL` reached only through an indirect jump (a jump table, a function
  pointer) into a page that is already executable is not found, and still
  enters the Windows kernel.
- Windows writes the exception record just below the stack pointer, where the
  System V ABI lets a leaf function keep up to 128 bytes (the red zone). A leaf
  that keeps locals there across an inline `SYSCALL` loses them.
- Every raw call costs an exception (microseconds); libc's functions do not.

Tested by `winapk-syscall-test` (`windows/test/x86_syscall_test.cpp`,
`windows/test/guest/syscall_guest.c`): the decoder against known encodings,
then raw calls from a constructor, the kernel's register and error
conventions, raw `mmap`/`mprotect`/`munmap`, and code written at run time,
across a page boundary, rewritten and made executable again. Under Wine a
`SYSCALL` that was not patched reaches the Linux kernel itself, so the test's
`getpid` and `getuid` checks compare with the shim's answers (Windows' process
id), which only a patched site gives.

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
