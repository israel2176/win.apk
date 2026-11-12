package android.os;
/** An event after which the system should take a profile of the app on its own
 *  (API 36): registered with ProfilingManager.addProfilingTriggers. Mac.apk runs
 *  no profiler, so a trigger is only kept; none ever fires.
 *  Source: frameworks/base/core/java/android/os/ProfilingTrigger.java */
public final class ProfilingTrigger {
    /** No trigger: what a ProfilingResult of an app's own request reports. */
    public static final int TRIGGER_TYPE_NONE = 0;
    /** The app reported itself fully drawn (Activity.reportFullyDrawn). */
    public static final int TRIGGER_TYPE_APP_FULLY_DRAWN = 1;
    /** The app stopped responding. */
    public static final int TRIGGER_TYPE_ANR = 2;

    private final int mTriggerType;
    private final int mRateLimitingPeriodHours;

    private ProfilingTrigger(int triggerType, int rateLimitingPeriodHours) {
        mTriggerType = triggerType;
        mRateLimitingPeriodHours = rateLimitingPeriodHours;
    }

    public int getTriggerType() { return mTriggerType; }
    public int getRateLimitingPeriodHours() { return mRateLimitingPeriodHours; }

    public static final class Builder {
        private final int mTriggerType;
        private int mRateLimitingPeriodHours = 0;

        public Builder(int triggerType) { mTriggerType = triggerType; }

        /** The least time between two profiles from this trigger; AOSP refuses a negative one. */
        public Builder setRateLimitingPeriodHours(int hours) {
            if (hours < 0) {
                throw new IllegalArgumentException("Hours can't be negative. Try again.");
            }
            mRateLimitingPeriodHours = hours;
            return this;
        }

        public ProfilingTrigger build() {
            return new ProfilingTrigger(mTriggerType, mRateLimitingPeriodHours);
        }
    }
}
