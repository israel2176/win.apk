// stubs.cpp -- what HWUI links against from the platform that Win.apk does not have, answering as
// a device without it does (HWUI's own such parts: hwui_stubs.cpp): no AHardwareBuffer
// (hardware bitmaps, TextureView's SurfaceTexture, ImageReader windows, HardwareBufferRenderer and
// PixelCopy of a hardware buffer fail as an allocation does), no Ultra HDR JPEG encoder. And the
// two functions of libandroidfw HWUI's nine-patch code uses (ResourceTypes.cpp's, copied).
#include <GrBackendSurface.h>
#include <jni.h>
#include <GrDirectContext.h>
#include <android/hardware_buffer.h>
#include <android/surface_texture.h>
#include <androidfw/ResourceTypes.h>
#include <log/log.h>
#include <media/NdkImageReader.h>
#include <ui/ANativeObjectBase.h>
#include <ultrahdr/jpegr.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>

#include "GrAHardwareBufferUtils.h"

// --- AHardwareBuffer: none can be allocated ---

extern "C" {

int AHardwareBuffer_allocate(const AHardwareBuffer_Desc*, AHardwareBuffer** out) {
    if (out) *out = nullptr;
    return -ENOMEM;
}
void AHardwareBuffer_acquire(AHardwareBuffer*) {}
void AHardwareBuffer_release(AHardwareBuffer*) {}
void AHardwareBuffer_describe(const AHardwareBuffer*, AHardwareBuffer_Desc* d) {
    if (d) std::memset(d, 0, sizeof *d);
}
int AHardwareBuffer_lock(AHardwareBuffer*, uint64_t, int32_t, const ARect*, void** addr) {
    if (addr) *addr = nullptr;
    return -EINVAL;
}
int AHardwareBuffer_unlock(AHardwareBuffer*, int32_t* fence) {
    if (fence) *fence = -1;
    return -EINVAL;
}

// libcutils' ashmem: no ashmem bitmaps are made here (Bitmap.asShared copies to memory).
int ashmem_get_size_region(int) { return -1; }

// --- TextureView's SurfaceTexture: none ---

ASurfaceTexture* ASurfaceTexture_fromSurfaceTexture(JNIEnv*, jobject) { return nullptr; }
void ASurfaceTexture_release(ASurfaceTexture*) {}

// --- ImageReader windows (HardwareRenderer.createHardwareBitmap): none ---

media_status_t AImageReader_newWithUsage(int32_t, int32_t, int32_t, uint64_t, int32_t, AImageReader** reader) {
    if (reader) *reader = nullptr;
    return AMEDIA_ERROR_UNSUPPORTED;
}
void AImageReader_delete(AImageReader*) {}
media_status_t AImageReader_getWindow(AImageReader*, ANativeWindow** window) {
    if (window) *window = nullptr;
    return AMEDIA_ERROR_UNSUPPORTED;
}
media_status_t AImageReader_acquireNextImage(AImageReader*, AImage** image) {
    if (image) *image = nullptr;
    return AMEDIA_ERROR_UNSUPPORTED;
}
void AImage_delete(AImage*) {}
media_status_t AImage_getHardwareBuffer(const AImage*, AHardwareBuffer** buffer) {
    if (buffer) *buffer = nullptr;
    return AMEDIA_ERROR_UNSUPPORTED;
}

}  // extern "C"

namespace android {
ANativeWindowBuffer* AHardwareBuffer_to_ANativeWindowBuffer(AHardwareBuffer*) { return nullptr; }
// Android libEGL's own blob cache file (the driver's shaders); ANGLE keeps none of its own. HWUI's
// ShaderCache (Skia's programs) has its file regardless.
void egl_set_cache_filename(const char*) {}
}  // namespace android

// --- Skia's AHardwareBuffer import: nothing to import ---

namespace GrAHardwareBufferUtils {
SkColorType GetSkColorTypeFromBufferFormat(uint32_t) { return kRGBA_8888_SkColorType; }
GrBackendFormat GetBackendFormat(GrDirectContext*, AHardwareBuffer*, uint32_t, bool) { return GrBackendFormat(); }
GrBackendTexture MakeBackendTexture(GrDirectContext*, AHardwareBuffer*, int, int, DeleteImageProc*, UpdateImageProc*,
                                    TexImageCtx*, bool, const GrBackendFormat&, bool, bool) {
    return GrBackendTexture();
}
}  // namespace GrAHardwareBufferUtils

// --- Ultra HDR JPEG (YuvImage.compressToJpegR, API 34): libultrahdr is not built ---

namespace android::ultrahdr {
status_t JpegR::encodeJPEGR(jr_uncompressed_ptr, jr_uncompressed_ptr, ultrahdr_transfer_function,
                            jr_compressed_ptr, int, jr_exif_ptr) {
    return ERROR_JPEGR_UNSUPPORTED_FEATURE;
}
}  // namespace android::ultrahdr

// --- libandroidfw's nine-patch (ResourceTypes.cpp) ---

namespace android {

static void fill9patchOffsets(Res_png_9patch* patch) {
    patch->xDivsOffset = sizeof(Res_png_9patch);
    patch->yDivsOffset = patch->xDivsOffset + (patch->numXDivs * sizeof(int32_t));
    patch->colorsOffset = patch->yDivsOffset + (patch->numYDivs * sizeof(int32_t));
}

static uint32_t swap32(uint32_t v) { return __builtin_bswap32(v); }

void Res_png_9patch::fileToDevice() {
    int32_t* xDivs = getXDivs();
    for (int i = 0; i < numXDivs; i++) xDivs[i] = static_cast<int32_t>(swap32(static_cast<uint32_t>(xDivs[i])));
    int32_t* yDivs = getYDivs();
    for (int i = 0; i < numYDivs; i++) yDivs[i] = static_cast<int32_t>(swap32(static_cast<uint32_t>(yDivs[i])));
    paddingLeft = static_cast<int32_t>(swap32(static_cast<uint32_t>(paddingLeft)));
    paddingRight = static_cast<int32_t>(swap32(static_cast<uint32_t>(paddingRight)));
    paddingTop = static_cast<int32_t>(swap32(static_cast<uint32_t>(paddingTop)));
    paddingBottom = static_cast<int32_t>(swap32(static_cast<uint32_t>(paddingBottom)));
    uint32_t* colors = getColors();
    for (int i = 0; i < numColors; i++) colors[i] = swap32(colors[i]);
}

size_t Res_png_9patch::serializedSize() const {
    return 32 + numXDivs * sizeof(int32_t) + numYDivs * sizeof(int32_t) + numColors * sizeof(uint32_t);
}

Res_png_9patch* Res_png_9patch::deserialize(void* inData) {
    Res_png_9patch* patch = reinterpret_cast<Res_png_9patch*>(inData);
    patch->wasDeserialized = true;
    fill9patchOffsets(patch);
    return patch;
}

}  // namespace android
