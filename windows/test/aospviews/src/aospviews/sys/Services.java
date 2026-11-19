package aospviews.sys;

import android.os.Binder;
import android.os.IBinder;
import android.os.IInterface;

import java.lang.reflect.Proxy;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * The system services AOSP's app-side code reaches through Binder (ServiceManager.getService(name)
 * then IXxx.Stub.asInterface), answered in-process: the service manager is a local Binder whose
 * IServiceManager hands out, by name, a local Binder carrying a java.lang.reflect.Proxy of the
 * service's AIDL interface. Each call is recorded (`calls()`) and answered with the type's empty
 * value (false, 0, null), unless an answer is registered. This is where Win.apk's own services
 * (com.macapk.loader: the window manager, IME, accessibility, package manager...) would plug in:
 * the same names and AIDL interfaces, implemented over the host. Nothing here crosses a process.
 */
public final class Services {
    private Services() {}

    /** Service name -> its AIDL interface, for the services the spike's views have been seen to ask for. */
    static final Map<String, String> INTERFACES = new HashMap<>();
    static {
        INTERFACES.put("window", "android.view.IWindowManager");
        INTERFACES.put("input_method", "com.android.internal.view.IInputMethodManager");
        INTERFACES.put("accessibility", "android.view.accessibility.IAccessibilityManager");
        INTERFACES.put("display", "android.hardware.display.IDisplayManager");
        INTERFACES.put("package", "android.content.pm.IPackageManager");
        INTERFACES.put("permission", "android.os.IPermissionController");
        INTERFACES.put("autofill", "android.view.autofill.IAutoFillManager");
        INTERFACES.put("textservices", "com.android.internal.textservice.ITextServicesManager");
        INTERFACES.put("uimode", "android.app.IUiModeManager");
        INTERFACES.put("activity_task", "android.app.IActivityTaskManager");
        INTERFACES.put("activity", "android.app.IActivityManager");
        INTERFACES.put("power", "android.os.IPowerManager");
    }

    /** "service.method" -> an answer other than the empty one. */
    static final Map<String, Object> ANSWERS = new HashMap<>();

    private static final Map<String, Integer> CALLS = new LinkedHashMap<>();
    private static final List<String> UNKNOWN = new ArrayList<>();
    private static final Map<String, IBinder> BOUND = new HashMap<>();

    public static synchronized Map<String, Integer> calls() { return new LinkedHashMap<>(CALLS); }
    public static synchronized List<String> unknown() { return new ArrayList<>(UNKNOWN); }
    public static void answer(String serviceDotMethod, Object value) { ANSWERS.put(serviceDotMethod, value); }

    private static synchronized void record(String what) { CALLS.merge(what, 1, Integer::sum); }

    /** BinderInternal.getContextObject's answer: the service manager. */
    static IBinder serviceManager() {
        return local("servicemanager", "android.os.IServiceManager", (name, m, args) -> {
            if (m.equals("getService") || m.equals("checkService")) return service((String) args[0]);
            return NO_ANSWER;
        });
    }

    static synchronized IBinder service(String name) {
        IBinder b = BOUND.get(name);
        if (b != null) return b;
        String iface = INTERFACES.get(name);
        if (iface == null) {
            if (!UNKNOWN.contains(name)) UNKNOWN.add(name);
            return null;
        }
        b = local(name, iface, null);
        BOUND.put(name, b);
        return b;
    }

    interface Handler { Object call(String service, String method, Object[] args); }
    private static final Object NO_ANSWER = new Object();

    static IBinder local(String name, String iface, Handler h) {
        try {
            Class<?> c = Class.forName(iface);
            Binder binder = new Binder();
            Object proxy = Proxy.newProxyInstance(c.getClassLoader(), new Class<?>[] {c}, (p, m, args) -> {
                if (m.getName().equals("asBinder")) return binder;
                if (m.getDeclaringClass() == Object.class) {
                    switch (m.getName()) {
                        case "hashCode": return System.identityHashCode(p);
                        case "equals": return p == args[0];
                        default: return name + " (" + iface + ")";
                    }
                }
                record(name + "." + m.getName());
                if (h != null) {
                    Object r = h.call(name, m.getName(), args);
                    if (r != NO_ANSWER) return r;
                }
                Object a = ANSWERS.get(name + "." + m.getName());
                if (a != null) return a;
                return empty(m.getReturnType());
            });
            String descriptor;
            try {
                descriptor = (String) c.getField("DESCRIPTOR").get(null);
            } catch (NoSuchFieldException e) {  // older AIDL: on the Stub
                java.lang.reflect.Field f = Class.forName(iface + "$Stub").getDeclaredField("DESCRIPTOR");
                f.setAccessible(true);
                descriptor = (String) f.get(null);
            }
            binder.attachInterface((IInterface) proxy, descriptor);
            return binder;
        } catch (ReflectiveOperationException e) {
            throw new IllegalStateException(iface, e);
        }
    }

    /** An object of interface c that records each call as name.method and answers it empty (no Binder
     *  behind it): for interfaces handed around without ServiceManager, like a ContentResolver's
     *  IContentProvider. */
    @SuppressWarnings("unchecked")
    public static <T> T stub(Class<T> c, String name) {
        Binder binder = new Binder();
        return (T) Proxy.newProxyInstance(c.getClassLoader(), new Class<?>[] {c}, (p, m, args) -> {
            if (m.getName().equals("asBinder")) return binder;
            if (m.getDeclaringClass() == Object.class) {
                switch (m.getName()) {
                    case "hashCode": return System.identityHashCode(p);
                    case "equals": return p == args[0];
                    default: return name;
                }
            }
            record(name + "." + m.getName());
            return empty(m.getReturnType());
        });
    }

    static Object empty(Class<?> t) {
        if (!t.isPrimitive()) return null;
        if (t == boolean.class) return false;
        if (t == void.class) return null;
        if (t == long.class) return 0L;
        if (t == float.class) return 0f;
        if (t == double.class) return 0d;
        if (t == char.class) return (char) 0;
        if (t == byte.class) return (byte) 0;
        if (t == short.class) return (short) 0;
        return 0;
    }
}
