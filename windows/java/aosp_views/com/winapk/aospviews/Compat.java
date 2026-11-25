// What AOSP's android.view / android.widget call that Win.apk's own classes (android.app, android.os,
// android.content...) do not have, answered as AOSP's would for an app process with no system
// server behind it. AospViewsRewrite.java (windows/scripts) sends those calls and field reads here:
// a static keeps its descriptor, an instance call passes its receiver first. Found by LinkScan
// (build_aosp_views.sh lists what is left).
package com.winapk.aospviews;

import android.content.Context;

public final class Compat {
    private Compat() {}

    // ---- android.os.Build (fields AOSP's Build has and Win.apk's lacks) ----
    public static final int HW_TIMEOUT_MULTIPLIER = 1;
    public static final boolean IS_ENG = false;
    public static final boolean IS_USER = true;
    /** Build.VERSION.RESOURCES_SDK_INT: the level of the framework's resources, android-all 14's. */
    public static final int RESOURCES_SDK_INT = 34;

    // ---- android.app ----
    /** ActivityThread.isSystem(): an app process. */
    public static boolean isSystem() { return false; }
    public static boolean isLowRamDeviceStatic() { return false; }
    public static boolean isSystemReady() { return true; }

    // ---- android.animation (Win.apk's) ----

    /** Animator.getBackgroundPauseDelay: how long AnimationHandler waits, once no window wants
     *  animators, before pausing them (AOSP's default, Animator.sBackgroundPauseDelay; a window's
     *  ViewRootImpl.doDie asks). */
    public static long getBackgroundPauseDelay() { return 1000; }

    // ---- android.view.SurfaceControl.Transaction (Win.apk's; the compositor here is Compositor) ----
    // What AOSP's views ask of a transaction that Win.apk's lacks, none of which the in-process
    // compositor has: a sync group's transaction made safe for another process (sanitize), blur
    // regions, shadows, dimming, the display's decoration, a frame timeline, a frame-rate vote.

    public static void sanitize(android.view.SurfaceControl.Transaction t, int pid, int uid) {}

    public static void clear(android.view.SurfaceControl.Transaction t) {}

    public static android.view.SurfaceControl.Transaction remove(android.view.SurfaceControl.Transaction t,
            android.view.SurfaceControl sc) { return t; }

    public static android.view.SurfaceControl.Transaction setBlurRegions(android.view.SurfaceControl.Transaction t,
            android.view.SurfaceControl sc, float[][] regions) { return t; }

    public static android.view.SurfaceControl.Transaction setDefaultFrameRateCompatibility(
            android.view.SurfaceControl.Transaction t, android.view.SurfaceControl sc, int compatibility) { return t; }

    public static android.view.SurfaceControl.Transaction setDimmingEnabled(android.view.SurfaceControl.Transaction t,
            android.view.SurfaceControl sc, boolean enabled) { return t; }

    public static android.view.SurfaceControl.Transaction setDisplayDecoration(
            android.view.SurfaceControl.Transaction t, android.view.SurfaceControl sc, boolean on) { return t; }

    public static android.view.SurfaceControl.Transaction setFrameTimelineVsync(
            android.view.SurfaceControl.Transaction t, long vsyncId) { return t; }

    public static android.view.SurfaceControl.Transaction setShadowRadius(android.view.SurfaceControl.Transaction t,
            android.view.SurfaceControl sc, float radius) { return t; }

    // ---- android.text.TextUtils / android.os.StrictMode (hidden) ----
    public static String emptyIfNull(String s) { return s == null ? "" : s; }
    public static String formatSimple(String format, Object... args) { return String.format(format, args); }
    public static void noteResourceMismatch(Object tag) {}

    // ---- android.text ----

    /** Spannable.removeSpan(Object, int), API 29's default method, which Win.apk's interface lacks:
     *  its default body (AOSP's Selection.removeSelection calls it, from LinkMovementMethod). */
    public static void removeSpan(android.text.Spannable s, Object what, int flags) { s.removeSpan(what); }

    // ---- ICU (Win.apk's android.icu is java.text's; what the key listeners ask of it) ----

    /** UCharacter.getCombiningClass: the Unicode canonical combining class (BaseKeyListener's
     *  backspace keeps a mark with its base: it only asks whether it is 0). java.text.Normalizer
     *  has the Unicode data but no getter, so the class is read off canonical reordering: in NFD,
     *  a mark of class c moves before a preceding mark of a higher class. U+0334 has class 1 and
     *  U+0345 class 240, the lowest and highest a mark has; a precomposed character's class is
     *  that of its decomposition's first character. Exact for zero or not, which is all it is
     *  asked; a nonzero answer is 1, 230 or 240. */
    public static int getCombiningClass(int cp) {
        java.text.Normalizer.Form nfd = java.text.Normalizer.Form.NFD;
        String c = new String(Character.toChars(
                java.text.Normalizer.normalize(new String(Character.toChars(cp)), nfd).codePointAt(0)));
        String lowAfter = "a" + c + "̴";   // c moves after U+0334 iff its class > 1
        String highBefore = "aͅ" + c;      // c moves before U+0345 iff 0 < its class < 240
        boolean above1 = !java.text.Normalizer.normalize(lowAfter, nfd).equals(lowAfter);
        boolean below240 = !java.text.Normalizer.normalize(highBefore, nfd).equals(highBefore);
        if (above1) return below240 ? 230 : 240;
        return below240 ? 1 : 0;
    }

    /** UCharacter.hasBinaryProperty for the properties AOSP's text code asks (UProperty's numbers):
     *  the emoji ones from java.lang.Character's own Unicode data (Java 21), the variation
     *  selectors and regional indicators by their code points. Any other property: false. */
    public static boolean hasBinaryProperty(int cp, int property) {
        switch (property) {
            case 36: return (cp >= 0x180B && cp <= 0x180D) || cp == 0x180F || (cp >= 0xFE00 && cp <= 0xFE0F)
                    || (cp >= 0xE0100 && cp <= 0xE01EF);                       // VARIATION_SELECTOR
            case 57: return Character.isEmoji(cp);                             // EMOJI
            case 58: return Character.isEmojiPresentation(cp);                 // EMOJI_PRESENTATION
            case 59: return Character.isEmojiModifier(cp);                     // EMOJI_MODIFIER
            case 60: return Character.isEmojiModifierBase(cp);                 // EMOJI_MODIFIER_BASE
            case 61: return Character.isEmojiComponent(cp);                    // EMOJI_COMPONENT
            case 62: return cp >= 0x1F1E6 && cp <= 0x1F1FF;                    // REGIONAL_INDICATOR
            case 64: return Character.isExtendedPictographic(cp);              // EXTENDED_PICTOGRAPHIC
            default: return false;
        }
    }

    /** UCharacter.getIntPropertyValue for what AOSP's text code asks: WORD_BREAK (4116), from
     *  WordIterator.isMidWordPunctuation, which only tells MidLetter (4), MidNumLet (11) and
     *  Single_Quote (15) from the rest; those are Unicode 15's WordBreakProperty.txt lists.
     *  Anything else: 0 (Other). */
    public static int getIntPropertyValue(int cp, int property) {
        if (property != 4116) return 0;
        switch (cp) {
            case 0x003A: case 0x00B7: case 0x0387: case 0x055F: case 0x05F4: case 0x2027: case 0xFE13:
            case 0xFE55: case 0xFF1A:
                return 4;
            case 0x002E: case 0x2018: case 0x2019: case 0x2024: case 0xFE52: case 0xFF07: case 0xFF0E:
                return 11;
            case 0x0027:
                return 15;
            default:
                return 0;
        }
    }

    /** DecimalFormatSymbols.getDecimalSeparatorString (DigitsKeyListener's locale's separator). */
    public static String getDecimalSeparatorString(android.icu.text.DecimalFormatSymbols s) {
        return String.valueOf(s.getDecimalSeparator());
    }

    /** DateFormat.getIcuDateFormatSymbols (NumberKeyListener's AM/PM characters, TimePicker). */
    public static android.icu.text.DateFormatSymbols getIcuDateFormatSymbols(java.util.Locale locale) {
        return android.icu.text.DateFormatSymbols.getInstance(locale);
    }

    // ---- android.media.AudioManager (hidden; on input, ViewRootImpl's sounds, the fallback keys) ----
    // A phone's default config_navigationRepeatSoundEffectsEnabled is false: one click sound only.
    public static boolean areNavigationRepeatSoundEffectsEnabled(android.media.AudioManager a) { return false; }
    public static int getNthNavigationRepeatSoundEffect(android.media.AudioManager a, int n) {
        return android.media.AudioManager.FX_KEY_CLICK;
    }
    // The audio service's volume-key preprocessing (the volume panel's long press): none on the host.
    public static void preDispatchKeyEvent(android.media.AudioManager a, android.view.KeyEvent e, int stream) {}

    /** ActivityThread.getSystemContext (a ContextImpl on Android; AospViewsRewrite): the process's
     *  Application, the one context an app process has here. An AIDL Stub's default constructor
     *  hands it to PermissionEnforcer.fromContext; the in-process services use LocalEnforcer. */
    public static Context systemContext(android.app.ActivityThread thread) {
        try {  // Win.apk's Application.macapk_get (this jar compiles against android-all's Application)
            return (Context) android.app.Application.class.getMethod("macapk_get").invoke(null);
        } catch (ReflectiveOperationException e) {
            return null;
        }
    }

    // ---- android.content.Context (hidden; ContextImpl's answers for an app's context) ----
    private static final java.util.concurrent.atomic.AtomicInteger sAutofillId =
            new java.util.concurrent.atomic.AtomicInteger(1073741824);

    public static int getDisplayId(Context c) { return android.view.Display.DEFAULT_DISPLAY; }
    public static android.view.Display getDisplayNoVerify(Context c) { return AospWindows.defaultDisplay(); }
    public static void updateDisplay(Context c, int displayId) {}
    public static boolean isAutofillCompatibilityEnabled(Context c) { return false; }
    public static android.content.AutofillOptions getAutofillOptions(Context c) { return null; }
    public static android.content.ContentCaptureOptions getContentCaptureOptions(Context c) { return null; }
    public static android.view.autofill.AutofillManager.AutofillClient getAutofillClient(Context c) { return null; }
    public static int getNextAutofillId(Context c) { return sAutofillId.getAndIncrement(); }
    public static boolean canLoadUnsafeResources(Context c) { return true; }
    public static boolean canStartActivityForResult(Context c) { return c instanceof android.app.Activity; }
    public static boolean isConfigurationContext(Context c) { return false; }
    public static android.os.Handler getMainThreadHandler(Context c) {
        return new android.os.Handler(android.os.Looper.getMainLooper());
    }
    public static android.os.IBinder getWindowContextToken(Context c) { return null; }
    /** Context.getToken(Context): the activity token, else the window context's; neither here. */
    public static android.os.IBinder getToken(Context c) { return null; }

    // ---- android.content.pm.ApplicationInfo (hidden) ----
    public static boolean hasRtlSupport(android.content.pm.ApplicationInfo info) {
        return (info.flags & android.content.pm.ApplicationInfo.FLAG_SUPPORTS_RTL) != 0;
    }
    public static boolean isEmbeddedDexUsed(android.content.pm.ApplicationInfo info) { return false; }
    public static boolean isPrivilegedApp(android.content.pm.ApplicationInfo info) { return false; }

    // ---- android.view.Surface (Win.apk's) ----
    /** A compatibility-mode translator scales a window for an app that does not support the
     *  display's density: Win.apk never scales one. */
    public static void setCompatibilityTranslator(android.view.Surface s,
            android.content.res.CompatibilityInfo.Translator translator) {}


    // ---- android.os / android.util / android.text (hidden statics) ----
    public static void assertConfigurationContext(Context c, String methodName) {}
    public static void onIncorrectContextUsed(String message, Throwable originStack) {}
    public static boolean vmFileUriExposureEnabled() { return false; }

    /** TextUtils.isPunctuation: AOSP's set of Unicode punctuation categories. */
    public static boolean isPunctuation(int codePoint) {
        int type = Character.getType(codePoint);
        return type == Character.CONNECTOR_PUNCTUATION || type == Character.DASH_PUNCTUATION
                || type == Character.END_PUNCTUATION || type == Character.FINAL_QUOTE_PUNCTUATION
                || type == Character.INITIAL_QUOTE_PUNCTUATION || type == Character.OTHER_PUNCTUATION
                || type == Character.START_PUNCTUATION;
    }
    /** TextUtils.isWhitespace: AOSP's list (Character.isWhitespace's and the no-break spaces). */
    public static boolean isWhitespace(int c) {
        return (c >= 0x0009 && c <= 0x000D) || c == 0x0020 || c == 0x0085 || c == 0x00A0 || c == 0x1680
                || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F
                || c == 0x3000;
    }
    public static boolean isWhitespaceExceptNewline(int c) { return isWhitespace(c) && c != '\n'; }
    public static int length(String s) { return s != null ? s.length() : 0; }
    public static String safeIntern(String s) { return s != null ? s.intern() : null; }
    public static CharSequence trimNoCopySpans(CharSequence text) { return text; }
    public static CharSequence trimToParcelableSize(CharSequence text) {
        return text != null && text.length() > 100_000 ? text.subSequence(0, 100_000) : text;
    }
    public static String formatUptime(long time) { return Long.toString(time); }
    public static String logTimeOfDay(long millis) { return Long.toString(millis); }
    public static void instant(long traceTag, String name) {}
    public static void instantForTrack(long traceTag, String trackName, String name) {}
    public static void asyncTraceForTrackBegin(long traceTag, String trackName, String name, int cookie) {}
    public static void asyncTraceForTrackEnd(long traceTag, String trackName, int cookie) {}
    public static void setTraceTag(android.os.Looper looper, long traceTag) {}
    public static void setSlowLogThresholdMs(android.os.Looper looper, long slowDispatchMs, long slowDeliveryMs) {}
    public static String colorModeToString(int colorMode) { return Integer.toString(colorMode); }
    public static String screenOrientationToString(int orientation) { return Integer.toString(orientation); }
    public static String getCallers(int depth) {
        StackTraceElement[] st = new Throwable().getStackTrace();
        StringBuilder sb = new StringBuilder();
        for (int i = 2; i < st.length && i < depth + 2; i++) sb.append(st[i]).append(' ');
        return sb.toString().trim();
    }
    public static long countInstancesOfClass(Class<?> klass) { return 0; }
    public static String flagsToString(Class<?> clazz, String prefix, long flags) { return Long.toHexString(flags); }
    private static final java.util.Map<android.os.HandlerThread, android.os.Handler> sThreadHandlers =
            java.util.Collections.synchronizedMap(new java.util.WeakHashMap<>());
    public static android.os.Handler getThreadHandler(android.os.HandlerThread t) {
        synchronized (sThreadHandlers) {
            android.os.Handler h = sThreadHandlers.get(t);
            if (h == null && t.getLooper() != null) sThreadHandlers.put(t, h = new android.os.Handler(t.getLooper()));
            return h;
        }
    }
    public static int getCallingUserId() { return 0; }
    public static boolean isProxy(android.os.IInterface iface) { return false; }

    /** CompatChanges / Compatibility.isChangeEnabled: an app process asks AppCompatCallbacks, which
     *  system_server filled with the changes disabled for the app's target SDK. Win.apk presents
     *  the app's target SDK (PresentedSdk), so every change the views gate is on. (AOSP's default
     *  delegate would log through ART's System.logW, which HotSpot lacks.) */
    public static boolean isChangeEnabled(long changeId) { return true; }

    /** SystemProperties.find: a property's handle, which PropertyInvalidatedCache reads its cache
     *  nonce through. None here (Win.apk's SystemProperties has no handles): the nonce is unset,
     *  and the cache queries the service every time, as before system_server sets one. */
    public static android.os.SystemProperties.Handle find(String name) { return null; }

    /** TextUtils.toUpperCase(Locale, CharSequence, boolean copySpans) (AllCapsTransformationMethod):
     *  the text upper-cased in the locale, the source itself when nothing changes, and its spans
     *  carried over to where their characters went (one character can become several: ß, ŉ). AOSP
     *  maps through ICU's CaseMap edits; here each code point is mapped on its own, which is the
     *  same for every mapping but the context-dependent Greek and Lithuanian ones. */
    public static CharSequence toUpperCase(java.util.Locale locale, CharSequence source, boolean copySpans) {
        if (locale == null) locale = java.util.Locale.getDefault();
        int n = source.length();
        StringBuilder out = new StringBuilder(n);
        int[] map = new int[n + 1];
        for (int i = 0; i < n; ) {
            int cp = Character.codePointAt(source, i);
            int len = Character.charCount(cp);
            for (int k = 0; k < len; k++) map[i + k] = out.length();
            out.append(new String(Character.toChars(cp)).toUpperCase(locale));
            i += len;
        }
        map[n] = out.length();
        if (out.toString().contentEquals(source)) return source;
        if (!copySpans || !(source instanceof android.text.Spanned spanned)) return out;
        android.text.SpannableStringBuilder result = new android.text.SpannableStringBuilder(out);
        for (Object span : spanned.getSpans(0, n, Object.class)) {
            result.setSpan(span, map[spanned.getSpanStart(span)], map[spanned.getSpanEnd(span)],
                    spanned.getSpanFlags(span));
        }
        return result;
    }

    /** Activity.getActivityInfo (hidden), for WindowOnBackInvokedDispatcher's check of the
     *  activity's android:enableOnBackInvokedCallback: none here, so the check falls back to the
     *  application's. Predictive back (OnBackInvokedCallback dispatch) is V2's: the application's
     *  answer is "not enabled", so back goes through onBackPressed and KEYCODE_BACK, as on an app
     *  that did not opt in. */
    public static android.content.pm.ActivityInfo getActivityInfo(android.app.Activity a) { return null; }
    public static boolean hasOnBackInvokedCallbackEnabled(android.content.pm.ActivityInfo info) { return false; }
    public static boolean isOnBackInvokedCallbackEnabled(android.content.pm.ActivityInfo info) { return false; }
    public static boolean isOnBackInvokedCallbackEnabled(android.content.pm.ApplicationInfo info) { return false; }
}
