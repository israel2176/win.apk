// Guava's Joiner.on(char).join(Iterable).
package com.winapk.aospres.guava.base;

public final class Joiner {
    private final String separator;

    private Joiner(String separator) { this.separator = separator; }

    public static Joiner on(char separator) { return new Joiner(String.valueOf(separator)); }

    public String join(Iterable<?> parts) {
        StringBuilder sb = new StringBuilder();
        boolean first = true;
        for (Object p : parts) {
            if (p == null) throw new NullPointerException();
            if (!first) sb.append(separator);
            sb.append(p);
            first = false;
        }
        return sb.toString();
    }
}
