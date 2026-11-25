#pragma once
#include <media/NdkImage.h>
#include <android/native_window.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct AImageReader AImageReader;
media_status_t AImageReader_newWithUsage(int32_t width, int32_t height, int32_t format, uint64_t usage,
                                         int32_t maxImages, AImageReader** reader);
void AImageReader_delete(AImageReader* reader);
media_status_t AImageReader_getWindow(AImageReader* reader, ANativeWindow** window);
media_status_t AImageReader_acquireNextImage(AImageReader* reader, AImage** image);
#ifdef __cplusplus
}
#endif
