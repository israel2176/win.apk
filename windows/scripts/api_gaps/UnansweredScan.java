// UnansweredScan.java -- which methods of the in-process system services answer with an empty value
// (docs/research/api-gaps.md, "Internal services"). AospAppRewrite.java and AospViewsRewrite.java generate
// com.winapk.aosp{app,views}.gen.<Interface>Impl, a Stub of each AIDL interface (IActivityManager,
// IPackageManager, IWindowManager...) whose methods call the plain handler class's method of the same name
// and descriptor, and where the handler has none, call Services.unanswered("<Interface>.<method>") and
// return the type's empty value (null, 0, false). At run time that is the log line "... no answer in this
// process, the empty one"; here it is found in the jars, every method at once, before an app reaches it.
//
//   java -cp asm.jar:asm-tree.jar UnansweredScan.java <aosp-app.jar> <aosp-views.jar>
//
// Prints, tab-separated: interface  method  descriptor  answered|unanswered, and per interface a
// "# <interface> <answered>/<total>" line.

import org.objectweb.asm.ClassReader;
import org.objectweb.asm.Opcodes;
import org.objectweb.asm.tree.AbstractInsnNode;
import org.objectweb.asm.tree.ClassNode;
import org.objectweb.asm.tree.MethodInsnNode;
import org.objectweb.asm.tree.MethodNode;

import java.io.InputStream;
import java.util.Collections;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

public class UnansweredScan {
    public static void main(String[] a) throws Exception {
        for (String jar : a) {
            try (ZipFile z = new ZipFile(jar)) {
                for (ZipEntry e : Collections.list(z.entries())) {
                    String n = e.getName();
                    if (!n.matches("com/winapk/aosp\\w+/gen/I\\w+Impl\\.class")) continue;
                    ClassNode c = new ClassNode();
                    try (InputStream in = z.getInputStream(e)) { new ClassReader(in.readAllBytes()).accept(c, 0); }
                    String iface = c.interfaces.isEmpty() ? c.superName : c.interfaces.get(0);
                    // The Impl extends <Interface>$Stub: the interface is the Stub's outer class.
                    if (c.superName.endsWith("$Stub")) iface = c.superName.substring(0, c.superName.length() - 5);
                    int total = 0, answered = 0;
                    StringBuilder rows = new StringBuilder();
                    for (MethodNode m : c.methods) {
                        if (m.name.startsWith("<") || (m.access & Opcodes.ACC_SYNTHETIC) != 0
                                || (m.access & Opcodes.ACC_PUBLIC) == 0 || m.name.equals("asBinder")) continue;
                        boolean empty = false;
                        for (AbstractInsnNode i : m.instructions) {
                            if (i instanceof MethodInsnNode mi && mi.name.equals("unanswered")
                                    && mi.owner.endsWith("/Services")) empty = true;
                        }
                        total++;
                        if (!empty) answered++;
                        rows.append(iface.replace('/', '.')).append('\t').append(m.name).append('\t').append(m.desc)
                                .append('\t').append(empty ? "unanswered" : "answered").append('\n');
                    }
                    System.out.print("# " + iface.replace('/', '.') + " " + answered + "/" + total + "\n" + rows);
                }
            }
        }
    }
}
