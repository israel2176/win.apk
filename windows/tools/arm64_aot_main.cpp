// arm64_aot_main.cpp -- winapk-aot: translates an arm64-v8a .so into C ahead
// of time, for the installer to compile into `<lib>.so.aot.dll` (see
// include/winapk/arm64_aot.h for how the runtime uses it).
//
//   winapk-aot <lib.so> <out.c>
//
// Functions are found from the symbol tables and, transitively, from the
// targets of BL; each is decoded by following its branches. In the C, every
// guest register a function touches is a local variable, loaded from the
// Cpu on entry and stored back only where something else may look at it:
// calls, returns, and the instructions handed to the interpreter. Decoding
// follows the interpreter (src/arm64/arm64_cpu.cpp) case by case, so the two
// can be checked against each other; the differential test runs the same
// libraries through both.
//
// Natively translated: the integer instructions, branches, and loads and
// stores of general and SIMD&FP registers. Everything else (floating-point
// and SIMD arithmetic, exclusives and atomics, most system registers) goes
// through the interpreter, one instruction at a time, which is correct but
// slow; the summary on stderr counts those, so the next ones to translate are
// always known.

#include "winapk/arm64_aot.h"

#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

using winapk::arm64::aot_hash;

// ---- ELF ---------------------------------------------------------------------

struct Ehdr64 {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};
struct Phdr64 {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
};
struct Shdr64 {
    uint32_t name, type;
    uint64_t flags, addr, offset, size;
    uint32_t link, info;
    uint64_t addralign, entsize;
};
struct Sym64 {
    uint32_t name;
    uint8_t info, other;
    uint16_t shndx;
    uint64_t value, size;
};

struct Image {
    std::vector<uint8_t> file;
    struct Seg {
        uint64_t vaddr, size, offset;
    };
    std::vector<Seg> exec;
    uint64_t text_hash = 0;
    std::map<uint64_t, uint64_t> symbols;  // function entry -> size (0: unknown)

    bool code(uint64_t va) const {
        for (const Seg& s : exec) {
            if (va >= s.vaddr && va + 4 <= s.vaddr + s.size) return true;
        }
        return false;
    }
    uint32_t insn(uint64_t va) const {
        for (const Seg& s : exec) {
            if (va >= s.vaddr && va + 4 <= s.vaddr + s.size) {
                uint32_t w;
                std::memcpy(&w, file.data() + s.offset + (va - s.vaddr), 4);
                return w;
            }
        }
        return 0;
    }
};

template <typename T> bool read_at(const std::vector<uint8_t>& f, uint64_t off, T* out) {
    if (off > f.size() || f.size() - off < sizeof(T)) return false;
    std::memcpy(out, f.data() + off, sizeof(T));
    return true;
}

bool read_image(const char* path, Image* img, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        *error = std::string("cannot open ") + path;
        return false;
    }
    img->file.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    Ehdr64 eh;
    if (!read_at(img->file, 0, &eh) || std::memcmp(eh.ident, "\x7f" "ELF", 4) != 0 || eh.ident[4] != 2 ||
        eh.machine != 183) {
        *error = "not a 64-bit arm64 ELF file";
        return false;
    }
    uint64_t h = 0xcbf29ce484222325ull;
    for (unsigned i = 0; i < eh.phnum; ++i) {
        Phdr64 ph;
        if (!read_at(img->file, eh.phoff + uint64_t(i) * eh.phentsize, &ph)) break;
        if (ph.type != 1 || !(ph.flags & 1) || ph.filesz == 0) continue;
        if (ph.offset + ph.filesz > img->file.size()) {
            *error = "truncated executable segment";
            return false;
        }
        img->exec.push_back({ph.vaddr, ph.filesz, ph.offset});
        h = aot_hash(img->file.data() + ph.offset, ph.filesz, h);
    }
    img->text_hash = h;
    if (img->exec.empty()) {
        *error = "no executable segment";
        return false;
    }
    // Function symbols from .symtab and .dynsym (a stripped library still
    // has the exported ones; BL targets find the rest).
    for (unsigned i = 0; i < eh.shnum; ++i) {
        Shdr64 sh;
        if (!read_at(img->file, eh.shoff + uint64_t(i) * eh.shentsize, &sh)) break;
        if (sh.type != 2 && sh.type != 11) continue;  // SHT_SYMTAB, SHT_DYNSYM
        for (uint64_t off = 0; off + sizeof(Sym64) <= sh.size; off += sizeof(Sym64)) {
            Sym64 s;
            if (!read_at(img->file, sh.offset + off, &s)) break;
            if ((s.info & 0xf) != 2 || s.shndx == 0 || !img->code(s.value)) continue;  // STT_FUNC, defined
            uint64_t& size = img->symbols[s.value];
            if (s.size > size) size = s.size;
        }
    }
    return true;
}

// ---- decoding helpers (as in src/arm64/arm64_internal.h) ----------------------

uint32_t field(uint32_t insn, unsigned hi, unsigned lo) {
    return (insn >> lo) & ((hi - lo == 31) ? ~0u : ((1u << (hi - lo + 1)) - 1));
}
bool bit(uint32_t insn, unsigned b) { return (insn >> b) & 1; }
uint64_t mask(unsigned n) { return n >= 64 ? ~uint64_t{0} : ((uint64_t{1} << n) - 1); }
int64_t sext(uint64_t v, unsigned bits) {
    return bits >= 64 ? static_cast<int64_t>(v) : static_cast<int64_t>(v << (64 - bits)) >> (64 - bits);
}
uint64_t ror(uint64_t v, unsigned r, unsigned size) {
    v &= mask(size);
    r %= size;
    if (r == 0) return v;
    return ((v >> r) | (v << (size - r))) & mask(size);
}

bool decode_bit_masks(unsigned n, unsigned imms, unsigned immr, bool immediate, unsigned datasize, uint64_t* wmask,
                      uint64_t* tmask) {
    const unsigned combined = (n << 6) | (~imms & 0x3f);
    if (combined == 0) return false;
    unsigned len = 31 - __builtin_clz(combined);
    if (len < 1) return false;
    const unsigned levels = (1u << len) - 1;
    if (immediate && (imms & levels) == levels) return false;
    const unsigned s = imms & levels, r = immr & levels;
    const unsigned esize = 1u << len;
    if (esize > datasize) return false;
    const unsigned d = (s - r) & levels;
    const uint64_t welem = mask(s + 1), telem = mask(d + 1);
    uint64_t w = ror(welem, r, esize), t = telem;
    for (unsigned i = esize; i < 64; i *= 2) {
        w |= w << i;
        t |= t << i;
    }
    *wmask = w & mask(datasize);
    *tmask = t & mask(datasize);
    return true;
}

// ---- control flow ----------------------------------------------------------------

enum class Flow { kSeq, kCond, kB, kBL, kBLR, kBR, kRet };

Flow classify(uint32_t insn, uint64_t va, uint64_t* target) {
    if ((insn & 0xff000010) == 0x54000000) {
        *target = va + static_cast<uint64_t>(sext(field(insn, 23, 5), 19) << 2);
        return Flow::kCond;
    }
    if ((insn & 0x7c000000) == 0x14000000) {
        *target = va + static_cast<uint64_t>(sext(field(insn, 25, 0), 26) << 2);
        return bit(insn, 31) ? Flow::kBL : Flow::kB;
    }
    if ((insn & 0x7e000000) == 0x34000000) {
        *target = va + static_cast<uint64_t>(sext(field(insn, 23, 5), 19) << 2);
        return Flow::kCond;
    }
    if ((insn & 0x7e000000) == 0x36000000) {
        *target = va + static_cast<uint64_t>(sext(field(insn, 18, 5), 14) << 2);
        return Flow::kCond;
    }
    if ((insn & 0xfe000000) == 0xd6000000) {
        const unsigned opc = field(insn, 24, 21);
        if (field(insn, 20, 16) != 0x1f || (opc > 2 && opc != 8 && opc != 9)) return Flow::kSeq;
        switch (opc & 3) {
            case 0: return Flow::kBR;
            case 1: return Flow::kBLR;
            default: return Flow::kRet;
        }
    }
    return Flow::kSeq;
}

struct Function {
    uint64_t entry = 0, end = 0;  // end 0: size unknown
    std::set<uint64_t> insns, labels;
    std::set<uint64_t> falls_out;  // instructions whose fallthrough leaves the function
};

constexpr size_t kMaxInsns = 100000;

// Follows the branches of the function at f->entry. BL targets go to `calls`.
bool discover(const Image& img, const std::set<uint64_t>& entries, Function* f, std::set<uint64_t>* calls) {
    auto inside = [&](uint64_t va) {
        if (!img.code(va)) return false;
        if (f->end) return va >= f->entry && va < f->end;
        return va == f->entry || !entries.count(va);  // another function's entry is a tail call
    };
    std::vector<uint64_t> work = {f->entry};
    f->labels.insert(f->entry);
    while (!work.empty()) {
        uint64_t va = work.back();
        work.pop_back();
        for (;;) {
            if (f->insns.count(va)) break;
            if (!inside(va)) {
                f->falls_out.insert(va - 4);
                break;
            }
            if (f->insns.size() >= kMaxInsns) return false;
            f->insns.insert(va);
            uint64_t t = 0;
            const Flow flow = classify(img.insn(va), va, &t);
            if (flow == Flow::kCond || flow == Flow::kB) {
                if (inside(t) && !f->labels.count(t)) {
                    f->labels.insert(t);
                    work.push_back(t);
                }
            }
            if (flow == Flow::kBL && img.code(t)) calls->insert(t);
            if (flow == Flow::kB || flow == Flow::kBR || flow == Flow::kRet) break;
            va += 4;
        }
    }
    return true;
}

// ---- C emission -----------------------------------------------------------------

std::string fmt(const char* f, ...) {
    va_list ap, ap2;
    va_start(ap, f);
    va_copy(ap2, ap);
    const int n = std::vsnprintf(nullptr, 0, f, ap);
    va_end(ap);
    std::string out(n > 0 ? static_cast<size_t>(n) : 0, '\0');
    std::vsnprintf(out.data(), out.size() + 1, f, ap2);
    va_end(ap2);
    return out;
}

std::string hex(uint64_t v) { return fmt("0x%" PRIx64 "ull", v); }

const char* kPrelude = R"(// Generated by winapk-aot. Compile with clang -O2 -shared -fno-strict-aliasing.
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t x[31];
    uint64_t sp, pc;
    uint32_t nzcv, fpcr, fpsr;
    uint64_t tpidr;
    _Alignas(16) uint64_t v[32][2];
} Cpu;
typedef struct { uint32_t (*branch)(Cpu* c, uint64_t target); void (*step)(Cpu* c); } AotRuntime;
typedef struct { uint64_t vaddr; uint32_t (*fn)(Cpu* c); } AotEntry;
typedef struct {
    uint32_t version, layout[4];
    uint64_t text_hash;
    uint64_t* base;
    AotRuntime* runtime;
    const AotEntry* entries;
    uint64_t count;
} AotModule;

static uint64_t aot_base;
static AotRuntime aot_rt;

// Guest memory: unaligned, and volatile so that the C compiler keeps every
// access, in order, as the guest's own synchronization expects.
typedef volatile uint8_t vu8;
typedef volatile uint16_t __attribute__((aligned(1))) vu16;
typedef volatile uint32_t __attribute__((aligned(1))) vu32;
typedef volatile uint64_t __attribute__((aligned(1))) vu64;
#define LD8(a) (*(vu8*)(uintptr_t)(a))
#define LD16(a) (*(vu16*)(uintptr_t)(a))
#define LD32(a) (*(vu32*)(uintptr_t)(a))
#define LD64(a) (*(vu64*)(uintptr_t)(a))
#define ST8(a, v) (*(vu8*)(uintptr_t)(a) = (uint8_t)(v))
#define ST16(a, v) (*(vu16*)(uintptr_t)(a) = (uint16_t)(v))
#define ST32(a, v) (*(vu32*)(uintptr_t)(a) = (uint32_t)(v))
#define ST64(a, v) (*(vu64*)(uintptr_t)(a) = (uint64_t)(v))

#define INL static inline __attribute__((always_inline))
INL uint64_t ror64(uint64_t v, unsigned r) { r &= 63; return r ? (v >> r) | (v << (64 - r)) : v; }
INL uint64_t ror32(uint64_t v, unsigned r) { uint32_t w = (uint32_t)v; r &= 31; return r ? (uint32_t)((w >> r) | (w << (32 - r))) : w; }
INL uint64_t asr64(uint64_t v, unsigned s) { return (uint64_t)((int64_t)v >> s); }
INL uint64_t asr32(uint64_t v, unsigned s) { return (uint32_t)((int32_t)(uint32_t)v >> s); }
// AddWithCarry(), setting NZCV.
INL uint64_t addf64(uint64_t x, uint64_t y, uint64_t cin, uint8_t* n, uint8_t* z, uint8_t* c, uint8_t* v) {
    const unsigned __int128 u = (unsigned __int128)x + y + cin;
    const uint64_t r = (uint64_t)u;
    *n = r >> 63; *z = r == 0; *c = (uint8_t)(u >> 64); *v = (((x ^ r) & (y ^ r)) >> 63) & 1;
    return r;
}
INL uint64_t addf32(uint64_t x64, uint64_t y64, uint64_t cin, uint8_t* n, uint8_t* z, uint8_t* c, uint8_t* v) {
    const uint32_t x = (uint32_t)x64, y = (uint32_t)y64;
    const uint64_t u = (uint64_t)x + y + cin;
    const uint32_t r = (uint32_t)u;
    *n = r >> 31; *z = r == 0; *c = (uint8_t)(u >> 32); *v = (((x ^ r) & (y ^ r)) >> 31) & 1;
    return r;
}
INL uint64_t clz64(uint64_t v) { return v ? (uint64_t)__builtin_clzll(v) : 64; }
INL uint64_t clz32(uint64_t v) { return (uint32_t)v ? (uint64_t)__builtin_clz((uint32_t)v) : 32; }
INL uint64_t rev16_64(uint64_t v) { return ((v & 0x00ff00ff00ff00ffull) << 8) | ((v >> 8) & 0x00ff00ff00ff00ffull); }
INL uint64_t rev32_64(uint64_t v) { return (uint64_t)__builtin_bswap32((uint32_t)v) | ((uint64_t)__builtin_bswap32((uint32_t)(v >> 32)) << 32); }
INL uint64_t udiv64(uint64_t a, uint64_t b) { return b ? a / b : 0; }
INL uint64_t udiv32(uint64_t a, uint64_t b) { return (uint32_t)b ? (uint32_t)a / (uint32_t)b : 0; }
INL uint64_t sdiv64(uint64_t a, uint64_t b) { return b == 0 ? 0 : b == ~0ull ? 0 - a : (uint64_t)((int64_t)a / (int64_t)b); }
INL uint64_t sdiv32(uint64_t a, uint64_t b) {
    const int32_t x = (int32_t)a, y = (int32_t)b;
    return y == 0 ? 0 : y == -1 ? (uint32_t)(0u - (uint32_t)x) : (uint32_t)(x / y);
}
)";

class Emitter {
public:
    Emitter(const Image& img, const Function& f, const std::set<uint64_t>& translated)
        : img_(img), f_(f), translated_(translated) {}

    std::string function(size_t* interpreted);

private:
    // Register access. x31 reads as zero (XZR) or as SP, by operand.
    std::string rx(unsigned n) {
        if (n == 31) return "0ull";
        used_x_ |= uint64_t{1} << n;
        return fmt("X%u", n);
    }
    std::string rxsp(unsigned n) {
        if (n == 31) {
            used_x_ |= uint64_t{1} << 31;
            return "SP";
        }
        return rx(n);
    }
    std::string reg(unsigned n) {  // x31 as SP, for writes
        used_x_ |= uint64_t{1} << n;
        written_x_ |= uint64_t{1} << n;
        return n == 31 ? "SP" : fmt("X%u", n);
    }
    void wx(unsigned n, const std::string& v, bool sf) {
        if (n == 31) {  // XZR: only the side effects (the flags of CMP, CMN, TST)
            line("(void)(" + v + ");");
            return;
        }
        line(reg(n) + (sf ? " = (uint64_t)(" + v + ");" : " = (uint64_t)(uint32_t)(" + v + ");"));
    }
    void wxsp(unsigned n, const std::string& v, bool sf) {
        line(reg(n) + (sf ? " = (uint64_t)(" + v + ");" : " = (uint64_t)(uint32_t)(" + v + ");"));
    }
    std::string vl(unsigned n) {
        used_v_ |= 1u << n;
        return fmt("V%uL", n);
    }
    std::string vh(unsigned n) {
        used_v_ |= 1u << n;
        return fmt("V%uH", n);
    }
    void wv(unsigned n, const std::string& lo, const std::string& hi) {
        used_v_ |= 1u << n;
        written_v_ |= 1u << n;
        line(fmt("V%uL = (uint64_t)(%s); V%uH = (uint64_t)(%s);", n, lo.c_str(), n, hi.c_str()));
    }
    const char* flags() {
        flags_written_ = true;
        return "&FN, &FZ, &FC, &FV";
    }
    std::string cond(unsigned c) {
        flags_used_ = true;
        std::string r;
        switch (c >> 1) {
            case 0: r = "FZ"; break;
            case 1: r = "FC"; break;
            case 2: r = "FN"; break;
            case 3: r = "FV"; break;
            case 4: r = "(FC && !FZ)"; break;
            case 5: r = "(FN == FV)"; break;
            case 6: r = "(FN == FV && !FZ)"; break;
            default: return "1";
        }
        return (c & 1) ? "!" + r : r;
    }
    std::string addr(uint64_t va) { return "(B + " + hex(va) + ")"; }
    void line(const std::string& s) { body_ += "    " + s + "\n"; }

    std::string shift(const std::string& v, unsigned type, unsigned amount, bool sf) {
        const unsigned size = sf ? 64 : 32;
        amount %= size;
        const std::string m = sf ? "" : "(uint32_t)";
        switch (type) {
            case 0: return fmt("(uint64_t)%s(%s << %u)", m.c_str(), v.c_str(), amount);
            case 1: return fmt("((uint64_t)%s(%s) >> %u)", m.c_str(), v.c_str(), amount);
            case 2: return fmt("asr%u(%s, %u)", size, v.c_str(), amount);
            default: return fmt("ror%u(%s, %u)", size, v.c_str(), amount);
        }
    }
    std::string extend(const std::string& v, unsigned option, unsigned sh) {
        static const char* kCasts[] = {"(uint8_t)", "(uint16_t)", "(uint32_t)", "",
                                        "(int64_t)(int8_t)", "(int64_t)(int16_t)", "(int64_t)(int32_t)", ""};
        return fmt("((uint64_t)%s(%s) << %u)", kCasts[option & 7], v.c_str(), sh);
    }

    // Writes back what others may read: before calls (flags excluded: the
    // AAPCS64 does not keep them across calls) and with the flags before the
    // interpreter or a jump that may leave the function.
    std::string flush(bool with_flags) { return with_flags ? "FLUSH_ALL();" : "FLUSH();"; }
    // A jump to `t` outside this function: a tail call, or the runtime.
    std::string exit_str(uint64_t t) {
        if (translated_.count(t)) return flush(false) + fmt(" return F_%" PRIx64 "(c);", t);
        return flush(true) + " c->pc = " + addr(t) + "; return aot_rt.branch(c, " + addr(t) + ");";
    }
    void exit_to(uint64_t t) { line(exit_str(t)); }
    void call_to(const std::string& target_expr, uint64_t direct, uint64_t ret) {
        wx(30, addr(ret), true);
        line("{");
        line("    const uint64_t t = " + target_expr + ";");
        line("    " + flush(false));
        if (direct && translated_.count(direct)) {
            line(fmt("    const uint32_t st = F_%" PRIx64 "(c);", direct));
        } else {
            line("    const uint32_t st = aot_rt.branch(c, t);");
        }
        line("    if (st) return st;");
        line("    RELOAD_CALL();");
        line("}");
        reload_call_used_ = true;
    }
    void interpret(uint64_t va) {
        ++interpreted_;
        line(flush(true) + " c->pc = " + addr(va) + "; aot_rt.step(c); RELOAD_ALL();");
        line("if (c->pc != " + addr(va + 4) + ") return 1;");
    }

    bool branch(uint64_t va, uint32_t insn);
    bool dp_immediate(uint32_t insn, uint64_t va);
    bool dp_register(uint32_t insn);
    bool loads_stores(uint32_t insn, uint64_t va);
    bool ldst_register(uint32_t insn);
    bool ldst_pair(uint32_t insn);
    bool system(uint32_t insn);

    const Image& img_;
    const Function& f_;
    const std::set<uint64_t>& translated_;
    std::string body_;
    uint64_t used_x_ = 0, written_x_ = 0;  // bit 31: SP
    uint32_t used_v_ = 0, written_v_ = 0;
    bool flags_used_ = false, flags_written_ = false, reload_call_used_ = false;
    size_t interpreted_ = 0;
};

bool Emitter::branch(uint64_t va, uint32_t insn) {
    uint64_t t = 0;
    const Flow flow = classify(insn, va, &t);
    auto go = [&](uint64_t target) {
        if (f_.labels.count(target)) return fmt("goto L_%" PRIx64 ";", target);
        return "{ " + exit_str(target) + " }";  // out of the function
    };
    switch (flow) {
        case Flow::kSeq:
            return false;
        case Flow::kCond: {
            std::string c;
            if ((insn & 0xff000010) == 0x54000000) {
                c = cond(field(insn, 3, 0));
            } else if ((insn & 0x7e000000) == 0x34000000) {
                std::string v = rx(field(insn, 4, 0));
                if (!bit(insn, 31)) v = "(uint32_t)" + v;
                c = bit(insn, 24) ? v + " != 0" : v + " == 0";
            } else {
                const unsigned b = (bit(insn, 31) << 5) | field(insn, 23, 19);
                c = fmt("((%s >> %u) & 1) == %u", rx(field(insn, 4, 0)).c_str(), b, unsigned(bit(insn, 24)));
            }
            line("if (" + c + ") " + go(t));
            return true;
        }
        case Flow::kB:
            line(go(t));
            return true;
        case Flow::kBL:
            call_to(addr(t), t, va + 4);
            return true;
        case Flow::kBLR: {
            // read the target before x30 is written (BLR x30)
            line("{ const uint64_t blr = " + rx(field(insn, 9, 5)) + ";");
            call_to("blr", 0, va + 4);
            line("}");
            return true;
        }
        case Flow::kBR: {
            line("{");
            line("    const uint64_t t = " + rx(field(insn, 9, 5)) + ";");
            line("    switch (t - B) {");
            for (uint64_t l : f_.labels) line(fmt("        case 0x%" PRIx64 "ull: goto L_%" PRIx64 ";", l, l));
            line("    }");
            line("    " + flush(true) + " c->pc = t; return aot_rt.branch(c, t);");
            line("}");
            return true;
        }
        case Flow::kRet:
            line("{ const uint64_t t = " + rx(field(insn, 9, 5)) + "; " + flush(false) +
                 " c->pc = t; return 0; }");
            return true;
    }
    return false;
}

bool Emitter::dp_immediate(uint32_t insn, uint64_t va) {
    const unsigned rd = field(insn, 4, 0), rn = field(insn, 9, 5);
    const bool sf = bit(insn, 31);
    switch (field(insn, 25, 23)) {
        case 0: case 1: {  // ADR, ADRP
            const int64_t imm = sext((field(insn, 23, 5) << 2) | field(insn, 30, 29), 21);
            const uint64_t v = bit(insn, 31) ? (va & ~uint64_t{0xfff}) + (static_cast<uint64_t>(imm) << 12)
                                             : va + static_cast<uint64_t>(imm);
            wx(rd, addr(v), true);
            return true;
        }
        case 2: {  // ADD/ADDS/SUB/SUBS (immediate)
            const bool sub = bit(insn, 30), s = bit(insn, 29);
            uint64_t imm = field(insn, 21, 10);
            if (bit(insn, 22)) imm <<= 12;
            const std::string a = rxsp(rn);
            if (s) {
                wx(rd, fmt("addf%u(%s, %s, %u, %s)", sf ? 64 : 32, a.c_str(), hex(sub ? ~imm : imm).c_str(),
                           unsigned(sub), flags()),
                   sf);
            } else {
                wxsp(rd, a + (sub ? " - " : " + ") + hex(imm), sf);
            }
            return true;
        }
        case 4: {  // AND/ORR/EOR/ANDS (immediate)
            uint64_t wmask, tmask;
            if (!sf && bit(insn, 22)) return false;
            if (!decode_bit_masks(bit(insn, 22), field(insn, 15, 10), field(insn, 21, 16), true, sf ? 64 : 32,
                                  &wmask, &tmask)) {
                return false;
            }
            static const char* kOps[] = {" & ", " | ", " ^ ", " & "};
            const unsigned opc = field(insn, 30, 29);
            const std::string r = "(" + rx(rn) + kOps[opc] + hex(wmask) + ")";
            if (opc == 3) {
                line("{ const uint64_t r = " + std::string(sf ? "" : "(uint32_t)") + r + ";");
                flags_written_ = true;
                line(sf ? "  FN = r >> 63; FZ = r == 0; FC = 0; FV = 0;"
                        : "  FN = (r >> 31) & 1; FZ = (uint32_t)r == 0; FC = 0; FV = 0;");
                wx(rd, "r", sf);
                line("}");
            } else {
                wxsp(rd, r, sf);
            }
            return true;
        }
        case 5: {  // MOVN/MOVZ/MOVK
            const unsigned hw = field(insn, 22, 21);
            if (!sf && hw > 1) return false;
            const unsigned pos = hw * 16;
            const uint64_t imm = uint64_t(field(insn, 20, 5)) << pos;
            switch (field(insn, 30, 29)) {
                case 0: wx(rd, hex(~imm), sf); return true;
                case 2: wx(rd, hex(imm), sf); return true;
                case 3:
                    wx(rd, "(" + rx(rd) + " & " + hex(~(uint64_t{0xffff} << pos)) + ") | " + hex(imm), sf);
                    return true;
                default: return false;
            }
        }
        case 6: {  // SBFM/BFM/UBFM
            const unsigned opc = field(insn, 30, 29);
            const unsigned size = sf ? 64 : 32;
            if (opc == 3 || bit(insn, 22) != sf) return false;
            const unsigned immr = field(insn, 21, 16), imms = field(insn, 15, 10);
            uint64_t wmask, tmask;
            if (!decode_bit_masks(bit(insn, 22), imms, immr, false, size, &wmask, &tmask)) return false;
            line("{ const uint64_t src = " + rx(rn) + " & " + hex(mask(size)) + ";");
            line(fmt("  const uint64_t bot_src = ror%u(src, %u);", size, immr));
            if (opc == 1) {  // BFM
                line("  const uint64_t dst = " + rx(rd) + ";");
                line("  const uint64_t bot = (dst & " + hex(~wmask) + ") | (bot_src & " + hex(wmask) + ");");
                wx(rd, "((dst & " + hex(~tmask) + ") | (bot & " + hex(tmask) + ")) & " + hex(mask(size)), true);
            } else if (opc == 0) {  // SBFM
                line("  const uint64_t bot = bot_src & " + hex(wmask) + ";");
                line(fmt("  const uint64_t top = ((src >> %u) & 1) ? %s : 0;", imms, hex(mask(size)).c_str()));
                wx(rd, "((top & " + hex(~tmask) + ") | (bot & " + hex(tmask) + ")) & " + hex(mask(size)), true);
            } else {  // UBFM
                wx(rd, "bot_src & " + hex(wmask & tmask), true);
            }
            line("}");
            return true;
        }
        case 7: {  // EXTR
            const unsigned size = sf ? 64 : 32;
            if (bit(insn, 22) != sf || field(insn, 30, 29) != 0 || bit(insn, 21)) return false;
            const unsigned lsb = field(insn, 15, 10);
            if (lsb >= size) return false;
            const std::string hi = "(" + rx(rn) + " & " + hex(mask(size)) + ")";
            const std::string lo = "(" + rx(field(insn, 20, 16)) + " & " + hex(mask(size)) + ")";
            if (lsb == 0) {
                wx(rd, hi, true);
            } else {
                wx(rd, fmt("((%s >> %u) | (%s << %u)) & %s", lo.c_str(), lsb, hi.c_str(), size - lsb,
                           hex(mask(size)).c_str()),
                   true);
            }
            return true;
        }
        default:
            return false;
    }
}

bool Emitter::dp_register(uint32_t insn) {
    const bool sf = bit(insn, 31);
    const unsigned size = sf ? 64 : 32;
    const unsigned rd = field(insn, 4, 0), rn = field(insn, 9, 5), rm = field(insn, 20, 16);
    const std::string m = hex(mask(size));

    if ((insn & 0x1f000000) == 0x0a000000) {  // logical (shifted register)
        const unsigned imm6 = field(insn, 15, 10);
        if (!sf && imm6 >= 32) return false;
        std::string b = shift(rx(rm), field(insn, 23, 22), imm6, sf);
        if (bit(insn, 21)) b = "~" + b;
        static const char* kOps[] = {" & ", " | ", " ^ ", " & "};
        const unsigned opc = field(insn, 30, 29);
        line("{ const uint64_t r = (" + rx(rn) + kOps[opc] + "(" + b + ")) & " + m + ";");
        if (opc == 3) {
            flags_written_ = true;
            line(fmt("  FN = (r >> %u) & 1; FZ = r == 0; FC = 0; FV = 0;", size - 1));
        }
        wx(rd, "r", true);
        line("}");
        return true;
    }
    if ((insn & 0x1f200000) == 0x0b000000) {  // add/sub (shifted register)
        const unsigned imm6 = field(insn, 15, 10), type = field(insn, 23, 22);
        if (type == 3 || (!sf && imm6 >= 32)) return false;
        const bool sub = bit(insn, 30), s = bit(insn, 29);
        const std::string b = shift(rx(rm), type, imm6, sf);
        if (s) {
            wx(rd, fmt("addf%u(%s, %s(%s), %u, %s)", size, rx(rn).c_str(), sub ? "~" : "", b.c_str(), unsigned(sub),
                       flags()),
               sf);
        } else {
            wx(rd, rx(rn) + (sub ? " - " : " + ") + "(" + b + ")", sf);
        }
        return true;
    }
    if ((insn & 0x1f200000) == 0x0b200000) {  // add/sub (extended register)
        const unsigned imm3 = field(insn, 12, 10);
        if (imm3 > 4 || field(insn, 23, 22) != 0) return false;
        const bool sub = bit(insn, 30), s = bit(insn, 29);
        const std::string b = extend(rx(rm), field(insn, 15, 13), imm3);
        if (s) {
            wx(rd, fmt("addf%u(%s, %s(%s), %u, %s)", size, rxsp(rn).c_str(), sub ? "~" : "", b.c_str(),
                       unsigned(sub), flags()),
               sf);
        } else {
            wxsp(rd, rxsp(rn) + (sub ? " - " : " + ") + b, sf);
        }
        return true;
    }
    if ((insn & 0x1fe00000) == 0x1a000000) {  // ADC/ADCS/SBC/SBCS
        if (field(insn, 15, 10) != 0) return false;
        const bool sub = bit(insn, 30), s = bit(insn, 29);
        flags_used_ = true;
        const std::string b = std::string(sub ? "~" : "") + rx(rm);
        if (s) {
            wx(rd, fmt("addf%u(%s, %s, FC, %s)", size, rx(rn).c_str(), b.c_str(), flags()), sf);
        } else {
            wx(rd, rx(rn) + " + (" + b + ") + FC", sf);
        }
        return true;
    }
    if ((insn & 0x1fe00000) == 0x1a400000) {  // CCMN/CCMP (register and immediate)
        if (!bit(insn, 29) || bit(insn, 10) || bit(insn, 4)) return false;
        const bool sub = bit(insn, 30);
        const std::string c = cond(field(insn, 15, 12));
        const std::string b = bit(insn, 11) ? hex(rm) : rx(rm);
        const unsigned nzcv = field(insn, 3, 0);
        line("if (" + c + ") {");
        line(fmt("    (void)addf%u(%s, %s(%s), %u, %s);", size, rx(rn).c_str(), sub ? "~" : "", b.c_str(),
                 unsigned(sub), flags()));
        line(fmt("} else { FN = %u; FZ = %u; FC = %u; FV = %u; }", (nzcv >> 3) & 1, (nzcv >> 2) & 1, (nzcv >> 1) & 1,
                 nzcv & 1));
        return true;
    }
    if ((insn & 0x1fe00000) == 0x1a800000) {  // CSEL/CSINC/CSINV/CSNEG
        if (bit(insn, 29) || bit(insn, 11)) return false;
        const unsigned op = (bit(insn, 30) << 1) | bit(insn, 10);
        static const char* kAlt[] = {"%s", "%s + 1", "~%s", "0 - %s"};
        const std::string alt = fmt(kAlt[op], rx(rm).c_str());
        wx(rd, "(" + cond(field(insn, 15, 12)) + " ? " + rx(rn) + " : (" + alt + ")) & " + m, true);
        return true;
    }
    if ((insn & 0x5fe00000) == 0x5ac00000) {  // data processing (1 source)
        const unsigned opcode2 = field(insn, 20, 16), opcode = field(insn, 15, 10);
        if (bit(insn, 29)) return false;
        if (opcode2 == 1) return sf;  // PAC*/AUT*/XPAC*: nothing to do (pointers are never signed)
        if (opcode2 != 0) return false;
        const std::string a = "(" + rx(rn) + " & " + m + ")";
        switch (opcode) {
            case 0:
                wx(rd, sf ? "__builtin_bitreverse64(" + a + ")" : "__builtin_bitreverse32((uint32_t)" + a + ")", true);
                return true;
            case 1:
                wx(rd, "rev16_64(" + a + ") & " + m, true);
                return true;
            case 2:
                wx(rd, sf ? "rev32_64(" + a + ")" : "__builtin_bswap32((uint32_t)" + a + ")", true);
                return true;
            case 3:
                if (!sf) return false;
                wx(rd, "__builtin_bswap64(" + a + ")", true);
                return true;
            case 4:
                wx(rd, fmt("clz%u(%s)", size, a.c_str()), true);
                return true;
            case 5:  // CLS
                line("{ const uint64_t t = (" + a + " ^ (" + a + " << 1)) & " + m + ";");
                wx(rd, fmt("t == 0 ? %u : clz%u(t)", size - 1, size), true);
                line("}");
                return true;
            default:
                return false;
        }
    }
    if ((insn & 0x5fe00000) == 0x1ac00000) {  // data processing (2 source)
        if (bit(insn, 29)) return false;
        const unsigned opcode = field(insn, 15, 10);
        const std::string a = "(" + rx(rn) + " & " + m + ")", b = "(" + rx(rm) + " & " + m + ")";
        switch (opcode) {
            case 2: wx(rd, fmt("udiv%u(%s, %s)", size, a.c_str(), b.c_str()), true); return true;
            case 3: wx(rd, fmt("sdiv%u(%s, %s)", size, a.c_str(), b.c_str()), true); return true;
            case 8: case 9: case 10: case 11: {  // LSLV, LSRV, ASRV, RORV
                const std::string amt = fmt("(unsigned)(%s %% %u)", b.c_str(), size);
                std::string r;
                switch (opcode - 8) {
                    case 0: r = "(" + a + " << " + amt + ") & " + m; break;
                    case 1: r = a + " >> " + amt; break;
                    case 2: r = fmt("asr%u(%s, %s)", size, a.c_str(), amt.c_str()); break;
                    default: r = fmt("ror%u(%s, %s)", size, a.c_str(), amt.c_str()); break;
                }
                wx(rd, r, true);
                return true;
            }
            case 12: wx(rd, "0", true); return true;  // PACGA
            default: return false;                    // CRC32*: the interpreter
        }
    }
    if ((insn & 0x1f000000) == 0x1b000000) {  // data processing (3 source)
        const unsigned op = (field(insn, 30, 29) << 4) | (field(insn, 23, 21) << 1) | bit(insn, 15);
        const std::string a = rx(rn), b = rx(rm), acc = rx(field(insn, 14, 10));
        switch (op) {
            case 0x00: wx(rd, acc + " + " + a + " * " + b, sf); return true;  // MADD
            case 0x01: wx(rd, acc + " - " + a + " * " + b, sf); return true;  // MSUB
            case 0x02: if (!sf) return false;
                wx(rd, acc + " + (uint64_t)((int64_t)(int32_t)" + a + " * (int32_t)" + b + ")", true); return true;
            case 0x03: if (!sf) return false;
                wx(rd, acc + " - (uint64_t)((int64_t)(int32_t)" + a + " * (int32_t)" + b + ")", true); return true;
            case 0x04: if (!sf) return false;  // SMULH
                wx(rd, "(uint64_t)(((__int128)(int64_t)" + a + " * (int64_t)" + b + ") >> 64)", true); return true;
            case 0x0a: if (!sf) return false;
                wx(rd, acc + " + (uint64_t)(uint32_t)" + a + " * (uint32_t)" + b, true); return true;
            case 0x0b: if (!sf) return false;
                wx(rd, acc + " - (uint64_t)(uint32_t)" + a + " * (uint32_t)" + b, true); return true;
            case 0x0c: if (!sf) return false;  // UMULH
                wx(rd, "(uint64_t)(((unsigned __int128)" + a + " * " + b + ") >> 64)", true); return true;
            default: return false;
        }
    }
    return false;
}

// LDR/STR (immediate, unscaled, pre/post-indexed, register offset), general
// and SIMD&FP registers.
bool Emitter::ldst_register(uint32_t insn) {
    const bool simd = bit(insn, 26);
    const unsigned size = field(insn, 31, 30), opc = field(insn, 23, 22);
    const unsigned rn = field(insn, 9, 5), rt = field(insn, 4, 0);
    unsigned scale = size;
    if (simd) {
        if (opc & 2) {
            if (size != 0) return false;
            scale = 4;  // Q
        }
    }
    if (!simd && ((size == 3 && opc == 3) || (size == 2 && opc == 3))) return false;
    if (!simd && size == 3 && opc == 2 && bit(insn, 24)) return true;  // PRFM: nothing to do
    if (!bit(insn, 24) && bit(insn, 21) && field(insn, 11, 10) != 2) return false;  // atomics, LDRAA/LDRAB
    if (!bit(insn, 24) && bit(insn, 21) && !(field(insn, 15, 13) & 2)) return false;
    std::string a, wb;
    const std::string base = rxsp(rn);
    if (bit(insn, 24)) {
        a = base + " + " + hex(uint64_t(field(insn, 21, 10)) << scale);
    } else if (!bit(insn, 21)) {
        const int64_t imm9 = sext(field(insn, 20, 12), 9);
        switch (field(insn, 11, 10)) {
            case 0: case 2: a = base + " + " + hex(static_cast<uint64_t>(imm9)); break;
            case 1: a = base; wb = base + " + " + hex(static_cast<uint64_t>(imm9)); break;
            default: a = base + " + " + hex(static_cast<uint64_t>(imm9)); wb = "a"; break;
        }
    } else if (field(insn, 11, 10) == 2) {
        const unsigned option = field(insn, 15, 13);
        if (!(option & 2)) return false;
        a = base + " + " + extend(rx(field(insn, 20, 16)), option, bit(insn, 12) ? scale : 0);
    } else {
        return false;  // atomics, LDRAA/LDRAB
    }
    line("{ const uint64_t a = " + a + ";");
    if (simd) {
        const bool load = opc & 1;
        if (scale == 4) {
            if (load) wv(rt, "LD64(a)", "LD64(a + 8)");
            else line("  ST64(a, " + vl(rt) + "); ST64(a + 8, " + vh(rt) + ");");
        } else {
            static const char* kLd[] = {"LD8(a)", "LD16(a)", "LD32(a)", "LD64(a)"};
            static const char* kSt[] = {"ST8", "ST16", "ST32", "ST64"};
            if (load) wv(rt, kLd[size], "0");
            else line(std::string("  ") + kSt[size] + "(a, " + vl(rt) + ");");
        }
    } else {
        static const char* kLd[] = {"LD8(a)", "LD16(a)", "LD32(a)", "LD64(a)"};
        static const char* kSt[] = {"ST8", "ST16", "ST32", "ST64"};
        static const char* kSx[] = {"(int64_t)(int8_t)", "(int64_t)(int16_t)", "(int64_t)(int32_t)", ""};
        if (opc == 0) {
            line(std::string("  ") + kSt[size] + "(a, " + rx(rt) + ");");
        } else if (opc == 1) {
            wx(rt, kLd[size], true);
        } else if (size == 3) {
            // PRFM / PRFUM: nothing to do
        } else if (opc == 2) {
            wx(rt, std::string("(uint64_t)") + kSx[size] + kLd[size], true);
        } else {
            wx(rt, std::string("(uint64_t)") + kSx[size] + kLd[size], false);
        }
    }
    if (!wb.empty()) {
        line("  " + reg(rn) + " = " + wb + ";");
    }
    line("}");
    return true;
}

bool Emitter::ldst_pair(uint32_t insn) {
    const bool simd = bit(insn, 26);
    const unsigned opc = field(insn, 31, 30), mode = field(insn, 24, 23);
    const bool l = bit(insn, 22);
    const unsigned rt2 = field(insn, 14, 10), rn = field(insn, 9, 5), rt = field(insn, 4, 0);
    unsigned scale;
    if (simd) {
        if (opc == 3) return false;
        scale = 2 + opc;
    } else {
        if (opc == 3 || (opc == 1 && !l)) return false;
        scale = opc == 2 ? 3 : 2;
    }
    const int64_t off = sext(field(insn, 21, 15), 7) << scale;
    const std::string base = rxsp(rn);
    line("{ const uint64_t base = " + base + ";");
    line("  const uint64_t a = " + std::string(mode == 1 ? "base" : "base + " + hex(static_cast<uint64_t>(off))) +
         ";");
    const unsigned bytes = 1u << scale;
    if (simd) {
        static const char* kLd[] = {"", "", "LD32", "LD64"};
        static const char* kSt[] = {"", "", "ST32", "ST64"};
        if (l) {
            if (bytes == 16) {
                line("  const uint64_t a0 = LD64(a), a1 = LD64(a + 8), b0 = LD64(a + 16), b1 = LD64(a + 24);");
                wv(rt, "a0", "a1");
                wv(rt2, "b0", "b1");
            } else {
                line(fmt("  const uint64_t a0 = %s(a), b0 = %s(a + %u);", kLd[scale], kLd[scale], bytes));
                wv(rt, "a0", "0");
                wv(rt2, "b0", "0");
            }
        } else if (bytes == 16) {
            line("  ST64(a, " + vl(rt) + "); ST64(a + 8, " + vh(rt) + "); ST64(a + 16, " + vl(rt2) +
                 "); ST64(a + 24, " + vh(rt2) + ");");
        } else {
            line(fmt("  %s(a, %s); %s(a + %u, %s);", kSt[scale], vl(rt).c_str(), kSt[scale], bytes,
                     vl(rt2).c_str()));
        }
    } else if (l) {
        const char* ld = scale == 3 ? "LD64" : "LD32";
        const char* sx = opc == 1 ? "(uint64_t)(int64_t)(int32_t)" : "(uint64_t)";
        line(fmt("  const uint64_t a0 = %s%s(a), b0 = %s%s(a + %u);", sx, ld, sx, ld, bytes));
        wx(rt, "a0", true);
        wx(rt2, "b0", true);
    } else {
        const char* st = scale == 3 ? "ST64" : "ST32";
        line(fmt("  %s(a, %s); %s(a + %u, %s);", st, rx(rt).c_str(), st, bytes, rx(rt2).c_str()));
    }
    if (mode == 1 || mode == 3) line("  " + reg(rn) + " = base + " + hex(static_cast<uint64_t>(off)) + ";");
    line("}");
    return true;
}

bool Emitter::loads_stores(uint32_t insn, uint64_t va) {
    if ((insn & 0x3f000000) == 0x08000000) return false;  // exclusives, acquire/release: the interpreter
    if ((insn & 0x3b000000) == 0x18000000) {  // LDR (literal)
        const uint64_t a = va + static_cast<uint64_t>(sext(field(insn, 23, 5), 19) << 2);
        const unsigned rt = field(insn, 4, 0);
        if (bit(insn, 26)) {
            switch (field(insn, 31, 30)) {
                case 0: wv(rt, "LD32(" + addr(a) + ")", "0"); return true;
                case 1: wv(rt, "LD64(" + addr(a) + ")", "0"); return true;
                case 2: wv(rt, "LD64(" + addr(a) + ")", "LD64(" + addr(a + 8) + ")"); return true;
                default: return false;
            }
        }
        switch (field(insn, 31, 30)) {
            case 0: wx(rt, "LD32(" + addr(a) + ")", true); return true;
            case 1: wx(rt, "LD64(" + addr(a) + ")", true); return true;
            case 2: wx(rt, "(uint64_t)(int64_t)(int32_t)LD32(" + addr(a) + ")", true); return true;
            default: return true;  // PRFM (literal)
        }
    }
    if ((insn & 0x3a000000) == 0x28000000) return ldst_pair(insn);
    if ((insn & 0x3a000000) == 0x38000000) return ldst_register(insn);
    return false;
}

bool Emitter::system(uint32_t insn) {
    if ((insn & 0xfffff01f) == 0xd503201f) return true;  // hints: NOP, YIELD, BTI, PACIASP, ...
    if ((insn & 0xfffff09f) == 0xd503309f) {             // DSB / DMB / ISB
        const unsigned crm = field(insn, 11, 8);
        if (field(insn, 7, 5) != 6 && (crm & 3) == 3) line("__atomic_thread_fence(__ATOMIC_SEQ_CST);");
        else line("__atomic_signal_fence(__ATOMIC_SEQ_CST);");
        return true;
    }
    if ((insn & 0xffffffe0) == 0xd53bd040) {  // MRS Xt, TPIDR_EL0
        wx(field(insn, 4, 0), "c->tpidr", true);
        return true;
    }
    return false;
}

std::string Emitter::function(size_t* interpreted) {
    for (uint64_t va : f_.insns) {
        if (f_.labels.count(va)) body_ += fmt("L_%" PRIx64 ":;\n", va);
        const uint32_t insn = img_.insn(va);
        bool done = false;
        uint64_t target;
        if (classify(insn, va, &target) != Flow::kSeq) {
            done = branch(va, insn);
        } else {
            switch (field(insn, 28, 25)) {
                case 0x8: case 0x9: done = dp_immediate(insn, va); break;
                case 0xa: case 0xb: done = ((insn & 0xffc00000) == 0xd5000000) && system(insn); break;
                case 0x4: case 0x6: case 0xc: case 0xe: done = loads_stores(insn, va); break;
                case 0x5: case 0xd: done = dp_register(insn); break;
                default: break;
            }
        }
        if (!done) interpret(va);
        if (f_.falls_out.count(va)) exit_to(va + 4);
    }
    *interpreted = interpreted_;

    // Declarations and the write-back macros, now that the body says what it uses.
    std::string out;
    const std::string name = fmt("F_%" PRIx64, f_.entry);
    std::string flush, reload_all, reload_call;
    for (unsigned n = 0; n < 32; ++n) {
        if (!(used_x_ >> n & 1)) continue;
        const std::string r = n == 31 ? "SP" : fmt("X%u", n), slot = n == 31 ? "c->sp" : fmt("c->x[%u]", n);
        if (written_x_ >> n & 1) flush += " " + slot + " = " + r + ";";
        reload_all += " " + r + " = " + slot + ";";
        if (n < 8) reload_call += " " + r + " = " + slot + ";";
    }
    for (unsigned n = 0; n < 32; ++n) {
        if (!(used_v_ >> n & 1)) continue;
        if (written_v_ >> n & 1) flush += fmt(" c->v[%u][0] = V%uL; c->v[%u][1] = V%uH;", n, n, n, n);
        reload_all += fmt(" V%uL = c->v[%u][0]; V%uH = c->v[%u][1];", n, n, n, n);
        if (n < 8) reload_call += fmt(" V%uL = c->v[%u][0]; V%uH = c->v[%u][1];", n, n, n, n);
    }
    const bool has_flags = flags_used_ || flags_written_;
    std::string flush_all = flush;
    if (flags_written_) {
        flush_all += " c->nzcv = ((uint32_t)FN << 31) | ((uint32_t)FZ << 30) | ((uint32_t)FC << 29) | "
                     "((uint32_t)FV << 28);";
    }
    if (has_flags) {
        reload_all += " FN = (c->nzcv >> 31) & 1; FZ = (c->nzcv >> 30) & 1; FC = (c->nzcv >> 29) & 1;"
                      " FV = (c->nzcv >> 28) & 1;";
    }
    out += "#define FLUSH() do {" + flush + " } while (0)\n";
    out += "#define FLUSH_ALL() do {" + flush_all + " } while (0)\n";
    out += "#define RELOAD_ALL() do {" + reload_all + " } while (0)\n";
    out += "#define RELOAD_CALL() do {" + reload_call + " } while (0)\n";
    out += "static uint32_t " + name + "(Cpu* restrict c) {\n";
    out += "    const uint64_t B = aot_base;\n";
    for (unsigned n = 0; n < 32; ++n) {
        if (!(used_x_ >> n & 1)) continue;
        out += n == 31 ? "    uint64_t SP = c->sp;\n" : fmt("    uint64_t X%u = c->x[%u];\n", n, n);
    }
    for (unsigned n = 0; n < 32; ++n) {
        if (used_v_ >> n & 1) out += fmt("    uint64_t V%uL = c->v[%u][0], V%uH = c->v[%u][1];\n", n, n, n, n);
    }
    if (has_flags) {
        out += "    uint8_t FN = (c->nzcv >> 31) & 1, FZ = (c->nzcv >> 30) & 1, FC = (c->nzcv >> 29) & 1,"
               " FV = (c->nzcv >> 28) & 1;\n";
    }
    out += fmt("    goto L_%" PRIx64 ";\n", f_.entry);
    out += body_;
    out += "}\n#undef FLUSH\n#undef FLUSH_ALL\n#undef RELOAD_ALL\n#undef RELOAD_CALL\n\n";
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: winapk-aot <lib.so> <out.c>\n");
        return 2;
    }
    Image img;
    std::string error;
    if (!read_image(argv[1], &img, &error)) {
        std::fprintf(stderr, "winapk-aot: %s: %s\n", argv[1], error.c_str());
        return 1;
    }

    // Every function: the symbols, then the targets of BL, transitively.
    std::set<uint64_t> entries;
    for (const auto& s : img.symbols) entries.insert(s.first);
    std::map<uint64_t, Function> functions;
    std::vector<uint64_t> work(entries.begin(), entries.end());
    size_t skipped = 0;
    while (!work.empty()) {
        const uint64_t entry = work.back();
        work.pop_back();
        if (functions.count(entry)) continue;
        Function f;
        f.entry = entry;
        auto it = img.symbols.find(entry);
        if (it != img.symbols.end() && it->second) f.end = entry + it->second;
        std::set<uint64_t> calls;
        if (!discover(img, entries, &f, &calls)) {
            ++skipped;
            continue;
        }
        functions.emplace(entry, std::move(f));
        for (uint64_t t : calls) {
            if (entries.insert(t).second) work.push_back(t);
        }
    }
    std::set<uint64_t> translated;
    for (const auto& f : functions) translated.insert(f.first);

    std::string out = kPrelude;
    for (uint64_t e : translated) out += fmt("static uint32_t F_%" PRIx64 "(Cpu* restrict c);\n", e);
    out += "\n";
    size_t insns = 0, interpreted = 0;
    for (const auto& f : functions) {
        Emitter em(img, f.second, translated);
        size_t n = 0;
        out += em.function(&n);
        insns += f.second.insns.size();
        interpreted += n;
    }
    out += "static const AotEntry entries[] = {\n";
    for (uint64_t e : translated) out += fmt("    {0x%" PRIx64 "ull, F_%" PRIx64 "},\n", e, e);
    out += "};\n\n";
    out += "__declspec(dllexport) const AotModule winapk_aot_module = {\n";
    out += fmt("    %u, {offsetof(Cpu, sp), offsetof(Cpu, nzcv), offsetof(Cpu, tpidr), offsetof(Cpu, v)},\n",
               winapk::arm64::kAotVersion);
    out += fmt("    0x%" PRIx64 "ull, &aot_base, &aot_rt, entries, sizeof entries / sizeof entries[0],\n",
               img.text_hash);
    out += "};\n";

    std::ofstream o(argv[2], std::ios::binary);
    o << out;
    if (!o) {
        std::fprintf(stderr, "winapk-aot: cannot write %s\n", argv[2]);
        return 1;
    }
    std::fprintf(stderr, "winapk-aot: %s: %zu functions, %zu instructions, %zu through the interpreter (%.1f%%)%s\n",
                 argv[1], functions.size(), insns, interpreted,
                 insns ? 100.0 * static_cast<double>(interpreted) / static_cast<double>(insns) : 0.0,
                 skipped ? fmt(", %zu too large to translate", skipped).c_str() : "");
    return 0;
}
