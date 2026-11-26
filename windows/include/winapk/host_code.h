// host_code.h -- which pages of a loaded PE image are the host's own code.
//
// An arm64 library may mprotect any page it can name, and the bridge runs a page an arm64 caller made
// executable as arm64 code (arm64_abi.cpp, track_exec). On Android a library that patches or hooks libc
// does exactly that to libc's text; here "libc" is the Bionic shim, x86_64 code in winapk-runtime.exe,
// and a page of it registered as guest code is then interpreted as arm64 instructions on the next call
// into any host function on it ("unimplemented or undefined instruction" on x86 bytes), made writable
// it is overwritten with arm64 code, made non-executable it kills the host. So an arm64 caller may not
// change the protection of host code. Host code is every page of a MEM_IMAGE mapping (winapk-runtime.exe,
// Windows' DLLs, a translation's <lib>.so.aot.dll, which is LoadLibrary'd too) inside a section with
// IMAGE_SCN_MEM_EXECUTE. An image's data pages stay open to it: Facebook's force_dlopen writes an arm64
// trampoline over a string gai_strerror returned, in the exe's .rdata. Guest code is never in an image:
// the ELF loader maps a library into VirtualAlloc'd memory (MEM_PRIVATE), and mmap gives MEM_PRIVATE or
// MEM_MAPPED pages.
//
// This header reads the section table from an image's headers, with no Windows call, so that
// test/host_code_test.cpp checks it on made-up headers.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace winapk {

constexpr uint32_t kPeScnMemExecute = 0x20000000;  // IMAGE_SCN_MEM_EXECUTE

// Whether any byte of [rva_lo, rva_hi) of a PE image lies in an executable section. `head` is the image's
// base as mapped and `n` how many bytes of it may be read (the headers' region). A section covers its
// virtual size (its raw size when that is larger, as the loader maps it) rounded up to the section
// alignment. Headers that cannot be read, or are not a PE image's, answer true: nothing about such an
// image says a page of it is not code. `*first` (when given) is the first executable RVA in the range.
inline bool pe_code_in(const uint8_t* head, size_t n, uint64_t rva_lo, uint64_t rva_hi, uint64_t* first = nullptr) {
    auto u16 = [&](size_t at) {
        uint16_t v;
        std::memcpy(&v, head + at, 2);
        return v;
    };
    auto u32 = [&](size_t at) {
        uint32_t v;
        std::memcpy(&v, head + at, 4);
        return v;
    };
    if (first) *first = rva_lo;
    if (rva_lo >= rva_hi) return false;
    if (!head || n < 0x40 || head[0] != 'M' || head[1] != 'Z') return true;
    const size_t nt = u32(0x3c);
    // "PE\0\0", the file header (20 bytes), then the optional header's SectionAlignment at +32 (PE32 and
    // PE32+ alike).
    if (nt > n || n - nt < 4 + 20 + 36 || std::memcmp(head + nt, "PE\0\0", 4) != 0) return true;
    const size_t file = nt + 4;
    const uint16_t sections = u16(file + 2);
    const size_t optional = file + 20;
    const size_t table = optional + u16(file + 16);
    uint64_t align = u32(optional + 32);
    if (align < 512 || (align & (align - 1))) align = 4096;
    if (table > n || (n - table) / 40 < sections) return true;
    bool found = false;
    uint64_t lowest = 0;
    for (uint16_t i = 0; i < sections; ++i) {
        const size_t s = table + size_t{i} * 40;
        if (!(u32(s + 36) & kPeScnMemExecute)) continue;
        const uint64_t va = u32(s + 12);
        const uint64_t size = u32(s + 8) > u32(s + 16) ? u32(s + 8) : u32(s + 16);
        const uint64_t end = (va + size + align - 1) & ~(align - 1);
        if (end <= rva_lo || va >= rva_hi) continue;
        const uint64_t at = va > rva_lo ? va : rva_lo;
        if (!found || at < lowest) lowest = at;
        found = true;
    }
    if (found && first) *first = lowest;
    return found;
}

}  // namespace winapk
