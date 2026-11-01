// native_signin.cpp -- com.macapk.gms.NativeSignIn on Windows: the Google
// sign-in window, upstream's runtime/src/gms_signin.mm on WebView2.
//
// The Java GMS layer decides that an account is needed (a licence check, an
// app's Google sign-in chooser's "Add an account", AccountManager.addAccount,
// the launcher's Settings) and asks the host to put Google's own sign-in page
// in front of the user. Google has no password API, and should not: the
// supported path for a device adding an account is to render
// accounts.google.com/EmbeddedSetup and take the `oauth_token` cookie the
// completed flow deposits. So this file shows that page, waits, reads one
// cookie, and hands it back; GoogleAuthClient exchanges it for a master token.
// Win.apk never sees the password.
//
// What makes Google serve the embedded flow is the User-Agent: an Android
// WebView's with ` MinuteMaid` appended, the suffix a device-setup WebView
// sends. Without it Google serves the desktop sign-in, which never sets
// `oauth_token`. The UA names Win.apk in its Build token rather than a
// handset: it is a statement about the engine, and the device fields are
// our own (the Mac does the same with Mac.apk).
//
// How the flow's end is seen, as upstream: three triggers converging on one
// check of the cookie jar. Every completed navigation; every source change
// (the `#close` fragment Google appends at the end is a same-document
// navigation, which fires no NavigationCompleted, only SourceChanged); and a
// 1-second timer while the window is open.
//
// The page runs in an in-private profile of the app's WebView2 environment
// (WebView2's counterpart of a non-persistent WKWebsiteDataStore): the Google
// session lives only as long as the window, is not written to disk and is not
// the app's own WebView jar. The window closes once the token is in the jar,
// so nothing is left for the next sign-in to find.
//
// presentSignInWindow BLOCKS for as long as a human takes, which is what the
// Java callers expect (they run it off the main thread; NativeSignIn's own
// guards keep it from the UI thread and from stacking windows). The window
// lives on the WebView2 thread (webview2_host.h), which pumps it; the calling
// Java thread waits on an event. Without WebView2 (headless, no runtime) there
// is no window: the call answers null, which the callers treat as a cancel,
// the state a Mac headless run is in.

#include <atomic>
#include <cstdio>
#include <string>

#include <windows.h>

#include "host_natives.h"
#include "webview2_host.h"
#include "win32_window.h"

namespace winapk::host {

namespace {

using wv2::ComPtr;

constexpr wchar_t kClass[] = L"WinApkGoogleSignIn";
constexpr wchar_t kProfile[] = L"signin";
constexpr wchar_t kUrl[] = L"https://accounts.google.com/EmbeddedSetup";
constexpr wchar_t kCookieOrigin[] = L"https://accounts.google.com";
constexpr wchar_t kCookieName[] = L"oauth_token";
// The Android WebView UA plus the MinuteMaid suffix (see the header).
constexpr wchar_t kUserAgent[] =
    L"Mozilla/5.0 (Linux; Android 10; Win.apk Build/PR1; wv) "
    L"AppleWebKit/537.36 (KHTML, like Gecko) Version/4.0 "
    L"Chrome/120.0.0.0 Mobile Safari/537.36 MinuteMaid";
constexpr UINT_PTR kPollTimer = 1;
constexpr UINT kPollMs = 1000;
// A hard ceiling so a forgotten window cannot park a guest thread forever:
// long enough for a password plus a 2FA prompt on a phone.
constexpr UINT_PTR kTimeoutTimer = 2;
constexpr UINT kTimeoutMs = 600 * 1000;

std::atomic<bool> g_active{false};

// One window at a time (g_active), so one of these at a time. WebView2 thread
// only, except `done`, `token` and `finished`, written before `done` is set.
struct Window {
    HWND hwnd = nullptr;
    HWND note = nullptr;  // the one-line reassurance over the page
    HFONT font = nullptr;
    UINT dpi = 96;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> view;
    ComPtr<ICoreWebView2CookieManager> cookies;
    std::wstring folder;
    bool finished = false;
    bool checking = false;  // a GetCookies is in flight
    std::wstring token;
    HANDLE done = nullptr;  // signalled when the window is gone
};

Window* g_window = nullptr;

UINT dpi_of(HWND hwnd) {
    using Fn = UINT(WINAPI*)(HWND);
    static auto fn = reinterpret_cast<Fn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow")));
    const UINT d = fn && hwnd ? fn(hwnd) : 96;
    return d ? d : 96;
}

int note_h(const Window* w) { return MulDiv(24, static_cast<int>(w->dpi), 96); }

void make_font(Window* w) {
    if (w->font) DeleteObject(w->font);
    const int px = MulDiv(12, static_cast<int>(w->dpi), 96);
    w->font = CreateFontW(-px, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    if (w->note) SendMessageW(w->note, WM_SETFONT, reinterpret_cast<WPARAM>(w->font), TRUE);
}

void layout(Window* w) {
    RECT c;
    GetClientRect(w->hwnd, &c);
    const int bar = note_h(w);
    MoveWindow(w->note, 0, 0, c.right, bar, TRUE);
    if (w->controller) {
        RECT web{0, bar, c.right, c.bottom > bar ? c.bottom : bar};
        w->controller->put_Bounds(web);
    }
}

// The window is done: the token is in `token` or stays empty (cancelled,
// timed out, failed). Closing it releases the waiting Java thread (WM_DESTROY).
void finish(Window* w) {
    if (w->finished) return;
    w->finished = true;
    KillTimer(w->hwnd, kPollTimer);
    KillTimer(w->hwnd, kTimeoutTimer);
    PostMessageW(w->hwnd, WM_CLOSE, 0, 0);
}

// The whole detection mechanism: does the jar hold `oauth_token` yet?
void check_cookies(Window* w) {
    if (w->finished || w->checking || !w->cookies) return;
    w->checking = true;
    auto done = wv2::handler<ICoreWebView2GetCookiesCompletedHandler>(
        IID_ICoreWebView2GetCookiesCompletedHandler, [](HRESULT hr, ICoreWebView2CookieList* list) -> HRESULT {
            Window* w = g_window;
            if (!w) return S_OK;
            w->checking = false;
            if (w->finished || FAILED(hr) || !list) return S_OK;
            UINT count = 0;
            list->get_Count(&count);
            for (UINT i = 0; i < count && w->token.empty(); ++i) {
                ComPtr<ICoreWebView2Cookie> c;
                if (FAILED(list->GetValueAtIndex(i, c.put())) || !c) continue;
                LPWSTR raw = nullptr;
                c->get_Name(&raw);
                if (wv2::take_co_string(raw) != kCookieName) continue;
                c->get_Value(&raw);
                w->token = wv2::take_co_string(raw);
            }
            if (!w->token.empty()) {
                std::fprintf(stderr, "[win.apk] gms-signin: the flow finished (oauth_token, %zu chars)\n",
                             w->token.size());
                std::fflush(stderr);
                finish(w);
            }
            return S_OK;
        });
    if (FAILED(w->cookies->GetCookies(kCookieOrigin, done.get()))) w->checking = false;
}

void hook_events(Window* w) {
    EventRegistrationToken token;
    w->view->add_NavigationCompleted(
        wv2::handler<ICoreWebView2NavigationCompletedEventHandler>(
            IID_ICoreWebView2NavigationCompletedEventHandler,
            [](ICoreWebView2* sender, ICoreWebView2NavigationCompletedEventArgs* a) -> HRESULT {
                Window* w = g_window;
                if (!w) return S_OK;
                BOOL ok = FALSE;
                a->get_IsSuccess(&ok);
                LPWSTR raw = nullptr;
                sender->get_Source(&raw);
                const std::wstring src = wv2::take_co_string(raw);
                if (ok) {
                    std::fprintf(stderr, "[win.apk] gms-signin: loaded %s\n", wv2::redact(src).c_str());
                } else {
                    // Not necessarily the end: the flow redirects through
                    // schemes the view refuses, and the token may be in the jar.
                    COREWEBVIEW2_WEB_ERROR_STATUS status = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
                    a->get_WebErrorStatus(&status);
                    std::fprintf(stderr, "[win.apk] gms-signin: navigation failed (%d) at %s\n",
                                 static_cast<int>(status), wv2::redact(src).c_str());
                }
                std::fflush(stderr);
                check_cookies(w);
                return S_OK;
            })
            .get(),
        &token);
    // The `#close` fragment at the end of the flow is a same-document
    // navigation: this is the only event it raises.
    w->view->add_SourceChanged(
        wv2::handler<ICoreWebView2SourceChangedEventHandler>(
            IID_ICoreWebView2SourceChangedEventHandler,
            [](ICoreWebView2*, ICoreWebView2SourceChangedEventArgs*) -> HRESULT {
                if (Window* w = g_window) check_cookies(w);
                return S_OK;
            })
            .get(),
        &token);
    // The flow stays in this window: a pop-up (a help link) loads here too.
    w->view->add_NewWindowRequested(
        wv2::handler<ICoreWebView2NewWindowRequestedEventHandler>(
            IID_ICoreWebView2NewWindowRequestedEventHandler,
            [](ICoreWebView2* sender, ICoreWebView2NewWindowRequestedEventArgs* a) -> HRESULT {
                a->put_Handled(TRUE);
                LPWSTR raw = nullptr;
                a->get_Uri(&raw);
                const std::wstring uri = wv2::take_co_string(raw);
                const std::wstring scheme = wv2::scheme_of(uri);
                if (scheme == L"http" || scheme == L"https") sender->Navigate(uri.c_str());
                return S_OK;
            })
            .get(),
        &token);
    w->view->add_WindowCloseRequested(
        wv2::handler<ICoreWebView2WindowCloseRequestedEventHandler>(
            IID_ICoreWebView2WindowCloseRequestedEventHandler, [](ICoreWebView2*, IUnknown*) -> HRESULT {
                if (Window* w = g_window) finish(w);
                return S_OK;
            })
            .get(),
        &token);
    // The flow ends in custom-scheme redirects WebView2 would otherwise ask
    // Windows to open: never.
    if (auto v18 = w->view.query<ICoreWebView2_18>(IID_ICoreWebView2_18)) {
        v18->add_LaunchingExternalUriScheme(
            wv2::handler<ICoreWebView2LaunchingExternalUriSchemeEventHandler>(
                IID_ICoreWebView2LaunchingExternalUriSchemeEventHandler,
                [](ICoreWebView2*, ICoreWebView2LaunchingExternalUriSchemeEventArgs* a) -> HRESULT {
                    a->put_Cancel(TRUE);
                    if (Window* w = g_window) check_cookies(w);
                    return S_OK;
                })
                .get(),
            &token);
    }
    w->view->add_ProcessFailed(
        wv2::handler<ICoreWebView2ProcessFailedEventHandler>(
            IID_ICoreWebView2ProcessFailedEventHandler,
            [](ICoreWebView2* sender, ICoreWebView2ProcessFailedEventArgs* a) -> HRESULT {
                Window* w = g_window;
                if (!w) return S_OK;
                COREWEBVIEW2_PROCESS_FAILED_KIND kind = COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED;
                a->get_ProcessFailedKind(&kind);
                std::fprintf(stderr, "[win.apk] gms-signin: WebView2 process failed (kind %d)\n",
                             static_cast<int>(kind));
                if (kind == COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED) finish(w);
                else if (kind == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED ||
                         kind == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_UNRESPONSIVE)
                    sender->Reload();
                return S_OK;
            })
            .get(),
        &token);
}

void controller_ready(HRESULT hr, ICoreWebView2Controller* controller) {
    Window* w = g_window;
    if (!w || w->finished) {
        if (controller) controller->Close();
        return;
    }
    if (FAILED(hr) || !controller) {
        std::fprintf(stderr, "[win.apk] gms-signin: no WebView2 controller (0x%08lx)\n",
                     static_cast<unsigned long>(hr));
        finish(w);
        return;
    }
    w->controller = ComPtr<ICoreWebView2Controller>::retain(controller);
    controller->get_CoreWebView2(w->view.put());
    if (!w->view) {
        finish(w);
        return;
    }
    ComPtr<ICoreWebView2Settings> settings;
    w->view->get_Settings(settings.put());
    if (settings) {
        settings->put_AreDevToolsEnabled(FALSE);
        settings->put_IsStatusBarEnabled(FALSE);
        settings->put_AreDefaultContextMenusEnabled(FALSE);
        if (auto s2 = settings.query<ICoreWebView2Settings2>(IID_ICoreWebView2Settings2)) {
            s2->put_UserAgent(kUserAgent);
        }
    }
    if (auto v2 = w->view.query<ICoreWebView2_2>(IID_ICoreWebView2_2)) v2->get_CookieManager(w->cookies.put());
    if (!w->cookies) {
        std::fprintf(stderr, "[win.apk] gms-signin: no cookie manager; the flow's end could not be seen\n");
        finish(w);
        return;
    }
    hook_events(w);
    layout(w);
    controller->put_IsVisible(TRUE);
    if (GetForegroundWindow() == w->hwnd) controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
    std::fprintf(stderr, "[win.apk] gms-signin: window up, loading %s\n", wv2::narrow(kUrl).c_str());
    std::fflush(stderr);
    w->view->Navigate(kUrl);
    SetTimer(w->hwnd, kPollTimer, kPollMs, nullptr);
}

void create_controller(ICoreWebView2Environment* environment) {
    Window* w = g_window;
    if (!w || w->finished) return;
    if (!environment) {
        finish(w);
        return;
    }
    auto done = wv2::handler<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
        IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler,
        [](HRESULT hr, ICoreWebView2Controller* c) -> HRESULT {
            controller_ready(hr, c);
            return S_OK;
        });
    // An in-private profile of its own (WebView2 1.0.1774+): nothing of the
    // Google session is kept. An older runtime has only the default profile;
    // the session would then be the app's WebViews' too, so no sign-in there.
    auto env10 = ComPtr<ICoreWebView2Environment>::retain(environment)
                     .query<ICoreWebView2Environment10>(IID_ICoreWebView2Environment10);
    ComPtr<ICoreWebView2ControllerOptions> options;
    if (env10 && SUCCEEDED(env10->CreateCoreWebView2ControllerOptions(options.put())) && options) {
        options->put_ProfileName(kProfile);
        options->put_IsInPrivateModeEnabled(TRUE);
        const HRESULT hr = env10->CreateCoreWebView2ControllerWithOptions(w->hwnd, options.get(), done.get());
        if (SUCCEEDED(hr)) return;
        controller_ready(hr, nullptr);
        return;
    }
    std::fprintf(stderr, "[win.apk] gms-signin: this WebView2 Runtime (%s) has no private profiles; update it\n",
                 wv2::narrow(wv2::runtime_version()).c_str());
    finish(w);
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* w = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_NCCREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            break;
        }
        case WM_CREATE: {
            w->hwnd = hwnd;
            w->dpi = dpi_of(hwnd);
            // Not decoration: a window asking for a Google password has to say
            // whose page it is, or the honest thing looks like phishing.
            w->note = CreateWindowExW(0, L"STATIC",
                                      L"This is Google's own sign-in page. Win.apk never sees your password.",
                                      WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE, 0, 0, 0, 0, hwnd, nullptr,
                                      GetModuleHandleW(nullptr), nullptr);
            make_font(w);
            layout(w);
            SetTimer(hwnd, kTimeoutTimer, kTimeoutMs, nullptr);
            return 0;
        }
        case WM_SIZE:
            if (w) layout(w);
            return 0;
        case WM_DPICHANGED: {
            w->dpi = HIWORD(wp);
            make_font(w);
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            layout(w);
            return 0;
        }
        case WM_TIMER:
            if (w && wp == kPollTimer) {
                check_cookies(w);
            } else if (w && wp == kTimeoutTimer) {
                std::fprintf(stderr, "[win.apk] gms-signin: timed out after %u s\n", kTimeoutMs / 1000);
                finish(w);
            }
            return 0;
        case WM_ERASEBKGND: {
            RECT c;
            GetClientRect(hwnd, &c);
            FillRect(reinterpret_cast<HDC>(wp), &c, GetSysColorBrush(COLOR_WINDOW));
            return 1;
        }
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wp);
            SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
            SetBkColor(dc, GetSysColor(COLOR_WINDOW));
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
        }
        case WM_SETFOCUS:
            if (w && w->controller) w->controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
            return 0;
        case WM_CLOSE:
            // The user closed the window: a cancel, which resolves the wait.
            if (w) w->finished = true;
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            if (w) {
                KillTimer(hwnd, kPollTimer);
                KillTimer(hwnd, kTimeoutTimer);
                if (w->controller) w->controller->Close();
                w->view.reset();
                w->cookies.reset();
                w->controller.reset();
                g_window = nullptr;
                SetEvent(w->done);
            }
            return 0;
        case WM_NCDESTROY:
            if (w) {
                if (w->font) DeleteObject(w->font);
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            }
            break;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void register_class() {
    static bool done = false;
    if (done) return;
    done = true;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
    wc.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));      // IDI_APPLICATION
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
}

// The app window's icons for the title bar and taskbar, when there is an app
// (a bounded ask: that window's thread may be busy in Java).
void copy_app_icon(HWND hwnd) {
    HWND app = static_cast<HWND>(window_handle());
    if (!app) return;
    for (WPARAM which : {static_cast<WPARAM>(ICON_BIG), static_cast<WPARAM>(ICON_SMALL)}) {
        DWORD_PTR icon = 0;
        if (SendMessageTimeoutW(app, WM_GETICON, which, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 300, &icon) && icon) {
            SendMessageW(hwnd, WM_SETICON, which, static_cast<LPARAM>(icon));
        }
    }
}

void open_on_thread(Window* w) {
    register_class();
    g_window = w;
    // Portrait, the flow is phone-shaped; centred on the app's window when
    // there is one, else on the primary monitor.
    const UINT dpi = dpi_of(nullptr);
    const int width = MulDiv(460, static_cast<int>(dpi), 96), height = MulDiv(720, static_cast<int>(dpi), 96);
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    RECT r{};
    HWND app = static_cast<HWND>(window_handle());
    if (app && !IsIconic(app) && GetWindowRect(app, &r) && r.right - r.left > 100) {
        x = r.left + (r.right - r.left - width) / 2;
        y = r.top + (r.bottom - r.top - height) / 2;
    } else {
        RECT work{};
        if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
            x = work.left + (work.right - work.left - width) / 2;
            y = work.top + (work.bottom - work.top - height) / 2;
        }
    }
    HWND hwnd = CreateWindowExW(0, kClass, L"Sign in to Google", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, width,
                                height, nullptr, nullptr, GetModuleHandleW(nullptr), w);
    if (!hwnd) {
        std::fprintf(stderr, "[win.apk] gms-signin: no window (error %lu)\n", GetLastError());
        g_window = nullptr;
        SetEvent(w->done);
        return;
    }
    copy_app_icon(hwnd);
    // The user asked for this (an account chooser's button, a Settings
    // button, a licence check they are watching): in front, and focused, so
    // they can type into it.
    ShowWindow(hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd);
    wv2::with_environment(w->folder, [](ICoreWebView2Environment* e) { create_controller(e); });
}

// The WebView2 user-data folder: the app's own (HostBrowserTab's
// app_webview2 in its data directory, the one environment this process has)
// when an app runs, else a folder under the GMS state directory, for a
// `--gms signin-window` run with no app. The in-private profile writes nothing
// of the session there either way.
std::wstring user_data_folder(JNIEnv* env) {
    jclass tab = env->FindClass("com/macapk/loader/HostBrowserTab");
    if (env->ExceptionCheck()) env->ExceptionClear();
    jmethodID m = tab ? env->GetStaticMethodID(tab, "macapkUserDataFolderPath", "()Ljava/lang/String;") : nullptr;
    if (env->ExceptionCheck()) env->ExceptionClear();
    std::wstring folder;
    if (m) {
        auto s = static_cast<jstring>(env->CallStaticObjectMethod(tab, m));
        if (env->ExceptionCheck()) env->ExceptionClear();
        folder = wv2::from_jstring(env, s);
        if (s) env->DeleteLocalRef(s);
    }
    if (tab) env->DeleteLocalRef(tab);
    if (!folder.empty()) return folder;
    wchar_t buf[32768];
    const DWORD n = GetEnvironmentVariableW(L"MACAPK_GMS_DIR", buf, 32768);
    if (n > 0 && n < 32768) return std::wstring(buf, n) + L"\\webview2";
    const DWORD m2 = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, 32768);
    if (m2 > 0 && m2 < 32768) return std::wstring(buf, m2) + L"\\Win.apk\\gms\\webview2";
    return std::wstring();
}

// ---- JNI ----------------------------------------------------------------------

jstring JNICALL SignIn_present(JNIEnv* env, jclass) {
    if (!wv2::available()) {
        std::fprintf(stderr, "[win.apk] gms-signin: no WebView2, so no sign-in window (cancelled)\n");
        std::fflush(stderr);
        return nullptr;
    }
    // One window at a time: a second licence check arriving while the user is
    // typing must not stack a second window on the first.
    bool expected = false;
    if (!g_active.compare_exchange_strong(expected, true)) {
        std::fprintf(stderr, "[win.apk] gms-signin: a sign-in window is already open\n");
        return nullptr;
    }
    JavaVM* vm = nullptr;
    env->GetJavaVM(&vm);
    wv2::set_vm(vm);
    auto* w = new Window();
    w->folder = user_data_folder(env);
    w->done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::wstring token;
    if (w->done && !w->folder.empty() && wv2::post([w] { open_on_thread(w); })) {
        WaitForSingleObject(w->done, INFINITE);
        token = w->token;
    }
    if (w->done) CloseHandle(w->done);
    delete w;
    g_active.store(false);
    if (token.empty()) {
        std::fprintf(stderr, "[win.apk] gms-signin: no token (cancelled or failed)\n");
        std::fflush(stderr);
        return nullptr;  // never an empty string: the Java side reads "" as a token
    }
    return wv2::to_jstring(env, token);
}

jboolean JNICALL SignIn_active(JNIEnv*, jclass) { return g_active.load() ? JNI_TRUE : JNI_FALSE; }

constexpr const char* kCls = "com.macapk.gms.NativeSignIn";

const HostMethod kMethods[] = {
    WINAPK_HOST_NATIVE(kCls, "presentSignInWindow", "()Ljava/lang/String;", SignIn_present),
    WINAPK_HOST_NATIVE(kCls, "signInWindowActive", "()Z", SignIn_active),
};

}  // namespace

HostMethods native_signin_methods() { return {kMethods, sizeof(kMethods) / sizeof(kMethods[0])}; }

}  // namespace winapk::host
