package android.content.res;

/**
 * AOSP's ComplexColor (@hide), the parent of {@link ColorStateList} and of AOSP's GradientColor: what
 * {@code TypedArray.getComplexColor} answers and AOSP's VectorDrawable fills a path with. The same
 * members as android14-release's, so that with WINAPK_AOSP_GFX=1, where lib/aosp-graphics.jar's copy
 * of this class comes first on the class path, ColorStateList links against either. Win.apk's own
 * code never calls it otherwise.
 */
public abstract class ComplexColor {
    private int mChangingConfigurations;

    public boolean isStateful() {
        return false;
    }

    public abstract int getDefaultColor();

    public abstract ConstantState<ComplexColor> getConstantState();

    public abstract boolean canApplyTheme();

    public abstract ComplexColor obtainForTheme(Resources.Theme t);

    final void setBaseChangingConfigurations(int changingConfigurations) {
        mChangingConfigurations = changingConfigurations;
    }

    public int getChangingConfigurations() {
        return mChangingConfigurations;
    }
}
