// Guava's Ints.fromBytes (big-endian, as Guava's).
package com.winapk.aospres.guava.primitives;

public final class Ints {
    private Ints() {}

    public static int fromBytes(byte b1, byte b2, byte b3, byte b4) {
        return b1 << 24 | (b2 & 0xff) << 16 | (b3 & 0xff) << 8 | (b4 & 0xff);
    }
}
