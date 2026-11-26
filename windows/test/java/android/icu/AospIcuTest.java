// AospIcuTest -- AOSP's ICU4J (android.icu, in aosp-graphics.jar) on the ICU 72 data Win.apk ships
// (bin/aosp/icu/icudt72l.dat, named by -Dicu.data.path as the host names it), on a plain JVM: the parts
// apps use that Win.apk's own android.icu did not have or answered from the JDK.
//
//   java -Dicu.data.path=<stage>/bin/aosp/icu/icudt72l.dat -cp <aosp-graphics.jar>:... android.icu.AospIcuTest
package android.icu;

import android.icu.text.Bidi;
import android.icu.text.Collator;
import android.icu.text.DateFormat;
import android.icu.text.MeasureFormat;
import android.icu.text.NumberFormat;
import android.icu.text.PluralRules;
import android.icu.text.RelativeDateTimeFormatter;
import android.icu.text.Transliterator;
import android.icu.util.Calendar;
import android.icu.util.HebrewCalendar;
import android.icu.util.IslamicCalendar;
import android.icu.util.Measure;
import android.icu.util.MeasureUnit;
import android.icu.util.TimeZone;
import android.icu.util.ULocale;

public class AospIcuTest {
    static int failures;

    static void check(String what, Object got, Object want) {
        boolean ok = want.equals(got);
        System.out.println((ok ? "  ok   " : "  FAIL ") + what + ": " + got + (ok ? "" : " (want " + want + ")"));
        if (!ok) failures++;
    }

    interface Value { Object get() throws Exception; }

    /** A check whose value may throw (data ICU cannot find): the exception is the failure. */
    static void check(String what, Value got, Object want) {
        try {
            check(what, got.get(), want);
        } catch (Throwable t) {
            System.out.println("  FAIL " + what + ": " + t);
            failures++;
        }
    }

    public static void main(String[] args) {
        ULocale he = new ULocale("he_IL");
        TimeZone utc = TimeZone.getTimeZone("UTC");

        // 2026-11-26 is 16 Kislev 5787.
        HebrewCalendar h = new HebrewCalendar(utc, he);
        h.clear();
        Calendar g = Calendar.getInstance(utc, ULocale.ROOT);
        g.clear();
        g.set(2026, Calendar.NOVEMBER, 26);
        h.setTimeInMillis(g.getTimeInMillis());
        check("Hebrew year", h.get(Calendar.YEAR), 5787);
        check("Hebrew month is Kislev", h.get(Calendar.MONTH), HebrewCalendar.KISLEV);
        check("Hebrew day", h.get(Calendar.DATE), 16);
        check("a Hebrew locale's calendar keyword", Calendar.getInstance(new ULocale("he_IL@calendar=hebrew")).getType(),
                "hebrew");
        check("a Hebrew date in Hebrew", () -> {
            DateFormat hf = DateFormat.getDateInstance(h, DateFormat.LONG, he);
            hf.setTimeZone(utc);
            return hf.format(h.getTime());
        }, "ט״ז בכסלו תשפ״ז");

        IslamicCalendar i = new IslamicCalendar(utc, ULocale.ROOT);
        i.setTimeInMillis(g.getTimeInMillis());
        check("Islamic year", i.get(Calendar.YEAR) >= 1447 && i.get(Calendar.YEAR) <= 1448, true);

        check("Hebrew plural 'two' (a dual)", PluralRules.forLocale(he).select(2), "two");
        check("a number in Hebrew", NumberFormat.getInstance(he).format(1234567.5), "1,234,567.5");
        check("a measure", () -> MeasureFormat.getInstance(ULocale.ENGLISH, MeasureFormat.FormatWidth.WIDE)
                .format(new Measure(5, MeasureUnit.KILOMETER)), "5 kilometers");
        check("relative time", () -> RelativeDateTimeFormatter.getInstance(ULocale.ENGLISH)
                .format(1, RelativeDateTimeFormatter.Direction.NEXT, RelativeDateTimeFormatter.RelativeUnit.DAYS),
                "in 1 day");
        check("Hebrew collation", () -> Collator.getInstance(he).compare("אב", "בא") < 0, true);
        check("a time zone's name", () -> TimeZone.getTimeZone("Asia/Jerusalem").getDisplayName(false, TimeZone.LONG,
                ULocale.ENGLISH), "Israel Standard Time");
        check("transliteration", () -> Transliterator.getInstance("Hebrew-Latin").transliterate("שלום").isEmpty(),
                false);

        check("Bidi: Hebrew is right to left", () -> new Bidi("abc שלום", Bidi.DIRECTION_DEFAULT_LEFT_TO_RIGHT)
                .getLevelAt(5) % 2, 1);
        check("Bidi: Latin is left to right", () -> new Bidi("abc שלום", Bidi.DIRECTION_DEFAULT_LEFT_TO_RIGHT)
                .getLevelAt(0) % 2, 0);

        System.out.println(failures == 0 ? "ALL PASSED" : failures + " FAILED");
        System.exit(failures == 0 ? 0 : 1);
    }
}
