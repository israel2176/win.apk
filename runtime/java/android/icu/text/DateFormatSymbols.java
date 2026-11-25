// android.icu.text.DateFormatSymbols -- locale date/time symbol set (month
// names, weekday names, am/pm, era) for SimpleDateFormat. ICU4J:
//   developer.android.com/reference/android/icu/text/DateFormatSymbols
//   public class DateFormatSymbols implements Serializable, Cloneable
//
// Faithful delegate over java.text.DateFormatSymbols, which carries the same
// localized name arrays.
package android.icu.text;

import android.icu.util.ULocale;

import java.util.Locale;

public class DateFormatSymbols implements java.io.Serializable, Cloneable {
    private static final long serialVersionUID = 1L;

    private final java.text.DateFormatSymbols d;
    /** The locale of the names the width forms below give (java.text keeps none of its own). */
    private final Locale locale;

    public DateFormatSymbols() {
        this(Locale.getDefault());
    }
    public DateFormatSymbols(Locale locale) {
        this.locale = (locale != null) ? locale : Locale.getDefault();
        this.d = new java.text.DateFormatSymbols(this.locale);
    }
    public DateFormatSymbols(ULocale locale) {
        this(ULocale.unwrap(locale));
    }
    DateFormatSymbols(java.text.DateFormatSymbols delegate, Locale locale) {
        this.d = delegate;
        this.locale = locale;
    }

    public static DateFormatSymbols getInstance() {
        return getInstance(Locale.getDefault());
    }
    public static DateFormatSymbols getInstance(Locale locale) {
        Locale l = (locale != null) ? locale : Locale.getDefault();
        return new DateFormatSymbols(java.text.DateFormatSymbols.getInstance(l), l);
    }
    public static DateFormatSymbols getInstance(ULocale locale) {
        return getInstance(ULocale.unwrap(locale));
    }

    public String[] getEras() { return d.getEras(); }
    public void setEras(String[] newEras) { d.setEras(newEras); }
    public String[] getMonths() { return d.getMonths(); }
    public void setMonths(String[] newMonths) { d.setMonths(newMonths); }
    public String[] getShortMonths() { return d.getShortMonths(); }
    public void setShortMonths(String[] newShortMonths) { d.setShortMonths(newShortMonths); }
    public String[] getWeekdays() { return d.getWeekdays(); }
    public void setWeekdays(String[] newWeekdays) { d.setWeekdays(newWeekdays); }
    public String[] getShortWeekdays() { return d.getShortWeekdays(); }
    public void setShortWeekdays(String[] newShortWeekdays) { d.setShortWeekdays(newShortWeekdays); }
    public String[] getAmPmStrings() { return d.getAmPmStrings(); }
    public void setAmPmStrings(String[] newAmpms) { d.setAmPmStrings(newAmpms); }

    // ICU4J's contexts and widths (DateFormatSymbols.FORMAT ... SHORT).
    public static final int FORMAT = 0;
    public static final int STANDALONE = 1;
    public static final int ABBREVIATED = 0;
    public static final int WIDE = 1;
    public static final int NARROW = 2;
    public static final int SHORT = 3;

    private static java.time.format.TextStyle style(int context, int width) {
        boolean alone = context == STANDALONE;
        switch (width) {
            case WIDE: return alone ? java.time.format.TextStyle.FULL_STANDALONE : java.time.format.TextStyle.FULL;
            case NARROW: return alone ? java.time.format.TextStyle.NARROW_STANDALONE : java.time.format.TextStyle.NARROW;
            default: return alone ? java.time.format.TextStyle.SHORT_STANDALONE : java.time.format.TextStyle.SHORT;
        }
    }

    /** ICU4J's getWeekdays(context, width): index 1 (Sunday) to 7, index 0 empty, as getWeekdays().
     *  The JDK's CLDR names of that width (SHORT, ICU's two-letter form, as ABBREVIATED). AOSP's
     *  SimpleMonthView (the calendar of DatePicker and CalendarView) heads its columns with them. */
    public String[] getWeekdays(int context, int width) {
        String[] out = new String[8];
        out[0] = "";
        for (int i = 1; i <= 7; i++) {
            java.time.DayOfWeek day = java.time.DayOfWeek.SUNDAY.plus(i - 1);
            out[i] = day.getDisplayName(style(context, width), locale);
        }
        return out;
    }

    /** ICU4J's getMonths(context, width): January first. */
    public String[] getMonths(int context, int width) {
        String[] out = new String[12];
        for (int i = 0; i < 12; i++) out[i] = java.time.Month.of(i + 1).getDisplayName(style(context, width), locale);
        return out;
    }

    /** ICU4J's narrow AM and PM ("a", "p" in English): TimePicker's AM/PM labels. */
    public String[] getAmpmNarrowStrings() {
        java.time.format.DateTimeFormatter f = new java.time.format.DateTimeFormatterBuilder()
                .appendText(java.time.temporal.ChronoField.AMPM_OF_DAY, java.time.format.TextStyle.NARROW)
                .toFormatter(locale);
        return new String[] {f.format(java.time.LocalTime.of(9, 0)), f.format(java.time.LocalTime.of(21, 0))};
    }

    java.text.DateFormatSymbols unwrap() { return d; }

    @Override public Object clone() {
        return new DateFormatSymbols((java.text.DateFormatSymbols) d.clone(), locale);
    }
    @Override public boolean equals(Object o) {
        return (o instanceof DateFormatSymbols) && ((DateFormatSymbols) o).d.equals(d);
    }
    @Override public int hashCode() { return d.hashCode(); }
}
