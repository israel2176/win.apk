package libcore.util;
import java.lang.ref.Cleaner;
// libcore's registry runs the native free function through sun.misc.Cleaner, which HotSpot does not
// have; java.lang.ref.Cleaner does the same job. applyFreeFunction is bound by the native runtime.
public class NativeAllocationRegistry {
  private static final Cleaner CLEANER = Cleaner.create();
  private final long freeFunction;
  public static NativeAllocationRegistry createNonmalloced(ClassLoader l, long f, long s) { return new NativeAllocationRegistry(l, f, s); }
  public static NativeAllocationRegistry createMalloced(ClassLoader l, long f, long s) { return new NativeAllocationRegistry(l, f, s); }
  public static NativeAllocationRegistry createMalloced(ClassLoader l, long f) { return new NativeAllocationRegistry(l, f, 0); }
  public NativeAllocationRegistry(ClassLoader l, long f, long s) { freeFunction = f; }
  public Runnable registerNativeAllocation(Object referent, long ptr) {
    final long fn = freeFunction;
    Cleaner.Cleanable c = CLEANER.register(referent, () -> applyFreeFunction(fn, ptr));
    return c::clean;
  }
  public static native void applyFreeFunction(long f, long p);
}
