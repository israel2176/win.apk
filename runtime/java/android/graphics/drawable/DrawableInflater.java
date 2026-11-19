package android.graphics.drawable;

/**
 * AOSP's DrawableInflater (@hide), which {@code Resources.getDrawableInflater()} hands out and AOSP's
 * {@code Drawable.createFromXmlInnerForDensity} inflates every drawable element through. With
 * WINAPK_AOSP_GFX=1 lib/aosp-graphics.jar's copy (AOSP's own: a tag or class name to a drawable,
 * then the drawable's own inflate) is the one loaded; this one, for the Java2D side, goes to Win.apk's
 * Drawable factory, and Win.apk's framework calls it nowhere.
 */
public final class DrawableInflater {
    private final android.content.res.Resources mRes;

    public DrawableInflater(android.content.res.Resources res, ClassLoader classLoader) {
        mRes = res;
    }

    public Drawable inflateFromXml(String name, org.xmlpull.v1.XmlPullParser parser,
                                   android.util.AttributeSet attrs, android.content.res.Resources.Theme theme)
            throws org.xmlpull.v1.XmlPullParserException, java.io.IOException {
        return Drawable.createFromXmlInner(mRes, parser, attrs, theme);
    }
}
