// Compat -- what AOSP's resources classes call that Win.apk's own classes (or HotSpot) do not have.
// AospResRewrite.java points those calls and fields here; the list is the result of a member scan of
// the taken classes against android-framework.jar, so each one below is a call that would otherwise
// end in NoSuchMethodError or NoSuchFieldError.
package com.winapk.aospres;

import android.content.res.FontScaleConverter;
import android.util.DisplayMetrics;

import dalvik.system.VMRuntime;

import java.lang.ref.WeakReference;
import java.lang.reflect.Field;
import java.util.Map;
import java.util.WeakHashMap;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicLong;

public final class Compat {
    private Compat() {}

    /** Build.VERSION.RESOURCES_SDK_INT: the level of the resource classes and the framework's table
     *  (android-all 14), which selects -vNN resources, as a device of that level does. */
    public static final int RESOURCES_SDK_INT = 34;

    // ActivityInfo.CONFIG_NATIVE_BITS (API 34): the native bit of each Java CONFIG_* bit, in order.
    private static final int[] CONFIG_NATIVE_BITS = {
        0x0002, 0x0001, 0x0004, 0x0008, 0x0010, 0x0020, 0x0040, 0x0080,
        0x0800, 0x1000, 0x0200, 0x2000, 0x0100, 0x4000, 0x10000, 0x20000,
    };

    /** ActivityInfo.activityInfoConfigNativeToJava. */
    public static int activityInfoConfigNativeToJava(int input) {
        int output = 0;
        for (int i = 0; i < CONFIG_NATIVE_BITS.length; i++) {
            if ((input & CONFIG_NATIVE_BITS[i]) != 0) output |= (1 << i);
        }
        return output;
    }

    /** StrictMode.noteResourceMismatch: Win.apk runs no StrictMode policy. */
    public static void noteResourceMismatch(Object tag) {}

    /** TextUtils.emptyIfNull. */
    public static String emptyIfNull(String s) { return s == null ? "" : s; }

    /** TextUtils.formatSimple: %s, %d, %x and %% (what AOSP's own takes), as String.format does them. */
    public static String formatSimple(String format, Object... args) {
        return String.format(java.util.Locale.ROOT, format, args);
    }

    /** ActivityThread.isSystem: an app's process is never the system server. */
    public static boolean isSystem() { return false; }

    // TypedArray hands AssetManager.nativeApplyStyle the addresses of its two int[]s (ART's
    // non-movable arrays); the Java bodies of the natives write into the arrays themselves. Win.apk's
    // VMRuntime has no addresses (addressOf is 0 there), so the resources classes' calls come here:
    // a number per array, and the way back to it.
    private static final AtomicLong NEXT = new AtomicLong(0x1000);
    private static final Map<Object, Long> ADDRESS = new WeakHashMap<>();  // arrays: identity equality
    private static final Map<Long, WeakReference<Object>> BY_ADDRESS = new ConcurrentHashMap<>();

    /** VMRuntime.addressOf, for the resources classes. */
    public static long addressOf(VMRuntime runtime, Object array) {
        if (array == null) return 0;
        synchronized (ADDRESS) {
            Long have = ADDRESS.get(array);
            if (have != null) return have;
            long a = NEXT.getAndAdd(16);
            ADDRESS.put(array, a);
            BY_ADDRESS.put(a, new WeakReference<>(array));
            if ((a & 0xfff0) == 0) BY_ADDRESS.values().removeIf(r -> r.get() == null);
            return a;
        }
    }

    /** The array addressOf numbered. */
    public static Object objectForAddress(long address) {
        WeakReference<Object> r = BY_ADDRESS.get(address);
        return r == null ? null : r.get();
    }

    // DisplayMetrics.fontScaleConverter (API 34; the non-linear font scaling above 1.3x): Win.apk's
    // DisplayMetrics has no such field, and its TypedValue, which turns sp into pixels for
    // TypedArray too, scales linearly. ResourcesImpl's converter would have no reader, so it is not
    // kept: what Win.apk's framework does today, now said in one place.
    public static FontScaleConverter fontScaleConverter(DisplayMetrics dm) { return null; }

    public static void setFontScaleConverter(DisplayMetrics dm, FontScaleConverter c) {}

    private static final Map<String, Field> FIELDS = new ConcurrentHashMap<>();

    /** A long field of AOSP's object by name (the native handle the JNI code reads). */
    static long longField(Object o, String name) {
        Field f = FIELDS.computeIfAbsent(o.getClass().getName() + "." + name, k -> {
            for (Class<?> c = o.getClass(); c != null; c = c.getSuperclass()) {
                try {
                    Field g = c.getDeclaredField(name);
                    g.setAccessible(true);
                    return g;
                } catch (NoSuchFieldException ignored) {
                }
            }
            throw new IllegalStateException("no field " + name + " in " + o.getClass());
        });
        try {
            return f.getLong(o);
        } catch (IllegalAccessException e) {
            throw new IllegalStateException(e);
        }
    }
}
