/*
 * Mac.apk java.base overlay (--patch-module java.base). This REPLACES the JDK's
 * java.io.FileReader. It is byte-for-byte the JDK 21 public API (five
 * constructors delegating to a FileInputStream + the inherited InputStreamReader
 * behaviour) with ONE addition: it redirects a read of the Linux procfs paths
 * /proc/self/smaps and /proc/self/maps -- which do NOT exist on macOS -- to an
 * in-memory synthesized body.
 *
 * WHY: macOS has no /proc, and our guest /proc synthesis lives in the Bionic
 * open() shim, which the GUEST's native code routes through -- but a Java caller
 * opens through libjava's own open(), which bypasses that shim entirely. There
 * is no process-wide interpose (dyld does not apply a main-executable __interpose
 * to a later-dlopen'd libjava) and no DYLD_INSERT entitlement, so the JVM read
 * hit the absent real /proc. Instagram's page-size probe (X.0te) does
 * `new FileReader("/proc/self/smaps")` and scans for a `KernelPageSize:` line; on
 * failure it throws IllegalStateException("Could not get page size") ->
 * ExceptionInInitializerError -> System.exit(10), which blocked the logged-in
 * feed. This overlay makes that read succeed with the HOST page size (Apple
 * Silicon = 16 kB, what the guest's native memory is actually laid out on). Only
 * these two exact /proc/self paths are rewritten (in Win.apk only smaps: its
 * /proc/self/maps is a real file), every other file open is
 * untouched, so ordinary Java file I/O is unchanged. Native /proc readers keep
 * using the full live census through the Bionic shim.
 *
 * NO TEMP FILE (2026-09-24). The body used to be written to
 * File.createTempFile("macapk_jvm_smaps", ".txt") with deleteOnExit(), and that
 * hook never runs here: window close is _exit, harnesses SIGKILL, and HotSpot runs
 * with -Xrs, under which JDK 21's Terminator says shutdown hooks run only through
 * System.exit. So every JVM that read it leaked one file into the per-user TMPDIR
 * (28 seen). A FileReader exposes no fd and no path, so serving the same ASCII
 * bytes from a ByteArrayInputStream is invisible to every caller.
 * See docs/procfs-synth-tmp-lifetime_2026-09-24.md.
 */
package java.io;

import java.nio.charset.Charset;

public class FileReader extends InputStreamReader {

    public FileReader(String fileName) throws FileNotFoundException {
        super(macapkOpen(fileName));
    }

    public FileReader(File file) throws FileNotFoundException {
        super(macapkOpen(file));
    }

    public FileReader(FileDescriptor fd) {
        super(new FileInputStream(fd));
    }

    public FileReader(String fileName, Charset charset) throws IOException {
        super(macapkOpen(fileName), charset);
    }

    public FileReader(File file, Charset charset) throws IOException {
        super(macapkOpen(file), charset);
    }

    // ---- Mac.apk /proc redirect (see the class comment) --------------------

    // A minimal but well-formed smaps carrying exactly the fields a page-size
    // probe scans. One synthetic mapping is enough: the probe reads the first
    // KernelPageSize it finds. Pure ASCII, so the byte[] is built without going
    // through a Charset.
    // The page size is the host's: Apple Silicon's 16 kB, an x86_64 host's 4 kB
    // (Win.apk), what the guest's native memory is laid out on.
    private static final String PAGE_KB = "aarch64".equals(System.getProperty("os.arch")) ? "16" : " 4";
    private static final byte[] PROC_BODY = macapkAscii(
          "0000000000000000-0000000000010000 r--p 00000000 00:00 0 \n"
        + "Size:                 64 kB\n"
        + "KernelPageSize:       " + PAGE_KB + " kB\n"
        + "MMUPageSize:          " + PAGE_KB + " kB\n"
        + "Rss:                   0 kB\n"
        + "Pss:                   0 kB\n"
        + "VmFlags: rd mr mw me \n");

    private static byte[] macapkAscii(String s) {
        byte[] b = new byte[s.length()];
        for (int i = 0; i < b.length; i++) b[i] = (byte) s.charAt(i);
        return b;
    }

    private static boolean macapkIsProc(String name) {
        // Constant-first equals: a null name is NOT a /proc path, so it falls
        // through to FileInputStream and throws the JDK's own NullPointerException.
        if ("/proc/self/smaps".equals(name)) return true;
        // Win.apk (winapk-runtime sets winapk.android.root): /proc/self/maps is a
        // real file, the process's program, linker and guest libraries, written at
        // each open as the kernel makes it (windows/src/bionic/paths.cpp). App
        // packers choose their loader's build by it; this one-line body named no
        // linker64 and sent them to a 32-bit build.
        return "/proc/self/maps".equals(name) && System.getProperty("winapk.android.root") == null;
    }

    // Any OTHER /proc path (2026-10-30): the runtime installs this hook
    // (Boot, via reflection) to the bionic shim's /proc synthesis, so a Java
    // read of e.g. /proc/self/status gets the same true per-process contents a
    // native read does (Google Play services' memory metrics read it this way).
    // Null outside the runtime, or for a path the shim does not answer: the
    // open then proceeds exactly as the JDK's.
    private static volatile java.util.function.UnaryOperator<String> macapkProcRemap;

    private static String macapkRemap(String name) {
        java.util.function.UnaryOperator<String> h = macapkProcRemap;
        if (h == null || name == null || !name.startsWith("/proc/")) return name;
        try {
            String host = h.apply(name);
            return host != null ? host : name;
        } catch (Throwable t) {
            return name;
        }
    }

    private static InputStream macapkOpen(String fileName) throws FileNotFoundException {
        if (macapkIsProc(fileName)) return new ByteArrayInputStream(PROC_BODY);
        return new FileInputStream(macapkRemap(fileName));
    }

    private static InputStream macapkOpen(File file) throws FileNotFoundException {
        if (file != null && macapkIsProc(file.getPath()))
            return new ByteArrayInputStream(PROC_BODY);
        if (file != null && file.getPath().startsWith("/proc/")) {
            String host = macapkRemap(file.getPath());
            if (!host.equals(file.getPath())) return new FileInputStream(host);
        }
        return new FileInputStream(file);
    }
}
