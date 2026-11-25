// Guava's ByteArrayDataOutput, the four calls the port makes (ResourceString).
package com.winapk.aospres.guava.io;

public interface ByteArrayDataOutput {
    void write(int b);

    void write(byte[] b);

    void writeShort(int v);

    byte[] toByteArray();
}
