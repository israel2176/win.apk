// NativeXmlBlock -- the bodies of android.content.res.XmlBlock's natives (API 34): libandroidfw's
// ResXMLTree and ResXMLParser (core/jni/android_util_XmlBlock.cpp), on Robolectric's Java port of
// them. Adapted from Robolectric 4.16's ShadowXmlBlock (Apache-2.0; THIRD_PARTY_NOTICES.txt).
package com.winapk.aospres;

import static org.robolectric.res.android.Errors.NO_ERROR;

import org.robolectric.res.android.Ref;
import org.robolectric.res.android.Registries;
import org.robolectric.res.android.ResXMLParser;
import org.robolectric.res.android.ResXMLTree;
import org.robolectric.res.android.ResourceTypes.Res_value;
import org.xmlpull.v1.XmlPullParserException;

public final class NativeXmlBlock {
  private NativeXmlBlock() {}

  public static long nativeCreate(byte[] bArray, int off, int len) {
    if (bArray == null) {
      throw new NullPointerException();
    }

    int bLen = bArray.length;
    if (off < 0 || off >= bLen || len < 0 || len > bLen || (off + len) > bLen) {
      throw new IndexOutOfBoundsException();
    }

    // todo: optimize
    byte[] b = new byte[len];
    System.arraycopy(bArray, off, b, 0, len);

    ResXMLTree osb = new ResXMLTree(null);
    osb.setTo(b, len, true);
    //    env->ReleaseByteArrayElements(bArray, b, 0);

    if (osb.getError() != NO_ERROR) {
      throw new IllegalArgumentException();
    }

    return Registries.NATIVE_RES_XML_TREES.register(osb);
  }

  public static long nativeGetStringBlock(long obj) {
    ResXMLTree osb = Registries.NATIVE_RES_XML_TREES.getNativeObject(obj);
    //    if (osb == NULL) {
    //      jniThrowNullPointerException(env, NULL);
    //      return 0;
    //    }

    return osb.getStrings().getNativePtr();
  }

  public static long nativeCreateParseState(long obj) {
    ResXMLTree osb = Registries.NATIVE_RES_XML_TREES.getNativeObject(obj);
    //    if (osb == NULL) {
    //      jniThrowNullPointerException(env, NULL);
    //      return 0;
    //    }

    ResXMLParser st = new ResXMLParser(osb);
    //    if (st == NULL) {
    //      jniThrowException(env, "java/lang/OutOfMemoryError", NULL);
    //      return 0;
    //    }

    st.restart();

    return Registries.NATIVE_RES_XML_PARSERS.register(st);
  }

  public static long nativeCreateParseState(long obj, int resid) {
    ResXMLTree osb = Registries.NATIVE_RES_XML_TREES.getNativeObject(obj);
    //    if (osb == NULL) {
    //      jniThrowNullPointerException(env, NULL);
    //      return 0;
    //    }

    ResXMLParser st = new ResXMLParser(osb);
    //    if (st == NULL) {
    //      jniThrowException(env, "java/lang/OutOfMemoryError", NULL);
    //      return 0;
    //    }

    st.setSourceResourceId(resid);
    st.restart();

    return Registries.NATIVE_RES_XML_PARSERS.register(st);
  }

  public static int nativeNext(long state) throws XmlPullParserException {
    ResXMLParser st = getResXMLParser(state);
    if (st == null) {
      return ResXMLParser.event_code_t.END_DOCUMENT;
    }

    do {
      int code = st.next();
      switch (code) {
        case ResXMLParser.event_code_t.START_TAG:
          return 2;
        case ResXMLParser.event_code_t.END_TAG:
          return 3;
        case ResXMLParser.event_code_t.TEXT:
          return 4;
        case ResXMLParser.event_code_t.START_DOCUMENT:
          return 0;
        case ResXMLParser.event_code_t.END_DOCUMENT:
          return 1;
        case ResXMLParser.event_code_t.BAD_DOCUMENT:
          //                goto bad;
          throw new XmlPullParserException("Corrupt XML binary file");
        default:
          break;
      }

    } while (true);
  }

  public static int nativeGetNamespace(long state) {
    ResXMLParser resXMLParser = getResXMLParser(state);
    if (resXMLParser == null) {
      return -1;
    }
    return resXMLParser.getElementNamespaceID();
  }

  public static int nativeGetName(long state) {
    ResXMLParser resXMLParser = getResXMLParser(state);
    if (resXMLParser == null) {
      return -1;
    }
    return resXMLParser.getElementNameID();
  }

  public static int nativeGetText(long state) {
    ResXMLParser resXMLParser = getResXMLParser(state);
    if (resXMLParser == null) {
      return -1;
    }
    return resXMLParser.getTextID();
  }

  public static int nativeGetLineNumber(long state) {
    ResXMLParser resXMLParser = getResXMLParser(state);
    return resXMLParser.getLineNumber();
  }

  public static int nativeGetAttributeCount(long state) {
    ResXMLParser resXMLParser = getResXMLParser(state);
    return resXMLParser.getAttributeCount();
  }

  public static int nativeGetAttributeNamespace(long state, int idx) {
    ResXMLParser resXMLParser = getResXMLParser(state);
    return resXMLParser.getAttributeNamespaceID(idx);
  }

  public static int nativeGetAttributeName(long state, int idx) {
    ResXMLParser resXMLParser = getResXMLParser(state);
    return resXMLParser.getAttributeNameID(idx);
  }

  public static int nativeGetAttributeResource(long state, int idx) {
    ResXMLParser resXMLParser = getResXMLParser(state);
    return resXMLParser.getAttributeNameResID(idx);
  }

  public static int nativeGetAttributeDataType(long state, int idx) {
    ResXMLParser resXMLParser = getResXMLParser(state);
    return resXMLParser.getAttributeDataType(idx);
  }

  public static int nativeGetAttributeData(long state, int idx) {
    ResXMLParser resXMLParser = getResXMLParser(state);
    return resXMLParser.getAttributeData(idx);
  }

  public static int nativeGetAttributeStringValue(long state, int idx) {
    ResXMLParser resXMLParser = getResXMLParser(state);
    return resXMLParser.getAttributeValueStringID(idx);
  }

  public static int nativeGetIdAttribute(long state) {
    ResXMLParser resXMLParser = getResXMLParser(state);
    int idx = resXMLParser.indexOfID();
    return idx >= 0 ? resXMLParser.getAttributeValueStringID(idx) : -1;
  }

  public static int nativeGetClassAttribute(long state) {
    ResXMLParser resXMLParser = getResXMLParser(state);
    int idx = resXMLParser.indexOfClass();
    return idx >= 0 ? resXMLParser.getAttributeValueStringID(idx) : -1;
  }

  public static int nativeGetStyleAttribute(long state) {
    ResXMLParser resXMLParser = getResXMLParser(state);
    int idx = resXMLParser.indexOfStyle();
    if (idx < 0) {
      return 0;
    }

    final Ref<Res_value> valueRef = new Ref<>(new Res_value());
    if (resXMLParser.getAttributeValue(idx, valueRef) < 0) {
      return 0;
    }
    Res_value value = valueRef.get();

    return value.dataType == org.robolectric.res.android.ResourceTypes.Res_value.TYPE_REFERENCE
            || value.dataType == org.robolectric.res.android.ResourceTypes.Res_value.TYPE_ATTRIBUTE
        ? value.data
        : 0;
  }

  public static int nativeGetAttributeIndex(long token, String ns, String name) {
    ResXMLParser st = getResXMLParser(token);
    if (st == null || name == null) {
      throw new NullPointerException();
    }

    int nsLen = 0;
    if (ns != null) {
      nsLen = ns.length();
    }

    return st.indexOfAttribute(ns, nsLen, name, name.length());
  }

  public static int nativeGetSourceResId(long state) {
    ResXMLParser st = getResXMLParser(state);
    if (st == null) {
      return 0;
    } else {
      return st.getSourceResourceId();
    }
  }

  public static void nativeDestroyParseState(long state) {
    Registries.NATIVE_RES_XML_PARSERS.unregister(state);
  }

  public static void nativeDestroy(long obj) {
    Registries.NATIVE_RES_XML_TREES.unregister(obj);
  }

  private static ResXMLParser getResXMLParser(long state) {
    return Registries.NATIVE_RES_XML_PARSERS.peekNativeObject(state);
  }
}
