package android.icu.platform;

// Where AOSP's ICU4J (android.icu, taken into aosp-graphics.jar) finds its data. On a device that is the
// i18n and time zone modules' etc/icu folders, named by the ANDROID_I18N_ROOT and ANDROID_TZDATA_ROOT
// environment variables; here it is the ICU 72 data the graphics DLL reads too, bin\aosp\icu\icudt72l.dat,
// which the host names as -Dicu.data.path (runtime_main.cpp). A plain JVM whose natives are Robolectric's
// (windows/test/aospapp/run.sh, accessibility/run.sh) passes Robolectric's own ICU 68 data there, which
// ICU4J 72 cannot read, and names ICU 72's folder with ICU's own property for it,
// android.icu.impl.ICUBinary.dataPath, which comes first. ICUBinary reads every .dat file of the folders
// this gives. The same public members as AOSP's.
public class AndroidDataFiles {
    public static final String ANDROID_ROOT_ENV = "ANDROID_ROOT";
    public static final String ANDROID_I18N_ROOT_ENV = "ANDROID_I18N_ROOT";
    public static final String ANDROID_TZDATA_ROOT_ENV = "ANDROID_TZDATA_ROOT";

    public AndroidDataFiles() {}

    /** The folder of the ICU data file, or "" when none is named (ICU then finds no data). */
    private static String icuFolder() {
        String folder = System.getProperty("android.icu.impl.ICUBinary.dataPath");
        if (folder != null && !folder.isEmpty()) return folder;
        String file = System.getProperty("icu.data.path");
        if (file != null && !file.isEmpty()) {
            java.io.File f = new java.io.File(file);
            // Only ICU 72's data (android-all 14's ICU4J): another version's (a plain JVM's Robolectric
            // 68) is passed over rather than read as items of another format.
            if (f.isDirectory()) return f.getPath();
            if (f.getName().startsWith("icudt72") && f.getParent() != null) return f.getParent();
        }
        // Else where every staged runtime has it, beside this jar: <stage>\lib\aosp-graphics.jar and
        // <stage>\bin\aosp\icu (a plain JVM run on the stage's jars, CI's Java tests).
        try {
            java.security.CodeSource cs = AndroidDataFiles.class.getProtectionDomain().getCodeSource();
            if (cs != null && cs.getLocation() != null) {
                java.io.File jar = new java.io.File(cs.getLocation().toURI());
                java.io.File stage = jar.getParentFile() != null ? jar.getParentFile().getParentFile() : null;
                java.io.File dir = stage != null ? new java.io.File(stage, "bin/aosp/icu") : null;
                if (dir != null && dir.isDirectory()) return dir.getPath();
            }
        } catch (java.net.URISyntaxException | SecurityException e) {
            // No data then, as before.
        }
        return "";
    }

    /** A file of the time zone module's ICU folder: the same folder here (one data file holds all). */
    public static String getTimeZoneModuleIcuFile(String fileName) {
        return new java.io.File(icuFolder(), fileName).getPath();
    }

    /** A file of the i18n module's ICU folder: the same folder here. */
    public static String getI18nModuleIcuFile(String fileName) {
        return new java.io.File(icuFolder(), fileName).getPath();
    }

    /** The folders ICUBinary reads data from: the one that holds icudt72l.dat. */
    public static String generateIcuDataPath() {
        return icuFolder();
    }
}
