package com.winapk.aospgfx;

import android.content.res.AssetManager;
import android.graphics.fonts.Font;

import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;

/**
 * AOSP's Font.Builder.createBuffer as ART runs it. A font file the APK holds compressed (aapt2 compresses
 * .ttf and .otf, so most app fonts) cannot be opened as a file descriptor, and AOSP then reads the
 * stream into {@code ByteBuffer.allocateDirect(n).array()}: an ART direct buffer has a Java array
 * behind it, a HotSpot one throws UnsupportedOperationException, so with the AOSP graphics every app
 * font androidx loads (ResourcesCompat.getFont, TypefaceCompat on API 29+: Font.Builder(Resources,
 * int)) failed. build_aosp_gfx.sh points Font$Builder's createBuffer calls here
 * (scripts/AospCallRedirect.java); AOSP's own runs first, and only where it hits that does this read
 * the entry into a direct buffer itself: the same bytes in native order, what ART's buffer holds.
 */
public final class FontBuffers {
    private FontBuffers() {}

    public static ByteBuffer createBuffer(AssetManager am, String path, boolean isAsset, int cookie)
            throws IOException {
        try {
            return Font.Builder.createBuffer(am, path, isAsset, cookie);
        } catch (UnsupportedOperationException hotSpotDirectBuffer) {
            try (InputStream in = isAsset ? am.open(path, AssetManager.ACCESS_BUFFER)
                    : am.openNonAsset(cookie, path, AssetManager.ACCESS_BUFFER)) {
                byte[] bytes = in.readAllBytes();
                ByteBuffer buffer = ByteBuffer.allocateDirect(bytes.length).order(ByteOrder.nativeOrder());
                buffer.put(bytes).flip();
                return buffer;
            }
        }
    }
}
