// process_image.h -- the files an Android app's process is made of, as the
// process itself sees them: /proc/self/maps names its program and its linker,
// and both are files a reader can open. On a 64-bit-only device (what Win.apk
// presents: Build.SUPPORTED_ABIS is x86_64 and arm64-v8a, no 32-bit ABI) they
// are /system/bin/app_process64 and /apex/com.android.runtime/bin/linker64,
// with /system/bin/linker64 the same file, and there is no /system/bin/linker.
//
// Code reads them to learn the process's architecture: app packers choose
// which build of their native loader to load by them (NetEase's reads
// app_process64's ELF header from the line /proc/self/maps names; ijiami's
// reads linker64's header and looks for "/bin/linker64" in /proc/self/maps).
// Without them both took the 32-bit build, which Win.apk cannot load.
//
// Each is an ELF header of the host's machine (x86_64), the same bytes in the
// file and in the memory the maps line names, so a reader that opens either
// finds an ELF it can parse and nothing to look up in it: no program or section
// headers. Win.apk's linker is the process's linker; the program is winapk-runtime.

#pragma once

#include <array>
#include <cstdint>
#include <cstring>

namespace winapk {

constexpr const char* kAppProcessPath = "/system/bin/app_process64";
constexpr const char* kRuntimeLinkerPath = "/apex/com.android.runtime/bin/linker64";
constexpr const char* kSystemLinkerPath = "/system/bin/linker64";

// Whether `path` is one of the files above (exactly; no normalization).
inline bool is_process_image_path(const char* path) {
    return path && (std::strcmp(path, kAppProcessPath) == 0 || std::strcmp(path, kRuntimeLinkerPath) == 0 ||
                    std::strcmp(path, kSystemLinkerPath) == 0);
}

// An ELF64 header: little-endian, ET_DYN, EM_X86_64, current version, no
// program headers, no section headers.
inline std::array<uint8_t, 64> process_image_header() {
    std::array<uint8_t, 64> h{};
    const uint8_t ident[] = {0x7f, 'E', 'L', 'F', 2 /* ELFCLASS64 */, 1 /* ELFDATA2LSB */, 1 /* EV_CURRENT */};
    std::memcpy(h.data(), ident, sizeof ident);
    h[16] = 3;    // e_type ET_DYN
    h[18] = 62;   // e_machine EM_X86_64
    h[20] = 1;    // e_version
    h[52] = 64;   // e_ehsize
    h[54] = 56;   // e_phentsize
    h[58] = 64;   // e_shentsize
    return h;
}

}  // namespace winapk
