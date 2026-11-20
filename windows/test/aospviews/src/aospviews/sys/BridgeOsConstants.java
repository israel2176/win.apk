package aospviews.sys;

/** android.system.OsConstants.initConstants (libcore's, from the C headers): the Linux values, as
 *  Robolectric's ShadowOsConstants sets them. Win.apk's own libcore has its table. */
public final class BridgeOsConstants {
    private BridgeOsConstants() {}

    public static void initConstants() {
        try {
            java.lang.reflect.Method m = Class.forName("org.robolectric.shadows.ShadowOsConstants")
                    .getDeclaredMethod("shadowInitConstants");
            m.setAccessible(true);
            m.invoke(null);
        } catch (ReflectiveOperationException e) {
            throw new IllegalStateException(e);
        }
    }
}
