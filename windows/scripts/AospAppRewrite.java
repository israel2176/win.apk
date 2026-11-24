// AospAppRewrite.java -- the build-time rewrite that lets AOSP 14's android.app lifecycle run on
// HotSpot beside Win.apk's framework (build_aosp_app.sh; docs/research/aosp-app.md, step A1). It
// edits the class files under <classes dir> in place, with ASM, as AospViewsRewrite.java does for
// the views:
//
//   * Natives. Each native method of a taken class becomes a plain method that calls
//     com.winapk.aospapp.Native<SimpleName>'s public static method of the same name and descriptor
//     (an instance native passes `this` first) when there is one, answers its type's zero when it
//     is listed in INERT (it reports or frees something that has no native side here), and throws
//     UnsupportedOperationException naming it otherwise.
//   * What AOSP's classes call that Win.apk's lack (TO_COMPAT, found by LinkScan): the call goes to
//     com.winapk.aospapp.Compat (same name; an instance call passes the receiver first).
//   * The in-process system services (SERVICES): for each AIDL interface, a generated subclass of
//     its Stub that calls the answering class of windows/java/aosp_app, and for a method that
//     class lacks reports it once (Services.unanswered) and answers its type's zero.
//
//   java -cp asm.jar:asm-commons.jar AospAppRewrite.java <classes dir>
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
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.TreeMap;
import java.util.TreeSet;
import java.util.stream.Stream;

public class AospAppRewrite {
    static final String PKG = "com/winapk/aospapp/";
    static final String COMPAT = PKG + "Compat";

    /** Natives with nothing behind them here: they answer zero / null / false / nothing. */
    static final Set<String> INERT = Set.of(
            "android/app/ActivityThread.nPurgePendingResources",
            "android/app/ActivityThread.nInitZygoteChildHeapProfiling",
            // The dynamic linker's warning about the app's native libraries (Activity.performStart
            // shows it in a dialog on a debuggable build): Win.apk's loader has none to give.
            "android/app/Activity.getDlWarning");

    /** owner.name+desc of a call (static or instance) -> Compat, same name; an instance call passes
     *  the receiver first. What AOSP's app layer calls that Win.apk's classes lack, on the way to a
     *  launched, resumed, paused, stopped and destroyed activity (LinkScan lists the rest). */
    static final Set<String> TO_COMPAT = Set.of(
            "android/app/ActivityManager.getService()Landroid/app/IActivityManager;",
            "android/app/Instrumentation.callApplicationOnCreate(Landroid/app/Application;)V",
            "android/app/compat/CompatChanges.isChangeEnabled(J)Z",
            "android/graphics/Compatibility.setTargetSdkVersion(I)V",
            "android/app/ActivityManager.isStartResultFatalError(I)Z",
            "android/os/FileUtils.newFileOrNull(Ljava/lang/String;)Ljava/io/File;",
            "android/os/FileUtils.listOrEmpty(Ljava/io/File;)[Ljava/lang/String;",
            "android/os/FileUtils.listFilesOrEmpty(Ljava/io/File;Ljava/io/FilenameFilter;)[Ljava/io/File;",
            "android/os/FileUtils.isValidExtFilename(Ljava/lang/String;)Z",
            "android/os/FileUtils.setPermissions(Ljava/lang/String;III)I",
            "android/os/FileUtils.copyFileOrThrow(Ljava/io/File;Ljava/io/File;)V",
            "android/os/FileUtils.copyPermissions(Ljava/io/File;Ljava/io/File;)V",
            "android/os/Environment.buildPaths([Ljava/io/File;[Ljava/lang/String;)[Ljava/io/File;",
            "android/os/Environment.buildExternalStorageAppCacheDirs(Ljava/lang/String;)[Ljava/io/File;",
            "android/os/Environment.buildExternalStorageAppFilesDirs(Ljava/lang/String;)[Ljava/io/File;",
            "android/os/Environment.buildExternalStorageAppMediaDirs(Ljava/lang/String;)[Ljava/io/File;",
            "android/os/Environment.buildExternalStorageAppObbDirs(Ljava/lang/String;)[Ljava/io/File;",
            "android/os/Environment.getDataPreloadsFileCacheDirectory(Ljava/lang/String;)Ljava/io/File;",
            "android/os/Environment.getUserConfigDirectory(I)Ljava/io/File;",
            "android/content/pm/ApplicationInfo.getCodePath()Ljava/lang/String;",
            "android/content/pm/ApplicationInfo.getBaseCodePath()Ljava/lang/String;",
            "android/content/pm/ApplicationInfo.getCustomApplicationClassNameForProcess(Ljava/lang/String;)Ljava/lang/String;",
            "android/os/UserHandle.isSameApp(II)Z",
            "android/os/UserHandle.getCacheAppGid(I)I",
            "android/os/UserManager.isUserUnlocked(I)Z",
            "android/os/UserManager.isUserUnlockingOrUnlocked(I)Z",
            "android/app/Activity.getDisplayId()I",
            "android/content/pm/ActivityInfo.getRealConfigChanged()I",
            "android/companion/virtual/VirtualDeviceManager.getDeviceIdForDisplayId(I)I",
            "android/companion/virtual/VirtualDeviceManager.isValidVirtualDeviceId(I)Z",
            "android/app/SystemServiceRegistry.createServiceCache()[Ljava/lang/Object;",
            "android/os/storage/StorageManager.mkdirs(Ljava/io/File;)V",
            "android/os/FileUtils.sync(Ljava/io/FileOutputStream;)Z",
            "android/content/pm/PackageManager.buildRequestPermissionsIntent([Ljava/lang/String;)Landroid/content/Intent;",
            "android/content/pm/PackageManager.shouldShowRequestPermissionRationale(Ljava/lang/String;)Z",
            "android/content/pm/PackageManager.getPackageInfoAsUserCached(Ljava/lang/String;JI)Landroid/content/pm/PackageInfo;",
            "android/content/pm/PackageManager.getApplicationInfoAsUserCached(Ljava/lang/String;JI)Landroid/content/pm/ApplicationInfo;");

    /** Instance methods added to a taken class, answering their type's zero (null, false, nothing): the hidden ContextWrapper members AOSP's
     *  own views (aosp-views.jar, not rewritten here) call on an Application, which Win.apk's
     *  ContextWrapper lacks. AOSP's answer the base ContextImpl's options, which ActivityThread sets from
     *  bindApplication: none here (no autofill or content-capture service). */
    static final Map<String, List<String>> NULL_METHODS = Map.of(
            "android/app/Application", List.of(
                    "getAutofillOptions()Landroid/content/AutofillOptions;",
                    "getContentCaptureOptions()Landroid/content/ContentCaptureOptions;"),
            "android/app/Activity", List.of(
                    "setAutofillOptions(Landroid/content/AutofillOptions;)V",
                    "isAutofillCompatibilityEnabled()Z"));

    /** Methods of the taken classes whose whole body becomes a call to Compat's static of the same name
     *  (the receiver first): they build a Win.apk object through members it does not have. */
    static final Set<String> BODY_TO_COMPAT = Set.of(
            "android/app/ContextImpl.createAttributionSource(Ljava/lang/String;Landroid/content/AttributionSource;Ljava/util/Set;)Landroid/content/AttributionSource;",
            // The package manager is the host's MacApkPackageManager (AOSP's ApplicationPackageManager
            // is a client of PackageManagerService).
            "android/app/ContextImpl.getPackageManager()Landroid/content/pm/PackageManager;");

    /** Methods of the taken classes whose whole body becomes an empty return: they install process-wide
     *  hooks of ART's libcore that HotSpot has no counterpart of (the zip path validator), or tell a
     *  system service there is none of here what the method does when it finds none (the voice
     *  interaction service, which a device without an assistant lacks). */
    static final Set<String> EMPTIED = Set.of(
            "android/app/ActivityThread.initZipPathValidatorCallback()V",
            "android/app/Activity.notifyVoiceInteractionManagerServiceActivityEvent(I)V");

    /** Classes an app process reaches on Android for the system's debugging, profiling and zygote
     *  plumbing (the DDM handshake, the binder GC watcher, the runtime's application object, the
     *  graphics driver's environment...), none of which Win.apk has or needs: every call to them
     *  answers its type's zero (a void does nothing). */
    static final Set<String> INERT_CLASSES = Set.of(
            "android/ddm/DdmHandleAppName",
            "com/android/internal/os/RuntimeInit",
            "com/android/internal/os/BinderInternal",
            "dalvik/system/VMDebug",
            "org/apache/harmony/dalvik/ddmc/DdmVmInternal",
            "dalvik/system/AppSpecializationHooks",
            "android/os/GraphicsEnvironment",
            "android/renderscript/RenderScriptCacheDir",
            "android/os/TransactionTracker",
            // StrictMode's VM policy hook in libcore (each private file path a context makes is
            // reported to it): no StrictMode VM policy is set in an app's run here.
            "dalvik/system/BlockGuard", "dalvik/system/BlockGuard$VmPolicy", "dalvik/system/BlockGuard$Policy",
            // The process's compat-change state (the change ids system_server disables for the app's
            // target SDK): Win.apk's CompatChanges answers every change enabled (aosp-views.md).
            "android/app/AppCompatCallbacks",
            // AOSP's managers' registry: ContextImpl's lookups are Win.apk's Context's (SERVICE_LOOKUPS),
            // so the per-context cache it makes for its fetchers is not needed.
            "android/app/SystemServiceRegistry",
            // The app's network security config installed as the TLS provider's trust manager:
            // Win.apk's TLS trust is the host's store, set by Boot for the process, as without this layer.
            "android/security/net/config/NetworkSecurityConfigProvider");

    /** Members (owner.name, any descriptor) Win.apk's classes lack whose work has no counterpart in
     *  this process, so every call answers its type's zero: the runtime's process bookkeeping (heap
     *  growth, process state and name, sensitive threads, start times), StrictMode's VM and thread
     *  defaults and its leak and intent checks (off, as in a user build without developer options),
     *  the parcel hooks of an Intent crossing a process (none crosses here), and the autofill /
     *  content-capture options system_server hands an app (none: no such services). */
    static final Set<String> INERT_METHODS = Set.of(
            "dalvik/system/VMRuntime.registerSensitiveThread", "dalvik/system/VMRuntime.setProcessPackageName",
            "dalvik/system/VMRuntime.updateProcessState", "dalvik/system/VMRuntime.clampGrowthLimit",
            "dalvik/system/VMRuntime.clearGrowthLimit", "dalvik/system/VMRuntime.registerAppInfo",
            "android/os/Process.setArgV0", "android/os/Process.setStartTimes", "android/os/Process.setProcessGroup",
            "android/os/Binder.enableStackTracking", "android/os/Binder.disableStackTracking",
            "android/os/Environment.initForCurrentUser", "android/os/Debug.suspendAllAndSendVmStart",
            "android/os/StrictMode.initThreadDefaults", "android/os/StrictMode.initVmDefaults",
            "android/os/StrictMode.incrementExpectedActivityCount",
            "android/os/StrictMode.decrementExpectedActivityCount",
            "android/os/StrictMode.allowThreadDiskWritesMask", "android/os/StrictMode.vmUnsafeIntentLaunchEnabled",
            "android/os/StrictMode.vmRegistrationLeaksEnabled", "android/os/StrictMode.onIntentReceiverLeaked",
            "android/os/StrictMode.onServiceConnectionLeaked",
            "java/text/DateFormat.set24HourTimePref", "android/net/TrafficStats.init",
            "android/net/Proxy.setHttpProxyConfiguration",
            "android/content/Intent.prepareToEnterProcess", "android/content/Intent.prepareToLeaveProcess",
            "android/content/Intent.migrateExtraStreamToClipData",
            "com/android/internal/content/ReferrerIntent.prepareToEnterProcess",
            "android/os/Bundle.setAllowFds", "android/content/pm/ApplicationInfo.initForUser",
            "android/content/pm/ApplicationInfo.requestsIsolatedSplitLoading",
            "android/app/ActivityManager.isProcStateCached",
            "android/app/Application.setAutofillOptions", "android/app/Application.setContentCaptureOptions",
            "android/app/Activity.setContentCaptureOptions", "android/content/Context.setAutofillClient",
            "android/app/Activity.getContentCaptureOptions", "android/app/Application.getContentCaptureOptions",
            "android/content/ContextWrapper.getAutofillClient", "android/app/Activity.updateDisplay",
            "android/os/StrictMode.trackActivity",
            "android/content/Context.setContentCaptureOptions", "android/content/Context.updateDeviceId",
            // HWUI's render thread is set up for the process at bind (step V3: every window draws in
            // software), and the system font map is Win.apk's graphics', set at the run's start as
            // the zygote preloads it on a device (AospGfx.materializeSystemFonts).
            "android/graphics/HardwareRenderer.setContextForInit",
            "android/graphics/HardwareRenderer.setDebuggingEnabled",
            "android/graphics/HardwareRenderer.setIsSystemOrPersistent",
            "android/graphics/HardwareRenderer.setIsolatedProcess",
            "android/graphics/HardwareRenderer.setPackageName", "android/graphics/HardwareRenderer.setupDiskCache",
            "android/graphics/HardwareRenderer.sendDeviceConfigurationForDebugging",
            "android/graphics/Typeface.loadPreinstalledSystemFontMap", "android/graphics/Typeface.setSystemFontMap",
            // The package manager is Win.apk's (MacApkPackageManager): its caches are its own.
            "android/app/ApplicationPackageManager.configurationChanged",
            // A private folder's owner and its xattrs (the cache quota's), and the external app
            // folders' ownership fix-up by vold: Windows' files have neither.
            "android/system/Os.chown", "android/system/Os.setxattr",
            "android/os/storage/StorageManager.fixupAppDir",
            // Win.apk's FontsContract finds the application itself.
            "android/provider/FontsContract.setApplicationContextForResources");

    /** Hidden fields (owner.name) Win.apk's classes lack, which hold nothing here: a read is the type's
     *  zero (null), a write is dropped. ApplicationInfo's ABIs and secondary library folder (Win.apk
     *  picks the app's native libraries itself), its class loader names and split dependencies (one
     *  class loader, the host's), shared libraries (none), its creation time. */
    static final Set<String> INERT_FIELDS = Set.of(
            "android/content/pm/ApplicationInfo.primaryCpuAbi", "android/content/pm/ApplicationInfo.secondaryCpuAbi",
            "android/content/pm/ApplicationInfo.secondaryNativeLibraryDir",
            "android/content/pm/ApplicationInfo.classLoaderName",
            "android/content/pm/ApplicationInfo.splitClassLoaderNames",
            "android/content/pm/ApplicationInfo.splitDependencies",
            "android/content/pm/ApplicationInfo.sharedLibraryInfos",
            "android/content/pm/ApplicationInfo.createTimestamp",
            "android/content/pm/InstrumentationInfo.primaryCpuAbi",
            "android/content/pm/InstrumentationInfo.secondaryCpuAbi",
            "android/content/pm/InstrumentationInfo.secondaryNativeLibraryDir",
            "android/content/pm/InstrumentationInfo.nativeLibraryDir");

    /** Hidden fields Win.apk's classes lack whose value follows from what they have: a read becomes a
     *  call of Compat's static of the same name, the object passed (StructStat.st_mtim, the
     *  modification time SharedPreferencesImpl compares, from st_mtime and st_mtime_nsec). */
    static final Set<String> FIELDS_TO_COMPAT = Set.of(
            "android/system/StructStat.st_mtim");

    /** ContextImpl's methods whose bodies become Win.apk's Context's: the lookups of system services
     *  (its managers are the host's; SystemServiceRegistry's fetchers are clients of system_server's),
     *  and the permission checks (Win.apk's answer from its signed grants, where
     *  AOSP's ask PermissionManager). */
    static final Set<String> SERVICE_LOOKUPS = Set.of(
            "getSystemService(Ljava/lang/String;)Ljava/lang/Object;",
            "getSystemServiceName(Ljava/lang/Class;)Ljava/lang/String;",
            "checkPermission(Ljava/lang/String;II)I",
            "checkSelfPermission(Ljava/lang/String;)I",
            "checkCallingPermission(Ljava/lang/String;)I",
            "checkCallingOrSelfPermission(Ljava/lang/String;)I",
            "enforcePermission(Ljava/lang/String;IILjava/lang/String;)V",
            "enforceCallingPermission(Ljava/lang/String;Ljava/lang/String;)V",
            "enforceCallingOrSelfPermission(Ljava/lang/String;Ljava/lang/String;)V",
            // Receivers, broadcasts and services: Win.apk's dispatch (its receiver registry, the
            // host's broadcasts, ServiceRegistry and the broker's services), until they are
            // ActivityThread's over the in-process activity manager (step A3).
            "registerReceiver(Landroid/content/BroadcastReceiver;Landroid/content/IntentFilter;)Landroid/content/Intent;",
            "registerReceiver(Landroid/content/BroadcastReceiver;Landroid/content/IntentFilter;I)Landroid/content/Intent;",
            "registerReceiver(Landroid/content/BroadcastReceiver;Landroid/content/IntentFilter;Ljava/lang/String;Landroid/os/Handler;)Landroid/content/Intent;",
            "registerReceiver(Landroid/content/BroadcastReceiver;Landroid/content/IntentFilter;Ljava/lang/String;Landroid/os/Handler;I)Landroid/content/Intent;",
            "unregisterReceiver(Landroid/content/BroadcastReceiver;)V",
            "sendBroadcast(Landroid/content/Intent;)V",
            "sendBroadcast(Landroid/content/Intent;Ljava/lang/String;)V",
            "sendBroadcast(Landroid/content/Intent;Ljava/lang/String;Landroid/os/Bundle;)V",
            "sendOrderedBroadcast(Landroid/content/Intent;ILjava/lang/String;Ljava/lang/String;Landroid/content/BroadcastReceiver;Landroid/os/Handler;Ljava/lang/String;Landroid/os/Bundle;Landroid/os/Bundle;)V",
            "sendOrderedBroadcast(Landroid/content/Intent;Ljava/lang/String;)V",
            "sendOrderedBroadcast(Landroid/content/Intent;Ljava/lang/String;Landroid/content/BroadcastReceiver;Landroid/os/Handler;ILjava/lang/String;Landroid/os/Bundle;)V",
            "sendOrderedBroadcast(Landroid/content/Intent;Ljava/lang/String;Landroid/os/Bundle;)V",
            "sendOrderedBroadcast(Landroid/content/Intent;Ljava/lang/String;Landroid/os/Bundle;Landroid/content/BroadcastReceiver;Landroid/os/Handler;ILjava/lang/String;Landroid/os/Bundle;)V",
            "sendOrderedBroadcast(Landroid/content/Intent;Ljava/lang/String;Ljava/lang/String;Landroid/content/BroadcastReceiver;Landroid/os/Handler;ILjava/lang/String;Landroid/os/Bundle;)V",
            "sendStickyBroadcast(Landroid/content/Intent;)V",
            "sendStickyBroadcast(Landroid/content/Intent;Landroid/os/Bundle;)V",
            "removeStickyBroadcast(Landroid/content/Intent;)V",
            "startService(Landroid/content/Intent;)Landroid/content/ComponentName;",
            "startForegroundService(Landroid/content/Intent;)Landroid/content/ComponentName;",
            "stopService(Landroid/content/Intent;)Z",
            "bindService(Landroid/content/Intent;ILjava/util/concurrent/Executor;Landroid/content/ServiceConnection;)Z",
            "bindService(Landroid/content/Intent;Landroid/content/Context$BindServiceFlags;Ljava/util/concurrent/Executor;Landroid/content/ServiceConnection;)Z",
            "bindService(Landroid/content/Intent;Landroid/content/ServiceConnection;I)Z",
            "bindService(Landroid/content/Intent;Landroid/content/ServiceConnection;Landroid/content/Context$BindServiceFlags;)Z",
            "bindIsolatedService(Landroid/content/Intent;ILjava/lang/String;Ljava/util/concurrent/Executor;Landroid/content/ServiceConnection;)Z",
            "bindIsolatedService(Landroid/content/Intent;Landroid/content/Context$BindServiceFlags;Ljava/lang/String;Ljava/util/concurrent/Executor;Landroid/content/ServiceConnection;)Z",
            "unbindService(Landroid/content/ServiceConnection;)V",
            "updateServiceGroup(Landroid/content/ServiceConnection;II)V");

    /** The in-process system services: AIDL interface -> the plain class that answers it
     *  (windows/java/aosp_app). */
    static final String[][] SERVICES = {
            {"android/app/IActivityManager", PKG + "ActivityManagerService"},
            {"android/app/IActivityTaskManager", PKG + "ActivityTaskService"},
            {"android/app/IActivityClientController", PKG + "ActivityClientService"},
            {"android/content/pm/IPackageManager", PKG + "PackageService"},
    };

    static final Map<String, Integer> counts = new TreeMap<>();
    static void count(String what) { counts.merge(what, 1, Integer::sum); }

    public static void main(String[] a) throws IOException {
        if (a.length != 1) throw new IllegalArgumentException("usage: AospAppRewrite <classes dir>");
        Path root = Path.of(a[0]);
        List<Path> files;
        try (Stream<Path> s = Files.walk(root)) {
            files = s.filter(p -> p.toString().endsWith(".class")).toList();
        }
        Map<String, Set<String>> bridges = new HashMap<>();
        Set<String> compat = Files.exists(root.resolve(COMPAT + ".class"))
                ? statics(root.resolve(COMPAT + ".class")) : Set.of();
        for (Path f : files) {
            String name = name(root, f);
            if (name.startsWith(PKG + "Native")) bridges.put(name, statics(f));
        }
        Set<String> unbodied = new TreeSet<>();
        for (Path f : files) {
            String name = name(root, f);
            if (name.startsWith(PKG)) continue;  // our own classes are written for HotSpot
            if (!name.startsWith("android/") && !name.startsWith("com/android/")) continue;
            byte[] in = Files.readAllBytes(f);
            ClassReader cr = new ClassReader(in);
            ClassWriter cw = new ClassWriter(cr, ClassWriter.COMPUTE_MAXS);
            String bridge = PKG + "Native" + name.substring(name.lastIndexOf('/') + 1).replace('$', '_');
            cr.accept(new Rewriter(cw, name, bridge, bridges.getOrDefault(bridge, Set.of()), compat, unbodied), 0);
            byte[] out = cw.toByteArray();
            if (!java.util.Arrays.equals(in, out)) Files.write(f, out);
        }
        for (String[] svc : SERVICES) {
            if (Files.exists(root.resolve(svc[1] + ".class"))) generateService(root, svc[0], svc[1]);
            else System.out.println("[aosp-app]   " + svc[0] + ": no " + svc[1] + ", not generated");
        }
        counts.forEach((k, v) -> System.out.println("[aosp-app]   " + v + " " + k));
        System.out.println("[aosp-app]   natives left throwing (by class): " + summarize(unbodied));
    }

    static String name(Path root, Path f) {
        String n = root.relativize(f).toString().replace('\\', '/');
        return n.substring(0, n.length() - ".class".length());
    }

    /** com/winapk/aospapp/gen/<I>Impl extends <I>$Stub (a local Binder whose queryLocalInterface
     *  answers itself), built with the handler: each method of the interface calls the handler's
     *  public method of the same name and descriptor when it has one, and otherwise reports the
     *  call once (Services.unanswered) and returns its type's zero. */
    static void generateService(Path root, String iface, String handler) throws IOException {
        Set<String> answered = new HashSet<>();
        new ClassReader(Files.readAllBytes(root.resolve(handler + ".class"))).accept(new ClassVisitor(Opcodes.ASM9) {
            @Override public MethodVisitor visitMethod(int acc, String n, String d, String s, String[] e) {
                if ((acc & Opcodes.ACC_PUBLIC) != 0 && (acc & Opcodes.ACC_STATIC) == 0) answered.add(n + d);
                return null;
            }
        }, ClassReader.SKIP_CODE);
        List<String[]> methods = new ArrayList<>();
        new ClassReader(Files.readAllBytes(root.resolve(iface + ".class"))).accept(new ClassVisitor(Opcodes.ASM9) {
            @Override public MethodVisitor visitMethod(int acc, String n, String d, String s, String[] e) {
                if ((acc & Opcodes.ACC_ABSTRACT) != 0) methods.add(new String[] {n, d});
                return null;
            }
        }, ClassReader.SKIP_CODE);
        String simple = iface.substring(iface.lastIndexOf('/') + 1);
        String gen = PKG + "gen/" + simple + "Impl";
        String stub = iface + "$Stub";
        // A Stub whose permissions are enforced has a constructor taking the PermissionEnforcer and a
        // no-argument one that makes it from the system context: the generated class hands it the
        // views' LocalEnforcer (aosp-views.jar) instead.
        boolean[] enforced = {false};
        new ClassReader(Files.readAllBytes(root.resolve(stub + ".class"))).accept(new ClassVisitor(Opcodes.ASM9) {
            @Override public MethodVisitor visitMethod(int acc, String n, String d, String s, String[] e) {
                if (n.equals("<init>") && d.equals("(Landroid/os/PermissionEnforcer;)V")) enforced[0] = true;
                return null;
            }
        }, ClassReader.SKIP_CODE);
        ClassWriter cw = new ClassWriter(ClassWriter.COMPUTE_MAXS);
        cw.visit(Opcodes.V17, Opcodes.ACC_PUBLIC | Opcodes.ACC_FINAL | Opcodes.ACC_SUPER, gen, null, stub, null);
        cw.visitField(Opcodes.ACC_PRIVATE | Opcodes.ACC_FINAL, "h", "L" + handler + ";", null, null).visitEnd();
        MethodVisitor init = cw.visitMethod(Opcodes.ACC_PUBLIC, "<init>", "(L" + handler + ";)V", null, null);
        init.visitCode();
        init.visitVarInsn(Opcodes.ALOAD, 0);
        if (enforced[0]) {
            String enforcer = "com/winapk/aospviews/LocalEnforcer";
            init.visitTypeInsn(Opcodes.NEW, enforcer);
            init.visitInsn(Opcodes.DUP);
            init.visitMethodInsn(Opcodes.INVOKESPECIAL, enforcer, "<init>", "()V", false);
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
                zero(mv, ret);
            }
            mv.visitMaxs(0, 0);
            mv.visitEnd();
        }
        cw.visitEnd();
        Path out = root.resolve(gen + ".class");
        Files.createDirectories(out.getParent());
        Files.write(out, cw.toByteArray());
        System.out.println("[aosp-app]   " + simple + ": " + answeredCount + " of " + methods.size()
                + " methods answered (" + handler.substring(handler.lastIndexOf('/') + 1) + ")");
    }

    static void zero(MethodVisitor mv, Type ret) {
        switch (ret.getSort()) {
            case Type.VOID -> mv.visitInsn(Opcodes.RETURN);
            case Type.LONG -> { mv.visitInsn(Opcodes.LCONST_0); mv.visitInsn(Opcodes.LRETURN); }
            case Type.FLOAT -> { mv.visitInsn(Opcodes.FCONST_0); mv.visitInsn(Opcodes.FRETURN); }
            case Type.DOUBLE -> { mv.visitInsn(Opcodes.DCONST_0); mv.visitInsn(Opcodes.DRETURN); }
            case Type.OBJECT, Type.ARRAY -> { mv.visitInsn(Opcodes.ACONST_NULL); mv.visitInsn(Opcodes.ARETURN); }
            default -> { mv.visitInsn(Opcodes.ICONST_0); mv.visitInsn(Opcodes.IRETURN); }
        }
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
        final Set<String> bridged, compat, unbodied;

        Rewriter(ClassVisitor cv, String cls, String bridge, Set<String> bridged, Set<String> compat,
                 Set<String> unbodied) {
            super(Opcodes.ASM9, cv);
            this.cls = cls;
            this.bridge = bridge;
            this.bridged = bridged;
            this.compat = compat;
            this.unbodied = unbodied;
        }

        /** ActivityThread.main's two steps for the host's main thread (com.winapk.aospapp.AospApp):
         *  `public static ActivityThread winapkAttach(long startSeq)` makes the thread and attaches it
         *  as an app process (attach(false, startSeq), private on AOSP). Added rather than reached by
         *  reflection, which loads every type the class's methods name. */
        @Override
        public void visitEnd() {
            if (cls.equals("android/app/ActivityThread")) {
                String at = "L" + cls + ";";
                MethodVisitor mv = super.visitMethod(Opcodes.ACC_PUBLIC | Opcodes.ACC_STATIC, "winapkAttach",
                        "(J)" + at, null, null);
                mv.visitCode();
                mv.visitTypeInsn(Opcodes.NEW, cls);
                mv.visitInsn(Opcodes.DUP);
                mv.visitMethodInsn(Opcodes.INVOKESPECIAL, cls, "<init>", "()V", false);
                mv.visitVarInsn(Opcodes.ASTORE, 2);
                mv.visitVarInsn(Opcodes.ALOAD, 2);
                mv.visitInsn(Opcodes.ICONST_0);
                mv.visitVarInsn(Opcodes.LLOAD, 0);
                mv.visitMethodInsn(Opcodes.INVOKESPECIAL, cls, "attach", "(ZJ)V", false);
                mv.visitVarInsn(Opcodes.ALOAD, 2);
                mv.visitInsn(Opcodes.ARETURN);
                mv.visitMaxs(0, 0);
                mv.visitEnd();
                count("ActivityThread.winapkAttach added");
            }
            for (String m : NULL_METHODS.getOrDefault(cls, List.of())) {
                String n = m.substring(0, m.indexOf('(')), d = m.substring(m.indexOf('('));
                MethodVisitor mv = super.visitMethod(Opcodes.ACC_PUBLIC, n, d, null, null);
                mv.visitCode();
                zero(mv, Type.getReturnType(d));
                mv.visitMaxs(0, 0);
                mv.visitEnd();
                count("methods answering zero added to " + cls.substring(cls.lastIndexOf('/') + 1));
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
                    zero(mv, ret);
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
            if (BODY_TO_COMPAT.contains(cls + "." + name + desc)) {
                boolean isStatic = (acc & Opcodes.ACC_STATIC) != 0;
                // ContextImpl is package-private: Compat takes it as a Context.
                String self = cls.equals("android/app/ContextImpl") ? "android/content/Context" : cls;
                String cd = isStatic ? desc : "(L" + self + ";" + desc.substring(1);
                if (!compat.contains(name + cd)) {
                    throw new IllegalStateException("Compat has no public static " + name + cd);
                }
                MethodVisitor mv = super.visitMethod(acc, name, desc, sig, exc);
                mv.visitCode();
                int slot = 0;
                if (!isStatic) mv.visitVarInsn(Opcodes.ALOAD, slot++);
                for (Type t : Type.getArgumentTypes(desc)) {
                    mv.visitVarInsn(t.getOpcode(Opcodes.ILOAD), slot);
                    slot += t.getSize();
                }
                mv.visitMethodInsn(Opcodes.INVOKESTATIC, COMPAT, name, cd, false);
                mv.visitInsn(Type.getReturnType(desc).getOpcode(Opcodes.IRETURN));
                mv.visitMaxs(0, 0);
                mv.visitEnd();
                count("method bodies sent to Compat");
                return null;
            }
            if (EMPTIED.contains(cls + "." + name + desc)) {
                MethodVisitor mv = super.visitMethod(acc, name, desc, sig, exc);
                mv.visitCode();
                mv.visitInsn(Opcodes.RETURN);
                mv.visitMaxs(0, 0);
                mv.visitEnd();
                count("methods emptied");
                return null;
            }
            if (cls.equals("android/app/ContextImpl") && SERVICE_LOOKUPS.contains(name + desc)) {
                // ContextImpl looks managers up in SystemServiceRegistry, whose fetchers construct
                // AOSP's managers over system_server's binders. Win.apk's managers (the host's audio,
                // network, notifications...) are what Win.apk's Context hands out, AOSP's views'
                // through ViewHost first: the lookup is Context's own body.
                MethodVisitor mv = super.visitMethod(acc, name, desc, sig, exc);
                mv.visitCode();
                mv.visitVarInsn(Opcodes.ALOAD, 0);
                int slot = 1;
                for (Type t : Type.getArgumentTypes(desc)) {
                    mv.visitVarInsn(t.getOpcode(Opcodes.ILOAD), slot);
                    slot += t.getSize();
                }
                mv.visitMethodInsn(Opcodes.INVOKESPECIAL, "android/content/Context", name, desc, false);
                mv.visitInsn(Type.getReturnType(desc).getOpcode(Opcodes.IRETURN));
                mv.visitMaxs(0, 0);
                mv.visitEnd();
                count("ContextImpl methods whose body is Win.apk's Context's");
                return null;
            }
            if (cls.equals("android/app/LoadedApk") && name.equals("createOrUpdateClassLoaderLocked")
                    && desc.equals("(Ljava/util/List;)V")) {
                // LoadedApk makes the app's PathClassLoader over its APK (ApplicationLoaders); Win.apk's
                // app classes are the host's, converted from DEX (Boot's AppClassLoader). The loader is
                // the host's, and the app's AppComponentFactory is made with it, as the method does at
                // its end on Android.
                String cl = "Ljava/lang/ClassLoader;";
                MethodVisitor mv = super.visitMethod(acc, name, desc, sig, exc);
                mv.visitCode();
                mv.visitVarInsn(Opcodes.ALOAD, 0);
                mv.visitVarInsn(Opcodes.ALOAD, 0);
                mv.visitMethodInsn(Opcodes.INVOKESTATIC, COMPAT, "appClassLoader", "(L" + cls + ";)" + cl, false);
                mv.visitFieldInsn(Opcodes.PUTFIELD, cls, "mClassLoader", cl);
                mv.visitVarInsn(Opcodes.ALOAD, 0);
                mv.visitVarInsn(Opcodes.ALOAD, 0);
                mv.visitFieldInsn(Opcodes.GETFIELD, cls, "mClassLoader", cl);
                mv.visitFieldInsn(Opcodes.PUTFIELD, cls, "mDefaultClassLoader", cl);
                mv.visitVarInsn(Opcodes.ALOAD, 0);
                mv.visitVarInsn(Opcodes.ALOAD, 0);
                mv.visitVarInsn(Opcodes.ALOAD, 0);
                mv.visitFieldInsn(Opcodes.GETFIELD, cls, "mApplicationInfo", "Landroid/content/pm/ApplicationInfo;");
                mv.visitVarInsn(Opcodes.ALOAD, 0);
                mv.visitFieldInsn(Opcodes.GETFIELD, cls, "mClassLoader", cl);
                mv.visitMethodInsn(Opcodes.INVOKESPECIAL, cls, "createAppFactory",
                        "(Landroid/content/pm/ApplicationInfo;" + cl + ")Landroid/app/AppComponentFactory;", false);
                mv.visitFieldInsn(Opcodes.PUTFIELD, cls, "mAppComponentFactory", "Landroid/app/AppComponentFactory;");
                mv.visitInsn(Opcodes.RETURN);
                mv.visitMaxs(0, 0);
                mv.visitEnd();
                count("LoadedApk's class loader made the host's");
                return null;
            }
            return new MethodVisitor(Opcodes.ASM9, super.visitMethod(acc, name, desc, sig, exc)) {
                @Override public void visitMethodInsn(int op, String owner, String n, String d, boolean itf) {
                    if ((INERT_CLASSES.contains(owner) || INERT_METHODS.contains(owner + "." + n))
                            && op != Opcodes.INVOKESPECIAL && !TO_COMPAT.contains(owner + "." + n + d)) {
                        // A call into a class with nothing behind it in this process (debugging,
                        // profiling, the zygote's hooks): its arguments (and receiver) dropped, its
                        // type's zero answered.
                        Type[] args = Type.getArgumentTypes(d);
                        for (int i = args.length - 1; i >= 0; i--) {
                            super.visitInsn(args[i].getSize() == 2 ? Opcodes.POP2 : Opcodes.POP);
                        }
                        if (op != Opcodes.INVOKESTATIC) super.visitInsn(Opcodes.POP);
                        Type ret = Type.getReturnType(d);
                        switch (ret.getSort()) {
                            case Type.VOID -> { }
                            case Type.LONG -> super.visitInsn(Opcodes.LCONST_0);
                            case Type.FLOAT -> super.visitInsn(Opcodes.FCONST_0);
                            case Type.DOUBLE -> super.visitInsn(Opcodes.DCONST_0);
                            case Type.OBJECT, Type.ARRAY -> super.visitInsn(Opcodes.ACONST_NULL);
                            default -> super.visitInsn(Opcodes.ICONST_0);
                        }
                        count("calls to " + owner.substring(owner.lastIndexOf('/') + 1) + " made inert");
                        return;
                    }
                    if (TO_COMPAT.contains(owner + "." + n + d)) {
                        String cd = op == Opcodes.INVOKESTATIC ? d : "(L" + owner + ";" + d.substring(1);
                        if (!compat.contains(n + cd)) {
                            throw new IllegalStateException("Compat has no public static " + n + cd
                                    + " for " + owner);
                        }
                        super.visitMethodInsn(Opcodes.INVOKESTATIC, COMPAT, n, cd, false);
                        count("calls to " + owner.substring(owner.lastIndexOf('/') + 1) + "." + n + " redirected");
                        return;
                    }
                    super.visitMethodInsn(op, owner, n, d, itf);
                }

                @Override public void visitFieldInsn(int op, String owner, String n, String d) {
                    if (op == Opcodes.GETFIELD && FIELDS_TO_COMPAT.contains(owner + "." + n)) {
                        String cd = "(L" + owner + ";)" + d;
                        if (!compat.contains(n + cd)) {
                            throw new IllegalStateException("Compat has no public static " + n + cd);
                        }
                        super.visitMethodInsn(Opcodes.INVOKESTATIC, COMPAT, n, cd, false);
                        count("reads of " + owner.substring(owner.lastIndexOf('/') + 1) + "." + n + " sent to Compat");
                        return;
                    }
                    if (INERT_FIELDS.contains(owner + "." + n)
                            && (op == Opcodes.GETFIELD || op == Opcodes.PUTFIELD)) {
                        Type t = Type.getType(d);
                        if (op == Opcodes.GETFIELD) {
                            super.visitInsn(Opcodes.POP);
                            switch (t.getSort()) {
                                case Type.LONG -> super.visitInsn(Opcodes.LCONST_0);
                                case Type.FLOAT -> super.visitInsn(Opcodes.FCONST_0);
                                case Type.DOUBLE -> super.visitInsn(Opcodes.DCONST_0);
                                case Type.OBJECT, Type.ARRAY -> super.visitInsn(Opcodes.ACONST_NULL);
                                default -> super.visitInsn(Opcodes.ICONST_0);
                            }
                        } else {
                            super.visitInsn(t.getSize() == 2 ? Opcodes.POP2 : Opcodes.POP);
                            super.visitInsn(Opcodes.POP);
                        }
                        count("accesses to fields Win.apk's classes lack made inert");
                        return;
                    }
                    super.visitFieldInsn(op, owner, n, d);
                }
            };
        }
    }
}
