package libcore.util;
import java.lang.ref.Cleaner;
// windows/java/aosp_gfx's stand-in (libcore's registry on java.lang.ref.Cleaner), plus one rule: a
// free function of 0 is never called. The resources bridge hands out 0 for a Theme's
// (BridgeAssetManager.nativeGetThemeFreeFunction): its themes live in a Java registry, not in C.
public class NativeAllocationRegistry {
  private static final Cleaner CLEANER = Cleaner.create();
  private final long freeFunction;
  public static NativeAllocationRegistry createNonmalloced(ClassLoader l, long f, long s) { return new NativeAllocationRegistry(l, f, s); }
  public static NativeAllocationRegistry createMalloced(ClassLoader l, long f, long s) { return new NativeAllocationRegistry(l, f, s); }
  public static NativeAllocationRegistry createMalloced(ClassLoader l, long f) { return new NativeAllocationRegistry(l, f, 0); }
  public NativeAllocationRegistry(ClassLoader l, long f, long s) { freeFunction = f; }
  public Runnable registerNativeAllocation(Object referent, long ptr) {
    final long fn = freeFunction;
    if (fn == 0) return () -> {};
    Cleaner.Cleanable c = CLEANER.register(referent, () -> applyFreeFunction(fn, ptr));
    return c::clean;
  }
  public static native void applyFreeFunction(long f, long p);
}
