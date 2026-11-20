// NativeStringBlock -- the bodies of android.content.res.StringBlock's natives (API 34):
// libandroidfw's ResStringPool, on Robolectric's Java port of it. Adapted from Robolectric 4.16's
// ShadowStringBlock (Apache-2.0; THIRD_PARTY_NOTICES.txt).
package com.winapk.aospres;

import static org.robolectric.res.android.Util.SIZEOF_INT;

import java.nio.ByteBuffer;
import org.robolectric.res.android.ResStringPool;
import org.robolectric.res.android.ResourceTypes.ResStringPool_span;

public final class NativeStringBlock {
  private NativeStringBlock() {}

  // A string pool from bytes (StringBlock(byte[])): no caller in the framework's resources path.
  public static long nativeCreate(byte[] data, int offset, int size) {
    throw new UnsupportedOperationException("StringBlock from bytes");
  }

  public static int nativeGetSize(long nativeId) {
    return ResStringPool.getNativeObject(nativeId).size();
  }

  public static String nativeGetString(long nativeId, int index) {
    return ResStringPool.getNativeObject(nativeId).stringAt(index);
  }

  public static int[] nativeGetStyle(long obj, int idx) {
    ResStringPool osb = ResStringPool.getNativeObject(obj);

    ResStringPool_span spans = osb.styleAt(idx);
    if (spans == null) {
      return null;
    }

    ResStringPool_span pos = spans;
    int num = 0;
    while (pos.name.index != ResStringPool_span.END) {
      num++;
      // pos++;
      pos = new ResStringPool_span(pos.myBuf(), pos.myOffset() + ResStringPool_span.SIZEOF);
    }

    if (num == 0) {
      return null;
    }

    // jintArray array = env->NewIntArray((num*sizeof(ResStringPool_span))/sizeof(jint));
    int[] array = new int[num * ResStringPool_span.SIZEOF / SIZEOF_INT];
    if (array == null) { // NewIntArray already threw OutOfMemoryError.
      return null;
    }

    num = 0;
    final int numInts = ResStringPool_span.SIZEOF / SIZEOF_INT;
    while (spans.name.index != ResStringPool_span.END) {
      // env->SetIntArrayRegion(array,
      //     num*numInts, numInts,
      //     (jint*)spans);
      setIntArrayRegion(array, num, numInts, spans);
      // spans++;
      spans = new ResStringPool_span(spans.myBuf(), spans.myOffset() + ResStringPool_span.SIZEOF);
      num++;
    }

    return array;
  }

  private static void setIntArrayRegion(
      int[] array, int num, int numInts, ResStringPool_span spans) {
    ByteBuffer buf = spans.myBuf();
    int startOffset = spans.myOffset();

    int start = num * numInts;
    for (int i = 0; i < numInts; i++) {
      array[start + i] = buf.getInt(startOffset + i * SIZEOF_INT);
    }
  }

  // The pools belong to their table or XML tree, which keeps them (as the port does).
  public static void nativeDestroy(long obj) {}
}
