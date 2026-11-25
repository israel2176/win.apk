// window_surface.h -- where HWUI's EGL code meets Win.apk's window layers (patch_src.py puts the
// calls in EglManager and SkiaOpenGLPipeline).
#pragma once

#include <EGL/egl.h>

struct ANativeWindow;

namespace winapk::hwui {

// The EGL display HWUI renders on: ANGLE on Direct3D 11 (WARP when there is no GPU).
EGLDisplay eglDisplay();
// A window's surface: a pbuffer of the window's size that the compositor can bind as a texture.
EGLSurface createWindowSurface(EGLDisplay display, EGLConfig config, ANativeWindow* window, const EGLint* attribs);
// True when the window's size is no longer its surface's (the pipeline then makes a new one).
bool windowResized(ANativeWindow* window, EGLSurface surface);
// The window's surface was swapped: a new frame of it is there to compose. vsyncNs: the frame's
// intended vsync (FrameInfo, systemTime's clock).
void windowPosted(ANativeWindow* window, EGLSurface surface, long long vsyncNs);
// The surface is about to be destroyed.
void windowSurfaceDestroyed(EGLSurface surface);

}  // namespace winapk::hwui
