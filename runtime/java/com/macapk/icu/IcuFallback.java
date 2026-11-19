// com.macapk.icu.IcuFallback -- HostIcu's answers when the host has no ICU:
// Windows before 10 1903 has no icu.dll, nor has Wine, and a bare JVM has no
// bridge registered at all.
//
// WHY. An app on Android never sees android.icu fail: Hermes' Intl.NumberFormat
// {style: "unit"} formats through MeasureFormat, and an exception there crosses
// a JNI frame Hermes does not catch it at, so the C++ runtime terminates the
// process (Bank Leumi under Wine). Degraded text beats a dead app, so each
// HostIcu call answers from what Java has:
//   * the number of a skeleton: java.text.DecimalFormat in the locale (its
//     digits, separators, grouping; the skeleton's fraction or significant-digit
//     window, minimum integer digits, grouping, rounding mode and sign), the
//     same CLDR family the JDK carries for numbers;
//   * the unit: the JDK has no unit patterns, so the symbol CLDR's root locale
//     writes ("{0} km", "{0} h", "{0}°C"): what ICU itself shows for a locale
//     with no unit data of its own. Percent and permille take the locale's own
//     pattern from java.text. One table for every width: root's long and narrow
//     forms are its short ones. A unit not in the table is written "{0} <subtype>";
//   * LocaleData: CLDR's supplemental territory lists for the measurement
//     system and paper size (keyed by the locale's region), root's quotation
//     marks, locale display pattern and separator, no exemplar set (root has
//     none), and the CLDR version of the JDK's own locale data.
// What ICU rejects (an unknown skeleton stem) is not checked here: the answer
// is a best effort, and only when the host's ICU is missing; with it, HostIcu
// never comes here.
package com.macapk.icu;

import java.math.BigDecimal;
import java.math.BigInteger;
import java.math.MathContext;
import java.math.RoundingMode;
import java.text.DecimalFormat;
import java.text.DecimalFormatSymbols;
import java.text.NumberFormat;
import java.util.HashMap;
import java.util.Locale;
import java.util.Map;

final class IcuFallback {
    private IcuFallback() {}

    // CLDR root's short unit patterns (common/main/root.xml, <unitLength type="short">)
    // for the units android.icu.util.MeasureUnit defines, by "<type>-<subtype>".
    private static final Map<String, String> ROOT_UNITS = new HashMap<>();

    static {
        String[] t = {
            "area-acre", "{0} ac", "area-hectare", "{0} ha",
            "digital-bit", "{0} bit", "digital-byte", "{0} byte",
            "digital-kilobit", "{0} kb", "digital-kilobyte", "{0} kB",
            "digital-megabit", "{0} Mb", "digital-megabyte", "{0} MB",
            "digital-gigabit", "{0} Gb", "digital-gigabyte", "{0} GB",
            "digital-terabit", "{0} Tb", "digital-terabyte", "{0} TB",
            "digital-petabyte", "{0} PB",
            "angle-degree", "{0}°",
            "length-millimeter", "{0} mm", "length-centimeter", "{0} cm", "length-meter", "{0} m",
            "length-kilometer", "{0} km", "length-inch", "{0} in", "length-foot", "{0} ft",
            "length-yard", "{0} yd", "length-mile", "{0} mi", "length-mile-scandinavian", "{0} smi",
            "mass-gram", "{0} g", "mass-kilogram", "{0} kg", "mass-ounce", "{0} oz",
            "mass-pound", "{0} lb", "mass-stone", "{0} st",
            "volume-milliliter", "{0} mL", "volume-liter", "{0} L", "volume-gallon", "{0} gal",
            "volume-fluid-ounce", "{0} fl oz",
            "duration-nanosecond", "{0} ns", "duration-microsecond", "{0} μs",
            "duration-millisecond", "{0} ms", "duration-second", "{0} s", "duration-minute", "{0} min",
            "duration-hour", "{0} h", "duration-day", "{0} d", "duration-week", "{0} w",
            "duration-month", "{0} m", "duration-year", "{0} y",
            "temperature-celsius", "{0}°C", "temperature-fahrenheit", "{0}°F",
            "speed-kilometer-per-hour", "{0} km/h", "speed-meter-per-second", "{0} m/s",
            "speed-mile-per-hour", "{0} mi/h",
        };
        for (int i = 0; i < t.length; i += 2) ROOT_UNITS.put(t[i], t[i + 1]);
    }

    /** An ICU locale ID ("nl_NL", "ar_EG@numbers=latn", "") as a java.util.Locale. */
    static Locale locale(String icuLocaleId) {
        String id = icuLocaleId == null ? "" : icuLocaleId;
        String keywords = "";
        int at = id.indexOf('@');
        if (at >= 0) {
            keywords = id.substring(at + 1);
            id = id.substring(0, at);
        }
        Locale base = id.isEmpty() ? Locale.ROOT : Locale.forLanguageTag(id.replace('_', '-'));
        if (keywords.isEmpty()) return base;
        Locale.Builder b = new Locale.Builder().setLocale(base);
        for (String kv : keywords.split(";")) {
            int eq = kv.indexOf('=');
            if (eq <= 0) continue;
            String key = kv.substring(0, eq).trim();
            String value = kv.substring(eq + 1).trim();
            try {
                if (key.equals("numbers")) b.setUnicodeLocaleKeyword("nu", value);
                else if (key.equals("rg")) b.setUnicodeLocaleKeyword("rg", value);
            } catch (RuntimeException ignored) {
                // a value BCP 47 cannot carry: ICU would ignore it too
            }
        }
        return b.build();
    }

    // ---- number skeletons ------------------------------------------------------------

    /** What MeasureFormat.skeleton() writes, read back. */
    private static final class Skeleton {
        String unit;                 // "<type>-<subtype>", or null
        String width = "short";      // full-name, short, narrow, iso-code, hidden
        int minFraction = 0, maxFraction = 6;
        int minSignificant = -1, maxSignificant = -1;
        int minInteger = 1;
        boolean grouping = true;
        RoundingMode rounding = RoundingMode.HALF_EVEN;
        boolean signAlways;
    }

    private static Skeleton parse(String skeleton) {
        Skeleton s = new Skeleton();
        for (String stem : skeleton.trim().split("\\s+")) {
            if (stem.isEmpty()) continue;
            if (stem.startsWith("measure-unit/")) {
                s.unit = stem.substring("measure-unit/".length());
            } else if (stem.startsWith("unit-width-")) {
                s.width = stem.substring("unit-width-".length());
            } else if (stem.equals("precision-integer")) {
                s.minFraction = 0;
                s.maxFraction = 0;
            } else if (stem.startsWith(".")) {
                int zeros = 0, hashes = 0;
                for (int i = 1; i < stem.length(); i++) {
                    char c = stem.charAt(i);
                    if (c == '0') zeros++;
                    else if (c == '#') hashes++;
                    else if (c == '*' || c == '+') hashes = 100 - zeros;  // unlimited
                }
                s.minFraction = zeros;
                s.maxFraction = zeros + hashes;
            } else if (stem.startsWith("@")) {
                int at = 0, hashes = 0;
                for (int i = 0; i < stem.length(); i++) {
                    char c = stem.charAt(i);
                    if (c == '@') at++;
                    else if (c == '#') hashes++;
                    else if (c == '*' || c == '+') hashes = 999 - at;
                }
                s.minSignificant = at;
                s.maxSignificant = at + hashes;
            } else if (stem.startsWith("integer-width/")) {
                int zeros = 0;
                for (char c : stem.substring("integer-width/".length()).toCharArray()) if (c == '0') zeros++;
                s.minInteger = Math.max(1, zeros);
            } else if (stem.equals("group-off")) {
                s.grouping = false;
            } else if (stem.startsWith("rounding-mode-")) {
                switch (stem.substring("rounding-mode-".length())) {
                    case "up": s.rounding = RoundingMode.UP; break;
                    case "down": s.rounding = RoundingMode.DOWN; break;
                    case "ceiling": s.rounding = RoundingMode.CEILING; break;
                    case "floor": s.rounding = RoundingMode.FLOOR; break;
                    case "half-up": s.rounding = RoundingMode.HALF_UP; break;
                    case "half-down": s.rounding = RoundingMode.HALF_DOWN; break;
                    case "unnecessary": s.rounding = RoundingMode.UNNECESSARY; break;
                    default: s.rounding = RoundingMode.HALF_EVEN; break;
                }
            } else if (stem.equals("sign-always")) {
                s.signAlways = true;
            }
            // Other stems MeasureFormat never writes: left at ICU's defaults.
        }
        return s;
    }

    /** A decimal string ("12.5", "-3", "1E+3") or a double, as a BigDecimal; null for NaN and infinity. */
    private static BigDecimal decimal(Object value) {
        if (value instanceof BigDecimal) return (BigDecimal) value;
        if (value instanceof BigInteger) return new BigDecimal((BigInteger) value);
        if (value instanceof String) return new BigDecimal((String) value);
        double d = ((Number) value).doubleValue();
        if (Double.isNaN(d) || Double.isInfinite(d)) return null;
        return new BigDecimal(Double.toString(d));  // the shortest form, as ICU reads a double
    }

    static String formatSkeleton(String skeleton, String icuLocaleId, Object value) {
        Skeleton s = parse(skeleton == null ? "" : skeleton);
        Locale locale = locale(icuLocaleId);
        String unit = s.unit;
        boolean percent = "concentr-percent".equals(unit) || "percent-percent".equals(unit)
                || "concentr-permille".equals(unit) || "permille-permille".equals(unit);
        DecimalFormat df;
        if (percent) {
            df = (DecimalFormat) NumberFormat.getPercentInstance(locale);
            df.setMultiplier(1);
            if (unit.endsWith("permille")) {
                // The locale's percent pattern with its per-mille sign in the percent sign's place.
                DecimalFormatSymbols sym = df.getDecimalFormatSymbols();
                String pattern = df.toPattern().replace('%', '‰');
                df = new DecimalFormat(pattern, sym);
                df.setMultiplier(1);
            }
        } else {
            NumberFormat nf = NumberFormat.getNumberInstance(locale);
            df = nf instanceof DecimalFormat ? (DecimalFormat) nf
                    : new DecimalFormat("#,##0.###", DecimalFormatSymbols.getInstance(locale));
        }
        df.setGroupingUsed(s.grouping);
        df.setRoundingMode(s.rounding);
        df.setMinimumIntegerDigits(s.minInteger);
        df.setMaximumIntegerDigits(Integer.MAX_VALUE);

        String number;
        BigDecimal bd = decimal(value);
        if (bd == null) {
            number = df.format(((Number) value).doubleValue());  // NaN, ∞ in the locale's symbols
        } else {
            if (s.minSignificant > 0) {
                bd = bd.round(new MathContext(s.maxSignificant, s.rounding));
                BigDecimal stripped = bd.signum() == 0 ? BigDecimal.ZERO : bd.stripTrailingZeros();
                int integerDigits = stripped.precision() - stripped.scale();
                int scale = Math.max(Math.max(stripped.scale(), 0), s.minSignificant - integerDigits);
                bd = stripped.setScale(Math.max(scale, 0), s.rounding);
                df.setMinimumFractionDigits(bd.scale());
                df.setMaximumFractionDigits(bd.scale());
            } else {
                df.setMinimumFractionDigits(s.minFraction);
                df.setMaximumFractionDigits(s.maxFraction);
            }
            number = df.format(bd);
        }
        if (s.signAlways && (bd == null ? ((Number) value).doubleValue() >= 0 : bd.signum() >= 0)
                && !number.startsWith(String.valueOf(df.getDecimalFormatSymbols().getMinusSign()))) {
            number = "+" + number;
        }
        if (unit == null || percent || "hidden".equals(s.width)) return number;
        String pattern = ROOT_UNITS.get(unit);
        if (pattern == null) {
            int dash = unit.indexOf('-');
            pattern = "{0} " + (dash >= 0 ? unit.substring(dash + 1) : unit);
        }
        return pattern.replace("{0}", number);
    }

    // ---- LocaleData --------------------------------------------------------------------

    /** The region ICU would key supplemental data by: rg keyword, the region subtag, else none. */
    private static String region(String icuLocaleId) {
        Locale l = locale(icuLocaleId);
        String rg = l.getUnicodeLocaleType("rg");
        if (rg != null && rg.length() >= 2) return rg.substring(0, 2).toUpperCase(Locale.ROOT);
        if (!l.getCountry().isEmpty()) return l.getCountry();
        // No likely-subtags data in the JDK: the user's own region when the
        // language is theirs ("en" on an American's machine is en_US), else none.
        Locale user = Locale.getDefault(Locale.Category.FORMAT);
        if (!l.getLanguage().isEmpty() && l.getLanguage().equals(user.getLanguage())) return user.getCountry();
        return "";
    }

    // CLDR supplementalData.xml <measurementData> and <paperSize>; "001" (metric, A4) elsewhere.
    private static final String US_MEASUREMENT = " LR MM US ";
    private static final String UK_MEASUREMENT = " GB ";
    private static final String LETTER_PAPER = " BZ CA CL CO CR GT MX NI PA PH PR SV US VE ";

    /** ICU4C UMeasurementSystem: 0 SI, 1 US, 2 UK. */
    static int measurementSystem(String icuLocaleId) {
        String r = " " + region(icuLocaleId) + " ";
        if (US_MEASUREMENT.contains(r)) return 1;
        if (UK_MEASUREMENT.contains(r)) return 2;
        return 0;
    }

    /** {height, width} in millimetres: US Letter or A4. */
    static int[] paperSize(String icuLocaleId) {
        String r = " " + region(icuLocaleId) + " ";
        return LETTER_PAPER.contains(r) ? new int[] {279, 216} : new int[] {297, 210};
    }

    /** The CLDR release of the JDK's own locale data (JEP 252; JDK 21 ships CLDR 42). */
    static int[] cldrVersion() {
        int feature = Runtime.version().feature();
        int cldr;
        if (feature <= 21) cldr = 42;
        else if (feature == 22) cldr = 44;
        else if (feature == 23) cldr = 45;
        else if (feature == 24) cldr = 46;
        else cldr = 47;
        return new int[] {cldr, 0, 0, 0};
    }

    // CLDR root's quotation marks: “ ” ‘ ’.
    private static final String[] ROOT_DELIMITERS = {"“", "”", "‘", "’"};

    /** Root's delimiter, or null under noSubstitute (the locale has none of its own here). */
    static String delimiter(String icuLocaleId, boolean noSubstitute, int type) {
        if (noSubstitute || type < 0 || type >= ROOT_DELIMITERS.length) return null;
        return ROOT_DELIMITERS[type];
    }

    /** Root has no exemplar set: null, which LocaleData turns into an empty set (or null under noSubstitute). */
    static Object[] exemplarSet() {
        return null;
    }

    static String localeDisplayPattern() {
        return "{0} ({1})";
    }

    static String localeSeparator() {
        return ", ";
    }
}
