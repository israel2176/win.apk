// nio_fd.cpp -- libnativehelper's java.nio.Buffer and java.io.FileDescriptor helpers for HotSpot
// (they replace its JNIPlatformHelp.c and file_descriptor_jni.c, written for ART's fields:
// Buffer._elementSizeShift, java.nio.NIOAccess, FileDescriptor.descriptor).
//
// HotSpot's Buffer has position, limit and capacity behind its public methods, a direct buffer's
// address through GetDirectBufferAddress, and a heap buffer's array and arrayOffset(). HotSpot's
// FileDescriptor on Windows holds a CRT descriptor (fd) or a Windows handle (handle).
#include <fcntl.h>
#include <io.h>
#include <jni.h>
#include <windows.h>

#include <cstring>

namespace {

jmethodID method(JNIEnv* env, const char* cls, const char* name, const char* sig) {
    jclass c = env->FindClass(cls);
    jmethodID m = c ? env->GetMethodID(c, name, sig) : nullptr;
    if (c) env->DeleteLocalRef(c);
    return m;
}

int call_int(JNIEnv* env, jobject o, const char* name) {
    static jmethodID position = method(env, "java/nio/Buffer", "position", "()I");
    static jmethodID limit = method(env, "java/nio/Buffer", "limit", "()I");
    static jmethodID array_offset = method(env, "java/nio/Buffer", "arrayOffset", "()I");
    jmethodID m = !std::strcmp(name, "position") ? position : !std::strcmp(name, "limit") ? limit : array_offset;
    return env->CallIntMethod(o, m);
}

// log2 of the element size: ByteBuffer 0, Char and Short 1, Int and Float 2, Long and Double 3.
int element_shift(JNIEnv* env, jobject buffer) {
    struct Kind {
        const char* cls;
        int shift;
    };
    static const Kind kKinds[] = {{"java/nio/ByteBuffer", 0},  {"java/nio/CharBuffer", 1},
                                  {"java/nio/ShortBuffer", 1}, {"java/nio/IntBuffer", 2},
                                  {"java/nio/FloatBuffer", 2}, {"java/nio/LongBuffer", 3},
                                  {"java/nio/DoubleBuffer", 3}};
    for (const Kind& k : kKinds) {
        jclass c = env->FindClass(k.cls);
        const bool is = c && env->IsInstanceOf(buffer, c);
        if (c) env->DeleteLocalRef(c);
        if (is) return k.shift;
    }
    return 0;
}

}  // namespace

extern "C" {

jarray jniGetNioBufferBaseArray(JNIEnv* env, jobject buffer) {
    static jmethodID has_array = method(env, "java/nio/Buffer", "hasArray", "()Z");
    static jmethodID array = method(env, "java/nio/Buffer", "array", "()Ljava/lang/Object;");
    if (!env->CallBooleanMethod(buffer, has_array)) return nullptr;
    return static_cast<jarray>(env->CallObjectMethod(buffer, array));
}

// In bytes from the array's first element, as ART's NIOAccess.getBaseArrayOffset answers.
int jniGetNioBufferBaseArrayOffset(JNIEnv* env, jobject buffer) {
    const int shift = element_shift(env, buffer);
    return (call_int(env, buffer, "arrayOffset") + call_int(env, buffer, "position")) << shift;
}

jlong jniGetNioBufferPointer(JNIEnv* env, jobject buffer) {
    void* base = env->GetDirectBufferAddress(buffer);
    if (!base) return 0;
    return reinterpret_cast<jlong>(base) + (static_cast<jlong>(call_int(env, buffer, "position"))
                                            << element_shift(env, buffer));
}

jlong jniGetNioBufferFields(JNIEnv* env, jobject buffer, jint* position, jint* limit, jint* elementSizeShift) {
    *position = call_int(env, buffer, "position");
    *limit = call_int(env, buffer, "limit");
    *elementSizeShift = element_shift(env, buffer);
    return reinterpret_cast<jlong>(env->GetDirectBufferAddress(buffer));
}

jobject AFileDescriptor_create(JNIEnv* env) {
    jclass c = env->FindClass("java/io/FileDescriptor");
    jmethodID init = env->GetMethodID(c, "<init>", "()V");
    jobject o = env->NewObject(c, init);
    env->DeleteLocalRef(c);
    return o;
}

// The CRT descriptor of a FileDescriptor; one made from its handle (a duplicate, which the caller
// may close) when it holds a handle only.
int AFileDescriptor_getFd(JNIEnv* env, jobject fd) {
    jclass c = env->GetObjectClass(fd);
    jfieldID f = env->GetFieldID(c, "fd", "I");
    jfieldID h = env->GetFieldID(c, "handle", "J");
    env->DeleteLocalRef(c);
    if (!f) {
        env->ExceptionClear();
        return -1;
    }
    const int crt = env->GetIntField(fd, f);
    if (crt >= 0 || !h) {
        env->ExceptionClear();
        return crt;
    }
    HANDLE handle = reinterpret_cast<HANDLE>(env->GetLongField(fd, h));
    HANDLE dup = nullptr;
    if (handle == INVALID_HANDLE_VALUE || !handle ||
        !DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        return -1;
    }
    return _open_osfhandle(reinterpret_cast<intptr_t>(dup), _O_RDONLY | _O_BINARY);
}

void AFileDescriptor_setFd(JNIEnv* env, jobject fd, int value) {
    jclass c = env->GetObjectClass(fd);
    jfieldID f = env->GetFieldID(c, "fd", "I");
    env->DeleteLocalRef(c);
    if (f) env->SetIntField(fd, f, value);
    else env->ExceptionClear();
}

}  // extern "C"
