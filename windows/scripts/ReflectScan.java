// ReflectScan.java -- reflection over the framework's classes, as apps do it (CI's runtime tests, on
// the staged runtime; windows/README.md, "AOSP app layer"):
//
//   java -XX:+UnlockDiagnosticVMOptions -XX:-BytecodeVerificationRemote \
//        --patch-module java.base=<lib>/java_base_patch --patch-module jdk.unsupported=<lib>/jdk_unsupported_patch \
//        windows/scripts/ReflectScan.java <stage>/lib
//
// Apps and their SDKs look members up by reflection on the framework's classes all the time: an
// analytics SDK's getMethod("currentApplication") on android.app.ActivityThread, getDeclaredField on
// View, Window, ContextImpl, LoadedApk... HotSpot builds every declared method, field and constructor
// of the class for any of those lookups, so each type a member's descriptor names (parameters, return
// type, declared exceptions, a field's type) and the class's supertypes must load. On a device every
// framework class exists and that never fails; here a type the shipped class path lacks (a nested
// class of one of Win.apk's own, a class outside the jars' closure) throws NoClassDefFoundError out
// of the app's own code, and a class of Win.apk's own left beside AOSP's of the same outer class
// (RemoteViews$N implementing a RemoteViews$Action that AOSP's declares a class) throws
// IncompatibleClassChangeError.
//
// The class path is the one the host gives an app's JVM (host/runtime_main.cpp, jvm_options): the
// AOSP jars first, then the framework as shipped (without the classes they shadow), the loader and
// bcprov. Every class of the jars below is loaded (not initialized) and its declared methods, fields
// and constructors asked for, with the bytecode verifier off as the host runs it. Each failure is
// printed, one line per class; the exit code is 1 when there is any.

import java.io.File;
import java.net.URL;
import java.net.URLClassLoader;
import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.TreeMap;
import java.util.TreeSet;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

public class ReflectScan {
    /** The class path, in the host's order. */
    static final String[] CLASS_PATH = {
        "aosp-app.jar", "aosp-views.jar", "aosp-resources.jar", "aosp-graphics.jar", "android-framework.jar",
        "macapk-loader.jar", "bcprov-jdk18on-1.79.jar",
    };
    /** Whose classes are scanned: every jar of the framework an app's code can reach by name. */
    static final String[] SCANNED = {
        "aosp-app.jar", "aosp-views.jar", "aosp-resources.jar", "aosp-graphics.jar", "android-framework.jar",
    };

    public static void main(String[] args) throws Exception {
        if (args.length != 1) {
            System.err.println("usage: java ReflectScan.java <staged runtime>/lib");
            System.exit(2);
        }
        File lib = new File(args[0]);
        List<URL> urls = new ArrayList<>();
        for (String j : CLASS_PATH) {
            File f = new File(lib, j);
            if (!f.isFile()) {
                System.err.println("[reflect] no " + f);
                System.exit(2);
            }
            urls.add(f.toURI().toURL());
        }
        Set<String> names = new LinkedHashSet<>();
        for (String j : SCANNED) {
            try (ZipFile z = new ZipFile(new File(lib, j))) {
                for (ZipEntry e : Collections.list(z.entries())) {
                    String n = e.getName();
                    if (!n.endsWith(".class") || n.endsWith("module-info.class") || n.startsWith("META-INF/")) continue;
                    names.add(n.substring(0, n.length() - ".class".length()).replace('/', '.'));
                }
            }
        }
        // Failures by what was thrown, the classes that threw it.
        Map<String, Set<String>> failures = new TreeMap<>();
        ClassLoader platform = ClassLoader.getPlatformClassLoader();
        try (URLClassLoader loader = new URLClassLoader(urls.toArray(new URL[0]), platform)) {
            for (String n : names) {
                try {
                    Class<?> c = Class.forName(n, false, loader);
                    c.getDeclaredMethods();
                    c.getDeclaredFields();
                    c.getDeclaredConstructors();
                } catch (Throwable t) {
                    failures.computeIfAbsent(t.toString(), k -> new TreeSet<>()).add(n);
                }
            }
        }
        int classes = 0;
        for (Map.Entry<String, Set<String>> f : failures.entrySet()) {
            classes += f.getValue().size();
            for (String n : f.getValue()) System.out.println(n + ": " + f.getKey());
        }
        System.out.println("[reflect] " + names.size() + " classes of " + String.join(", ", SCANNED) + ": "
                + (classes == 0 ? "every one's declared methods, fields and constructors resolve"
                                : classes + " whose declared members do not resolve (" + failures.size()
                                          + " distinct errors, above)"));
        System.exit(classes == 0 ? 0 : 1);
    }
}
