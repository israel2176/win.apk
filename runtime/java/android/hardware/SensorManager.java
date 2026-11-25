// android.hardware.SensorManager -- mirrors AOSP's public surface.
//
// Mac.apk has no sensor hardware. Two halves of the contract:
//
//  (1) The static math utilities (getRotationMatrix, getOrientation,
//      remapCoordinateSystem, getInclination, getAltitude,
//      getAngleChange, getRotationMatrixFromVector,
//      getQuaternionFromVector) are PURE FUNCTIONS -- they compute on
//      caller-supplied float arrays, no hardware involved. These are
//      copied verbatim from AOSP frameworks/base SensorManager.java so
//      tilt/AR games that feed us their own data get the correct
//      result. (URL in repo CLAUDE.md reading list.)
//
//  (2) The instance side hands back real Sensor objects for the sensors
//      this device has, and delivers their events. A PC has none of a
//      phone's motion or environment sensors, so the catalog is EMPTY: no
//      accelerometer and nothing fused from one (gravity, linear
//      acceleration, rotation vectors, orientation), no gyroscope, no
//      magnetometer, light, proximity or step counter. The owner's decisions
//      of November 2026 removed the synthetic "phone held upright" pose and
//      then the gyroscope that reported 0,0,0. getDefaultSensor returns null for every type not in
//      the catalog and PackageManager's sensor features are derived from it,
//      the answer an Android device without the hardware gives.
//      A single daemon timer ticks all registered listeners at their
//      requested-ish rate.
//
//      The timer thread only SYNTHESIZES; it never calls back into the
//      app directly. AOSP resolves one delivery Looper per listener
//      (SystemSensorManager.registerListenerImpl: `Looper looper =
//      (handler != null) ? handler.getLooper() : mMainLooper;`), so the
//      callbacks land on the supplied Handler's thread, or the MAIN
//      thread when none was supplied.
package android.hardware;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.concurrent.CopyOnWriteArrayList;

public class SensorManager {
    public static final int SENSOR_DELAY_FASTEST = 0;
    public static final int SENSOR_DELAY_GAME = 1;
    public static final int SENSOR_DELAY_UI = 2;
    public static final int SENSOR_DELAY_NORMAL = 3;

    public static final int SENSOR_STATUS_NO_CONTACT = -1;
    public static final int SENSOR_STATUS_UNRELIABLE = 0;
    public static final int SENSOR_STATUS_ACCURACY_LOW = 1;
    public static final int SENSOR_STATUS_ACCURACY_MEDIUM = 2;
    public static final int SENSOR_STATUS_ACCURACY_HIGH = 3;

    // Legacy SensorListener bit constants (deprecated API, still queried).
    public static final int SENSOR_ORIENTATION = 1 << 0;
    public static final int SENSOR_ACCELEROMETER = 1 << 1;
    public static final int SENSOR_TEMPERATURE = 1 << 2;
    public static final int SENSOR_MAGNETIC_FIELD = 1 << 3;
    public static final int SENSOR_LIGHT = 1 << 4;
    public static final int SENSOR_PROXIMITY = 1 << 5;
    public static final int SENSOR_TRICORDER = 1 << 6;
    public static final int SENSOR_ORIENTATION_RAW = 1 << 7;
    public static final int SENSOR_ALL = 0x7F;
    public static final int SENSOR_MIN = SENSOR_ORIENTATION;
    public static final int SENSOR_MAX = ((SENSOR_ALL + 1) >> 1);

    public static final int DATA_X = 0;
    public static final int DATA_Y = 1;
    public static final int DATA_Z = 2;
    public static final int RAW_DATA_INDEX = 3;
    public static final int RAW_DATA_X = 3;
    public static final int RAW_DATA_Y = 4;
    public static final int RAW_DATA_Z = 5;

    // Physical constants (verbatim AOSP).
    public static final float STANDARD_GRAVITY = 9.80665f;
    public static final float GRAVITY_SUN = 275.0f;
    public static final float GRAVITY_MERCURY = 3.70f;
    public static final float GRAVITY_VENUS = 8.87f;
    public static final float GRAVITY_EARTH = 9.80665f;
    public static final float GRAVITY_MOON = 1.6f;
    public static final float GRAVITY_MARS = 3.71f;
    public static final float GRAVITY_JUPITER = 23.12f;
    public static final float GRAVITY_SATURN = 8.96f;
    public static final float GRAVITY_URANUS = 8.69f;
    public static final float GRAVITY_NEPTUNE = 11.0f;
    public static final float GRAVITY_PLUTO = 0.6f;
    public static final float GRAVITY_DEATH_STAR_I = 0.000000353036145f;
    public static final float GRAVITY_THE_ISLAND = 4.815162342f;

    public static final float MAGNETIC_FIELD_EARTH_MAX = 60.0f;
    public static final float MAGNETIC_FIELD_EARTH_MIN = 30.0f;
    public static final float PRESSURE_STANDARD_ATMOSPHERE = 1013.25f;

    public static final float LIGHT_SUNLIGHT_MAX = 120000.0f;
    public static final float LIGHT_SUNLIGHT = 110000.0f;
    public static final float LIGHT_SHADE = 20000.0f;
    public static final float LIGHT_OVERCAST = 10000.0f;
    public static final float LIGHT_SUNRISE = 400.0f;
    public static final float LIGHT_CLOUDY = 100.0f;
    public static final float LIGHT_FULLMOON = 0.25f;
    public static final float LIGHT_NO_MOON = 0.001f;

    // Matches AOSP remapCoordinateSystem's sTempMatrix scratch buffer.
    private static final float[] sTempMatrix = new float[16];

    // --- synthesized sensor catalog -------------------------------------
    private final List<Sensor> mSensors = new ArrayList<>();

    // ---- WHAT THIS DEVICE HONESTLY HAS, AND WHAT IT DOES NOT ---------------
    //
    // A Mac has none of the motion or environment sensors an Android phone has.
    // Probed on Mac14,7 (MacBook Pro 13" M2), 2026-08-23: zero instances of every
    // accelerometer/motion IOKit service name, and no ambient-light service
    // (AppleLMUController / AppleSPUVD6287 / AppleALS all absent).
    //
    // ⚠ There IS an undocumented motion stream. Six AppleSPUHIDDevice instances
    // exist, two with MaxInputReportSize=22, on VENDOR-DEFINED usage page 0xFF00
    // (not the HID Sensors page 0x20), which is why searching for service NAMES
    // finds nothing and concludes "no sensor" -- a wrong answer this file's own
    // history is evidence for. `debug/spuhid_probe.swift` dumps them, and
    // IOHIDManagerOpen succeeds with NO TCC prompt. In a 25s listen those two
    // devices emitted nothing while telemetry devices chattered, so the stream is
    // event-driven or needs a feature report to enable. NOT cracked.
    //
    // OWNER DECISIONS, 2026-08-23, recorded verbatim because they are judgement
    // calls and not derivable from AOSP:
    //   * MAGNETIC_FIELD, PROXIMITY, PRESSURE, AMBIENT_TEMPERATURE and
    //     RELATIVE_HUMIDITY are GONE from the catalog. getDefaultSensor returns
    //     null and getSensorList omits them -- the answer AOSP documents for a
    //     device without the hardware, and the one apps already handle. These
    //     were the worst offenders: a weather or altimeter app would render our
    //     fabricated 1013.25 hPa / 22 C / 45% to the user AS A READING.
    //   * LIGHT is also gone. The owner asked for it to be wired to the host's
    //     real ambient-light sensor (rung 1 of CLAUDE.md's decision ladder, like
    //     pmset for battery and ioreg for brightness) -- but the probe above
    //     found no reachable ALS service on this machine, so there is nothing to
    //     wire yet. Absent is the truthful answer until there is; see
    //     docs/future_implementations.md.
    //   * GYROSCOPE STAYED upstream, reporting 0,0,0 ("a laptop on a desk does
    //     not turn"). Win.apk, November 2026 (the owner's decision): GONE as
    //     well. A PC has no gyroscope, and a gyroscope that can only report
    //     zero is a made-up sensor; Android without one returns null.
    //   * Win.apk, November 2026 (the owner's decision): the MOTION family
    //     (ACCELEROMETER, GRAVITY, LINEAR_ACCELERATION, ROTATION_VECTOR,
    //     GAME_ROTATION_VECTOR, ORIENTATION) is GONE too. It reported a
    //     made-up "phone held upright" gravity vector (upstream's
    //     POSE_ACCEL_*), which a tilt game or a level app takes as a reading.
    //     A PC has no accelerometer, so none is reported, as on an Android
    //     device without one. Windows' own sensor API (the accelerometer of
    //     some 2-in-1s) is not wired yet.
    //
    // Do not "improve" the zeros back into plausible values. A plausible lie is
    // strictly worse than an obvious one: it is what let the fabricated pressure
    // and humidity readings sit here unnoticed.
    /** protected, matching AOSP (SensorManager is abstract upstream with a
     *  protected ctor). Made non-package-private 2026-08-24 so an
     *  InputDevice-scoped SensorManager can be subclassed from android.view:
     *  InputDevice.getSensorManager() must report an EMPTY catalog, because a
     *  touchscreen or a virtual keyboard has no built-in accelerometer, and
     *  handing back this process-wide instance would tell an app the keyboard
     *  has a gyroscope. */
    protected SensorManager() {
        // Synthesize the "device still" sensor set. Continuous sensors carry a
        // 10000us (100Hz) minDelay like a typical phone.
        // No sensor at all: see makeEvent and the header. A PC has no
        // accelerometer (and so nothing fused from one), no gyroscope, no
        // magnetometer, light or proximity sensor. When Windows' own sensor
        // API is wired (a 2-in-1's real accelerometer), its sensors are added
        // here and to MACAPK_CATALOG_TYPES together.
        // No TYPE_STEP_COUNTER / TYPE_STEP_DETECTOR: a Mac has no pedometer.
        // getDefaultSensor(TYPE_STEP_COUNTER) is THE feature-detection idiom
        // ("non-null means this device counts steps"), and an Android device
        // without a step-counter HAL returns null for it. Advertising one that
        // can only ever report 0 steps is silently wrong -- indistinguishable
        // from "you did not walk" -- where null is the honest, well-understood
        // answer every fitness app already handles.

        // Guard: MACAPK_CATALOG_TYPES is what PackageManager advertises, and the
        // adds above are what apps actually get. If those two ever disagree we
        // are back to advertising a sensor that returns null -- the exact bug
        // this arrangement exists to prevent -- so say so LOUDLY rather than
        // letting it sit unnoticed for months as the last one did.
        if (mSensors.size() != MACAPK_CATALOG_TYPES.length) {
            com.macapk.loader.NativeLog.e("SensorManager",
                "catalog/advertised MISMATCH: " + mSensors.size() + " sensors built vs "
                + MACAPK_CATALOG_TYPES.length + " advertised -- hasSystemFeature will lie");
        } else {
            for (int type : MACAPK_CATALOG_TYPES) {
                boolean found = false;
                for (Sensor sn : mSensors) if (sn.getType() == type) { found = true; break; }
                if (!found) com.macapk.loader.NativeLog.e("SensorManager",
                    "advertised sensor type " + type + " is NOT in the built catalog"
                    + " -- getDefaultSensor(" + type + ") will return null after"
                    + " hasSystemFeature said yes");
            }
        }
    }

    /** Loader-side factory; the constructor is package-private in real Android too. */
    public static SensorManager macapkNew() { return new SensorManager(); }

    // ---- the NDK side: ASensorManager / ASensorEventQueue -------------------
    //
    // A native game (Unity's Input.acceleration, Gameloft's engines, any
    // NativeActivity) reads sensors through libandroid's ASensor* API, not this
    // class. On a device both are views of ONE sensor service; here the bionic
    // shim (runtime/translator/src/bionic_shim_sensors.cpp) mirrors THIS
    // catalog and asks THIS class for every event's values, so the NDK reports
    // exactly what a Java listener would -- the at-rest pose, or the Tilt
    // Control pose while it is on. docs/tilt-control_2026-10-02.md.
    private static SensorManager sNative;
    private static synchronized SensorManager macapkNativeInstance() {
        if (sNative == null) sNative = new SensorManager();
        return sNative;
    }

    /** The catalog for the shim: {type, minDelayUs, reportingMode} per sensor,
     *  in catalog order (the order getSensorList(TYPE_ALL) returns). */
    public static int[] macapkNativeCatalog() {
        List<Sensor> l = macapkNativeInstance().mSensors;
        int[] out = new int[l.size() * 3];
        for (int i = 0; i < l.size(); i++) {
            Sensor s = l.get(i);
            out[i * 3] = s.getType();
            out[i * 3 + 1] = s.getMinDelay();
            out[i * 3 + 2] = s.getReportingMode();
        }
        return out;
    }

    /** {maxRange, resolution, power} of the sensor at catalog index i. */
    public static float[] macapkNativeFloats(int i) {
        Sensor s = macapkNativeInstance().mSensors.get(i);
        return new float[] { s.getMaximumRange(), s.getResolution(), s.getPower() };
    }

    /** Name (which == 0) or string type (which == 1) of catalog index i. */
    public static String macapkNativeString(int i, int which) {
        Sensor s = macapkNativeInstance().mSensors.get(i);
        return which == 0 ? s.getName() : s.getStringType();
    }

    /** One event for `type`, exactly what makeEvent gives a Java listener now:
     *  the values into `out` (returns how many; 0 when this device has no such
     *  sensor) and the event timestamp (elapsedRealtimeNanos base) in ts[0]. */
    public static int macapkNativeSample(int type, float[] out, long[] ts) {
        SensorManager sm = macapkNativeInstance();
        Sensor s = sm.getDefaultSensor(type);
        if (s == null) return 0;
        SensorEvent ev = sm.makeEvent(s, System.nanoTime());
        if (ev == null) return 0;
        int n = Math.min(out.length, ev.values.length);
        System.arraycopy(ev.values, 0, out, 0, n);
        if (ts != null && ts.length > 0) ts[0] = ev.timestamp;
        return n;
    }

    public Sensor getDefaultSensor(int type) {
        for (Sensor s : mSensors) if (s.getType() == type) return s;
        return null;
    }

    public Sensor getDefaultSensor(int type, boolean wakeUp) {
        // Mac.apk synthesizes only non-wakeup sensors.
        if (wakeUp) return null;
        return getDefaultSensor(type);
    }

    public List<Sensor> getSensorList(int type) {
        if (type == Sensor.TYPE_ALL) {
            return Collections.unmodifiableList(new ArrayList<>(mSensors));
        }
        List<Sensor> out = new ArrayList<>();
        for (Sensor s : mSensors) if (s.getType() == type) out.add(s);
        return Collections.unmodifiableList(out);
    }

    public List<Sensor> getDynamicSensorList(int type) { return Collections.emptyList(); }

    /** The `android.hardware.sensor.*` PackageManager features this device may
     *  honestly claim, DERIVED from the catalog above rather than hand-listed.
     *
     *  Why derived: MacApkPackageManager used to keep its own copy of this list,
     *  and on 2026-08-23 the catalog lost MAGNETIC_FIELD / LIGHT / PROXIMITY /
     *  PRESSURE / AMBIENT_TEMPERATURE / RELATIVE_HUMIDITY while that copy kept
     *  claiming all six. hasSystemFeature("...sensor.compass") answered true and
     *  getDefaultSensor(TYPE_MAGNETIC_FIELD) then returned null -- an app takes
     *  its has-compass branch and NPEs, which is the forbidden fourth rung
     *  (advertising a capability we cannot honour) reintroduced by two commits
     *  that each obeyed the coherence rule against a different snapshot of the
     *  other file. Same reasoning, and same cure, as getLegacySensorMask below:
     *  one answer, computed from the catalog, cannot drift from it.
     *
     *  Static because PackageManager answers before any Context-scoped
     *  SensorManager exists; it reads the same table the instance ctor does. */
    public static java.util.Set<String> macapkSensorFeatures() {
        java.util.LinkedHashSet<String> f = new java.util.LinkedHashSet<>();
        for (int type : MACAPK_CATALOG_TYPES) {
            String name = macapkFeatureForType(type);
            if (name != null) f.add(name);
        }
        return f;
    }

    /** AOSP PackageManager FEATURE_SENSOR_* <-> Sensor.TYPE_* mapping. Only the
     *  types that HAVE a feature string appear; TYPE_GRAVITY,
     *  TYPE_LINEAR_ACCELERATION, TYPE_ROTATION_VECTOR, TYPE_GAME_ROTATION_VECTOR
     *  and TYPE_ORIENTATION have none upstream, so they contribute nothing. */
    private static String macapkFeatureForType(int type) {
        switch (type) {
            case Sensor.TYPE_ACCELEROMETER:       return "android.hardware.sensor.accelerometer";
            case Sensor.TYPE_GYROSCOPE:           return "android.hardware.sensor.gyroscope";
            case Sensor.TYPE_MAGNETIC_FIELD:      return "android.hardware.sensor.compass";
            case Sensor.TYPE_LIGHT:               return "android.hardware.sensor.light";
            case Sensor.TYPE_PROXIMITY:           return "android.hardware.sensor.proximity";
            case Sensor.TYPE_PRESSURE:            return "android.hardware.sensor.barometer";
            case Sensor.TYPE_AMBIENT_TEMPERATURE: return "android.hardware.sensor.ambient_temperature";
            case Sensor.TYPE_RELATIVE_HUMIDITY:   return "android.hardware.sensor.relative_humidity";
            case Sensor.TYPE_HEART_RATE:          return "android.hardware.sensor.heartrate";
            case Sensor.TYPE_STEP_COUNTER:        return "android.hardware.sensor.stepcounter";
            case Sensor.TYPE_STEP_DETECTOR:       return "android.hardware.sensor.stepdetector";
            default: return null;
        }
    }

    /** The one place the catalog's membership is written down. The instance
     *  constructor builds its Sensor objects from these same types, so adding or
     *  removing a sensor moves BOTH the catalog and the advertised features. */
    static final int[] MACAPK_CATALOG_TYPES = {};

    // ---- the ORIGINAL (API 1) SensorListener API -------------------------
    //
    // Deprecated upstream since API 3 and still shipped by a great many older
    // apps. It is NOT a thin alias for the modern API -- different sensor
    // identifiers, negated axes, an extra yaw filter -- so it is bridged for
    // real through LegacySensorManager rather than stubbed. See that file's
    // header for what would break if it were not.
    //
    // AOSP lazily creates the bridge (getLegacySensorManager()); same here, so
    // an app that never touches the legacy API never allocates it.
    private LegacySensorManager mLegacySensorManager;

    private LegacySensorManager getLegacySensorManager() {
        synchronized (mSensors) {
            if (mLegacySensorManager == null) {
                mLegacySensorManager = new LegacySensorManager(this);
            }
            return mLegacySensorManager;
        }
    }

    /** AOSP SensorManager.java:405. A bitmask of SENSOR_* legacy bits, derived
     *  from the sensors this device actually reports -- so it stays consistent
     *  with getSensorList() by construction rather than being a second,
     *  hand-maintained answer that could drift from it. */
    @Deprecated
    public int getSensors() {
        return getLegacySensorManager().getSensors();
    }

    @Deprecated
    public boolean registerListener(SensorListener listener, int sensors) {
        return registerListener(listener, sensors, SENSOR_DELAY_NORMAL);
    }

    @Deprecated
    public boolean registerListener(SensorListener listener, int sensors, int rate) {
        return getLegacySensorManager().registerListener(listener, sensors, rate);
    }

    /** AOSP :642 -- note the mask: SENSOR_ALL does NOT include
     *  SENSOR_ORIENTATION_RAW (it is 0x7F, bit 7 is RAW), so unregistering
     *  "everything" has to name RAW explicitly or a raw-orientation
     *  registration would survive. */
    @Deprecated
    public void unregisterListener(SensorListener listener) {
        unregisterListener(listener, SENSOR_ALL | SENSOR_ORIENTATION_RAW);
    }

    @Deprecated
    public void unregisterListener(SensorListener listener, int sensors) {
        getLegacySensorManager().unregisterListener(listener, sensors);
    }

    // ---- dynamic sensors -------------------------------------------------
    //
    // "Dynamic" means hot-pluggable: a sensor that appears after boot, e.g. one
    // inside a plugged-in USB peripheral or a connected game controller. A Mac
    // exposes no such HAL through this runtime, so the catalog never changes
    // and a registered callback is never invoked -- which is precisely what
    // isDynamicSensorDiscoverySupported() reporting false MEANS, and an app is
    // documented to check that first.

    /** AOSP SensorManager.java:1062 derives this from the presence of a
     *  TYPE_DYNAMIC_SENSOR_META sensor rather than from a flag. Mirrored, so
     *  the day a dynamic-sensor source exists the answer follows the catalog
     *  automatically instead of needing this line changed too. */
    public boolean isDynamicSensorDiscoverySupported() {
        return getSensorList(Sensor.TYPE_DYNAMIC_SENSOR_META).size() > 0;
    }

    public void registerDynamicSensorCallback(DynamicSensorCallback callback) {
        registerDynamicSensorCallback(callback, null);
    }

    /** The callback is held rather than dropped so that unregister() can be
     *  symmetric and so an app that registers twice does not accumulate. It is
     *  never invoked: nothing here can connect or disconnect a sensor. */
    public void registerDynamicSensorCallback(DynamicSensorCallback callback,
            android.os.Handler handler) {
        if (callback == null) {
            throw new IllegalArgumentException("callback cannot be null");
        }
        mDynamicSensorCallbacks.addIfAbsent(callback);
    }

    public void unregisterDynamicSensorCallback(DynamicSensorCallback callback) {
        if (callback == null) return;
        mDynamicSensorCallbacks.remove(callback);
    }

    private final CopyOnWriteArrayList<DynamicSensorCallback> mDynamicSensorCallbacks =
            new CopyOnWriteArrayList<>();

    /** AOSP SensorManager.DynamicSensorCallback. */
    public abstract static class DynamicSensorCallback {
        public void onDynamicSensorConnected(Sensor sensor) { }
        public void onDynamicSensorDisconnected(Sensor sensor) { }
    }

    // ---- direct channels -------------------------------------------------
    //
    // A direct channel is shared memory a sensor HAL writes samples into with
    // no Binder hop. There is no HAL here -- the values this class reports are
    // synthesised in Java -- so there is nothing on the other end of a buffer
    // to write into it.
    //
    // null is AOSP's own documented failure return ("null on failure"), so an
    // app already branches on it and falls back to registerListener, which
    // DOES work. Returning a live-looking SensorDirectChannel that no producer
    // ever writes to would be the worse failure: the app would poll a buffer
    // that never changes and conclude the device is perfectly still, with no
    // error anywhere to explain it.
    //
    // The argument checks are AOSP's (SensorManager.java:943/:965 dereference
    // the argument immediately, and both parameters are @NonNull).

    public SensorDirectChannel createDirectChannel(android.os.MemoryFile mem) {
        if (mem == null) {
            throw new NullPointerException("MemoryFile cannot be null");
        }
        return null;
    }

    public SensorDirectChannel createDirectChannel(HardwareBuffer mem) {
        if (mem == null) {
            throw new NullPointerException("HardwareBuffer cannot be null");
        }
        return null;
    }

    // --- listener registration + synthetic delivery ----------------------
    private static final class Registration {
        final SensorEventListener listener;
        final Sensor sensor;
        final int rateUs;
        /** The thread callbacks run on. AOSP keeps one Looper per listener,
         *  owned by that listener's SensorEventQueue; we keep a Handler bound
         *  to the same Looper and post through it. */
        final android.os.Handler handler;
        long lastDeliveryNanos;
        /** Last accuracy handed to onAccuracyChanged. AOSP tracks this in a
         *  SparseIntArray whose absent-key read is 0, and fires the callback
         *  only when the incoming accuracy differs -- so it lands once, on the
         *  delivery thread, just before the first onSensorChanged. */
        int lastAccuracy;
        /** At most one event in flight per registration. The real event queue
         *  is a fixed-size ring buffer: when the consumer thread falls behind,
         *  the HAL drops samples rather than growing an unbounded backlog that
         *  would later replay as one burst. Dropping matches that. */
        final java.util.concurrent.atomic.AtomicBoolean inFlight =
            new java.util.concurrent.atomic.AtomicBoolean();
        Registration(SensorEventListener l, Sensor s, int rateUs, android.os.Handler h) {
            this.listener = l; this.sensor = s; this.rateUs = rateUs; this.handler = h;
        }
    }

    private final CopyOnWriteArrayList<Registration> mRegs = new CopyOnWriteArrayList<>();
    private volatile java.util.Timer mTimer;

    public boolean registerListener(SensorEventListener l, Sensor s, int rate) {
        return registerListener(l, s, rate, null);
    }

    public boolean registerListener(SensorEventListener l, Sensor s, int rate, android.os.Handler h) {
        if (l == null || s == null) return false;
        // AOSP SystemSensorManager.registerListenerImpl:
        //   Looper looper = (handler != null) ? handler.getLooper() : mMainLooper;
        // i.e. the supplied Handler's thread, else the app's MAIN thread --
        // never "whichever thread happened to call registerListener" and never
        // an internal worker. mMainLooper is always non-null there; the null
        // guard below covers a Handler built on a thread with no Looper, which
        // would otherwise leave us with nowhere to dispatch.
        android.os.Looper looper = (h != null) ? h.getLooper() : null;
        if (looper == null) looper = android.os.Looper.getMainLooper();
        mRegs.add(new Registration(l, s, samplingPeriodUs(rate), new android.os.Handler(looper)));
        // No onAccuracyChanged here: AOSP does not fire one at registration.
        // It arrives on the delivery thread immediately before the first event
        // (see deliverTick), which is what listeners are written against.
        ensureTimer();
        return true;
    }

    public boolean registerListener(SensorEventListener l, Sensor s, int rate, int maxReportLatencyUs) {
        return registerListener(l, s, rate, (android.os.Handler) null);
    }

    public boolean registerListener(SensorEventListener l, Sensor s, int rate,
                                    int maxReportLatencyUs, android.os.Handler h) {
        return registerListener(l, s, rate, h);
    }

    public void unregisterListener(SensorEventListener l) {
        mRegs.removeIf(r -> r.listener == l);
    }

    public void unregisterListener(SensorEventListener l, Sensor s) {
        mRegs.removeIf(r -> r.listener == l && r.sensor == s);
    }

    // AOSP accepts SENSOR_DELAY_* tokens OR a microsecond period. Map the
    // named rates to typical microsecond periods; pass through raw us values.
    private static int samplingPeriodUs(int rate) {
        switch (rate) {
            case SENSOR_DELAY_FASTEST: return 0;
            case SENSOR_DELAY_GAME:    return 20000;
            case SENSOR_DELAY_UI:      return 66667;
            case SENSOR_DELAY_NORMAL:  return 200000;
            default:                   return rate; // already a us period
        }
    }

    private synchronized void ensureTimer() {
        if (mTimer != null) return;
        java.util.Timer t = new java.util.Timer("macapk-sensor", true);
        // Tick at 60Hz; per-registration rate gating decides who fires.
        t.scheduleAtFixedRate(new java.util.TimerTask() {
            @Override public void run() { deliverTick(); }
        }, 0L, 16L);
        mTimer = t;
    }

    private void deliverTick() {
        long now = System.nanoTime();
        for (final Registration r : mRegs) {
            long periodNs = Math.max(r.rateUs, 16000) * 1000L;
            if (now - r.lastDeliveryNanos < periodNs) continue;
            // Consumer still holds the previous sample: drop this one rather
            // than queue it (fixed-ring-buffer semantics, see Registration).
            if (!r.inFlight.compareAndSet(false, true)) continue;
            r.lastDeliveryNanos = now;
            final SensorEvent ev = makeEvent(r.sensor, now);
            if (ev == null) { r.inFlight.set(false); continue; }
            // Synthesis happens on this timer thread; DELIVERY happens on the
            // registration's Looper, per AOSP's one-Looper-per-listener rule.
            // A fresh SensorEvent per tick is what makes that safe -- nothing
            // is mutated after the post.
            r.handler.post(() -> {
                try {
                    // AOSP ignores events still queued for a listener that has
                    // since unregistered (SensorEventQueue.dispatchSensorEvent
                    // returns early once the listener's entry is gone).
                    if (!mRegs.contains(r)) return;
                    if (ev.accuracy >= 0 && r.lastAccuracy != ev.accuracy) {
                        r.lastAccuracy = ev.accuracy;
                        r.listener.onAccuracyChanged(ev.sensor, ev.accuracy);
                    }
                    r.listener.onSensorChanged(ev);
                } finally {
                    // Exceptions from app callbacks propagate to the looper,
                    // which logs them (Android would let them reach the
                    // uncaught handler); swallowing here would hide app bugs.
                    r.inFlight.set(false);
                }
            });
        }
    }

    // Build an event for the given sensor. The catalog is empty, so there is
    // no sensor behind any type and none gets an event.
    //
    // The owner's decision (November 2026): no synthetic accelerometer. The
    // made-up "phone held upright" gravity vector this file used to report
    // (and the gravity, linear-acceleration, rotation-vector and orientation
    // sensors derived from it) is gone. A PC has no accelerometer, so this
    // device reports none, as an Android device without one does:
    // getDefaultSensor(TYPE_ACCELEROMETER) is null, the list has none, and
    // PackageManager has no android.hardware.sensor.accelerometer (derived from
    // the catalog). The
    // gyroscope that reported 0,0,0 went the same way (the owner's decision,
    // November 2026): a PC has none.
    private SensorEvent makeEvent(Sensor s, long nowNanos) {
        float[] values = readingOf(s);
        if (values == null) return null;
        SensorEvent ev = SensorEvent.macapkNew(values.length);
        System.arraycopy(values, 0, ev.values, 0, values.length);
        ev.sensor = s;
        ev.accuracy = SENSOR_STATUS_ACCURACY_HIGH;
        // AOSP frameworks/base/core/java/android/hardware/SensorEvent.java :834-836:
        // "the time in nanoseconds ... in the same time base as
        // SystemClock#elapsedRealtimeNanos()".
        ev.timestamp = android.os.SystemClock.elapsedRealtimeNanos();
        return ev;
    }

    /** The sensor's current reading, or null when nothing measures it. Every
     *  type is null now: the catalog is empty. A sensor backed by Windows'
     *  sensor API answers here with what the hardware reads. */
    private static float[] readingOf(Sensor s) {
        return null;
    }

    // --- TriggerEventListener (one-shot sensors) -------------------------
    public boolean requestTriggerSensor(TriggerEventListener listener, Sensor sensor) {
        // Mac.apk's flat-and-still device never satisfies a significant-motion
        // trigger, so the request is accepted but never fires. Honest no-op.
        return listener != null && sensor != null;
    }

    public boolean cancelTriggerSensor(TriggerEventListener listener, Sensor sensor) {
        return listener != null && sensor != null;
    }

    /** AOSP: onFlushCompleted "is called after all the events in the batch have
     *  been delivered"; returns "false if no sensor is previously registered for
     *  this listener". We returned true for a never-registered listener and NEVER
     *  fired the callback, so an app using the standard batch-drain idiom (flush,
     *  then block until onFlushCompleted) hung.
     *
     *  Our FIFO is size 0, so there is nothing to drain and the flush completes
     *  immediately -- the same reasoning LocationManager.requestFlush already
     *  uses one package over. */
    public boolean flush(SensorEventListener listener) {
        if (listener == null) return false;
        boolean any = false;
        final boolean l2able = listener instanceof SensorEventListener2;
        // mRegs is copy-on-write, so iterating it needs no lock and a concurrent
        // unregister cannot break this walk.
        for (final Registration reg : mRegs) {
            if (reg.listener != listener) continue;
            any = true;
            if (!l2able) continue;
            final SensorEventListener2 l2 = (SensorEventListener2) listener;
            final Sensor sn = reg.sensor;
            Runnable task = new Runnable() { @Override public void run() {
                try { l2.onFlushCompleted(sn); } catch (Throwable ignored) { }
            } };
            // Deliver on the handler this listener registered with, matching the
            // event path above -- not on whatever thread called flush().
            try { reg.handler.post(task); } catch (Throwable t) { task.run(); }
        }
        return any;   // AOSP: false when nothing was registered for this listener
    }

    // === Static math utilities (verbatim AOSP frameworks/base) ===========

    public static boolean getRotationMatrix(float[] R, float[] I,
            float[] gravity, float[] geomagnetic) {
        float Ax = gravity[0];
        float Ay = gravity[1];
        float Az = gravity[2];

        final float normsqA = (Ax * Ax + Ay * Ay + Az * Az);
        final float g = 9.81f;
        final float freeFallGravitySquared = 0.01f * g * g;
        if (normsqA < freeFallGravitySquared) {
            // gravity less than 10% of normal value
            return false;
        }

        final float Ex = geomagnetic[0];
        final float Ey = geomagnetic[1];
        final float Ez = geomagnetic[2];
        float Hx = Ey * Az - Ez * Ay;
        float Hy = Ez * Ax - Ex * Az;
        float Hz = Ex * Ay - Ey * Ax;
        final float normH = (float) Math.sqrt(Hx * Hx + Hy * Hy + Hz * Hz);

        if (normH < 0.1f) {
            // device is close to free fall (or in space?), or close to
            // magnetic north pole. Typical values are  > 100.
            return false;
        }
        final float invH = 1.0f / normH;
        Hx *= invH;
        Hy *= invH;
        Hz *= invH;
        final float invA = 1.0f / (float) Math.sqrt(Ax * Ax + Ay * Ay + Az * Az);
        Ax *= invA;
        Ay *= invA;
        Az *= invA;
        final float Mx = Ay * Hz - Az * Hy;
        final float My = Az * Hx - Ax * Hz;
        final float Mz = Ax * Hy - Ay * Hx;
        if (R != null) {
            if (R.length == 9) {
                R[0] = Hx;     R[1] = Hy;     R[2] = Hz;
                R[3] = Mx;     R[4] = My;     R[5] = Mz;
                R[6] = Ax;     R[7] = Ay;     R[8] = Az;
            } else if (R.length == 16) {
                R[0]  = Hx;    R[1]  = Hy;    R[2]  = Hz;   R[3]  = 0;
                R[4]  = Mx;    R[5]  = My;    R[6]  = Mz;   R[7]  = 0;
                R[8]  = Ax;    R[9]  = Ay;    R[10] = Az;   R[11] = 0;
                R[12] = 0;     R[13] = 0;     R[14] = 0;    R[15] = 1;
            }
        }
        if (I != null) {
            // compute the inclination matrix by projecting the geomagnetic
            // vector onto the Z (gravity) and X (horizontal component
            // of geomagnetic vector) axes.
            final float invE = 1.0f / (float) Math.sqrt(Ex * Ex + Ey * Ey + Ez * Ez);
            final float c = (Ex * Mx + Ey * My + Ez * Mz) * invE;
            final float s = (Ex * Ax + Ey * Ay + Ez * Az) * invE;
            if (I.length == 9) {
                I[0] = 1;     I[1] = 0;     I[2] = 0;
                I[3] = 0;     I[4] = c;     I[5] = s;
                I[6] = 0;     I[7] = -s;     I[8] = c;
            } else if (I.length == 16) {
                I[0] = 1;     I[1] = 0;     I[2] = 0;
                I[4] = 0;     I[5] = c;     I[6] = s;
                I[8] = 0;     I[9] = -s;     I[10] = c;
                I[3] = I[7] = I[11] = I[12] = I[13] = I[14] = 0;
                I[15] = 1;
            }
        }
        return true;
    }

    public static float getInclination(float[] I) {
        if (I.length == 9) {
            return (float) Math.atan2(I[5], I[4]);
        } else {
            return (float) Math.atan2(I[6], I[5]);
        }
    }

    public static boolean remapCoordinateSystem(float[] inR, int X, int Y, float[] outR) {
        if (inR == outR) {
            final float[] temp = sTempMatrix;
            synchronized (temp) {
                // we don't expect to have a lot of contention
                if (remapCoordinateSystemImpl(inR, X, Y, temp)) {
                    final int size = outR.length;
                    for (int i = 0; i < size; i++) {
                        outR[i] = temp[i];
                    }
                    return true;
                }
            }
        }
        return remapCoordinateSystemImpl(inR, X, Y, outR);
    }

    private static boolean remapCoordinateSystemImpl(float[] inR, int X, int Y, float[] outR) {
        final int length = outR.length;
        if (inR.length != length) {
            return false;   // invalid parameter
        }
        if ((X & 0x7C) != 0 || (Y & 0x7C) != 0) {
            return false;   // invalid parameter
        }
        if (((X & 0x3) == 0) || ((Y & 0x3) == 0)) {
            return false;   // no axis specified
        }
        if ((X & 0x3) == (Y & 0x3)) {
            return false;   // same axis specified
        }

        // Z is "the other" axis, its sign is either +/- sign(X)*sign(Y)
        // this can be calculated by exclusive-or'ing X and Y; except for
        // the sign inversion (+/-) which is calculated below.
        int Z = X ^ Y;

        // extract the axis (remove the sign), offset in the range 0 to 2.
        final int x = (X & 0x3) - 1;
        final int y = (Y & 0x3) - 1;
        final int z = (Z & 0x3) - 1;

        // compute the sign of Z (whether it needs to be inverted)
        final int axis_y = (z + 1) % 3;
        final int axis_z = (z + 2) % 3;
        if (((x ^ axis_y) | (y ^ axis_z)) != 0) {
            Z ^= 0x80;
        }

        final boolean sx = (X >= 0x80);
        final boolean sy = (Y >= 0x80);
        final boolean sz = (Z >= 0x80);

        // Perform R * r, in avoiding actual muls and adds.
        final int rowLength = ((length == 16) ? 4 : 3);
        for (int j = 0; j < 3; j++) {
            final int offset = j * rowLength;
            for (int i = 0; i < 3; i++) {
                if (x == i)   outR[offset + i] = sx ? -inR[offset + 0] : inR[offset + 0];
                if (y == i)   outR[offset + i] = sy ? -inR[offset + 1] : inR[offset + 1];
                if (z == i)   outR[offset + i] = sz ? -inR[offset + 2] : inR[offset + 2];
            }
        }
        if (length == 16) {
            outR[3] = outR[7] = outR[11] = outR[12] = outR[13] = outR[14] = 0;
            outR[15] = 1;
        }
        return true;
    }

    public static float[] getOrientation(float[] R, float[] values) {
        /*
         * 4x4 (length=16) case:
         *   /  R[ 0]   R[ 1]   R[ 2]   0  \
         *   |  R[ 4]   R[ 5]   R[ 6]   0  |
         *   |  R[ 8]   R[ 9]   R[10]   0  |
         *   \      0       0       0   1  /
         *
         * 3x3 (length=9) case:
         *   /  R[ 0]   R[ 1]   R[ 2]  \
         *   |  R[ 3]   R[ 4]   R[ 5]  |
         *   \  R[ 6]   R[ 7]   R[ 8]  /
         */
        if (R.length == 9) {
            values[0] = (float) Math.atan2(R[1], R[4]);
            values[1] = (float) Math.asin(-R[7]);
            values[2] = (float) Math.atan2(-R[6], R[8]);
        } else {
            values[0] = (float) Math.atan2(R[1], R[5]);
            values[1] = (float) Math.asin(-R[9]);
            values[2] = (float) Math.atan2(-R[8], R[10]);
        }

        return values;
    }

    public static float getAltitude(float p0, float p) {
        final float coef = 1.0f / 5.255f;
        return 44330.0f * (1.0f - (float) Math.pow(p / p0, coef));
    }

    public static void getAngleChange(float[] angleChange, float[] R, float[] prevR) {
        float rd1 = 0, rd4 = 0, rd6 = 0, rd7 = 0, rd8 = 0;
        float ri0 = 0, ri1 = 0, ri2 = 0, ri3 = 0, ri4 = 0, ri5 = 0, ri6 = 0, ri7 = 0, ri8 = 0;
        float pri0 = 0, pri1 = 0, pri2 = 0, pri3 = 0, pri4 = 0;
        float pri5 = 0, pri6 = 0, pri7 = 0, pri8 = 0;

        if (R.length == 9) {
            ri0 = R[0];
            ri1 = R[1];
            ri2 = R[2];
            ri3 = R[3];
            ri4 = R[4];
            ri5 = R[5];
            ri6 = R[6];
            ri7 = R[7];
            ri8 = R[8];
        } else if (R.length == 16) {
            ri0 = R[0];
            ri1 = R[1];
            ri2 = R[2];
            ri3 = R[4];
            ri4 = R[5];
            ri5 = R[6];
            ri6 = R[8];
            ri7 = R[9];
            ri8 = R[10];
        }

        if (prevR.length == 9) {
            pri0 = prevR[0];
            pri1 = prevR[1];
            pri2 = prevR[2];
            pri3 = prevR[3];
            pri4 = prevR[4];
            pri5 = prevR[5];
            pri6 = prevR[6];
            pri7 = prevR[7];
            pri8 = prevR[8];
        } else if (prevR.length == 16) {
            pri0 = prevR[0];
            pri1 = prevR[1];
            pri2 = prevR[2];
            pri3 = prevR[4];
            pri4 = prevR[5];
            pri5 = prevR[6];
            pri6 = prevR[8];
            pri7 = prevR[9];
            pri8 = prevR[10];
        }

        // calculate the parts of the rotation difference matrix we need
        // rd[i][j] = pri[0][i] * ri[0][j] + pri[1][i] * ri[1][j] + pri[2][i] * ri[2][j];

        rd1 = pri0 * ri1 + pri3 * ri4 + pri6 * ri7; // rd[0][1]
        rd4 = pri1 * ri1 + pri4 * ri4 + pri7 * ri7; // rd[1][1]
        rd6 = pri2 * ri0 + pri5 * ri3 + pri8 * ri6; // rd[2][0]
        rd7 = pri2 * ri1 + pri5 * ri4 + pri8 * ri7; // rd[2][1]
        rd8 = pri2 * ri2 + pri5 * ri5 + pri8 * ri8; // rd[2][2]

        angleChange[0] = (float) Math.atan2(rd1, rd4);
        angleChange[1] = (float) Math.asin(-rd7);
        angleChange[2] = (float) Math.atan2(-rd6, rd8);
    }

    public static void getRotationMatrixFromVector(float[] R, float[] rotationVector) {

        float q0;
        float q1 = rotationVector[0];
        float q2 = rotationVector[1];
        float q3 = rotationVector[2];

        if (rotationVector.length >= 4) {
            q0 = rotationVector[3];
        } else {
            q0 = 1 - q1 * q1 - q2 * q2 - q3 * q3;
            q0 = (q0 > 0) ? (float) Math.sqrt(q0) : 0;
        }

        float sq_q1 = 2 * q1 * q1;
        float sq_q2 = 2 * q2 * q2;
        float sq_q3 = 2 * q3 * q3;
        float q1_q2 = 2 * q1 * q2;
        float q3_q0 = 2 * q3 * q0;
        float q1_q3 = 2 * q1 * q3;
        float q2_q0 = 2 * q2 * q0;
        float q2_q3 = 2 * q2 * q3;
        float q1_q0 = 2 * q1 * q0;

        if (R.length == 9) {
            R[0] = 1 - sq_q2 - sq_q3;
            R[1] = q1_q2 - q3_q0;
            R[2] = q1_q3 + q2_q0;

            R[3] = q1_q2 + q3_q0;
            R[4] = 1 - sq_q1 - sq_q3;
            R[5] = q2_q3 - q1_q0;

            R[6] = q1_q3 - q2_q0;
            R[7] = q2_q3 + q1_q0;
            R[8] = 1 - sq_q1 - sq_q2;
        } else if (R.length == 16) {
            R[0] = 1 - sq_q2 - sq_q3;
            R[1] = q1_q2 - q3_q0;
            R[2] = q1_q3 + q2_q0;
            R[3] = 0.0f;

            R[4] = q1_q2 + q3_q0;
            R[5] = 1 - sq_q1 - sq_q3;
            R[6] = q2_q3 - q1_q0;
            R[7] = 0.0f;

            R[8] = q1_q3 - q2_q0;
            R[9] = q2_q3 + q1_q0;
            R[10] = 1 - sq_q1 - sq_q2;
            R[11] = 0.0f;

            R[12] = R[13] = R[14] = 0.0f;
            R[15] = 1.0f;
        }
    }

    public static void getQuaternionFromVector(float[] Q, float[] rv) {
        if (rv.length >= 4) {
            Q[0] = rv[3];
        } else {
            Q[0] = 1 - rv[0] * rv[0] - rv[1] * rv[1] - rv[2] * rv[2];
            Q[0] = (Q[0] > 0) ? (float) Math.sqrt(Q[0]) : 0;
        }
        Q[1] = rv[0];
        Q[2] = rv[1];
        Q[3] = rv[2];
    }
}
