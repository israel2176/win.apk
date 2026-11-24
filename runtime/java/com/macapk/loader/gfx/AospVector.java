// AospVector -- AOSP's own VectorDrawable, built from a <vector> element for AospGfx.
//
// Win.apk's Resources inflates drawables from a DOM of the XML itself, and its VectorDrawable has
// constructors over that DOM which AOSP's has not; AOSP's inflate() needs
// com.android.internal.R.styleable and TypedArray features (getComplexColor) this framework lacks.
// So, until Resources hands AOSP's drawables a parser (windows/README.md, Milestone 2, step 3),
// this reads the element and builds AOSP's tree through the members AOSP's own inflate and its
// animators use: VGroup, VFullPath, VClipPath, PathParser.PathData and the native tree. It must be
// a VectorDrawable, not a look-alike: AppCompat refuses to start (ResourceManagerInternal's
// checkVectorDrawableSetup) unless its test vector's class is exactly
// android.graphics.drawable.VectorDrawable. Not here yet: gradient fills, trimPath*, theme
// re-application (a `?attr` colour is resolved once, against the inflating theme).
package com.macapk.loader.gfx;

import android.content.res.ColorStateList;
import android.content.res.Resources;
import android.graphics.drawable.VectorDrawable;

import org.w3c.dom.Element;
import org.w3c.dom.Node;

import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Method;

final class AospVector {

    private static final String NS = "http://schemas.android.com/apk/res/android";
    private static final String VD = "android.graphics.drawable.VectorDrawable";

    /** AOSP's members, looked up once. */
    private static final class Aosp {
        final Class<?> group, fullPath, clipPath, vObject, pathData;
        final Constructor<?> newGroup, newFullPath, newClipPath, newPathData;
        final Method addChild, setNativePathData, pathDataPtr, updateFullPath, nativePtr, onTreeFinished,
                setViewport, updateLocalState;
        final Method[] groupSetters;  // rotation, pivotX, pivotY, scaleX, scaleY, translateX, translateY
        final Field state, rootGroup, baseWidth, baseHeight, density, pathDataField;

        Aosp() throws ReflectiveOperationException {
            group = Class.forName(VD + "$VGroup");
            fullPath = Class.forName(VD + "$VFullPath");
            clipPath = Class.forName(VD + "$VClipPath");
            vObject = Class.forName(VD + "$VObject");
            pathData = Class.forName("android.util.PathParser$PathData");
            Class<?> vpath = Class.forName(VD + "$VPath");
            Class<?> stateClass = Class.forName(VD + "$VectorDrawableState");
            newGroup = open(group.getDeclaredConstructor());
            newFullPath = open(fullPath.getDeclaredConstructor());
            newClipPath = open(clipPath.getDeclaredConstructor());
            newPathData = open(pathData.getDeclaredConstructor(String.class));
            addChild = open(group.getDeclaredMethod("addChild", vObject));
            setNativePathData = open(VectorDrawable.class.getDeclaredMethod("nSetPathData", long.class, long.class));
            pathDataPtr = open(pathData.getDeclaredMethod("getNativePtr"));
            nativePtr = open(vObject.getDeclaredMethod("getNativePtr"));
            updateFullPath = open(VectorDrawable.class.getDeclaredMethod("nUpdateFullPathProperties", long.class,
                    float.class, int.class, float.class, int.class, float.class, float.class, float.class,
                    float.class, float.class, int.class, int.class, int.class));
            onTreeFinished = open(stateClass.getDeclaredMethod("onTreeConstructionFinished"));
            setViewport = open(stateClass.getDeclaredMethod("setViewportSize", float.class, float.class));
            updateLocalState = open(VectorDrawable.class.getDeclaredMethod("updateLocalState", Resources.class));
            String[] names = {"setRotation", "setPivotX", "setPivotY", "setScaleX", "setScaleY",
                    "setTranslateX", "setTranslateY"};
            groupSetters = new Method[names.length];
            for (int i = 0; i < names.length; i++) groupSetters[i] = open(group.getDeclaredMethod(names[i], float.class));
            state = open(VectorDrawable.class.getDeclaredField("mVectorState"));
            rootGroup = open(stateClass.getDeclaredField("mRootGroup"));
            baseWidth = open(stateClass.getDeclaredField("mBaseWidth"));
            baseHeight = open(stateClass.getDeclaredField("mBaseHeight"));
            density = open(stateClass.getDeclaredField("mDensity"));
            pathDataField = open(vpath.getDeclaredField("mPathData"));
        }

        private static <T extends java.lang.reflect.AccessibleObject> T open(T member) {
            member.setAccessible(true);
            return member;
        }
    }

    private static Aosp sAosp;

    private static synchronized Aosp aosp() throws ReflectiveOperationException {
        if (sAosp == null) sAosp = new Aosp();
        return sAosp;
    }

    private final Resources res;
    private final Resources.Theme theme;
    private final Aosp a;

    private AospVector(Resources res, Resources.Theme theme, Aosp a) {
        this.res = res;
        this.theme = theme;
        this.a = a;
    }

    /** AOSP's VectorDrawable for the element, sized in pixels at the resources' density. */
    static VectorDrawable build(Element e, Resources.Theme theme, Resources res) throws ReflectiveOperationException {
        return new AospVector(res, theme, aosp()).vector(e);
    }

    private VectorDrawable vector(Element e) throws ReflectiveOperationException {
        VectorDrawable vd = new VectorDrawable();
        Object state = a.state.get(vd);
        int width = Math.round(dimension(e, "width", 0f));
        int height = Math.round(dimension(e, "height", 0f));
        float vw = number(e, "viewportWidth", 0f), vh = number(e, "viewportHeight", 0f);
        if (vw <= 0) vw = Math.max(1, width);
        if (vh <= 0) vh = Math.max(1, height);
        // The size is in pixels already, so the state's density is the screen's: no scaling after.
        int dpi = res != null && res.getDisplayMetrics() != null ? res.getDisplayMetrics().densityDpi : 160;
        a.density.setInt(state, dpi);
        a.baseWidth.setInt(state, width);
        a.baseHeight.setInt(state, height);
        a.setViewport.invoke(state, vw, vh);
        children(e, a.rootGroup.get(state));
        a.onTreeFinished.invoke(state);
        a.updateLocalState.invoke(vd, res);
        ColorStateList tint = colors(attr(e, "tint"));
        if (tint != null) vd.setTintList(tint);
        android.graphics.PorterDuff.Mode mode = mode(attr(e, "tintMode"));
        if (mode != null) vd.setTintMode(mode);
        float alpha = number(e, "alpha", 1f);
        if (alpha < 1f) vd.setAlpha(Math.round(alpha * 255));
        if ("true".equals(attr(e, "autoMirrored"))) vd.setAutoMirrored(true);
        return vd;
    }

    private void children(Element parent, Object into) throws ReflectiveOperationException {
        for (Node n = parent.getFirstChild(); n != null; n = n.getNextSibling()) {
            if (!(n instanceof Element)) continue;
            Element c = (Element) n;
            String tag = c.getLocalName() != null ? c.getLocalName() : c.getTagName();
            if (tag.equals("group")) {
                Object g = a.newGroup.newInstance();
                String[] attrs = {"rotation", "pivotX", "pivotY", "scaleX", "scaleY", "translateX", "translateY"};
                float[] defs = {0f, 0f, 0f, 1f, 1f, 0f, 0f};
                for (int i = 0; i < attrs.length; i++) {
                    float v = number(c, attrs[i], defs[i]);
                    if (v != defs[i]) a.groupSetters[i].invoke(g, v);
                }
                a.addChild.invoke(into, g);
                children(c, g);
            } else if (tag.equals("path") || tag.equals("clip-path")) {
                String data = attr(c, "pathData");
                if (data == null) continue;
                Object pathData;
                try {
                    pathData = a.newPathData.newInstance(data);
                } catch (java.lang.reflect.InvocationTargetException bad) {
                    continue;  // AOSP throws on a malformed path too; the rest of the vector still draws
                }
                if (tag.equals("clip-path")) {
                    Object clip = a.newClipPath.newInstance();
                    setPath(clip, pathData);
                    a.addChild.invoke(into, clip);
                    continue;
                }
                Object path = a.newFullPath.newInstance();
                setPath(path, pathData);
                ColorStateList fill = colors(attr(c, "fillColor"));
                ColorStateList stroke = colors(attr(c, "strokeColor"));
                int cap = choice(attr(c, "strokeLineCap"), "butt", "round", "square");
                int join = choice(attr(c, "strokeLineJoin"), "miter", "round", "bevel");
                int fillType = choice(attr(c, "fillType"), "nonZero", "evenOdd");
                a.updateFullPath.invoke(null, (Long) a.nativePtr.invoke(path),
                        number(c, "strokeWidth", 0f), stroke == null ? 0 : stroke.getDefaultColor(),
                        number(c, "strokeAlpha", 1f), fill == null ? 0 : fill.getDefaultColor(),
                        number(c, "fillAlpha", 1f), number(c, "trimPathStart", 0f), number(c, "trimPathEnd", 1f),
                        number(c, "trimPathOffset", 0f), number(c, "strokeMiterLimit", 4f), cap, join, fillType);
                a.addChild.invoke(into, path);
            }
        }
    }

    /** What VFullPath / VClipPath's inflate does with a path: keep it, and hand it to the native node. */
    private void setPath(Object node, Object pathData) throws ReflectiveOperationException {
        a.pathDataField.set(node, pathData);
        a.setNativePathData.invoke(null, (Long) a.nativePtr.invoke(node), (Long) a.pathDataPtr.invoke(pathData));
    }

    // ---- Reading the element: text XML (an app's) or the compiled platform files' DOM ----

    private static String attr(Element e, String name) {
        String v = e.getAttributeNS(NS, name);
        if (v == null || v.isEmpty()) v = e.getAttribute("android:" + name);
        return v == null || v.isEmpty() ? null : v.trim();
    }

    /** An enum attribute: its index among the names, or the number it was compiled to. */
    private static int choice(String v, String... names) {
        if (v == null) return 0;
        for (int i = 0; i < names.length; i++) if (names[i].equals(v)) return i;
        try {
            return Integer.decode(v);
        } catch (NumberFormatException nfe) {
            return 0;
        }
    }

    private float number(Element e, String name, float def) {
        String v = attr(e, name);
        if (v == null) return def;
        try {
            return Float.parseFloat(v);
        } catch (NumberFormatException nfe) {
            return dimension(e, name, def);
        }
    }

    private float dimension(Element e, String name, float def) {
        String v = attr(e, name);
        if (v == null || res == null) return def;
        return res.macapkDimensionFor(v, theme, def);
    }

    private ColorStateList colors(String v) {
        if (v == null || res == null) return null;
        try {
            if (v.startsWith("#")) return ColorStateList.valueOf(GfxBackend.get().parseResourceColor(v));
            return res.macapkTintListFor(v, theme);
        } catch (Throwable t) {
            return null;
        }
    }

    private static android.graphics.PorterDuff.Mode mode(String v) {
        if (v == null) return null;
        switch (v) {
            case "src_over": case "3": return android.graphics.PorterDuff.Mode.SRC_OVER;
            case "src_in": case "5": return android.graphics.PorterDuff.Mode.SRC_IN;
            case "src_atop": case "9": return android.graphics.PorterDuff.Mode.SRC_ATOP;
            case "multiply": case "14": return android.graphics.PorterDuff.Mode.MULTIPLY;
            case "screen": case "15": return android.graphics.PorterDuff.Mode.SCREEN;
            case "add": case "16": return android.graphics.PorterDuff.Mode.ADD;
            default: return null;
        }
    }
}
