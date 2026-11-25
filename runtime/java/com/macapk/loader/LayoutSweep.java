// LayoutSweep -- every layout resource of an app, inflated, measured, laid out
// and drawn, each view's place written as text. A debug mode, off unless
// MACAPK_LAYOUT_SWEEP=1: it checks every screen of an app against Android,
// including the ones normally reached only after a login, without logging in
// (the screens' layouts are all in the APK).
//
// The same file runs twice: inside Win.apk (Boot hands it the app's
// Application instead of launching an activity, see runFromBoot) and on the
// real AOSP framework under Robolectric (windows/test/screens/, which compiles
// this file into its test). So it names only android.* and java.*: nothing of
// the runtime's own. The lines are the UI probe's (windows/test/uiprobe,
// Probes.dump), so its compare.py lines the two up.
//
// Each layout is inflated in a ContextThemeWrapper over the Application with
// the app's theme, through AppCompat's view inflater when the app has one (what
// an AppCompatActivity does: <TextView> becomes AppCompatTextView, a Material
// theme's MaterialComponentsViewInflater turns <Button> into MaterialButton),
// into a FrameLayout of 360x640 dp, left to right and right to left. A layout
// that throws is a line too: "FAILED", with the root cause and where it was
// thrown -- an exception on one side only is the most useful thing it finds.
//
// What it cannot see: what code does after inflating (a list's rows, texts set
// from data, views shown or hidden), a <fragment> (there is no activity), and a
// custom view whose constructor wants state the app sets up at start.
package com.macapk.loader;

import android.content.Context;
import android.content.res.Resources;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.text.Layout;
import android.util.AttributeSet;
import android.view.ContextThemeWrapper;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.lang.reflect.Constructor;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.List;
import java.util.TreeMap;
import java.util.regex.Pattern;

public final class LayoutSweep {
    private LayoutSweep() {}

    /** The screen every layout is laid out on, in dp: the UI probe's. */
    static final int W_DP = 360, H_DP = 640;

    /** MACAPK_LAYOUT_SWEEP=1 turns the mode on. */
    public static boolean enabled() {
        return "1".equals(System.getenv("MACAPK_LAYOUT_SWEEP"));
    }

    /**
     * Win.apk's entry: called on the app's main thread once Application.onCreate
     * has returned, in place of launching the activity. Prints "PROBE <line>" for
     * every line, then "PROBE done", and ends the process.
     *
     *   MACAPK_LAYOUT_SWEEP_THEME     the theme, as a resource id (0x7f...) or a name
     *                                 (style/Theme.App); default the launcher
     *                                 activity's, else the application's
     *   MACAPK_LAYOUT_SWEEP_FILTER    a regex: only layouts whose name it finds
     *   MACAPK_LAYOUT_SWEEP_PICTURES  a folder for each case's picture as a PNG
     */
    public static void runFromBoot(Context app) {
        try {
            int theme = parseTheme(app, System.getenv("MACAPK_LAYOUT_SWEEP_THEME"));
            if (theme == 0) theme = launcherTheme(app);
            String pictures = System.getenv("MACAPK_LAYOUT_SWEEP_PICTURES");
            // Each line out as it is made: a layout that takes the process down (a native
            // crash, a hang) leaves the lines before it, and its own case line names it.
            List<String> printer = new ArrayList<String>() {
                @Override public boolean add(String line) {
                    System.out.println("PROBE " + line);
                    System.out.flush();
                    return true;
                }
            };
            run(app, theme, System.getenv("MACAPK_LAYOUT_SWEEP_FILTER"),
                pictures == null || pictures.isEmpty() ? null : new File(pictures), printer);
        } catch (Throwable t) {
            System.out.println("PROBE sweep failed " + t);
            t.printStackTrace();
        }
        System.out.println("PROBE done");
        System.out.flush();
        System.err.flush();
        Runtime.getRuntime().halt(0);
    }

    /** run() for a caller that must not name an Android class: the oracle's test class is
     *  loaded outside Robolectric's sandbox first, where there is no android.content.Context. */
    public static List<String> runOn(Object app, String theme, String filter, File pictures) {
        Context c = (Context) app;
        int t = parseTheme(c, theme);
        if (t == 0) t = launcherTheme(c);
        return run(c, t, filter, pictures);
    }

    /** A theme given as "0x7f140123", "style/Theme.App" or "Theme.App"; 0 for none. */
    static int parseTheme(Context app, String s) {
        if (s == null || s.isEmpty()) return 0;
        if (s.startsWith("0x")) return (int) Long.parseLong(s.substring(2), 16);
        String name = s.startsWith("@") ? s.substring(1) : s;
        if (name.startsWith("style/")) name = name.substring(6);
        return app.getResources().getIdentifier(name, "style", app.getPackageName());
    }

    /** The launcher activity's theme, else the application's: what the first screen wears. */
    static int launcherTheme(Context app) {
        try {
            android.content.Intent main = new android.content.Intent(android.content.Intent.ACTION_MAIN)
                .addCategory(android.content.Intent.CATEGORY_LAUNCHER).setPackage(app.getPackageName());
            List<android.content.pm.ResolveInfo> found = app.getPackageManager().queryIntentActivities(main, 0);
            if (found != null) {
                for (android.content.pm.ResolveInfo ri : found) {
                    if (ri.activityInfo != null && ri.activityInfo.getThemeResource() != 0) {
                        return ri.activityInfo.getThemeResource();
                    }
                }
            }
        } catch (Throwable ignored) { }
        return app.getApplicationInfo().theme;
    }

    /** Every layout of the app, left to right and right to left, as lines of text. */
    public static List<String> run(Context app, int theme, String filter, File pictures) {
        return run(app, theme, filter, pictures, new ArrayList<>());
    }

    private static List<String> run(Context app, int theme, String filter, File pictures, List<String> out) {
        Resources res = app.getResources();
        float density = res.getDisplayMetrics().density;
        out.add("density " + density);
        out.add("rtl-support " + ((app.getApplicationInfo().flags & android.content.pm.ApplicationInfo.FLAG_SUPPORTS_RTL) != 0));
        int w = Math.round(W_DP * density), h = Math.round(H_DP * density);

        // A launcher theme is often a splash screen's (androidx.core.splashscreen); the
        // screens behind it wear the one it names.
        int post = attr(app, theme, "postSplashScreenTheme");
        out.add("theme " + name(res, theme) + (post != 0 ? " -> " + name(res, post) : ""));
        if (post != 0) theme = post;

        // Android throws NotFoundException for an id with no resource; a name made up for
        // it instead is a difference of its own (and would make every id a layout).
        String none;
        try {
            none = "answers " + res.getResourceEntryName(0x7f00ffff);
        } catch (Resources.NotFoundException e) {
            none = "throws NotFoundException";
        } catch (Throwable t) {
            none = "throws " + t.getClass().getName();
        }
        out.add("no-such-resource " + none);
        TreeMap<String, Integer> layouts = layouts(app, theme);
        Pattern only = filter == null || filter.isEmpty() ? null : Pattern.compile(filter);
        if (only != null) layouts.keySet().removeIf(n -> !only.matcher(n).find());
        out.add("layouts " + layouts.size());
        out.add("inflater " + inflaterHow(new ContextThemeWrapper(app, theme)));
        for (java.util.Map.Entry<String, Integer> e : layouts.entrySet()) {
            for (int dir = 0; dir < 2; dir++) {
                String name = e.getKey() + (dir == 0 ? ".ltr" : ".rtl");
                out.add("case " + name);
                try {
                    Context c = new ContextThemeWrapper(app, theme);
                    LayoutInflater inflater = inflater(c);
                    FrameLayout root = new FrameLayout(c);
                    root.setLayoutDirection(dir == 0 ? View.LAYOUT_DIRECTION_LTR : View.LAYOUT_DIRECTION_RTL);
                    inflater.inflate(e.getValue(), root, true);
                    root.measure(View.MeasureSpec.makeMeasureSpec(w, View.MeasureSpec.EXACTLY),
                                 View.MeasureSpec.makeMeasureSpec(h, View.MeasureSpec.EXACTLY));
                    root.layout(0, 0, w, h);
                    Bitmap picture = null;
                    try {
                        picture = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
                        Canvas canvas = new Canvas(picture);
                        canvas.drawColor(0xFFFFFFFF);
                        root.draw(canvas);
                        if (pictures != null) {
                            pictures.mkdirs();
                            try (FileOutputStream f = new FileOutputStream(new File(pictures, name + ".png"))) {
                                picture.compress(Bitmap.CompressFormat.PNG, 100, f);
                            }
                        }
                    } catch (Throwable t) {
                        out.add("  NOT DRAWN " + t);
                        failure(t, out);
                        picture = null;
                    }
                    // A <merge> leaves several children.
                    for (int i = 0; i < root.getChildCount(); i++) {
                        dump(root.getChildAt(i), 0, 0, 0, density, out, c, picture);
                    }
                    if (picture != null) picture.recycle();
                } catch (Throwable t) {
                    out.add("  FAILED " + oneLine(t));
                    failure(t, out);
                }
            }
        }
        return out;
    }

    /** The root cause of a failure and where it was thrown, when the throwable shown is a wrapper. */
    private static void failure(Throwable t, List<String> out) {
        Throwable root = t;
        while (root.getCause() != null && root.getCause() != root) root = root.getCause();
        if (root != t) out.add("  cause " + oneLine(root));
        StackTraceElement[] st = root.getStackTrace();
        for (int i = 0; i < Math.min(3, st.length); i++) out.add("  at " + st[i]);
    }

    private static String oneLine(Throwable t) {
        return String.valueOf(t).replace('\n', ' ').replace('\r', ' ');
    }

    /** Every layout resource of the app's package, by name. There is no API that lists a
     *  package's resources (aapt2 does it offline), so walk the ids: 0x7fTTEEEE, the type
     *  whose name is "layout", its entries in order until a long run of missing ones. */
    static TreeMap<String, Integer> layouts(Context app, int theme) {
        Resources r = app.getResources();
        TreeMap<String, Integer> out = new TreeMap<>();
        // The app's package is 0x7f; a theme of the app's (not the platform's, 0x01) says
        // otherwise for an APK built with another package id.
        int pkg = theme != 0 && (theme >>> 24) != 0x01 ? theme & 0xff000000 : 0x7f000000;
        int type = 0;
        for (int t = 1; t <= 0xff && type == 0; t++) {
            for (int entry = 0; entry < 8; entry++) {
                int id = pkg | (t << 16) | entry;
                try {
                    if ("layout".equals(r.getResourceTypeName(id))) { type = t; break; }
                    break;  // a type, not the layouts
                } catch (Resources.NotFoundException ignored) { }
            }
        }
        if (type == 0) return out;
        int missing = 0;
        for (int entry = 0; entry <= 0xffff && missing < 256; entry++) {
            int id = pkg | (type << 16) | entry;
            try {
                String name = r.getResourceEntryName(id);
                if (made(name, id)) {
                    missing++;
                    continue;
                }
                out.put(name, id);
                missing = 0;
            } catch (Resources.NotFoundException e) {
                missing++;
            }
        }
        return out;
    }

    /** A name made up for an id with no resource ("id_0x7f0c0417"), not one the APK has. */
    private static boolean made(String name, int id) {
        return name == null || name.equalsIgnoreCase("id_0x" + Integer.toHexString(id));
    }

    /** The resource id an attribute of the app's package (or android:) has in a theme; 0 if none. */
    private static int attr(Context app, int theme, String name) {
        if (theme == 0) return 0;
        try {
            int attr = app.getResources().getIdentifier(name, "attr", app.getPackageName());
            if (attr == 0) return 0;
            android.util.TypedValue v = new android.util.TypedValue();
            Resources.Theme t = app.getResources().newTheme();
            t.applyStyle(theme, true);
            return t.resolveAttribute(attr, v, true) ? v.resourceId : 0;
        } catch (Throwable ignored) {
            return 0;
        }
    }

    private static String name(Resources r, int id) {
        if (id == 0) return "none";
        try {
            return r.getResourceTypeName(id) + "/" + r.getResourceEntryName(id);
        } catch (Throwable t) {
            return "0x" + Integer.toHexString(id);
        }
    }

    // ---- The view inflater an AppCompatActivity would install ----

    /** AppCompatViewInflater.createView's switch: the bare tags AppCompat hands out its own
     *  widget for. */
    private static final String[][] APPCOMPAT = {
        {"TextView", "androidx.appcompat.widget.AppCompatTextView"},
        {"ImageView", "androidx.appcompat.widget.AppCompatImageView"},
        {"Button", "androidx.appcompat.widget.AppCompatButton"},
        {"EditText", "androidx.appcompat.widget.AppCompatEditText"},
        {"Spinner", "androidx.appcompat.widget.AppCompatSpinner"},
        {"ImageButton", "androidx.appcompat.widget.AppCompatImageButton"},
        {"CheckBox", "androidx.appcompat.widget.AppCompatCheckBox"},
        {"RadioButton", "androidx.appcompat.widget.AppCompatRadioButton"},
        {"CheckedTextView", "androidx.appcompat.widget.AppCompatCheckedTextView"},
        {"AutoCompleteTextView", "androidx.appcompat.widget.AppCompatAutoCompleteTextView"},
        {"MultiAutoCompleteTextView", "androidx.appcompat.widget.AppCompatMultiAutoCompleteTextView"},
        {"RatingBar", "androidx.appcompat.widget.AppCompatRatingBar"},
        {"SeekBar", "androidx.appcompat.widget.AppCompatSeekBar"},
        {"ToggleButton", "androidx.appcompat.widget.AppCompatToggleButton"},
    };

    /** MaterialComponentsViewInflater's overrides, for a theme that names it. */
    private static final String[][] MATERIAL = {
        {"Button", "com.google.android.material.button.MaterialButton"},
        {"CheckBox", "com.google.android.material.checkbox.MaterialCheckBox"},
        {"RadioButton", "com.google.android.material.radiobutton.MaterialRadioButton"},
        {"TextView", "com.google.android.material.textview.MaterialTextView"},
        {"AutoCompleteTextView", "com.google.android.material.textfield.MaterialAutoCompleteTextView"},
    };

    /** The class an AppCompat theme names in viewInflaterClass, else AppCompatViewInflater if the
     *  app has AppCompat; null for an app without it. */
    private static String inflaterClass(Context c) {
        String name = null;
        try {
            int attr = c.getResources().getIdentifier("viewInflaterClass", "attr", c.getPackageName());
            if (attr != 0) {
                android.content.res.TypedArray a = c.obtainStyledAttributes(new int[] {attr});
                name = a.getString(0);
                a.recycle();
            }
        } catch (Throwable ignored) { }
        if (name == null || name.isEmpty()) name = "androidx.appcompat.app.AppCompatViewInflater";
        if (load(c, name) != null) return name;
        // R8 drops AppCompatViewInflater when nothing names it: AppCompat itself is there if
        // its widgets are.
        return load(c, APPCOMPAT[0][1]) != null ? "androidx.appcompat.app.AppCompatViewInflater" : null;
    }

    /** How the factory makes a view: the app's view inflater's own createView, or (R8 inlined
     *  or renamed it) its table of widgets. */
    private static String inflaterHow(Context c) {
        String cls = inflaterClass(c);
        if (cls == null) return "platform";
        return cls + (createView(load(c, cls)) != null ? " (createView)" : " (table)");
    }

    private static Class<?> load(Context c, String name) {
        if (c == null || name == null) return null;
        ClassLoader[] loaders = {c.getClassLoader(), LayoutSweep.class.getClassLoader(),
                                 Thread.currentThread().getContextClassLoader()};
        for (ClassLoader l : loaders) {
            if (l == null) continue;
            try { return Class.forName(name, false, l); } catch (Throwable ignored) { }
        }
        return null;
    }

    /** AppCompatViewInflater.createView, found by its parameters -- it is package-private and R8
     *  renames it: (View, String, Context, AttributeSet, boolean x4) -> View. */
    private static Method createView(Class<?> k) {
        for (Class<?> s = k; s != null; s = s.getSuperclass()) {
            for (Method m : s.getDeclaredMethods()) {
                Class<?>[] p = m.getParameterTypes();
                if (View.class.equals(m.getReturnType()) && p.length == 8 && p[0] == View.class
                        && p[1] == String.class && p[2] == Context.class && p[3] == AttributeSet.class
                        && p[4] == boolean.class && p[5] == boolean.class && p[6] == boolean.class
                        && p[7] == boolean.class) {
                    return m;
                }
            }
        }
        return null;
    }

    private static RuntimeException rethrow(Throwable t) {
        if (t instanceof java.lang.reflect.InvocationTargetException && t.getCause() != null) t = t.getCause();
        if (t instanceof RuntimeException) return (RuntimeException) t;
        if (t instanceof Error) throw (Error) t;
        return new RuntimeException(t);
    }

    /** c's own LayoutInflater (a new ContextThemeWrapper's, which no one has used yet) with
     *  AppCompat's view inflater as its factory, as AppCompatDelegate sets on an activity's:
     *  a view that inflates more views through LayoutInflater.from(its context) gets
     *  AppCompat's widgets too (a library casting what it inflated to AppCompatImageView
     *  throws otherwise). */
    private static LayoutInflater inflater(Context c) {
        LayoutInflater li = LayoutInflater.from(c);
        String cls = inflaterClass(c);
        if (cls == null || li.getFactory() != null) return li;
        try {
            Class<?> k = load(c, cls);
            Object made = null;
            Method create = createView(k);
            if (create != null) {
                Constructor<?> ctor = k.getDeclaredConstructor();
                ctor.setAccessible(true);
                made = ctor.newInstance();
                create.setAccessible(true);
            }
            final Object viewInflater = made;
            final Method createView = create;
            final java.util.Map<String, String> table = new java.util.HashMap<>();
            for (String[] row : APPCOMPAT) table.put(row[0], row[1]);
            if (cls.endsWith("MaterialComponentsViewInflater")) {
                for (String[] row : MATERIAL) table.put(row[0], row[1]);
            }
            li.setFactory2(new LayoutInflater.Factory2() {
                @Override public View onCreateView(View parent, String name, Context context, AttributeSet attrs) {
                    if (createView != null) {
                        try {
                            // AppCompatDelegateImpl.createView's arguments on API 21 and later.
                            return (View) createView.invoke(viewInflater, parent, name, context, attrs,
                                                            false, false, true, false);
                        } catch (Throwable t) {
                            throw rethrow(t);
                        }
                    }
                    Class<?> widget = load(context, table.get(name));
                    if (widget == null) return null;
                    try {
                        return (View) widget.getConstructor(Context.class, AttributeSet.class)
                            .newInstance(context, attrs);
                    } catch (Throwable t) {
                        throw rethrow(t);
                    }
                }
                @Override public View onCreateView(String name, Context context, AttributeSet attrs) {
                    return onCreateView(null, name, context, attrs);
                }
            });
        } catch (Throwable ignored) { }
        return li;
    }

    // ---- The lines: Probes.dump's format (windows/test/uiprobe/src/dev/winapk/probe/Probes.java) ----

    private static String hex(int colour) {
        return String.format("%08x", colour);
    }

    private static String dp(float px, float density) {
        // Tenths of a dp: the two sides round a pixel differently now and then.
        return String.valueOf(Math.round(px / density * 10) / 10f);
    }

    /** The mean colour of a rectangle of the picture, each channel as 0..9. */
    private static String colour(Bitmap b, int l, int t, int r, int bottom) {
        l = Math.max(0, l); t = Math.max(0, t);
        r = Math.min(b.getWidth(), r); bottom = Math.min(b.getHeight(), bottom);
        if (r <= l || bottom <= t) return "-";
        int w = r - l, h = bottom - t;
        int[] px = new int[w * h];
        b.getPixels(px, 0, w, l, t, w, h);
        long red = 0, green = 0, blue = 0;
        for (int p : px) {
            red += (p >> 16) & 0xff;
            green += (p >> 8) & 0xff;
            blue += p & 0xff;
        }
        long n = px.length;
        return (red / n * 10 / 256) + "," + (green / n * 10 / 256) + "," + (blue / n * 10 / 256);
    }

    private static void dump(View v, int depth, int ox, int oy, float d, List<String> out, Context c,
                             Bitmap picture) {
        StringBuilder sb = new StringBuilder("  ");
        for (int i = 0; i < depth; i++) sb.append("  ");
        sb.append(v.getClass().getSimpleName());
        if (v.getId() != View.NO_ID && (v.getId() >>> 24) == 0) {
            // View.generateViewId's: no resource, and a number that depends on what ran before.
            sb.append("#?");
        } else if (v.getId() != View.NO_ID) {
            try { sb.append('#').append(c.getResources().getResourceEntryName(v.getId())); }
            catch (Throwable t) { sb.append("#?"); }
        }
        sb.append(" at ").append(dp(v.getLeft(), d)).append(',').append(dp(v.getTop(), d))
          .append(" size ").append(dp(v.getWidth(), d)).append('x').append(dp(v.getHeight(), d));
        if (v.getVisibility() != View.VISIBLE) sb.append(" vis=").append(v.getVisibility());
        sb.append(" dir=").append(v.getLayoutDirection());
        sb.append(" pad=").append(dp(v.getPaddingLeft(), d)).append(',').append(dp(v.getPaddingTop(), d))
          .append(',').append(dp(v.getPaddingRight(), d)).append(',').append(dp(v.getPaddingBottom(), d));
        if (v.getLayoutParams() instanceof ViewGroup.MarginLayoutParams) {
            ViewGroup.MarginLayoutParams m = (ViewGroup.MarginLayoutParams) v.getLayoutParams();
            sb.append(" margins=").append(dp(m.leftMargin, d)).append(',').append(dp(m.topMargin, d))
              .append(',').append(dp(m.rightMargin, d)).append(',').append(dp(m.bottomMargin, d))
              .append(" lpdir=").append(m.getLayoutDirection());
        }
        if (v instanceof TextView) {
            TextView tv = (TextView) v;
            try {
                sb.append(" baseline=").append(dp(tv.getBaseline(), d));
                Layout l = tv.getLayout();
                if (l != null) {
                    sb.append(" lines=").append(l.getLineCount());
                    int last = l.getLineCount() - 1;
                    sb.append(" texth=").append(dp(l.getHeight(), d))
                      .append(" lastw=").append(dp(l.getLineWidth(last), d))
                      .append(" ell=").append(l.getEllipsisCount(last));
                    // Where the first line's ink sits inside the view.
                    sb.append(" text=").append(dp(l.getLineLeft(0) + tv.getTotalPaddingLeft(), d))
                      .append("..").append(dp(l.getLineRight(0) + tv.getTotalPaddingLeft(), d))
                      .append(" top=").append(dp(l.getLineTop(0) + tv.getTotalPaddingTop(), d));
                } else {
                    sb.append(" layout=null");
                }
            } catch (Throwable t) {
                sb.append(" text?").append(t.getClass().getSimpleName());
            }
        }
        if (v instanceof android.widget.ImageView && ((android.widget.ImageView) v).getDrawable() != null) {
            android.widget.ImageView iv = (android.widget.ImageView) v;
            float[] m = new float[9];
            iv.getImageMatrix().getValues(m);
            android.graphics.Rect b = iv.getDrawable().getBounds();
            sb.append(" scale=").append(Math.round(m[0] * 100) / 100f).append('x').append(Math.round(m[4] * 100) / 100f)
              .append(" shift=").append(dp(m[2], d)).append(',').append(dp(m[5], d))
              .append(" pic=").append(dp(b.width(), d)).append('x').append(dp(b.height(), d));
        }
        if (v instanceof android.widget.ImageView) {
            android.content.res.ColorStateList tint = ((android.widget.ImageView) v).getImageTintList();
            sb.append(" tint=").append(tint == null ? "none" : hex(tint.getColorForState(v.getDrawableState(), 0)));
        }
        if (v instanceof android.widget.Checkable) {
            sb.append(" checked=").append(((android.widget.Checkable) v).isChecked());
        }
        // Only whether the text is masked: the method's class is AppCompat's emoji wrapper
        // wherever EmojiCompat is set up, which it is in a started app and not under test.
        if (v instanceof TextView && ((TextView) v).getTransformationMethod()
                instanceof android.text.method.PasswordTransformationMethod) {
            sb.append(" password");
        }
        if (v instanceof TextView) sb.append(" gravity=").append(Integer.toHexString(((TextView) v).getGravity()));
        if (v.getBackgroundTintList() != null) {
            sb.append(" bgtint=").append(hex(v.getBackgroundTintList().getColorForState(v.getDrawableState(), 0)));
        }
        int x = ox + v.getLeft(), y = oy + v.getTop();
        if (picture != null && !(v instanceof ViewGroup) && v.getWidth() > 0 && v.getHeight() > 0) {
            // The whole view, then its middle third each way.
            int tw = v.getWidth() / 3, th = v.getHeight() / 3;
            sb.append(" px=").append(colour(picture, x, y, x + v.getWidth(), y + v.getHeight()))
              .append(';').append(colour(picture, x + tw, y + th, x + 2 * tw, y + 2 * th));
        }
        out.add(sb.toString());
        if (v instanceof ViewGroup) {
            ViewGroup g = (ViewGroup) v;
            for (int i = 0; i < g.getChildCount(); i++) dump(g.getChildAt(i), depth + 1, x, y, d, out, c, picture);
        }
    }
}
