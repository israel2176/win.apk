// getIdentifier's cost and answers on AOSP's resources (aosp-resources.jar): LoadedPackage.FindEntryByName,
// which AospResRewrite points at PortFixes.findEntryByName, over the platform's own table
// (framework-res-34.apk, ~20k keys in an unsorted pool), checked against com.android.internal.R, the ids
// compiled for that same table. The port's own body decoded the whole key pool and walked every entry of
// the type for each name, milliseconds a lookup: an app whose R fields are looked up by name (~22k for a
// bank app) waited minutes for its first frame. Prints ALL PASSED.
//
//   java -cp aosp-resources.jar:<this test's classes> org.robolectric.res.android.FindEntryByNameTest \
//       framework-res-34.apk
//
// In the port's package, to load a table and call the package as AssetManager2 does.
package org.robolectric.res.android;

import java.io.InputStream;
import java.lang.reflect.Field;
import java.lang.reflect.Modifier;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.ArrayList;
import java.util.List;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

public class FindEntryByNameTest {
    static int failures;

    static void check(boolean ok, String what) {
        if (!ok) {
            failures++;
            if (failures <= 20) System.out.println("FAIL " + what);
        }
    }

    record Name(String type, String entry, int id) {}

    public static void main(String[] args) throws Exception {
        byte[] arsc;
        try (ZipFile z = new ZipFile(args[0])) {
            ZipEntry e = z.getEntry("resources.arsc");
            try (InputStream in = z.getInputStream(e)) { arsc = in.readAllBytes(); }
        }
        ByteBuffer buf = ByteBuffer.wrap(arsc).order(ByteOrder.LITTLE_ENDIAN);
        LoadedArsc table = LoadedArsc.Load(new StringPiece(buf, 0), null, true, false);
        LoadedArsc.LoadedPackage pkg = table.GetPackages().get(0);
        System.out.println("table: " + pkg.GetPackageName() + ", " + pkg.GetKeyStringPool().size()
                + " keys, " + pkg.GetTypeStringPool().size() + " types");

        // The oracle: every id com.android.internal.R has for the table (styles and styleables aside:
        // their field names stand for dotted names, or for no entry at all).
        List<Name> names = new ArrayList<>();
        for (Class<?> c : Class.forName("com.android.internal.R").getDeclaredClasses()) {
            String type = c.getSimpleName();
            if (type.equals("style") || type.equals("styleable")) continue;
            for (Field f : c.getDeclaredFields()) {
                if (!Modifier.isStatic(f.getModifiers()) || f.getType() != int.class) continue;
                names.add(new Name(type, f.getName(), f.getInt(null)));
            }
        }
        System.out.println("names: " + names.size());

        long t0 = System.nanoTime();
        int first = pkg.FindEntryByName("string", "ok");
        long firstNs = System.nanoTime() - t0;
        check(first != 0 && (first | 0x01000000) == com.android.internal.R.string.ok, "string/ok");

        int found = 0;
        for (Name n : names) {
            int id = pkg.FindEntryByName(n.type, n.entry);
            if (id == 0 && n.type.equals("attr")) id = pkg.FindEntryByName("^attr-private", n.entry);
            check(id != 0 && (id & 0x00ffffff) == (n.id & 0x00ffffff),
                    n.type + "/" + n.entry + ": 0x" + Integer.toHexString(id) + ", R has 0x" + Integer.toHexString(n.id));
            if (id != 0) found++;
        }
        check(pkg.FindEntryByName("string", "no_such_name_anywhere") == 0, "a name the table lacks");
        check(pkg.FindEntryByName("no_such_type", "ok") == 0, "a type the table lacks");
        check(pkg.FindEntryByName("layout", "ok") == 0, "a name of another type");

        // Each name looked up again, five times over: hash hits once the package's names are indexed.
        int rounds = 5;
        long t1 = System.nanoTime();
        int sink = 0;
        for (int r = 0; r < rounds; r++) {
            for (Name n : names) sink += pkg.FindEntryByName(n.type, n.entry);
        }
        long ns = System.nanoTime() - t1;
        double perLookupUs = ns / 1000.0 / (rounds * (double) names.size());
        System.out.printf("first lookup (indexes the package): %.1f ms; %d lookups: %.1f ms, %.2f us each (%d)%n",
                firstNs / 1e6, rounds * names.size(), ns / 1e6, perLookupUs, sink == 42 ? 1 : 0);
        // The port's own scan took milliseconds per name on this table; a hash hit takes well under one
        // microsecond. The bound only catches a return of the scan.
        check(perLookupUs < 50, "lookups average " + perLookupUs + " us, the scan's cost");

        System.out.println(found + " of " + names.size() + " names found");
        if (failures > 0) {
            System.out.println(failures + " FAILED");
            System.exit(1);
        }
        System.out.println("ALL PASSED");
    }
}
