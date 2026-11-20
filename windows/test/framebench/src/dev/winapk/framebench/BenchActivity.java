// The frame bench's one screen: a list of 200 rows (a coloured oval, two lines of text, a number,
// a rounded card with a border) that scrolls by itself on every frame, bouncing at either end, so
// every vsync draws a new frame for as long as the app runs. Plain framework views, no library:
// what is timed is the view tree's drawing and the present (windows/README.md, "Frame time").
package dev.winapk.framebench;

import android.app.Activity;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.os.Bundle;
import android.util.Log;
import android.util.TypedValue;
import android.view.Choreographer;
import android.view.Gravity;
import android.view.View;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

public class BenchActivity extends Activity implements Choreographer.FrameCallback {
    private static final int ROWS = 200;
    private ScrollView scroll;
    private int step;
    private long callbacks;

    private int dp(float v) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, getResources().getDisplayMetrics());
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        LinearLayout list = new LinearLayout(this);
        list.setOrientation(LinearLayout.VERTICAL);
        list.setPadding(dp(8), dp(8), dp(8), dp(8));
        for (int i = 0; i < ROWS; i++) list.addView(row(i));
        scroll = new ScrollView(this);
        scroll.setBackgroundColor(0xFFF2F2F7);
        scroll.addView(list);
        setContentView(scroll);
        step = dp(4);
        Log.i("FrameBench", "FRAMEBENCH start: " + ROWS + " rows, scrolling " + step + " px a frame");
        Choreographer.getInstance().postFrameCallback(this);
    }

    private View row(int i) {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(dp(12), dp(10), dp(12), dp(10));
        GradientDrawable card = new GradientDrawable();
        card.setColor(Color.WHITE);
        card.setCornerRadius(dp(12));
        card.setStroke(dp(1), 0xFFD1D1D6);
        row.setBackground(card);
        LinearLayout.LayoutParams rowLp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        rowLp.bottomMargin = dp(6);
        row.setLayoutParams(rowLp);

        View dot = new View(this);
        GradientDrawable oval = new GradientDrawable();
        oval.setShape(GradientDrawable.OVAL);
        oval.setColor(Color.HSVToColor(new float[] {(i * 37) % 360, 0.6f, 0.9f}));
        dot.setBackground(oval);
        row.addView(dot, new LinearLayout.LayoutParams(dp(40), dp(40)));

        LinearLayout texts = new LinearLayout(this);
        texts.setOrientation(LinearLayout.VERTICAL);
        texts.setPadding(dp(12), 0, dp(8), 0);
        TextView title = new TextView(this);
        title.setText("Row " + (i + 1) + ": the quick brown fox jumps over the lazy dog");
        title.setTextSize(TypedValue.COMPLEX_UNIT_SP, 16);
        title.setTextColor(0xFF1C1C1E);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        title.setSingleLine(true);
        texts.addView(title);
        TextView sub = new TextView(this);
        sub.setText("Second line of text, smaller and grey, " + (i * 7 % 60) + " minutes ago");
        sub.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
        sub.setTextColor(0xFF8E8E93);
        sub.setSingleLine(true);
        texts.addView(sub);
        row.addView(texts, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));

        TextView count = new TextView(this);
        count.setText(String.valueOf((i * 13) % 100));
        count.setTextSize(TypedValue.COMPLEX_UNIT_SP, 18);
        count.setTextColor(0xFF007AFF);
        row.addView(count);
        return row;
    }

    @Override
    public void doFrame(long frameTimeNanos) {
        int max = scroll.getChildAt(0).getHeight() - scroll.getHeight();
        int y = scroll.getScrollY();
        if (max > 0 && ((step > 0 && y >= max) || (step < 0 && y <= 0))) step = -step;
        scroll.scrollBy(0, step);
        if (++callbacks % 600 == 0) Log.i("FrameBench", "FRAMEBENCH " + callbacks + " frames, at y=" + y);
        Choreographer.getInstance().postFrameCallback(this);
    }
}
