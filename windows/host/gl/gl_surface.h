// gl_surface.h -- the framework's own EGL context on ANGLE, for GLSurfaceView.
//
// Upstream (angle_glue.cpp) binds one EGL context to a CAMetalLayer and
// brackets every GL view's frame with AngleBeginFrame/AngleEndFrame. Here the
// context renders into an offscreen pbuffer per view, and the end of a frame
// reads the pixels back for the window to show (the Java UI arrives as the
// overlay layer, composited on top). Render thread only.
//
// The display is ANGLE on Direct3D 11; without a usable GPU (a server, CI)
// Direct3D's WARP rasterizer or ANGLE's software Vulkan (SwiftShader) stands
// in. WINAPK_ANGLE_BACKEND=d3d11|swiftshader picks one.

#pragma once

#include <cstdint>
#include <vector>

namespace winapk::host {

// Makes the context current on view `view`'s pbuffer, (re)created at w x h.
// False when ANGLE is missing or could not initialize; GL calls then do
// nothing.
bool gl_frame_begin(long long view, int w, int h);

// Reads the frame back as 0xAARRGGBB rows, top row first, w x h of the last
// begin. False if there is no frame.
bool gl_frame_read(std::vector<uint32_t>* argb, int* w, int* h);

// Drops a view's pbuffer.
void gl_view_release(long long view);

// The EGLDisplay the framework uses, initialized; null without ANGLE. An app's
// native code gets the same one from eglGetDisplay (guest_gl.cpp). Any thread.
void* gl_shared_display();

// Reads the current draw surface's w x h pixels as 0xAARRGGBB rows, top row
// first (gl_frame_read without the framework's context). Any thread with a
// current context. This is the synchronous read: glReadPixels stalls the GL
// thread until the GPU has copied the frame to the CPU.
void gl_read_current(std::vector<uint32_t>* argb, int w, int h);

// The asynchronous read for a presenting swap (guest_eglSwapBuffers). Issues
// this frame's readback into one of two pixel pack buffers (returns at once:
// the GPU DMAs into the buffer) and hands back the PREVIOUS frame, mapped from
// the other buffer, so the GL thread never blocks on the GPU->CPU copy. The
// cost is one frame of latency on that layer. Keyed by the current EGL context
// (each app GL thread gets its own pair); the buffers follow a lost or
// recreated context. Needs ES 3; on ES 2, a mapping failure, or any PBO error
// it falls back to gl_read_current for that context from then on.
//
// Fills *argb (0xAARRGGBB, top row first) and *pw/*ph with the presented
// frame's size and returns true when there is one. Returns false on the very
// first frame of a context (nothing buffered yet: skip the present). Any thread
// with a current context.
bool gl_read_current_async(std::vector<uint32_t>* argb, int w, int h, int* pw, int* ph);

// Whether the asynchronous readback is opted into (WINAPK_GL_ASYNC_READBACK=1).
// Off by default: a presenting swap reads the frame back synchronously, as it
// has since the window surface was added, so no app regresses. The async path
// trades one frame of latency for a non-stalling readback, which on-demand
// renderers (a Flutter UI that draws a burst then idles) cannot take -- their
// final frame is left in a buffer, never presented -- so it stays opt-in until
// it is correct for every app.
bool gl_async_readback_enabled();

}  // namespace winapk::host
