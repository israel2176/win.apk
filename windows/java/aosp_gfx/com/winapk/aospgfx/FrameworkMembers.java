package com.winapk.aospgfx;

import android.content.res.ColorStateList;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.RippleDrawable;

import com.android.internal.graphics.drawable.AnimationScaleListDrawable;

import java.lang.reflect.Field;
import java.lang.reflect.Method;

/**
 * Members Win.apk's own framework (android-framework.jar) calls on a class that lib/aosp-graphics.jar
 * replaces with AOSP's, which AOSP's does not have under that name. Added to the class as
 * Api35's are (scripts/AospAddMethods.java): each public static method becomes an instance method of
 * its first parameter's class.
 */
public final class FrameworkMembers {
    private FrameworkMembers() {}

    private static Field state;
    private static Method add;
    private static Field rippleState, rippleColor;

    /**
     * Resources' own inflater (where AOSP's inflate of an {@code <animation-scale-list>} failed) adds
     * each item with addDrawable; AOSP's AnimationScaleListDrawable does it in inflateChildElements,
     * through its state's addDrawable, which sorts the item into the animatable or the static slot.
     */
    public static void addDrawable(AnimationScaleListDrawable d, Drawable item) {
        try {
            if (add == null) {
                Field f = AnimationScaleListDrawable.class.getDeclaredField("mAnimationScaleListState");
                f.setAccessible(true);
                Method m = f.getType().getDeclaredMethod("addDrawable", Drawable.class);
                m.setAccessible(true);
                state = f;
                add = m;
            }
            add.invoke(state.get(d), item);
        } catch (ReflectiveOperationException e) {
            throw new IllegalStateException("AnimationScaleListDrawable.addDrawable", e);
        }
    }

    /**
     * View's ripple (Win.apk's own, over the background) takes its colour from the background's
     * RippleDrawable: the colour AOSP's keeps in its state (mState.mColor, the android:color it was
     * inflated or made with), which AOSP's class has no getter for. Null when there is none.
     */
    public static ColorStateList getColor(RippleDrawable d) {
        try {
            if (rippleColor == null) {
                Field f = RippleDrawable.class.getDeclaredField("mState");
                f.setAccessible(true);
                Field c = f.getType().getDeclaredField("mColor");
                c.setAccessible(true);
                rippleState = f;
                rippleColor = c;
            }
            Object s = rippleState.get(d);
            return s == null ? null : (ColorStateList) rippleColor.get(s);
        } catch (ReflectiveOperationException | ClassCastException e) {
            return null;
        }
    }
}
