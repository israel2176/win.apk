// Guava's Bytes.indexOf.
package com.winapk.aospres.guava.primitives;

public final class Bytes {
    private Bytes() {}

    public static int indexOf(byte[] a, byte target) {
        for (int i = 0; i < a.length; i++) {
            if (a[i] == target) return i;
        }
        return -1;
    }
}
