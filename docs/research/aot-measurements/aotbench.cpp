// aotbench: runs functions of an arm64 ELF .so on Linux, through the
// interpreter or through a winapk-aot translation compiled to a host .so,
// and times them against a native build.
//
//   aotbench <guest.so> <aot.so|-> <n> <name>=<vaddr> ...
//
// Prints one line per function: name, seconds, checksum.
#include "winapk/arm64_aot.h"
#include "winapk/arm64_cpu.h"

#include <dlfcn.h>
#include <elf.h>
#include <sys/mman.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

using namespace winapk::arm64;

static uint64_t g_lo, g_hi, g_bias;
static std::unordered_map<uint64_t, AotFn> g_fns;
static const uint64_t kStop = 0x10;  // never guest code

static bool g_fast = std::getenv("AOTBENCH_FASTMAP") != nullptr;
struct Slot { uint64_t pc; AotFn fn; };
static Slot g_slots[4096];
static AotFn lookup_map(uint64_t pc) {
    auto it = g_fns.find(pc);
    return it == g_fns.end() ? nullptr : it->second;
}
// A direct-mapped cache in front of the map (hits only: a miss goes to the map).
static AotFn lookup(uint64_t pc) {
    if (!g_fast) return lookup_map(pc);
    Slot& s = g_slots[(pc >> 2) & 4095];
    if (s.pc == pc) return s.fn;
    AotFn fn = lookup_map(pc);
    if (fn) s = {pc, fn};
    return fn;
}

static bool code_range(uint64_t pc, uint64_t* lo, uint64_t* hi) {
    if (pc < g_lo || pc >= g_hi) return false;
    *lo = g_lo;
    *hi = g_hi;
    return true;
}
static void host_call(Cpu& c, uint64_t target) {
    std::fprintf(stderr, "aotbench: unexpected host call to %#llx\n", (unsigned long long)target);
    std::exit(3);
}
static void fault(Cpu& c, uint32_t insn, const char* what) {
    std::fprintf(stderr, "aotbench: fault %s insn %08x at %#llx\n", what, insn, (unsigned long long)c.pc);
    std::exit(4);
}

static uint32_t rt_branch(Cpu* c, uint64_t target) {
    if (AotFn fn = lookup(target)) {
        c->pc = target;
        return fn(c);
    }
    if (target >= g_lo && target < g_hi) {
        c->pc = target;
        return kAotBail;
    }
    host_call(*c, target);
    return kAotBail;
}
static void rt_step(Cpu* c) { step(*c); }
static void* rt_lookup(uint64_t t) { return reinterpret_cast<void*>(lookup(t)); }
static uint32_t rt_plain(uint64_t) { return 0; }

// Runs from c.pc until kStop: translated functions where there are any,
// the interpreter in between.
static void run_mixed(Cpu& c) {
    while (c.pc != kStop) {
        if (AotFn fn = lookup(c.pc)) {
            fn(&c);  // kAotDone: c.pc is the return address; kAotBail: continue there
            continue;
        }
        step(c);
    }
}

int main(int argc, char** argv) {
    if (argc < 5) return 2;
    FILE* f = std::fopen(argv[1], "rb");
    if (!f) return 2;
    std::vector<uint8_t> file;
    {
        uint8_t buf[65536];
        for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) file.insert(file.end(), buf, buf + n);
        std::fclose(f);
    }
    auto* eh = reinterpret_cast<Elf64_Ehdr*>(file.data());
    auto* ph = reinterpret_cast<Elf64_Phdr*>(file.data() + eh->e_phoff);
    uint64_t span = 0;
    for (int i = 0; i < eh->e_phnum; ++i)
        if (ph[i].p_type == PT_LOAD) span = std::max<uint64_t>(span, ph[i].p_vaddr + ph[i].p_memsz);
    span = (span + 0xffff) & ~0xffffull;
    auto* mem = static_cast<uint8_t*>(mmap(nullptr, span, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    g_bias = reinterpret_cast<uint64_t>(mem);
    g_lo = ~0ull;
    g_hi = 0;
    Elf64_Dyn* dyn = nullptr;
    for (int i = 0; i < eh->e_phnum; ++i) {
        if (ph[i].p_type == PT_LOAD) {
            std::memcpy(mem + ph[i].p_vaddr, file.data() + ph[i].p_offset, ph[i].p_filesz);
            if (ph[i].p_flags & PF_X) {
                g_lo = std::min(g_lo, g_bias + ph[i].p_vaddr);
                g_hi = std::max(g_hi, g_bias + ph[i].p_vaddr + ph[i].p_memsz);
            }
        }
        if (ph[i].p_type == PT_DYNAMIC) dyn = reinterpret_cast<Elf64_Dyn*>(mem + ph[i].p_vaddr);
    }
    uint64_t rela = 0, relasz = 0;
    for (; dyn && dyn->d_tag != DT_NULL; ++dyn) {
        if (dyn->d_tag == DT_RELA) rela = dyn->d_un.d_ptr;
        if (dyn->d_tag == DT_RELASZ) relasz = dyn->d_un.d_val;
    }
    for (uint64_t o = 0; o < relasz; o += sizeof(Elf64_Rela)) {
        auto* r = reinterpret_cast<Elf64_Rela*>(mem + rela + o);
        if (ELF64_R_TYPE(r->r_info) == R_AARCH64_RELATIVE)
            *reinterpret_cast<uint64_t*>(mem + r->r_offset) = g_bias + r->r_addend;
    }

    Hooks h;
    h.code_range = code_range;
    h.host_call = host_call;
    h.fault = fault;
    set_hooks(h);

    const bool aot = std::strcmp(argv[2], "-") != 0;
    std::vector<std::string> dlls;
    for (std::string l = argv[2]; aot && !l.empty();) {
        const size_t c = l.find(',');
        dlls.push_back(l.substr(0, c));
        l = c == std::string::npos ? "" : l.substr(c + 1);
    }
    for (const std::string& dll : dlls) {
        void* so = dlopen(dll.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!so) {
            std::fprintf(stderr, "aotbench: %s\n", dlerror());
            return 2;
        }
        auto* m = static_cast<const AotModule*>(dlsym(so, "winapk_aot_module"));
        if (!m) return 2;
        *m->base = g_bias;
        m->runtime->branch = rt_branch;
        m->runtime->step = rt_step;
        m->runtime->plain = rt_plain;
        m->runtime->call_sysv = nullptr;
        if (std::getenv("AOTBENCH_LOOKUP")) reinterpret_cast<void**>(m->runtime)[4] = reinterpret_cast<void*>(rt_lookup);
        for (uint64_t i = 0; i < m->count; ++i) g_fns[g_bias + m->entries[i].vaddr] = m->entries[i].fn;
    }
    const uint64_t n = std::strtoull(argv[3], nullptr, 0);
    std::vector<uint8_t> stack(1 << 20);
    for (int a = 4; a < argc; ++a) {
        std::string arg = argv[a];
        const size_t eq = arg.find('=');
        const uint64_t entry = g_bias + std::strtoull(arg.c_str() + eq + 1, nullptr, 16);
        Cpu c;
        c.x[0] = n;
        c.x[1] = 12345;
        c.x[2] = 7;
        c.x[30] = kStop;
        c.stop_pc = kStop;
        c.sp = (reinterpret_cast<uint64_t>(stack.data()) + stack.size()) & ~15ull;
        c.pc = entry;
        const auto t0 = std::chrono::steady_clock::now();
        if (aot) run_mixed(c);
        else run(c);
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("%s %.4f %016llx\n", arg.substr(0, eq).c_str(), s, (unsigned long long)c.x[0]);
    }
    return 0;
}
