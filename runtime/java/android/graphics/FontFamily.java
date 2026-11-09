// android.graphics.FontFamily -- the hidden, deprecated builder of one font
// family that AOSP still ships (frameworks/base/graphics/java/android/graphics/
// FontFamily.java, @UnsupportedAppUsage) and that androidx.core reflects on:
// TypefaceCompatApi21Impl (addFontWeightStyle(String, int, boolean)),
// TypefaceCompatApi24Impl (addFontWeightStyle(ByteBuffer, int, List, int,
// boolean)) and TypefaceCompatApi26Impl/28Impl (addFontFromAssetManager,
// addFontFromBuffer, freeze, abortCreation, then
// Typeface.createFromFamiliesWithDefault). Without the class, androidx logged
// "Unable to collect necessary methods" (DroidFish) and fell back to copying
// every font to a temporary file.
//
// Each added font is loaded into a face here (MacApkFont, as createFromFile
// does) with the weight and slant the caller gave, or the font's own OS/2
// values for RESOLVE_BY_FONT_TABLE (-1), which is what minikin records for it.
// freeze() answers false for a family with no font, as nCreateFamily returns 0
// for one, and adding to a frozen family throws IllegalStateException.
package android.graphics;

import android.content.res.AssetManager;
import android.graphics.fonts.FontVariationAxis;

import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.List;

@Deprecated
public class FontFamily {
    /** AOSP Typeface/FontFamily's RESOLVE_BY_FONT_TABLE. */
    private static final int RESOLVE_BY_FONT_TABLE = -1;

    /** AOSP's handle, non-zero once frozen with at least one font. Apps test it. */
    public long mNativePtr;
    private boolean mBuilding = true;
    private final ArrayList<Object[]> mFaces = new ArrayList<>();   // {weight, italic, java.awt.Font}

    public FontFamily() { }

    public FontFamily(String[] langs, int variant) { }

    /** The faces, in add order, as MacApkFontFamilies.registerAppFamily takes them. */
    List<Object[]> macapkFaces() { return mFaces; }

    public boolean freeze() {
        if (!mBuilding) throw new IllegalStateException("This FontFamily is already frozen");
        mBuilding = false;
        mNativePtr = mFaces.isEmpty() ? 0 : 1;
        return mNativePtr != 0;
    }

    public void abortCreation() {
        if (!mBuilding) throw new IllegalStateException("This FontFamily is already frozen or abandoned");
        mBuilding = false;
        mFaces.clear();
    }

    public boolean addFont(String path, int ttcIndex, FontVariationAxis[] axes, int weight, int italic) {
        checkBuilding();
        if (path == null) return false;
        return add(MacApkFont.loadFace(new java.io.File(path)), weight, italic);
    }

    public boolean addFontFromBuffer(ByteBuffer font, int ttcIndex, FontVariationAxis[] axes,
                                     int weight, int italic) {
        checkBuilding();
        return add(load(font), weight, italic);
    }

    public boolean addFontFromAssetManager(AssetManager mgr, String path, int cookie, boolean isAsset,
                                           int ttcIndex, int weight, int isItalic, FontVariationAxis[] axes) {
        checkBuilding();
        if (mgr == null || path == null) return false;
        MacApkFont.LoadedFace face = null;
        try (java.io.InputStream in = isAsset ? mgr.open(path) : mgr.openNonAsset(cookie, path)) {
            face = MacApkFont.loadFace(in);
        } catch (Throwable ignored) { }
        if (face == null && !isAsset) face = macapkFromResourcePath(path);
        return add(face, weight, isItalic);
    }

    /** API 21-25 (TypefaceCompatApi21Impl): a font file by path. */
    public boolean addFontWeightStyle(String path, int weight, boolean italic) {
        checkBuilding();
        if (path == null) return false;
        return add(MacApkFont.loadFace(new java.io.File(path)), weight, italic ? 1 : 0);
    }

    /** API 24-25 (TypefaceCompatApi24Impl): a font in a buffer. */
    public boolean addFontWeightStyle(ByteBuffer font, int ttcIndex, List<FontVariationAxis> axes,
                                      int weight, boolean italic) {
        checkBuilding();
        return add(load(font), weight, italic ? 1 : 0);
    }

    private void checkBuilding() {
        if (!mBuilding) throw new IllegalStateException("Unable to call addFont after freezing.");
    }

    private boolean add(MacApkFont.LoadedFace f, int weight, int italic) {
        if (f == null || f.font == null) return false;
        int w = (weight == RESOLVE_BY_FONT_TABLE) ? f.weight : Math.max(1, Math.min(1000, weight));
        boolean it = (italic == RESOLVE_BY_FONT_TABLE) ? f.italic : italic == 1;
        mFaces.add(new Object[] { w, it, f.font });
        return true;
    }

    private static MacApkFont.LoadedFace load(ByteBuffer font) {
        if (font == null) return null;
        ByteBuffer b = font.duplicate();
        b.rewind();
        byte[] bytes = new byte[b.remaining()];
        b.get(bytes);
        return MacApkFont.loadFace(bytes);
    }

    /** A resource file's path as TypedValue.string spells it ("res/font/x.ttf")
     *  when the apk names that entry differently (resource shrinking renames
     *  entries; ours come from the decoded tree): the font is opened through
     *  its resource id, res/<type>[-qualifiers]/<name>.<ext>, in the app. */
    private static MacApkFont.LoadedFace macapkFromResourcePath(String path) {
        try {
            java.io.File abs = new java.io.File(path);
            if (abs.isAbsolute() && abs.isFile()) return MacApkFont.loadFace(abs);
            String p = path.replace('\\', '/');
            int r = p.lastIndexOf("res/");
            if (r < 0) return null;
            String[] parts = p.substring(r + 4).split("/");
            if (parts.length != 2) return null;
            String type = parts[0];
            int dash = type.indexOf('-');
            if (dash > 0) type = type.substring(0, dash);
            String name = parts[1];
            int dot = name.indexOf('.');
            if (dot > 0) name = name.substring(0, dot);
            android.app.Application app = android.app.ActivityThread.currentApplication();
            if (app == null) return null;
            android.content.res.Resources res = app.getResources();
            int id = res.getIdentifier(name, type, app.getPackageName());
            if (id == 0) return null;
            try (java.io.InputStream in = res.openRawResource(id)) {
                return MacApkFont.loadFace(in);
            }
        } catch (Throwable t) {
            return null;
        }
    }
}
