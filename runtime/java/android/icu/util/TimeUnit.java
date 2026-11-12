package android.icu.util;

import java.util.HashMap;
import java.util.Map;

/**
 * The units of duration, as their own type: ICU's TimeUnit is the MeasureUnit
 * subclass that MeasureUnit.SECOND ... MeasureUnit.YEAR are declared as, and
 * what TimeUnitAmount and the duration formatters take.
 *
 * Source: https://developer.android.com/reference/android/icu/util/TimeUnit
 */
public class TimeUnit extends MeasureUnit {

    // One instance per unit, whichever of MeasureUnit and TimeUnit is
    // initialized first. Each class's constants name the other's, so a plain
    // `SECOND = MeasureUnit.SECOND` reads null when MeasureUnit's initializer is
    // the one that started this class's. Both sides go through of() instead;
    // the map belongs to neither class's static state.
    private static final class Units {
        static final Map<String, TimeUnit> BY_SUBTYPE = new HashMap<String, TimeUnit>();
    }

    static TimeUnit of(String subtype) {
        synchronized (Units.BY_SUBTYPE) {
            TimeUnit unit = Units.BY_SUBTYPE.get(subtype);
            if (unit == null) {
                unit = new TimeUnit("duration", subtype);
                Units.BY_SUBTYPE.put(subtype, unit);
            }
            return unit;
        }
    }

    public static final TimeUnit SECOND = of("second");
    public static final TimeUnit MINUTE = of("minute");
    public static final TimeUnit HOUR = of("hour");
    public static final TimeUnit DAY = of("day");
    public static final TimeUnit WEEK = of("week");
    public static final TimeUnit MONTH = of("month");
    public static final TimeUnit YEAR = of("year");

    TimeUnit(String type, String subtype) {
        super(type, subtype);
    }

    /** The seven units, smallest first. */
    public static TimeUnit[] values() {
        return new TimeUnit[] { SECOND, MINUTE, HOUR, DAY, WEEK, MONTH, YEAR };
    }
}
