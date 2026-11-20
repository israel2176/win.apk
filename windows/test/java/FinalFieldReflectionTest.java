// FinalFieldReflectionTest -- what Reanimated's reflection on ART's
// java.lang.reflect.Field.accessFlags comes to on Win.apk's HotSpot.
//
// Reanimated (ReanimatedUIManagerFactory.create, ReanimatedNativeHierarchyManager's
// constructor; Bank Leumi) replaces a React Native field by reflection:
//
//     Field f = Base.class.getDeclaredField("mUIImplementation");   // private final
//     f.setAccessible(true);
//     try {
//         Field flags = Field.class.getDeclaredField("accessFlags");  // ART's
//         flags.setAccessible(true);
//         flags.setInt(f, f.getModifiers() & ~Modifier.FINAL);
//     } catch (NoSuchFieldException | IllegalAccessException e) { e.printStackTrace(); }
//     f.set(this, replacement);
//
// HotSpot's Field has no accessFlags (its count is "modifiers", and every member of
// Field is hidden from reflection: Reflection.fieldFilterMap, kept in Win.apk's
// java_base_patch), so the middle step throws NoSuchFieldException, caught: a stack
// trace in the log. The step that matters, f.set on a private final INSTANCE field
// after setAccessible(true), succeeds on HotSpot without it (a final field of an
// ordinary class is not a trusted final), so the replacement is made. This test pins
// both halves, so that a JVM option or overlay change that makes the set fail shows
// here. Why Win.apk does not add an accessFlags field to Field:
// docs/research/open-issues.md, "Reanimated reflects on ART's Field.accessFlags".
//
//   javac -d out windows/test/java/FinalFieldReflectionTest.java
//   java --patch-module java.base=<runtime>/lib/java_base_patch -cp out FinalFieldReflectionTest

import java.lang.reflect.Field;
import java.lang.reflect.Modifier;

public final class FinalFieldReflectionTest {
    private static int failures;

    private static void check(boolean ok, String what) {
        System.out.println((ok ? "[PASS] " : "[FAIL] ") + what);
        if (!ok) failures++;
    }

    static class Base {
        private final Object mUIImplementation = "framework's";
        Object get() { return mUIImplementation; }
    }

    static final class Sub extends Base { }

    public static void main(String[] args) throws Exception {
        Sub obj = new Sub();
        Field f = Sub.class.getSuperclass().getDeclaredField("mUIImplementation");
        f.setAccessible(true);
        check(Modifier.isFinal(f.getModifiers()), "the field is final");
        boolean noSuchField = false;
        try {
            Field flags = Field.class.getDeclaredField("accessFlags");
            flags.setAccessible(true);
            flags.setInt(f, f.getModifiers() & ~Modifier.FINAL);
        } catch (NoSuchFieldException e) {
            noSuchField = true;
        }
        check(noSuchField, "Field.accessFlags: NoSuchFieldException, as Reanimated catches");
        f.set(obj, "reanimated's");
        // Read through compiled code, not reflection: what the class itself sees.
        for (int i = 0; i < 20_000; i++) obj.get();
        check("reanimated's".equals(obj.get()), "Field.set on a private final instance field takes effect");

        if (failures != 0) {
            System.out.println(failures + " FAILED");
            System.exit(1);
        }
        System.out.println("ALL PASSED");
    }
}
