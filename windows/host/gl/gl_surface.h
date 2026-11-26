// gl_surface.h -- the app's ANGLE display, and the read-back of a frame.
//
// Every GL client of the app's -- native code's EGL (guest_gl.cpp), MacApkEGL10 and EGL14
// (gl_bridges.cpp) -- draws on one ANGLE display, the app's own, not the graphics DLL's. The host
// draws nothing on it: ANGLE has one Direct3D 11 immediate context per display and no lock between
// contexts that share nothing, so a context of the host's in use beside the app's GL threads
// corrupts ANGLE's state (gl_surface.cpp, native_cocoa.cpp's gl_tick). The app's frames leave the
// display as pixels, read back on the app's own thread (gl_read_current).
//
// The display is ANGLE on Direct3D 11; without a usable GPU (a server, CI)
// Direct3D's WARP rasterizer or ANGLE's software Vulkan (SwiftShader) stands
// in.

#pragma once

#include <cstdint>
#include <vector>

namespace winapk::host {

// The app's EGLDisplay, initialized; null without ANGLE. Native code's eglGetDisplay, MacApkEGL10
// and EGL14.eglGetDisplay(EGL_DEFAULT_DISPLAY) answer it. Any thread.
void* gl_shared_display();

// Reads the current draw surface's w x h pixels as 0xAARRGGBB rows, top row
// first. Any thread with a current context. This is the synchronous read: glReadPixels stalls the GL
// thread until the GPU has copied the frame to the CPU.
void gl_read_current(std::vector<uint32_t>* argb, int w, int h);

}  // namespace winapk::host
