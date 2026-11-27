/*
 * Win.apk: how java.lang.ProcessBuilder (Win.apk's java.base overlay) starts an Android program.
 *
 * On a device, ProcessBuilder runs a file of the app's (a helper binary in its native library
 * directory: Simon Tatham's Puzzles generates its boards with libpuzzlesgen.so run as a program)
 * as a process. Here such a file is an Android ELF executable, which Windows cannot start: the
 * command becomes winapk-runtime.exe --exec (windows/host/exec_main.cpp), which runs it
 * with the same loader, Bionic shim and Android paths as the app's native code, its argv as the app
 * gave it. A variable the app set or changed in the ProcessBuilder's environment is given to the
 * program over Android's defaults, as the shim's own posix_spawn gives it. An Android path as the
 * working directory is the host folder it names, as java.io names it (WinApkPaths).
 *
 * Any other command (a Windows program) is left as it is, and outside Win.apk's runtime (no
 * winapk.android.root) nothing changes.
 */
package jdk.internal.util;

import java.io.IOException;
import java.io.RandomAccessFile;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.Objects;

public final class WinApkExec {
    private WinApkExec() {}

    /** The command ProcessImpl should start for `cmd`; `env` is the ProcessBuilder's environment
     *  (null when the app never asked for it). */
    public static String[] command(String[] cmd, Map<String, String> env) {
        String root = System.getProperty("winapk.android.root");
        String runtime = System.getProperty("winapk.runtime.exe");
        if (root == null || runtime == null || cmd.length == 0) return cmd;
        String prog = cmd[0];
        String host = WinApkPaths.isAndroid(prog) ? WinApkPaths.hostPath(prog) : prog;
        if (!isAndroidProgram(host)) return cmd;
        List<String> out = new ArrayList<>();
        out.add(runtime);
        out.add("--exec");
        out.add("--android-root");
        out.add(root);
        String pkg = System.getProperty("winapk.app.package");
        String data = System.getProperty("winapk.app.data");
        if (pkg != null && data != null) {
            out.add("--app");
            out.add(pkg);
            out.add(data);
        }
        if (env != null) {
            for (Map.Entry<String, String> e : env.entrySet()) {
                if (!Objects.equals(System.getenv(e.getKey()), e.getValue())) {
                    out.add("--env");
                    out.add(e.getKey() + "=" + e.getValue());
                }
            }
        }
        out.add("--");
        out.add(host);
        for (String a : cmd) out.add(a);
        return out.toArray(new String[0]);
    }

    /** The working directory as a host folder: an Android path is mapped, any other kept. */
    public static String directory(String dir) {
        if (dir == null || System.getProperty("winapk.android.root") == null) return dir;
        return WinApkPaths.isAndroid(dir) ? WinApkPaths.hostPath(dir) : dir;
    }

    /** Whether `path` is an ELF executable, the kind of program an Android app runs. */
    private static boolean isAndroidProgram(String path) {
        try (RandomAccessFile f = new RandomAccessFile(path, "r")) {
            byte[] magic = new byte[4];
            return f.read(magic) == 4 && magic[0] == 0x7f && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F';
        } catch (IOException | SecurityException e) {
            return false;
        }
    }
}
