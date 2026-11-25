#!/usr/bin/env python3
"""The source changes the AOSP 14 HWUI closure needs to build for Windows with clang on MinGW-w64
(libstdc++, not Soong's libc++). Each is a literal replacement, applied once (idempotent)."""
import os
import sys

SRC = os.path.abspath(sys.argv[1])

EDITS = [
    # libstdc++'s allocator_traits rebinds through __replace_first_arg, which a non-type template
    # parameter defeats; libc++ falls back on the allocator itself. Spell the rebind out.
    ('native_libs/ui/include/ui/FatVector.h',
     '    typedef T value_type; // needed to implement std::allocator\n',
     '    typedef T value_type; // needed to implement std::allocator\n'
     '    template <class U>\n    struct rebind {\n        typedef InlineStdAllocator<U, SIZE> other;\n    };\n'),
    # CommonPool's thread ids are int (Bionic's pid_t); MinGW's pid_t is 64 bits.
    ('hwui/renderthread/HintSessionWrapper.cpp',
     '    std::vector<pid_t> tids = CommonPool::getThreadIds();\n',
     '    std::vector<int> tids = CommonPool::getThreadIds();\n'),
    ('hwui/renderthread/CanvasContext.cpp',
     '                                     int32_t uiThreadId, int32_t renderThreadId) {\n',
     '                                     pid_t uiThreadId, pid_t renderThreadId) {\n'),
    # libutils' Thread::getTid() is Bionic's (the kernel thread id): the render thread keeps its
    # Windows thread id itself, which gettid() (the compat header) answers on the calling thread.
    ('hwui/renderthread/RenderThread.h',
     '    virtual bool threadLoop() override;\n',
     '    virtual bool threadLoop() override;\n\npublic:\n    pid_t getTid() const { return mTid; }\n\n'
     'private:\n    std::atomic<pid_t> mTid{-1};\n'),
    ('hwui/renderthread/RenderThread.cpp',
     'bool RenderThread::threadLoop() {\n',
     'bool RenderThread::threadLoop() {\n    mTid = gettid();\n'),

    # The window's EGL surface (hwui_glue/window_surface.h): on Android eglCreateWindowSurface
    # takes the ANativeWindow and SurfaceFlinger composes its buffers. Here ANGLE's
    # EGLNativeWindowType is an HWND, and every Android window is a layer of the one host window:
    # HWUI draws each window into a pbuffer the size of its window, which the compositor (in the
    # same EGL context, on the render thread) samples as a texture when the window posts a frame.
    ('hwui/renderthread/EglManager.h',
     '    Result<EGLSurface, EGLint> createSurface(EGLNativeWindowType window, ColorMode colorMode,\n',
     '    Result<EGLSurface, EGLint> createSurface(ANativeWindow* window, ColorMode colorMode,\n'),
    ('hwui/renderthread/EglManager.cpp',
     'Result<EGLSurface, EGLint> EglManager::createSurface(EGLNativeWindowType window,\n',
     'Result<EGLSurface, EGLint> EglManager::createSurface(ANativeWindow* window,\n'),
    ('hwui/renderthread/EglManager.cpp',
     '    EGLSurface surface = eglCreateWindowSurface(mEglDisplay, config, window, attribs);\n',
     '    EGLSurface surface = winapk::hwui::createWindowSurface(mEglDisplay, config, window, attribs);\n'),
    ('hwui/renderthread/EglManager.cpp',
     '    mEglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);\n',
     '    mEglDisplay = winapk::hwui::eglDisplay();\n'),
    # Windows' surfaces are pbuffers the compositor binds as textures.
    ('hwui/renderthread/EglManager.cpp',
     '                        EGL_WINDOW_BIT | eglSwapBehavior,\n',
     '                        EGL_WINDOW_BIT | EGL_PBUFFER_BIT | eglSwapBehavior,\n'
     '                        EGL_BIND_TO_TEXTURE_RGBA,\n                        EGL_TRUE,\n', 4),
    ('hwui/renderthread/EglManager.cpp',
     '    if (!eglDestroySurface(mEglDisplay, surface)) {\n',
     '    winapk::hwui::windowSurfaceDestroyed(surface);\n    if (!eglDestroySurface(mEglDisplay, surface)) {\n'),
    ('hwui/renderthread/EglManager.cpp',
     '#include "EglManager.h"\n',
     '#include "EglManager.h"\n#include "window_surface.h"\n'),
    # A window whose size changed gets a pbuffer of its new size before its next frame (on
    # Android the next buffer dequeued has it).
    ('hwui/pipeline/skia/SkiaOpenGLPipeline.cpp',
     '        "drawRenderNode called on a context with no surface!");\n    return mEglManager.beginFrame(mEglSurface);\n',
     '        "drawRenderNode called on a context with no surface!");\n'
     '    if (winapk::hwui::windowResized(mNativeWindow.get(), mEglSurface)) setSurface(mNativeWindow.get(), mSwapBehavior);\n'
     '    return mEglManager.beginFrame(mEglSurface);\n'),
    # The window posted a frame: the compositor composes the host window.
    ('hwui/pipeline/skia/SkiaOpenGLPipeline.cpp',
     '    if (*requireSwap && (CC_UNLIKELY(!mEglManager.swapBuffers(frame, screenDirty)))) {\n        return false;\n    }\n',
     '    if (*requireSwap && (CC_UNLIKELY(!mEglManager.swapBuffers(frame, screenDirty)))) {\n        return false;\n    }\n'
     '    if (*requireSwap) {\n'
     '        winapk::hwui::windowPosted(mNativeWindow.get(), mEglSurface,\n'
     '                                   currentFrameInfo->get(FrameInfoIndex::IntendedVsync));\n'
     '    }\n'),
    ('hwui/pipeline/skia/SkiaOpenGLPipeline.cpp',
     '#include "SkiaOpenGLPipeline.h"\n',
     '#include "SkiaOpenGLPipeline.h"\n#include "window_surface.h"\n'),
    # ANativeWindow_fromSurface is Win.apk's (hwui_glue), not libandroid's.
    ('hwui/jni/android_graphics_HardwareRenderer.cpp',
     '    void* handle_ = dlopen("libandroid.so", RTLD_NOW | RTLD_NODELETE);\n'
     '    fromSurface = (ANW_fromSurface)dlsym(handle_, "ANativeWindow_fromSurface");\n',
     '    fromSurface = &ANativeWindow_fromSurface;\n'),
    ('hwui/jni/android_graphics_HardwareRenderer.cpp',
     'ANW_fromSurface fromSurface;\n',
     'ANW_fromSurface fromSurface;\nextern "C" ANativeWindow* ANativeWindow_fromSurface(JNIEnv* env, jobject surface);\n'),
    # Hardware bitmaps: none (AImageReader never gives an image here, nor a Bitmap a buffer).
    ('hwui/jni/android_graphics_HardwareRenderer.cpp',
     '    sk_sp<Bitmap> bitmap = Bitmap::createFrom(buffer, cs);\n',
     '    sk_sp<Bitmap> bitmap;\n    return nullptr;  // Win.apk: no hardware bitmaps\n'),
    ('hwui/apex/android_bitmap.cpp',
     '    AHardwareBuffer* buffer = bitmap->hardwareBuffer();\n',
     '    AHardwareBuffer* buffer = nullptr;  // Win.apk: no hardware bitmaps\n    (void)bitmap;\n'),
    ('hwui/apex/android_bitmap.cpp',
     '    if (buffer) {\n        AHardwareBuffer_acquire(buffer);\n    }\n',
     '    // (no buffer to acquire)\n'),
    # Bitmap.copy(HARDWARE) and a decode to HARDWARE: no gralloc buffer, so a heap bitmap with the
    # pixels (the host build's branch allocated an empty one: the copy drew nothing).
    ('hwui/hwui/Bitmap.cpp',
     '#else\n    return Bitmap::allocateHeapBitmap(bitmap.info());\n#endif\n',
     '#else\n    sk_sp<Bitmap> heap = Bitmap::allocateHeapBitmap(bitmap.info());\n'
     '    if (heap && !bitmap.readPixels(heap->info(), heap->pixels(), heap->rowBytes(), 0, 0)) return nullptr;\n'
     '    return heap;\n#endif\n'),
    # FileBlobCache (ANGLE's program cache and HWUI's Skia shader cache) on Windows: the file is
    # read, not mapped (no mmap here); opened in binary mode (MinGW's default text mode turns each
    # \n written into \r\n); and kept writable, since Windows deletes no read-only file and the
    # next write deletes the old one first (it failed with "Permission denied", every run).
    ('native_opengl/EGL/FileBlobCache.cpp', '#include <unistd.h>\n', '#include <unistd.h>\n\n#include <vector>\n'),
    ('native_opengl/EGL/FileBlobCache.cpp',
     'int fd = open(mFilename.c_str(), O_RDONLY, 0);\n',
     'int fd = open(mFilename.c_str(), O_RDONLY | O_BINARY, 0);\n'),
    ('native_opengl/EGL/FileBlobCache.cpp',
     '        uint8_t* buf = reinterpret_cast<uint8_t*>(mmap(nullptr, fileSize,\n'
     '                PROT_READ, MAP_PRIVATE, fd, 0));\n'
     '        if (buf == MAP_FAILED) {\n',
     '        std::vector<uint8_t> contents(fileSize);  // Win.apk: read, not mapped\n'
     '        uint8_t* buf = contents.data();\n'
     '        if (fileSize < headerSize || read(fd, buf, fileSize) != static_cast<int>(fileSize)) {\n'),
    ('native_opengl/EGL/FileBlobCache.cpp',
     'open(fname, O_CREAT | O_EXCL | O_RDWR, 0)',
     'open(fname, O_CREAT | O_EXCL | O_RDWR | O_BINARY, S_IRUSR | S_IWUSR)', 2),
    ('native_opengl/EGL/FileBlobCache.cpp',
     '                if (unlink(fname) == -1) {\n',
     '                chmod(fname, S_IRUSR | S_IWUSR);  // Win.apk: an older build left it read-only\n'
     '                if (unlink(fname) == -1) {\n'),
    ('native_opengl/EGL/FileBlobCache.cpp',
     '        fchmod(fd, S_IRUSR);\n',
     '        // Win.apk: left writable (Windows deletes no read-only file).\n'),
    # HardwareBufferRenderer (API 34, rendering into an AHardwareBuffer): none here.
    ('hwui/pipeline/skia/SkiaPipeline.cpp',
     '        mBufferSurface = SkSurface::MakeFromAHardwareBuffer(\n'
     '                mRenderThread.getGrContext(), mHardwareBuffer, kTopLeft_GrSurfaceOrigin,\n'
     '                bufferColorSpace, nullptr, true);\n',
     '        mBufferSurface = nullptr;\n'),
    ('hwui/apex/jni_runtime.cpp',
     '            REG_JNI(register_android_graphics_pdf_PdfDocument),\n'
     '            REG_JNI(register_android_graphics_pdf_PdfEditor),\n'
     '            REG_JNI(register_android_graphics_pdf_PdfRenderer),\n',
     '            // Win.apk: no PdfDocument, PdfEditor, PdfRenderer (libpdfium).\n'),
    # The compositor (hwui_build/glue) posts to the render thread.
    ('hwui/renderthread/RenderThread.h',
     '    static bool hasInstance();\n    static RenderThread& getInstance();\n',
     'public:\n    static bool hasInstance();\n    static RenderThread& getInstance();\nprivate:\n'),
    # The Vulkan pipeline is not built (no Vulkan here; the GL pipeline is the one chosen).
    ('hwui/renderthread/CanvasContext.cpp',
     '                                     std::make_unique<skiapipeline::SkiaVulkanPipeline>(thread),\n',
     '                                     std::make_unique<skiapipeline::SkiaOpenGLPipeline>(thread),'
     '  // Win.apk: no Vulkan\n'),
    ('hwui/renderthread/CanvasContext.cpp',
     '            skiapipeline::SkiaVulkanPipeline::invokeFunctor(thread, functor);\n',
     '            skiapipeline::SkiaOpenGLPipeline::invokeFunctor(thread, functor);  // Win.apk: no Vulkan\n'),
    ('hwui/pipeline/skia/SkiaRecordingCanvas.cpp',
     '        functorDrawable = mDisplayList->allocateDrawable<VkFunctorDrawable>(functor, asSkCanvas());\n',
     '        functorDrawable = mDisplayList->allocateDrawable<GLFunctorDrawable>(functor, asSkCanvas());'
     '  // Win.apk: no Vulkan\n'),
    ('hwui/renderthread/RenderThread.cpp',
     '            return skiapipeline::SkiaVulkanPipeline::allocateHardwareBitmap(*this, skBitmap);\n',
     '            return nullptr;  // Win.apk: no Vulkan\n'),
    # eglBeginFrame is Android libEGL's (frame timing hints); ANGLE has none.
    ('hwui/renderthread/EglManager.cpp',
     '    eglBeginFrame(mEglDisplay, surface);\n',
     '    // (eglBeginFrame: Android libEGL\'s, not ANGLE\'s)\n'),
    # Vulkan's queue ownership hand-off (the Vulkan pipeline's only; it is not built).
    ('hwui/AutoBackendTextureRelease.cpp',
     '    if (mBackendTexture.isValid()) {\n        // Passing in VK_IMAGE_LAYOUT_UNDEFINED means we keep the old layout.\n',
     '    if ((false)) {\n        // Passing in VK_IMAGE_LAYOUT_UNDEFINED means we keep the old layout.\n'),
    ('hwui/AutoBackendTextureRelease.cpp',
     '        GrBackendSurfaceMutableState newState(VK_IMAGE_LAYOUT_UNDEFINED,\n'
     '                                              VK_QUEUE_FAMILY_FOREIGN_EXT);\n',
     '        GrBackendSurfaceMutableState newState;\n'),
    # Zygote's Vulkan driver preload (never the pipeline here).
    ('hwui/apex/jni_runtime.cpp',
     '        vkEnumerateInstanceVersion(&apiVersion);\n',
     '        apiVersion = 0;  // Win.apk: no Vulkan\n'),
    # libandroid's ASurfaceControl (a window's SurfaceControl given to HWUI: BLAST, WebView's
    # overlays): Win.apk's windows have none (useBLAST is false); the functions stay null, as the
    # render thread never calls them without a SurfaceControl.
    ('hwui/renderthread/RenderThread.cpp',
     'ASurfaceControlFunctions::ASurfaceControlFunctions() {\n'
     '    void* handle_ = dlopen("libandroid.so", RTLD_NOW | RTLD_NODELETE);\n',
     'ASurfaceControlFunctions::ASurfaceControlFunctions() {\n'
     '    std::memset(this, 0, sizeof *this);  // Win.apk: no ASurfaceControl\n'
     '    if ((true)) return;\n'
     '    void* handle_ = dlopen("libandroid.so", RTLD_NOW | RTLD_NODELETE);\n'),
    # The render thread makes no Vulkan manager (only the Vulkan pipeline asks for one).
    ('hwui/renderthread/RenderThread.cpp',
     '    mRenderState = new RenderState(*this);\n    mVkManager = VulkanManager::getInstance();\n',
     '    mRenderState = new RenderState(*this);\n    mVkManager = nullptr;  // Win.apk: no Vulkan\n'),
    # A window's surface is a pbuffer the compositor samples (glue/layers.cpp): its swap is only
    # the end of the frame, with no damage to pass on and no buffer to post; ANGLE need not have
    # EGL_KHR_swap_buffers_with_damage.
    ('hwui/renderthread/EglManager.cpp',
     '    LOG_ALWAYS_FATAL_IF(!extensions.has("EGL_KHR_swap_buffers_with_damage"),\n'
     '                        "Missing required extension EGL_KHR_swap_buffers_with_damage");\n',
     '    // (EGL_KHR_swap_buffers_with_damage: not needed, Win.apk\'s windows are pbuffers)\n'),
    ('hwui/renderthread/EglManager.cpp',
     '    eglSwapBuffersWithDamageKHR(mEglDisplay, frame.mSurface, rects, screenDirty.isEmpty() ? 0 : 1);\n',
     '    eglSwapBuffers(mEglDisplay, frame.mSurface);  // Win.apk: a pbuffer, composed by the glue\n'),
    # Skia: a format GL says has no multisample counts (GL_NUM_SAMPLE_COUNTS 0, as Direct3D's WARP
    # through ANGLE answers) is still renderable with one sample; Skia left it with no sample
    # counts at all, so not renderable, and HWUI then had no surface for a window.
    ('skia14/src/gpu/ganesh/gl/GrGLCaps.cpp',
     '                if (count) {\n                    std::unique_ptr<int[]> temp(new int[count]);\n',
     '                if (!count) fFormatTable[i].fColorSampleCounts.push_back(1);  // Win.apk: no MSAA\n'
     '                if (count) {\n                    std::unique_ptr<int[]> temp(new int[count]);\n'),
    # libutils' systemTime on Windows was the wall clock for every clock id. HWUI compares its
    # times with the frame's vsync from Java's Choreographer (System.nanoTime: on HotSpot for Windows,
    # QueryPerformanceCounter as below), as Android's are one clock (CLOCK_MONOTONIC): on the wall
    # clock the render thread took every frame for one from 56 years before and dropped frames,
    # and the frame times were nonsense. The monotonic clocks are now HotSpot's javaTimeNanos.
    ('system_core/libutils/Timers.cpp',
     '    // Windows is windows.\n    timeval t = {};\n',
     '    // Windows is windows.\n'
     '#if defined(_WIN32)\n'
     '    if (clock != SYSTEM_TIME_REALTIME) {  // Win.apk: HotSpot\'s System.nanoTime\n'
     '        static const double freq = [] {\n'
     '            LARGE_INTEGER f;\n'
     '            QueryPerformanceFrequency(&f);\n'
     '            return static_cast<double>(f.QuadPart);\n'
     '        }();\n'
     '        LARGE_INTEGER c;\n'
     '        QueryPerformanceCounter(&c);\n'
     '        return static_cast<nsecs_t>((static_cast<double>(c.QuadPart) / freq) * 1000000000.0);\n'
     '    }\n'
     '#endif\n'
     '    timeval t = {};\n'),
    ('system_core/libutils/Timers.cpp',
     '#include <utils/Timers.h>\n',
     '#include <utils/Timers.h>\n#if defined(_WIN32)\n#include <windows.h>\n#endif\n'),
    # The shader caches' files: Java names them by their Android path (Win.apk's File keeps it,
    # /data/user/0/<pkg>/cache/...); native code opens host paths (glue: WinApkPaths.hostPath).
    ('hwui/jni/android_graphics_HardwareRenderer.cpp',
     '        jstring diskCachePath, jstring skiaDiskCachePath) {\n',
     '        jstring diskCachePath, jstring skiaDiskCachePath) {\n'
     '    diskCachePath = winapk_host_path(env, diskCachePath);  // Win.apk: the host file\n'
     '    skiaDiskCachePath = winapk_host_path(env, skiaDiskCachePath);\n'),
    ('hwui/jni/android_graphics_HardwareRenderer.cpp',
     'ANW_fromSurface fromSurface;\n',
     'ANW_fromSurface fromSurface;\njstring winapk_host_path(JNIEnv* env, jstring path);  // hwui_build/glue\n'),
    # The caches' limits follow the display (CacheManager sizes them from DeviceInfo, AOSP's memory
    # policy): computed when the render thread starts, before the window's process initializer
    # has told DeviceInfo the display (Win.apk's Choreographer starts the thread first), they were
    # a default 1080x1920 phone's. Again when the GL context is made, as the display is known then.
    ('hwui/renderthread/CacheManager.cpp',
     'void CacheManager::reset(sk_sp<GrDirectContext> context) {\n',
     'void CacheManager::reset(sk_sp<GrDirectContext> context) {\n'
     '    mMaxSurfaceArea = static_cast<size_t>((DeviceInfo::getWidth() * DeviceInfo::getHeight()) *\n'
     '                                          mMemoryPolicy.initialMaxSurfaceAreaScale);  // Win.apk\n'
     '    setupCacheLimits();\n'),
    # libutils' Looper on hwui_glue's emulated eventfd and epoll: sys/eventfd.h (its read, write
    # and close) before anything that closes a descriptor (unique_fd).
    ('system_core/libutils/Looper.cpp',
     '#include <utils/Looper.h>\n',
     '#include <sys/eventfd.h>\n#include <utils/Looper.h>\n'),
    ('hwui/apex/jni_runtime.cpp',
     '            REG_JNI(register_android_graphics_HardwareBufferRenderer),\n',
     '            // Win.apk: no HardwareBufferRenderer (AHardwareBuffer).\n'),
]

# HWUI says "has the render thread and the GPU pipeline" as __ANDROID__ (Layoutlib, its host build,
# has neither). Here those parts are built: HWUI_DEVICE is defined for libhwui and stands for it,
# except where __ANDROID__ really means Bionic, the device's kernel or a device-only library.
HOST_KEEP = {
    'jni/Typeface.cpp',              # statx and fs-verity of font files
    'jni/graphics_jni_helpers.h',    # @CriticalNative takes no JNIEnv only under ART
    'Properties.cpp',                # sysprop-generated properties
    'jni/ImageDecoder.cpp', 'jni/BitmapFactory.cpp',  # F_DUPFD_CLOEXEC
    'jni/android_graphics_ColorSpace.cpp',            # __fp16
    'jni/HardwareBufferHelpers.cpp',  # libandroid's AHardwareBuffer <-> HardwareBuffer
    'jni/Gainmap.cpp', 'effects/GainmapRenderer.cpp', 'Tonemapper.cpp',  # libtonemap, Parcel
    'hwui/Bitmap.h', 'hwui/Bitmap.cpp',  # hardware bitmaps: AHardwareBuffer, gralloc
    'HardwareBitmapUploader.h',
    'jni/Bitmap.cpp', 'jni/Region.cpp', 'jni/ScopedParcel.h', 'jni/ScopedParcel.cpp',  # Parcel: Win.apk's own
    'apex/android_bitmap.cpp',       # AndroidBitmap_getHardwareBuffer
}
hw = os.path.join(SRC, 'hwui')
for root, dirs, files in os.walk(hw):
    for fn in files:
        if not fn.endswith(('.h', '.cpp')):
            continue
        p = os.path.join(root, fn)
        s = open(p, encoding='utf-8', errors='surrogateescape').read()
        if os.path.relpath(p, hw) in HOST_KEEP:
            if 'HWUI_DEVICE' in s:  # a file added to the list after an earlier run
                open(p, 'w', encoding='utf-8', errors='surrogateescape').write(s.replace('HWUI_DEVICE', '__ANDROID__'))
            continue
        if '__ANDROID__' in s:
            open(p, 'w', encoding='utf-8', errors='surrogateescape').write(s.replace('__ANDROID__', 'HWUI_DEVICE'))

bad = 0
for e in EDITS:
    rel, old, new = e[:3]
    times = e[3] if len(e) > 3 else 1  # how many times the text is there
    p = os.path.join(SRC, rel)
    s = open(p, encoding='utf-8').read()
    if new in s:
        continue
    if s.count(old) != times:
        print('patch_src: %s: the text to replace is not there %d time(s): %r' % (rel, times, old[:60]))
        bad += 1
        continue
    open(p, 'w', encoding='utf-8').write(s.replace(old, new))
sys.exit(1 if bad else 0)
