package dalvik.system;

// Just what android.graphics and android.text touch of ART's VMRuntime, on HotSpot.
public final class VMRuntime {
    private static final VMRuntime THE_ONE = new VMRuntime();

    public static VMRuntime getRuntime() { return THE_ONE; }

    public Object newUnpaddedArray(Class<?> c, int n) { return java.lang.reflect.Array.newInstance(c, n); }

    // ART hands native code the address of a non-movable array; HotSpot moves arrays, so, as
    // Robolectric's ShadowVMRuntime does, back each one with a direct buffer and give out that
    // buffer's address. Native writes land in the buffer, not the array: enough for what the
    // graphics natives use it for (scratch arrays they fill and read themselves).
    private final java.util.Map<Object, java.nio.ByteBuffer> backing =
            java.util.Collections.synchronizedMap(new java.util.IdentityHashMap<>());

    public Object newNonMovableArray(Class<?> c, int n) {
        Object a = java.lang.reflect.Array.newInstance(c, n);
        int unit = c == byte.class || c == boolean.class ? 1
                : c == short.class || c == char.class ? 2
                : c == long.class || c == double.class ? 8 : 4;
        backing.put(a, java.nio.ByteBuffer.allocateDirect(Math.max(1, n * unit)));
        return a;
    }

    public long addressOf(Object a) { return bufferAddress(backing.get(a)); }

    // In the redirect agent (redirect_agent.c), which the test loads first.
    private static native long bufferAddress(java.nio.ByteBuffer b);

    public int getTargetSdkVersion() { return 34; }
    public static int getCurrentTargetSdkVersion() { return 34; }
    public void registerNativeAllocation(long b) {}
    public void registerNativeFree(long b) {}
    public void registerNativeAllocation(int b) {}
    public void registerNativeFree(int b) {}
    public boolean is64Bit() { return true; }
    public static boolean isJavaDebuggable() { return false; }
    public void notifyNativeAllocation() {}
}
