package org.robolectric.shadows;

/** Java bodies for android.content.res.StringBlock's natives (API 34): libandroidfw's ResStringPool,
 *  here Robolectric's port of it. */
public final class BridgeStringBlock {
    private BridgeStringBlock() {}

    public static long nativeCreate(byte[] b, int off, int len) { return ShadowStringBlock.nativeCreate(b, off, len); }
    public static int nativeGetSize(long p) { return ShadowStringBlock.nativeGetSize(p); }
    public static String nativeGetString(long p, int i) { return ShadowStringBlock.nativeGetString(p, i); }
    public static int[] nativeGetStyle(long p, int i) { return ShadowStringBlock.nativeGetStyle(p, i); }
    public static void nativeDestroy(long p) { ShadowStringBlock.nativeDestroy(p); }
}
