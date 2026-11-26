// The services of the app's process, as ActivityManagerService's ActiveServices keeps them: a record
// per component (started or not, its bindings per intent, the connections on each), and the client
// transactions ActivityThread gets for them (scheduleCreateService, scheduleServiceArgs,
// scheduleBindService, scheduleUnbindService, scheduleStopService), which run the app's Service the
// way a device runs it. A service of another app is the system broker's (com.macapk.loader.SystemClient,
// Binder over named pipes); one of this app's that the broker binds into it, a job's and an account
// authenticator's are bound here "now" (the host waits for the binder or the instance): on the main
// thread ActivityThread's own handleCreateService and handleBindService run in place, from any other
// thread they are posted there and waited for.
package com.winapk.aospapp;

import android.app.ActivityThread;
import android.app.IApplicationThread;
import android.app.IServiceConnection;
import android.app.Service;
import android.app.ServiceStartArgs;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.content.pm.PackageManager;
import android.content.pm.ParceledListSlice;
import android.content.pm.ResolveInfo;
import android.content.pm.ServiceInfo;
import android.content.res.CompatibilityInfo;
import android.os.Binder;
import android.os.IBinder;
import android.os.Looper;
import android.os.RemoteException;

import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

final class ActiveServices {
    /** ActivityManager.PROCESS_STATE_SERVICE: what the process is while it runs a service. */
    private static final int PROCESS_STATE_SERVICE = 10;

    static final class ServiceRecord {
        final ComponentName name;
        final ServiceInfo info;
        final IBinder token = new Binder();
        boolean created;
        boolean startRequested;
        int lastStartId;
        final Map<Intent.FilterComparison, IntentBinding> bindings = new HashMap<>();
        /** Binds the host holds: the broker's into this app, an account authenticator's... */
        int hostBindings;
        int jobBindings;
        int foregroundId;
        int foregroundType;

        ServiceRecord(ComponentName name, ServiceInfo info) {
            this.name = name;
            this.info = info;
        }

        boolean hasConnections() {
            for (IntentBinding b : bindings.values()) {
                for (Connection c : b.connections) if ((c.flags & Context.BIND_AUTO_CREATE) != 0) return true;
            }
            return false;
        }
    }

    /** IntentBindRecord: one intent's binding of a service, and the binder its onBind returned. The
     *  binder outlives its connections while the service lives: onBind runs once per intent, a later
     *  client gets the same binder, and onRebind runs only when onUnbind asked for it. */
    static final class IntentBinding {
        final Intent intent;
        boolean requested;
        boolean received;
        /** onBind (or onRebind) ran and onUnbind has not: the last connection's going calls it. */
        boolean hasBound;
        /** onUnbind returned true: the next client gets onRebind (unbindFinished). */
        boolean doRebind;
        IBinder binder;
        final List<Connection> connections = new ArrayList<>();
        final List<CountDownLatch> waiting = new ArrayList<>();

        IntentBinding(Intent intent) { this.intent = intent; }
    }

    /** ConnectionRecord. */
    static final class Connection {
        final IServiceConnection conn;
        final ServiceRecord service;
        final IntentBinding binding;
        final long flags;

        Connection(IServiceConnection conn, ServiceRecord service, IntentBinding binding, long flags) {
            this.conn = conn;
            this.service = service;
            this.binding = binding;
            this.flags = flags;
        }
    }

    private final AospApp app;
    private final Map<ComponentName, ServiceRecord> byName = new HashMap<>();
    private final Map<IBinder, ServiceRecord> byToken = new HashMap<>();
    private final Map<IBinder, List<Connection>> byConnection = new HashMap<>();
    /** A connection to another app's service: the broker's, through a ServiceConnection. */
    private final Map<IBinder, ServiceConnection> remote = new HashMap<>();

    ActiveServices(AospApp app) { this.app = app; }

    // ---- Resolution (ActiveServices.retrieveServiceLocked) ----

    private static Context context() {
        return ActivityThread.currentApplication();
    }

    /** Another app's service: the system broker's to start or bind (SystemClient). */
    private static boolean isRemote(Intent intent) {
        Context ctx = context();
        try {
            return ctx != null && com.macapk.loader.SystemClient.bindsRemotely(ctx, intent);
        } catch (LinkageError e) {
            return false;   // a JVM without the loader (the plain-JVM test)
        }
    }

    /** The app's service an intent names: the package manager's answer, or, for a class of the
     *  app's that the manifest does not declare (the runtime's own services the app reaches),
     *  a ServiceInfo of its own, as the old host allowed. Null: nothing of the app's, or declared
     *  and disabled (a no-op on a device). */
    private ServiceRecord retrieve(Intent intent, boolean create) {
        PackageManager pm = app.packageManager;
        ComponentName cn = intent.getComponent();
        ServiceInfo si = null;
        try {
            if (cn != null) {
                if (!app.info.packageName.equals(cn.getPackageName())) return null;
                ServiceRecord r = byName.get(cn);
                if (r != null || !create) return r;
                try {
                    si = pm.getServiceInfo(cn, PackageManager.GET_META_DATA);
                } catch (PackageManager.NameNotFoundException e) {
                    try {
                        pm.getServiceInfo(cn, PackageManager.MATCH_DISABLED_COMPONENTS);
                        System.err.println("[aosp-app] " + cn.getClassName() + " is declared android:enabled=false:"
                                + " not started");
                        return null;
                    } catch (PackageManager.NameNotFoundException undeclared) {
                        si = undeclared(cn);
                    }
                }
            } else {
                ResolveInfo ri = pm.resolveService(intent, PackageManager.GET_META_DATA);
                if (ri == null || ri.serviceInfo == null) return null;
                si = ri.serviceInfo;
                cn = new ComponentName(si.packageName != null ? si.packageName : app.info.packageName, si.name);
                ServiceRecord r = byName.get(cn);
                if (r != null || !create) return r;
            }
        } catch (RuntimeException e) {
            System.err.println("[aosp-app] resolving the service " + intent + " threw " + e);
            return null;
        }
        if (si == null) return null;
        if (runtimeOwn(si)) si = hostedHere(si);
        if (si.applicationInfo == null) si.applicationInfo = app.info;
        if (si.packageName == null) si.packageName = app.info.packageName;
        if (si.processName == null) si.processName = app.info.processName;
        ServiceRecord r = new ServiceRecord(cn, si);
        byName.put(cn, r);
        byToken.put(r.token, r);
        return r;
    }

    /** Whether `si` is one of the runtime's own services standing in for another package's (the
     *  register and measurement services the package manager resolves for com.google.android.gms
     *  when Play services is not installed): a class of the runtime, not of any installed app. On
     *  a device that package's process would run it; here it runs in the app's, as the old host
     *  ran it. AOSP's handleCreateService would otherwise build that package's LoadedApk, and
     *  there is no such package to load. */
    private boolean runtimeOwn(ServiceInfo si) {
        if (si.packageName == null || si.packageName.equals(app.info.packageName)) return false;
        try {
            Class<?> c = Class.forName(si.name, false, app.classLoader);
            ClassLoader own = c.getClassLoader();
            return Service.class.isAssignableFrom(c) && own != app.classLoader
                    && own == ActiveServices.class.getClassLoader();
        } catch (ClassNotFoundException | LinkageError e) {
            return false;
        }
    }

    /** A copy of `si` that runs with the app's ApplicationInfo and process; its component name
     *  (the package it answers for) stays. */
    private ServiceInfo hostedHere(ServiceInfo si) {
        ServiceInfo here = new ServiceInfo(si);
        here.applicationInfo = app.info;
        here.processName = app.info.processName;
        return here;
    }

    private ServiceInfo undeclared(ComponentName cn) {
        try {
            Class<?> c = Class.forName(cn.getClassName(), false, app.classLoader);
            if (!Service.class.isAssignableFrom(c)) return null;
        } catch (ClassNotFoundException | LinkageError e) {
            return null;
        }
        ServiceInfo si = new ServiceInfo();
        si.name = cn.getClassName();
        si.packageName = cn.getPackageName();
        si.applicationInfo = app.info;
        si.processName = app.info.processName;
        si.enabled = true;
        return si;
    }

    private IApplicationThread thread() { return app.appThread; }

    // ---- Context.startService / stopService / Service.stopSelf ----

    synchronized ComponentName startService(Intent intent) {
        if (isRemote(intent)) return com.macapk.loader.SystemClient.startRemote(intent);
        ServiceRecord r = retrieve(intent, true);
        if (r == null) return null;
        r.startRequested = true;
        int startId = ++r.lastStartId;
        bringUp(r);
        try {
            thread().scheduleServiceArgs(r.token,
                    new ParceledListSlice<>(List.of(new ServiceStartArgs(false, startId, 0, intent))));
        } catch (RemoteException e) {
            throw new IllegalStateException(e);
        }
        return r.name;
    }

    synchronized int stopService(Intent intent) {
        if (isRemote(intent)) return com.macapk.loader.SystemClient.stopRemote(intent) ? 1 : 0;
        ServiceRecord r = retrieve(intent, false);
        if (r == null) return 0;
        r.startRequested = false;
        maybeStop(r);
        return 1;
    }

    /** ActiveServices.stopServiceTokenLocked: only the newest start stops it (or startId < 0). */
    synchronized boolean stopServiceToken(ComponentName name, IBinder token, int startId) {
        ServiceRecord r = byToken.get(token);
        if (r == null) return false;
        if (startId >= 0 && startId != r.lastStartId) return false;
        r.startRequested = false;
        maybeStop(r);
        return true;
    }

    synchronized void setServiceForeground(IBinder token, int id, int type) {
        ServiceRecord r = byToken.get(token);
        if (r == null) return;
        r.foregroundId = id;
        r.foregroundType = id != 0 ? type : 0;
    }

    synchronized int getForegroundServiceType(IBinder token) {
        ServiceRecord r = byToken.get(token);
        return r != null ? r.foregroundType : 0;
    }

    // ---- Context.bindService / unbindService ----

    synchronized int bindService(Intent intent, IServiceConnection conn, long flags) {
        if (isRemote(intent)) return bindRemote(intent, conn, flags) ? 1 : 0;
        ServiceRecord r = retrieve(intent, true);
        if (r == null) return 0;
        IntentBinding b = r.bindings.computeIfAbsent(new Intent.FilterComparison(new Intent(intent)),
                k -> new IntentBinding(k.getIntent()));
        Connection c = new Connection(conn, r, b, flags);
        b.connections.add(c);
        byConnection.computeIfAbsent(conn.asBinder(), k -> new ArrayList<>()).add(c);
        if ((flags & Context.BIND_AUTO_CREATE) != 0) bringUp(r);
        if (r.created) {
            if (b.received) {
                // ActiveServices.bindServiceLocked: the published binder at once; onRebind only for
                // the first client after an onUnbind that returned true.
                connected(c, b.binder);
                if (b.connections.size() == 1 && b.doRebind) requestBind(r, b, true);
            } else if (!b.requested) {
                requestBind(r, b, false);
            }
        }
        return 1;
    }

    synchronized boolean unbindService(IServiceConnection conn) {
        ServiceConnection sc = remote.remove(conn.asBinder());
        if (sc != null) {
            com.macapk.loader.SystemClient.unbindRemote(sc);
            return true;
        }
        List<Connection> list = byConnection.remove(conn.asBinder());
        if (list == null) return false;
        for (Connection c : list) {
            IntentBinding b = c.binding;
            b.connections.remove(c);
            if (b.connections.isEmpty() && b.hasBound && c.service.created) {
                // ActiveServices.removeConnectionLocked: the last connection of this intent gone,
                // onUnbind runs; the binder stays published for the next client.
                b.hasBound = false;
                b.doRebind = false;
                try {
                    thread().scheduleUnbindService(c.service.token, b.intent);
                } catch (RemoteException e) {
                    throw new IllegalStateException(e);
                }
            }
            maybeStop(c.service);
        }
        return true;
    }

    synchronized void publishService(IBinder token, Intent intent, IBinder service) {
        ServiceRecord r = byToken.get(token);
        if (r == null) return;
        IntentBinding b = r.bindings.get(new Intent.FilterComparison(intent));
        if (b == null) return;
        b.binder = service;
        b.received = true;
        for (Connection c : new ArrayList<>(b.connections)) connected(c, service);
        for (CountDownLatch l : b.waiting) l.countDown();
        b.waiting.clear();
    }

    /** onUnbind returned true (ActiveServices.unbindFinishedLocked): a client that bound since gets
     *  onRebind now, else the next one will. */
    synchronized void unbindFinished(IBinder token, Intent intent, boolean doRebind) {
        ServiceRecord r = byToken.get(token);
        if (r == null || !doRebind) return;
        IntentBinding b = r.bindings.get(new Intent.FilterComparison(intent));
        if (b == null) return;
        if (!b.connections.isEmpty() && r.created) requestBind(r, b, true);
        else b.doRebind = true;
    }

    synchronized IBinder peekService(Intent intent) {
        ServiceRecord r = retrieve(intent, false);
        if (r == null) return null;
        IntentBinding b = r.bindings.get(new Intent.FilterComparison(intent));
        return b != null && b.received ? b.binder : null;
    }

    private void connected(Connection c, IBinder binder) {
        try {
            c.conn.connected(c.service.name, binder, false);
        } catch (RemoteException | RuntimeException e) {
            System.err.println("[aosp-app] the connection to " + c.service.name.getClassName() + " threw " + e);
        }
    }

    /** Another app's service: the broker binds it; its answer goes to the IServiceConnection as
     *  ActiveServices would send it (connected, a dead binder, a null binding). */
    private boolean bindRemote(Intent intent, IServiceConnection conn, long flags) {
        ServiceConnection sc = new ServiceConnection() {
            @Override public void onServiceConnected(ComponentName name, IBinder service) {
                try { conn.connected(name, service, false); } catch (RemoteException ignored) {}
            }

            @Override public void onServiceDisconnected(ComponentName name) {
                try { conn.connected(name, null, true); } catch (RemoteException ignored) {}
            }

            @Override public void onBindingDied(ComponentName name) {
                try { conn.connected(name, null, true); } catch (RemoteException ignored) {}
            }

            @Override public void onNullBinding(ComponentName name) {
                try { conn.connected(name, null, false); } catch (RemoteException ignored) {}
            }
        };
        Context ctx = context();
        if (!com.macapk.loader.SystemClient.bindRemote(ctx, intent, (int) flags, Runnable::run, sc)) return false;
        remote.put(conn.asBinder(), sc);
        return true;
    }

    // ---- The record's lifecycle (bringUpServiceLocked, bringDownServiceLocked) ----

    /** The create goes to the main thread as CREATE_SERVICE would (a message in order with the
     *  client transactions that follow it), running ActivityThread's handleCreateService; posted
     *  rather than scheduled through the ApplicationThread so that a bind the host waits for on
     *  the main thread can create it in place first without a second instance. */
    private void bringUp(ServiceRecord r) {
        if (r.created) return;
        r.created = true;
        new android.os.Handler(Looper.getMainLooper()).post(() -> {
            synchronized (this) {
                if (r.created && instance(r) == null) Access.create(ActivityThread.currentActivityThread(), r.token, r.info);
            }
        });
        // requestServiceBindingsLocked: what clients bound before it existed.
        for (IntentBinding b : r.bindings.values()) {
            if (!b.requested && !b.connections.isEmpty()) requestBind(r, b, false);
        }
    }

    /** ActiveServices.requestServiceBindingLocked: onBind, or onRebind when `rebind`. */
    private void requestBind(ServiceRecord r, IntentBinding b, boolean rebind) {
        b.requested = true;
        b.hasBound = true;
        if (rebind) b.doRebind = false;
        try {
            thread().scheduleBindService(r.token, b.intent, rebind, PROCESS_STATE_SERVICE, 0);
        } catch (RemoteException e) {
            throw new IllegalStateException(e);
        }
    }

    /** bringDownServiceIfNeededLocked: not started, nothing bound with BIND_AUTO_CREATE, no job and
     *  no host holding it: onUnbind for each binding, then onDestroy. */
    private boolean maybeStop(ServiceRecord r) {
        if (r.startRequested || r.hasConnections() || r.hostBindings > 0 || r.jobBindings > 0) return false;
        if (r.created) {
            try {
                for (IntentBinding b : r.bindings.values()) {
                    if (b.hasBound) thread().scheduleUnbindService(r.token, b.intent);
                }
                thread().scheduleStopService(r.token);
            } catch (RemoteException e) {
                throw new IllegalStateException(e);
            }
            // bringDownServiceLocked: a connection still there (made without BIND_AUTO_CREATE) is
            // told the service is gone (onServiceDisconnected, onBindingDied).
            for (IntentBinding b : r.bindings.values()) {
                for (Connection c : b.connections) {
                    try {
                        c.conn.connected(r.name, null, true);
                    } catch (RemoteException | RuntimeException e) {
                        System.err.println("[aosp-app] the connection to " + r.name.getClassName() + " threw " + e);
                    }
                }
            }
        }
        // A connection made without BIND_AUTO_CREATE stays registered and waits for the next start.
        boolean keep = false;
        for (IntentBinding b : r.bindings.values()) if (!b.connections.isEmpty()) keep = true;
        r.created = false;
        r.lastStartId = 0;
        for (IntentBinding b : r.bindings.values()) {
            b.requested = false;
            b.received = false;
            b.hasBound = false;
            b.doRebind = false;
            b.binder = null;
        }
        if (!keep) {
            byName.remove(r.name);
            byToken.remove(r.token);
        }
        return true;
    }

    // ---- The host's binds "now" (the broker's into this app, a job's, an authenticator's) ----

    /** The service's binder for `intent`, created and bound if it was not, the binding held by the
     *  host for the process's life (as the old host's were); null when nothing of the app's answers
     *  or its onBind returned null. Blocks until the binder is published (60 s at most). */
    IBinder bindNow(Intent intent) {
        IntentBinding b;
        ServiceRecord r;
        CountDownLatch published = new CountDownLatch(1);
        synchronized (this) {
            r = retrieve(intent, true);
            if (r == null) return null;
            r.hostBindings++;
            b = r.bindings.computeIfAbsent(new Intent.FilterComparison(new Intent(intent)),
                    k -> new IntentBinding(k.getIntent()));
            if (b.received) return b.binder;
            b.waiting.add(published);
        }
        onMain(() -> {
            synchronized (this) {
                if (b.received) return;
                createInPlace(r);
                if (!b.requested) {
                    b.requested = true;
                    b.hasBound = true;
                    Access.bind(ActivityThread.currentActivityThread(), r.token, b.intent);
                }
            }
        });
        try {
            if (!published.await(60, TimeUnit.SECONDS)) {
                System.err.println("[aosp-app] " + r.name.getClassName() + " did not publish its binder in 60 s");
            }
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
        synchronized (this) {
            return b.binder;
        }
    }

    /** JobServiceContext's BIND_AUTO_CREATE bind for one execution: the service's instance, created
     *  in place if needed (the scheduler runs on the main thread and calls it directly). */
    Service jobBind(ComponentName name) {
        ServiceRecord[] out = new ServiceRecord[1];
        synchronized (this) {
            ServiceRecord r = retrieve(new Intent().setComponent(name), true);
            if (r == null) return null;
            r.jobBindings++;
            out[0] = r;
        }
        onMain(() -> {
            synchronized (this) {
                createInPlace(out[0]);
            }
        });
        return instance(out[0]);
    }

    /** The end of a job's execution: true when that destroyed the service. */
    synchronized boolean jobUnbind(ComponentName name) {
        ServiceRecord r = byName.get(name);
        if (r == null || r.jobBindings == 0) return false;
        r.jobBindings--;
        return maybeStop(r);
    }

    /** The live instance of the app's service named `className`, or null. */
    Service peek(String className) {
        ServiceRecord r;
        synchronized (this) {
            r = byName.get(new ComponentName(app.info.packageName, className));
            if (r == null || !r.created) return null;
        }
        return instance(r);
    }

    /** On the main thread: the service created now if the posted create has not run yet (which
     *  then finds it made). */
    private void createInPlace(ServiceRecord r) {
        r.created = true;
        if (instance(r) == null) Access.create(ActivityThread.currentActivityThread(), r.token, r.info);
    }

    private static Service instance(ServiceRecord r) {
        return Access.service(ActivityThread.currentActivityThread(), r.token);
    }

    private static void onMain(Runnable run) {
        if (Looper.myLooper() == Looper.getMainLooper()) {
            run.run();
            return;
        }
        CountDownLatch done = new CountDownLatch(1);
        new android.os.Handler(Looper.getMainLooper()).post(() -> {
            try { run.run(); } finally { done.countDown(); }
        });
        try {
            done.await(60, TimeUnit.SECONDS);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }

    /** ActivityThread's own service handlers, for the binds the host waits for (private in AOSP;
     *  reached by a full-privilege lookup, as the classes share the unnamed module). */
    private static final class Access {
        private static final MethodHandle CREATE, BIND, SERVICES, NEW_CREATE, NEW_BIND;
        private static final MethodHandle C_TOKEN, C_INFO, C_COMPAT, B_TOKEN, B_INTENT;

        static {
            try {
                MethodHandles.Lookup at = MethodHandles.privateLookupIn(ActivityThread.class, MethodHandles.lookup());
                Class<?> cd = Class.forName("android.app.ActivityThread$CreateServiceData");
                Class<?> bd = Class.forName("android.app.ActivityThread$BindServiceData");
                MethodHandles.Lookup lc = MethodHandles.privateLookupIn(cd, MethodHandles.lookup());
                MethodHandles.Lookup lb = MethodHandles.privateLookupIn(bd, MethodHandles.lookup());
                CREATE = at.findVirtual(ActivityThread.class, "handleCreateService", MethodType.methodType(void.class, cd));
                BIND = at.findVirtual(ActivityThread.class, "handleBindService", MethodType.methodType(void.class, bd));
                SERVICES = at.findGetter(ActivityThread.class, "mServices", android.util.ArrayMap.class);
                NEW_CREATE = lc.findConstructor(cd, MethodType.methodType(void.class));
                NEW_BIND = lb.findConstructor(bd, MethodType.methodType(void.class));
                C_TOKEN = lc.findSetter(cd, "token", IBinder.class);
                C_INFO = lc.findSetter(cd, "info", ServiceInfo.class);
                C_COMPAT = lc.findSetter(cd, "compatInfo", CompatibilityInfo.class);
                B_TOKEN = lb.findSetter(bd, "token", IBinder.class);
                B_INTENT = lb.findSetter(bd, "intent", Intent.class);
            } catch (ReflectiveOperationException e) {
                throw new ExceptionInInitializerError(e);
            }
        }

        static void create(ActivityThread t, IBinder token, ServiceInfo info) {
            try {
                Object d = NEW_CREATE.invoke();
                C_TOKEN.invoke(d, token);
                C_INFO.invoke(d, info);
                C_COMPAT.invoke(d, CompatibilityInfo.DEFAULT_COMPATIBILITY_INFO);
                CREATE.invoke(t, d);
            } catch (RuntimeException | Error e) {
                throw e;
            } catch (Throwable e) {
                throw new IllegalStateException(e);
            }
        }

        static void bind(ActivityThread t, IBinder token, Intent intent) {
            try {
                Object d = NEW_BIND.invoke();
                B_TOKEN.invoke(d, token);
                B_INTENT.invoke(d, intent);
                BIND.invoke(t, d);
            } catch (RuntimeException | Error e) {
                throw e;
            } catch (Throwable e) {
                throw new IllegalStateException(e);
            }
        }

        static Service service(ActivityThread t, IBinder token) {
            try {
                android.util.ArrayMap<?, ?> m = (android.util.ArrayMap<?, ?>) SERVICES.invoke(t);
                synchronized (m) {
                    return (Service) m.get(token);
                }
            } catch (RuntimeException | Error e) {
                throw e;
            } catch (Throwable e) {
                throw new IllegalStateException(e);
            }
        }
    }
}
