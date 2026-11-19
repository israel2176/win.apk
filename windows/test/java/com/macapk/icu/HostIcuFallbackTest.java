// HostIcuFallbackTest -- android.icu with no host ICU, on a plain JVM.
//
// A plain JVM registers none of com.macapk.icu.HostIcu's natives, which is
// what a run without icu.dll comes to (Windows before 10 1903, Wine): every
// HostIcu call must answer from IcuFallback instead of throwing. Bank Leumi's
// Hermes Intl.NumberFormat {style: "unit"} died on the throw. Checks what
// MeasureFormat (as Hermes calls it), LocaleData and the java.text-backed
// android.icu classes give.
//
//   javac -cp <android-framework.jar> -d out windows/test/java/com/macapk/icu/HostIcuFallbackTest.java
//   java -cp <android-framework.jar>:out com.macapk.icu.HostIcuFallbackTest
package com.macapk.icu;

import android.icu.text.DecimalFormat;
import android.icu.text.MeasureFormat;
import android.icu.text.NumberFormat;
import android.icu.util.LocaleData;
import android.icu.util.Measure;
import android.icu.util.MeasureUnit;
import android.icu.util.ULocale;

import java.math.BigDecimal;
import java.util.Locale;

public final class HostIcuFallbackTest {
    private static int failures;

    private static void check(String what, boolean ok, Object detail) {
        System.out.println((ok ? "  [PASS] " : "  [FAIL] ") + what + (detail != null ? ": " + detail : ""));
        if (!ok) failures++;
    }

    private static void eq(String what, String want, String got) {
        check(what, want.equals(got), "\"" + got + "\"" + (want.equals(got) ? "" : " (want \"" + want + "\")"));
    }

    private static String fmt(String locale, MeasureFormat.FormatWidth width, NumberFormat nf, Number n,
            MeasureUnit unit) {
        // As Hermes does: java.text.Format.format(Object) on a Measure.
        return MeasureFormat.getInstance(new ULocale(locale), width, nf).format(new Measure(n, unit));
    }

    private static NumberFormat nf(String locale, int minF, int maxF) {
        NumberFormat f = NumberFormat.getInstance(new ULocale(locale));
        f.setMinimumFractionDigits(minF);
        f.setMaximumFractionDigits(maxF);
        return f;
    }

    public static void main(String[] args) {
        Locale.setDefault(Locale.US);
        final String nbsp = " ";
        MeasureFormat.FormatWidth SHORT = MeasureFormat.FormatWidth.SHORT;

        System.out.println("MeasureFormat with no host ICU");
        eq("en kilometres, short", "5.5 km", fmt("en_US", SHORT, nf("en_US", 0, 2), 5.5, MeasureUnit.KILOMETER));
        eq("en, grouping and the fraction window", "1,234.50 km",
                fmt("en_US", SHORT, nf("en_US", 2, 2), 1234.5, MeasureUnit.KILOMETER));
        eq("de: the locale's separators", "1.234,5 km",
                fmt("de_DE", SHORT, nf("de_DE", 0, 3), 1234.5, MeasureUnit.KILOMETER));
        eq("he (Leumi's locale), hours", "3 h", fmt("he_IL", SHORT, nf("he_IL", 0, 0), 3, MeasureUnit.HOUR));
        eq("wide width: root's pattern", "12 kg",
                fmt("en_US", MeasureFormat.FormatWidth.WIDE, nf("en_US", 0, 0), 12L, MeasureUnit.KILOGRAM));
        eq("narrow, Celsius", "-4°C",
                fmt("en_US", MeasureFormat.FormatWidth.NARROW, nf("en_US", 0, 0), -4, MeasureUnit.CELSIUS));
        eq("BigDecimal through formatDecimal's path", "0.125 L",
                fmt("en_US", SHORT, nf("en_US", 0, 3), new BigDecimal("0.125"), MeasureUnit.LITER));
        eq("ar with Latin digits by keyword", "7 MB",
                fmt("ar_EG@numbers=latn", SHORT, nf("en_US", 0, 0), 7, MeasureUnit.MEGABYTE));
        eq("percent: the locale's own pattern, no x100", "12%",
                fmt("en_US", SHORT, nf("en_US", 0, 0), 12, MeasureUnit.PERCENT));
        eq("percent in fr: the locale's spacing", "12" + " " + "%",
                fmt("fr_FR", SHORT, nf("fr_FR", 0, 0), 12, MeasureUnit.PERCENT).replace(nbsp, " "));
        {
            DecimalFormat d = (DecimalFormat) NumberFormat.getInstance(new ULocale("en_US"));
            d.setSignificantDigitsUsed(true);
            d.setMinimumSignificantDigits(1);
            d.setMaximumSignificantDigits(3);
            eq("significant digits (@##)", "1,230 m", fmt("en_US", SHORT, d, 1234.567, MeasureUnit.METER));
            d.setMinimumSignificantDigits(2);
            d.setMaximumSignificantDigits(2);
            eq("significant digits (@@), small", "0.00012 s", fmt("en_US", SHORT, d, 0.000123, MeasureUnit.SECOND));
            eq("significant digits (@@), zero", "0.0 s", fmt("en_US", SHORT, d, 0.0, MeasureUnit.SECOND));
            d.setSignificantDigitsUsed(false);
            d.setMaximumFractionDigits(0);
            d.setSignAlwaysShown(true);
            d.setGroupingUsed(false);
            d.setMinimumIntegerDigits(3);
            eq("sign-always, group-off, integer-width", "+1234 ms",
                    fmt("en_US", SHORT, d, 1234, MeasureUnit.MILLISECOND));
            eq("sign-always, minimum integer digits", "+007 ms", fmt("en_US", SHORT, d, 7, MeasureUnit.MILLISECOND));
        }
        {
            NumberFormat down = nf("en_US", 0, 1);
            down.setRoundingMode(BigDecimal.ROUND_DOWN);
            eq("rounding mode (down)", "2.9 mi", fmt("en_US", SHORT, down, 2.99, MeasureUnit.MILE));
        }
        eq("several measures, joined", "1 h, 5 min",
                MeasureFormat.getInstance(new ULocale("en_US"), SHORT)
                        .formatMeasures(new Measure(1, MeasureUnit.HOUR), new Measure(5, MeasureUnit.MINUTE)));
        eq("the skeleton reader on its own: unknown unit", "3 furlong",
                HostIcu.formatSkeleton("measure-unit/length-furlong unit-width-short precision-integer", "en_US", 3));
        eq("unit-width-hidden: the number only", "3.5",
                HostIcu.formatSkeleton("measure-unit/length-meter unit-width-hidden .0", "en_US", 3.5));

        System.out.println("LocaleData with no host ICU");
        check("measurement system, en_US: US",
                LocaleData.getMeasurementSystem(new ULocale("en_US")) == LocaleData.MeasurementSystem.US, null);
        check("measurement system, en_GB: UK",
                LocaleData.getMeasurementSystem(new ULocale("en_GB")) == LocaleData.MeasurementSystem.UK, null);
        check("measurement system, he_IL: SI",
                LocaleData.getMeasurementSystem(new ULocale("he_IL")) == LocaleData.MeasurementSystem.SI, null);
        check("measurement system, en (the user's region, US)",
                LocaleData.getMeasurementSystem(new ULocale("en")) == LocaleData.MeasurementSystem.US, null);
        check("measurement system, de_DE@rg=uszzzz: US",
                LocaleData.getMeasurementSystem(new ULocale("de_DE@rg=uszzzz")) == LocaleData.MeasurementSystem.US,
                null);
        LocaleData.PaperSize letter = LocaleData.getPaperSize(new ULocale("en_CA"));
        check("paper size, en_CA: Letter", letter.getHeight() == 279 && letter.getWidth() == 216,
                letter.getHeight() + "x" + letter.getWidth());
        LocaleData.PaperSize a4 = LocaleData.getPaperSize(new ULocale("fr_FR"));
        check("paper size, fr_FR: A4", a4.getHeight() == 297 && a4.getWidth() == 210,
                a4.getHeight() + "x" + a4.getWidth());
        LocaleData ld = LocaleData.getInstance(new ULocale("he_IL"));
        eq("quotation start (root)", "“", ld.getDelimiter(LocaleData.QUOTATION_START));
        eq("alternate quotation end (root)", "’", ld.getDelimiter(LocaleData.ALT_QUOTATION_END));
        eq("locale display pattern (root)", "{0} ({1})", ld.getLocaleDisplayPattern());
        eq("locale separator (root)", ", ", ld.getLocaleSeparator());
        check("exemplar set: empty (root has none)", ld.getExemplarSet(0, LocaleData.ES_STANDARD).isEmpty(), null);
        ld.setNoSubstitute(true);
        check("exemplar set under noSubstitute: null", ld.getExemplarSet(0, LocaleData.ES_STANDARD) == null, null);
        check("delimiter under noSubstitute: null", ld.getDelimiter(LocaleData.QUOTATION_START) == null, null);
        check("CLDR version: the JDK's", LocaleData.getCLDRVersion().getMajor() >= 42,
                LocaleData.getCLDRVersion());

        System.out.println("the java.text-backed classes");
        eq("NumberFormat, he_IL", "1,234.5", NumberFormat.getInstance(new ULocale("he_IL")).format(1234.5));

        System.out.println(failures == 0 ? "ALL PASSED" : "FAILED: " + failures + " failure(s)");
        if (failures != 0) System.exit(1);
    }
}
