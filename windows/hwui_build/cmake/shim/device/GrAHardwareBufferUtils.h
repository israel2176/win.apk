// Skia's GrAHardwareBufferUtils (built only with SK_BUILD_FOR_ANDROID): HWUI's AHardwareBuffer
// users (TextureView's layers, PixelCopy, hardware bitmaps) name it. Win.apk's AHardwareBuffer
// (hwui_glue/ahardwarebuffer.cpp) is plain memory and has no GPU import: these answer "none".
#pragma once
#include "include/core/SkTypes.h"
#include "include/core/SkImageInfo.h"
#include "include/gpu/GrBackendSurface.h"
#include "include/gpu/GrTypes.h"
class GrDirectContext;
extern "C" {
typedef struct AHardwareBuffer AHardwareBuffer;
}
namespace GrAHardwareBufferUtils {
SkColorType GetSkColorTypeFromBufferFormat(uint32_t bufferFormat);
GrBackendFormat GetBackendFormat(GrDirectContext* context, AHardwareBuffer* hardwareBuffer, uint32_t bufferFormat,
                                 bool requireKnownFormat);
typedef void* TexImageCtx;
typedef void (*DeleteImageProc)(TexImageCtx);
typedef void (*UpdateImageProc)(TexImageCtx, GrDirectContext*);
GrBackendTexture MakeBackendTexture(GrDirectContext* context, AHardwareBuffer* hardwareBuffer, int width, int height,
                                    DeleteImageProc* deleteProc, UpdateImageProc* updateProc, TexImageCtx* imageCtx,
                                    bool isProtectedContent, const GrBackendFormat& backendFormat, bool isRenderable,
                                    bool fromAndroidWindow = false);
}  // namespace GrAHardwareBufferUtils
