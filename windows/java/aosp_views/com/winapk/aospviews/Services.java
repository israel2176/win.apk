// In-process system services for AOSP's views: what their code finds through ServiceManager on
// Android (system_server's window and display managers), answered in this process. Robolectric
// answers the same calls with shadows; Win.apk cannot instrument, so each service is a real
// implementation of its AIDL interface: AospViewsRewrite.java (build time) generates
// com.winapk.aospviews.gen.<Interface>Impl, a subclass of the interface's Stub (so a local Binder
// whose queryLocalInterface answers itself, with no parcel in between) whose methods call the
// public method of the same name and descriptor on a plain Java object (AospWindows'
// WindowManagerService, WindowSession, DisplayService), and give the return type's empty answer
// when that object has none (logged once per method, so what an app needs next shows up by name).
package com.winapk.aospviews;

import android.os.IBinder;
import android.os.IInterface;
import android.os.ServiceManager;

import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;

public final class Services {
    private Services() {}

    private static final Set<String> sLogged = ConcurrentHashMap.newKeySet();

    /** The generated implementation of the AIDL interface `iface` over `handler`. */
    static <T extends IInterface> T of(Class<T> iface, Object handler) {
        String gen = "com.winapk.aospviews.gen." + iface.getSimpleName() + "Impl";
        try {
            Object impl = Class.forName(gen).getConstructor(handler.getClass()).newInstance(handler);
            return iface.cast(impl);
        } catch (ReflectiveOperationException e) {
            throw new IllegalStateException("no " + gen + " (windows/scripts/AospViewsRewrite.java makes it)", e);
        }
    }

    /** `of`, registered under `name` for ServiceManager.getService. */
    static <T extends IInterface> T register(String name, Class<T> iface, Object handler) {
        T service = of(iface, handler);
        IBinder binder = service.asBinder();
        ServiceManager.addService(name, binder);
        return service;
    }

    /** A method of a service with no answer here: its type's empty one (the generated code returns
     *  it), noted once. */
    public static void unanswered(String method) {
        if (sLogged.add(method)) System.err.println("[aosp-views] " + method + ": no answer in this process, the empty one");
    }
}
