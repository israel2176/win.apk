// Guava's Shorts.fromBytes (big-endian, as Guava's).
package com.winapk.aospres.guava.primitives;

public final class Shorts {
    private Shorts() {}

    public static short fromBytes(byte b1, byte b2) { return (short) ((b1 << 8) | (b2 & 0xff)); }
}
