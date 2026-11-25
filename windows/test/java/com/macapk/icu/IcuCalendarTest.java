package com.macapk.icu;

import android.icu.util.Calendar;
import android.icu.util.GregorianCalendar;
import android.icu.util.TimeZone;
import android.icu.util.ULocale;

import java.util.Date;

/**
 * android.icu.util.Calendar against what Android 14's own ICU answers (each
 * expected value was read from android-all 14's android.icu on a JVM): the week
 * data of a region, the weekend, the ICU-only fields 17-22 and their limits,
 * set/clear/isSet, add/roll, fieldDifference, equality and the class
 * getInstance answers. Gmail's message list called getWeekData(), which the
 * class lacked (NoSuchMethodError).
 *
 * Run by hand against the built jars:
 *   javac -cp android-framework.jar -d out IcuCalendarTest.java
 *   java -cp android-framework.jar:out com.macapk.icu.IcuCalendarTest
 * Exit code: the number of failed checks.
 */
public final class IcuCalendarTest {

    private static int failed;

    private static void check(String what, Object got, Object want) {
        if (String.valueOf(got).equals(String.valueOf(want))) return;
        failed++;
        System.err.println("FAIL " + what + ": got " + got + " want " + want);
    }

    public static void main(String[] args) {
        check("week 001", Calendar.getWeekDataForRegion("001"), "{2, 1, 7, 0, 1, 86400000}");
        check("week IL", Calendar.getWeekDataForRegion("IL"), "{1, 1, 6, 0, 7, 86400000}");
        check("week US", Calendar.getWeekDataForRegion("US"), "{1, 1, 7, 0, 1, 86400000}");
        check("week DE", Calendar.getWeekDataForRegion("DE"), "{2, 4, 7, 0, 1, 86400000}");
        check("week IR", Calendar.getWeekDataForRegion("IR"), "{7, 1, 6, 0, 6, 86400000}");
        check("week AF", Calendar.getWeekDataForRegion("AF"), "{7, 1, 5, 0, 6, 86400000}");
        check("week IN", Calendar.getWeekDataForRegion("IN"), "{1, 1, 1, 0, 1, 86400000}");

        Calendar c = Calendar.getInstance(TimeZone.getTimeZone("Asia/Jerusalem"), new ULocale("he_IL"));
        check("class", c.getClass().getName(), "android.icu.util.GregorianCalendar");
        check("instanceof", c instanceof GregorianCalendar, true);
        check("type", c.getType(), "gregorian");
        check("he_IL week", c.getWeekData(), "{1, 1, 6, 0, 7, 86400000}");
        check("field count", c.getFieldCount(), 23);
        int[][] limits = {
            { -5838270, -5838270, 5828964, 5838271 },
            { 1, 1, 7, 7 },
            { -5838269, -5838269, 5828963, 5838270 },
            { -2130706432, -2130706432, 2130706432, 2130706432 },
            { 0, 0, 86399999, 86399999 },
            { 0, 0, 1, 1 },
        };
        for (int f = 17; f <= 22; f++) {
            int[] l = limits[f - 17];
            check("limits " + f, c.getMinimum(f) + " " + c.getGreatestMinimum(f) + " " + c.getLeastMaximum(f)
                    + " " + c.getMaximum(f), l[0] + " " + l[1] + " " + l[2] + " " + l[3]);
        }
        c.clear();
        c.set(2026, 10, 23, 9, 24, 5);
        check("time", c.getTimeInMillis(), 1795418645000L);
        StringBuilder fields = new StringBuilder();
        for (int f = 0; f <= 22; f++) fields.append(c.get(f)).append(' ');
        check("fields", fields.toString().trim(),
              "1 2026 10 48 4 23 327 2 4 0 9 9 24 5 0 7200000 0 2026 2 2026 2461368 33845000 0");
        check("isSet after get", c.isSet(Calendar.DATE), true);
        check("Monday weekend", c.isWeekend(), false);
        c.set(Calendar.DAY_OF_WEEK, Calendar.SATURDAY);
        check("Saturday weekend", c.isWeekend(), true);
        check("Saturday time", c.getTimeInMillis(), 1795850645000L);
        c.clear(Calendar.HOUR_OF_DAY);
        check("isSet cleared", c.isSet(Calendar.HOUR_OF_DAY), false);
        check("display name", c.getDisplayName(java.util.Locale.ROOT), "android.icu.util.GregorianCalendar");

        Calendar b = Calendar.getInstance(new ULocale("en_US"));
        b.clear();
        b.set(2026, 0, 31);
        b.add(Calendar.MONTH, 1);
        check("add month", b.get(Calendar.DATE) + " " + b.get(Calendar.MONTH), "28 1");
        b.roll(Calendar.DATE, 5);
        check("roll date", b.get(Calendar.DATE) + " " + b.get(Calendar.MONTH), "5 1");
        b.setTimeZone(TimeZone.getTimeZone("UTC"));
        b.clear();
        b.set(2020, 1, 1);
        Calendar b2 = (Calendar) b.clone();
        check("clone", b.equals(b2) + " " + b.isEquivalentTo(b2) + " " + b.compareTo(b2), "true true 0");
        Date target = new Date(b.getTimeInMillis() + 400L * 86400000L + 7200000L);
        check("fieldDifference", b.fieldDifference(target, Calendar.YEAR) + " "
                + b.fieldDifference(target, Calendar.MONTH) + " " + b.fieldDifference(target, Calendar.DATE) + " "
                + b.fieldDifference(target, Calendar.HOUR_OF_DAY), "1 1 6 2");
        check("wall time", b.isLenient() + " " + b.getRepeatedWallTimeOption() + " "
                + b.getSkippedWallTimeOption(), "true 0 0");

        if (failed == 0) System.err.println("IcuCalendarTest: all checks passed");
        System.exit(failed);
    }
}
