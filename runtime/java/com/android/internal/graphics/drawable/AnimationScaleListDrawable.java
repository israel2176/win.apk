// com.android.internal.graphics.drawable.AnimationScaleListDrawable -- the
// @hide DrawableContainer the platform's progress-bar styles use as their
// indeterminate drawable (core/res/res/drawable/progress_*_material.xml roots
// are this class by name). AOSP:
//   https://cs.android.com/android/platform/superproject/main/+/main:frameworks/base/core/java/com/android/internal/graphics/drawable/AnimationScaleListDrawable.java
//
// It holds two children, an Animatable one (the animated-vector spinner) and a
// static one, and shows the static child only when the animator duration scale
// is 0 ("Remove animations"), the animatable one otherwise. start/stop/isRunning
// forward to the shown child.
//
// Without this class every such XML fell to the class-name fallback in
// Resources.macapkInflateDrawableElement, which found no class and returned a
// transparent drawable: apps that draw the style's indeterminate drawable
// themselves (Google Play's PhoneskyProgressBar) showed no spinner at all.
// Built like LevelListDrawable: our DrawableContainer has no per-subclass
// ConstantState, so the two indices live on the drawable itself.
package com.android.internal.graphics.drawable;

import android.animation.ValueAnimator;
import android.graphics.drawable.Animatable;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.DrawableContainer;

public class AnimationScaleListDrawable extends DrawableContainer implements Animatable {
    private int mStaticDrawableIndex = -1;
    private int mAnimatableDrawableIndex = -1;

    public AnimationScaleListDrawable() {}

    /** AOSP addDrawable: an Animatable child becomes the animated choice, any
     *  other the static one (the last of each kind wins, as in AOSP). Resources
     *  calls this once per {@code <item>}. */
    public void addDrawable(Drawable drawable) {
        final int pos = addChild(drawable);
        if (drawable instanceof Animatable) {
            mAnimatableDrawableIndex = pos;
        } else {
            mStaticDrawableIndex = pos;
        }
        selectDrawable(currentIndexForScale());
    }

    /** AOSP AnimationScaleListState.getCurrentDrawableIndexBasedOnScale. One
     *  departure: AOSP shows nothing when the wanted kind is missing. Here a
     *  platform animated-vector can still come back as a non-Animatable
     *  drawable, leaving only "static" children, so the other kind is used
     *  rather than painting nothing. */
    private int currentIndexForScale() {
        final boolean animate = ValueAnimator.getDurationScale() != 0;
        int idx = animate ? mAnimatableDrawableIndex : mStaticDrawableIndex;
        if (idx < 0) idx = animate ? mStaticDrawableIndex : mAnimatableDrawableIndex;
        return idx;
    }

    /** AOSP re-picks the child on every state change, which is where a change of
     *  the duration scale is noticed. */
    @Override public boolean onStateChange(int[] stateSet) {
        final boolean changed = super.onStateChange(stateSet);
        return selectDrawable(currentIndexForScale()) || changed;
    }

    @Override public void start() {
        final Drawable dr = getCurrent();
        if (dr instanceof Animatable) ((Animatable) dr).start();
    }

    @Override public void stop() {
        final Drawable dr = getCurrent();
        if (dr instanceof Animatable) ((Animatable) dr).stop();
    }

    @Override public boolean isRunning() {
        final Drawable dr = getCurrent();
        return dr instanceof Animatable && ((Animatable) dr).isRunning();
    }
}
