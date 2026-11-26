// gl_turn.cpp -- the app's GL threads take turns on their display (gl_turn.h).

#include "gl/gl_turn.h"

#include <atomic>
#include <mutex>
#include <utility>
#include <vector>

#include "gl/gl_loader.h"

namespace winapk::host {

namespace {

// What this thread made current through the forwarders.
struct Current {
    EGLDisplay dpy = EGL_NO_DISPLAY;
    EGLSurface draw = EGL_NO_SURFACE;
    EGLSurface read = EGL_NO_SURFACE;
    EGLContext ctx = EGL_NO_CONTEXT;
};
thread_local Current t_current;

// Recursive: a call in turn never comes back through a forwarder today, but a callback of ANGLE's
// that did must not deadlock its own thread.
std::recursive_mutex g_turn;
// Per display, the context whose state its renderer holds: the one made current last. Few displays
// (the app's, the framework's): a list. Under g_turn. It may name a context since destroyed: a
// thread only compares it with a context it made current itself, which set it then.
std::vector<std::pair<EGLDisplay, EGLContext>> g_holder;
std::atomic<unsigned long> g_rebinds{0};

EGLContext& holder(EGLDisplay dpy) {
    for (auto& d : g_holder) {
        if (d.first == dpy) return d.second;
    }
    g_holder.emplace_back(dpy, EGL_NO_CONTEXT);
    return g_holder.back().second;
}

GlMakeCurrentFn raw_make_current() {
    static const GlMakeCurrentFn fn = reinterpret_cast<GlMakeCurrentFn>(gl_proc("eglMakeCurrent", GlLib::Egl));
    return fn;
}

// This thread's context takes its display back: released and bound again, so that ANGLE runs its
// whole makeCurrent (a bind of what is already current returns at once) and syncs every state bit.
void rebind(Current& cur) {
    GlMakeCurrentFn fn = raw_make_current();
    if (!fn) return;
    fn(cur.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (fn(cur.dpy, cur.draw, cur.read, cur.ctx)) {
        holder(cur.dpy) = cur.ctx;
        g_rebinds.fetch_add(1, std::memory_order_relaxed);
    } else {
        // Gone while released (a context or surface the app destroyed while it was current):
        // nothing is current on this thread now, as ANGLE has it.
        holder(cur.dpy) = EGL_NO_CONTEXT;
        cur = Current{};
    }
}

}  // namespace

GlTurn::GlTurn(bool draws) {
    Current& cur = t_current;
    // A GL call with nothing current does nothing in ANGLE; an EGL call always takes the turn, from
    // any thread: creating or destroying a context or surface reaches the renderer too.
    if (draws && cur.ctx == EGL_NO_CONTEXT) return;
    g_turn.lock();
    held_ = true;
    if (draws && holder(cur.dpy) != cur.ctx) rebind(cur);
}

GlTurn::~GlTurn() {
    if (held_) g_turn.unlock();
}

EGLBoolean gl_turn_make_current(GlMakeCurrentFn fn, EGLDisplay dpy, EGLSurface draw, EGLSurface read,
                                EGLContext ctx) {
    std::lock_guard<std::recursive_mutex> hold(g_turn);
    EGLBoolean ok = fn(dpy, draw, read, ctx);
    if (!ok) return ok;  // EGL leaves the thread's binding as it was
    Current& cur = t_current;
    if (ctx == EGL_NO_CONTEXT) {
        cur = Current{};
    } else {
        cur.dpy = dpy;
        cur.draw = draw;
        cur.read = read;
        cur.ctx = ctx;
        holder(dpy) = ctx;
    }
    return ok;
}

EGLBoolean gl_turn_release_thread(EGLBoolean(EGLAPIENTRY* fn)()) {
    std::lock_guard<std::recursive_mutex> hold(g_turn);
    EGLBoolean ok = fn();
    if (ok) t_current = Current{};
    return ok;
}

unsigned long gl_turn_rebinds() { return g_rebinds.load(std::memory_order_relaxed); }

}  // namespace winapk::host
