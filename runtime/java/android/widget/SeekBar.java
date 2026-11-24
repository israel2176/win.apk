// android.widget.SeekBar -- a horizontal slider. Previously a no-touch stub
// (the OnSeekBarChangeListener resolved but never fired). Now real: a
// horizontal drag/tap maps the x position to a progress value across
// [min, max], updates the ProgressBar progress, and fires the listener with
// fromUser=true plus start/stop-tracking callbacks. AOSP:
//   frameworks/base/core/java/android/widget/SeekBar.java
//   public class SeekBar extends AbsSeekBar
package android.widget;

import android.content.Context;
import android.util.AttributeSet;
import android.view.MotionEvent;

public class SeekBar extends AbsSeekBar {
    public interface OnSeekBarChangeListener {
        void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser);
        void onStartTrackingTouch(SeekBar seekBar);
        void onStopTrackingTouch(SeekBar seekBar);
    }

    private OnSeekBarChangeListener listener;
    private boolean mIsDragging;

    public SeekBar(Context context) { super(context); }
    public SeekBar(Context context, AttributeSet attrs) { super(context, attrs); }
    public SeekBar(Context context, AttributeSet attrs, int defStyleAttr) {
        super(context, attrs, defStyleAttr);
    }
    public SeekBar(Context context, AttributeSet attrs, int defStyleAttr, int defStyleRes) {
        super(context, attrs, defStyleAttr, defStyleRes);
    }

    /** AOSP android14-release SeekBar.java:82: SeekBar(Context, AttributeSet) is
     *  this(context, attrs, com.android.internal.R.attr.seekBarStyle). */
    @Override int macapkDefaultStyleAttr() { return 0x0101007b; /* android.R.attr.seekBarStyle */ }

    public void setOnSeekBarChangeListener(OnSeekBarChangeListener l) { this.listener = l; }
    public OnSeekBarChangeListener getOnSeekBarChangeListener() { return listener; }

    @Override public boolean onTouchEvent(MotionEvent ev) {
        switch (ev.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
                mIsDragging = true;
                if (listener != null) listener.onStartTrackingTouch(this);
                trackTouchEvent(ev);
                return true;
            case MotionEvent.ACTION_MOVE:
                if (mIsDragging) trackTouchEvent(ev);
                return true;
            case MotionEvent.ACTION_UP:
                if (mIsDragging) {
                    trackTouchEvent(ev);
                    mIsDragging = false;
                    if (listener != null) listener.onStopTrackingTouch(this);
                }
                return true;
            case MotionEvent.ACTION_CANCEL:
                if (mIsDragging) {
                    mIsDragging = false;
                    if (listener != null) listener.onStopTrackingTouch(this);
                }
                return true;
        }
        return super.onTouchEvent(ev);
    }

    private void trackTouchEvent(MotionEvent ev) {
        final int width = getWidth();
        final int available = width - getPaddingLeft() - getPaddingRight();
        final int x = Math.round(ev.getX());
        float scale;
        if (available <= 0) {
            scale = 0f;
        } else if (x < getPaddingLeft()) {
            scale = 0.0f;
        } else if (x > width - getPaddingRight()) {
            scale = 1.0f;
        } else {
            scale = (float) (x - getPaddingLeft()) / (float) available;
        }
        final int range = getMax() - getMin();
        final int progress = getMin() + Math.round(scale * range);
        setProgressInternal(progress, true);
    }

    private void setProgressInternal(int progress, boolean fromUser) {
        setProgress(progress);
        if (listener != null) {
            listener.onProgressChanged(this, getProgress(), fromUser);
        }
    }

    @Override public synchronized void setProgress(int progress) {
        super.setProgress(progress);
        // Programmatic changes also notify (AOSP fires fromUser=false), but
        // only when not in the middle of a user drag to avoid double-firing.
        if (!mIsDragging && listener != null) {
            listener.onProgressChanged(this, getProgress(), false);
        }
    }

    // ---- Accessibility: AOSP AbsSeekBar (android-14), for a screen reader ----

    @Override
    public void onInitializeAccessibilityNodeInfoInternal(android.view.accessibility.AccessibilityNodeInfo info) {
        super.onInitializeAccessibilityNodeInfoInternal(info);
        if (isEnabled()) {
            final int progress = getProgress();
            if (progress > getMin()) info.addAction(android.view.accessibility.AccessibilityNodeInfo.AccessibilityAction.ACTION_SCROLL_BACKWARD);
            if (progress < getMax()) info.addAction(android.view.accessibility.AccessibilityNodeInfo.AccessibilityAction.ACTION_SCROLL_FORWARD);
            info.addAction(android.view.accessibility.AccessibilityNodeInfo.AccessibilityAction.ACTION_SET_PROGRESS);
        }
    }

    /** AOSP AbsSeekBar.performAccessibilityActionInternal: a step of a twentieth of the range
     *  (its default key increment), or the value given; the listener hears it as the user's. */
    @Override
    public boolean performAccessibilityActionInternal(int action, android.os.Bundle arguments) {
        if (super.performAccessibilityActionInternal(action, arguments)) return true;
        if (!isEnabled()) return false;
        if (action == android.view.accessibility.AccessibilityNodeInfo.ACTION_SCROLL_FORWARD || action == android.view.accessibility.AccessibilityNodeInfo.ACTION_SCROLL_BACKWARD) {
            int increment = Math.max(1, Math.round((getMax() - getMin()) / 20f));
            if (action == android.view.accessibility.AccessibilityNodeInfo.ACTION_SCROLL_BACKWARD) increment = -increment;
            int target = Math.max(getMin(), Math.min(getMax(), getProgress() + increment));
            if (target == getProgress()) return false;
            setProgressInternal(target, true);
            return true;
        }
        if (action == android.view.accessibility.AccessibilityNodeInfo.AccessibilityAction.ACTION_SET_PROGRESS.getId()) {
            final String key = "android.view.accessibility.action.ARGUMENT_PROGRESS_VALUE";
            if (arguments == null || !arguments.containsKey(key)) return false;
            int target = Math.max(getMin(), Math.min(getMax(), (int) arguments.getFloat(key)));
            if (target == getProgress()) return false;
            setProgressInternal(target, true);
            return true;
        }
        return false;
    }
}
