// Guava's ByteStreams.toByteArray and newDataOutput.
package com.winapk.aospres.guava.io;

import java.io.ByteArrayOutputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.UncheckedIOException;

public final class ByteStreams {
    private ByteStreams() {}

    public static byte[] toByteArray(InputStream in) throws IOException { return in.readAllBytes(); }

    public static ByteArrayDataOutput newDataOutput(int size) {
        ByteArrayOutputStream bytes = new ByteArrayOutputStream(Math.max(size, 0));
        DataOutputStream out = new DataOutputStream(bytes);
        return new ByteArrayDataOutput() {
            @Override public void write(int b) { run(() -> out.write(b)); }
            @Override public void write(byte[] b) { run(() -> out.write(b)); }
            @Override public void writeShort(int v) { run(() -> out.writeShort(v)); }
            @Override public byte[] toByteArray() { return bytes.toByteArray(); }
        };
    }

    private interface Io { void run() throws IOException; }

    private static void run(Io io) {
        try {
            io.run();
        } catch (IOException e) {
            throw new UncheckedIOException(e);  // a byte array never throws; as Guava's AssertionError
        }
    }
}
