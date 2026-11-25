package android.content.res;

/**
 * AOSP's ConstantState (@hide): a resource's shared state, which {@link ComplexColor} hands out.
 * android14-release's members; lib/aosp-graphics.jar carries AOSP's own copy (WINAPK_AOSP_GFX=1).
 */
public abstract class ConstantState<T> {
    public abstract int getChangingConfigurations();

    public abstract T newInstance();

    public T newInstance(Resources res) {
        return newInstance();
    }

    public T newInstance(Resources res, Resources.Theme theme) {
        return newInstance(res);
    }
}
