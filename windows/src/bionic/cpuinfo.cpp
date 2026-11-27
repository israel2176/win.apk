// cpuinfo.cpp -- /proc/cpuinfo and /sys/devices/system/cpu/{possible,present,online}, as an
// x86_64 Android device's kernel shows them.
//
// Engines read these instead of asking an API: Unity's player, Unreal's and Google's
// cpu_features count the processors and look for "sse4_2", "avx2"... in the flags line; a
// game sizes its job threads by the "processor" blocks, and a CPU list it cannot read is a
// single core. Nothing on Windows has these files, so the file the path table names for
// /proc/cpuinfo was missing and every such read failed. Like /proc/<pid>/status (paths.cpp), they
// are real files under the android root, written when a path that names one is mapped: an
// open, a stat, a java.io read (the JDK's path mapper is host_path too), so native and Java
// code read one text. They are this machine's, the same for every app, written once per
// process and only when the text differs, so a reader in another app never sees a half file.
//
// The text is the kernel's (arch/x86/kernel/cpu/proc.c): one block per logical processor in
// its field order, the flags under Linux's names (arch/x86/include/asm/cpufeatures.h) for
// what CPUID reports, the topology from GetLogicalProcessorInformationEx, the clock from
// Windows' own record of it. What Windows does not tell a program is left out (microcode,
// bugs), and apicid is the processor's number: a program learns its own APIC id only by
// running on that processor. An arm64 app sees this text too: Win.apk is an x86_64 device
// whose arm64 code is translated, and no ARM "Features" line is made up for it.

#include "bionic_internal.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <intrin.h>
#include <windows.h>

namespace winapk::bionic {

namespace {

struct Regs {
    uint32_t a, b, c, d;
};

Regs cpuid(uint32_t leaf, uint32_t sub = 0) {
    int r[4];
    __cpuidex(r, static_cast<int>(leaf), static_cast<int>(sub));
    return {static_cast<uint32_t>(r[0]), static_cast<uint32_t>(r[1]), static_cast<uint32_t>(r[2]),
            static_cast<uint32_t>(r[3])};
}

struct Flag {
    uint8_t bit;
    const char* name;
};

// Linux's names, word by word in the order the kernel prints them. Bits Linux keeps to itself
// (osxsave, the reserved ones) are not listed.
constexpr Flag k1Edx[] = {
    {0, "fpu"},   {1, "vme"},    {2, "de"},     {3, "pse"},   {4, "tsc"},    {5, "msr"},     {6, "pae"},
    {7, "mce"},   {8, "cx8"},    {9, "apic"},   {11, "sep"},  {12, "mtrr"},  {13, "pge"},    {14, "mca"},
    {15, "cmov"}, {16, "pat"},   {17, "pse36"}, {18, "pn"},   {19, "clflush"}, {21, "dts"},  {22, "acpi"},
    {23, "mmx"},  {24, "fxsr"},  {25, "sse"},   {26, "sse2"}, {27, "ss"},    {28, "ht"},     {29, "tm"},
    {30, "ia64"}, {31, "pbe"},
};
constexpr Flag kExt1Edx[] = {
    {11, "syscall"}, {19, "mp"}, {20, "nx"}, {22, "mmxext"}, {25, "fxsr_opt"}, {26, "pdpe1gb"}, {27, "rdtscp"},
    {29, "lm"}, {30, "3dnowext"}, {31, "3dnow"},
};
constexpr Flag k1Ecx[] = {
    {0, "pni"},    {1, "pclmulqdq"}, {2, "dtes64"},  {3, "monitor"},  {4, "ds_cpl"},  {5, "vmx"},
    {6, "smx"},    {7, "est"},       {8, "tm2"},     {9, "ssse3"},    {10, "cid"},    {11, "sdbg"},
    {12, "fma"},   {13, "cx16"},     {14, "xtpr"},   {15, "pdcm"},    {17, "pcid"},   {18, "dca"},
    {19, "sse4_1"}, {20, "sse4_2"},  {21, "x2apic"}, {22, "movbe"},   {23, "popcnt"}, {24, "tsc_deadline_timer"},
    {25, "aes"},   {26, "xsave"},    {28, "avx"},    {29, "f16c"},    {30, "rdrand"}, {31, "hypervisor"},
};
constexpr Flag kExt1Ecx[] = {
    {0, "lahf_lm"},  {1, "cmp_legacy"}, {2, "svm"},     {3, "extapic"},       {4, "cr8_legacy"}, {5, "abm"},
    {6, "sse4a"},    {7, "misalignsse"}, {8, "3dnowprefetch"}, {9, "osvw"},   {10, "ibs"},       {11, "xop"},
    {12, "skinit"},  {13, "wdt"},       {15, "lwp"},    {16, "fma4"},         {17, "tce"},       {19, "nodeid_msr"},
    {21, "tbm"},     {22, "topoext"},   {23, "perfctr_core"}, {24, "perfctr_nb"}, {26, "bpext"}, {27, "ptsc"},
    {28, "perfctr_llc"}, {29, "mwaitx"},
};
constexpr Flag k7Ebx[] = {
    {0, "fsgsbase"},  {1, "tsc_adjust"}, {3, "bmi1"},     {4, "hle"},       {5, "avx2"},      {7, "smep"},
    {8, "bmi2"},      {9, "erms"},       {10, "invpcid"}, {11, "rtm"},      {12, "cqm"},      {14, "mpx"},
    {15, "rdt_a"},    {16, "avx512f"},   {17, "avx512dq"}, {18, "rdseed"},  {19, "adx"},      {20, "smap"},
    {21, "avx512ifma"}, {23, "clflushopt"}, {24, "clwb"}, {25, "intel_pt"}, {26, "avx512pf"}, {27, "avx512er"},
    {28, "avx512cd"}, {29, "sha_ni"},    {30, "avx512bw"}, {31, "avx512vl"},
};
constexpr Flag kXsaveEax[] = {{0, "xsaveopt"}, {1, "xsavec"}, {2, "xgetbv1"}, {3, "xsaves"}};
constexpr Flag k71Eax[] = {{4, "avx_vnni"}, {5, "avx512_bf16"}};
constexpr Flag kExt8Ebx[] = {{0, "clzero"}, {1, "irperf"}, {2, "xsaveerptr"}, {9, "wbnoinvd"}};
constexpr Flag k7Ecx[] = {
    {1, "avx512vbmi"},   {2, "umip"},        {3, "pku"},       {4, "ospke"},           {5, "waitpkg"},
    {6, "avx512_vbmi2"}, {8, "gfni"},        {9, "vaes"},      {10, "vpclmulqdq"},     {11, "avx512_vnni"},
    {12, "avx512_bitalg"}, {13, "tme"},      {14, "avx512_vpopcntdq"}, {22, "rdpid"},
    {25, "cldemote"},    {27, "movdiri"},    {28, "movdir64b"},
};
constexpr Flag k7Edx[] = {
    {2, "avx512_4vnniw"}, {3, "avx512_4fmaps"}, {4, "fsrm"},      {8, "avx512_vp2intersect"}, {10, "md_clear"},
    {14, "serialize"},    {16, "tsxldtrk"},     {18, "pconfig"},  {19, "arch_lbr"},           {22, "amx_bf16"},
    {23, "avx512_fp16"},  {24, "amx_tile"},     {25, "amx_int8"}, {28, "flush_l1d"},          {29, "arch_capabilities"},
};

template <size_t N>
void add_flags(std::string* out, uint32_t reg, const Flag (&flags)[N]) {
    for (const Flag& f : flags) {
        if (!(reg & (1u << f.bit))) continue;
        if (!out->empty()) out->push_back(' ');
        *out += f.name;
    }
}

// Where each logical processor sits: its package, its core's number in the package, and how
// many logical processors and cores that package has.
struct Place {
    int package = 0, core = 0, siblings = 1, cores = 1;
};

// A logical processor's number as the shim counts them everywhere (sched_getaffinity,
// sched_getcpu): the processors of the groups before its own, then its bit in the group.
std::vector<Place> topology(int count) {
    std::vector<Place> places(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) places[static_cast<size_t>(i)] = {0, i, count, count};
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationAll, nullptr, &len);
    if (len == 0) return places;
    std::vector<uint8_t> buf(len);
    auto* first = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data());
    if (!GetLogicalProcessorInformationEx(RelationAll, first, &len)) return places;
    std::vector<int> group_base;
    for (WORD g = 0, groups = GetActiveProcessorGroupCount(); g < groups; ++g) {
        group_base.push_back(g == 0 ? 0 : group_base.back() + static_cast<int>(GetActiveProcessorCount(g - 1)));
    }
    auto cpus_of = [&](const GROUP_AFFINITY* masks, WORD n) {
        std::vector<int> cpus;
        for (WORD m = 0; m < n; ++m) {
            if (masks[m].Group >= group_base.size()) continue;
            for (int bit = 0; bit < 64; ++bit) {
                if (!(masks[m].Mask & (KAFFINITY(1) << bit))) continue;
                const int cpu = group_base[masks[m].Group] + bit;
                if (cpu < count) cpus.push_back(cpu);
            }
        }
        return cpus;
    };
    // Packages first (cores may come before their package in the list), then each core numbered
    // within its package.
    std::vector<int> package_of(static_cast<size_t>(count), 0);
    std::vector<std::vector<int>> cores;  // logical processors per core, in the list's order
    int packages = 0;
    for (int pass = 0; pass < 2; ++pass) {
        for (DWORD off = 0; off < len;) {
            const auto* e = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data() + off);
            if (pass == 0 && e->Relationship == RelationProcessorPackage) {
                for (int cpu : cpus_of(e->Processor.GroupMask, e->Processor.GroupCount)) {
                    package_of[static_cast<size_t>(cpu)] = packages;
                }
                ++packages;
            } else if (pass == 1 && e->Relationship == RelationProcessorCore) {
                cores.push_back(cpus_of(e->Processor.GroupMask, e->Processor.GroupCount));
            }
            if (e->Size == 0) break;
            off += e->Size;
        }
    }
    if (cores.empty()) return places;
    std::vector<int> cores_in(static_cast<size_t>(packages > 0 ? packages : 1), 0);
    std::vector<int> cpus_in(cores_in.size(), 0);
    for (const std::vector<int>& core : cores) {
        if (core.empty()) continue;
        const size_t pkg = static_cast<size_t>(package_of[static_cast<size_t>(core[0])]);
        if (pkg >= cores_in.size()) continue;
        for (int cpu : core) {
            places[static_cast<size_t>(cpu)].package = static_cast<int>(pkg);
            places[static_cast<size_t>(cpu)].core = cores_in[pkg];
        }
        ++cores_in[pkg];
        cpus_in[pkg] += static_cast<int>(core.size());
    }
    for (Place& p : places) {
        p.siblings = cpus_in[static_cast<size_t>(p.package)];
        p.cores = cores_in[static_cast<size_t>(p.package)];
    }
    return places;
}

// The size in KB of the cache Linux names on the "cache size" line: the last level for Intel
// (and most others), the L2 for AMD and Hygon, whose kernel code reports that one.
unsigned cache_kb(bool amd) {
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationCache, nullptr, &len);
    if (len == 0) return 0;
    std::vector<uint8_t> buf(len);
    auto* first = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data());
    if (!GetLogicalProcessorInformationEx(RelationCache, first, &len)) return 0;
    unsigned best_level = 0, size = 0;
    for (DWORD off = 0; off < len;) {
        const auto* e = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data() + off);
        const CACHE_RELATIONSHIP& c = e->Cache;
        if (c.Type != CacheInstruction && (amd ? c.Level == 2 && size == 0 : c.Level > best_level)) {
            best_level = c.Level;
            size = c.CacheSize / 1024;
        }
        if (e->Size == 0) break;
        off += e->Size;
    }
    return size;
}

// The processor's clock in MHz: Windows' record of it (what Task Manager shows as the base
// speed), else CPUID's base frequency leaf, else the "@ 2.10GHz" an Intel brand string ends in.
unsigned clock_mhz(uint32_t max_leaf, const std::string& brand) {
    DWORD mhz = 0, size = sizeof mhz;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"~MHz",
                     RRF_RT_REG_DWORD, nullptr, &mhz, &size) == ERROR_SUCCESS && mhz) {
        return mhz;
    }
    if (max_leaf >= 0x16 && (cpuid(0x16).a & 0xffff)) return cpuid(0x16).a & 0xffff;
    const size_t at = brand.rfind('@');
    if (at == std::string::npos || brand.find("GHz", at) == std::string::npos) return 0;
    return static_cast<unsigned>(std::strtod(brand.c_str() + at + 1, nullptr) * 1000 + 0.5);
}

std::string cpuinfo_text() {
    const Regs l0 = cpuid(0);
    const uint32_t max_leaf = l0.a;
    char vendor[13] = {};
    std::memcpy(vendor, &l0.b, 4);
    std::memcpy(vendor + 4, &l0.d, 4);
    std::memcpy(vendor + 8, &l0.c, 4);
    const Regs l1 = cpuid(1);
    unsigned family = (l1.a >> 8) & 0xf, model = (l1.a >> 4) & 0xf;
    const unsigned stepping = l1.a & 0xf;
    if (family == 0xf) family += (l1.a >> 20) & 0xff;
    if (family >= 6) model += ((l1.a >> 16) & 0xf) << 4;

    const uint32_t max_ext = cpuid(0x80000000).a;
    std::string name;
    if (max_ext >= 0x80000004) {
        char brand[49] = {};
        for (uint32_t i = 0; i < 3; ++i) {
            const Regs r = cpuid(0x80000002 + i);
            std::memcpy(brand + i * 16, &r, 16);
        }
        name = brand;
        const size_t start = name.find_first_not_of(' ');
        const size_t end = name.find_last_not_of(' ');
        name = start == std::string::npos ? std::string() : name.substr(start, end - start + 1);
    }
    if (name.empty()) name = "unknown";

    const Regs ext1 = max_ext >= 0x80000001 ? cpuid(0x80000001) : Regs{};
    const Regs ext7 = max_ext >= 0x80000007 ? cpuid(0x80000007) : Regs{};
    const Regs ext8 = max_ext >= 0x80000008 ? cpuid(0x80000008) : Regs{};
    const Regs l7 = max_leaf >= 7 ? cpuid(7) : Regs{};
    const Regs l71 = max_leaf >= 7 && l7.a >= 1 ? cpuid(7, 1) : Regs{};
    const Regs xsave = max_leaf >= 0xd ? cpuid(0xd, 1) : Regs{};

    std::string flags;
    add_flags(&flags, l1.d, k1Edx);
    add_flags(&flags, ext1.d, kExt1Edx);
    // Word 3, Linux's own: the invariant TSC (constant and nonstop), and CPUID itself.
    if (ext7.d & (1u << 8)) flags += " constant_tsc";
    flags += " cpuid";
    if (ext7.d & (1u << 8)) flags += " nonstop_tsc";
    add_flags(&flags, l1.c, k1Ecx);
    add_flags(&flags, ext1.c, kExt1Ecx);
    add_flags(&flags, l7.b, k7Ebx);
    add_flags(&flags, xsave.a, kXsaveEax);
    add_flags(&flags, l71.a, k71Eax);
    add_flags(&flags, ext8.b, kExt8Ebx);
    add_flags(&flags, l7.c, k7Ecx);
    add_flags(&flags, l7.d, k7Edx);

    const bool amd = std::strcmp(vendor, "AuthenticAMD") == 0 || std::strcmp(vendor, "HygonGenuine") == 0;
    const unsigned mhz = clock_mhz(max_leaf, name), cache = cache_kb(amd);
    const unsigned clflush = ((l1.b >> 8) & 0xff) * 8;
    const unsigned phys_bits = ext8.a ? ext8.a & 0xff : 36, virt_bits = ext8.a ? (ext8.a >> 8) & 0xff : 48;

    const int count = static_cast<int>(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
    const std::vector<Place> places = topology(count > 0 ? count : 1);
    std::string text;
    char line[512];
    for (size_t i = 0; i < places.size(); ++i) {
        const Place& p = places[i];
        std::snprintf(line, sizeof line,
                      "processor\t: %zu\nvendor_id\t: %s\ncpu family\t: %u\nmodel\t\t: %u\nmodel name\t: %s\n"
                      "stepping\t: %u\ncpu MHz\t\t: %u.000\n",
                      i, vendor, family, model, name.c_str(), stepping, mhz);
        text += line;
        if (cache) {
            std::snprintf(line, sizeof line, "cache size\t: %u KB\n", cache);
            text += line;
        }
        std::snprintf(line, sizeof line,
                      "physical id\t: %d\nsiblings\t: %d\ncore id\t\t: %d\ncpu cores\t: %d\napicid\t\t: %zu\n"
                      "initial apicid\t: %zu\nfpu\t\t: yes\nfpu_exception\t: yes\ncpuid level\t: %u\nwp\t\t: yes\n",
                      p.package, p.siblings, p.core, p.cores, i, i, max_leaf);
        text += line;
        text += "flags\t\t: " + flags + "\n";
        // The kernel's bogomips on x86 is twice the TSC's clock.
        std::snprintf(line, sizeof line,
                      "bogomips\t: %u.00\nclflush size\t: %u\ncache_alignment\t: %u\n"
                      "address sizes\t: %u bits physical, %u bits virtual\npower management:\n\n",
                      mhz * 2, clflush, clflush, phys_bits, virt_bits);
        text += line;
    }
    return text;
}

// /sys/devices/system/cpu/{possible,present,online}: the kernel's CPU list, "0-7" (or "0").
std::string cpu_list_text() {
    const DWORD count = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return count > 1 ? "0-" + std::to_string(count - 1) + "\n" : std::string("0\n");
}

// Writes `text` to `file` unless it already holds exactly that, making its folders first. In
// place, not replaced, as /proc/<pid>/status is: a reader keeps its handle.
void write_if_changed(const std::wstring& file, const std::string& text) {
    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        std::string have;
        char buf[4096];
        DWORD got = 0;
        while (ReadFile(h, buf, sizeof buf, &got, nullptr) && got > 0 && have.size() <= text.size()) {
            have.append(buf, got);
        }
        CloseHandle(h);
        if (have == text) return;
    } else {
        for (size_t sep = file.find_first_of(L"\\/", 3); sep != std::wstring::npos;
             sep = file.find_first_of(L"\\/", sep + 1)) {
            CreateDirectoryW(file.substr(0, sep).c_str(), nullptr);
        }
    }
    h = CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wrote = 0;
    WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &wrote, nullptr);
    SetEndOfFile(h);
    CloseHandle(h);
}

}  // namespace

void refresh_cpu_file(const char* guest_path, const std::wstring& host) {
    if (!guest_path || host.empty()) return;
    const char* const kSysCpu = "/sys/devices/system/cpu/";
    const size_t sys_len = std::strlen(kSysCpu);
    const bool cpuinfo = std::strcmp(guest_path, "/proc/cpuinfo") == 0;
    const bool list = std::strncmp(guest_path, kSysCpu, sys_len) == 0 &&
                      (std::strcmp(guest_path + sys_len, "possible") == 0 ||
                       std::strcmp(guest_path + sys_len, "present") == 0 ||
                       std::strcmp(guest_path + sys_len, "online") == 0);
    if (!cpuinfo && !list) return;
    // Once per file and process: the text is the machine's and does not change while it runs.
    static std::mutex lock;
    static std::set<std::wstring> written;
    std::lock_guard<std::mutex> hold(lock);
    if (!written.insert(host).second) return;
    write_if_changed(host, cpuinfo ? cpuinfo_text() : cpu_list_text());
}

}  // namespace winapk::bionic
