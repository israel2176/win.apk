package libcore.util;
import java.lang.ref.Cleaner;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.function.LongConsumer;
// libcore's registry runs the native free function through sun.misc.Cleaner, which HotSpot does not
// have; java.lang.ref.Cleaner does the same job. applyFreeFunction is bound by the native runtime.
// A free function of 0 is never called. A free function registered here as Java code
// (registerJavaFreeFunction) runs in place of a C one: AOSP's resources (-Dwinapk.aosp.res=1) keep
// their themes in a Java registry, and a Theme's entry is dropped there once the Theme is
// unreachable, as ART frees the native theme (windows/java/aosp_res, NativeAssetManager).
public class NativeAllocationRegistry {
  private static final Cleaner CLEANER = Cleaner.create();
  private static final Map<Long, LongConsumer> JAVA_FREE = new ConcurrentHashMap<>();
  private final long freeFunction;
  public static NativeAllocationRegistry createNonmalloced(ClassLoader l, long f, long s) { return new NativeAllocationRegistry(l, f, s); }
  public static NativeAllocationRegistry createMalloced(ClassLoader l, long f, long s) { return new NativeAllocationRegistry(l, f, s); }
  public static NativeAllocationRegistry createMalloced(ClassLoader l, long f) { return new NativeAllocationRegistry(l, f, 0); }
  public NativeAllocationRegistry(ClassLoader l, long f, long s) { freeFunction = f; }
  /** A free function that is Java code, under a token a "native" free-function getter answers. */
  public static void registerJavaFreeFunction(long token, LongConsumer free) { JAVA_FREE.put(token, free); }
  public Runnable registerNativeAllocation(Object referent, long ptr) {
    final long fn = freeFunction;
    if (fn == 0) return () -> {};
    final LongConsumer javaFree = JAVA_FREE.get(fn);
    Cleaner.Cleanable c = CLEANER.register(referent,
        javaFree != null ? () -> javaFree.accept(ptr) : () -> applyFreeFunction(fn, ptr));
    return c::clean;
  }
  public static native void applyFreeFunction(long f, long p);
}
