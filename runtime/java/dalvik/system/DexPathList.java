// dalvik.system.DexPathList -- ART's internal list of dex/native-lib elements
// that backs a BaseDexClassLoader (the `pathList` field). It is package-private
// in AOSP, but a number of plugin/hotfix frameworks (Tinker, RePlugin, etc.)
// reach it BY REFLECTION: they read BaseDexClassLoader.pathList, then mutate its
// `dexElements` / `nativeLibraryDirectories` arrays to inject patched code. A
// missing class makes those `Class.forName("dalvik.system.DexPathList")` /
// field-type lookups fail.
//
// AOSP shape we mirror (the fields the reflection-based hotfix tools name):
//   Object[] dexElements
//   List<File> nativeLibraryDirectories
//   plus String findLibrary(String) / getDexPaths() helpers.
//
// Mac.apk does not route real class loading through this object (BaseDexClassLoader
// is a URLClassLoader, so the live element list lives in URLClassLoader's
// internal ucp). This class therefore exists primarily so the TYPE resolves and
// the named fields are present for reflective probes; mutating its arrays does
// NOT redirect Mac.apk's loading (the hotfix would not take effect, but the
// reflective access succeeds instead of crashing the app at startup).
package dalvik.system;

import java.io.File;
import java.util.ArrayList;
import java.util.List;

public final class DexPathList {

    /** AOSP field name. Reflective hotfix tools index/replace this array. We
     *  start it empty (our real loading is URLClassLoader-backed). */
    Object[] dexElements = new Object[0];

    /** AOSP field name (list of dirs searched for native libs). */
    List<File> nativeLibraryDirectories = new ArrayList<>();

    /** The librarySearchPath the loader was created with: what findLibrary
     *  searches. Not the list above, which is only there to be read. */
    private NativeLibraryPath nativeLibraries = new NativeLibraryPath(null);

    /** AOSP-internal Element type; reflection-based tools sometimes construct
     *  these. We keep a minimal shape so getDeclaredFields succeeds. */
    static final class Element {
        final File path;
        Element(File path) { this.path = path; }
        @Override public String toString() {
            return "dex file \"" + path + "\"";
        }
    }

    DexPathList() {}

    DexPathList(ClassLoader definingContext, String dexPath,
                String librarySearchPath, File optimizedDirectory) {
        nativeLibraries = new NativeLibraryPath(librarySearchPath);
        nativeLibraryDirectories.addAll(nativeLibraries.directories());
        if (dexPath != null && !dexPath.isEmpty()) {
            List<Object> els = new ArrayList<>();
            for (String p : dexPath.split(File.pathSeparator)) {
                if (!p.isEmpty()) els.add(new Element(new File(p)));
            }
            dexElements = els.toArray(new Object[0]);
        }
    }

    /** AOSP DexPathList.addDexPath: append one element. */
    synchronized void addDexPath(String p) {
        Object[] n = java.util.Arrays.copyOf(dexElements, dexElements.length + 1);
        n[dexElements.length] = new Element(new File(p));
        dexElements = n;
    }

    /** AOSP: lib<libraryName>.so of the first native-library directory that
     *  has it, or null. Only the loader's own librarySearchPath is searched
     *  here (NativeLibraryPath); the app's lib dir, which AOSP's list also
     *  holds, is AppClassLoader.findLibrary's, and BaseDexClassLoader.findLibrary
     *  asks both. */
    public String findLibrary(String libraryName) {
        File f = findLibraryFile(libraryName);
        return f == null ? null : f.getPath();
    }

    /** The same as a file the host can open: a library of a "<zip>!/<dir>"
     *  entry is copied out of the zip. */
    File findLibraryFile(String libraryName) {
        return nativeLibraries.find(libraryName);
    }

    /** AOSP: the configured dex code paths, joined. */
    public String getDexPaths() {
        StringBuilder sb = new StringBuilder();
        for (Object e : dexElements) {
            if (sb.length() > 0) sb.append(File.pathSeparator);
            sb.append(e);
        }
        return sb.toString();
    }

    @Override
    public String toString() {
        return "DexPathList[" + getDexPaths() + ",nativeLibraryDirectories="
                + nativeLibraryDirectories + "]";
    }
}
