// hwui_stubs.cpp -- HWUI's own parts that need what Win.apk has not (built into libhwui, with its
// flags): Vulkan's manager (never made: ro.hwui.use_vulkan is not set, the GL pipeline runs; the
// Vulkan pipeline is not built, patch_src.py), and what
// reads an AHardwareBuffer: a TextureView's layer (LayerDrawable; no SurfaceTexture is ever
// attached, DeferredLayerUpdater has nothing to draw) and Readback (PixelCopy of a window's last
// buffer, a hardware bitmap's copy), which answer as a device does when the source has no buffer.
#include "HardwareBitmapUploader.h"
#include "Readback.h"
#include "pipeline/skia/LayerDrawable.h"
#include "renderthread/RenderThread.h"
#include "renderthread/VulkanManager.h"
#include "window_surface.h"

#include <SkBitmap.h>
#include <SkCanvas.h>
#include <SkImage.h>
#include <SkPaint.h>

#include <log/log.h>
#include <surfacetexture/surface_texture_platform.h>

// TextureView's SurfaceTexture (ASurfaceTexture_fromSurfaceTexture answers none, stubs.cpp): never
// called with one.
namespace android {
unsigned int ASurfaceTexture_getCurrentTextureTarget(ASurfaceTexture*) { return 0; }
void ASurfaceTexture_takeConsumerOwnership(ASurfaceTexture*) {}
void ASurfaceTexture_releaseConsumerOwnership(ASurfaceTexture*) {}
AHardwareBuffer* ASurfaceTexture_dequeueBuffer(ASurfaceTexture*, int*, android_dataspace*, AHdrMetadataType*,
                                               android_cta861_3_metadata*, android_smpte2086_metadata*, float*,
                                               uint32_t*, bool* newContent, ASurfaceTexture_createReleaseFence,
                                               ASurfaceTexture_fenceWait, void*, ARect*) {
    if (newContent) *newContent = false;
    return nullptr;
}
}  // namespace android

namespace android::uirenderer {

namespace renderthread {
sp<VulkanManager> VulkanManager::getInstance() {
    LOG_ALWAYS_FATAL("Vulkan is not available");
    return nullptr;
}
sp<VulkanManager> VulkanManager::peekInstance() { return nullptr; }
void VulkanManager::initialize() { LOG_ALWAYS_FATAL("Vulkan is not available"); }
sk_sp<GrDirectContext> VulkanManager::createContext(GrContextOptions&, ContextType) { return nullptr; }
status_t VulkanManager::fenceWait(int, GrDirectContext*) { return INVALID_OPERATION; }
status_t VulkanManager::createReleaseFence(int* fence, GrDirectContext*) {
    *fence = -1;
    return INVALID_OPERATION;
}
}  // namespace renderthread

// Hardware bitmaps are not made (Bitmap.Config.HARDWARE copies stay in memory).
void HardwareBitmapUploader::initialize() {}

namespace skiapipeline {

void LayerDrawable::onDraw(SkCanvas*) {}
bool LayerDrawable::DrawLayer(GrRecordingContext*, SkCanvas*, Layer*, const SkRect*, const SkRect*, bool) {
    return false;
}
}  // namespace skiapipeline

// PixelCopy of a window: its last frame, read back from its layer (layers.cpp), scaled into the
// request's bitmap as Readback.cpp does with a device's buffer.
void Readback::copySurfaceInto(ANativeWindow* window, const std::shared_ptr<CopyRequest>& request) {
    std::vector<uint32_t> px;
    int w = 0, h = 0;
    if (!window) return request->onCopyFinished(CopyResult::SourceInvalid);
    mRenderThread.requireGlContext();
    if (!winapk::hwui::readWindow(window, &px, &w, &h)) return request->onCopyFinished(CopyResult::SourceEmpty);
    SkBitmap src;
    src.installPixels(SkImageInfo::Make(w, h, kRGBA_8888_SkColorType, kPremul_SkAlphaType, SkColorSpace::MakeSRGB()),
                      px.data(), static_cast<size_t>(w) * 4);
    SkRect srcRect = request->srcRect.toSkRect();
    const SkRect whole = SkRect::MakeIWH(w, h);
    if (srcRect.isEmpty()) {
        srcRect = whole;
    } else if (!srcRect.intersect(whole)) {
        return request->onCopyFinished(CopyResult::UnknownError);
    }
    SkBitmap dst = request->getDestinationBitmap(srcRect.width(), srcRect.height());
    if (dst.isNull() || !dst.getPixels()) return request->onCopyFinished(CopyResult::DestinationInvalid);
    SkCanvas canvas(dst);
    SkPaint paint;
    paint.setBlendMode(SkBlendMode::kSrc);
    canvas.drawImageRect(src.asImage(), srcRect, SkRect::MakeIWH(dst.width(), dst.height()),
                         SkSamplingOptions(SkFilterMode::kLinear), &paint, SkCanvas::kFast_SrcRectConstraint);
    dst.notifyPixelsChanged();
    request->onCopyFinished(CopyResult::Success);
}
CopyResult Readback::copyHWBitmapInto(Bitmap*, SkBitmap*) { return CopyResult::UnknownError; }
CopyResult Readback::copyImageInto(const sk_sp<SkImage>&, SkBitmap*) { return CopyResult::UnknownError; }
CopyResult Readback::copyLayerInto(DeferredLayerUpdater*, SkBitmap*) { return CopyResult::UnknownError; }

}  // namespace android::uirenderer
