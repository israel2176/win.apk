// The in-process window manager's compositor (docs/research/aosp-views.md, "V3"): what
// SurfaceFlinger does with the windows' layers, on the GPU. Each window's SurfaceControl has a
// layer in the graphics DLL (HwuiLayers), which the window's ThreadedRenderer draws into through
// HWUI (ViewRootImpl's Surface, copied from the SurfaceControl, carries the layer post the DLL finds
// the layer by); a window drawn in software (hardwareAccelerated="false") posts its bitmap to the
// layer instead. This class says which layers are shown, bottom to top, where in the display, and
// the dim layer under a FLAG_DIM_BEHIND window (black at the window's dimAmount over the whole
// display), as WindowManagerService's surface placement does; the DLL's compositor draws them into
// the host window on HWUI's render thread whenever a window posts a frame or the scene changes.
// Main thread only, as every ViewRootImpl lays out there.
//
// Order: the activity's main window (TYPE_BASE_APPLICATION, which its WindowToken keeps at the
// bottom of its windows), then the other application windows and sub-windows (dialogs, panels,
// popups) in the order they were added, then the system types (toasts, overlays) in theirs.
// WindowManagerService orders by DisplayPolicy's layer of the type and then by the tokens'
// z-order; with one task and no system windows of its own, that is this order.
//
// A layer an app's process builds as a child of a window's SurfaceControl (SurfaceControl.Builder
// .setParent: the text Magnifier's popup, which ThreadedRenderer.SimpleRenderer draws into through
// a BLASTBufferQueue's Surface; a drag shadow) is SurfaceFlinger's child layer: Win.apk's
// SurfaceControl keeps the tree (parent, position, z, shown, which Transactions set) and tells this
// class (SurfaceControl.MacapkLayerTree); each buffer layer of it gets a layer in the DLL when built,
// and is composed with its window, at the window's place plus its own and its ancestors' positions,
// among the window's children by z (below the window for a negative z, as SurfaceFlinger draws
// them), while it and its ancestors are shown. No crop, alpha, scale or color layer is composed.
package com.winapk.aospviews;

import android.view.WindowManager;

import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;

final class Compositor {
    private Compositor() {}

    // Win.apk's SurfaceControl's own members, which this jar is compiled without (AOSP's class is
    // first on its compile class path), so through reflection.
    private static java.lang.reflect.Method sSetLayerPost, sGetLayerPost, sParent, sPlacement;
    private static Class<?> sLayerPost;

    private static synchronized void reflect() throws ReflectiveOperationException {
        if (sSetLayerPost != null) return;
        Class<android.view.SurfaceControl> c = android.view.SurfaceControl.class;
        sLayerPost = Class.forName("android.view.Surface$MacapkLayerPost");
        sGetLayerPost = c.getMethod("macapkLayerPost");
        sParent = c.getMethod("macapkParent");
        sPlacement = c.getMethod("macapkPlacement");
        // The layer tree's changes come here (a child layer built, moved, shown, removed).
        Class<?> tree = Class.forName("android.view.SurfaceControl$MacapkLayerTree");
        Object hook = java.lang.reflect.Proxy.newProxyInstance(tree.getClassLoader(), new Class<?>[] {tree},
                (proxy, m, args) -> {
                    switch (m.getName()) {
                        case "layerBuilt" -> childBuilt((android.view.SurfaceControl) args[0]);
                        case "treeChanged" -> childrenChanged();
                        case "layerReleased" -> childReleased((android.view.SurfaceControl) args[0]);
                        case "equals" -> { return proxy == args[0]; }
                        case "hashCode" -> { return System.identityHashCode(proxy); }
                        case "toString" -> { return "the window manager's compositor"; }
                        default -> { }
                    }
                    return null;
                });
        c.getMethod("macapkSetLayerTree", tree).invoke(null, hook);
        sSetLayerPost = c.getMethod("macapkSetLayerPost", sLayerPost);
    }

    /** A layer post (Surface$MacapkLayerPost) whose software frames go to `frames`. */
    private static Object layerPost(String name, java.util.function.Consumer<android.graphics.Bitmap> frames) {
        return java.lang.reflect.Proxy.newProxyInstance(sLayerPost.getClassLoader(),
                new Class<?>[] {sLayerPost}, (proxy, m, args) -> {
                    if (m.getDeclaringClass() == Object.class) {
                        return switch (m.getName()) {
                            case "equals" -> proxy == args[0];
                            case "hashCode" -> System.identityHashCode(proxy);
                            default -> name;
                        };
                    }
                    frames.accept((android.graphics.Bitmap) args[1]);
                    return null;
                });
    }

    /** The window's SurfaceControl gets a layer: the DLL's, of the control's size, and the layer post
     *  its Surface takes (Win.apk's SurfaceControl.macapkSetLayerPost) for a software frame. */
    static void attach(android.view.SurfaceControl control, AospWindows.Win w) {
        try {
            reflect();
            Object post = layerPost("layer of " + w.attrs.getTitle(), frame -> post(w, frame));
            w.post = post;
            w.layer = HwuiLayers.create(post, control.getWidth(), control.getHeight());
            w.drew = false;
            sSetLayerPost.invoke(control, post);
        } catch (ReflectiveOperationException e) {
            throw new IllegalStateException("Win.apk's SurfaceControl.macapkSetLayerPost", e);
        }
    }

    // ---- child layers (SurfaceFlinger's layer tree under a window) ---------------------------

    /** A buffer layer built with a parent: its control (the handle it was built as), its layer in
     *  the DLL and the layer post its Surface carries there, the layer's size. */
    static final class Child {
        final android.view.SurfaceControl control;
        final Object post;
        final long layer;
        int width, height;

        Child(android.view.SurfaceControl control, Object post, long layer, int width, int height) {
            this.control = control;
            this.post = post;
            this.layer = layer;
            this.width = width;
            this.height = height;
        }
    }

    static final List<Child> sChildren = new java.util.concurrent.CopyOnWriteArrayList<>();

    /** SurfaceControl.Builder built a buffer layer with a parent (any thread). */
    static void childBuilt(android.view.SurfaceControl control) {
        try {
            reflect();
            int w = Math.max(1, control.getWidth()), h = Math.max(1, control.getHeight());
            Child[] self = new Child[1];
            Object post = layerPost("layer " + control, frame -> {
                Child c = self[0];
                if (c != null) HwuiLayers.postBitmap(c.layer, frame);
            });
            Child c = new Child(control, post, HwuiLayers.create(post, w, h), w, h);
            self[0] = c;
            sSetLayerPost.invoke(control, post);
            sChildren.add(c);
            if (TRACE) System.err.println(AospWindows.TAG + "child layer " + control + " " + w + "x" + h);
        } catch (ReflectiveOperationException e) {
            throw new IllegalStateException("Win.apk's SurfaceControl.macapkSetLayerPost", e);
        }
    }

    /** A transaction changed the tree (any thread): a layer's new buffer size is its layer's now,
     *  before HWUI draws its next frame; the scene follows on the main thread. */
    static void childrenChanged() {
        for (Child c : sChildren) {
            int w = Math.max(1, c.control.getWidth()), h = Math.max(1, c.control.getHeight());
            if (w != c.width || h != c.height) {
                c.width = w;
                c.height = h;
                HwuiLayers.resize(c.layer, w, h);
            }
        }
        scheduleCompose();
    }

    /** The last handle to a child buffer layer was released (any thread): its layer goes. */
    static void childReleased(android.view.SurfaceControl control) {
        Object post = invoke(sGetLayerPost, control);
        for (Child c : sChildren) {
            if (c.post == post) {
                sChildren.remove(c);
                HwuiLayers.release(c.post);
            }
        }
        scheduleCompose();
    }

    private static final java.util.concurrent.atomic.AtomicBoolean sComposePending =
            new java.util.concurrent.atomic.AtomicBoolean();

    private static void scheduleCompose() {
        if (sComposePending.compareAndSet(false, true)) {
            AospWindows.main().post(() -> {
                sComposePending.set(false);
                composeScene();
            });
        }
    }

    private static Object invoke(java.lang.reflect.Method m, Object target) {
        try {
            return m.invoke(target);
        } catch (ReflectiveOperationException e) {
            throw new IllegalStateException("Win.apk's SurfaceControl." + m.getName(), e);
        }
    }

    /** One layer of the scene: where, the dim before it, and its path from the window down (each
     *  ancestor's placement, {x, y, z, shown, id}: the window's own is empty). */
    private record Placed(long layer, int x, int y, int dim, List<int[]> path) {}

    /** The child `c` placed in the scene of the window whose layer post is `post`, at (wx, wy);
     *  null when it is not under that window, or it or an ancestor is hidden. */
    private static Placed place(Child c, Object post, int wx, int wy) {
        android.view.SurfaceControl s = c.control;
        List<int[]> path = new ArrayList<>();
        int dx = 0, dy = 0;
        for (int depth = 0; depth < 64 && s != null; depth++) {
            android.view.SurfaceControl parent = (android.view.SurfaceControl) invoke(sParent, s);
            if (parent == null) {
                if (post == null || invoke(sGetLayerPost, s) != post) return null;
                java.util.Collections.reverse(path);
                return new Placed(c.layer, wx + dx, wy + dy, 0, path);
            }
            int[] p = (int[]) invoke(sPlacement, s);
            if (p[3] == 0 || !s.isValid()) return null;
            dx += p[0];
            dy += p[1];
            path.add(p);
            s = parent;
        }
        return null;
    }

    /** SurfaceFlinger's order among a window's layers: siblings by z, then by when they were built;
     *  a layer's children over it, but those of a negative z under it. */
    private static int byPath(Placed a, Placed b) {
        List<int[]> p = a.path, q = b.path;
        int n = Math.min(p.size(), q.size());
        for (int i = 0; i < n; i++) {
            int[] x = p.get(i), y = q.get(i);
            if (x[4] != y[4]) return x[2] != y[2] ? Integer.compare(x[2], y[2]) : Integer.compare(x[4], y[4]);
        }
        if (p.size() == q.size()) return 0;
        return p.size() > q.size() ? (p.get(n)[2] < 0 ? -1 : 1) : (q.get(n)[2] < 0 ? 1 : -1);
    }

    /** The window's SurfaceControl goes: its layer goes with the last reference to it (HWUI may still
     *  hold the window's ANativeWindow until its next frame). */
    static void detach(AospWindows.Win w) {
        if (w.post != null) HwuiLayers.release(w.post);
        w.post = null;
        w.layer = 0;
        w.drew = false;
    }

    /** WINAPK_AOSP_VIEWS_TRACE=1: each relayout, each window's first frame and each new window order
     *  in the log. */
    static final boolean TRACE = "1".equals(System.getenv("WINAPK_AOSP_VIEWS_TRACE"));
    private static String sLastOrder;

    /** A window drawn in software posted a frame (its Surface's bitmap), on the drawing thread. */
    static void post(AospWindows.Win w, android.graphics.Bitmap frame) {
        if (TRACE && !w.drew) {
            System.err.println(AospWindows.TAG + "first software frame of " + w.attrs.getTitle() + " type="
                    + w.attrs.type + " frame=" + w.frame + " surface=" + frame.getWidth() + "x" + frame.getHeight());
        }
        long layer = w.layer;
        if (layer == 0) return;
        HwuiLayers.postBitmap(layer, frame);
        if (!w.drew) {
            w.drew = true;
            compose();
        }
    }

    static int band(int type) {
        if (type >= WindowManager.LayoutParams.FIRST_SYSTEM_WINDOW) return 2;
        // An activity token's main window is the bottom of its windows (WindowToken keeps the
        // TYPE_BASE_APPLICATION window first), whenever its dialogs were added.
        return type == WindowManager.LayoutParams.TYPE_BASE_APPLICATION ? 0 : 1;
    }

    /** The scene changed (a window shown, hidden, moved, added or removed, its dim): the DLL composes
     *  with it at once, and at every frame a window posts. */
    static void compose() {
        InputRouter.windowsChanged();  // the focus follows the windows shown (step V2)
        composeScene();
    }

    /** The scene from the windows shown and their child layers, to the DLL. Main thread. */
    static void composeScene() {
        List<AospWindows.Win> order = new ArrayList<>();
        for (AospWindows.Win w : AospWindows.sWindows.values()) {
            if (w.visible && w.layer != 0 && w.attrs != null) order.add(w);
        }
        order.sort(Comparator.comparingInt((AospWindows.Win w) -> band(w.attrs.type))
                .thenComparingLong(w -> w.added));
        if (TRACE) {
            StringBuilder sb = new StringBuilder();
            for (AospWindows.Win w : order) sb.append(w.attrs.type).append('@').append(w.frame).append(' ');
            String now = sb.toString();
            if (!now.equals(sLastOrder)) {
                sLastOrder = now;
                System.err.println(AospWindows.TAG + "compose " + now);
            }
        }
        List<Placed> scene = new ArrayList<>(order.size());
        for (AospWindows.Win w : order) {
            WindowManager.LayoutParams a = w.attrs;
            // Each at its frame less its surface insets (a floating window's room for its shadow).
            int x = w.frame.left - a.surfaceInsets.left, y = w.frame.top - a.surfaceInsets.top;
            int dim = 0;
            if ((a.flags & WindowManager.LayoutParams.FLAG_DIM_BEHIND) != 0 && a.dimAmount > 0f) {
                dim = Math.round(Math.min(1f, a.dimAmount) * 255f);
            }
            List<Placed> mine = new ArrayList<>(1);
            mine.add(new Placed(w.layer, x, y, dim, List.of()));
            for (Child c : sChildren) {
                Placed p = place(c, w.post, x, y);
                if (p != null) mine.add(p);
            }
            if (mine.size() > 1) {
                mine.sort(Compositor::byPath);
                // The dim goes under the window and its children, as SurfaceFlinger's dim layer.
                int at = 0;
                while (mine.get(at).layer != w.layer) at++;
                if (at > 0 && dim > 0) {
                    mine.set(at, new Placed(w.layer, x, y, 0, List.of()));
                    Placed first = mine.get(0);
                    mine.set(0, new Placed(first.layer, first.x, first.y, dim, first.path));
                }
            }
            scene.addAll(mine);
        }
        long[] layers = new long[scene.size()];
        int[] xy = new int[2 * scene.size()];
        int[] dim = new int[scene.size()];
        for (int i = 0; i < layers.length; i++) {
            Placed p = scene.get(i);
            layers[i] = p.layer;
            xy[2 * i] = p.x;
            xy[2 * i + 1] = p.y;
            dim[i] = p.dim;
        }
        // Nothing under the windows is black, as SurfaceFlinger's empty display; but while a
        // SurfaceView's GL view is under them (AospWindows.showSurfaceBelow) the display starts
        // clear, so the host's layer shows through the SurfaceView's hole.
        HwuiLayers.setScene(layers, xy, dim, AospWindows.widthPx(), AospWindows.heightPx(),
                AospWindows.surfaceBelow());
    }
}
