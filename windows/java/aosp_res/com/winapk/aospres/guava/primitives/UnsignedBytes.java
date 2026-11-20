// Guava's UnsignedBytes.toInt, max and checkedCast.
package com.winapk.aospres.guava.primitives;

public final class UnsignedBytes {
    private UnsignedBytes() {}

    public static int toInt(byte b) { return b & 0xff; }

    public static byte max(byte... a) {
        if (a.length == 0) throw new IllegalArgumentException();
        int m = toInt(a[0]);
        for (byte b : a) m = Math.max(m, toInt(b));
        return (byte) m;
    }

    public static byte checkedCast(long v) {
        if ((v >> 8) != 0) throw new IllegalArgumentException("Out of range: " + v);
        return (byte) v;
    }
}
