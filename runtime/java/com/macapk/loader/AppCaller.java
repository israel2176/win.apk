package com.macapk.loader;

/** Whether the code that called a framework method is the app's own: its
 *  classes (and the libraries it bundles, androidx included) come from an
 *  app class loader, while the framework's come from the JVM's system or
 *  platform loader, or the boot loader.
 *
 *  A few framework answers are spelled two ways in Win.apk: the way Android
 *  answers the app, and the way Win.apk's own framework code resolves them
 *  (an attribute's "@string/name" where Android answers "@2131886250"). This
 *  decides which spelling a caller gets, so the app sees Android's and the
 *  framework keeps the spelling it was written for.
 *  MACAPK_ATTR_TEXT_FOR_APPS=1 gives the app the framework's spelling again. */
public final class AppCaller {
    private AppCaller() {}


    private static final StackWalker WALKER =
        StackWalker.getInstance(StackWalker.Option.RETAIN_CLASS_REFERENCE);

    /** True when the caller of the method that calls this is app code.
     *  `framesAbove` counts the framework frames between this call and that
     *  method's caller (0: the method calling isApp is the one the app called). */
    public static boolean isApp(int framesAbove) {
        try {
            Class<?> c = WALKER.walk(s -> s.skip(2 + framesAbove).findFirst()
                .map(StackWalker.StackFrame::getDeclaringClass).orElse(null));
            if (c == null) return false;
            ClassLoader cl = c.getClassLoader();
            return cl != null && cl != ClassLoader.getSystemClassLoader()
                && cl != ClassLoader.getPlatformClassLoader();
        } catch (Throwable t) {
            return false;
        }
    }
}
