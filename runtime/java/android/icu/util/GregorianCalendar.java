package android.icu.util;

import java.util.Locale;

/**
 * ICU's GregorianCalendar: what Calendar.getInstance answers for a Gregorian
 * locale on Android (an app's getClass() or instanceof sees this class), and a
 * public class apps construct. Backed by java.util.GregorianCalendar, whose
 * rules (the Julian/Gregorian cutover of 1582, leap years) are ICU's too.
 *
 * Source: https://developer.android.com/reference/android/icu/util/GregorianCalendar
 */
public class GregorianCalendar extends Calendar {

    private static final long serialVersionUID = 1L;

    public static final int BC = 0;
    public static final int AD = 1;

    GregorianCalendar(java.util.Calendar backing, Locale locale) {
        super(backing, locale);
    }

    public GregorianCalendar() {
        this(TimeZone.getDefault(), Locale.getDefault());
    }
    public GregorianCalendar(TimeZone zone) {
        this(zone, Locale.getDefault());
    }
    public GregorianCalendar(Locale aLocale) {
        this(TimeZone.getDefault(), aLocale);
    }
    public GregorianCalendar(ULocale locale) {
        this(TimeZone.getDefault(), locale != null ? locale.toLocale() : Locale.getDefault());
    }
    public GregorianCalendar(TimeZone zone, ULocale locale) {
        this(zone, locale != null ? locale.toLocale() : Locale.getDefault());
    }
    public GregorianCalendar(TimeZone zone, Locale aLocale) {
        this(gregorian(zone, aLocale), aLocale != null ? aLocale : Locale.getDefault());
    }
    public GregorianCalendar(int year, int month, int date) {
        this();
        clear();
        set(year, month, date);
    }
    public GregorianCalendar(int year, int month, int date, int hour, int minute) {
        this();
        clear();
        set(year, month, date, hour, minute);
    }
    public GregorianCalendar(int year, int month, int date, int hour, int minute, int second) {
        this();
        clear();
        set(year, month, date, hour, minute, second);
    }

    /** A Gregorian calendar whatever the locale's own system is (th_TH's is
     *  Buddhist on the JDK), as ICU's constructor gives. */
    private static java.util.Calendar gregorian(TimeZone zone, Locale locale) {
        Locale loc = locale != null ? locale : Locale.getDefault();
        java.util.TimeZone tz = zone != null ? (java.util.TimeZone) zone.unwrap().clone()
                                             : java.util.TimeZone.getDefault();
        return new java.util.Calendar.Builder()
                .setCalendarType("gregory")
                .setLocale(loc)
                .setTimeZone(tz)
                .setInstant(System.currentTimeMillis())
                .build();
    }

    private java.util.GregorianCalendar jdk() {
        return (java.util.GregorianCalendar) toJavaCalendar();
    }

    public void setGregorianChange(java.util.Date date) { jdk().setGregorianChange(date); }
    public final java.util.Date getGregorianChange() { return jdk().getGregorianChange(); }
    public boolean isLeapYear(int year) { return jdk().isLeapYear(year); }

    @Override public boolean isEquivalentTo(Calendar other) {
        return super.isEquivalentTo(other)
                && getGregorianChange().equals(((GregorianCalendar) other).getGregorianChange());
    }

    @Override public int hashCode() {
        return super.hashCode() ^ (int) getGregorianChange().getTime();
    }

    @Override public String getType() { return "gregorian"; }
}
