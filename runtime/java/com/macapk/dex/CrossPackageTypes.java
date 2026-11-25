// CrossPackageTypes -- which app classes the converter must make public because
// code in ANOTHER package may need to name them in a checkcast that ART never
// needed.
//
// THE BUG THIS FIXES
// R8 repackages classes (-repackageclasses) and leaves members whose signatures
// name a package-private class of a different package. Gmail 2026.11:
//
//     com.google.android.material.floatingactionbutton.ExtendedFloatingActionButton
//         stores and passes values of type `bbqp`, a package-private class R8
//         moved into the default package.
//
// That is legal DEX and ART runs it: a type that only appears in a field or
// method SIGNATURE is never access-checked (art/runtime/verifier: class access
// is checked where an instruction names a class -- new-instance, check-cast,
// const-class, a member's declaring class -- not for the types of the values
// it moves). The JVM agrees on that much. But the translation inserts its own
// checkcasts where its StackMapTable widens a slot (an exception handler's
// join, a register reused for two types: see Translator.castIfImprecise and
// TypeInference.pick), and a checkcast DOES name its class. HotSpot resolves it
// with the JVMS 5.4.4 access check and throws
//
//     IllegalAccessError: failed to access class bbqp from class
//     ...ExtendedFloatingActionButton (bbqp and ...ExtendedFloatingActionButton
//     are in unnamed module of loader com.macapk.loader.AppClassLoader)
//
// inside a constructor ART runs cleanly, so the inflater replaced Gmail's
// floating "Compose" button with a placeholder.
//
// THE FIX
// A class whose name a cross-package signature carries is given ACC_PUBLIC in
// its class file. Only access is widened, never narrowed, so nothing ART allows
// starts failing; nothing ART forbids starts working either, because every
// instruction that names a class explicitly was already checked by ART against
// the DEX's own flags. The one observable difference is Class.getModifiers() on
// such a class, and javac cannot produce the shape at all (source code cannot
// name a package-private class of another package), so in practice it touches
// only classes a shrinker renamed and moved.
//
// WHICH TYPES
// Every reference type, array element types included, that a method's code
// meets in a field reference's type or an invoked method's parameter or return
// types, plus the method's own parameter and return types: these are the casts'
// targets (an invoke's arguments, a field store, a return). A type counts when
// it is an app class (in the session), not public, and in a different package
// from the class whose code meets it. The scan rides on InitRepointPlan's pass
// over every method, so it costs no second decode.

package com.macapk.dex;

import java.util.Map;
import java.util.Set;

final class CrossPackageTypes {

    private CrossPackageTypes() { }

    /** Record into {@code out} the classes {@code method}'s code may cast to across packages. */
    static void collect(Map<String, DexClass> byName, DexClass owner, DexMethod method,
                        Instruction[] decoded, Set<String> out) {
        String pkg = packageOf(owner.name());
        DexFile.Proto own = method.proto();
        if (own != null) addProto(byName, pkg, own, out);
        if (decoded == null) return;
        DexFile dex = owner.dex();
        for (Instruction in : decoded) {
            if (in.isPayload()) continue;
            int op = in.opcode();
            try {
                if (op >= 0x52 && op <= 0x6d) {                 // iget*/iput*/sget*/sput*
                    DexFile.FieldRef f = dex.fieldRef(in.index());
                    if (f != null) add(byName, pkg, f.type(), out);
                } else if ((op >= 0x6e && op <= 0x72) || (op >= 0x74 && op <= 0x78)) {
                    DexFile.MethodRef m = dex.methodRef(in.index());   // invoke-*[/range]
                    if (m != null && m.proto() != null) addProto(byName, pkg, m.proto(), out);
                }
            } catch (RuntimeException e) {
                // A reference the translator cannot read either; it reports it.
            }
        }
    }

    private static void addProto(Map<String, DexClass> byName, String pkg, DexFile.Proto p,
                                 Set<String> out) {
        add(byName, pkg, p.returnType(), out);
        for (String t : p.parameterTypes()) add(byName, pkg, t, out);
    }

    private static void add(Map<String, DexClass> byName, String pkg, String descriptor,
                            Set<String> out) {
        if (descriptor == null) return;
        int i = 0;
        while (i < descriptor.length() && descriptor.charAt(i) == '[') i++;
        if (i >= descriptor.length() || descriptor.charAt(i) != 'L') return;
        String name = DexFile.internalName(descriptor.substring(i));
        if (name == null || out.contains(name)) return;
        DexClass c = byName.get(name);
        if (c == null || (c.accessFlags() & ClassFileWriter.ACC_PUBLIC) != 0) return;
        if (packageOf(name).equals(pkg)) return;
        out.add(name);
    }

    /** The run-time package's name part: everything before the last '/'. */
    static String packageOf(String internalName) {
        int slash = internalName.lastIndexOf('/');
        return slash < 0 ? "" : internalName.substring(0, slash);
    }
}
