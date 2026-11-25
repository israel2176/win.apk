// com.macapk.icu.HostIcu -- the host's ICU (macOS libicucore) for the parts of
// android.icu.* that need CLDR data the JDK does not carry.
//
// WHY. android.icu.* on Mac.apk is a front over java.text / java.util, which
// share CLDR locale data with ICU for numbers, dates and collation. Unit
// formatting is the exception: the JDK ships no CLDR unit patterns, so
// "5 kilometer" (nl, WIDE) / "5 km" (SHORT) / "5km" (en, NARROW) cannot be
// produced from Java. macOS itself ships ICU4C as /usr/lib/libicucore.A.dylib
// and exports its C API, including the number-skeleton formatter
// (unumf_openForSkeletonAndLocale / unumf_formatDouble / unumf_formatDecimal /
// unumf_resultToString) -- i.e. the SAME engine and data family Android's
// android.icu (ICU4J) formats units with. Decision ladder rung 1: the host
// supports it natively, route it there. The natives are registered by
// runtime/src/icu_host_bridge.cpp (RegisterNatives at JVM start; on Windows
// windows/host/native_icu.cpp, over icu.dll).
//
// Without a host ICU (Windows before 10 1903 and Wine have no icu.dll; a bare
// JVM registers nothing) every call answers from Java instead, IcuFallback:
// the number from java.text in the locale, the unit as CLDR's root locale
// writes it, LocaleData from CLDR's supplemental territory lists and root.
// Android's android.icu never fails here, and an exception would kill a Hermes
// app (its Intl.NumberFormat throws through a JNI frame into std::terminate).
// The first such call says so once in the log. An ICU failure status from a
// host that has ICU (IllegalArgumentException) is still thrown as before.
//
// Also the home of MACAPK_ICU_PLATFORM_TYPES (default ON): android.icu
// factories return ICU's concrete types (DecimalFormat / SimpleDateFormat /
// RuleBasedCollator), formatToCharacterIterator carries ICU Field attributes,
// and DateFormat.getInstanceForSkeleton honours the skeleton. =0 restores the
// pre-change opaque wrappers, attribute-less iterators and the skeleton-blind
// getInstanceForSkeleton (= getDateInstance()), byte-identically.
// docs/android-icu-hermes-intl-surface_2026-10-01.md.
package com.macapk.icu;

public final class HostIcu {
    private HostIcu() {}


    // Bound by RegisterNatives (icu_host_bridge.cpp, kIcuHostMethods). Both throw
    // IllegalArgumentException("<u_errorName>") on an ICU failure status.
    private static native String nativeFormatDouble(String skeleton, String icuLocaleId, double value);
    private static native String nativeFormatDecimal(String skeleton, String icuLocaleId, String decimal);

    // android.icu.util.LocaleData over ICU4C's ulocdata_* (same file, kIcuHostMethods).
    // Lookup failures throw java.util.MissingResourceException, as ICU4J does.
    private static native int nativeMeasurementSystem(String icuLocaleId);
    private static native int[] nativePaperSize(String icuLocaleId);
    private static native int[] nativeCldrVersion();
    private static native String nativeDelimiter(String icuLocaleId, boolean noSubstitute, int type);
    private static native Object[] nativeExemplarSet(String icuLocaleId, boolean noSubstitute,
            int options, int icu4cExtype);
    private static native String nativeLocaleDisplayPattern(String icuLocaleId);
    private static native String nativeLocaleSeparator(String icuLocaleId);

    // The two families of natives (number skeletons; LocaleData), each set by
    // the first call that finds no host ICU for it: later calls go straight to
    // IcuFallback. Two, as native_icu.cpp looks each family up on its own: an
    // icu.dll without ulocdata_* still formats units.
    private static final int FORMAT = 0, LOCALE_DATA = 1;
    private static final boolean[] sNoHostIcu = new boolean[2];

    /** True while the natives of `family` answer. */
    private static boolean host(int family) {
        synchronized (sNoHostIcu) {
            return !sNoHostIcu[family];
        }
    }

    /**
     * The natives' "no ICU here": UnsatisfiedLinkError (nothing registered) or
     * UnsupportedOperationException (native_icu.cpp: no icu.dll, or one without
     * the functions). Said once in the log per family.
     */
    private static void noHostIcu(int family, Throwable why) {
        synchronized (sNoHostIcu) {
            if (sNoHostIcu[family]) return;
            sNoHostIcu[family] = true;
        }
        String what = why instanceof UnsatisfiedLinkError
                ? "the host ICU bridge is not registered in this process" : String.valueOf(why.getMessage());
        System.err.println("[win.apk] android.icu: " + what + "; "
                + (family == FORMAT ? "units are formatted from Java (the locale's numbers, CLDR root's unit symbols)"
                        : "LocaleData answers from CLDR's territory lists and root locale")
                + " instead");
    }

    /** ICU4C UMeasurementSystem: 0 SI, 1 US, 2 UK. Region from rg / region / likely subtags. */
    public static int measurementSystem(String icuLocaleId) {
        if (host(LOCALE_DATA)) {
            try { return nativeMeasurementSystem(icuLocaleId); }
            catch (UnsatisfiedLinkError | UnsupportedOperationException e) { noHostIcu(LOCALE_DATA, e); }
        }
        return IcuFallback.measurementSystem(icuLocaleId);
    }

    /** {height, width} in millimetres. */
    public static int[] paperSize(String icuLocaleId) {
        if (host(LOCALE_DATA)) {
            try { return nativePaperSize(icuLocaleId); }
            catch (UnsatisfiedLinkError | UnsupportedOperationException e) { noHostIcu(LOCALE_DATA, e); }
        }
        return IcuFallback.paperSize(icuLocaleId);
    }

    /** The host ICU's CLDR version, {major, minor, milli, micro}. */
    public static int[] cldrVersion() {
        if (host(LOCALE_DATA)) {
            try { return nativeCldrVersion(); }
            catch (UnsatisfiedLinkError | UnsupportedOperationException e) { noHostIcu(LOCALE_DATA, e); }
        }
        return IcuFallback.cldrVersion();
    }

    /** One quotation delimiter (ULocaleDataDelimiterType 0-3); null when there is none. */
    public static String delimiter(String icuLocaleId, boolean noSubstitute, int type) {
        if (host(LOCALE_DATA)) {
            try { return nativeDelimiter(icuLocaleId, noSubstitute, type); }
            catch (UnsatisfiedLinkError | UnsupportedOperationException e) { noHostIcu(LOCALE_DATA, e); }
        }
        return IcuFallback.delimiter(icuLocaleId, noSubstitute, type);
    }

    /**
     * An exemplar set as {int[] start,end pairs, String[] multi-character strings};
     * null when ICU has none (or, with noSubstitute, only root has one).
     */
    public static Object[] exemplarSet(String icuLocaleId, boolean noSubstitute, int options,
            int icu4cExtype) {
        if (host(LOCALE_DATA)) {
            try { return nativeExemplarSet(icuLocaleId, noSubstitute, options, icu4cExtype); }
            catch (UnsatisfiedLinkError | UnsupportedOperationException e) { noHostIcu(LOCALE_DATA, e); }
        }
        return IcuFallback.exemplarSet();
    }

    public static String localeDisplayPattern(String icuLocaleId) {
        if (host(LOCALE_DATA)) {
            try { return nativeLocaleDisplayPattern(icuLocaleId); }
            catch (UnsatisfiedLinkError | UnsupportedOperationException e) { noHostIcu(LOCALE_DATA, e); }
        }
        return IcuFallback.localeDisplayPattern();
    }

    public static String localeSeparator(String icuLocaleId) {
        if (host(LOCALE_DATA)) {
            try { return nativeLocaleSeparator(icuLocaleId); }
            catch (UnsatisfiedLinkError | UnsupportedOperationException e) { noHostIcu(LOCALE_DATA, e); }
        }
        return IcuFallback.localeSeparator();
    }

    /**
     * Format a number with an ICU number skeleton ("measure-unit/length-kilometer
     * unit-width-full-name .00") in an ICU locale ID ("nl_NL", "en@numbers=arab").
     */
    public static String formatSkeleton(String skeleton, String icuLocaleId, Number value) {
        boolean asDouble = value instanceof Double || value instanceof Float;
        String dec = null;
        if (!asDouble) {
            if (value instanceof java.math.BigDecimal) dec = ((java.math.BigDecimal) value).toString();
            else if (value instanceof java.math.BigInteger) dec = value.toString();
            else if (value instanceof Long || value instanceof Integer
                    || value instanceof Short || value instanceof Byte) dec = Long.toString(value.longValue());
            else asDouble = true;
        }
        if (host(FORMAT)) {
            try {
                return asDouble ? nativeFormatDouble(skeleton, icuLocaleId, value.doubleValue())
                        : nativeFormatDecimal(skeleton, icuLocaleId, dec);
            } catch (UnsatisfiedLinkError | UnsupportedOperationException e) {
                noHostIcu(FORMAT, e);
            }
        }
        return IcuFallback.formatSkeleton(skeleton, icuLocaleId, asDouble ? (Object) value.doubleValue() : dec);
    }
}
