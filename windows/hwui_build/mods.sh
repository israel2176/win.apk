# The Android.bp modules built, those built with their android (device) sections, and the sources
# left out (regular expression on the path).
MODS=libz,libpng,libjpeg,libwebp-decode,libwebp-encode,libexpat,libft2,libwuffs_mirror_release_c,libskia,libharfbuzz_ng,libicuuc,libicui18n,libminikin,libutils,libcutils,liblog,libbase,libfmt,libnativehelper,libhwui,libgif,libarect,libEGL_blobCache
ANDROID=libskia,libhwui
# Skia: Vulkan, HEIF/RAW/MPF decoders, Perfetto, POSIX ports (Windows' are in).
# HWUI: Vulkan, PdfEditor and PdfRenderer (pdfium; PdfDocument is built: it writes with Skia's PDF backend,
# which printing needs), graphics stats (protobuf), and for now what lives on AHardwareBuffer
# (hardware bitmaps' upload, TextureView's layer and PixelCopy: glue/hwui_stubs.cpp, HardwareBufferRenderer).
DROP='/vk/|Vk[A-Z]|/gpu/vk|SkHeifCodec|Perfetto|SkRawCodec|SkJpegMultiPicture|SkJpegXmp|SkJpegSegmentScan|SkDebug_stdio|SkOSFile_posix|SkOSLibrary_posix|GrVulkanMemoryAllocator|AHardwareBuffer|hwui/jni/pdf/Pdf(Editor|Renderer|Utils)\.cpp|GraphicsStatsService|\.proto$|\.sysprop$|Vulkan|VkFunctor|VkInterop|hwui/HardwareBitmapUploader\.cpp|hwui/jni/android_graphics_HardwareBufferRenderer\.cpp|libnativehelper/JNIPlatformHelp\.c|libnativehelper/file_descriptor_jni\.c|hwui/apex/LayoutlibLoader\.cpp|hwui/pipeline/skia/LayerDrawable\.cpp|hwui/Readback\.cpp'
