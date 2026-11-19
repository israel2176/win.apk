package org.robolectric.shadows;

import org.xmlpull.v1.XmlPullParserException;

/** Java bodies for android.content.res.XmlBlock's 22 natives (API 34): libandroidfw's ResXMLTree and
 *  ResXMLParser, here Robolectric's port of them through its shadow's statics. */
public final class BridgeXmlBlock {
    private BridgeXmlBlock() {}

    public static long nativeCreate(byte[] b, int off, int len) { return ShadowXmlBlock.nativeCreate(b, off, len); }
    public static long nativeGetStringBlock(long p) { return ShadowXmlBlock.nativeGetStringBlock(p); }
    public static long nativeCreateParseState(long p, int resId) { return ShadowXmlBlock.nativeCreateParseState(p, resId); }
    public static void nativeDestroyParseState(long p) { ShadowXmlBlock.nativeDestroyParseState(p); }
    public static void nativeDestroy(long p) { ShadowXmlBlock.nativeDestroy(p); }
    public static int nativeGetAttributeIndex(long p, String ns, String name) {
        return ShadowXmlBlock.nativeGetAttributeIndex(p, ns, name);
    }
    public static int nativeNext(long p) throws XmlPullParserException { return ShadowXmlBlock.nativeNext(p); }
    public static int nativeGetNamespace(long p) { return ShadowXmlBlock.nativeGetNamespace(p); }
    public static int nativeGetName(long p) { return ShadowXmlBlock.nativeGetName(p); }
    public static int nativeGetText(long p) { return ShadowXmlBlock.nativeGetText(p); }
    public static int nativeGetLineNumber(long p) { return ShadowXmlBlock.nativeGetLineNumber(p); }
    public static int nativeGetAttributeCount(long p) { return ShadowXmlBlock.nativeGetAttributeCount(p); }
    public static int nativeGetAttributeNamespace(long p, int i) { return ShadowXmlBlock.nativeGetAttributeNamespace(p, i); }
    public static int nativeGetAttributeName(long p, int i) { return ShadowXmlBlock.nativeGetAttributeName(p, i); }
    public static int nativeGetAttributeResource(long p, int i) { return ShadowXmlBlock.nativeGetAttributeResource(p, i); }
    public static int nativeGetAttributeDataType(long p, int i) { return ShadowXmlBlock.nativeGetAttributeDataType(p, i); }
    public static int nativeGetAttributeData(long p, int i) { return ShadowXmlBlock.nativeGetAttributeData(p, i); }
    public static int nativeGetAttributeStringValue(long p, int i) { return ShadowXmlBlock.nativeGetAttributeStringValue(p, i); }
    public static int nativeGetIdAttribute(long p) { return ShadowXmlBlock.nativeGetIdAttribute(p); }
    public static int nativeGetClassAttribute(long p) { return ShadowXmlBlock.nativeGetClassAttribute(p); }
    public static int nativeGetStyleAttribute(long p) { return ShadowXmlBlock.nativeGetStyleAttribute(p); }
    public static int nativeGetSourceResId(long p) { return ShadowXmlBlock.nativeGetSourceResId(p); }
}
