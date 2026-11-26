// gl_turn.h -- the app's GL threads take turns on their display, as a phone's driver lets them share it.
//
// On a phone every thread keeps its own EGL context current and calls GL whenever it likes: the
// driver is safe across threads. ANGLE on Direct3D 11 is not, for contexts on one display:
//
//   * the display has one renderer, and its state manager (StateManager11) caches what the context
//     made current LAST, on any thread, has bound: the program, the vertex array, the framebuffer.
//     eglMakeCurrent on one thread resets that cache (StateManager11::onMakeCurrent), and a
//     context that was current all along on another thread then draws through it with nothing to
//     say its state is gone: a null program (updateState, StateManager11.cpp), the access violation
//     at libGLESv2.dll+0x31ff04 that stopped Telegram when its welcome screen's GL thread drew after
//     a second GL view made its context current on another thread (issue #195);
//   * contexts that share nothing take no common lock, so two threads' calls run at once on the
//     renderer's one Direct3D 11 immediate context, which is not safe across threads.
//
// So every GL and EGL call that comes through the runtime's forwarders (gl_stubs.inc: the GLES
// bindings, EGL10/EGL14, an app's native code) takes the display's turn first: one lock for the
// thread's call, and, when another context became current on the display since, the thread's own
// context made current again, released and re-bound, since ANGLE's eglMakeCurrent of what is
// already current does nothing (egl_stubs.cpp MakeCurrent) and a re-bind marks all its state dirty
// (gl::Context::makeCurrent). EGL calls take the turn from any thread (destroying a context reaches
// the renderer's state manager); a GL call from a thread whose context was made current outside the
// forwarders (the graphics DLL's render thread, on its own display), or with nothing current, takes
// none. The pure queries (eglGetError, eglGetCurrent*, eglGetProcAddress, eglQueryAPI, eglBindAPI)
// take none either.
//
// What it costs: an uncontended lock per call, and a full state sync each time the display passes
// from one context to another. What it cannot be: concurrent. Two GL threads of an app run their
// calls one at a time, where a phone's GPU driver would interleave them.

#pragma once

#include <EGL/egl.h>

namespace winapk::host {

// One GL or EGL call's turn on the calling thread's display: held from construction to
// destruction. `draws`: the call uses the context's state (every gl* call), so the thread's context
// is made current again first when another took the display.
class GlTurn {
public:
    explicit GlTurn(bool draws);
    ~GlTurn();
    GlTurn(const GlTurn&) = delete;
    GlTurn& operator=(const GlTurn&) = delete;

private:
    bool held_ = false;
};

using GlMakeCurrentFn = EGLBoolean(EGLAPIENTRY*)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);

// eglMakeCurrent through `fn`, in turn: remembers what this thread made current, and that the
// display's renderer now holds `ctx`'s state.
EGLBoolean gl_turn_make_current(GlMakeCurrentFn fn, EGLDisplay dpy, EGLSurface draw, EGLSurface read,
                                EGLContext ctx);

// eglReleaseThread through `fn`, in turn: this thread has nothing current afterwards.
EGLBoolean gl_turn_release_thread(EGLBoolean(EGLAPIENTRY* fn)());

// Times a thread's context was made current again because another had taken its display: the test's
// window on the turns (test/gl_turn_test.cpp, test/gl_test.cpp).
unsigned long gl_turn_rebinds();

}  // namespace winapk::host
