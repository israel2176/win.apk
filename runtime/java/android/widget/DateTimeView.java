// android.widget.DateTimeView -- the TextView a notification template shows its
// time in (notification_template_part_time: android:id/time,
// android:showRelative="true"). Was absent, so the framework's templates and
// an app's custom notification layouts that name it got a FrameLayout. AOSP:
//   frameworks/base/core/java/android/widget/DateTimeView.java (Android 14)
//   @RemoteView public class DateTimeView extends TextView
// The logic is AOSP 14's, member for member (checked against the bytecode in
// android-all 14): the absolute form is the short time within today (or
// within 12 hours either side of it), else the short date, re-evaluated at the
// boundary; the relative form is "now", "5m", "3h", "2d", "1y" (and "in 5m"
// ahead), from com.android.internal.R.string's duration_*_shortest strings in
// the framework table, the accessibility text from duration_*_relative; one
// receiver per thread for TIME_TICK, TIME_SET, CONFIGURATION_CHANGED and
// TIMEZONE_CHANGED updates every attached view. Win.apk-only: the framework
// strings are looked up by name (this tree has no com.android.internal.R),
// English when the table lacks one; the plural message of the accessibility
// text is selected here (android.icu.text.MessageFormat has no plural yet);
// onAttachedToWindow, onDetachedFromWindow and onConfigurationChanged are
// public because this tree's View and TextView declare them so.
package android.widget;

import android.app.ActivityThread;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.os.Handler;
import android.text.TextUtils;
import android.util.AttributeSet;
import android.view.accessibility.AccessibilityNodeInfo;

import java.text.DateFormat;
import java.time.Instant;
import java.time.LocalDate;
import java.time.LocalDateTime;
import java.time.LocalTime;
import java.time.ZoneId;
import java.time.temporal.JulianFields;
import java.util.ArrayList;
import java.util.Date;

public class DateTimeView extends TextView {
    private static final String ANDROID_NS = "http://schemas.android.com/apk/res/android";
    private static final int SHOW_TIME = 0;
    private static final int SHOW_MONTH_DAY_YEAR = 1;

    private long mTimeMillis;
    // The LocalDateTime equivalent of mTimeMillis but truncated to minute, i.e. no seconds / nanos.
    private LocalDateTime mLocalTime;

    int mLastDisplay = -1;
    DateFormat mLastFormat;

    private long mUpdateTimeMillis;
    private static final ThreadLocal<ReceiverInfo> sReceiverInfo = new ThreadLocal<ReceiverInfo>();
    private String mNowText;
    private boolean mShowRelativeTime;

    public DateTimeView(Context context) {
        this(context, null);
    }

    public DateTimeView(Context context, AttributeSet attrs) {
        super(context, attrs);
        // AOSP reads com.android.internal.R.styleable.DateTimeView (showRelative,
        // a private attribute in the android namespace) through
        // obtainStyledAttributes; the framework layouts spell it android:showRelative.
        if (attrs != null && attrs.getAttributeValue(ANDROID_NS, "showRelative") != null) {
            setShowRelativeTime(attrs.getAttributeBooleanValue(ANDROID_NS, "showRelative", false));
        }
    }

    @Override
    public void onAttachedToWindow() {
        super.onAttachedToWindow();
        ReceiverInfo ri = sReceiverInfo.get();
        if (ri == null) {
            ri = new ReceiverInfo();
            sReceiverInfo.set(ri);
        }
        ri.addView(this);
        // The view may not have been attached when we had to update it.
        if (mShowRelativeTime) {
            update();
        }
    }

    @Override
    public void onDetachedFromWindow() {
        super.onDetachedFromWindow();
        final ReceiverInfo ri = sReceiverInfo.get();
        if (ri != null) {
            ri.removeView(this);
        }
    }

    /** Sets the time, in milliseconds since the epoch (RemoteViews.setLong("setTime")). */
    public void setTime(long timeMillis) {
        mTimeMillis = timeMillis;
        LocalDateTime dateTime = toLocalDateTime(timeMillis, ZoneId.systemDefault());
        mLocalTime = dateTime.withSecond(0);
        update();
    }

    public void setShowRelativeTime(boolean showRelativeTime) {
        mShowRelativeTime = showRelativeTime;
        updateNowText();
        update();
    }

    /** True when the time is shown relative to now ("5m"), not as a time or date. */
    public boolean isShowRelativeTime() {
        return mShowRelativeTime;
    }

    @Override
    public void setVisibility(int visibility) {
        boolean gotVisible = visibility != GONE && getVisibility() == GONE;
        super.setVisibility(visibility);
        if (gotVisible) {
            update();
        }
    }

    void update() {
        if (mLocalTime == null || getVisibility() == GONE) {
            return;
        }
        if (mShowRelativeTime) {
            updateRelativeTime();
            return;
        }

        int display;
        ZoneId zoneId = ZoneId.systemDefault();

        // localTime is the local time for mTime but with zero seconds / nanos.
        LocalDateTime localTime = mLocalTime;
        LocalDateTime localStartOfDay = LocalDateTime.of(localTime.toLocalDate(), LocalTime.MIDNIGHT);
        LocalDateTime localTomorrowStartOfDay = localStartOfDay.plusDays(1);
        // now is current local time but with zero seconds / nanos.
        LocalDateTime localNow = LocalDateTime.now(zoneId).withSecond(0);

        long twelveHoursBefore = toEpochMillis(localTime.minusHours(12), zoneId);
        long twelveHoursAfter = toEpochMillis(localTime.plusHours(12), zoneId);
        long midnightBefore = toEpochMillis(localStartOfDay, zoneId);
        long midnightAfter = toEpochMillis(localTomorrowStartOfDay, zoneId);
        long time = toEpochMillis(localTime, zoneId);
        long now = toEpochMillis(localNow, zoneId);

        // Choose the display mode
        if ((now >= midnightBefore && now < midnightAfter)
                || (now >= twelveHoursBefore && now < twelveHoursAfter)) {
            display = SHOW_TIME;
        } else {
            display = SHOW_MONTH_DAY_YEAR;
        }

        // Choose the format
        DateFormat format;
        if (display == mLastDisplay && mLastFormat != null) {
            // use cached format
            format = mLastFormat;
        } else {
            switch (display) {
                case SHOW_TIME:
                    format = getTimeFormat();
                    break;
                case SHOW_MONTH_DAY_YEAR:
                    format = DateFormat.getDateInstance(DateFormat.SHORT);
                    break;
                default:
                    throw new RuntimeException("unknown display value: " + display);
            }
            mLastFormat = format;
        }

        // Set the text
        String text = format.format(new Date(time));
        maybeSetText(text);

        // Schedule the next update
        if (display == SHOW_TIME) {
            // Currently showing the time, update at the later of twelve hours after or midnight.
            mUpdateTimeMillis = twelveHoursAfter > midnightAfter ? twelveHoursAfter : midnightAfter;
        } else {
            // Currently showing the date
            if (mTimeMillis < now) {
                // If the time is in the past, don't need to update
                mUpdateTimeMillis = 0;
            } else {
                // If the time is in the future, schedule one at the earlier of twelve hours
                // before or midnight before.
                mUpdateTimeMillis = twelveHoursBefore < midnightBefore ? twelveHoursBefore : midnightBefore;
            }
        }
    }

    private void updateRelativeTime() {
        long now = System.currentTimeMillis();
        long duration = Math.abs(now - mTimeMillis);
        int count;
        long millisIncrease;
        boolean past = (now >= mTimeMillis);
        String result;
        if (duration < MINUTE_IN_MILLIS) {
            maybeSetText(mNowText);
            mUpdateTimeMillis = mTimeMillis + MINUTE_IN_MILLIS + 1;
            return;
        } else if (duration < HOUR_IN_MILLIS) {
            count = (int) (duration / MINUTE_IN_MILLIS);
            result = shortest(past ? "duration_minutes_shortest" : "duration_minutes_shortest_future",
                    past ? "%dm" : "in %dm", count);
            millisIncrease = MINUTE_IN_MILLIS;
        } else if (duration < DAY_IN_MILLIS) {
            count = (int) (duration / HOUR_IN_MILLIS);
            result = shortest(past ? "duration_hours_shortest" : "duration_hours_shortest_future",
                    past ? "%dh" : "in %dh", count);
            millisIncrease = HOUR_IN_MILLIS;
        } else if (duration < YEAR_IN_MILLIS) {
            // In weird cases it can become 0 because of daylight savings
            LocalDateTime localDateTime = mLocalTime;
            ZoneId zoneId = ZoneId.systemDefault();
            LocalDateTime localNow = toLocalDateTime(now, zoneId);

            count = Math.max(Math.abs(dayDistance(localDateTime, localNow)), 1);
            result = shortest(past ? "duration_days_shortest" : "duration_days_shortest_future",
                    past ? "%dd" : "in %dd", count);
            if (past || count != 1) {
                mUpdateTimeMillis = computeNextMidnight(localNow, zoneId);
                millisIncrease = -1;
            } else {
                millisIncrease = DAY_IN_MILLIS;
            }
        } else {
            count = (int) (duration / YEAR_IN_MILLIS);
            result = shortest(past ? "duration_years_shortest" : "duration_years_shortest_future",
                    past ? "%dy" : "in %dy", count);
            millisIncrease = YEAR_IN_MILLIS;
        }
        if (millisIncrease != -1) {
            if (past) {
                mUpdateTimeMillis = mTimeMillis + millisIncrease * (count + 1) + 1;
            } else {
                mUpdateTimeMillis = mTimeMillis - millisIncrease * count + 1;
            }
        }
        maybeSetText(result);
    }

    // android.text.format.DateUtils' constants, as AOSP's source uses them.
    private static final long MINUTE_IN_MILLIS = 60000L;
    private static final long HOUR_IN_MILLIS = 3600000L;
    private static final long DAY_IN_MILLIS = 86400000L;
    private static final long YEAR_IN_MILLIS = 31449600000L;   // 52 weeks, as DateUtils has it

    /** Sets the text only when it changed, so a minute's tick relays out nothing. */
    private void maybeSetText(String text) {
        if (TextUtils.equals(getText(), text)) {
            return;
        }
        setText(text);
    }

    /** Returns the epoch millis for the next midnight in the specified timezone. */
    private static long computeNextMidnight(LocalDateTime time, ZoneId zoneId) {
        // This ignores the chance of overflow: it should never happen.
        LocalDate tomorrow = time.toLocalDate().plusDays(1);
        LocalDateTime nextMidnight = LocalDateTime.of(tomorrow, LocalTime.MIDNIGHT);
        return toEpochMillis(nextMidnight, zoneId);
    }

    @Override
    public void onConfigurationChanged(Configuration newConfig) {
        super.onConfigurationChanged(newConfig);
        updateNowText();
        update();
    }

    private void updateNowText() {
        if (!mShowRelativeTime) {
            return;
        }
        mNowText = frameworkString("now_string_shortest", "now");
    }

    // Return the number of days between the two dates.
    private static int dayDistance(LocalDateTime start, LocalDateTime end) {
        return (int) (end.getLong(JulianFields.JULIAN_DAY) - start.getLong(JulianFields.JULIAN_DAY));
    }

    private DateFormat getTimeFormat() {
        return android.text.format.DateFormat.getTimeFormat(getContext());
    }

    void clearFormatAndUpdate() {
        mLastFormat = null;
        update();
    }

    @Override
    public void onInitializeAccessibilityNodeInfoInternal(AccessibilityNodeInfo info) {
        super.onInitializeAccessibilityNodeInfoInternal(info);
        if (mShowRelativeTime) {
            // The short version of the time might not be completely understandable and for
            // accessibility we rather have a longer version.
            long now = System.currentTimeMillis();
            long duration = Math.abs(now - mTimeMillis);
            int count;
            boolean past = (now >= mTimeMillis);
            String result;
            if (duration < MINUTE_IN_MILLIS) {
                result = mNowText;
            } else if (duration < HOUR_IN_MILLIS) {
                count = (int) (duration / MINUTE_IN_MILLIS);
                result = relative(past ? "duration_minutes_relative" : "duration_minutes_relative_future",
                        past ? "{count, plural, =1 {# minute ago} other {# minutes ago}}"
                             : "{count, plural, =1 {in # minute} other {in # minutes}}", count);
            } else if (duration < DAY_IN_MILLIS) {
                count = (int) (duration / HOUR_IN_MILLIS);
                result = relative(past ? "duration_hours_relative" : "duration_hours_relative_future",
                        past ? "{count, plural, =1 {# hour ago} other {# hours ago}}"
                             : "{count, plural, =1 {in # hour} other {in # hours}}", count);
            } else if (duration < YEAR_IN_MILLIS) {
                // In weird cases it can become 0 because of daylight savings
                LocalDateTime localDateTime = mLocalTime;
                ZoneId zoneId = ZoneId.systemDefault();
                LocalDateTime localNow = toLocalDateTime(now, zoneId);

                count = Math.max(Math.abs(dayDistance(localDateTime, localNow)), 1);
                result = relative(past ? "duration_days_relative" : "duration_days_relative_future",
                        past ? "{count, plural, =1 {# day ago} other {# days ago}}"
                             : "{count, plural, =1 {in # day} other {in # days}}", count);
            } else {
                count = (int) (duration / YEAR_IN_MILLIS);
                result = relative(past ? "duration_years_relative" : "duration_years_relative_future",
                        past ? "{count, plural, =1 {# year ago} other {# years ago}}"
                             : "{count, plural, =1 {in # year} other {in # years}}", count);
            }
            info.setText(result);
        }
    }

    /**
     * @hide
     */
    public static void setReceiverHandler(Handler handler) {
        ReceiverInfo ri = sReceiverInfo.get();
        if (ri == null) {
            ri = new ReceiverInfo();
            sReceiverInfo.set(ri);
        }
        ri.setHandler(handler);
    }

    // ---- the framework strings (com.android.internal.R.string in AOSP) ----

    private String frameworkString(String name, String english) {
        try {
            Resources res = getContext().getResources();
            int id = res.getIdentifier(name, "string", "android");
            if (id != 0) {
                String s = res.getString(id);
                if (s != null) return s;
            }
        } catch (RuntimeException ignored) {
            // no framework table: English, as the default locale's strings are
        }
        return english;
    }

    /** Resources.getString(id, count) of a duration_*_shortest string ("%dm"). */
    private String shortest(String name, String english, int count) {
        String pattern = frameworkString(name, english);
        try {
            return String.format(uiLocale(), pattern, count);
        } catch (java.util.IllegalFormatException e) {
            return String.format(java.util.Locale.ROOT, english, count);
        }
    }

    /** PluralsMessageFormatter.format(res, {count}, id) of a duration_*_relative
     *  string: "{count, plural, =1 {# minute ago} other {# minutes ago}}". */
    private String relative(String name, String english, int count) {
        String s = formatPlural(frameworkString(name, english), count);
        return s != null ? s : formatPlural(english, count);
    }

    private java.util.Locale uiLocale() {
        try {
            java.util.Locale l = getContext().getResources().getConfiguration().locale;
            if (l != null) return l;
        } catch (RuntimeException ignored) {
        }
        return java.util.Locale.getDefault();
    }

    /** The one ICU MessageFormat shape the duration strings use: a single
     *  "{count, plural, ...}" argument, its branches "=N {...}" or a plural
     *  keyword, '#' the number. Null when the pattern is not of that shape. */
    private String formatPlural(String pattern, int count) {
        if (pattern == null) return null;
        int open = pattern.indexOf('{');
        int comma = pattern.indexOf("plural", open + 1);
        if (open < 0 || comma < 0) return null;
        int i = pattern.indexOf(',', comma);
        if (i < 0) return null;
        i++;
        String keyword;
        try {
            keyword = android.icu.text.PluralRules.forLocale(uiLocale()).select(count);
        } catch (RuntimeException e) {
            keyword = count == 1 ? "one" : "other";
        }
        String exact = "=" + count;
        String chosen = null, other = null, byKeyword = null;
        int n = pattern.length();
        while (i < n) {
            while (i < n && Character.isWhitespace(pattern.charAt(i))) i++;
            if (i >= n || pattern.charAt(i) == '}') break;
            int sel = i;
            while (i < n && pattern.charAt(i) != '{' && !Character.isWhitespace(pattern.charAt(i))) i++;
            String selector = pattern.substring(sel, i);
            while (i < n && pattern.charAt(i) != '{') i++;
            if (i >= n) return null;
            int depth = 0, start = i + 1;
            for (; i < n; i++) {
                char c = pattern.charAt(i);
                if (c == '{') depth++;
                else if (c == '}' && --depth == 0) break;
            }
            if (i >= n) return null;
            String body = pattern.substring(start, i);
            i++;
            if (selector.equals(exact) && chosen == null) chosen = body;
            else if (selector.equals(keyword) && byKeyword == null) byKeyword = body;
            else if (selector.equals("other")) other = body;
        }
        String body = chosen != null ? chosen : byKeyword != null ? byKeyword : other;
        if (body == null) return null;
        String num = java.text.NumberFormat.getIntegerInstance(uiLocale()).format(count);
        return (pattern.substring(0, open) + body.replace("#", num)).trim();
    }

    private static LocalDateTime toLocalDateTime(long timeMillis, ZoneId zoneId) {
        // java.time types are immutable, but the ZoneRules lookup is cheap enough.
        Instant instant = Instant.ofEpochMilli(timeMillis);
        return LocalDateTime.ofInstant(instant, zoneId);
    }

    private static long toEpochMillis(LocalDateTime time, ZoneId zoneId) {
        Instant instant = time.toInstant(zoneId.getRules().getOffset(time));
        return instant.toEpochMilli();
    }

    private static class ReceiverInfo {
        private final ArrayList<DateTimeView> mAttachedViews = new ArrayList<DateTimeView>();
        private final BroadcastReceiver mReceiver = new BroadcastReceiver() {
            @Override
            public void onReceive(Context context, Intent intent) {
                String action = intent.getAction();
                if (Intent.ACTION_TIME_TICK.equals(action)) {
                    if (System.currentTimeMillis() < getSoonestUpdateTime()) {
                        // The update() function takes a few milliseconds to run because of
                        // all of the time conversions it needs to do, so we can't do that
                        // every minute.
                        return;
                    }
                }
                // ACTION_TIME_CHANGED can also signal a change of 12/24 hr. format.
                updateAll();
            }
        };

        private Handler mHandler = new Handler();

        public void addView(DateTimeView v) {
            synchronized (mAttachedViews) {
                final boolean register = mAttachedViews.isEmpty();
                mAttachedViews.add(v);
                if (register) {
                    register(getApplicationContextIfAvailable(v.getContext()));
                }
            }
        }

        public void removeView(DateTimeView v) {
            synchronized (mAttachedViews) {
                final boolean removed = mAttachedViews.remove(v);
                // Only unregister once when we remove the last view in the list otherwise we risk
                // trying to unregister a receiver that is no longer registered.
                if (removed && mAttachedViews.isEmpty()) {
                    unregister(getApplicationContextIfAvailable(v.getContext()));
                }
            }
        }

        void updateAll() {
            synchronized (mAttachedViews) {
                final int count = mAttachedViews.size();
                for (int i = 0; i < count; i++) {
                    final DateTimeView view = mAttachedViews.get(i);
                    view.post(() -> view.clearFormatAndUpdate());
                }
            }
        }

        long getSoonestUpdateTime() {
            long result = Long.MAX_VALUE;
            synchronized (mAttachedViews) {
                final int count = mAttachedViews.size();
                for (int i = 0; i < count; i++) {
                    final long time = mAttachedViews.get(i).mUpdateTimeMillis;
                    if (time < result) {
                        result = time;
                    }
                }
            }
            return result;
        }

        static final Context getApplicationContextIfAvailable(Context context) {
            final Context ac = context.getApplicationContext();
            if (ac != null) return ac;
            android.app.Application app = ActivityThread.currentApplication();
            return app != null ? app.getApplicationContext() : context;
        }

        void register(Context context) {
            final IntentFilter filter = new IntentFilter();
            filter.addAction(Intent.ACTION_TIME_TICK);
            filter.addAction(Intent.ACTION_TIME_CHANGED);
            filter.addAction(Intent.ACTION_CONFIGURATION_CHANGED);
            filter.addAction(Intent.ACTION_TIMEZONE_CHANGED);
            context.registerReceiver(mReceiver, filter, null, mHandler);
        }

        void unregister(Context context) {
            context.unregisterReceiver(mReceiver);
        }

        public void setHandler(Handler handler) {
            mHandler = handler;
            synchronized (mAttachedViews) {
                if (!mAttachedViews.isEmpty()) {
                    unregister(mAttachedViews.get(0).getContext());
                    register(mAttachedViews.get(0).getContext());
                }
            }
        }
    }
}
