// The probes: layouts inflated, measured and laid out with nothing attached,
// and every view's place written as text. The same class runs inside Win.apk
// (ProbeActivity, in the probe APK) and on the real AOSP framework under
// Robolectric (oracle/ProbeOracleTest); compare.py lines the two up. A line
// that differs is a place where Win.apk's framework is not Android's.
package dev.winapk.probe;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.text.Layout;
import android.view.ContextThemeWrapper;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.util.ArrayList;
import java.util.List;

public final class Probes {
    private Probes() {}

    /** The screen every case is laid out on, in dp. */
    static final int W_DP = 360, H_DP = 640;

    private static final int[] LAYOUTS = {
        R.layout.text_gravity, R.layout.linear_weights, R.layout.frame_gravity, R.layout.relative,
        R.layout.edit_compound, R.layout.custom_params, R.layout.icons,
    };
    private static final String[] NAMES = {
        "text_gravity", "linear_weights", "frame_gravity", "relative", "edit_compound", "custom_params", "icons",
    };

    /** runAll for a caller that must not name an Android class: the oracle's test class is
     *  loaded outside Robolectric's sandbox first, where there is no android.content.Context. */
    public static List<String> runAllOn(Object app, File pictures) {
        return runAll((Context) app, pictures);
    }

    /** Every case, left to right and right to left, as lines of text. */
    public static List<String> runAll(Context app, File pictures) {
        List<String> out = new ArrayList<>();
        float density = app.getResources().getDisplayMetrics().density;
        out.add("density " + density);
        int w = Math.round(W_DP * density), h = Math.round(H_DP * density);
        for (int i = 0; i < LAYOUTS.length; i++) {
            for (int dir = 0; dir < 2; dir++) {
                String name = NAMES[i] + (dir == 0 ? ".ltr" : ".rtl");
                out.add("case " + name);
                try {
                    Context c = new ContextThemeWrapper(app, R.style.ProbeTheme);
                    FrameLayout root = new FrameLayout(c);
                    root.setLayoutDirection(dir == 0 ? View.LAYOUT_DIRECTION_LTR : View.LAYOUT_DIRECTION_RTL);
                    LayoutInflater.from(c).inflate(LAYOUTS[i], root, true);
                    root.measure(View.MeasureSpec.makeMeasureSpec(w, View.MeasureSpec.EXACTLY),
                                 View.MeasureSpec.makeMeasureSpec(h, View.MeasureSpec.EXACTLY));
                    root.layout(0, 0, w, h);
                    // What was drawn: the case on a white bitmap, each view's patch of it as a
                    // few average colours beside its place (a wrong tint, a missing icon, a
                    // black box show there), and the whole picture as a PNG for looking at.
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
                        picture = null;
                    }
                    dump(root.getChildAt(0), 0, 0, 0, density, out, c, picture);
                } catch (Throwable t) {
                    out.add("  FAILED " + t);
                }
            }
        }
        return out;
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
        if (v.getId() != View.NO_ID) {
            try { sb.append('#').append(c.getResources().getResourceEntryName(v.getId())); }
            catch (Throwable t) { sb.append("#?"); }
        }
        sb.append(" at ").append(dp(v.getLeft(), d)).append(',').append(dp(v.getTop(), d))
          .append(" size ").append(dp(v.getWidth(), d)).append('x').append(dp(v.getHeight(), d));
        if (v.getVisibility() != View.VISIBLE) sb.append(" vis=").append(v.getVisibility());
        sb.append(" dir=").append(v.getLayoutDirection());
        sb.append(" pad=").append(dp(v.getPaddingLeft(), d)).append(',').append(dp(v.getPaddingTop(), d))
          .append(',').append(dp(v.getPaddingRight(), d)).append(',').append(dp(v.getPaddingBottom(), d));
        if (v instanceof TextView) {
            TextView tv = (TextView) v;
            try {
                sb.append(" baseline=").append(dp(tv.getBaseline(), d));
                Layout l = tv.getLayout();
                if (l != null) {
                    sb.append(" lines=").append(l.getLineCount());
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
