// host_code_test.cpp -- winapk-host-code-test: which pages of a PE image are host code
// (include/winapk/host_code.h), on made-up headers. No file and no Windows call.
//
//   winapk-host-code-test
//
// An image of headers, .text (executable), .rdata and .data: a range in .text, one reaching into it from
// either side, its last partial page, the data sections and the headers alone, the first executable
// address reported; a section whose raw size is larger than its virtual size; headers that are not a PE
// image's, or cut short, count as code.

#include <cstdio>
#include <cstring>
#include <vector>

#include "winapk/host_code.h"

namespace {

int g_failures = 0;

void check(const char* what, bool ok) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

struct Section {
    const char* name;
    uint32_t va, virtual_size, raw_size, flags;
};

constexpr uint32_t kCode = 0x00000020, kRead = 0x40000000, kWrite = 0x80000000, kData = 0x00000040;

// DOS header, "PE\0\0", the file header and a PE32+ optional header (240 bytes), then the section table.
std::vector<uint8_t> image(const std::vector<Section>& sections, uint32_t align = 0x1000) {
    std::vector<uint8_t> h(0x1000, 0);
    auto put16 = [&](size_t at, uint16_t v) { std::memcpy(&h[at], &v, 2); };
    auto put32 = [&](size_t at, uint32_t v) { std::memcpy(&h[at], &v, 4); };
    h[0] = 'M';
    h[1] = 'Z';
    const size_t nt = 0x80;
    put32(0x3c, nt);
    std::memcpy(&h[nt], "PE\0\0", 4);
    const size_t file = nt + 4;
    put16(file, 0x8664);
    put16(file + 2, static_cast<uint16_t>(sections.size()));
    put16(file + 16, 240);
    const size_t optional = file + 20;
    put16(optional, 0x20b);
    put32(optional + 32, align);
    size_t s = optional + 240;
    for (const Section& sec : sections) {
        std::memcpy(&h[s], sec.name, std::strlen(sec.name));
        put32(s + 8, sec.virtual_size);
        put32(s + 12, sec.va);
        put32(s + 16, sec.raw_size);
        put32(s + 36, sec.flags);
        s += 40;
    }
    return h;
}

}  // namespace

int main() {
    using winapk::kPeScnMemExecute;
    using winapk::pe_code_in;
    std::printf("host code (host_code.h)\n");

    // .text 0x1000..0x3800 (pages to 0x4000), .rdata 0x4000.., .data 0x6000..
    const auto h = image({{".text", 0x1000, 0x2800, 0x2a00, kCode | kPeScnMemExecute | kRead},
                          {".rdata", 0x4000, 0x1200, 0x1400, kData | kRead},
                          {".data", 0x6000, 0x300, 0x200, kData | kRead | kWrite}});
    uint64_t at = 0;
    check("a page in .text is code", pe_code_in(h.data(), h.size(), 0x2000, 0x3000, &at) && at == 0x2000);
    check(".text's last, partial page is code", pe_code_in(h.data(), h.size(), 0x3000, 0x4000, &at) && at == 0x3000);
    check("a range from the headers into .text: code, first at .text",
          pe_code_in(h.data(), h.size(), 0x0, 0x2000, &at) && at == 0x1000);
    check("a range from .text into .rdata: code", pe_code_in(h.data(), h.size(), 0x3000, 0x5000, &at) && at == 0x3000);
    check(".rdata is not code (force_dlopen's trampoline over gai_strerror's string)",
          !pe_code_in(h.data(), h.size(), 0x4000, 0x5000));
    check(".data is not code", !pe_code_in(h.data(), h.size(), 0x6000, 0x7000));
    check("the headers are not code", !pe_code_in(h.data(), h.size(), 0x0, 0x1000));
    check("past the last section is not code", !pe_code_in(h.data(), h.size(), 0x8000, 0x9000));
    check("an empty range is not code", !pe_code_in(h.data(), h.size(), 0x2000, 0x2000));

    // A section mapped by its raw size when that is the larger (and a second executable section).
    const auto raw = image({{".text", 0x1000, 0x800, 0x1800, kCode | kPeScnMemExecute | kRead},
                            {".rdata", 0x3000, 0x100, 0x200, kData | kRead},
                            {".text2", 0x4000, 0x100, 0x200, kPeScnMemExecute | kRead}});
    check("an executable section's raw size counts", pe_code_in(raw.data(), raw.size(), 0x2000, 0x3000));
    check("a second executable section is code", pe_code_in(raw.data(), raw.size(), 0x3000, 0x5000, &at) &&
                                                     at == 0x4000);
    // Aligned to 64 KB: the section reaches its alignment.
    const auto wide = image({{".text", 0x10000, 0x100, 0x200, kCode | kPeScnMemExecute | kRead}}, 0x10000);
    check("a section reaches its alignment", pe_code_in(wide.data(), wide.size(), 0x1f000, 0x20000) &&
                                                 !pe_code_in(wide.data(), wide.size(), 0x20000, 0x21000));

    // Headers that tell nothing: everything is code.
    std::vector<uint8_t> junk(0x1000, 0x41);
    check("not an MZ image: code", pe_code_in(junk.data(), junk.size(), 0x4000, 0x5000));
    auto no_pe = h;
    no_pe[0x80] = 'X';
    check("no PE signature: code", pe_code_in(no_pe.data(), no_pe.size(), 0x4000, 0x5000));
    check("headers cut short of the section table: code", pe_code_in(h.data(), 0x100, 0x4000, 0x5000));
    check("no headers readable: code", pe_code_in(h.data(), 0, 0x4000, 0x5000));
    check("no headers at all: code", pe_code_in(nullptr, 0, 0x4000, 0x5000));
    auto far = h;
    const uint32_t beyond = 0x2000;
    std::memcpy(&far[0x3c], &beyond, 4);
    check("e_lfanew past the headers: code", pe_code_in(far.data(), far.size(), 0x4000, 0x5000));

    std::printf("%s (%d failed)\n", g_failures ? "FAILED" : "OK", g_failures);
    return g_failures;
}
