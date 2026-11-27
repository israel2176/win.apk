// WinApkExecTest -- java.lang.ProcessBuilder in Win.apk's java.base overlay (jdk.internal.util
// .WinApkExec): an Android program (an ELF executable) is started through winapk-runtime.exe --exec,
// a Windows program as it is, and an Android working directory is the host folder.
//
//   java --patch-module java.base=<lib/java_base_patch> --add-exports java.base/jdk.internal.util=ALL-UNNAMED
//        -Dwinapk.android.root=<dir> -Dwinapk.runtime.exe=<build>\winapk-runtime.exe
//        WinApkExecTest [<winapk_exec_guest>]
//
// With the guest program (test/guest/exec_guest.c) it is also run for real: its output read back
// through the Process's streams, its exit status from waitFor.
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

public final class WinApkExecTest {
    private static int failures;

    private static void check(String what, boolean ok, Object got) {
        System.out.println("  [" + (ok ? "PASS" : "FAIL") + "] " + what + (ok ? "" : ": " + got));
        if (!ok) failures++;
    }

    public static void main(String[] args) throws Exception {
        String runtime = System.getProperty("winapk.runtime.exe");
        File elf = File.createTempFile("winapk-elf", ".bin");
        try (OutputStream o = new FileOutputStream(elf)) {
            o.write(new byte[] {0x7f, 'E', 'L', 'F', 2, 1, 1, 0});
        }
        File text = File.createTempFile("winapk-text", ".txt");
        try (OutputStream o = new FileOutputStream(text)) {
            o.write("not a program".getBytes(StandardCharsets.UTF_8));
        }

        System.setProperty("winapk.app.package", "com.example.app");
        System.setProperty("winapk.app.data", "C:\\data\\com.example.app");
        Map<String, String> env = new HashMap<>(System.getenv());
        env.put("WINAPK_EXEC_TEST", "set by the app");
        String[] cmd = jdk.internal.util.WinApkExec.command(new String[] {elf.getPath(), "a b", "c"}, env);
        List<String> c = Arrays.asList(cmd);
        check("an ELF program goes through winapk-runtime.exe --exec",
                cmd[0].equals(runtime) && cmd[1].equals("--exec"), cmd[0] + " " + cmd[1]);
        int dash = c.indexOf("--");
        check("the mapping and the app's data", c.contains("--android-root") && c.contains("--app")
                && c.contains("com.example.app"), c);
        check("only the variable the app changed", c.contains("WINAPK_EXEC_TEST=set by the app")
                && c.stream().filter("--env"::equals).count() == 1, c);
        check("then the program and the app's argv", dash > 0 && c.get(dash + 1).equals(elf.getPath())
                && c.subList(dash + 2, c.size()).equals(Arrays.asList(elf.getPath(), "a b", "c")), c);
        String[] plain = {text.getPath(), "x"};
        check("any other file as it is", jdk.internal.util.WinApkExec.command(plain.clone(), null)[0]
                .equals(text.getPath()), null);
        String[] windows = {"cmd.exe", "/c", "echo"};
        check("a Windows program as it is", Arrays.equals(jdk.internal.util.WinApkExec.command(windows.clone(), null),
                windows), null);
        check("a host working directory as it is", "C:\\x".equals(jdk.internal.util.WinApkExec.directory("C:\\x")),
                null);
        elf.delete();
        text.delete();

        if (args.length > 0) {
            // For real: ProcessBuilder, the Process's streams, waitFor.
            ProcessBuilder pb = new ProcessBuilder(new File(args[0]).getAbsolutePath(), "exit=5", "from java");
            pb.environment().put("WINAPK_EXEC_TEST", "from the builder");
            Process p = pb.start();
            try (OutputStream in = p.getOutputStream()) {
                in.write("lower\n".getBytes(StandardCharsets.UTF_8));
            }
            String out = new String(readAll(p.getInputStream()), StandardCharsets.UTF_8);
            String err = new String(readAll(p.getErrorStream()), StandardCharsets.UTF_8);
            int code = p.waitFor();
            check("the program's output", out.contains("argv:from java\n") && out.contains("env:from the builder\n")
                    && out.contains("LOWER\n"), out);
            check("its stderr, nothing of the runtime's", err.equals("to stderr\n"), err);
            check("its exit status", code == 5, code);
        }

        System.out.println(failures == 0 ? "ALL PASSED" : failures + " FAILED");
        System.exit(failures == 0 ? 0 : 1);
    }

    private static byte[] readAll(InputStream in) throws Exception {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] buf = new byte[4096];
        int n;
        while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
        return out.toByteArray();
    }
}
