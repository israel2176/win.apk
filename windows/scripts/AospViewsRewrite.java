// AospViewsRewrite.java -- the build-time rewrite that lets AOSP 14's android.view / android.widget run
// on HotSpot beside Win.apk's framework (build_aosp_views.sh; docs/research/aosp-views.md, step V1).
// It edits the class files under <classes dir> in place, with ASM:
//
//   * Natives. Each native method of a taken class becomes a plain method. When
//     com.winapk.aospviews.Native<SimpleName> has a public static method of the same name and
//     descriptor (an instance native passes `this` first), it calls it. A native listed in INERT
//     answers its type's zero (it releases or reports something that has no native side here: a
//     SurfaceSession's client, an input window handle's). Any other throws
//     UnsupportedOperationException naming it, so it shows up by name, never as a silent 0.
//   * What AOSP's classes call that Win.apk's lack: calls go to com.winapk.aospviews.Compat (same
//     name; an instance call passes the receiver first), static field reads to Compat's fields,
//     DisplayMetrics' noncompat* fields to the plain ones (Win.apk never scales for compatibility).
//   * `new Surface()` (Win.apk's class) is released at once: AOSP's is invalid until copyFrom.
//   * RippleDrawable's patterned ripple is not started on a software canvas: AOSP builds its
//     RippleShader and then skips drawing it there ("not supported for a non-hardware accelerated
//     Canvas"); the native runtime's older SkSL refuses that shader, which threw out of the draw.
//   * The in-process system services (SERVICES): for each AIDL interface, a generated subclass of
//     its Stub that calls the answering class of windows/java/aosp_views.
//
//   java -cp asm.jar:asm-commons.jar AospViewsRewrite.java <classes dir>
//
// Prints what it changed, per kind.

import org.objectweb.asm.ClassReader;
import org.objectweb.asm.ClassVisitor;
import org.objectweb.asm.ClassWriter;
import org.objectweb.asm.MethodVisitor;
import org.objectweb.asm.Opcodes;
import org.objectweb.asm.Type;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.TreeMap;
import java.util.stream.Stream;

public class AospViewsRewrite {
    static final String PKG = "com/winapk/aospviews/";
    static final String COMPAT = PKG + "Compat";

    /** Natives with nothing behind them here: they answer zero / null / false / nothing. */
    static final Set<String> INERT = Set.of(
            "android/view/SurfaceSession.nativeCreate", "android/view/SurfaceSession.nativeDestroy",
            "android/view/InputWindowHandle.nativeDispose", "android/view/InputApplicationHandle.nativeDispose",
            "android/view/InputChannel.nativeDispose", "android/view/InputChannel.nativeGetToken",
            "android/view/InputChannel.nativeGetFinalizer",
            "android/view/InputChannel.nativeGetName", "android/view/InputChannel.nativeDup");

    /** owner.name+desc of a call (static or instance) -> Compat, same name; an instance call passes
     *  the receiver first. Found by LinkScan: what AOSP's views call that Win.apk's classes lack and
     *  that runs on the way to a drawn window. */
    static final Set<String> TO_COMPAT = Set.of(
            "android/app/ActivityThread.isSystem()Z",
            "android/animation/Animator.getBackgroundPauseDelay()J",
            "android/view/SurfaceControl$Transaction.sanitize(II)V",
            "android/view/SurfaceControl$Transaction.clear()V",
            "android/view/SurfaceControl$Transaction.remove(Landroid/view/SurfaceControl;)Landroid/view/SurfaceControl$Transaction;",
            "android/view/SurfaceControl$Transaction.setBlurRegions(Landroid/view/SurfaceControl;[[F)Landroid/view/SurfaceControl$Transaction;",
            "android/view/SurfaceControl$Transaction.setDefaultFrameRateCompatibility(Landroid/view/SurfaceControl;I)Landroid/view/SurfaceControl$Transaction;",
            "android/view/SurfaceControl$Transaction.setDimmingEnabled(Landroid/view/SurfaceControl;Z)Landroid/view/SurfaceControl$Transaction;",
            "android/view/SurfaceControl$Transaction.setDisplayDecoration(Landroid/view/SurfaceControl;Z)Landroid/view/SurfaceControl$Transaction;",
            "android/view/SurfaceControl$Transaction.setFrameTimelineVsync(J)Landroid/view/SurfaceControl$Transaction;",
            "android/view/SurfaceControl$Transaction.setShadowRadius(Landroid/view/SurfaceControl;F)Landroid/view/SurfaceControl$Transaction;",
            "android/text/Spannable.removeSpan(Ljava/lang/Object;I)V",
            "android/icu/lang/UCharacter.getCombiningClass(I)I",
            "android/icu/lang/UCharacter.hasBinaryProperty(II)Z",
            "android/icu/lang/UCharacter.getIntPropertyValue(II)I",
            "android/icu/text/DecimalFormatSymbols.getDecimalSeparatorString()Ljava/lang/String;",
            "android/text/format/DateFormat.getIcuDateFormatSymbols(Ljava/util/Locale;)Landroid/icu/text/DateFormatSymbols;",
            "android/media/AudioManager.areNavigationRepeatSoundEffectsEnabled()Z",
            "android/media/AudioManager.getNthNavigationRepeatSoundEffect(I)I",
            "android/media/AudioManager.preDispatchKeyEvent(Landroid/view/KeyEvent;I)V",
            "android/app/ActivityManager.isLowRamDeviceStatic()Z",
            "android/app/ActivityManager.isSystemReady()Z",
            "android/text/TextUtils.emptyIfNull(Ljava/lang/String;)Ljava/lang/String;",
            "android/text/TextUtils.formatSimple(Ljava/lang/String;[Ljava/lang/Object;)Ljava/lang/String;",
            "android/os/StrictMode.noteResourceMismatch(Ljava/lang/Object;)V",
            "android/content/Context.getDisplayId()I",
            "android/content/Context.getDisplayNoVerify()Landroid/view/Display;",
            "android/content/Context.updateDisplay(I)V",
            "android/content/Context.isAutofillCompatibilityEnabled()Z",
            "android/content/Context.getAutofillOptions()Landroid/content/AutofillOptions;",
            "android/content/Context.getContentCaptureOptions()Landroid/content/ContentCaptureOptions;",
            "android/content/Context.getAutofillClient()Landroid/view/autofill/AutofillManager$AutofillClient;",
            "android/content/Context.getNextAutofillId()I",
            "android/content/Context.canLoadUnsafeResources()Z",
            "android/content/Context.canStartActivityForResult()Z",
            "android/content/Context.isConfigurationContext()Z",
            "android/content/Context.getMainThreadHandler()Landroid/os/Handler;",
            "android/content/Context.getWindowContextToken()Landroid/os/IBinder;",
            "android/content/Context.getToken(Landroid/content/Context;)Landroid/os/IBinder;",
            "android/content/pm/ApplicationInfo.hasRtlSupport()Z",
            "android/content/pm/ApplicationInfo.isEmbeddedDexUsed()Z",
            "android/content/pm/ApplicationInfo.isPrivilegedApp()Z",
            "android/view/Surface.setCompatibilityTranslator(Landroid/content/res/CompatibilityInfo$Translator;)V",
            "android/os/StrictMode.assertConfigurationContext(Landroid/content/Context;Ljava/lang/String;)V",
            "android/os/StrictMode.onIncorrectContextUsed(Ljava/lang/String;Ljava/lang/Throwable;)V",
            "android/os/StrictMode.vmFileUriExposureEnabled()Z",
            "android/text/TextUtils.isPunctuation(I)Z",
            "android/text/TextUtils.isWhitespace(I)Z",
            "android/text/TextUtils.isWhitespaceExceptNewline(I)Z",
            "android/text/TextUtils.length(Ljava/lang/String;)I",
            "android/text/TextUtils.safeIntern(Ljava/lang/String;)Ljava/lang/String;",
            "android/text/TextUtils.trimNoCopySpans(Ljava/lang/CharSequence;)Ljava/lang/CharSequence;",
            "android/text/TextUtils.trimToParcelableSize(Ljava/lang/CharSequence;)Ljava/lang/CharSequence;",
            "android/util/TimeUtils.formatUptime(J)Ljava/lang/String;",
            "android/util/TimeUtils.logTimeOfDay(J)Ljava/lang/String;",
            "android/os/Trace.instant(JLjava/lang/String;)V",
            "android/os/Trace.instantForTrack(JLjava/lang/String;Ljava/lang/String;)V",
            "android/os/Trace.asyncTraceForTrackBegin(JLjava/lang/String;Ljava/lang/String;I)V",
            "android/os/Trace.asyncTraceForTrackEnd(JLjava/lang/String;I)V",
            "android/os/Looper.setTraceTag(J)V",
            "android/os/Looper.setSlowLogThresholdMs(JJ)V",
            "android/content/pm/ActivityInfo.colorModeToString(I)Ljava/lang/String;",
            "android/content/pm/ActivityInfo.screenOrientationToString(I)Ljava/lang/String;",
            "android/os/Debug.getCallers(I)Ljava/lang/String;",
            "android/os/Debug.countInstancesOfClass(Ljava/lang/Class;)J",
            "android/util/DebugUtils.flagsToString(Ljava/lang/Class;Ljava/lang/String;J)Ljava/lang/String;",
            "android/os/HandlerThread.getThreadHandler()Landroid/os/Handler;",
            "android/os/UserHandle.getCallingUserId()I",
            "android/os/Binder.isProxy(Landroid/os/IInterface;)Z",
            "android/app/compat/CompatChanges.isChangeEnabled(J)Z",
            "android/compat/Compatibility.isChangeEnabled(J)Z",
            "android/os/SystemProperties.find(Ljava/lang/String;)Landroid/os/SystemProperties$Handle;",
            "android/graphics/HardwareRenderer.setFPSDivisor(I)V",
            "android/view/ThreadedRenderer.setFPSDivisor(I)V",
            "android/graphics/HardwareRenderer.isDrawingEnabled()Z",
            "android/view/ThreadedRenderer.isDrawingEnabled()Z",
            "android/text/TextUtils.toUpperCase(Ljava/util/Locale;Ljava/lang/CharSequence;Z)Ljava/lang/CharSequence;",
            "android/graphics/RenderNode.addPositionUpdateListener(Landroid/graphics/RenderNode$PositionUpdateListener;)V",
            "android/graphics/RenderNode.removePositionUpdateListener(Landroid/graphics/RenderNode$PositionUpdateListener;)V",
            "android/app/Activity.getActivityInfo()Landroid/content/pm/ActivityInfo;",
            "android/content/pm/ActivityInfo.hasOnBackInvokedCallbackEnabled()Z",
            "android/content/pm/ActivityInfo.isOnBackInvokedCallbackEnabled()Z",
            "android/content/pm/ApplicationInfo.isOnBackInvokedCallbackEnabled()Z");
    static final Map<String, String> STATIC_FIELDS = Map.of(
            "android/os/Build.HW_TIMEOUT_MULTIPLIER", COMPAT,
            "android/os/Build.IS_ENG", COMPAT,
            "android/os/Build.IS_USER", COMPAT,
            "android/os/Build$VERSION.RESOURCES_SDK_INT", COMPAT);
    static final Map<String, String> RENAMED_FIELDS = Map.of(
            "noncompatWidthPixels", "widthPixels", "noncompatHeightPixels", "heightPixels",
            "noncompatDensity", "density", "noncompatScaledDensity", "scaledDensity",
            "noncompatXdpi", "xdpi", "noncompatYdpi", "ydpi", "noncompatDensityDpi", "densityDpi");

    static final Map<String, Integer> counts = new TreeMap<>();
    static void count(String what) { counts.merge(what, 1, Integer::sum); }

    public static void main(String[] a) throws IOException {
        if (a.length != 1) throw new IllegalArgumentException("usage: AospViewsRewrite <classes dir>");
        Path root = Path.of(a[0]);
        List<Path> files;
        try (Stream<Path> s = Files.walk(root)) {
            files = s.filter(p -> p.toString().endsWith(".class")).toList();
        }
        Map<String, Set<String>> bridges = new HashMap<>();
        for (Path f : files) {
            String name = root.relativize(f).toString().replace('\\', '/');
            name = name.substring(0, name.length() - ".class".length());
            if (name.startsWith(PKG + "Native")) bridges.put(name, statics(f));
        }
        Set<String> unbodied = new java.util.TreeSet<>();
        for (Path f : files) {
            String name = root.relativize(f).toString().replace('\\', '/');
            name = name.substring(0, name.length() - ".class".length());
            if (name.startsWith(PKG)) continue;  // our own classes are written for HotSpot
            if (!name.startsWith("android/") && !name.startsWith("com/android/")) continue;
            byte[] in = Files.readAllBytes(f);
            ClassReader cr = new ClassReader(in);
            ClassWriter cw = new ClassWriter(cr, ClassWriter.COMPUTE_MAXS);
            String bridge = PKG + "Native" + name.substring(name.lastIndexOf('/') + 1).replace('$', '_');
            cr.accept(new Rewriter(cw, name, bridge, bridges.getOrDefault(bridge, Set.of()), unbodied), 0);
            byte[] out = cw.toByteArray();
            if (!java.util.Arrays.equals(in, out)) Files.write(f, out);
        }
        for (String[] svc : SERVICES) generateService(root, svc[0], svc[1]);
        counts.forEach((k, v) -> System.out.println("[aosp-views]   " + v + " " + k));
        System.out.println("[aosp-views]   natives left throwing (by class): " + summarize(unbodied));
    }

    /** The in-process system services: AIDL interface -> the plain class that answers it
     *  (windows/java/aosp_views). java.lang.reflect.Proxy cannot stand in for these: it loads every
     *  type an interface's methods name, and IWindowManager names classes no app process needs
     *  (media projection, ...). So the implementation is generated here instead. */
    static final String[][] SERVICES = {
            {"android/view/IWindowManager", PKG + "AospWindows$WindowManagerService"},
            {"android/view/IWindowSession", PKG + "AospWindows$WindowSession"},
            {"android/hardware/display/IDisplayManager", PKG + "AospWindows$DisplayService"},
            {"com/android/internal/view/IInputMethodManager", PKG + "ImeService"},
            {"android/view/accessibility/IAccessibilityManager", PKG + "AccessibilityService"},
            {"android/app/INotificationManager", PKG + "ToastService"},
            {"com/android/internal/textservice/ITextServicesManager", PKG + "TextServicesService"},
            {"android/hardware/input/IInputManager", PKG + "InputManagerService"},
    };

    /** com/winapk/aospviews/gen/<I>Impl extends <I>$Stub (a local Binder whose queryLocalInterface
     *  answers itself), built with the handler: each method of the interface calls the handler's
     *  public method of the same name and descriptor when it has one, and otherwise reports the
     *  call once (Services.unanswered) and returns its type's zero. */
    static void generateService(Path root, String iface, String handler) throws IOException {
        Path ifaceFile = root.resolve(iface + ".class");
        Path handlerFile = root.resolve(handler + ".class");
        if (!Files.exists(ifaceFile) || !Files.exists(handlerFile)) {
            throw new IllegalStateException("service " + iface + " / " + handler + ": class missing");
        }
        Set<String> answered = new HashSet<>();
        new ClassReader(Files.readAllBytes(handlerFile)).accept(new ClassVisitor(Opcodes.ASM9) {
            @Override public MethodVisitor visitMethod(int acc, String n, String d, String s, String[] e) {
                if ((acc & Opcodes.ACC_PUBLIC) != 0 && (acc & Opcodes.ACC_STATIC) == 0) answered.add(n + d);
                return null;
            }
        }, ClassReader.SKIP_CODE);
        List<String[]> methods = new java.util.ArrayList<>();
        new ClassReader(Files.readAllBytes(ifaceFile)).accept(new ClassVisitor(Opcodes.ASM9) {
            @Override public MethodVisitor visitMethod(int acc, String n, String d, String s, String[] e) {
                if ((acc & Opcodes.ACC_ABSTRACT) != 0) methods.add(new String[] {n, d});
                return null;
            }
        }, ClassReader.SKIP_CODE);
        String simple = iface.substring(iface.lastIndexOf('/') + 1);
        String gen = PKG + "gen/" + simple + "Impl";
        String stub = iface + "$Stub";
        ClassWriter cw = new ClassWriter(ClassWriter.COMPUTE_MAXS);
        cw.visit(Opcodes.V17, Opcodes.ACC_PUBLIC | Opcodes.ACC_FINAL | Opcodes.ACC_SUPER, gen, null, stub, null);
        cw.visitField(Opcodes.ACC_PRIVATE | Opcodes.ACC_FINAL, "h", "L" + handler + ";", null, null).visitEnd();
        // A Stub whose permissions are enforced (@EnforcePermission) has a constructor taking the
        // PermissionEnforcer, and a no-argument one that makes it from ActivityThread's system
        // context: the generated class hands it LocalEnforcer instead.
        boolean[] enforced = {false};
        new ClassReader(Files.readAllBytes(root.resolve(stub + ".class"))).accept(new ClassVisitor(Opcodes.ASM9) {
            @Override public MethodVisitor visitMethod(int acc, String n, String d, String s, String[] e) {
                if (n.equals("<init>") && d.equals("(Landroid/os/PermissionEnforcer;)V")) enforced[0] = true;
                return null;
            }
        }, ClassReader.SKIP_CODE);
        MethodVisitor init = cw.visitMethod(Opcodes.ACC_PUBLIC, "<init>", "(L" + handler + ";)V", null, null);
        init.visitCode();
        init.visitVarInsn(Opcodes.ALOAD, 0);
        if (enforced[0]) {
            init.visitTypeInsn(Opcodes.NEW, PKG + "LocalEnforcer");
            init.visitInsn(Opcodes.DUP);
            init.visitMethodInsn(Opcodes.INVOKESPECIAL, PKG + "LocalEnforcer", "<init>", "()V", false);
            init.visitMethodInsn(Opcodes.INVOKESPECIAL, stub, "<init>", "(Landroid/os/PermissionEnforcer;)V", false);
        } else {
            init.visitMethodInsn(Opcodes.INVOKESPECIAL, stub, "<init>", "()V", false);
        }
        init.visitVarInsn(Opcodes.ALOAD, 0);
        init.visitVarInsn(Opcodes.ALOAD, 1);
        init.visitFieldInsn(Opcodes.PUTFIELD, gen, "h", "L" + handler + ";");
        init.visitInsn(Opcodes.RETURN);
        init.visitMaxs(0, 0);
        init.visitEnd();
        int answeredCount = 0;
        for (String[] m : methods) {
            MethodVisitor mv = cw.visitMethod(Opcodes.ACC_PUBLIC, m[0], m[1], null, null);
            mv.visitCode();
            Type ret = Type.getReturnType(m[1]);
            if (answered.contains(m[0] + m[1])) {
                mv.visitVarInsn(Opcodes.ALOAD, 0);
                mv.visitFieldInsn(Opcodes.GETFIELD, gen, "h", "L" + handler + ";");
                int slot = 1;
                for (Type t : Type.getArgumentTypes(m[1])) {
                    mv.visitVarInsn(t.getOpcode(Opcodes.ILOAD), slot);
                    slot += t.getSize();
                }
                mv.visitMethodInsn(Opcodes.INVOKEVIRTUAL, handler, m[0], m[1], false);
                mv.visitInsn(ret.getOpcode(Opcodes.IRETURN));
                answeredCount++;
            } else {
                mv.visitLdcInsn(simple + "." + m[0]);
                mv.visitMethodInsn(Opcodes.INVOKESTATIC, PKG + "Services", "unanswered", "(Ljava/lang/String;)V", false);
                switch (ret.getSort()) {
                    case Type.VOID -> mv.visitInsn(Opcodes.RETURN);
                    case Type.LONG -> { mv.visitInsn(Opcodes.LCONST_0); mv.visitInsn(Opcodes.LRETURN); }
                    case Type.FLOAT -> { mv.visitInsn(Opcodes.FCONST_0); mv.visitInsn(Opcodes.FRETURN); }
                    case Type.DOUBLE -> { mv.visitInsn(Opcodes.DCONST_0); mv.visitInsn(Opcodes.DRETURN); }
                    case Type.OBJECT, Type.ARRAY -> { mv.visitInsn(Opcodes.ACONST_NULL); mv.visitInsn(Opcodes.ARETURN); }
                    default -> { mv.visitInsn(Opcodes.ICONST_0); mv.visitInsn(Opcodes.IRETURN); }
                }
            }
            mv.visitMaxs(0, 0);
            mv.visitEnd();
        }
        cw.visitEnd();
        Path out = root.resolve(gen + ".class");
        Files.createDirectories(out.getParent());
        Files.write(out, cw.toByteArray());
        System.out.println("[aosp-views]   " + simple + ": " + answeredCount + " of " + methods.size()
                + " methods answered (" + handler.substring(handler.lastIndexOf('/') + 1) + ")");
    }

    static String summarize(Set<String> natives) {
        Map<String, Integer> by = new TreeMap<>();
        for (String n : natives) by.merge(n.substring(0, n.indexOf('.')), 1, Integer::sum);
        StringBuilder sb = new StringBuilder();
        by.forEach((k, v) -> sb.append(k.substring(k.lastIndexOf('/') + 1)).append(' ').append(v).append(", "));
        return sb.length() == 0 ? "none" : sb.substring(0, sb.length() - 2);
    }

    static Set<String> statics(Path f) throws IOException {
        Set<String> have = new HashSet<>();
        new ClassReader(Files.readAllBytes(f)).accept(new ClassVisitor(Opcodes.ASM9) {
            @Override public MethodVisitor visitMethod(int acc, String n, String d, String s, String[] e) {
                if ((acc & Opcodes.ACC_STATIC) != 0 && (acc & Opcodes.ACC_PUBLIC) != 0) have.add(n + d);
                return null;
            }
        }, ClassReader.SKIP_CODE);
        return have;
    }

    static final class Rewriter extends ClassVisitor {
        final String cls, bridge;
        final Set<String> bridged, unbodied;

        Rewriter(ClassVisitor cv, String cls, String bridge, Set<String> bridged, Set<String> unbodied) {
            super(Opcodes.ASM9, cv);
            this.cls = cls;
            this.bridge = bridge;
            this.bridged = bridged;
            this.unbodied = unbodied;
        }

        /** What Win.apk's own SurfaceView (kept: the host's GL and video surfaces) reads of its
         *  superclass and of Display, added to AOSP's: View's host handle (nativeHandle and its
         *  accessors) and macapkAttached, Win.apk's stand-in for `mAttachInfo != null`, kept equal
         *  to it by dispatchAttachedToWindow / dispatchDetachedFromWindow; Display's real size in
         *  pixels (DisplayConfig's). */
        @Override
        public void visitEnd() {
            if (cls.equals("android/view/View")) {
                super.visitField(Opcodes.ACC_PROTECTED, "nativeHandle", "J", null, null).visitEnd();
                super.visitField(0, "macapkAttached", "Z", null, null).visitEnd();
                MethodVisitor get = super.visitMethod(Opcodes.ACC_PUBLIC, "getNativeHandle", "()J", null, null);
                get.visitCode();
                get.visitVarInsn(Opcodes.ALOAD, 0);
                get.visitFieldInsn(Opcodes.GETFIELD, cls, "nativeHandle", "J");
                get.visitInsn(Opcodes.LRETURN);
                get.visitMaxs(0, 0);
                get.visitEnd();
                MethodVisitor set = super.visitMethod(Opcodes.ACC_PROTECTED, "setNativeHandle", "(J)V", null, null);
                set.visitCode();
                set.visitVarInsn(Opcodes.ALOAD, 0);
                set.visitVarInsn(Opcodes.LLOAD, 1);
                set.visitFieldInsn(Opcodes.PUTFIELD, cls, "nativeHandle", "J");
                set.visitInsn(Opcodes.RETURN);
                set.visitMaxs(0, 0);
                set.visitEnd();
                count("View members for Win.apk's SurfaceView added");
            }
            if (cls.equals("android/view/Display")) {
                for (String m : new String[] {"Width", "Height"}) {
                    MethodVisitor mv = super.visitMethod(Opcodes.ACC_PUBLIC | Opcodes.ACC_STATIC,
                            "macapkReal" + m + "Px", "()I", null, null);
                    mv.visitCode();
                    mv.visitMethodInsn(Opcodes.INVOKESTATIC, "com/macapk/loader/DisplayConfig",
                            "real" + m + "Px", "()I", false);
                    mv.visitInsn(Opcodes.IRETURN);
                    mv.visitMaxs(0, 0);
                    mv.visitEnd();
                }
                count("Display members for Win.apk's SurfaceView added");
            }
            super.visitEnd();
        }

        @Override
        public MethodVisitor visitMethod(int acc, String name, String desc, String sig, String[] exc) {
            if ((acc & Opcodes.ACC_NATIVE) != 0) {
                boolean isStatic = (acc & Opcodes.ACC_STATIC) != 0;
                MethodVisitor mv = super.visitMethod(acc & ~Opcodes.ACC_NATIVE, name, desc, sig, exc);
                mv.visitCode();
                String bdesc = isStatic ? desc : "(L" + cls + ";" + desc.substring(1);
                Type ret = Type.getReturnType(desc);
                if (bridged.contains(name + bdesc)) {
                    int slot = 0;
                    if (!isStatic) mv.visitVarInsn(Opcodes.ALOAD, slot++);
                    for (Type t : Type.getArgumentTypes(desc)) {
                        mv.visitVarInsn(t.getOpcode(Opcodes.ILOAD), slot);
                        slot += t.getSize();
                    }
                    mv.visitMethodInsn(Opcodes.INVOKESTATIC, bridge, name, bdesc, false);
                    mv.visitInsn(ret.getOpcode(Opcodes.IRETURN));
                    count("natives given Java bodies");
                } else if (INERT.contains(cls + "." + name)) {
                    switch (ret.getSort()) {
                        case Type.VOID -> mv.visitInsn(Opcodes.RETURN);
                        case Type.LONG -> { mv.visitInsn(Opcodes.LCONST_0); mv.visitInsn(Opcodes.LRETURN); }
                        case Type.FLOAT -> { mv.visitInsn(Opcodes.FCONST_0); mv.visitInsn(Opcodes.FRETURN); }
                        case Type.DOUBLE -> { mv.visitInsn(Opcodes.DCONST_0); mv.visitInsn(Opcodes.DRETURN); }
                        case Type.OBJECT, Type.ARRAY -> { mv.visitInsn(Opcodes.ACONST_NULL); mv.visitInsn(Opcodes.ARETURN); }
                        default -> { mv.visitInsn(Opcodes.ICONST_0); mv.visitInsn(Opcodes.IRETURN); }
                    }
                    count("natives with nothing behind them made inert");
                } else {
                    mv.visitTypeInsn(Opcodes.NEW, "java/lang/UnsupportedOperationException");
                    mv.visitInsn(Opcodes.DUP);
                    mv.visitLdcInsn("not implemented on HotSpot: " + cls.replace('/', '.') + "." + name + desc);
                    mv.visitMethodInsn(Opcodes.INVOKESPECIAL, "java/lang/UnsupportedOperationException", "<init>",
                            "(Ljava/lang/String;)V", false);
                    mv.visitInsn(Opcodes.ATHROW);
                    unbodied.add(cls + "." + name + desc);
                }
                mv.visitMaxs(0, 0);
                mv.visitEnd();
                return null;
            }
            MethodVisitor out = super.visitMethod(acc, name, desc, sig, exc);
            if (cls.equals("android/view/View")
                    && (name.equals("dispatchAttachedToWindow") && desc.equals("(Landroid/view/View$AttachInfo;I)V")
                        || name.equals("dispatchDetachedFromWindow") && desc.equals("()V"))) {
                final boolean attached = name.equals("dispatchAttachedToWindow");
                out = new MethodVisitor(Opcodes.ASM9, out) {
                    @Override public void visitCode() {
                        super.visitCode();
                        super.visitVarInsn(Opcodes.ALOAD, 0);
                        super.visitInsn(attached ? Opcodes.ICONST_1 : Opcodes.ICONST_0);
                        super.visitFieldInsn(Opcodes.PUTFIELD, cls, "macapkAttached", "Z");
                    }
                };
            }
            final boolean patterned = cls.equals("android/graphics/drawable/RippleDrawable")
                    && name.equals("drawPatterned") && desc.equals("(Landroid/graphics/Canvas;)V");
            return new MethodVisitor(Opcodes.ASM9, out) {
                @Override public void visitMethodInsn(int op, String owner, String n, String d, boolean itf) {

                    if (owner.equals("android/app/ActivityThread") && n.equals("getSystemContext")
                            && d.equals("()Landroid/app/ContextImpl;")) {
                        // An AIDL Stub's default constructor: PermissionEnforcer.fromContext(the
                        // system context). Win.apk has no ContextImpl, so the class did not link
                        // (a CDS dump's verification); the call answers a Context instead.
                        super.visitMethodInsn(Opcodes.INVOKESTATIC, COMPAT, "systemContext",
                                "(Landroid/app/ActivityThread;)Landroid/content/Context;", false);
                        count("calls to ActivityThread.getSystemContext answered with a Context");
                        return;
                    }
                    if (TO_COMPAT.contains(owner + "." + n + d)) {
                        String cd = op == Opcodes.INVOKESTATIC ? d : "(L" + owner + ";" + d.substring(1);
                        super.visitMethodInsn(Opcodes.INVOKESTATIC, COMPAT, n, cd, false);
                        count("calls to " + owner.substring(owner.lastIndexOf('/') + 1) + "." + n + " redirected");
                        return;
                    }
                    super.visitMethodInsn(op, owner, n, d, itf);
                    if (op == Opcodes.INVOKESPECIAL && owner.equals("android/view/Surface") && n.equals("<init>")
                            && d.equals("()V")) {
                        // AOSP's `new Surface()` is invalid until copyFrom; Win.apk's is valid from
                        // the start (its own SurfaceHolders hand it out so). Released here, so
                        // ViewRootImpl sees its window's Surface appear at the first relayout.
                        super.visitInsn(Opcodes.DUP);
                        super.visitMethodInsn(Opcodes.INVOKEVIRTUAL, owner, "release", "()V", false);
                        count("new Surface() made invalid until copyFrom");
                    }
                }

                @Override public void visitFieldInsn(int op, String owner, String n, String d) {
                    if (patterned && op == Opcodes.GETFIELD && n.equals("mAddRipple")) {
                        // addRipple = mAddRipple && canvas.isHardwareAccelerated(): the patterned
                        // ripple's session draws only on a hardware canvas (on a software one AOSP
                        // logs "not supported" and skips it), so on a software canvas it is not made.
                        super.visitFieldInsn(op, owner, n, d);
                        super.visitVarInsn(Opcodes.ALOAD, 1);
                        super.visitMethodInsn(Opcodes.INVOKEVIRTUAL, "android/graphics/Canvas",
                                "isHardwareAccelerated", "()Z", false);
                        super.visitInsn(Opcodes.IAND);
                        count("RippleDrawable's patterned ripple made only on a hardware canvas");
                        return;
                    }
                    String to = STATIC_FIELDS.get(owner + "." + n);
                    if (op == Opcodes.GETSTATIC && to != null) {
                        super.visitFieldInsn(op, to, n, d);
                        count("reads of " + owner.substring(owner.lastIndexOf('/') + 1) + "." + n + " redirected");
                        return;
                    }
                    if (owner.equals("android/util/DisplayMetrics")) {
                        String plain = RENAMED_FIELDS.get(n);
                        if (plain != null) {
                            super.visitFieldInsn(op, owner, plain, d);
                            count("DisplayMetrics.noncompat* accesses to the plain fields");
                            return;
                        }
                    }
                    super.visitFieldInsn(op, owner, n, d);
                }
            };
        }
    }
}
