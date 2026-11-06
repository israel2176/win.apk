"""Writes the assembly of libwinapk_arm64_fuzz.so: random arm64 instruction
sequences for the differential test of the arm64 engines (test/arm64_test.cpp,
run_fuzz). Each function runs the same code on the interpreter, the JIT and
the translation made ahead of time, from the same state, and every register,
the flags and a scratch buffer must come out the same as on the interpreter.

usage: python3 gen_arm64_fuzz.py <functions> <instructions> <seed> > fuzz.S

Every function is

    fuzz_<i>(State* x0, uint8_t scratch[4096] x1)

with State = { uint64_t x[32]; uint64_t nzcv; uint8_t v[32][16]; } (784
bytes): it loads x2..x27, NZCV and v0..v31 from the state, runs its random
instructions, and stores them back. x28 holds the scratch buffer (loads and
stores go there only) and x29 the state; neither is an operand. The
instructions are those compilers emit for ordinary code (integer, bitfield,
conditional, multiply and divide, loads and stores of every addressing mode,
NEON integer and floating point, conversions, exclusives and LSE atomics),
forward branches, counted loops, a jump through a register to a label (a
jump the translation cannot follow), and calls to small helpers that, like
LLVM's outlined functions, leave flags and registers to their caller.

Left out on purpose: FRECPE/FRSQRTE/FRECPX (the interpreter and the
translation compute 1/x exactly, the JIT Arm's 8-bit estimate) and anything
that reads the stack pointer or system registers other than NZCV.

FMAX/FMIN and their kin are only in about a third of the functions, which
also get the symbol fuzz_<i>_jitnan: the JIT runs with dynarmic's
Unsafe_InaccurateNaN (arm64_jit.cpp), under which those return the number,
not the NaN, for a NaN operand, so the test leaves their JIT results
unchecked. Those functions jump through no register either, as the
translation hands such a jump to the JIT: what they check is the
translation's own.
"""
import random
import sys

N_FUNCS = int(sys.argv[1])
N_INSNS = int(sys.argv[2])
rng = random.Random(int(sys.argv[3]))

MINMAX = False  # whether the function being written may have FMAX/FMIN (see above)
DATA = list(range(2, 27))  # x2..x26: random operands; x27 is the loop counter
CONDS = ["eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le", "al"]


def x(): return "x%d" % rng.choice(DATA)
def w(): return "w%d" % rng.choice(DATA)
def r(sf): return x() if sf else w()
def distinct(n): return rng.sample(DATA, n)
def vreg(): return rng.randrange(32)


def logical_imm(bits):
    """A random value encodable as a logical immediate of `bits` bits."""
    while True:
        esize = rng.choice([e for e in (2, 4, 8, 16, 32, 64) if e <= bits])
        ones = rng.randrange(1, esize)  # not all ones
        rot = rng.randrange(esize)
        elem = (1 << ones) - 1
        elem = ((elem >> rot) | (elem << (esize - rot))) & ((1 << esize) - 1)
        v = 0
        for i in range(0, bits, esize):
            v |= elem << i
        return v


def shift_op(sf):
    return "%s %d" % (rng.choice(["lsl", "lsr", "asr"]), rng.randrange(64 if sf else 32))


def integer():
    sf = rng.random() < 0.6
    k = rng.randrange(27)
    R = lambda: r(sf)
    if k == 0:
        return "%s %s, %s, #%d" % (rng.choice(["add", "sub", "adds", "subs"]), R(), R(), rng.randrange(4096))
    if k == 1:
        return "%s %s, %s, %s, %s" % (rng.choice(["add", "sub", "adds", "subs"]), R(), R(), R(), shift_op(sf))
    if k == 2:
        ext = rng.choice(["uxtb", "uxth", "uxtw", "sxtb", "sxth", "sxtw"] + (["uxtx", "sxtx"] if sf else []))
        src = w() if sf and ext[-1] != "x" else R()
        return "%s %s, %s, %s, %s #%d" % (rng.choice(["add", "sub", "adds", "subs"]), R(), R(), src, ext,
                                         rng.randrange(5))
    if k == 3:
        op = rng.choice(["and", "orr", "eor", "ands", "bic", "orn", "eon", "bics"])
        sh = "%s %d" % (rng.choice(["lsl", "lsr", "asr", "ror"]), rng.randrange(64 if sf else 32))
        return "%s %s, %s, %s, %s" % (op, R(), R(), R(), sh)
    if k == 4:
        return "%s %s, %s, #0x%x" % (rng.choice(["and", "orr", "eor", "ands"]), R(), R(), logical_imm(64 if sf else 32))
    if k == 5:
        hw = rng.randrange(4 if sf else 2) * 16
        return "%s %s, #0x%x, lsl %d" % (rng.choice(["movz", "movk", "movn"]), R(), rng.randrange(65536), hw)
    if k == 6:
        return "%s %s, %s, %s" % (rng.choice(["adc", "adcs", "sbc", "sbcs"]), R(), R(), R())
    if k == 7:
        return "%s %s, %s, %s, %s" % (rng.choice(["csel", "csinc", "csinv", "csneg"]), R(), R(), R(), rng.choice(CONDS))
    if k == 8:
        b = ("#%d" % rng.randrange(32)) if rng.random() < 0.5 else R()
        return "%s %s, %s, #%d, %s" % (rng.choice(["ccmp", "ccmn"]), R(), b, rng.randrange(16), rng.choice(CONDS))
    if k == 9:
        return "%s %s, %s, %s" % (rng.choice(["lslv", "lsrv", "asrv", "rorv"]), R(), R(), R())
    if k == 10:
        return "%s %s, %s, %s, %s" % (rng.choice(["madd", "msub"]), R(), R(), R(), R())
    if k == 11:
        return "%s %s, %s, %s, %s" % (rng.choice(["smaddl", "umaddl", "smsubl", "umsubl"]), x(), w(), w(), x())
    if k == 12:
        return "%s %s, %s, %s" % (rng.choice(["smulh", "umulh"]), x(), x(), x())
    if k == 13:
        return "%s %s, %s, %s" % (rng.choice(["udiv", "sdiv"]), R(), R(), R())
    if k == 14:
        ops = ["clz", "cls", "rbit", "rev", "rev16"] + (["rev32"] if sf else [])
        return "%s %s, %s" % (rng.choice(ops), R(), R())
    if k == 15:
        size = 64 if sf else 32
        lsb = rng.randrange(size)
        width = rng.randrange(1, size - lsb + 1)
        return "%s %s, %s, #%d, #%d" % (rng.choice(["bfi", "bfxil", "sbfx", "ubfx", "sbfiz", "ubfiz"]), R(), R(),
                                        lsb, width)
    if k == 16:
        return "extr %s, %s, %s, #%d" % (R(), R(), R(), rng.randrange(64 if sf else 32))
    if k == 17:
        return "%s %s, %s" % (rng.choice(["cset", "csetm"]), R(), rng.choice(CONDS[:-1]))
    if k == 18:
        return "%s %s, %s" % (rng.choice(["cmp", "cmn", "tst"]), R(), R())
    if k == 19:
        return "%s %s, %s, #%d" % (rng.choice(["lsl", "lsr", "asr", "ror"]), R(), R(), rng.randrange(64 if sf else 32))
    if k == 20:
        if sf:
            return "%s %s, %s" % (rng.choice(["sxtb", "sxth", "sxtw"]), x(), w())
        return "%s %s, %s" % (rng.choice(["sxtb", "sxth", "uxtb", "uxth"]), w(), w())
    if k == 21:
        return "%s w%d, w%d, %s" % (rng.choice(["crc32b", "crc32h", "crc32w", "crc32cb", "crc32ch", "crc32cw"]),
                                   rng.choice(DATA), rng.choice(DATA), w()) if rng.random() < 0.8 else \
            "%s w%d, w%d, %s" % (rng.choice(["crc32x", "crc32cx"]), rng.choice(DATA), rng.choice(DATA), x())
    if k == 22:
        return "%s %s, %s" % (rng.choice(["neg", "negs", "mvn"]), R(), R())
    if k == 23:
        return "mov %s, #%d" % (R(), rng.randrange(-65536, 65536))
    if k == 24:
        return "%s %s, %s, %s" % (rng.choice(["mul", "mneg"]), R(), R(), R())
    if k == 25:
        return "%s %s, %s, %s" % (rng.choice(["smull", "umull", "smnegl", "umnegl"]), x(), w(), w())
    return "%s %s, #%d" % (rng.choice(["cmp", "cmn"]), R(), rng.randrange(4096))


def memory():
    k = rng.randrange(12)
    if k == 0:  # unsigned offset, every size
        size, reg, op = rng.choice([(1, "w", "ldrb"), (1, "w", "strb"), (2, "w", "ldrh"), (2, "w", "strh"),
                                    (4, "w", "ldr"), (4, "w", "str"), (8, "x", "ldr"), (8, "x", "str"),
                                    (1, "x", "ldrsb"), (1, "w", "ldrsb"), (2, "x", "ldrsh"), (2, "w", "ldrsh"),
                                    (4, "x", "ldrsw")])
        return "%s %s%d, [x28, #%d]" % (op, reg, rng.choice(DATA), size * rng.randrange(4096 // size - 2))
    if k == 1:  # unscaled
        op, reg = rng.choice([("ldur", "x"), ("stur", "x"), ("ldur", "w"), ("sturh", "w"), ("ldursb", "x"),
                              ("ldurh", "w"), ("ldursw", "x"), ("sturb", "w")])
        return "%s %s%d, [x28, #%d]" % (op, reg, rng.choice(DATA), rng.randrange(0, 256))
    if k == 2:  # pre- and post-index, through a register that is set first
        b, t = distinct(2)
        op = rng.choice(["ldr", "str", "ldrb", "strh", "ldrsw"])
        reg = "x" if op in ("ldr", "str", "ldrsw") else "w"
        mode = rng.choice(["[x%d, #%d]!" % (b, rng.randrange(-256, 256)), "[x%d], #%d" % (b, rng.randrange(-256, 256))])
        return "add x%d, x28, #%d\n    %s %s%d, %s" % (b, rng.randrange(1024, 3072), op, reg, t, mode)
    if k == 3:  # register offset
        b, i, t = distinct(3)
        op, reg, sh = rng.choice([("ldr", "x", 3), ("str", "x", 3), ("ldr", "w", 2), ("strb", "w", 0),
                                  ("ldrh", "w", 1), ("ldrsw", "x", 2)])
        ext = rng.choice(["uxtw", "lsl"])
        amount = rng.choice([0, sh])
        idx = ("w%d" % i) if ext == "uxtw" else ("x%d" % i)
        return "and x%d, x%d, #0xff\n    mov x%d, x28\n    %s %s%d, [x%d, %s, %s #%d]" % (
            i, rng.choice(DATA), b, op, reg, t, b, idx, ext, amount)
    if k == 4:  # pairs
        a, b = distinct(2)
        op = rng.choice(["ldp", "stp", "ldpsw", "ldnp", "stnp"])
        if op == "ldpsw":
            return "ldpsw x%d, x%d, [x28, #%d]" % (a, b, 4 * rng.randrange(64))
        reg, sz = rng.choice([("x", 8), ("w", 4)])
        return "%s %s%d, %s%d, [x28, #%d]" % (op, reg, a, reg, b, sz * rng.randrange(64))
    if k == 5:  # pair with writeback
        base, a, b = distinct(3)
        op = rng.choice(["ldp", "stp"])
        mode = rng.choice(["[x%d, #%d]!" % (base, 16 * rng.randrange(-4, 4)),
                           "[x%d], #%d" % (base, 16 * rng.randrange(-4, 4))])
        return "add x%d, x28, #%d\n    %s x%d, x%d, %s" % (base, rng.randrange(1024, 3072) & ~15, op, a, b, mode)
    if k == 6:  # SIMD&FP registers
        op, reg, sz = rng.choice([("ldr", "q", 16), ("str", "q", 16), ("ldr", "d", 8), ("str", "d", 8),
                                  ("ldr", "s", 4), ("str", "s", 4), ("ldr", "h", 2), ("str", "b", 1)])
        return "%s %s%d, [x28, #%d]" % (op, reg, vreg(), sz * rng.randrange(4096 // sz - 4))
    if k == 7:
        a, b = rng.sample(range(32), 2)
        op = rng.choice(["ldp", "stp"])
        reg, sz = rng.choice([("q", 16), ("d", 8), ("s", 4)])
        return "%s %s%d, %s%d, [x28, #%d]" % (op, reg, a, reg, b, sz * rng.randrange(32))
    if k == 8:  # structures
        base = rng.choice(DATA)
        v0 = rng.randrange(29)
        arr = rng.choice(["16b", "8b", "8h", "4s", "2d", "4h", "2s"])
        form = rng.choice([
            "ld1 {v%d.%s}" % (v0, arr), "st1 {v%d.%s}" % (v0, arr),
            "ld1 {v%d.%s, v%d.%s}" % (v0, arr, v0 + 1, arr),
            "st1 {v%d.%s, v%d.%s, v%d.%s}" % (v0, arr, v0 + 1, arr, v0 + 2, arr),
            "ld1 {v%d.%s, v%d.%s, v%d.%s, v%d.%s}" % (v0, arr, v0 + 1, arr, v0 + 2, arr, v0 + 3, arr),
            "ld2 {v%d.%s, v%d.%s}" % (v0, arr if arr != "2d" else "4s", v0 + 1, arr if arr != "2d" else "4s"),
            "st2 {v%d.%s, v%d.%s}" % (v0, arr if arr != "2d" else "4s", v0 + 1, arr if arr != "2d" else "4s"),
            "ld3 {v%d.4s, v%d.4s, v%d.4s}" % (v0, v0 + 1, v0 + 2),
            "st4 {v%d.8b, v%d.8b, v%d.8b, v%d.8b}" % (v0, v0 + 1, v0 + 2, v0 + 3),
            "ld1r {v%d.%s}" % (v0, arr), "ld1 {v%d.s}[%d]" % (v0, rng.randrange(4)),
            "st1 {v%d.h}[%d]" % (v0, rng.randrange(8)),
            "ld1 {v%d.d}[%d]" % (v0, rng.randrange(2)), "ld1 {v%d.b}[%d]" % (v0, rng.randrange(16)),
        ])
        post = rng.choice(["", ", #%d" % 0, ", x%d" % rng.choice([d for d in DATA if d != base])])
        if post == ", #0":
            post = ""
        if post.startswith(", x"):
            idx = int(post[3:])
            return "add x%d, x28, #%d\n    and x%d, x%d, #0x7f\n    %s, [x%d]%s" % (base, rng.randrange(256, 2048),
                                                                             idx, rng.choice(DATA), form, base, post)
        return "add x%d, x28, #%d\n    %s, [x%d]" % (base, rng.randrange(256, 2048), form, base)
    if k == 9:  # exclusives, as a whole loop (a store-exclusive alone may fail)
        a, s, t = distinct(3)
        size = rng.choice(["x", "w"])
        return ("add x{a}, x28, #{off}\n1:\n    ld{acq}xr {sz}{t}, [x{a}]\n    add {sz}{t}, {sz}{t}, #{inc}\n"
                "    st{rel}xr w{s}, {sz}{t}, [x{a}]\n    cbnz w{s}, 1b").format(
            a=a, s=s, t=t, sz=size, off=8 * rng.randrange(400), inc=rng.randrange(1, 100),
            acq=rng.choice(["", "a"]), rel=rng.choice(["", "l"]))
    if k == 10:  # LSE atomics
        a, s, t = distinct(3)
        op = rng.choice(["ldadd", "ldclr", "ldeor", "ldset", "ldsmax", "ldumin", "swp", "ldaddal", "swpl", "cas",
                         "casal"])
        return "add x%d, x28, #%d\n    %s x%d, x%d, [x%d]" % (a, 8 * rng.randrange(400), op, s, t, a)
    # LDAR/STLR
    a, t = distinct(2)
    return "add x%d, x28, #%d\n    %s x%d, [x%d]" % (a, 8 * rng.randrange(400), rng.choice(["ldar", "stlr", "ldapr"]),
                                                     t, a)


ARR_I = ["8b", "16b", "4h", "8h", "2s", "4s"]
ARR_F = ["2s", "4s", "2d"]


def simd():
    k = rng.randrange(26)
    d, n, m = vreg(), vreg(), vreg()
    a = rng.choice(ARR_I)
    if k == 0:
        arr = rng.choice(ARR_I + ["2d"])
        op = rng.choice(["add", "sub", "cmeq", "cmgt", "cmge", "cmhi", "cmhs", "cmtst"])
        return "%s v%d.%s, v%d.%s, v%d.%s" % (op, d, arr, n, arr, m, arr)
    if k == 1:
        op = rng.choice(["mul", "mla", "mls", "smax", "smin", "umax", "umin", "addp", "sabd", "uabd", "saba",
                         "uaba", "sqadd", "uqadd", "sqsub", "uqsub", "shadd", "uhadd", "srhadd", "urhadd", "smaxp",
                         "uminp", "sshl", "ushl", "srshl", "sqdmulh"])
        if op == "sqdmulh":
            a = rng.choice(["4h", "8h", "2s", "4s"])
        return "%s v%d.%s, v%d.%s, v%d.%s" % (op, d, a, n, a, m, a)
    if k == 2:
        b = rng.choice(["8b", "16b"])
        return "%s v%d.%s, v%d.%s, v%d.%s" % (rng.choice(["and", "orr", "eor", "bic", "orn", "bsl", "bit", "bif"]),
                                               d, b, n, b, m, b)
    if k == 3:
        op = rng.choice(["abs", "neg", "cmeq", "cmgt", "cmge", "cmlt", "cmle", "rev64", "sqabs", "sqneg", "cls", "clz"])
        if op.startswith("cm"):
            return "%s v%d.%s, v%d.%s, #0" % (op, d, a, n, a)
        if op == "rev64":
            a = rng.choice(["8b", "16b", "4h", "8h", "2s", "4s"])
        return "%s v%d.%s, v%d.%s" % (op, d, a, n, a)
    if k == 4:
        b = rng.choice(["8b", "16b"])
        op = rng.choice(["not", "cnt", "rbit", "rev16", "rev32"])
        if op == "rev32":
            b = rng.choice(["8b", "16b", "4h", "8h"])
        return "%s v%d.%s, v%d.%s" % (op, d, b, n, b)
    if k == 5:
        b = rng.choice(["8b", "16b", "4h", "8h", "4s"])
        op = rng.choice(["addv", "smaxv", "uminv", "umaxv", "sminv", "saddlv", "uaddlv"])
        reg = {"8b": "b", "16b": "b", "4h": "h", "8h": "h", "4s": "s"}[b]
        if op in ("saddlv", "uaddlv"):
            reg = {"b": "h", "h": "s", "s": "d"}[reg]
        return "%s %s%d, v%d.%s" % (op, reg, d, n, b)
    if k == 6:
        nar, wid = rng.choice([("8b", "8h"), ("4h", "4s"), ("2s", "2d")])
        nar2 = {"8b": "16b", "4h": "8h", "2s": "4s"}[nar]
        op = rng.choice(["saddl", "uaddl", "ssubl", "usubl", "smull", "umull", "smlal", "umlal", "smlsl", "umlsl",
                         "sabdl", "uabdl", "sabal", "uabal"])
        if rng.random() < 0.5:
            return "%s v%d.%s, v%d.%s, v%d.%s" % (op, d, wid, n, nar, m, nar)
        return "%s2 v%d.%s, v%d.%s, v%d.%s" % (op, d, wid, n, nar2, m, nar2)
    if k == 7:
        nar, wid = rng.choice([("8b", "8h"), ("4h", "4s"), ("2s", "2d")])
        nar2 = {"8b": "16b", "4h": "8h", "2s": "4s"}[nar]
        op = rng.choice(["saddw", "uaddw", "ssubw", "usubw"])
        return "%s v%d.%s, v%d.%s, v%d.%s" % (op, d, wid, n, wid, m, nar) if rng.random() < 0.5 else \
            "%s2 v%d.%s, v%d.%s, v%d.%s" % (op, d, wid, n, wid, m, nar2)
    if k == 8:
        arr = rng.choice(ARR_I + ["2d"])
        bits = {"8b": 8, "16b": 8, "4h": 16, "8h": 16, "2s": 32, "4s": 32, "2d": 64}[arr]
        op = rng.choice(["sshr", "ushr", "ssra", "usra", "shl", "sli", "sri", "srshr", "urshr"])
        sh = rng.randrange(1, bits + 1) if op not in ("shl", "sli") else rng.randrange(bits)
        return "%s v%d.%s, v%d.%s, #%d" % (op, d, arr, n, arr, sh)
    if k == 9:
        nar, wid, bits = rng.choice([("8b", "8h", 8), ("4h", "4s", 16), ("2s", "2d", 32)])
        nar2 = {"8b": "16b", "4h": "8h", "2s": "4s"}[nar]
        op = rng.choice(["shrn", "rshrn", "sqshrn", "uqshrn", "sqxtn", "uqxtn", "xtn", "sshll", "ushll"])
        if op in ("sshll", "ushll"):
            sh = rng.randrange(bits)
            return "%s v%d.%s, v%d.%s, #%d" % (op, d, wid, n, nar, sh) if rng.random() < 0.5 else \
                "%s2 v%d.%s, v%d.%s, #%d" % (op, d, wid, n, nar2, sh)
        if op in ("sqxtn", "uqxtn", "xtn"):
            return "%s v%d.%s, v%d.%s" % (op, d, nar, n, wid) if rng.random() < 0.5 else \
                "%s2 v%d.%s, v%d.%s" % (op, d, nar2, n, wid)
        sh = rng.randrange(1, bits + 1)
        return "%s v%d.%s, v%d.%s, #%d" % (op, d, nar, n, wid, sh) if rng.random() < 0.5 else \
            "%s2 v%d.%s, v%d.%s, #%d" % (op, d, nar2, n, wid, sh)
    if k == 10:
        arr = rng.choice(ARR_I + ["2d"])
        return "%s v%d.%s, v%d.%s, v%d.%s" % (rng.choice(["zip1", "zip2", "uzp1", "uzp2", "trn1", "trn2"]), d, arr, n,
                                               arr, m, arr)
    if k == 11:
        q = rng.random() < 0.5
        return "ext v%d.%s, v%d.%s, v%d.%s, #%d" % (d, "16b" if q else "8b", n, "16b" if q else "8b", m,
                                                    "16b" if q else "8b", rng.randrange(16 if q else 8))
    if k == 12:
        sz, lanes = rng.choice([("b", 16), ("h", 8), ("s", 4), ("d", 2)])
        arr = {"b": "16b", "h": "8h", "s": "4s", "d": "2d"}[sz]
        op = rng.randrange(6)
        if op == 0:
            return "dup v%d.%s, v%d.%s[%d]" % (d, arr, n, sz, rng.randrange(lanes))
        if op == 1:
            return "dup v%d.%s, %s" % (d, arr, x() if sz == "d" else w())
        if op == 2:
            return "ins v%d.%s[%d], v%d.%s[%d]" % (d, sz, rng.randrange(lanes), n, sz, rng.randrange(lanes))
        if op == 3:
            return "ins v%d.%s[%d], %s" % (d, sz, rng.randrange(lanes), x() if sz == "d" else w())
        if op == 4:
            return "umov %s, v%d.%s[%d]" % (x() if sz == "d" else w(), n, sz, rng.randrange(lanes))
        if sz == "d":
            sz, lanes = "s", 4
        return "smov %s, v%d.%s[%d]" % (x(), n, sz, rng.randrange(lanes))
    if k == 13:
        op = rng.randrange(5)
        if op == 0:
            return "movi v%d.%s, #%d" % (d, rng.choice(["8b", "16b"]), rng.randrange(256))
        if op == 1:
            arr = rng.choice(["4h", "8h", "2s", "4s"])
            return "%s v%d.%s, #%d, lsl #%d" % (rng.choice(["movi", "mvni", "orr", "bic"]), d, arr, rng.randrange(256),
                                                rng.choice([0, 8]))
        if op == 2:
            v = 0
            for i in range(8):
                v |= (0xff if rng.random() < 0.5 else 0) << (8 * i)
            return "movi v%d.2d, #0x%x" % (d, v)
        if op == 3:
            return "fmov v%d.%s, #%s" % (d, rng.choice(["2s", "4s", "2d"]),
                                         rng.choice(["1.0", "-2.5", "0.125", "31.0"]))
        return "movi v%d.4s, #%d, msl #%d" % (d, rng.randrange(256), rng.choice([8, 16]))
    if k == 14:
        arr = rng.choice(["4h", "8h", "2s", "4s"])
        lanes = {"4h": 8, "8h": 8, "2s": 4, "4s": 4}[arr]
        el = "h" if arr[-1] == "h" else "s"
        mm = rng.randrange(16) if el == "h" else m
        return "%s v%d.%s, v%d.%s, v%d.%s[%d]" % (rng.choice(["mul", "mla", "mls"]), d, arr, n, arr, mm, el,
                                                 rng.randrange(lanes))
    if k == 15:
        nar, wid, el, lanes = rng.choice([("4h", "4s", "h", 8), ("2s", "2d", "s", 4)])
        mm = rng.randrange(16) if el == "h" else m
        op = rng.choice(["smull", "umull", "smlal", "umlal", "smlsl", "umlsl"])
        return "%s v%d.%s, v%d.%s, v%d.%s[%d]" % (op, d, wid, n, nar, mm, el, rng.randrange(lanes))
    if k == 16:
        b = rng.choice(["8b", "16b"])
        return "%s v%d.%s, {v%d.16b}, v%d.%s" % (rng.choice(["tbl", "tbx"]), d, b, n, m, b)
    if k == 17:  # scalar integer AdvSIMD
        op = rng.choice(["add", "sub", "cmeq", "cmgt", "cmhi", "cmtst", "ushl", "sshl"])
        return "%s d%d, d%d, d%d" % (op, d, n, m)
    if k == 18:
        return "%s d%d, d%d, #%d" % (rng.choice(["shl", "ushr", "sshr", "usra"]), d, n, rng.randrange(1, 64))
    if k == 19:
        return "addp d%d, v%d.2d" % (d, n)
    # floating point
    arr = rng.choice(ARR_F)
    if k == 20:
        op = rng.choice(["fadd", "fsub", "fmul", "fdiv", "fabd", "fmla", "fmls", "fcmeq", "fcmge", "fcmgt", "facge",
                         "facgt", "faddp", "frecps", "frsqrts", "fmulx"] +
                        (["fmax", "fmin", "fmaxnm", "fminnm", "fmaxp", "fminnmp"] * 3 if MINMAX else []))
        return "%s v%d.%s, v%d.%s, v%d.%s" % (op, d, arr, n, arr, m, arr)
    if k == 21:
        op = rng.choice(["fabs", "fneg", "fsqrt", "frintn", "frinta", "frintm", "frintp", "frintz", "frintx",
                         "frinti", "fcvtzs", "fcvtzu", "fcvtns", "fcvtas", "fcvtms", "fcvtpu", "scvtf", "ucvtf"])
        return "%s v%d.%s, v%d.%s" % (op, d, arr, n, arr)
    if k == 22:
        op = rng.choice(["fcmeq", "fcmge", "fcmgt", "fcmle", "fcmlt"])
        return "%s v%d.%s, v%d.%s, #0.0" % (op, d, arr, n, arr)
    if k == 23:
        t = rng.choice(["s", "d"])
        op = rng.choice(["fadd", "fsub", "fmul", "fdiv", "fnmul", "fabd"] +
                        (["fmax", "fmin", "fmaxnm", "fminnm"] * 2 if MINMAX else []))
        return "%s %s%d, %s%d, %s%d" % (op, t, d, t, n, t, m)
    if k == 24:
        t = rng.choice(["s", "d"])
        op = rng.randrange(9)
        if op == 0:
            return "%s %s%d, %s%d, %s%d, %s%d" % (rng.choice(["fmadd", "fmsub", "fnmadd", "fnmsub"]), t, d, t, n, t, m,
                                                 t, vreg())
        if op == 1:
            return "%s %s%d, %s%d" % (rng.choice(["fcmp", "fcmpe"]), t, n, t, m)
        if op == 2:
            return "fcsel %s%d, %s%d, %s%d, %s" % (t, d, t, n, t, m, rng.choice(CONDS))
        if op == 3:
            return "%s %s%d, %s%d" % (rng.choice(["fabs", "fneg", "fsqrt", "frintn", "frinta", "frintm", "frintp",
                                                  "frintz", "frintx", "frinti", "fmov"]), t, d, t, n)
        if op == 4:
            return "fcvt %s%d, %s%d" % ("d" if t == "s" else "s", d, t, n)
        if op == 5:
            return "%s %s, %s%d" % (rng.choice(["fcvtzs", "fcvtzu", "fcvtns", "fcvtas", "fcvtms", "fcvtps", "fcvtnu",
                                                "fcvtau"]), r(rng.random() < 0.5), t, n)
        if op == 6:
            return "%s %s%d, %s" % (rng.choice(["scvtf", "ucvtf"]), t, d, r(rng.random() < 0.5))
        if op == 7:
            return "fccmp %s%d, %s%d, #%d, %s" % (t, n, t, m, rng.randrange(16), rng.choice(CONDS))
        return "fmov %s%d, #%s" % (t, d, rng.choice(["1.0", "-0.5", "2.0", "17.0"]))
    # moves between the register files
    op = rng.randrange(4)
    if op == 0:
        return "fmov %s, d%d" % (x(), n)
    if op == 1:
        return "fmov d%d, %s" % (d, x())
    if op == 2:
        return "fmov %s, s%d" % (w(), n)
    return "fmov v%d.d[1], %s" % (d, x())


class Body:
    def __init__(self, fn):
        self.fn = fn
        self.lines = []
        self.labels = 0

    def label(self):
        self.labels += 1
        return ".L%d_%d" % (self.fn, self.labels)

    def emit(self, s):
        for part in s.split("\n"):
            self.lines.append("    " + part.strip() if not part.strip().endswith(":") else part.strip())


def straight(body, n):
    for _ in range(n):
        k = rng.random()
        body.emit(integer() if k < 0.45 else memory() if k < 0.65 else simd())


def control(body, in_loop=False):
    k = rng.randrange(7)
    if (k == 1 and in_loop) or (k == 2 and MINMAX):  # one loop counter; see MINMAX
        k = 0
    if k == 0:  # a forward conditional branch over a few instructions
        skip = body.label()
        form = rng.randrange(4)
        if form == 0:
            body.emit("b.%s %s" % (rng.choice(CONDS[:-1]), skip))
        elif form == 1:
            body.emit("%s %s, %s" % (rng.choice(["cbz", "cbnz"]), r(rng.random() < 0.5), skip))
        else:
            body.emit("%s %s, #%d, %s" % (rng.choice(["tbz", "tbnz"]), x(), rng.randrange(64), skip))
        straight(body, rng.randrange(1, 4))
        body.emit(skip + ":")
    elif k == 1:  # a counted loop
        top = body.label()
        body.emit("mov w27, #%d" % rng.randrange(1, 5))
        body.emit(top + ":")
        straight(body, rng.randrange(1, 5))
        if rng.random() < 0.5:
            control(body, True)
        body.emit("subs w27, w27, #1")
        body.emit("b.ne " + top)
    elif k == 2:  # a jump through a register to a label in this function
        to = body.label()
        reg = rng.choice(DATA)
        body.emit("adr x%d, %s" % (reg, to))
        body.emit("br x%d" % reg)
        straight(body, 1)  # skipped
        body.emit(to + ":")
    elif k == 3:  # if / else
        other, done = body.label(), body.label()
        body.emit("b.%s %s" % (rng.choice(CONDS[:-1]), other))
        straight(body, rng.randrange(1, 3))
        body.emit("b " + done)
        body.emit(other + ":")
        straight(body, rng.randrange(1, 3))
        body.emit(done + ":")
    elif k == 4:  # a call to a helper that leaves flags and registers to its caller
        if rng.random() < 0.5:
            body.emit("bl fuzz_helper_%d" % rng.randrange(len(HELPERS)))
        else:
            reg = rng.choice(DATA)
            body.emit("adr x%d, fuzz_helper_%d" % (reg, rng.randrange(len(HELPERS))))
            body.emit("blr x%d" % reg)
    else:
        straight(body, rng.randrange(2, 6))


# Helpers as LLVM's machine outliner makes them: they set or read the
# caller's flags, and change registers no calling convention would let them.
HELPERS = [
    "cmp x3, x4\n    ret",
    "adds x5, x5, x6\n    csel x7, x8, x9, mi\n    ret",
    "csinc x10, x11, x12, ne\n    ret",
    "ldr x13, [x28, #64]\n    add x13, x13, x14\n    str x13, [x28, #72]\n    ret",
    "fadd d1, d2, d3\n    fcmp d1, d4\n    ret",
    "mov x15, x30\n    bl fuzz_helper_0\n    mov x30, x15\n    ccmp x16, x17, #4, eq\n    ret",
]

out = []
out.append("// Generated by windows/test/guest/gen_arm64_fuzz.py %s %s %s: do not edit." % tuple(sys.argv[1:4]))
out.append(".text")
out.append(".arch armv8.2-a+crc+lse+rcpc")
for i, h in enumerate(HELPERS):
    out.append(".p2align 2")
    out.append(".type fuzz_helper_%d, %%function" % i)
    out.append("fuzz_helper_%d:" % i)
    out.append("    " + h)
    out.append(".size fuzz_helper_%d, .-fuzz_helper_%d" % (i, i))

for f in range(N_FUNCS):
    MINMAX = rng.random() < 0.3
    body = Body(f)
    while len(body.lines) < N_INSNS:
        control(body)
    name = "fuzz_%d" % f
    out.append(".p2align 2")
    if MINMAX:
        out.append(".globl %s_jitnan" % name)
        out.append("%s_jitnan:" % name)
    out.append(".globl %s" % name)
    out.append(".type %s, %%function" % name)
    out.append(name + ":")
    out.append("""    stp x29, x30, [sp, #-176]!
    stp x19, x20, [sp, #16]
    stp x21, x22, [sp, #32]
    stp x23, x24, [sp, #48]
    stp x25, x26, [sp, #64]
    stp x27, x28, [sp, #80]
    stp d8, d9, [sp, #96]
    stp d10, d11, [sp, #112]
    stp d12, d13, [sp, #128]
    stp d14, d15, [sp, #144]
    mov x29, x0
    mov x28, x1
    ldr x2, [x29, #256]
    msr nzcv, x2""")
    for i in range(2, 28, 2):
        out.append("    ldp x%d, x%d, [x29, #%d]" % (i, i + 1, 8 * i))
    for i in range(0, 32, 2):
        out.append("    ldp q%d, q%d, [x29, #%d]" % (i, i + 1, 272 + 16 * i))
    out.extend(body.lines)
    for i in range(2, 28, 2):
        out.append("    stp x%d, x%d, [x29, #%d]" % (i, i + 1, 8 * i))
    for i in range(0, 32, 2):
        out.append("    stp q%d, q%d, [x29, #%d]" % (i, i + 1, 272 + 16 * i))
    out.append("""    mrs x2, nzcv
    str x2, [x29, #256]
    ldp d14, d15, [sp, #144]
    ldp d12, d13, [sp, #128]
    ldp d10, d11, [sp, #112]
    ldp d8, d9, [sp, #96]
    ldp x27, x28, [sp, #80]
    ldp x25, x26, [sp, #64]
    ldp x23, x24, [sp, #48]
    ldp x21, x22, [sp, #32]
    ldp x19, x20, [sp, #16]
    ldp x29, x30, [sp], #176
    ret""")
    out.append(".size %s, .-%s" % (name, name))

print("\n".join(out))
