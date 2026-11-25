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
}
hw = os.path.join(SRC, 'hwui')
for root, dirs, files in os.walk(hw):
    for fn in files:
        if not fn.endswith(('.h', '.cpp')):
            continue
        p = os.path.join(root, fn)
        if os.path.relpath(p, hw) in HOST_KEEP:
            continue
        s = open(p, encoding='utf-8', errors='surrogateescape').read()
        if '__ANDROID__' in s:
            open(p, 'w', encoding='utf-8', errors='surrogateescape').write(s.replace('__ANDROID__', 'HWUI_DEVICE'))

bad = 0
for rel, old, new in EDITS:
    p = os.path.join(SRC, rel)
    s = open(p, encoding='utf-8').read()
    if new in s:
        continue
    if s.count(old) != 1:
        print('patch_src: %s: the text to replace is not there once' % rel)
        bad += 1
        continue
    open(p, 'w', encoding='utf-8').write(s.replace(old, new))
sys.exit(1 if bad else 0)
