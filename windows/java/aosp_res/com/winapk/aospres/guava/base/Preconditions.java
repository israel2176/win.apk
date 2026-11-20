// Guava's Preconditions, the two checks the port makes.
package com.winapk.aospres.guava.base;

public final class Preconditions {
    private Preconditions() {}

    public static void checkState(boolean ok, Object message) {
        if (!ok) throw new IllegalStateException(String.valueOf(message));
    }

    public static void checkArgument(boolean ok, String template, int arg) {
        if (!ok) throw new IllegalArgumentException(template.replace("%s", Integer.toString(arg)));
    }
}
