"""Writes the C of a guest library for run_aot_worker_wine.sh: <n> functions
of about 250 arm64 instructions each (at -O0), every fourth calling the one
before it.

usage: python3 gen_guest.py <n> > guest.c
"""
import sys

n = int(sys.argv[1])
for i in range(n):
    print("unsigned long f%d(unsigned long a, unsigned long b) {" % i)
    print("  for (int k = 0; k < 8; ++k) {")
    for j in range(12):
        print("    a = a * %d + (b ^ (a >> %d)); b += a << %d;" % (2 * i + 3 + j, 1 + j % 13, 1 + (i + j) % 7))
    print("  }")
    print("  return a ^ b%s;" % (" ^ f%d(b, a)" % (i - 1) if i % 4 == 0 and i > 0 else ""))
    print("}")
