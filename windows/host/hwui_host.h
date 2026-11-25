// hwui_host.h -- the host's side of the graphics DLL's compositor (windows/hwui_build/glue/
// hwui_host_api.h): the app's windows are composed on the GPU by HWUI's render thread into the
// host window, as SurfaceFlinger composes layers. The host tells it where the display goes in the
// window and is told of every frame shown; what the host still has as pixels (an app's native GL
// or video surface, read back; a SurfaceView's software canvas) goes to the DLL as the layer under
// the windows.
#pragma once

#include <cstdint>

namespace winapk::host {

// Binds the DLL's compositor to the host window (aosp_gfx_load's, once the DLL is loaded). False
// when the DLL has none (a test's other runtime): the window then shows the frames the host has.
bool hwui_attach();
// Whether the DLL composes the window.
bool hwui_active();
// Compose again (the window was resized, exposed or restored).
void hwui_recompose();
// The host's own frame, under the app's windows (0xAARRGGBB, top row first); null drops it.
void hwui_host_layer(const uint32_t* argb, int w, int h);
// The same over the app's windows (a setZOrderOnTop surface); alpha 0 where nothing is.
void hwui_host_layer_above(const uint32_t* argb, int w, int h);
// The app's display in px (the window manager's); false before the first windows.
bool hwui_display_size(int* w, int* h);

}  // namespace winapk::host
