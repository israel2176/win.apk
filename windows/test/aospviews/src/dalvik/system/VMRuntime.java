package dalvik.system;

// Just what AOSP's graphics, text, resources and views touch of ART's VMRuntime, on HotSpot. As
// windows/test/aospgfx's, plus the way back from an address to its array: TypedArray hands
// AssetManager.nativeApplyStyle the addresses of two non-movable int[]s, and the resources bridge
// (Java, not C) writes into the arrays themselves, as Robolectric's ShadowVMRuntime lets it.
public final class VMRuntime {
    private static final VMRuntime THE_ONE = new VMRuntime();

    public static VMRuntime getRuntime() { return THE_ONE; }

    public Object newUnpaddedArray(Class<?> c, int n) { return java.lang.reflect.Array.newInstance(c, n); }

    // ART hands native code the address of a non-movable array; HotSpot moves arrays, so back each
    // one with a direct buffer and give out that buffer's address. Native writes land in the
    // buffer, not the array: enough for the graphics natives (scratch arrays they fill and read).
    private static final java.util.Map<Object, java.nio.ByteBuffer> backing =
            java.util.Collections.synchronizedMap(new java.util.IdentityHashMap<>());  // kept for the run, as aospgfx's
    private static final java.util.Map<Long, java.lang.ref.WeakReference<Object>> byAddress =
            new java.util.concurrent.ConcurrentHashMap<>();

    public Object newNonMovableArray(Class<?> c, int n) {
        Object a = java.lang.reflect.Array.newInstance(c, n);
        int unit = c == byte.class || c == boolean.class ? 1
                : c == short.class || c == char.class ? 2
                : c == long.class || c == double.class ? 8 : 4;
        java.nio.ByteBuffer b = java.nio.ByteBuffer.allocateDirect(Math.max(1, n * unit));
        backing.put(a, b);
        byAddress.put(bufferAddress(b), new java.lang.ref.WeakReference<>(a));
        return a;
    }

    public long addressOf(Object a) { return bufferAddress(backing.get(a)); }

    /** The array whose address addressOf gave out (the resources bridge's way back). */
    public static Object objectForAddress(long address) {
        java.lang.ref.WeakReference<Object> r = byAddress.get(address);
        return r == null ? null : r.get();
    }

    // In the redirect agent (windows/test/aospgfx/redirect_agent.c), which the test loads first.
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
    public String vmInstructionSet() { return "x86_64"; }
}
