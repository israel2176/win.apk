package android.widget;

import android.text.BoringLayout;
import android.text.DynamicLayout;
import android.text.Layout;
import android.text.StaticLayout;
import android.text.TextDirectionHeuristic;
import android.text.TextPaint;
import android.text.TextUtils;
import android.view.Gravity;
import android.view.View.MeasureSpec;
import android.view.ViewGroup.LayoutParams;

/**
 * TextView's text layout on AOSP's own android.text classes, used only with the AOSP graphics
 * switch on (WINAPK_AOSP_GFX=1, GfxBackend.isAosp()). There the Layout, StaticLayout,
 * BoringLayout and DynamicLayout on the class path are android-all 14's, and this class builds
 * and uses them the way AOSP 14's TextView does: makeNewLayout / makeSingleLayout, onMeasure
 * with getDesiredHeight and desired(), getExtendedPaddingTop/Bottom, getVerticalOffset,
 * getBaseline, onDraw's clip and translate around Layout.draw, getLineBounds, checkForRelayout
 * and bringTextIntoView / bringPointIntoView's horizontal half. The methods are ported from
 * frameworks/base android14-release core/java/android/widget/TextView.java; the line numbers
 * in the comments are that file's.
 *
 * Win.apk's TextView keeps its own line arithmetic for the Java2D path (the switch off), where
 * its Layout classes are Win.apk's; this class is its one branch point (TextView.mAospLayout,
 * null with the switch off). When the views layer moves to AOSP's own TextView it goes away.
 *
 * What differs from AOSP, and why:
 * - The layouts are not dropped by every setter (AOSP's nullLayouts in setTextSize,
 *   setTypeface, setLineSpacing...): Win.apk's setters do not call one. Instead every entry
 *   point compares what the layout was built from (the text, the paint's state, the layout
 *   parameters) with the view's state now and builds anew at the old widths when it moved,
 *   which is what makeNewLayout does in checkForRelayout; the setter's requestLayout then
 *   re-measures as on a device.
 * - The view is never scrolled: AOSP scrolls a horizontally scrolling TextView (its layout is
 *   VERY_WIDE) from onPreDraw with bringTextIntoView or, with a movement method,
 *   bringPointIntoView. Win.apk's TextView has no pre-draw hook, and scrolling the View would
 *   move what Win.apk's View draws with the scroll. The same scroll is worked out here and kept
 *   for the text alone (textScrollX), applied where AOSP's mScrollX enters: onDraw's
 *   translate, the caret and convertToLocalHorizontalCoordinate. Vertical scrolling (an editor
 *   taller than its box) is not done.
 * - No marquee, no autosize, no optical insets (Win.apk's TextView has none of them).
 */
final class TextViewAospLayout {
    /** TextView.VERY_WIDE (:839). */
    static final int VERY_WIDE = 1024 * 1024;
    /** TextView.UNKNOWN_BORING (:10849): "not measured yet", told apart by identity. */
    private static final BoringLayout.Metrics UNKNOWN_BORING = new BoringLayout.Metrics();

    private final TextView mTv;
    private Layout mLayout;
    private Layout mHintLayout;
    private BoringLayout.Metrics mBoring, mHintBoring;
    /** mOldMaximum / mOldMaxMode (:10541-10543). */
    private int mOldMaximum = Integer.MAX_VALUE;
    private boolean mOldMaxModeLines = true;
    private int mDesiredHeightAtMeasure = -1;
    /** The ellipsis width the last makeNewLayout was given, to rebuild at it. */
    private int mEllipsisWidth;
    /** What mLayout and mHintLayout were built from (inputsKey); null when there are none. */
    private Inputs mBuiltFrom;
    /** The horizontal scroll AOSP's onPreDraw would give the view (see the class comment). */
    private int mTextScrollX;

    TextViewAospLayout(TextView tv) {
        mTv = tv;
    }

    // ---- what a layout is built from ----------------------------------------------------

    /** Everything makeSingleLayout reads, to tell when the layouts must be made again. */
    private static final class Inputs {
        CharSequence text, hint;
        String textContent, hintContent, spans;
        boolean dynamic;
        float size, skew, scaleX, letterSpacing, wordSpacing;
        Object typeface, locales;
        String features, variations;
        int flags;
        boolean fakeBold, elegant;
        Layout.Alignment align;
        TextDirectionHeuristic dir;
        float spacingMult, spacingAdd;
        boolean includePad, horiz, keyListener;
        int fallback, breakStrategy, hyphenation, justification, lineBreakStyle, lineBreakWordStyle;
        boolean maxModeLines;
        int maximum;
        TextUtils.TruncateAt ellipsize, effectiveEllipsize;

        boolean same(Inputs o) {
            return o != null && text == o.text && hint == o.hint && dynamic == o.dynamic
                && java.util.Objects.equals(textContent, o.textContent)
                && java.util.Objects.equals(hintContent, o.hintContent)
                && java.util.Objects.equals(spans, o.spans)
                && size == o.size && skew == o.skew && scaleX == o.scaleX
                && letterSpacing == o.letterSpacing && wordSpacing == o.wordSpacing
                && typeface == o.typeface && java.util.Objects.equals(locales, o.locales)
                && java.util.Objects.equals(features, o.features)
                && java.util.Objects.equals(variations, o.variations)
                && flags == o.flags && fakeBold == o.fakeBold && elegant == o.elegant
                && align == o.align && dir == o.dir
                && spacingMult == o.spacingMult && spacingAdd == o.spacingAdd
                && includePad == o.includePad && horiz == o.horiz && keyListener == o.keyListener
                && fallback == o.fallback && breakStrategy == o.breakStrategy
                && hyphenation == o.hyphenation && justification == o.justification
                && lineBreakStyle == o.lineBreakStyle && lineBreakWordStyle == o.lineBreakWordStyle
                && maxModeLines == o.maxModeLines && maximum == o.maximum
                && ellipsize == o.ellipsize && effectiveEllipsize == o.effectiveEllipsize;
        }
    }

    /** The content of a text a layout cannot follow by itself: null for a String (immutable,
     *  identity says it all) and for the text of a DynamicLayout (it reflows on its own). */
    private static String contentOf(CharSequence t, boolean dynamic) {
        if (t == null || dynamic || t instanceof String) return null;
        return t.toString();
    }

    /** The metric-affecting spans of a text a StaticLayout or BoringLayout measured once. */
    private static String spansOf(CharSequence t) {
        if (!(t instanceof android.text.Spanned) || t.length() == 0) return null;
        final android.text.Spanned sp = (android.text.Spanned) t;
        final Object[] m = sp.getSpans(0, t.length(), android.text.style.MetricAffectingSpan.class);
        if (m == null || m.length == 0) return null;
        final StringBuilder sb = new StringBuilder(m.length * 16);
        for (Object s : m) {
            sb.append(System.identityHashCode(s)).append(':').append(sp.getSpanStart(s)).append('-')
              .append(sp.getSpanEnd(s)).append(';');
        }
        return sb.toString();
    }

    private Inputs inputsNow() {
        final TextView tv = mTv;
        final TextPaint p = tv.macapkAospPaint();
        final Inputs in = new Inputs();
        in.dynamic = useDynamicLayout();
        in.text = tv.macapkAospTransformed();
        in.hint = tv.getHint();
        in.textContent = contentOf(in.text, in.dynamic);
        in.hintContent = contentOf(in.hint, false);
        in.spans = in.dynamic ? null : spansOf(in.text);
        in.size = p.getTextSize();
        in.skew = p.getTextSkewX();
        in.scaleX = p.getTextScaleX();
        in.letterSpacing = p.getLetterSpacing();
        in.wordSpacing = p.getWordSpacing();
        in.typeface = p.getTypeface();
        in.locales = p.getTextLocales();
        in.features = p.getFontFeatureSettings();
        in.variations = p.getFontVariationSettings();
        in.flags = p.getFlags();
        in.fakeBold = p.isFakeBoldText();
        in.elegant = p.isElegantTextHeight();
        in.align = getLayoutAlignment();
        in.dir = tv.getTextDirectionHeuristic();
        in.spacingMult = tv.getLineSpacingMultiplier();
        in.spacingAdd = tv.getLineSpacingExtra();
        in.includePad = tv.getIncludeFontPadding();
        in.horiz = tv.mHorizontallyScrolling;
        in.keyListener = tv.getKeyListener() != null;
        in.fallback = tv.macapkFallbackLineSpacingMode();
        in.breakStrategy = tv.getBreakStrategy();
        in.hyphenation = tv.getHyphenationFrequency();
        in.justification = tv.getJustificationMode();
        in.lineBreakStyle = tv.getLineBreakStyle();
        in.lineBreakWordStyle = tv.getLineBreakWordStyle();
        in.maxModeLines = tv.mMaxModeLines;
        in.maximum = tv.mMaximum;
        in.ellipsize = tv.getEllipsize();
        in.effectiveEllipsize = tv.macapkEffectiveEllipsize();
        return in;
    }

    /** The stand-in for AOSP's nullLayouts in every setter (see the class comment): when what
     *  the layouts were built from moved, they are made again at the widths they had. */
    private void revalidate() {
        if (mLayout == null) return;
        final Inputs now = inputsNow();
        if (now.same(mBuiltFrom)) return;
        mBoring = mHintBoring = null;
        makeNewLayout(mLayout.getWidth(), mHintLayout == null ? 0 : mHintLayout.getWidth(),
                UNKNOWN_BORING, UNKNOWN_BORING, mEllipsisWidth, now);
    }

    // ---- TextView's private members, from its state -------------------------------------

    /** useDynamicLayout (:10677): `isTextSelectable() || (mSpannable != null && mPrecomputed
     *  == null)`. AOSP's mText is Spannable when setText made it Editable or, for
     *  BufferType.SPANNABLE or a movement method, Spannable (:7052-7071); otherwise it is a
     *  String or SpannedString. Win.apk's TextView keeps a caller's Spannable as it was given,
     *  so the test is on why AOSP's would be one. */
    private boolean useDynamicLayout() {
        final CharSequence t = mTv.getText();
        if (t instanceof android.text.Editable) return true;
        return t instanceof android.text.Spannable
            && (mTv.mBufferType == TextView.BufferType.SPANNABLE || mTv.getMovementMethod() != null);
    }

    private boolean isFallbackLineSpacingForBoringLayout() {
        return mTv.macapkFallbackLineSpacingMode() == 2;
    }

    private boolean isFallbackLineSpacingForStaticLayout() {
        return mTv.macapkFallbackLineSpacingMode() != 0;
    }

    private int gravity() {
        return mTv.getGravity();
    }

    /** isShowingHint (:7476): `TextUtils.isEmpty(mText) && !TextUtils.isEmpty(mHint)`. */
    private boolean isShowingHint() {
        return TextUtils.isEmpty(mTv.getText()) && !TextUtils.isEmpty(mTv.getHint());
    }

    private int maxLinesForBuilder() {
        return mTv.mMaxModeLines ? mTv.mMaximum : Integer.MAX_VALUE;
    }

    // ---- making the layouts ----------------------------------------------------------------

    /** getLayoutAlignment (:10474-10531). */
    private Layout.Alignment getLayoutAlignment() {
        final Layout.Alignment alignment;
        switch (mTv.getTextAlignment()) {
            case android.view.View.TEXT_ALIGNMENT_GRAVITY:
                switch (gravity() & Gravity.RELATIVE_HORIZONTAL_GRAVITY_MASK) {
                    case Gravity.START: alignment = Layout.Alignment.ALIGN_NORMAL; break;
                    case Gravity.END: alignment = Layout.Alignment.ALIGN_OPPOSITE; break;
                    case Gravity.LEFT: alignment = Layout.Alignment.ALIGN_LEFT; break;
                    case Gravity.RIGHT: alignment = Layout.Alignment.ALIGN_RIGHT; break;
                    case Gravity.CENTER_HORIZONTAL: alignment = Layout.Alignment.ALIGN_CENTER; break;
                    default: alignment = Layout.Alignment.ALIGN_NORMAL; break;
                }
                break;
            case android.view.View.TEXT_ALIGNMENT_TEXT_START:
                alignment = Layout.Alignment.ALIGN_NORMAL; break;
            case android.view.View.TEXT_ALIGNMENT_TEXT_END:
                alignment = Layout.Alignment.ALIGN_OPPOSITE; break;
            case android.view.View.TEXT_ALIGNMENT_CENTER:
                alignment = Layout.Alignment.ALIGN_CENTER; break;
            case android.view.View.TEXT_ALIGNMENT_VIEW_START:
                alignment = (mTv.getLayoutDirection() == android.view.View.LAYOUT_DIRECTION_RTL)
                        ? Layout.Alignment.ALIGN_RIGHT : Layout.Alignment.ALIGN_LEFT;
                break;
            case android.view.View.TEXT_ALIGNMENT_VIEW_END:
                alignment = (mTv.getLayoutDirection() == android.view.View.LAYOUT_DIRECTION_RTL)
                        ? Layout.Alignment.ALIGN_LEFT : Layout.Alignment.ALIGN_RIGHT;
                break;
            default:
                alignment = Layout.Alignment.ALIGN_NORMAL; break;
        }
        return alignment;
    }

    /** LineBreakConfig.getLineBreakConfig(style, wordStyle, autoPhraseBreaking) with
     *  autoPhraseBreaking false: FeatureFlagUtils.SETTINGS_AUTO_TEXT_WRAPPING is off. */
    private android.graphics.text.LineBreakConfig lineBreakConfig() {
        return new android.graphics.text.LineBreakConfig.Builder()
            .setLineBreakStyle(mTv.getLineBreakStyle())
            .setLineBreakWordStyle(mTv.getLineBreakWordStyle())
            .build();
    }

    /** assumeLayout (:10456-10470). */
    private void assumeLayout() {
        int width = mTv.getWidth() - mTv.getCompoundPaddingLeft() - mTv.getCompoundPaddingRight();
        if (width < 1) width = 0;
        final int physicalWidth = width;
        if (mTv.mHorizontallyScrolling) width = VERY_WIDE;
        makeNewLayout(width, physicalWidth, UNKNOWN_BORING, UNKNOWN_BORING, physicalWidth, null);
    }

    /** makeNewLayout (:10533-10670), without the marquee and the cursor controllers. */
    private void makeNewLayout(int wantWidth, int hintWidth, BoringLayout.Metrics boring,
            BoringLayout.Metrics hintBoring, int ellipsisWidth, Inputs in) {
        final TextView tv = mTv;
        final TextPaint paint = tv.macapkAospPaint();
        if (in == null) in = inputsNow();
        mOldMaximum = tv.mMaximum;
        mOldMaxModeLines = tv.mMaxModeLines;
        if (wantWidth < 0) wantWidth = 0;
        if (hintWidth < 0) hintWidth = 0;
        final Layout.Alignment alignment = in.align;
        boolean shouldEllipsize = in.ellipsize != null && !in.keyListener;
        final TextUtils.TruncateAt effectiveEllipsize = in.effectiveEllipsize != null
            ? in.effectiveEllipsize : in.ellipsize;
        final TextDirectionHeuristic textDir = in.dir;

        mLayout = makeSingleLayout(wantWidth, boring, ellipsisWidth, alignment, shouldEllipsize,
                effectiveEllipsize, paint, textDir, in);

        shouldEllipsize = in.ellipsize != null;
        mHintLayout = null;
        final CharSequence hint = in.hint;
        if (hint != null) {
            if (shouldEllipsize) hintWidth = wantWidth;
            if (hintBoring == UNKNOWN_BORING) {
                hintBoring = BoringLayout.isBoring(hint, paint, textDir,
                        isFallbackLineSpacingForBoringLayout(), mHintBoring);
                if (hintBoring != null) mHintBoring = hintBoring;
            }
            if (hintBoring != null) {
                if (hintBoring.width <= hintWidth
                        && (!shouldEllipsize || hintBoring.width <= ellipsisWidth)) {
                    mHintLayout = BoringLayout.make(hint, paint, hintWidth, alignment,
                            in.spacingMult, in.spacingAdd, hintBoring, in.includePad);
                } else if (shouldEllipsize && hintBoring.width <= hintWidth) {
                    mHintLayout = BoringLayout.make(hint, paint, hintWidth, alignment,
                            in.spacingMult, in.spacingAdd, hintBoring, in.includePad,
                            in.ellipsize, ellipsisWidth);
                }
            }
            if (mHintLayout == null) {
                final StaticLayout.Builder builder = StaticLayout.Builder.obtain(hint, 0,
                        hint.length(), paint, hintWidth)
                        .setAlignment(alignment)
                        .setTextDirection(textDir)
                        .setLineSpacing(in.spacingAdd, in.spacingMult)
                        .setIncludePad(in.includePad)
                        .setUseLineSpacingFromFallbacks(isFallbackLineSpacingForStaticLayout())
                        .setBreakStrategy(in.breakStrategy)
                        .setHyphenationFrequency(in.hyphenation)
                        .setJustificationMode(in.justification)
                        .setMaxLines(maxLinesForBuilder())
                        .setLineBreakConfig(lineBreakConfig());
                if (shouldEllipsize) {
                    builder.setEllipsize(in.ellipsize).setEllipsizedWidth(ellipsisWidth);
                }
                mHintLayout = builder.build();
            }
        }
        mEllipsisWidth = ellipsisWidth;
        mBuiltFrom = in;
    }

    /** makeSingleLayout (:10684-10768). */
    private Layout makeSingleLayout(int wantWidth, BoringLayout.Metrics boring, int ellipsisWidth,
            Layout.Alignment alignment, boolean shouldEllipsize, TextUtils.TruncateAt effectiveEllipsize,
            TextPaint paint, TextDirectionHeuristic textDir, Inputs in) {
        final CharSequence transformed = in.text;
        Layout result = null;
        if (in.dynamic) {
            final DynamicLayout.Builder builder = DynamicLayout.Builder.obtain(mTv.getText(), paint,
                    wantWidth)
                    .setDisplayText(transformed)
                    .setAlignment(alignment)
                    .setTextDirection(textDir)
                    .setLineSpacing(in.spacingAdd, in.spacingMult)
                    .setIncludePad(in.includePad)
                    .setUseLineSpacingFromFallbacks(isFallbackLineSpacingForStaticLayout())
                    .setBreakStrategy(in.breakStrategy)
                    .setHyphenationFrequency(in.hyphenation)
                    .setJustificationMode(in.justification)
                    .setEllipsize(in.keyListener ? null : effectiveEllipsize)
                    .setEllipsizedWidth(ellipsisWidth);
            result = builder.build();
        } else {
            if (boring == UNKNOWN_BORING) {
                boring = BoringLayout.isBoring(transformed, paint, textDir,
                        isFallbackLineSpacingForBoringLayout(), mBoring);
                if (boring != null) mBoring = boring;
            }
            if (boring != null) {
                if (boring.width <= wantWidth
                        && (effectiveEllipsize == null || boring.width <= ellipsisWidth)) {
                    result = BoringLayout.make(transformed, paint, wantWidth, alignment,
                            in.spacingMult, in.spacingAdd, boring, in.includePad);
                } else if (shouldEllipsize && boring.width <= wantWidth) {
                    result = BoringLayout.make(transformed, paint, wantWidth, alignment,
                            in.spacingMult, in.spacingAdd, boring, in.includePad,
                            effectiveEllipsize, ellipsisWidth);
                }
            }
        }
        if (result == null) {
            final StaticLayout.Builder builder = StaticLayout.Builder.obtain(transformed, 0,
                    transformed.length(), paint, wantWidth)
                    .setAlignment(alignment)
                    .setTextDirection(textDir)
                    .setLineSpacing(in.spacingAdd, in.spacingMult)
                    .setIncludePad(in.includePad)
                    .setUseLineSpacingFromFallbacks(isFallbackLineSpacingForStaticLayout())
                    .setBreakStrategy(in.breakStrategy)
                    .setHyphenationFrequency(in.hyphenation)
                    .setJustificationMode(in.justification)
                    .setMaxLines(maxLinesForBuilder())
                    .setLineBreakConfig(lineBreakConfig());
            if (shouldEllipsize) {
                builder.setEllipsize(effectiveEllipsize).setEllipsizedWidth(ellipsisWidth);
            }
            result = builder.build();
        }
        return result;
    }

    /** desired (:10792-10812). */
    private static int desired(Layout layout) {
        final int n = layout.getLineCount();
        final CharSequence text = layout.getText();
        float max = 0;
        for (int i = 0; i < n - 1; i++) {
            if (text.charAt(layout.getLineEnd(i) - 1) != '\n') return -1;
        }
        for (int i = 0; i < n; i++) max = Math.max(max, layout.getLineMax(i));
        return (int) Math.ceil(max);
    }

    // ---- measuring ----------------------------------------------------------------------------

    /** onMeasure (:10852-11000), returning {width, height} for TextView's setMeasuredDimension. */
    int[] onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
        final TextView tv = mTv;
        revalidate();
        final int widthMode = MeasureSpec.getMode(widthMeasureSpec);
        final int heightMode = MeasureSpec.getMode(heightMeasureSpec);
        final int widthSize = MeasureSpec.getSize(widthMeasureSpec);
        final int heightSize = MeasureSpec.getSize(heightMeasureSpec);
        final Inputs in = inputsNow();
        final TextPaint paint = tv.macapkAospPaint();
        final CharSequence transformed = in.text;
        final CharSequence hint = in.hint;
        final TextDirectionHeuristic textDir = in.dir;

        int width;
        int height;
        BoringLayout.Metrics boring = UNKNOWN_BORING;
        BoringLayout.Metrics hintBoring = UNKNOWN_BORING;
        int des = -1;
        boolean fromexisting = false;
        final float widthLimit = (widthMode == MeasureSpec.AT_MOST) ? (float) widthSize : Float.MAX_VALUE;

        if (widthMode == MeasureSpec.EXACTLY) {
            width = widthSize;
        } else {
            if (mLayout != null && in.ellipsize == null) des = desired(mLayout);
            if (des < 0) {
                boring = BoringLayout.isBoring(transformed, paint, textDir,
                        isFallbackLineSpacingForBoringLayout(), mBoring);
                if (boring != null) mBoring = boring;
            } else {
                fromexisting = true;
            }
            if (boring == null || boring == UNKNOWN_BORING) {
                if (des < 0) {
                    des = (int) Math.ceil(Layout.getDesiredWidthWithLimit(transformed, 0,
                            transformed.length(), paint, textDir, widthLimit));
                }
                width = des;
            } else {
                width = boring.width;
            }
            width = Math.max(width, TextView.macapkDrawableW(tv.mDrawableTop));
            width = Math.max(width, TextView.macapkDrawableW(tv.mDrawableBottom));

            if (hint != null) {
                int hintDes = -1;
                int hintWidth;
                if (mHintLayout != null && in.ellipsize == null) hintDes = desired(mHintLayout);
                if (hintDes < 0) {
                    hintBoring = BoringLayout.isBoring(hint, paint, textDir,
                            isFallbackLineSpacingForBoringLayout(), mHintBoring);
                    if (hintBoring != null) mHintBoring = hintBoring;
                }
                if (hintBoring == null || hintBoring == UNKNOWN_BORING) {
                    if (hintDes < 0) {
                        hintDes = (int) Math.ceil(Layout.getDesiredWidthWithLimit(hint, 0,
                                hint.length(), paint, textDir, widthLimit));
                    }
                    hintWidth = hintDes;
                } else {
                    hintWidth = hintBoring.width;
                }
                if (hintWidth > width) width = hintWidth;
            }

            width += tv.getCompoundPaddingLeft() + tv.getCompoundPaddingRight();
            if (tv.mMaxEms >= 0) {
                width = Math.min(width, tv.mMaxEms * tv.getLineHeight());
            } else {
                width = Math.min(width, tv.mMaxWidthPx);
            }
            if (tv.mMinEms >= 0) {
                width = Math.max(width, tv.mMinEms * tv.getLineHeight());
            } else {
                width = Math.max(width, tv.mMinWidthPx);
            }
            width = Math.max(width, tv.macapkSuggestedMinimumWidth());
            if (widthMode == MeasureSpec.AT_MOST) width = Math.min(widthSize, width);
        }

        int want = width - tv.getCompoundPaddingLeft() - tv.getCompoundPaddingRight();
        final int unpaddedWidth = want;
        if (in.horiz) want = VERY_WIDE;
        final int hintWant = want;
        final int hintWidth = (mHintLayout == null) ? hintWant : mHintLayout.getWidth();
        final int ellipsisWidth = width - tv.getCompoundPaddingLeft() - tv.getCompoundPaddingRight();

        if (mLayout == null) {
            makeNewLayout(want, hintWant, boring, hintBoring, ellipsisWidth, in);
        } else {
            final boolean layoutChanged = (mLayout.getWidth() != want) || (hintWidth != hintWant)
                    || (mLayout.getEllipsizedWidth() != ellipsisWidth);
            final boolean widthChanged = (hint == null) && (in.ellipsize == null)
                    && (want > mLayout.getWidth())
                    && (mLayout instanceof BoringLayout || (fromexisting && des >= 0 && des <= want));
            final boolean maximumChanged = (tv.mMaxModeLines != mOldMaxModeLines)
                    || (tv.mMaximum != mOldMaximum);
            if (layoutChanged || maximumChanged) {
                if (!maximumChanged && widthChanged) {
                    mLayout.increaseWidthTo(want);
                } else {
                    makeNewLayout(want, hintWant, boring, hintBoring, ellipsisWidth, in);
                }
            }
        }

        if (heightMode == MeasureSpec.EXACTLY) {
            height = heightSize;
            mDesiredHeightAtMeasure = -1;
        } else {
            final int desired = getDesiredHeight();
            height = desired;
            mDesiredHeightAtMeasure = desired;
            if (heightMode == MeasureSpec.AT_MOST) height = Math.min(desired, heightSize);
        }

        int unpaddedHeight = height - tv.getCompoundPaddingTop() - tv.getCompoundPaddingBottom();
        if (tv.mMaxModeLines && mLayout.getLineCount() > tv.mMaximum) {
            unpaddedHeight = Math.min(unpaddedHeight, mLayout.getLineTop(tv.mMaximum));
        }
        // AOSP registers for the pre-draw scroll here, or scrolls to (0, 0).
        if (tv.getMovementMethod() != null || mLayout.getWidth() > unpaddedWidth
                || mLayout.getHeight() > unpaddedHeight) {
            mScrollPending = true;
        } else {
            mScrollPending = false;
            mTextScrollX = 0;
        }
        return new int[] { width, height };
    }

    /** getDesiredHeight() (:11147-11151). */
    private int getDesiredHeight() {
        return Math.max(getDesiredHeight(mLayout, true),
                getDesiredHeight(mHintLayout, mTv.getEllipsize() != null));
    }

    /** getDesiredHeight(Layout, boolean) (:11153-11205). */
    private int getDesiredHeight(Layout layout, boolean cap) {
        if (layout == null) return 0;
        final TextView tv = mTv;
        int desired = layout.getHeight(cap);
        final int drawH = Math.max(TextView.macapkDrawableH(tv.mDrawableLeft),
                TextView.macapkDrawableH(tv.mDrawableRight));
        desired = Math.max(desired, drawH);
        int linecount = layout.getLineCount();
        final int padding = tv.getCompoundPaddingTop() + tv.getCompoundPaddingBottom();
        desired += padding;
        if (!tv.mMaxModeLines) {
            desired = Math.min(desired, tv.mMaximum);
        } else if (cap && linecount > tv.mMaximum && (layout instanceof DynamicLayout
                || layout instanceof BoringLayout)) {
            desired = layout.getLineTop(tv.mMaximum);
            desired = Math.max(desired, drawH);
            desired += padding;
            linecount = tv.mMaximum;
        }
        if (tv.mMinModeLines) {
            if (linecount < tv.mMinimum) desired += tv.getLineHeight() * (tv.mMinimum - linecount);
        } else {
            desired = Math.max(desired, tv.mMinimum);
        }
        return Math.max(desired, tv.macapkSuggestedMinimumHeight());
    }

    /** checkForRelayout (:11246-11298), as setText's end (:7178: only with a layout). */
    void checkForRelayout() {
        final TextView tv = mTv;
        if (mLayout == null) {
            // AOSP: `if (mLayout != null) checkForRelayout();`. Win.apk's setText always asked
            // for a layout pass; a view never measured is measured by its first one anyway.
            tv.requestLayout();
            tv.invalidate();
            return;
        }
        final LayoutParams lp = tv.getLayoutParams();
        final boolean wrapW = lp == null || lp.width == LayoutParams.WRAP_CONTENT;
        final boolean fixedLimits = (tv.mMaxEms >= 0) == (tv.mMinEms >= 0)
            && ((tv.mMaxEms >= 0) ? tv.mMaxEms == tv.mMinEms : tv.mMaxWidthPx == tv.mMinWidthPx);
        final CharSequence hint = tv.getHint();
        if ((!wrapW || fixedLimits) && (hint == null || mHintLayout != null)
                && (tv.getWidth() - tv.getCompoundPaddingLeft() - tv.getCompoundPaddingRight() > 0)) {
            final int oldht = mLayout.getHeight();
            final int want = mLayout.getWidth();
            final int hintWant = mHintLayout == null ? 0 : mHintLayout.getWidth();
            mBoring = mHintBoring = null;
            makeNewLayout(want, hintWant, UNKNOWN_BORING, UNKNOWN_BORING,
                    tv.getWidth() - tv.getCompoundPaddingLeft() - tv.getCompoundPaddingRight(), null);
            if (tv.getEllipsize() != TextUtils.TruncateAt.MARQUEE && lp != null) {
                if (lp.height != LayoutParams.WRAP_CONTENT && lp.height != LayoutParams.MATCH_PARENT) {
                    tv.invalidate();
                    return;
                }
                if (mLayout.getHeight() == oldht
                        && (mHintLayout == null || mHintLayout.getHeight() == oldht)) {
                    tv.invalidate();
                    return;
                }
            }
            tv.requestLayout();
            tv.invalidate();
        } else {
            nullLayouts();
            tv.requestLayout();
            tv.invalidate();
        }
    }

    /** nullLayouts (:10432-10448). */
    private void nullLayouts() {
        mLayout = mHintLayout = null;
        mBoring = mHintBoring = null;
        mBuiltFrom = null;
    }

    // ---- what TextView answers from its layout -------------------------------------------------

    /** getLayout (:2588): mLayout, null before the first measure. */
    Layout getLayout() {
        revalidate();
        return mLayout;
    }

    /** getHintLayout (:2597). */
    Layout getHintLayout() {
        revalidate();
        return mHintLayout;
    }

    /** getExtendedPaddingTop (:2923-2952). */
    int getExtendedPaddingTop() {
        final TextView tv = mTv;
        if (!tv.mMaxModeLines) return tv.getCompoundPaddingTop();
        if (mLayout == null) assumeLayout(); else revalidate();
        if (mLayout.getLineCount() <= tv.mMaximum) return tv.getCompoundPaddingTop();
        final int top = tv.getCompoundPaddingTop();
        final int bottom = tv.getCompoundPaddingBottom();
        final int viewht = tv.getHeight() - top - bottom;
        final int layoutht = mLayout.getLineTop(tv.mMaximum);
        if (layoutht >= viewht) return top;
        final int g = gravity() & Gravity.VERTICAL_GRAVITY_MASK;
        if (g == Gravity.TOP) return top;
        if (g == Gravity.BOTTOM) return top + viewht - layoutht;
        return top + (viewht - layoutht) / 2;
    }

    /** getExtendedPaddingBottom (:2960-2989). */
    int getExtendedPaddingBottom() {
        final TextView tv = mTv;
        if (!tv.mMaxModeLines) return tv.getCompoundPaddingBottom();
        if (mLayout == null) assumeLayout(); else revalidate();
        if (mLayout.getLineCount() <= tv.mMaximum) return tv.getCompoundPaddingBottom();
        final int top = tv.getCompoundPaddingTop();
        final int bottom = tv.getCompoundPaddingBottom();
        final int viewht = tv.getHeight() - top - bottom;
        final int layoutht = mLayout.getLineTop(tv.mMaximum);
        if (layoutht >= viewht) return bottom;
        final int g = gravity() & Gravity.VERTICAL_GRAVITY_MASK;
        if (g == Gravity.TOP) return bottom + viewht - layoutht;
        if (g == Gravity.BOTTOM) return bottom;
        return bottom + (viewht - layoutht) / 2;
    }

    /** getBoxHeight (:8259-8266), without the optical insets. */
    private int getBoxHeight(Layout l) {
        final int padding = (l == mHintLayout)
                ? mTv.getCompoundPaddingTop() + mTv.getCompoundPaddingBottom()
                : getExtendedPaddingTop() + getExtendedPaddingBottom();
        return mTv.getMeasuredHeight() - padding;
    }

    /** getVerticalOffset (:8268-8290). */
    int getVerticalOffset(boolean forceNormal) {
        int voffset = 0;
        final int g = gravity() & Gravity.VERTICAL_GRAVITY_MASK;
        if (mLayout == null) assumeLayout(); else revalidate();
        Layout l = mLayout;
        if (!forceNormal && mTv.getText().length() == 0 && mHintLayout != null) l = mHintLayout;
        if (g != Gravity.TOP) {
            final int boxht = getBoxHeight(l);
            final int textht = l.getHeight();
            if (textht < boxht) {
                if (g == Gravity.BOTTOM) voffset = boxht - textht;
                else voffset = (boxht - textht) >> 1;
            }
        }
        return voffset;
    }

    /** getBottomVerticalOffset (:8292-8314). */
    int getBottomVerticalOffset(boolean forceNormal) {
        int voffset = 0;
        final int g = gravity() & Gravity.VERTICAL_GRAVITY_MASK;
        if (mLayout == null) assumeLayout(); else revalidate();
        Layout l = mLayout;
        if (!forceNormal && mTv.getText().length() == 0 && mHintLayout != null) l = mHintLayout;
        if (g != Gravity.BOTTOM) {
            final int boxht = getBoxHeight(l);
            final int textht = l.getHeight();
            if (textht < boxht) {
                if (g == Gravity.TOP) voffset = boxht - textht;
                else voffset = (boxht - textht) >> 1;
            }
        }
        return voffset;
    }

    /** getBaseline (:9178-9184) with getBaselineOffset (:9186-9197); -1 without a layout. */
    int getBaseline() {
        revalidate();
        if (mLayout == null) return -1;
        int voffset = 0;
        if ((gravity() & Gravity.VERTICAL_GRAVITY_MASK) != Gravity.TOP) voffset = getVerticalOffset(true);
        return getExtendedPaddingTop() + voffset + mLayout.getLineBaseline(0);
    }

    /** getLineBounds (:9157-9176). */
    int getLineBounds(int line, android.graphics.Rect bounds) {
        revalidate();
        if (mLayout == null) {
            if (bounds != null) bounds.set(0, 0, 0, 0);
            return 0;
        }
        final int baseline = mLayout.getLineBounds(line, bounds);
        int voffset = getExtendedPaddingTop();
        if ((gravity() & Gravity.VERTICAL_GRAVITY_MASK) != Gravity.TOP) voffset += getVerticalOffset(true);
        if (bounds != null) bounds.offset(mTv.getCompoundPaddingLeft(), voffset);
        return baseline + voffset;
    }

    /** The horizontal scroll the text is drawn and hit-tested at (see the class comment). */
    int textScrollX() {
        return mTextScrollX;
    }

    // ---- drawing --------------------------------------------------------------------------------

    /** Whether onMeasure asked for AOSP's pre-draw scroll (registerForPreDraw). */
    private boolean mScrollPending;

    /** onPreDraw's scroll (:8447-8480): bringPointIntoView(selection end) with a movement
     *  method, bringTextIntoView otherwise; horizontal only, into textScrollX. */
    private void preDrawScroll() {
        if (!mScrollPending) return;
        mScrollPending = false;
        final Layout layout = isShowingHint() ? mHintLayout : mLayout;
        if (layout == null) return;
        if (mTv.getMovementMethod() != null) {
            int curs = mTv.getSelectionEnd();
            if (curs < 0 && (gravity() & Gravity.VERTICAL_GRAVITY_MASK) == Gravity.BOTTOM) {
                curs = mTv.getText().length();
            }
            if (curs >= 0) bringPointIntoView(layout, Math.min(curs, layout.getText().length()));
        } else {
            bringTextIntoView(layout);
        }
    }

    /** bringTextIntoView (:11321-11390), horizontal half. */
    private void bringTextIntoView(Layout layout) {
        int line = 0;
        if ((gravity() & Gravity.VERTICAL_GRAVITY_MASK) == Gravity.BOTTOM) line = layout.getLineCount() - 1;
        Layout.Alignment a = layout.getParagraphAlignment(line);
        final int dir = layout.getParagraphDirection(line);
        final int hspace = mTv.getWidth() - mTv.getCompoundPaddingLeft() - mTv.getCompoundPaddingRight();
        int scrollx;
        if (a == Layout.Alignment.ALIGN_NORMAL) {
            a = dir == Layout.DIR_LEFT_TO_RIGHT ? Layout.Alignment.ALIGN_LEFT : Layout.Alignment.ALIGN_RIGHT;
        } else if (a == Layout.Alignment.ALIGN_OPPOSITE) {
            a = dir == Layout.DIR_LEFT_TO_RIGHT ? Layout.Alignment.ALIGN_RIGHT : Layout.Alignment.ALIGN_LEFT;
        }
        if (a == Layout.Alignment.ALIGN_CENTER) {
            final int left = (int) Math.floor(layout.getLineLeft(line));
            final int right = (int) Math.ceil(layout.getLineRight(line));
            if (right - left < hspace) {
                scrollx = (right + left) / 2 - hspace / 2;
            } else {
                scrollx = dir < 0 ? right - hspace : left;
            }
        } else if (a == Layout.Alignment.ALIGN_RIGHT) {
            final int right = (int) Math.ceil(layout.getLineRight(line));
            scrollx = right - hspace;
        } else {
            scrollx = (int) Math.floor(layout.getLineLeft(line));
        }
        mTextScrollX = scrollx;
    }

    /** bringPointIntoView (:11411-11570), horizontal half. */
    private void bringPointIntoView(Layout layout, int offset) {
        final int line = layout.getLineForOffset(offset);
        final int grav;
        switch (layout.getParagraphAlignment(line)) {
            case ALIGN_LEFT: grav = 1; break;
            case ALIGN_RIGHT: grav = -1; break;
            case ALIGN_NORMAL: grav = layout.getParagraphDirection(line); break;
            case ALIGN_OPPOSITE: grav = -layout.getParagraphDirection(line); break;
            case ALIGN_CENTER:
            default: grav = 0; break;
        }
        final boolean clamped = grav > 0;
        final int x = (int) layout.getPrimaryHorizontal(offset, clamped);
        final int top = layout.getLineTop(line);
        final int bottom = layout.getLineTop(line + 1);
        final int left = (int) Math.floor(layout.getLineLeft(line));
        int right = (int) Math.ceil(layout.getLineRight(line));
        final int hspace = mTv.getWidth() - mTv.getCompoundPaddingLeft() - mTv.getCompoundPaddingRight();
        if (!mTv.mHorizontallyScrolling && right - left > hspace && right > x) {
            right = Math.max(x, left + hspace);
        }
        int hslack = (bottom - top) / 2;
        if (hslack > hspace / 4) hslack = hspace / 4;
        int hs = mTextScrollX;
        if (grav != 0) {
            if (x - hs < hslack) hs = x - hslack;
            if (x - hs > hspace - hslack) hs = x - (hspace - hslack);
        }
        if (grav < 0) {
            if (left - hs > 0) hs = left;
            if (right - hs < hspace) hs = right - hspace;
        } else if (grav > 0) {
            if (right - hs < hspace) hs = right - hspace;
            if (left - hs > 0) hs = left;
        } else {
            if (right - left <= hspace) {
                hs = left - (hspace - (right - left)) / 2;
            } else if (x > right - hslack) {
                hs = right - hspace;
            } else if (x < left + hslack) {
                hs = left;
            } else if (left > hs) {
                hs = left;
            } else if (right < hs + hspace) {
                hs = right - hspace;
            } else {
                if (x - hs < hslack) hs = x - hslack;
                if (x - hs > hspace - hslack) hs = x - (hspace - hslack);
            }
        }
        mTextScrollX = hs;
    }

    /** Where onDraw puts the layout's origin: {x, y} of `translate(compoundPaddingLeft,
     *  extendedPaddingTop + voffset)` (:9036-9043), minus the text's scroll. With
     *  `cursor`, the caret's: AOSP adds cursorOffsetVertical (voffsetCursor - voffsetText). */
    int[] textOrigin(boolean cursor) {
        if (mLayout == null) assumeLayout(); else revalidate();
        preDrawScroll();
        int voffset = 0;
        if ((gravity() & Gravity.VERTICAL_GRAVITY_MASK) != Gravity.TOP) voffset = getVerticalOffset(cursor);
        return new int[] { mTv.getCompoundPaddingLeft() - mTextScrollX, getExtendedPaddingTop() + voffset };
    }

    /** onDraw's text half (:8992-9068), after the compound drawables, which TextView draws.
     *  The caret is TextView's too (macapkDrawCursor, over this same origin). */
    void drawText(android.graphics.Canvas canvas) {
        final TextView tv = mTv;
        if (mLayout == null) assumeLayout(); else revalidate();
        preDrawScroll();
        int color = tv.getCurrentTextColor();
        Layout layout = mLayout;
        final CharSequence hint = tv.getHint();
        if (hint != null && tv.getText().length() == 0) {
            if (tv.getHintTextColors() != null) color = tv.getCurrentHintTextColor();
            layout = mHintLayout;
        }
        if (layout == null) return;
        final TextPaint paint = tv.macapkAospPaint();
        paint.setColor(color);
        paint.drawableState = tv.getDrawableState();
        final float shadowRadius = tv.getShadowRadius(), shadowDx = tv.getShadowDx();
        final float shadowDy = tv.getShadowDy();
        final int shadowColor = tv.getShadowColor();
        if (shadowColor != 0 && (shadowRadius > 0f || shadowDx != 0f || shadowDy != 0f)) {
            paint.setShadowLayer(shadowRadius, shadowDx, shadowDy, shadowColor);
        } else {
            paint.clearShadowLayer();
        }

        final int compoundPaddingLeft = tv.getCompoundPaddingLeft();
        final int compoundPaddingTop = tv.getCompoundPaddingTop();
        final int compoundPaddingRight = tv.getCompoundPaddingRight();
        final int compoundPaddingBottom = tv.getCompoundPaddingBottom();
        final int scrollX = tv.getScrollX();
        final int scrollY = tv.getScrollY();
        final int width = tv.getWidth(), height = tv.getHeight();

        final int saved = canvas.save();
        final int extendedPaddingTop = getExtendedPaddingTop();
        final int extendedPaddingBottom = getExtendedPaddingBottom();
        final int vspace = height - compoundPaddingBottom - compoundPaddingTop;
        final int maxScrollY = mLayout.getHeight() - vspace;
        float clipLeft = compoundPaddingLeft + scrollX;
        float clipTop = (scrollY == 0) ? 0 : extendedPaddingTop + scrollY;
        float clipRight = width - compoundPaddingRight + scrollX;
        float clipBottom = height + scrollY - ((scrollY == maxScrollY) ? 0 : extendedPaddingBottom);
        if (shadowRadius != 0) {
            clipLeft += Math.min(0, shadowDx - shadowRadius);
            clipRight += Math.max(0, shadowDx + shadowRadius);
            clipTop += Math.min(0, shadowDy - shadowRadius);
            clipBottom += Math.max(0, shadowDy + shadowRadius);
        }
        canvas.clipRect(clipLeft, clipTop, clipRight, clipBottom);
        int voffsetText = 0;
        if ((gravity() & Gravity.VERTICAL_GRAVITY_MASK) != Gravity.TOP) voffsetText = getVerticalOffset(false);
        canvas.translate(compoundPaddingLeft - mTextScrollX, extendedPaddingTop + voffsetText);
        try {
            layout.draw(canvas);
        } finally {
            canvas.restoreToCount(saved);
        }
    }
}
