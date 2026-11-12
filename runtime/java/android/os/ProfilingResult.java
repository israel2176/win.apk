package android.os;
/** The result of a requestProfiling call. API 35. Mac.apk runs no profiler, so
 *  no result is ever delivered; the type exists for the Consumer<ProfilingResult>
 *  callbacks apps register.
 *  Source: frameworks/base/core/java/android/os/ProfilingResult.java */
public final class ProfilingResult {
    public static final int ERROR_UNKNOWN = 0;
    ProfilingResult() { }
    public int getErrorCode() { return ERROR_UNKNOWN; }
    public String getErrorMessage() { return "profiling not supported"; }
    public String getResultFilePath() { return null; }
    public String getTag() { return null; }
    /** API 36: what set the profile off; none, for one the app asked for itself. */
    public int getTriggerType() { return ProfilingTrigger.TRIGGER_TYPE_NONE; }
}
