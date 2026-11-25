package android.icu.util;

import java.util.Locale;

/**
 * ICU's Calendar. Android exposes ICU4J publicly; apps call
 * Calendar.getInstance(locale).getType() to learn the calendar system
 * ("gregorian", "japanese", ...) for the locale, and getWeekData() / isWeekend()
 * for the region's week (Gmail's message list reads getWeekData()).
 *
 * Backed by java.util.Calendar (which shares CLDR-derived locale data on
 * OpenJDK), so getInstance / field access match for the ordinary Gregorian
 * cases these apps use. getType returns the ICU calendar-keyword string
 * derived from the JDK calendar class. getInstance answers a GregorianCalendar
 * for a Gregorian locale, as ICU does. The fields ICU adds to the JDK's (17-22:
 * YEAR_WOY, DOW_LOCAL, EXTENDED_YEAR, JULIAN_DAY, MILLISECONDS_IN_DAY,
 * IS_LEAP_MONTH) are computed here from the JDK's ones, with ICU's limits.
 * The weekend comes from CLDR's weekData (ICU's supplementalData), the table
 * below; the JDK has no weekend data.
 *
 * Source: https://developer.android.com/reference/android/icu/util/Calendar,
 * ICU4J com/ibm/icu/util/Calendar.java (Android 14's external/icu).
 */
public class Calendar implements java.io.Serializable, Cloneable, Comparable<Calendar> {

    private static final long serialVersionUID = 1L;

    public static final int ERA = java.util.Calendar.ERA;
    public static final int YEAR = java.util.Calendar.YEAR;
    public static final int MONTH = java.util.Calendar.MONTH;
    public static final int DATE = java.util.Calendar.DATE;
    public static final int DAY_OF_MONTH = java.util.Calendar.DAY_OF_MONTH;
    public static final int DAY_OF_WEEK = java.util.Calendar.DAY_OF_WEEK;
    public static final int HOUR = java.util.Calendar.HOUR;
    public static final int HOUR_OF_DAY = java.util.Calendar.HOUR_OF_DAY;
    public static final int MINUTE = java.util.Calendar.MINUTE;
    public static final int SECOND = java.util.Calendar.SECOND;
    public static final int MILLISECOND = java.util.Calendar.MILLISECOND;
    // The rest of ICU's field numbering (android.icu.util.Calendar). 0-16 equal
    // java.util.Calendar's; 17-22 are ICU-only fields.
    public static final int WEEK_OF_YEAR = java.util.Calendar.WEEK_OF_YEAR;
    public static final int WEEK_OF_MONTH = java.util.Calendar.WEEK_OF_MONTH;
    public static final int DAY_OF_YEAR = java.util.Calendar.DAY_OF_YEAR;
    public static final int DAY_OF_WEEK_IN_MONTH = java.util.Calendar.DAY_OF_WEEK_IN_MONTH;
    public static final int AM_PM = java.util.Calendar.AM_PM;
    public static final int ZONE_OFFSET = java.util.Calendar.ZONE_OFFSET;
    public static final int DST_OFFSET = java.util.Calendar.DST_OFFSET;
    public static final int YEAR_WOY = 17;
    public static final int DOW_LOCAL = 18;
    public static final int EXTENDED_YEAR = 19;
    public static final int JULIAN_DAY = 20;
    public static final int MILLISECONDS_IN_DAY = 21;
    public static final int IS_LEAP_MONTH = 22;

    public static final int SUNDAY = 1;
    public static final int MONDAY = 2;
    public static final int TUESDAY = 3;
    public static final int WEDNESDAY = 4;
    public static final int THURSDAY = 5;
    public static final int FRIDAY = 6;
    public static final int SATURDAY = 7;

    public static final int JANUARY = 0;
    public static final int FEBRUARY = 1;
    public static final int MARCH = 2;
    public static final int APRIL = 3;
    public static final int MAY = 4;
    public static final int JUNE = 5;
    public static final int JULY = 6;
    public static final int AUGUST = 7;
    public static final int SEPTEMBER = 8;
    public static final int OCTOBER = 9;
    public static final int NOVEMBER = 10;
    public static final int DECEMBER = 11;
    public static final int UNDECIMBER = 12;

    public static final int AM = 0;
    public static final int PM = 1;

    /** Deprecated ICU day-of-week types (getDayOfWeekType), still in the API. */
    public static final int WEEKDAY = 0;
    public static final int WEEKEND = 1;
    public static final int WEEKEND_ONSET = 2;
    public static final int WEEKEND_CEASE = 3;

    public static final int WALLTIME_LAST = 0;
    public static final int WALLTIME_FIRST = 1;
    public static final int WALLTIME_NEXT_VALID = 2;

    protected static final int ONE_SECOND = 1000;
    protected static final int ONE_MINUTE = 60 * ONE_SECOND;
    protected static final int ONE_HOUR = 60 * ONE_MINUTE;
    protected static final long ONE_DAY = 24 * ONE_HOUR;
    protected static final long ONE_WEEK = 7 * ONE_DAY;
    protected static final int EPOCH_JULIAN_DAY = 2440588;

    private static final int FIELD_COUNT = 23;

    /** ICU's limits of the ICU-only fields (GregorianCalendar.LIMITS and
     *  Calendar.LIMITS): minimum, greatest minimum, least maximum, maximum. */
    private static final int[][] ICU_FIELD_LIMITS = {
        { -5838270, -5838270, 5828964, 5838271 },               // YEAR_WOY
        { 1, 1, 7, 7 },                                         // DOW_LOCAL
        { -5838269, -5838269, 5828963, 5838270 },               // EXTENDED_YEAR
        { -0x7F000000, -0x7F000000, 0x7F000000, 0x7F000000 },   // JULIAN_DAY
        { 0, 0, 24 * ONE_HOUR - 1, 24 * ONE_HOUR - 1 },         // MILLISECONDS_IN_DAY
        { 0, 0, 1, 1 },                                         // IS_LEAP_MONTH
    };

    private java.util.Calendar mCal;
    private int mWeekendOnset;
    private int mWeekendOnsetMillis;
    private int mWeekendCease;
    private int mWeekendCeaseMillis;
    private int mRepeatedWallTime = WALLTIME_LAST;
    private int mSkippedWallTime = WALLTIME_LAST;

    protected Calendar() {
        this(TimeZone.getDefault(), Locale.getDefault());
    }

    protected Calendar(TimeZone zone, Locale locale) {
        this(newJavaCalendar(zone, locale), locale);
    }

    protected Calendar(TimeZone zone, ULocale locale) {
        this(zone, locale != null ? locale.toLocale() : Locale.getDefault());
    }

    protected Calendar(java.util.Calendar backing) {
        this(backing, Locale.getDefault());
    }

    /** `locale` only picks the region's weekend: the JDK calendar already holds
     *  everything else the locale decides (first day, minimal days). */
    Calendar(java.util.Calendar backing, Locale locale) {
        mCal = backing;
        WeekData wd = getWeekDataForRegion(regionOf(locale));
        mWeekendOnset = wd.weekendOnset;
        mWeekendOnsetMillis = wd.weekendOnsetMillis;
        mWeekendCease = wd.weekendCease;
        mWeekendCeaseMillis = wd.weekendCeaseMillis;
    }

    static java.util.Calendar newJavaCalendar(TimeZone zone, Locale locale) {
        java.util.TimeZone tz = zone != null ? (java.util.TimeZone) zone.unwrap().clone()
                                             : java.util.TimeZone.getDefault();
        return java.util.Calendar.getInstance(tz, locale != null ? locale : Locale.getDefault());
    }

    private static Calendar create(TimeZone zone, Locale locale) {
        Locale loc = locale != null ? locale : Locale.getDefault();
        java.util.Calendar c = newJavaCalendar(zone, loc);
        return "gregory".equals(c.getCalendarType()) ? new GregorianCalendar(c, loc) : new Calendar(c, loc);
    }

    public static Calendar getInstance() {
        return create(null, Locale.getDefault());
    }
    public static Calendar getInstance(TimeZone zone) {
        return create(zone, Locale.getDefault());
    }
    public static Calendar getInstance(Locale locale) {
        return create(null, locale);
    }
    public static Calendar getInstance(ULocale locale) {
        return create(null, locale != null ? locale.toLocale() : Locale.getDefault());
    }
    public static Calendar getInstance(TimeZone zone, Locale locale) {
        return create(zone, locale);
    }
    public static Calendar getInstance(TimeZone zone, ULocale locale) {
        return create(zone, locale != null ? locale.toLocale() : Locale.getDefault());
    }

    public static Locale[] getAvailableLocales() {
        return java.util.Calendar.getAvailableLocales();
    }

    /** ICU calendar-keyword string derived from the JDK calendar class. */
    public String getType() {
        String t = mCal.getCalendarType();
        if ("japanese".equals(t)) return "japanese";
        if ("buddhist".equals(t)) return "buddhist";
        return "gregorian";
    }

    /** ICU: the calendar's zone, as an android.icu.util.TimeZone. */
    public TimeZone getTimeZone() {
        return new TimeZone(mCal.getTimeZone());
    }

    public void setTimeZone(TimeZone value) {
        if (value != null) mCal.setTimeZone((java.util.TimeZone) value.unwrap().clone());
    }

    /**
     * ICU: the "calendar" keyword values for a locale -- the region's preferred
     * calendars first (CLDR calendarPreferenceData), then, unless commonlyUsed,
     * every other calendar type, in LEGACY spelling ("gregorian"). ICU answers
     * from its own data; the calendars this layer can actually build are the
     * JDK's (java.util.Calendar.getAvailableCalendarTypes(): gregory, buddhist,
     * japanese), so the list is those, with the locale's own default (which the
     * JDK also takes from CLDR: th_TH -> buddhist) first. Advertising a calendar
     * we cannot construct would only fail later, inside a formatter.
     */
    public static final String[] getKeywordValuesForLocale(String key, ULocale locale,
                                                           boolean commonlyUsed) {
        java.util.LinkedHashSet<String> out = new java.util.LinkedHashSet<>();
        Locale base = ULocale.unwrap(locale).stripExtensions();
        out.add(ULocale.toLegacyType("ca", java.util.Calendar.getInstance(base).getCalendarType()));
        if (!commonlyUsed) {
            for (String t : java.util.Calendar.getAvailableCalendarTypes()) {
                out.add(ULocale.toLegacyType("ca", t));
            }
        }
        return out.toArray(new String[0]);
    }

    public final java.util.Date getTime() { return mCal.getTime(); }
    public final void setTime(java.util.Date date) { mCal.setTime(date); }
    public long getTimeInMillis() { return mCal.getTimeInMillis(); }
    public void setTimeInMillis(long millis) { mCal.setTimeInMillis(millis); }

    // ---- fields -------------------------------------------------------

    private static void checkField(int field) {
        if (field < 0 || field >= FIELD_COUNT) {
            throw new ArrayIndexOutOfBoundsException(field);
        }
    }

    /** The wall-clock milliseconds since the epoch: what ICU's JULIAN_DAY and
     *  MILLISECONDS_IN_DAY are computed from. */
    private long localMillis() {
        return mCal.getTimeInMillis() + mCal.get(ZONE_OFFSET) + mCal.get(DST_OFFSET);
    }

    public final int get(int field) {
        checkField(field);
        switch (field) {
            case YEAR_WOY:
                return mCal instanceof java.util.GregorianCalendar g && g.isWeekDateSupported()
                        ? g.getWeekYear() : mCal.get(YEAR);
            case DOW_LOCAL: {
                int d = mCal.get(DAY_OF_WEEK) - mCal.getFirstDayOfWeek();
                return (d < 0 ? d + 7 : d) + 1;
            }
            case EXTENDED_YEAR:
                return extendedYear();
            case JULIAN_DAY:
                return (int) (Math.floorDiv(localMillis(), ONE_DAY) + EPOCH_JULIAN_DAY);
            case MILLISECONDS_IN_DAY:
                return (int) Math.floorMod(localMillis(), ONE_DAY);
            case IS_LEAP_MONTH:
                return 0;
            default:
                return mCal.get(field);
        }
    }

    private int extendedYear() {
        if ("gregory".equals(mCal.getCalendarType()) && mCal.get(ERA) == java.util.GregorianCalendar.BC) {
            return 1 - mCal.get(YEAR);
        }
        return mCal.get(YEAR);
    }

    public final void set(int field, int value) {
        checkField(field);
        switch (field) {
            case YEAR_WOY:
                if (mCal instanceof java.util.GregorianCalendar g && g.isWeekDateSupported()) {
                    g.setWeekDate(value, g.get(WEEK_OF_YEAR), g.get(DAY_OF_WEEK));
                } else {
                    mCal.set(YEAR, value);
                }
                return;
            case DOW_LOCAL: {
                int d = (value - 1 + mCal.getFirstDayOfWeek() - 1) % 7;
                mCal.set(DAY_OF_WEEK, (d < 0 ? d + 7 : d) + 1);
                return;
            }
            case EXTENDED_YEAR:
                if (value <= 0 && "gregory".equals(mCal.getCalendarType())) {
                    mCal.set(ERA, java.util.GregorianCalendar.BC);
                    mCal.set(YEAR, 1 - value);
                } else {
                    if ("gregory".equals(mCal.getCalendarType())) mCal.set(ERA, java.util.GregorianCalendar.AD);
                    mCal.set(YEAR, value);
                }
                return;
            case JULIAN_DAY: {
                long dayDelta = (long) value - get(JULIAN_DAY);
                mCal.add(DATE, (int) dayDelta);
                return;
            }
            case MILLISECONDS_IN_DAY:
                mCal.set(HOUR_OF_DAY, value / ONE_HOUR);
                mCal.set(MINUTE, (value / ONE_MINUTE) % 60);
                mCal.set(SECOND, (value / ONE_SECOND) % 60);
                mCal.set(MILLISECOND, value % ONE_SECOND);
                return;
            case IS_LEAP_MONTH:
                return; // no leap months in the calendars built here
            default:
                mCal.set(field, value);
        }
    }

    public final void set(int year, int month, int date) {
        set(YEAR, year); set(MONTH, month); set(DATE, date);
    }
    public final void set(int year, int month, int date, int hour, int minute) {
        set(year, month, date); set(HOUR_OF_DAY, hour); set(MINUTE, minute);
    }
    public final void set(int year, int month, int date, int hour, int minute, int second) {
        set(year, month, date, hour, minute); set(SECOND, second);
    }

    public final void clear() { mCal.clear(); }

    public final void clear(int field) {
        checkField(field);
        if (field < java.util.Calendar.FIELD_COUNT) mCal.clear(field);
    }

    /** ICU-only fields are derived here, never stored, so they read as set
     *  exactly when the JDK's own fields are computed (ICU's after a get). */
    public final boolean isSet(int field) {
        checkField(field);
        if (field < java.util.Calendar.FIELD_COUNT) return mCal.isSet(field);
        return mCal.isSet(YEAR) && mCal.isSet(DAY_OF_YEAR);
    }

    public void add(int field, int amount) {
        if (amount == 0) return;
        switch (field) {
            case YEAR_WOY: case EXTENDED_YEAR: mCal.add(YEAR, amount); return;
            case DOW_LOCAL: case JULIAN_DAY: mCal.add(DATE, amount); return;
            case MILLISECONDS_IN_DAY: mCal.add(MILLISECOND, amount); return;
            case IS_LEAP_MONTH: return;
            default: checkField(field); mCal.add(field, amount);
        }
    }

    public final void roll(int field, boolean up) { roll(field, up ? +1 : -1); }

    public void roll(int field, int amount) {
        if (amount == 0) return;
        switch (field) {
            case DOW_LOCAL: mCal.roll(DAY_OF_WEEK, amount); return;
            case YEAR_WOY: case EXTENDED_YEAR: mCal.roll(YEAR, amount); return;
            case JULIAN_DAY: case MILLISECONDS_IN_DAY: add(field, amount); return;
            case IS_LEAP_MONTH: return;
            default: checkField(field); mCal.roll(field, amount);
        }
    }

    /** ICU's Calendar.fieldDifference: the whole number of `field` units from
     *  this calendar's time to `when`, found by doubling then bisecting add();
     *  the calendar is left advanced by that many units, as ICU leaves it. */
    public int fieldDifference(java.util.Date when, int field) {
        int min = 0;
        long startMs = getTimeInMillis();
        long targetMs = when.getTime();
        if (startMs < targetMs) {
            int max = 1;
            for (;;) {
                setTimeInMillis(startMs);
                add(field, max);
                long ms = getTimeInMillis();
                if (ms == targetMs) return max;
                if (ms > targetMs) break;
                if (max < Integer.MAX_VALUE) {
                    min = max;
                    max <<= 1;
                    if (max < 0) max = Integer.MAX_VALUE;
                } else {
                    throw new RuntimeException();
                }
            }
            while ((max - min) > 1) {
                int t = min + (max - min) / 2;
                setTimeInMillis(startMs);
                add(field, t);
                long ms = getTimeInMillis();
                if (ms == targetMs) return t;
                if (ms > targetMs) max = t; else min = t;
            }
        } else if (startMs > targetMs) {
            int max = -1;
            for (;;) {
                setTimeInMillis(startMs);
                add(field, max);
                long ms = getTimeInMillis();
                if (ms == targetMs) return max;
                if (ms < targetMs) break;
                min = max;
                max <<= 1;
                if (max == 0) throw new RuntimeException();
            }
            while ((min - max) > 1) {
                int t = min + (max - min) / 2;
                setTimeInMillis(startMs);
                add(field, t);
                long ms = getTimeInMillis();
                if (ms == targetMs) return t;
                if (ms < targetMs) max = t; else min = t;
            }
        }
        setTimeInMillis(startMs);
        add(field, min);
        return min;
    }

    // ---- limits -------------------------------------------------------

    private int icuLimit(int field, int which) {
        return ICU_FIELD_LIMITS[field - YEAR_WOY][which];
    }

    public final int getMinimum(int field) {
        checkField(field);
        return field >= YEAR_WOY ? icuLimit(field, 0) : mCal.getMinimum(field);
    }
    public final int getGreatestMinimum(int field) {
        checkField(field);
        return field >= YEAR_WOY ? icuLimit(field, 1) : mCal.getGreatestMinimum(field);
    }
    public final int getLeastMaximum(int field) {
        checkField(field);
        return field >= YEAR_WOY ? icuLimit(field, 2) : mCal.getLeastMaximum(field);
    }
    public final int getMaximum(int field) {
        checkField(field);
        return field >= YEAR_WOY ? icuLimit(field, 3) : mCal.getMaximum(field);
    }
    public int getActualMinimum(int field) {
        checkField(field);
        return field >= YEAR_WOY ? icuLimit(field, 0) : mCal.getActualMinimum(field);
    }
    public int getActualMaximum(int field) {
        checkField(field);
        return field >= YEAR_WOY ? icuLimit(field, 3) : mCal.getActualMaximum(field);
    }

    public final int getFieldCount() { return FIELD_COUNT; }

    // ---- behaviour settings -------------------------------------------

    public void setLenient(boolean lenient) { mCal.setLenient(lenient); }
    public boolean isLenient() { return mCal.isLenient(); }

    /** Stored and reported. The JDK resolves a repeated or skipped wall time
     *  as ICU's default, WALLTIME_LAST, does; the other two are not applied. */
    public void setRepeatedWallTimeOption(int option) {
        if (option == WALLTIME_LAST || option == WALLTIME_FIRST) mRepeatedWallTime = option;
    }
    public int getRepeatedWallTimeOption() { return mRepeatedWallTime; }
    public void setSkippedWallTimeOption(int option) {
        if (option >= WALLTIME_LAST && option <= WALLTIME_NEXT_VALID) mSkippedWallTime = option;
    }
    public int getSkippedWallTimeOption() { return mSkippedWallTime; }

    public void setFirstDayOfWeek(int value) {
        if (value < SUNDAY || value > SATURDAY) {
            throw new IllegalArgumentException("Invalid day of week");
        }
        mCal.setFirstDayOfWeek(value);
    }
    public int getFirstDayOfWeek() { return mCal.getFirstDayOfWeek(); }
    public void setMinimalDaysInFirstWeek(int value) {
        if (value < 1) value = 1; else if (value > 7) value = 7;
        mCal.setMinimalDaysInFirstWeek(value);
    }
    public int getMinimalDaysInFirstWeek() { return mCal.getMinimalDaysInFirstWeek(); }

    // ---- week data and weekend ----------------------------------------

    /** ICU's Calendar.WeekData: the six values of a region's week. */
    public static final class WeekData {
        public final int firstDayOfWeek;
        public final int minimalDaysInFirstWeek;
        public final int weekendOnset;
        public final int weekendOnsetMillis;
        public final int weekendCease;
        public final int weekendCeaseMillis;

        public WeekData(int fdow, int mdifw, int weekendOnset, int weekendOnsetMillis,
                        int weekendCease, int weekendCeaseMillis) {
            this.firstDayOfWeek = fdow;
            this.minimalDaysInFirstWeek = mdifw;
            this.weekendOnset = weekendOnset;
            this.weekendOnsetMillis = weekendOnsetMillis;
            this.weekendCease = weekendCease;
            this.weekendCeaseMillis = weekendCeaseMillis;
        }

        @Override public int hashCode() {
            return ((((firstDayOfWeek * 37 + minimalDaysInFirstWeek) * 37 + weekendOnset) * 37
                    + weekendOnsetMillis) * 37 + weekendCease) * 37 + weekendCeaseMillis;
        }

        @Override public boolean equals(Object other) {
            if (this == other) return true;
            if (!(other instanceof WeekData)) return false;
            WeekData that = (WeekData) other;
            return firstDayOfWeek == that.firstDayOfWeek
                    && minimalDaysInFirstWeek == that.minimalDaysInFirstWeek
                    && weekendOnset == that.weekendOnset
                    && weekendOnsetMillis == that.weekendOnsetMillis
                    && weekendCease == that.weekendCease
                    && weekendCeaseMillis == that.weekendCeaseMillis;
        }

        @Override public String toString() {
            return "{" + firstDayOfWeek + ", " + minimalDaysInFirstWeek + ", " + weekendOnset
                    + ", " + weekendOnsetMillis + ", " + weekendCease + ", " + weekendCeaseMillis + "}";
        }
    }

    /** CLDR's weekData (firstDay, minDays, weekendStart, weekendEnd) as ICU 72
     *  (Android 14) answers getWeekDataForRegion for every ISO region, grouped
     *  by value; a region not listed has "001"'s. Generated from Android's own
     *  android.icu.util.Calendar, not chosen by hand. */
    private static final WeekData WORLD = new WeekData(2, 1, 7, 0, 1, 86400000);
    private static final java.util.Map<String, WeekData> REGION_WEEK = new java.util.HashMap<>();
    private static void region(WeekData wd, String regions) {
        for (String r : regions.split(" ")) REGION_WEEK.put(r, wd);
    }
    static {
        region(new WeekData(1, 1, 1, 0, 1, 86400000), "IN");
        region(new WeekData(1, 1, 6, 0, 7, 86400000), "IL SA YE");
        region(new WeekData(1, 1, 7, 0, 1, 86400000),
               "AG AS BD BR BS BT BW BZ CA CO DM DO ET GT GU HK HN ID JM JP KE KH KR LA MH MM MO MT"
               + " MX MZ NI NP PA PE PH PK PR PY SG SV TH TT TW UM US VE VI WS ZA ZW");
        region(new WeekData(1, 4, 7, 0, 1, 86400000), "PT");
        region(new WeekData(2, 1, 1, 0, 1, 86400000), "UG");
        region(new WeekData(2, 4, 7, 0, 1, 86400000),
               "AD AT AX BE BG CH CZ DE DK EE ES FI FJ FO FR GB GF GG GI GP GR HU IE IM IS IT JE LI"
               + " LT LU MC MQ NL NO PL RE RU SE SJ SK SM VA");
        region(new WeekData(6, 1, 7, 0, 1, 86400000), "MV");
        region(new WeekData(7, 1, 5, 0, 6, 86400000), "AF");
        region(new WeekData(7, 1, 6, 0, 6, 86400000), "IR");
        region(new WeekData(7, 1, 6, 0, 7, 86400000), "BH DZ EG IQ JO KW LY OM QA SD SY");
        region(new WeekData(7, 1, 7, 0, 1, 86400000), "AE DJ");
    }

    /** ICU's getRegionForSupplementalData: the "rg" override, else the
     *  locale's region; a language alone takes the default locale's region
     *  when the languages agree (ICU adds likely subtags), else "001". */
    private static String regionOf(Locale locale) {
        if (locale == null) return "001";
        String rg = locale.getUnicodeLocaleType("rg");
        if (rg != null && rg.length() >= 2) return rg.substring(0, 2).toUpperCase(Locale.ROOT);
        String c = locale.getCountry();
        if (c != null && !c.isEmpty()) return c;
        Locale def = Locale.getDefault();
        if (def.getLanguage().equals(locale.getLanguage()) && !def.getCountry().isEmpty()) {
            return def.getCountry();
        }
        return "001";
    }

    public static WeekData getWeekDataForRegion(String region) {
        if (region == null) return WORLD;
        WeekData wd = REGION_WEEK.get(region.toUpperCase(Locale.ROOT));
        return wd != null ? wd : WORLD;
    }

    public WeekData getWeekData() {
        return new WeekData(getFirstDayOfWeek(), getMinimalDaysInFirstWeek(), mWeekendOnset,
                mWeekendOnsetMillis, mWeekendCease, mWeekendCeaseMillis);
    }

    public Calendar setWeekData(WeekData wdata) {
        setFirstDayOfWeek(wdata.firstDayOfWeek);
        setMinimalDaysInFirstWeek(wdata.minimalDaysInFirstWeek);
        mWeekendOnset = wdata.weekendOnset;
        mWeekendOnsetMillis = wdata.weekendOnsetMillis;
        mWeekendCease = wdata.weekendCease;
        mWeekendCeaseMillis = wdata.weekendCeaseMillis;
        return this;
    }

    /** ICU's getDayOfWeekType (deprecated, hidden from the SDK but present on a device), which isWeekend uses. */
    public int getDayOfWeekType(int dayOfWeek) {
        if (dayOfWeek < SUNDAY || dayOfWeek > SATURDAY) {
            throw new IllegalArgumentException("Invalid day of week");
        }
        if (mWeekendOnset == mWeekendCease) {
            if (dayOfWeek != mWeekendOnset) return WEEKDAY;
            return mWeekendOnsetMillis == 0 ? WEEKEND : WEEKEND_ONSET;
        }
        if (mWeekendOnset < mWeekendCease) {
            if (dayOfWeek < mWeekendOnset || dayOfWeek > mWeekendCease) return WEEKDAY;
        } else {
            if (dayOfWeek > mWeekendCease && dayOfWeek < mWeekendOnset) return WEEKDAY;
        }
        if (dayOfWeek == mWeekendOnset) return mWeekendOnsetMillis == 0 ? WEEKEND : WEEKEND_ONSET;
        if (dayOfWeek == mWeekendCease) {
            return mWeekendCeaseMillis >= 86400000 ? WEEKEND : WEEKEND_CEASE;
        }
        return WEEKEND;
    }

    /** ICU's getWeekendTransition (deprecated and hidden like getDayOfWeekType). */
    public int getWeekendTransition(int dayOfWeek) {
        if (dayOfWeek == mWeekendOnset) return mWeekendOnsetMillis;
        if (dayOfWeek == mWeekendCease) return mWeekendCeaseMillis;
        throw new IllegalArgumentException("Not weekend transition day");
    }

    public boolean isWeekend(java.util.Date date) {
        setTime(date);
        return isWeekend();
    }

    public boolean isWeekend() {
        int dow = get(DAY_OF_WEEK);
        int type = getDayOfWeekType(dow);
        switch (type) {
            case WEEKDAY: return false;
            case WEEKEND: return true;
            default: {
                int millisInDay = mCal.get(MILLISECOND) + 1000 * (mCal.get(SECOND)
                        + 60 * (mCal.get(MINUTE) + 60 * mCal.get(HOUR_OF_DAY)));
                int transition = type == WEEKEND_ONSET ? mWeekendOnsetMillis : mWeekendCeaseMillis;
                return type == WEEKEND_ONSET ? millisInDay >= transition : millisInDay < transition;
            }
        }
    }

    // ---- formatting ---------------------------------------------------

    /** ICU answers the implementation class's name here. */
    public String getDisplayName(Locale loc) { return getClass().getName(); }
    public String getDisplayName(ULocale loc) { return getClass().getName(); }

    public android.icu.text.DateFormat getDateTimeFormat(int dateStyle, int timeStyle, Locale loc) {
        android.icu.text.DateFormat f =
                android.icu.text.DateFormat.getDateTimeInstance(dateStyle, timeStyle, loc);
        f.setCalendar((Calendar) clone());
        return f;
    }
    public android.icu.text.DateFormat getDateTimeFormat(int dateStyle, int timeStyle, ULocale loc) {
        return getDateTimeFormat(dateStyle, timeStyle, loc != null ? loc.toLocale() : Locale.getDefault());
    }

    // ---- identity -----------------------------------------------------

    public boolean isEquivalentTo(Calendar other) {
        return other != null && getClass() == other.getClass()
                && isLenient() == other.isLenient()
                && getFirstDayOfWeek() == other.getFirstDayOfWeek()
                && getMinimalDaysInFirstWeek() == other.getMinimalDaysInFirstWeek()
                && mCal.getTimeZone().equals(other.mCal.getTimeZone())
                && mRepeatedWallTime == other.mRepeatedWallTime
                && mSkippedWallTime == other.mSkippedWallTime
                && getType().equals(other.getType());
    }

    @Override public boolean equals(Object obj) {
        if (obj == null) return false;
        if (this == obj) return true;
        if (getClass() != obj.getClass()) return false;
        Calendar that = (Calendar) obj;
        return isEquivalentTo(that) && getTimeInMillis() == that.getTimeInMillis();
    }

    @Override public int hashCode() {
        return (isLenient() ? 1 : 0) | (getFirstDayOfWeek() << 1) | (getMinimalDaysInFirstWeek() << 4)
                | (mRepeatedWallTime << 7) | (mSkippedWallTime << 9)
                | (mCal.getTimeZone().hashCode() << 11);
    }

    public boolean before(Object when) { return when instanceof Calendar c && compareTo(c) < 0; }
    public boolean after(Object when) { return when instanceof Calendar c && compareTo(c) > 0; }

    @Override public int compareTo(Calendar that) {
        return Long.compare(getTimeInMillis(), that.getTimeInMillis());
    }

    @Override public Object clone() {
        try {
            Calendar c = (Calendar) super.clone();
            c.mCal = (java.util.Calendar) mCal.clone();
            return c;
        } catch (CloneNotSupportedException e) {
            throw new AssertionError(e);
        }
    }

    @Override public String toString() {
        return getClass().getName() + "[time=" + getTimeInMillis() + ",lenient=" + isLenient()
                + ",zone=" + mCal.getTimeZone().getID() + ",firstDayOfWeek=" + getFirstDayOfWeek()
                + ",minimalDaysInFirstWeek=" + getMinimalDaysInFirstWeek()
                + ",repeatedWallTime=" + mRepeatedWallTime + ",skippedWallTime=" + mSkippedWallTime
                + ",ERA=" + get(ERA) + ",YEAR=" + get(YEAR) + ",MONTH=" + get(MONTH)
                + ",DAY_OF_MONTH=" + get(DAY_OF_MONTH) + ",HOUR_OF_DAY=" + get(HOUR_OF_DAY)
                + ",MINUTE=" + get(MINUTE) + ",SECOND=" + get(SECOND) + ",MILLISECOND=" + get(MILLISECOND)
                + "]";
    }

    public java.util.Calendar toJavaCalendar() { return mCal; }

    /** Bridge for android.icu.text.* (which plumbs through java.util.Calendar) --
     *  the mirror of toJavaCalendar. Not part of ICU's surface; the ctor is
     *  protected so a cross-package wrap needs this, exactly as ULocale.unwrap
     *  exists for java.util.Locale. */
    public static Calendar wrap(java.util.Calendar c) {
        if (c == null) return null;
        Locale loc = Locale.getDefault();
        return "gregory".equals(c.getCalendarType()) ? new GregorianCalendar(c, loc) : new Calendar(c, loc);
    }
}
