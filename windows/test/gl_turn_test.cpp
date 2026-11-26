// gl_turn_test.cpp -- winapk-gl-turn-test: the app's GL threads taking turns on their display
// (host/gl/gl_turn.h), without ANGLE. A fake eglMakeCurrent stands in for ANGLE's Direct3D 11
// renderer: like StateManager11, it holds the state of the context made current last, on any thread,
// and a re-bind of what a thread already has current does nothing. The checks: a thread's GL call
// always runs with its own context's state in the renderer (Telegram's welcome screen drew with
// another thread's, issue #195), two threads' calls never overlap, and EGL calls that use no state
// re-bind nothing.
//
//   winapk-gl-turn-test

#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "gl/gl_loader.h"
#include "gl/gl_turn.h"

namespace {

int g_failures = 0;

void check(const char* what, bool ok) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

EGLDisplay const kDisplay = reinterpret_cast<EGLDisplay>(0x10);
EGLContext ctx(int n) { return reinterpret_cast<EGLContext>(static_cast<intptr_t>(0x100 + n)); }
EGLSurface surface(int n) { return reinterpret_cast<EGLSurface>(static_cast<intptr_t>(0x200 + n)); }

// The fake ANGLE: per thread what EGL has current, and the renderer's one state holder.
thread_local EGLContext t_bound = EGL_NO_CONTEXT;
thread_local EGLSurface t_bound_draw = EGL_NO_SURFACE;
std::atomic<EGLContext> g_renderer{EGL_NO_CONTEXT};
std::atomic<int> g_binds{0};
std::atomic<bool> g_refuse{false};
std::atomic<int> g_inside{0};

EGLBoolean EGLAPIENTRY fake_make_current(EGLDisplay, EGLSurface draw, EGLSurface, EGLContext c) {
    if (c != EGL_NO_CONTEXT && g_refuse.load()) return EGL_FALSE;
    // egl_stubs.cpp MakeCurrent: nothing changes when nothing does.
    if (c == t_bound && draw == t_bound_draw) return EGL_TRUE;
    t_bound = c;
    t_bound_draw = draw;
    if (c != EGL_NO_CONTEXT) {
        g_renderer.store(c);  // StateManager11::onMakeCurrent: the renderer now holds c
        g_binds.fetch_add(1);
    }
    return EGL_TRUE;
}

EGLBoolean EGLAPIENTRY fake_release_thread() {
    t_bound = EGL_NO_CONTEXT;
    t_bound_draw = EGL_NO_SURFACE;
    return EGL_TRUE;
}

// A GL call as a forwarder makes it (gl_stubs.inc): the turn, then the call, which must find its own
// context's state in the renderer.
bool draw_call() {
    winapk::host::GlTurn turn(true);
    bool own = g_renderer.load() == t_bound;
    if (g_inside.fetch_add(1) != 0) own = false;  // another call is running at the same time
    std::this_thread::yield();
    g_inside.fetch_sub(1);
    return own;
}

void make_current(int n) {
    winapk::host::gl_turn_make_current(fake_make_current, kDisplay, surface(n), surface(n), ctx(n));
}

// Runs `fn` on a thread of its own and waits for it.
template <typename F>
void on_thread(F fn) {
    std::thread t(fn);
    t.join();
}

}  // namespace

// The test's gl_loader: ANGLE's eglMakeCurrent is the fake.
namespace winapk::host {
void* gl_proc(const char* name, GlLib) {
    if (std::strcmp(name, "eglMakeCurrent") == 0) return reinterpret_cast<void*>(&fake_make_current);
    return nullptr;
}
bool gl_available() { return true; }
}  // namespace winapk::host

int main() {
    using winapk::host::gl_turn_rebinds;
    std::printf("winapk-gl-turn-test\n");

    draw_call();
    check("a thread with nothing current takes no turn and re-binds nothing", gl_turn_rebinds() == 0);

    // Two threads, each with its own context kept current, as Telegram's welcome screen and a second
    // GL view: A draws, B makes its context current, A draws again.
    std::atomic<int> step{0};
    bool a_first = false, a_after = false, b_draw = false;
    unsigned long rebinds_before = 0, rebinds_after = 0;
    std::thread a([&] {
        make_current(1);
        a_first = draw_call();
        rebinds_before = gl_turn_rebinds();
        step = 1;
        while (step != 2) std::this_thread::yield();
        a_after = draw_call();
        rebinds_after = gl_turn_rebinds();
        step = 3;
    });
    std::thread b([&] {
        while (step != 1) std::this_thread::yield();
        make_current(2);  // the renderer now holds B's context
        step = 2;
        while (step != 3) std::this_thread::yield();
        b_draw = draw_call();
    });
    a.join();
    b.join();
    check("A's first call finds its own context in the renderer", a_first);
    check("A's first call needed no re-bind", rebinds_before == 0);
    check("after B made its context current, A's next call runs with A's state", a_after);
    check("  ... A's context was made current again, once", rebinds_after == 1);
    check("then B's call runs with B's state again", b_draw);
    check("  ... B re-bound once in turn", gl_turn_rebinds() == 2);

    // An EGL call (no context state) re-binds nothing.
    on_thread([] {
        make_current(3);
        on_thread([] { make_current(4); });
        unsigned long before = gl_turn_rebinds();
        { winapk::host::GlTurn turn(false); }
        check("an EGL call in turn re-binds nothing", gl_turn_rebinds() == before);
        check("but the next GL call does", draw_call() && gl_turn_rebinds() == before + 1);
    });

    // A context or surface gone while released: the thread has nothing current afterwards.
    on_thread([] {
        make_current(5);
        on_thread([] { make_current(6); });
        g_refuse = true;
        draw_call();
        g_refuse = false;
        unsigned long before = gl_turn_rebinds();
        on_thread([] { make_current(7); });
        draw_call();
        check("a re-bind that fails leaves the thread with nothing to re-bind", gl_turn_rebinds() == before);
    });

    // eglMakeCurrent(EGL_NO_CONTEXT) and eglReleaseThread end a thread's turns.
    on_thread([] {
        make_current(8);
        winapk::host::gl_turn_make_current(fake_make_current, kDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE,
                                           EGL_NO_CONTEXT);
        on_thread([] { make_current(9); });
        unsigned long before = gl_turn_rebinds();
        draw_call();
        check("after a release no call re-binds", gl_turn_rebinds() == before);
        make_current(8);
        winapk::host::gl_turn_release_thread(fake_release_thread);
        on_thread([] { make_current(9); });
        draw_call();
        check("after eglReleaseThread no call re-binds", gl_turn_rebinds() == before);
    });

    // Many threads, many calls: every call sees its own context, and none overlaps another.
    std::atomic<int> wrong{0};
    std::vector<std::thread> pool;
    for (int t = 0; t < 4; ++t) {
        pool.emplace_back([t, &wrong] {
            make_current(20 + t);
            for (int i = 0; i < 20000; ++i) {
                if (!draw_call()) wrong.fetch_add(1);
            }
        });
    }
    for (auto& t : pool) t.join();
    check("4 threads x 20000 calls: each ran with its own context's state, one at a time", wrong == 0);

    if (g_failures) {
        std::printf("%d FAILED\n", g_failures);
        return 1;
    }
    std::printf("ALL PASSED\n");
    return 0;
}
