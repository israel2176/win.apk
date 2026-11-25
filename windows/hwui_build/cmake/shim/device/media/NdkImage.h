// The NDK's AImage, as HardwareRenderer.createHardwareBitmap reads one (an ImageReader's image's
// AHardwareBuffer). Win.apk has no hardware bitmaps: hwui_glue's AImageReader fails to be made.
#pragma once
#include <stdint.h>
#include <media/NdkMediaError.h>
#include <android/hardware_buffer.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct AImage AImage;
enum { AIMAGE_FORMAT_RGBA_8888 = 0x1 };
void AImage_delete(AImage* image);
media_status_t AImage_getHardwareBuffer(const AImage* image, AHardwareBuffer** buffer);
#ifdef __cplusplus
}
#endif
