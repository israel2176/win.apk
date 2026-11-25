// The broadcasts of the app's process, as ActivityManagerService's BroadcastQueue delivers them: the
// receivers registered at run time (a ReceiverList of filters per IIntentReceiver, which is
// LoadedApk.ReceiverDispatcher's), the manifest's receivers (ActivityThread.handleReceiver through
// scheduleReceiver), the stickies, and ordered broadcasts one receiver at a time with their result
// carried on (finishReceiver) to the final result receiver. Another app's receivers hear a broadcast
// through the system broker (SystemClient.forwardBroadcast); one from another app arrives here with
// deliver(intent, true), heard only by exported receivers. The system's own stickies and ticks a
// device's system_server sends are here too: ACTION_BATTERY_CHANGED from the host's battery, and
// ACTION_TIME_TICK each minute to registered receivers only.
package com.winapk.aospapp;

import android.app.ActivityManager;
import android.app.ActivityThread;
import android.app.IApplicationThread;
import android.content.Context;
import android.content.IIntentReceiver;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.ActivityInfo;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.content.res.CompatibilityInfo;
import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.Process;
import android.os.RemoteException;

import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

final class ActiveBroadcasts {
    private static final int PROCESS_STATE_RECEIVER = 11;   // ActivityManager.PROCESS_STATE_RECEIVER

    /** BroadcastFilter: one registration of a receiver. */
    static final class Filter {
        final IIntentReceiver receiver;
        final IntentFilter filter;
        final int flags;

        Filter(IIntentReceiver receiver, IntentFilter filter, int flags) {
            this.receiver = receiver;
            this.filter = filter;
            this.flags = flags;
        }
    }

    /** BroadcastRecord of an ordered broadcast: its targets in order and the result so far. */
    static final class Ordered {
        final Intent intent;
        final List<Object> targets;   // Filter or ActivityInfo
        final IIntentReceiver resultTo;
        int next;
        int resultCode;
        String resultData;
        Bundle resultExtras;
        boolean aborted;
        /** The finishReceiver caller waited for: the process's ApplicationThread, for a manifest
         *  receiver and a registered one alike (AOSP 14's PendingResult token is the app thread's
         *  binder in both, LoadedApk.ReceiverDispatcher.Args). */
        IBinder waitingFor;
        /** The registered receiver it waits on, if it is one: its unregistering moves the broadcast on. */
        IBinder waitingReceiver;

        Ordered(Intent intent, List<Object> targets, IIntentReceiver resultTo, int code, String data,
                Bundle extras) {
            this.intent = intent;
            this.targets = targets;
            this.resultTo = resultTo;
            this.resultCode = code;
            this.resultData = data;
            this.resultExtras = extras;
        }
    }

    private final AospApp app;
    private final Map<IBinder, List<Filter>> registered = new LinkedHashMap<>();
    private final Map<String, Intent> stickies = new HashMap<>();
    private final ArrayDeque<Ordered> ordered = new ArrayDeque<>();
    private boolean ticking;

    ActiveBroadcasts(AospApp app) { this.app = app; }

    private IApplicationThread thread() { return app.appThread; }

    private static Context context() { return ActivityThread.currentApplication(); }

    // ---- Context.registerReceiver / unregisterReceiver ----

    synchronized Intent register(IIntentReceiver receiver, IntentFilter filter, int flags) {
        if (filter == null) return null;
        if (filter.matchAction(Intent.ACTION_BATTERY_CHANGED)) stickies.put(Intent.ACTION_BATTERY_CHANGED, battery());
        // Every sticky the filter matches goes to the receiver; the first is the call's answer
        // (ActivityManagerService.registerReceiverWithFeature).
        List<Intent> matches = new ArrayList<>();
        for (Intent s : stickies.values()) if (matches(filter, s)) matches.add(s);
        if (receiver != null) {
            registered.computeIfAbsent(receiver.asBinder(), k -> new ArrayList<>())
                    .add(new Filter(receiver, filter, flags));
            for (Intent s : matches) performReceive(receiver, s, 0, null, null, false, true);
            if (filter.matchAction(Intent.ACTION_TIME_TICK)) startTicking();
        }
        return matches.isEmpty() ? null : matches.get(0);
    }

    synchronized void unregister(IIntentReceiver receiver) {
        registered.remove(receiver.asBinder());
        // An ordered broadcast waiting on it goes on (BroadcastQueue skips a receiver that left).
        Ordered o = ordered.peek();
        if (o != null && o.waitingFor != null && o.waitingReceiver == receiver.asBinder()) {
            o.waitingFor = null;
            o.waitingReceiver = null;
            deliverNext();
        }
    }

    // ---- Context.sendBroadcast and its kin (broadcastIntentWithFeature) ----

    synchronized int broadcast(Intent intent, IIntentReceiver resultTo, int resultCode, String resultData,
            Bundle resultExtras, boolean serialized, boolean sticky) {
        intent = new Intent(intent);
        if (sticky && intent.getAction() != null) stickies.put(intent.getAction(), new Intent(intent));
        try {
            com.macapk.loader.SystemClient.forwardBroadcast(intent);
        } catch (LinkageError e) {
            // a JVM without the loader (the plain-JVM test)
        }
        deliver(intent, false, resultTo, resultCode, resultData, resultExtras, serialized);
        return ActivityManager.BROADCAST_SUCCESS;
    }

    /** A broadcast another app (or the system broker) sent, to this process's receivers only. */
    synchronized void deliverFromOutside(Intent intent, boolean fromOtherApp) {
        deliver(new Intent(intent), fromOtherApp, null, 0, null, null, false);
    }

    synchronized Intent removeSticky(Intent intent) {
        return intent.getAction() != null ? stickies.remove(intent.getAction()) : null;
    }

    /** BroadcastRecord's delivery. A sticky broadcast sent now is not an initial sticky one: only
     *  the stickies a registration replays are (BroadcastRecord.initialSticky, which
     *  BroadcastReceiver.isInitialStickyBroadcast reports), so every delivery here says false. */
    private void deliver(Intent intent, boolean fromOtherApp, IIntentReceiver resultTo, int code, String data,
            Bundle extras, boolean serialized) {
        String target = intent.getComponent() != null ? intent.getComponent().getPackageName() : intent.getPackage();
        if (target != null && !target.equals(app.info.packageName)) {
            // Aimed at another app: the broker's alone; a result receiver still hears the result.
            if (resultTo != null) performReceive(resultTo, intent, code, data, extras, false, false);
            return;
        }
        // The registered receivers (a component broadcast goes to the manifest only), in the
        // receiver resolver's order: by the filter's priority, highest first.
        List<Filter> dynamic = new ArrayList<>();
        if (intent.getComponent() == null) {
            for (List<Filter> list : registered.values()) {
                for (Filter f : list) {
                    if (fromOtherApp && (f.flags & Context.RECEIVER_NOT_EXPORTED) != 0) continue;
                    if (matches(f.filter, intent)) {
                        dynamic.add(f);
                        break;   // one delivery per receiver, as its ReceiverList has one
                    }
                }
            }
            dynamic.sort((a, b) -> Integer.compare(b.filter.getPriority(), a.filter.getPriority()));
        }
        List<ResolveInfo> manifest = new ArrayList<>();
        if ((intent.getFlags() & Intent.FLAG_RECEIVER_REGISTERED_ONLY) == 0) {
            for (ResolveInfo ri : manifestReceivers(intent)) {
                if (fromOtherApp && !ri.activityInfo.exported) continue;
                manifest.add(ri);
            }
        }
        List<Object> targets = merge(dynamic, manifest);
        if (!serialized) {
            for (Object t : targets) {
                if (t instanceof Filter) {
                    performReceive(((Filter) t).receiver, intent, code, data, extras, false, false);
                } else {
                    scheduleReceiver((ActivityInfo) t, intent, code, data, extras, false);
                }
            }
            if (resultTo != null) performReceive(resultTo, intent, code, data, extras, false, false);
            return;
        }
        ordered.add(new Ordered(intent, targets, resultTo, code, data, extras));
        if (ordered.size() == 1) deliverNext();
    }

    /** ActivityManagerService.broadcastIntentLocked's merge of the two lists, each already in priority
     *  order: a registered receiver goes before the manifest receivers whose priority is not above
     *  its own, so at an equal priority the registered one is first. */
    static List<Object> merge(List<Filter> dynamic, List<ResolveInfo> manifest) {
        List<Object> out = new ArrayList<>(dynamic.size() + manifest.size());
        int d = 0;
        for (ResolveInfo ri : manifest) {
            while (d < dynamic.size() && dynamic.get(d).filter.getPriority() >= ri.priority) out.add(dynamic.get(d++));
            out.add(ri.activityInfo);
        }
        while (d < dynamic.size()) out.add(dynamic.get(d++));
        return out;
    }

    /** BroadcastQueue.processNextBroadcast for the ordered one at the head: its next receiver, or,
     *  when none is left (or one aborted it), the result receiver, then the next broadcast. */
    private void deliverNext() {
        while (!ordered.isEmpty()) {
            Ordered o = ordered.peek();
            if (o.waitingFor != null) return;
            if (!o.aborted && o.next < o.targets.size()) {
                Object t = o.targets.get(o.next++);
                if (t instanceof Filter) {
                    Filter f = (Filter) t;
                    if (!registered.containsKey(f.receiver.asBinder())) continue;   // unregistered since
                    o.waitingFor = thread().asBinder();
                    o.waitingReceiver = f.receiver.asBinder();
                    performReceive(f.receiver, o.intent, o.resultCode, o.resultData, o.resultExtras, true, false);
                } else {
                    o.waitingFor = thread().asBinder();
                    scheduleReceiver((ActivityInfo) t, o.intent, o.resultCode, o.resultData, o.resultExtras, true);
                }
                return;
            }
            ordered.poll();
            if (o.resultTo != null) {
                performReceive(o.resultTo, o.intent, o.resultCode, o.resultData, o.resultExtras, false, false);
            }
        }
    }

    /** A receiver of an ordered broadcast finished (BroadcastReceiver.PendingResult.finish): its
     *  result is the broadcast's, and the next receiver gets it. A finish of an unordered delivery
     *  (a manifest receiver's always comes) has nothing to carry on. */
    synchronized void finishReceiver(IBinder who, int resultCode, String resultData, Bundle resultExtras,
            boolean abort) {
        Ordered o = ordered.peek();
        if (o == null || o.waitingFor == null || o.waitingFor != who) return;
        o.waitingFor = null;
        o.waitingReceiver = null;
        o.resultCode = resultCode;
        o.resultData = resultData;
        o.resultExtras = resultExtras;
        if (abort && (o.intent.getFlags() & Intent.FLAG_RECEIVER_NO_ABORT) == 0) o.aborted = true;
        deliverNext();
    }

    private void performReceive(IIntentReceiver receiver, Intent intent, int code, String data, Bundle extras,
            boolean orderedDelivery, boolean sticky) {
        try {
            // ActivityThread.ApplicationThread.scheduleRegisteredReceiver: the receiver's dispatcher
            // posts onReceive to the handler it was registered with.
            thread().scheduleRegisteredReceiver(receiver, intent, code, data, extras, orderedDelivery, sticky,
                    !orderedDelivery /* assumeDelivered: no finishReceiver for an unordered one */, 0, PROCESS_STATE_RECEIVER, Process.myUid(), app.info.packageName);
        } catch (RemoteException | RuntimeException e) {
            System.err.println("[aosp-app] a receiver of " + intent.getAction() + " threw " + e);
        }
    }

    private void scheduleReceiver(ActivityInfo ai, Intent intent, int code, String data, Bundle extras,
            boolean orderedDelivery) {
        Intent i = new Intent(intent);
        i.setComponent(new android.content.ComponentName(ai.packageName, ai.name));
        if (ai.applicationInfo == null) ai.applicationInfo = app.info;
        try {
            thread().scheduleReceiver(i, ai, CompatibilityInfo.DEFAULT_COMPATIBILITY_INFO, code, data, extras,
                    orderedDelivery, !orderedDelivery /* assumeDelivered */, 0, PROCESS_STATE_RECEIVER, Process.myUid(), app.info.packageName);
        } catch (RemoteException e) {
            throw new IllegalStateException(e);
        }
    }

    /** The app's manifest receivers for the intent, PackageManager's resolution (in its priority
     *  order, each with its filter's priority). ACTION_PACKAGE_ADDED and its kin are not delivered to
     *  the package they name (the broker's rule for its events). */
    private List<ResolveInfo> manifestReceivers(Intent intent) {
        List<ResolveInfo> out = new ArrayList<>();
        PackageManager pm = app.packageManager;
        try {
            if (intent.getComponent() != null) {
                if (!app.info.packageName.equals(intent.getComponent().getPackageName())) return out;
                ResolveInfo ri = new ResolveInfo();
                ri.activityInfo = pm.getReceiverInfo(intent.getComponent(), PackageManager.GET_META_DATA);
                out.add(ri);
                return out;
            }
            List<ResolveInfo> hits = pm.queryBroadcastReceivers(intent, 0);
            if (hits == null) return out;
            String skip = null;
            String action = intent.getAction();
            if (Intent.ACTION_PACKAGE_ADDED.equals(action) || Intent.ACTION_PACKAGE_RESTARTED.equals(action)
                    || Intent.ACTION_PACKAGE_DATA_CLEARED.equals(action)) {
                skip = intent.getData() != null ? intent.getData().getSchemeSpecificPart() : null;
            }
            for (ResolveInfo ri : hits) {
                if (ri == null || ri.activityInfo == null) continue;
                if (skip != null && skip.equals(ri.activityInfo.packageName)) continue;
                if (!app.info.packageName.equals(ri.activityInfo.packageName)) continue;
                out.add(ri);
            }
        } catch (PackageManager.NameNotFoundException | RuntimeException e) {
            // nothing declared: as a device with no receiver
        }
        return out;
    }

    private static boolean matches(IntentFilter f, Intent intent) {
        Context ctx = context();
        try {
            return f.match(ctx != null ? ctx.getContentResolver() : null, intent, true, "ActiveBroadcasts") >= 0;
        } catch (RuntimeException e) {
            String action = intent.getAction();
            return action == null ? f.countActions() == 0 : f.matchAction(action);
        }
    }

    // ---- What system_server sends on its own ----

    /** BatteryService's sticky ACTION_BATTERY_CHANGED, from the host's battery. */
    private static Intent battery() {
        Intent i = new Intent(Intent.ACTION_BATTERY_CHANGED);
        i.addFlags(Intent.FLAG_RECEIVER_REGISTERED_ONLY | Intent.FLAG_RECEIVER_REPLACE_PENDING);
        try {
            android.os.BatteryManager bm = (android.os.BatteryManager) context().getSystemService(Context.BATTERY_SERVICE);
            int level = bm.getIntProperty(android.os.BatteryManager.BATTERY_PROPERTY_CAPACITY);
            int status = bm.getIntProperty(android.os.BatteryManager.BATTERY_PROPERTY_STATUS);
            boolean charging = bm.isCharging();
            if (level < 0) level = 100;
            i.putExtra(android.os.BatteryManager.EXTRA_LEVEL, level);
            i.putExtra(android.os.BatteryManager.EXTRA_SCALE, 100);
            i.putExtra(android.os.BatteryManager.EXTRA_STATUS, status);
            i.putExtra(android.os.BatteryManager.EXTRA_PRESENT, true);
            i.putExtra(android.os.BatteryManager.EXTRA_HEALTH, 2 /* BATTERY_HEALTH_GOOD */);
            i.putExtra(android.os.BatteryManager.EXTRA_PLUGGED, charging ? 1 /* BATTERY_PLUGGED_AC */ : 0);
            i.putExtra(android.os.BatteryManager.EXTRA_BATTERY_LOW, level <= 15);
            i.putExtra(android.os.BatteryManager.EXTRA_TECHNOLOGY, "Li-ion");
        } catch (RuntimeException e) {
            // no battery service: the intent with no extras, as an unknown battery
        }
        return i;
    }

    /** AlarmManagerService's ACTION_TIME_TICK at each minute, to registered receivers only. */
    private void startTicking() {
        if (ticking) return;
        ticking = true;
        Handler h = new Handler(Looper.getMainLooper());
        Runnable[] tick = new Runnable[1];
        tick[0] = () -> {
            Intent t = new Intent(Intent.ACTION_TIME_TICK).addFlags(Intent.FLAG_RECEIVER_REGISTERED_ONLY);
            synchronized (this) {
                deliver(t, false, null, 0, null, null, false);
            }
            h.postDelayed(tick[0], toNextMinute());
        };
        h.postDelayed(tick[0], toNextMinute());
    }

    private static long toNextMinute() {
        long ms = 60_000L - System.currentTimeMillis() % 60_000L;
        return ms < 1_000L ? ms + 60_000L : ms;
    }
}
