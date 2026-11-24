// native_text_input.cpp -- com.macapk.loader.WindowsTextInput on Windows: the editor's side of
// Windows' text services (win32_window.h, "text input"). The Java class tells the window which
// editor has the focus and sends copies of it, from the editor's thread; the window's thread
// calls back here with the text services' edits (WindowsTextInput.hostEdit) and, for an editor
// the Java side cannot watch, after each key and touch (hostSync). Both only post to the
// editor's thread.

#include "host_natives.h"
#include "win32_window.h"

#include <cstdio>
#include <vector>

namespace winapk::host {

namespace {

JavaVM* g_vm = nullptr;
jclass g_class = nullptr;  // WindowsTextInput, a global reference
jmethodID g_edit = nullptr, g_sync = nullptr;

std::u16string text_of(JNIEnv* env, jstring s) {
    std::u16string out;
    if (!s) return out;
    const jsize n = env->GetStringLength(s);
    out.resize(static_cast<size_t>(n));
    if (n > 0) env->GetStringRegion(s, 0, n, reinterpret_cast<jchar*>(&out[0]));
    return out;
}

// The window's thread, attached once (a daemon: it never ends before the process).
JNIEnv* window_env() {
    thread_local JNIEnv* env = nullptr;
    if (!env && g_vm) {
        JavaVMAttachArgs args{JNI_VERSION_1_6, const_cast<char*>("winapk-ui"), nullptr};
        if (g_vm->AttachCurrentThreadAsDaemon(reinterpret_cast<void**>(&env), &args) != JNI_OK) env = nullptr;
    }
    return env;
}

void clear(JNIEnv* env) {
    if (!env->ExceptionCheck()) return;
    std::fprintf(stderr, "[win.apk] text input: exception from WindowsTextInput\n");
    env->ExceptionDescribe();
    env->ExceptionClear();
}

class JavaEditor : public TextInputClient {
public:
    void edit(const textinput::Edit& e) override {
        JNIEnv* env = window_env();
        if (!env || !g_edit) return;
        jstring text = env->NewString(reinterpret_cast<const jchar*>(e.text.data()), static_cast<jsize>(e.text.size()));
        jintArray runs = nullptr;
        if (!e.runs.empty()) {
            std::vector<jint> flat;
            for (const textinput::Run& r : e.runs) {
                flat.insert(flat.end(), {r.start, r.end, static_cast<jint>(r.flags), static_cast<jint>(r.foreground),
                                         static_cast<jint>(r.background)});
            }
            runs = env->NewIntArray(static_cast<jsize>(flat.size()));
            if (runs) env->SetIntArrayRegion(runs, 0, static_cast<jsize>(flat.size()), flat.data());
        }
        if (text) {
            env->CallStaticVoidMethod(g_class, g_edit, e.epoch, e.seq, e.start, e.end, text, e.comp_start, e.comp_end,
                                      e.anchor, e.caret, runs);
        }
        clear(env);
        if (runs) env->DeleteLocalRef(runs);
        if (text) env->DeleteLocalRef(text);
    }
    void sync(int epoch) override {
        JNIEnv* env = window_env();
        if (!env || !g_sync) return;
        env->CallStaticVoidMethod(g_class, g_sync, static_cast<jint>(epoch));
        clear(env);
    }
};

JavaEditor g_editor;

bool install(JNIEnv* env, jclass cls) {
    if (g_class) return true;
    env->GetJavaVM(&g_vm);
    g_edit = env->GetStaticMethodID(cls, "hostEdit", "(IIIILjava/lang/String;IIII[I)V");
    g_sync = env->GetStaticMethodID(cls, "hostSync", "(I)V");
    if (env->ExceptionCheck() || !g_edit || !g_sync) {
        env->ExceptionClear();
        g_edit = g_sync = nullptr;
        return false;
    }
    g_class = static_cast<jclass>(env->NewGlobalRef(cls));
    window_text_client(&g_editor);
    return true;
}

textinput::Snapshot snapshot(JNIEnv* env, jint epoch, jint acked, jstring text, jint offset, jint anchor,
                             jint caret) {
    textinput::Snapshot s;
    s.epoch = epoch;
    s.acked = acked;
    s.offset = offset;
    s.text = text_of(env, text);
    s.anchor = anchor;
    s.caret = caret;
    return s;
}

jboolean JNICALL TextInput_focus(JNIEnv* env, jclass cls, jint epoch, jint input_type, jint ime_options,
                                 jboolean watched, jstring text, jint offset, jint anchor, jint caret) {
    if (!window_handle() || !install(env, cls)) return JNI_FALSE;  // headless: nothing to tell
    WindowTextField f;
    f.epoch = epoch;
    f.input_type = input_type;
    f.ime_options = ime_options;
    f.watched = watched == JNI_TRUE;
    window_text_focus(f, snapshot(env, epoch, 0, text, offset, anchor, caret));
    return JNI_TRUE;
}

void JNICALL TextInput_blur(JNIEnv*, jclass, jint epoch) { window_text_blur(epoch); }

void JNICALL TextInput_document(JNIEnv* env, jclass, jint epoch, jint acked, jstring text, jint offset, jint anchor,
                                jint caret) {
    window_text_snapshot(snapshot(env, epoch, acked, text, offset, anchor, caret));
}

void JNICALL TextInput_layout(JNIEnv* env, jclass, jint epoch, jint acked, jfloatArray editor, jfloatArray caret,
                              jint first, jfloatArray boxes, jbooleanArray rtl) {
    textinput::Layout l;
    l.epoch = epoch;
    l.acked = acked;
    auto box = [&](jfloatArray a, textinput::Box* out) {
        if (!a || env->GetArrayLength(a) != 4) return false;
        float v[4];
        env->GetFloatArrayRegion(a, 0, 4, v);
        *out = textinput::Box{v[0], v[1], v[2], v[3]};
        return true;
    };
    l.has_editor = box(editor, &l.editor);
    l.has_caret = box(caret, &l.caret);
    l.first = first;
    const jsize n = boxes ? env->GetArrayLength(boxes) / 4 : 0;
    if (n > 0) {
        std::vector<float> v(static_cast<size_t>(n) * 4);
        env->GetFloatArrayRegion(boxes, 0, n * 4, v.data());
        l.boxes.resize(static_cast<size_t>(n));
        for (jsize i = 0; i < n; ++i) l.boxes[i] = textinput::Box{v[i * 4], v[i * 4 + 1], v[i * 4 + 2], v[i * 4 + 3]};
        l.rtl.assign(static_cast<size_t>(n), false);
        if (rtl && env->GetArrayLength(rtl) >= n) {
            std::vector<jboolean> r(static_cast<size_t>(n));
            env->GetBooleanArrayRegion(rtl, 0, n, r.data());
            for (jsize i = 0; i < n; ++i) l.rtl[i] = r[i] == JNI_TRUE;
        }
    }
    window_text_layout(l);
}

void JNICALL TextInput_softInput(JNIEnv*, jclass, jboolean show) { window_soft_input(show == JNI_TRUE); }

constexpr const char* kCls = "com.macapk.loader.WindowsTextInput";

const HostMethod kMethods[] = {
    WINAPK_HOST_NATIVE(kCls, "nativeFocus", "(IIIZLjava/lang/String;III)Z", TextInput_focus),
    WINAPK_HOST_NATIVE(kCls, "nativeBlur", "(I)V", TextInput_blur),
    WINAPK_HOST_NATIVE(kCls, "nativeDocument", "(IILjava/lang/String;III)V", TextInput_document),
    WINAPK_HOST_NATIVE(kCls, "nativeLayout", "(II[F[FI[F[Z)V", TextInput_layout),
    WINAPK_HOST_NATIVE(kCls, "nativeSoftInput", "(Z)V", TextInput_softInput),
};

}  // namespace

HostMethods native_text_input_methods() { return {kMethods, sizeof(kMethods) / sizeof(kMethods[0])}; }

}  // namespace winapk::host
