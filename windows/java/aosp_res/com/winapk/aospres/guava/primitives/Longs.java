// Guava's Longs.fromBytes (big-endian, as Guava's).
package com.winapk.aospres.guava.primitives;

public final class Longs {
    private Longs() {}

    public static long fromBytes(byte b1, byte b2, byte b3, byte b4, byte b5, byte b6, byte b7, byte b8) {
        return (b1 & 0xffL) << 56 | (b2 & 0xffL) << 48 | (b3 & 0xffL) << 40 | (b4 & 0xffL) << 32
                | (b5 & 0xffL) << 24 | (b6 & 0xffL) << 16 | (b7 & 0xffL) << 8 | (b8 & 0xffL);
    }
}
