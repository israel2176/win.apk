// launcher_main.cpp -- Win.apk.exe, the app library.
//
// The Windows counterpart of upstream's launcher (launcher/*.swift), laid out
// the way upstream's SwiftUI window is: a centred title, a drop zone that
// installs whatever is dropped on (or chosen through) it, and a searchable
// grid of the installed apps with their icons. A gear opens the settings
// sheet, right-click > Get Info opens an app's sheet. The window is drawn
// with GDI+ and follows Windows' light or dark mode (or the one picked in
// the settings), in English or, laid out right to left, in Hebrew.
//
// An APK dropped on the window, chosen in the drop zone, double-clicked in
// Explorer (the launcher registers itself for .apk files) or passed on the
// command line is installed, one at a time:
//
//   1. copied into %LOCALAPPDATA%\Win.apk\library, so the app stays when the
//      download is deleted;
//   2. installed and converted once by winapk-runtime --install-only, which
//      also writes the app's icon and name (apps\<name>\icon.png, label.txt;
//      the name moves next to the APK as library\<name>.label, because the
//      app's first run moves files it does not know out of apps\<name>);
//   3. given a Start menu shortcut (Start > Win.apk Apps) and, unless turned
//      off in the settings, a desktop shortcut, with the app's own icon, that
//      start it through this program.
//
// Double-clicking an app runs it (winapk-runtime --apk, without a console);
// if it stops with an error the log can be opened. The screen size and
// orientation picked in the settings reach the runtime as WINAPK_PHONE_SIZE.
//
//   Win.apk.exe                      the library
//   Win.apk.exe <app.apk>            install, then run it
//   Win.apk.exe --run <library apk>  run an installed app (what shortcuts do)

#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

using Gdiplus::Color;
using Gdiplus::Graphics;
using Gdiplus::GraphicsPath;
using Gdiplus::RectF;
using Gdiplus::SizeF;

constexpr UINT kInstalled = WM_APP + 1;       // wParam: ok | run after << 1, lParam: new std::wstring (the name)
constexpr UINT kRunExited = WM_APP + 2;       // wParam: ok, lParam: new std::wstring (the app's name)
constexpr UINT kInstallStarted = WM_APP + 3;  // lParam: new std::wstring (the app's name)
constexpr UINT_PTR kAnimTimer = 1;
constexpr int kIdSearch = 100;
enum Cmd { kCmdOpen = 200, kCmdInfo, kCmdShortcut, kCmdShowApk, kCmdAppFolder, kCmdLog, kCmdReinstall, kCmdUpdate,
           kCmdRemove };

HINSTANCE g_inst;
HWND g_main, g_search;
HFONT g_edit_font, g_mono_font;
HBRUSH g_edit_brush, g_code_brush;
float g_dpi = 96;

float px(float v) { return v * g_dpi / 96.0f; }
int ipx(float v) { return static_cast<int>(std::lround(px(v))); }

// ---------------------------------------------------------------------------
// Language: English, or Hebrew laid out right to left

bool g_rtl = false;    // Hebrew: every window is mirrored (WS_EX_LAYOUTRTL) and its text reads right to left
float g_paint_w = 0;   // the width being painted, to put text and pictures back in a mirrored layout

DWORD setting(const wchar_t* name, DWORD fallback);

void pick_language() {
    const DWORD pref = setting(L"Language", 0);  // 0 as Windows, 1 English, 2 Hebrew
    g_rtl = pref == 2 || (pref == 0 && PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_HEBREW);
}

// The Hebrew for an English string, keyed by the English itself.
const wchar_t* tr(const wchar_t* en) {
    static const std::map<std::wstring, const wchar_t*> he = {
        {L"Run Android apps natively on Windows", L"הרצת אפליקציות Android ישירות ב-Windows"},
        {L"Drop to install", L"שחרר כדי להתקין"},
        {L"Win.apk keeps its own copy, so the download can go", L"Win.apk שומרת עותק משלה, כך שאפשר למחוק את ההורדה"},
        {L"Installing…", L"מתקין…"},
        {L"Installing {0} of {1}…", L"מתקין {0} מתוך {1}…"},
        {L"Installing {0} of {1}", L"מתקין {0} מתוך {1}"},
        {L"Installing", L"מתקין"},
        {L"Unpacking manifest, icon, and assets", L"פורס את המניפסט, הסמל והמשאבים"},
        {L"{0}  ·  the first install of an app takes up to a minute", L"{0}  ·  ההתקנה הראשונה של אפליקציה נמשכת עד דקה"},
        {L"Drop an APK here to install", L"גרור לכאן קובץ APK כדי להתקין"},
        {L"or click to choose a file  ·  .apk .apkm .xapk .apks", L"או לחץ כדי לבחור קובץ  ·  apk, apkm, xapk, apks"},
        {L"Installed apps", L"אפליקציות מותקנות"},
        {L"Add app", L"הוספת אפליקציה"},
        {L"No apps match “{0}”", L"אין אפליקציות שתואמות ל„{0}”"},
        {L"Press Esc in the search box to show every app.", L"הקש Esc בתיבת החיפוש כדי להציג את כל האפליקציות."},
        {L"No apps installed yet.", L"עדיין לא הותקנו אפליקציות."},
        {L"Drop an APK above (or click it to choose one) to install one, then open it from here.", L"גרור קובץ APK לאזור שלמעלה (או לחץ עליו כדי לבחור קובץ) כדי להתקין, ואז פתח את האפליקציה מכאן."},
        {L"Logs", L"לוגים"},
        {L"Stop", L"עצירה"},
        {L"Drop an APK above to install your first app", L"גרור קובץ APK לאזור שלמעלה כדי להתקין את האפליקציה הראשונה"},
        {L"Double-click to open  ·  right-click for options  ·  drop an APK above to add more", L"לחיצה כפולה לפתיחה  ·  לחיצה ימנית לאפשרויות  ·  גרירת APK למעלה להוספה"},
        {L"{0} apps are running", L"{0} אפליקציות פועלות"},
        {L"{0} is running", L"{0} פועלת"},
        {L"{0} app", L"אפליקציה אחת"},
        {L"{0} apps", L"{0} אפליקציות"},
        {L"The log may say why:\n{0}", L"ייתכן שהלוג מסביר למה:\n{0}"},
        {L"Open the log", L"פתיחת הלוג"},
        {L"Android apps", L"אפליקציות Android"},
        {L"All files", L"כל הקבצים"},
        {L"Install an Android app", L"התקנת אפליקציית Android"},
        {L"Installing {0}…", L"מתקין את {0}…"},
        {L"{0} is still being installed.", L"{0} עדיין בהתקנה."},
        {L"{0} is already open.", L"{0} כבר פתוחה."},
        {L"Opening {0}…", L"פותח את {0}…"},
        {L"Remove {0}?", L"להסיר את {0}?"},
        {L"Its shortcuts go too. What the app saved stays, so installing it again picks up where it left off.", L"גם קיצורי הדרך שלה יוסרו. מה שהאפליקציה שמרה נשאר, כך שהתקנה מחדש תמשיך מאותה נקודה."},
        {L"Remove", L"הסרה"},
        {L"{0} was removed.", L"{0} הוסרה."},
        {L"Update {0} from an APK", L"עדכון {0} מקובץ APK"},
        {L"Done", L"סיום"},
        {L"Theme", L"ערכת צבעים"},
        {L"Match Windows", L"כמו ב-Windows"},
        {L"Light", L"בהירה"},
        {L"Dark", L"כהה"},
        {L"Appearance", L"מראה"},
        {L"Language", L"שפה"},
        {L"Open Android app files with Win.apk", L"פתיחת קובצי אפליקציות Android עם Win.apk"},
        {L"Default", L"ברירת מחדל"},
        {L"Make default", L"הגדרה כברירת מחדל"},
        {L"File types", L"סוגי קבצים"},
        {L"Covers .apk, .apkm, .xapk and .apks. Double-clicking one in Explorer installs it and opens it.", L"כולל קובצי apk, apkm, xapk ו-apks. לחיצה כפולה על קובץ כזה בסייר הקבצים מתקינה אותו ופותחת אותו."},
        {L"Screen size", L"גודל מסך"},
        {L"Auto (fits the screen)", L"אוטומטי (מותאם למסך)"},
        {L"320 × 480  (small, older apps)", L"320 × 480  (קטן, לאפליקציות ישנות)"},
        {L"360 × 640  (5\" phone)", L"360 × 640  (טלפון 5 אינץ')"},
        {L"360 × 780  (6.5\" phone)", L"360 × 780  (טלפון 6.5 אינץ')"},
        {L"480 × 800  (6\" HDPI)", L"480 × 800  (‏6 אינץ' HDPI)"},
        {L"540 × 960  (large phone)", L"540 × 960  (טלפון גדול)"},
        {L"600 × 960  (7\" tablet)", L"600 × 960  (טאבלט 7 אינץ')"},
        {L"800 × 1280  (10\" tablet)", L"800 × 1280  (טאבלט 10 אינץ')"},
        {L"Orientation", L"כיוון"},
        {L"Portrait", L"לאורך"},
        {L"Landscape", L"לרוחב"},
        {L"Screen", L"מסך"},
        {L"Used the next time an app opens, from here or from its shortcut. Sizes are in dp, as on a phone.", L"יחול בפעם הבאה שאפליקציה תיפתח, מכאן או מקיצור הדרך שלה. הגדלים ביחידות dp, כמו בטלפון."},
        {L"Desktop shortcut for new apps", L"קיצור דרך בשולחן העבודה לאפליקציות חדשות"},
        {L"Every app also gets one in Start > Win.apk Apps.", L"כל אפליקציה מקבלת גם קיצור דרך בתפריט התחל > Win.apk Apps."},
        {L"Shortcuts", L"קיצורי דרך"},
        {L"Make the Start menu shortcuts of every app again, with their icons.", L"יצירה מחדש של קיצורי הדרך בתפריט התחל לכל האפליקציות, עם הסמלים שלהן."},
        {L"Recreate", L"יצירה מחדש"},
        {L"The Start menu shortcuts were made again.", L"קיצורי הדרך בתפריט התחל נוצרו מחדש."},
        {L"Diagnostics", L"אבחון"},
        {L"install-<app>.log records each install, last-run.log the app opened last.", L"הקובץ install-<app>.log מתעד כל התקנה, והקובץ last-run.log את האפליקציה שנפתחה אחרונה."},
        {L"View logs", L"הצגת הלוגים"},
        {L"Logs folder", L"תיקיית הלוגים"},
        {L"Open folder", L"פתיחת התיקייה"},
        {L"Attach the log to a bug report: it says what the runtime did and where it stopped.", L"צרף את הלוג לדיווח על באג: הוא מראה מה סביבת ההרצה עשתה והיכן נעצרה."},
        {L"Saves, caches and files", L"שמירות, מטמון וקבצים"},
        {L"Each installed app keeps its data here. Removing an app keeps what it saved.", L"כל אפליקציה מותקנת שומרת כאן את הנתונים שלה. הסרת אפליקציה משאירה את מה שהיא שמרה."},
        {L"App data", L"נתוני אפליקציות"},
        {L"Version {0}", L"גרסה {0}"},
        {L"An unofficial port of Mac.apk to Windows. Not affiliated with or endorsed by the Mac.apk project.", L"הסבה לא רשמית של Mac.apk ל-Windows. אינה קשורה לפרויקט Mac.apk ואינה מאושרת על ידו."},
        {L"About", L"אודות"},
        {L"Remove all apps", L"הסרת כל האפליקציות"},
        {L"Their shortcuts go too. What they saved stays.", L"גם קיצורי הדרך שלהן יוסרו. מה שהן שמרו נשאר."},
        {L"Remove all…", L"הסרת הכול…"},
        {L"Remove all", L"הסרת הכול"},
        {L"Remove all {0} apps?", L"להסיר את כל {0} האפליקציות?"},
        {L"Every app was removed.", L"כל האפליקציות הוסרו."},
        {L"Settings", L"הגדרות"},
        {L"{0}  ·  Android app", L"{0}  ·  אפליקציית Android"},
        {L"Identity", L"זיהוי"},
        {L"Name", L"שם"},
        {L"Library name", L"שם בספרייה"},
        {L"Installed", L"הותקנה"},
        {L"Storage", L"אחסון"},
        {L"Source APK  ·  {0}", L"קובץ ה-APK המקורי  ·  {0}"},
        {L"App files  ·  {0}", L"קבצי האפליקציה  ·  {0}"},
        {L"Show", L"הצגה"},
        {L"Open", L"פתיחה"},
        {L"Install log", L"לוג ההתקנה"},
        {L"View", L"הצגה"},
        {L"Open {0}", L"פתיחת {0}"},
        {L"Desktop shortcut", L"קיצור דרך בשולחן העבודה"},
        {L"Put a shortcut to the app on the desktop.", L"הוספת קיצור דרך לאפליקציה בשולחן העבודה."},
        {L"Create", L"יצירה"},
        {L"A shortcut to {0} is on the desktop.", L"קיצור דרך ל-{0} נמצא בשולחן העבודה."},
        {L"Update from APK", L"עדכון מקובץ APK"},
        {L"Install a newer version over this one. Saves stay.", L"התקנת גרסה חדשה יותר במקום הנוכחית. השמירות נשארות."},
        {L"Choose…", L"בחירה…"},
        {L"Reinstall", L"התקנה מחדש"},
        {L"Convert the app again from its APK.", L"המרת האפליקציה מחדש מקובץ ה-APK שלה."},
        {L"Its shortcuts go too. What the app saved stays.", L"גם קיצורי הדרך שלה יוסרו. מה שהאפליקציה שמרה נשאר."},
        {L"Remove…", L"הסרה…"},
        {L"Actions", L"פעולות"},
        {L"Each app is converted for Windows once. That takes up to a minute.", L"כל אפליקציה מומרת ל-Windows פעם אחת. זה נמשך עד דקה."},
        {L"{0} is ready", L"{0} מוכנה"},
        {L"All set", L"הכול מוכן"},
        {L"{0} app is in the library and in Start > Win.apk Apps.", L"האפליקציה נמצאת בספרייה ובתפריט התחל > Win.apk Apps."},
        {L"{0} apps are in the library and in Start > Win.apk Apps.", L"{0} אפליקציות נמצאות בספרייה ובתפריט התחל > Win.apk Apps."},
        {L"The install failed", L"ההתקנה נכשלה"},
        {L"Some installs failed", L"חלק מההתקנות נכשלו"},
        {L"{0} of {1} could not be installed. The log says why.", L"{0} מתוך {1} לא הותקנו. הלוג מסביר למה."},
        {L"Waiting", L"ממתינה"},
        {L"Converting  ·  {0}", L"ממירה  ·  {0}"},
        {L"Installed in {0}", L"הותקנה תוך {0}"},
        {L"Failed after {0}", L"נכשלה אחרי {0}"},
        {L"View log", L"הצגת הלוג"},
        {L"Log  ·  {0}", L"לוג  ·  {0}"},
        {L"Log", L"לוג"},
        {L"Starting the runtime…", L"מפעיל את סביבת ההרצה…"},
        {L"The install's output appears here.", L"הפלט של ההתקנה יופיע כאן."},
        {L"Hide", L"הסתרה"},
        {L"Open full log", L"פתיחת הלוג המלא"},
        {L"Last run", L"הרצה אחרונה"},
        {L"Install  ·  {0}", L"התקנה  ·  {0}"},
        {L"No app has run yet. This log is written when one does.", L"עדיין לא הורצה אף אפליקציה. הלוג ייכתב כשאחת תורץ."},
        {L"This log is written the next time the app is installed.", L"הלוג ייכתב בפעם הבאה שהאפליקציה תותקן."},
        {L"Live", L"חי"},
        {L"Copy", L"העתקה"},
        {L"Notepad", L"פנקס רשימות"},
        {L"Folder", L"תיקייה"},
        {L"The log was copied.", L"הלוג הועתק."},
        {L"&Open", L"פתיחה"},
        {L"Get &Info…", L"מידע…"},
        {L"Create &desktop shortcut", L"יצירת קיצור דרך בשולחן העבודה"},
        {L"Show the &APK in Explorer", L"הצגת קובץ ה-APK בסייר הקבצים"},
        {L"Open the app's &files", L"פתיחת קבצי האפליקציה"},
        {L"Show the last &log", L"הצגת הלוג האחרון"},
        {L"&Update from APK…", L"עדכון מקובץ APK…"},
        {L"Rein&stall", L"התקנה מחדש"},
        {L"&Remove…", L"הסרה…"},
        {L"Search apps", L"חיפוש אפליקציות"},
        {L"Installing - Win.apk", L"התקנה - Win.apk"},
        {L"Logs - Win.apk", L"לוגים - Win.apk"},
        {L"{0} is installed: it is in the Start menu and on the desktop.", L"{0} הותקנה: היא בתפריט התחל ובשולחן העבודה."},
        {L"{0} is installed: it is in the Start menu.", L"{0} הותקנה: היא בתפריט התחל."},
        {L"Installing {0}… The first install of an app takes up to a minute.", L"מתקין את {0}… ההתקנה הראשונה של אפליקציה נמשכת עד דקה."},
        {L"Installing {0} failed.", L"ההתקנה של {0} נכשלה."},
        {L"Installing {0} failed", L"ההתקנה של {0} נכשלה"},
        {L"{0} was stopped.", L"{0} נעצרה."},
        {L"{0} stopped with an error.", L"{0} נעצרה עם שגיאה."},
        {L"{0} stopped with an error", L"{0} נעצרה עם שגיאה"},
        {L"The app stopped with an error.\n\nThe log is in:\n{0}", L"האפליקציה נעצרה עם שגיאה.\n\nהלוג נמצא ב:\n{0}"},
        {L"bytes", L"בתים"},
        {L"unknown", L"לא ידוע"}
    };
    if (!g_rtl) return en;
    const auto it = he.find(en);
    return it == he.end() ? en : it->second;
}

// tr(en) with {0}, {1}... replaced by `args`.
std::wstring fmt(const wchar_t* en, std::initializer_list<std::wstring> args) {
    std::wstring s = tr(en);
    size_t i = 0;
    for (const std::wstring& a : args) {
        const std::wstring key = L"{" + std::to_wstring(i++) + L"}";
        const size_t at = s.find(key);
        if (at != std::wstring::npos) s.replace(at, key.size(), a);
    }
    return s;
}

std::wstring app_count(size_t n) { return fmt(n == 1 ? L"{0} app" : L"{0} apps", {std::to_wstring(n)}); }
DWORD layout_style() { return g_rtl ? WS_EX_LAYOUTRTL : 0; }
// A popup's flags; `anchored` popups open under a box whose start edge is the point.
UINT menu_flags(bool anchored) {
    return g_rtl ? TPM_LAYOUTRTL | (anchored ? TPM_RIGHTALIGN : 0u) : (anchored ? TPM_LEFTALIGN : 0u);
}

// ---------------------------------------------------------------------------
// Paths

fs::path exe_path() {
    wchar_t buf[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 4);
    return fs::path(std::wstring(buf, n));
}
fs::path root() { return exe_path().parent_path(); }
fs::path runtime_exe() { return root() / L"bin" / L"winapk-runtime.exe"; }

fs::path data_dir() {
    wchar_t buf[MAX_PATH * 4];
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH * 4);
    return fs::path(std::wstring(buf, n)) / L"Win.apk";
}
fs::path library_dir() { return data_dir() / L"library"; }
fs::path app_dir(const std::wstring& name) { return data_dir() / L"apps" / name; }
fs::path logs_dir() { return data_dir() / L"logs"; }
fs::path last_run_log() { return logs_dir() / L"last-run.log"; }
fs::path install_log(const std::wstring& name) { return logs_dir() / (L"install-" + name + L".log"); }

fs::path known_folder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    fs::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) out = p;
    CoTaskMemFree(p);
    return out;
}
fs::path start_menu_apps() { return known_folder(FOLDERID_Programs) / L"Win.apk Apps"; }

const wchar_t* const kExtensions[] = {L".apk", L".apkm", L".xapk", L".apks"};

bool is_app_file(const fs::path& p) {
    std::wstring ext = p.extension().wstring();
    for (auto& c : ext) c = static_cast<wchar_t>(towlower(c));
    return std::any_of(std::begin(kExtensions), std::end(kExtensions), [&](const wchar_t* e) { return ext == e; });
}

std::wstring utf8_to_wide(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

std::wstring file_name_safe(std::wstring s) {
    for (auto& c : s) {
        if (c < 32 || wcschr(L"<>:\"/\\|?*", c)) c = L'_';
    }
    while (!s.empty() && (s.back() == L'.' || s.back() == L' ')) s.pop_back();
    return s.empty() ? L"App" : s;
}

std::wstring lower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(&s[0], static_cast<DWORD>(s.size()));
    return s;
}

std::wstring format_bytes(uintmax_t n) {
    const wchar_t* units[] = {tr(L"bytes"), L"KB", L"MB", L"GB"};
    double v = static_cast<double>(n);
    int u = 0;
    while (v >= 1024 && u < 3) v /= 1024, ++u;
    wchar_t buf[64];
    swprintf(buf, 64, u == 0 ? L"%.0f %ls" : L"%.1f %ls", v, units[u]);
    return buf;
}

uintmax_t folder_size(const fs::path& dir) {
    uintmax_t total = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec)) total += it->file_size(ec);
    }
    return total;
}

std::wstring file_date(const fs::path& p) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    SYSTEMTIME utc, local;
    wchar_t buf[128] = {};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a) ||
        !FileTimeToSystemTime(&a.ftLastWriteTime, &utc) ||
        !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local) ||
        !GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_LONGDATE, &local, nullptr, buf, 128, nullptr)) {
        return tr(L"unknown");
    }
    return buf;
}

// The version in launcher.rc, read from the program's own resource.
std::wstring version() {
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(VS_VERSION_INFO), RT_VERSION);
    HGLOBAL mem = res ? LoadResource(nullptr, res) : nullptr;
    const auto* p = static_cast<const BYTE*>(mem ? LockResource(mem) : nullptr);
    const DWORD size = res ? SizeofResource(nullptr, res) : 0;
    for (DWORD i = 0; p && i + sizeof(VS_FIXEDFILEINFO) <= size; i += 4) {
        const auto* fi = reinterpret_cast<const VS_FIXEDFILEINFO*>(p + i);
        if (fi->dwSignature != 0xFEEF04BD) continue;
        return std::to_wstring(HIWORD(fi->dwFileVersionMS)) + L"." + std::to_wstring(LOWORD(fi->dwFileVersionMS)) +
               L"." + std::to_wstring(HIWORD(fi->dwFileVersionLS));
    }
    return L"0";
}

void open_path(const fs::path& p) {
    std::error_code ec;
    fs::create_directories(p, ec);
    ShellExecuteW(nullptr, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void show_in_explorer(const fs::path& p) {
    const std::wstring args = L"/select,\"" + p.wstring() + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

void open_log(const fs::path& log) {
    ShellExecuteW(nullptr, L"open", L"notepad.exe", log.c_str(), nullptr, SW_SHOWNORMAL);
}

// ---------------------------------------------------------------------------
// Settings, in HKCU\Software\Win.apk

constexpr wchar_t kSettingsKey[] = L"Software\\Win.apk";

DWORD setting(const wchar_t* name, DWORD fallback) {
    DWORD v = 0, size = sizeof(v);
    return RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, name, RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS
               ? v
               : fallback;
}

void set_setting(const wchar_t* name, DWORD v) {
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, name, REG_DWORD, &v, sizeof(v));
}

struct PhonePreset {
    const wchar_t* label;
    int w, h;  // dp; 0 x 0 is the runtime's own choice
};

const PhonePreset kPhonePresets[] = {
    {L"Auto (fits the screen)", 0, 0},
    {L"320 × 480  (small, older apps)", 320, 480},
    {L"360 × 640  (5\" phone)", 360, 640},
    {L"360 × 780  (6.5\" phone)", 360, 780},
    {L"393 × 851  (Pixel 7)", 393, 851},
    {L"412 × 915  (Pixel 6)", 412, 915},
    {L"480 × 800  (6\" HDPI)", 480, 800},
    {L"540 × 960  (large phone)", 540, 960},
    {L"600 × 960  (7\" tablet)", 600, 960},
    {L"800 × 1280  (10\" tablet)", 800, 1280},
};

// The settings' screen size and orientation, for the runtime started from
// here (it inherits this process's environment). "Auto" leaves whatever
// WINAPK_PHONE_SIZE the user set themselves.
void apply_runtime_env() {
    const DWORD i = setting(L"PhoneSize", 0);
    const bool landscape = setting(L"Landscape", 0) != 0;
    if (i == 0 || i >= std::size(kPhonePresets)) {
        if (landscape) SetEnvironmentVariableW(L"WINAPK_PHONE_SIZE", L"800x360");
        return;
    }
    int w = kPhonePresets[i].w, h = kPhonePresets[i].h;
    if (landscape) std::swap(w, h);
    SetEnvironmentVariableW(L"WINAPK_PHONE_SIZE", (std::to_wstring(w) + L"x" + std::to_wstring(h)).c_str());
}

// ---------------------------------------------------------------------------
// Windows integration: file types and shortcuts

void set_reg_default(const std::wstring& sub, const std::wstring& value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, sub.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) ==
        ERROR_SUCCESS) {
        RegSetValueExW(key, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                       static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(key);
    }
}

std::wstring reg_default(const std::wstring& sub) {
    wchar_t buf[256] = {};
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, sub.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) {
        return L"";
    }
    return buf;
}

bool is_default_handler() { return reg_default(L"Software\\Classes\\.apk") == L"WinApk.Package"; }

// Claims the app file types; without force, only those no other program has.
void register_file_types(bool force) {
    const std::wstring exe = exe_path().wstring();
    set_reg_default(L"Software\\Classes\\WinApk.Package", L"Android app");
    set_reg_default(L"Software\\Classes\\WinApk.Package\\DefaultIcon", L"\"" + exe + L"\",0");
    set_reg_default(L"Software\\Classes\\WinApk.Package\\shell\\open\\command", L"\"" + exe + L"\" \"%1\"");
    for (const wchar_t* ext : kExtensions) {
        const std::wstring key = std::wstring(L"Software\\Classes\\") + ext;
        const std::wstring current = reg_default(key);
        if (force || current.empty() || current == L"WinApk.Package") set_reg_default(key, L"WinApk.Package");
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

bool make_shortcut(const fs::path& lnk, const std::wstring& args, const fs::path& icon, const std::wstring& what) {
    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) return false;
    link->SetPath(exe_path().c_str());
    link->SetArguments(args.c_str());
    link->SetWorkingDirectory(root().c_str());
    link->SetDescription(what.c_str());
    if (!icon.empty()) link->SetIconLocation(icon.c_str(), 0);
    IPersistFile* file = nullptr;
    bool ok = SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file)));
    if (ok) {
        std::error_code ec;
        fs::create_directories(lnk.parent_path(), ec);
        ok = SUCCEEDED(file->Save(lnk.c_str(), TRUE));
        file->Release();
    }
    link->Release();
    return ok;
}

void integrate_with_windows() {
    const fs::path self = known_folder(FOLDERID_Programs) / L"Win.apk.lnk";
    std::error_code ec;
    if (!fs::exists(self, ec)) make_shortcut(self, L"", fs::path(), L"Android apps on Windows");
    register_file_types(false);
}

// ---------------------------------------------------------------------------
// The library

struct App {
    std::wstring name;   // the library file's stem: the key everywhere
    std::wstring label;  // what the user sees
    fs::path apk;
};

fs::path label_file(const std::wstring& name) { return library_dir() / (name + L".label"); }
fs::path shortcut_icon(const std::wstring& name) { return library_dir() / (name + L".ico"); }

std::wstring label_of(const std::wstring& name) {
    std::ifstream f(label_file(name), std::ios::binary);
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const std::wstring w = utf8_to_wide(s);
    return w.empty() ? name : w;
}

std::vector<App> library() {
    std::vector<App> apps;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(library_dir(), ec)) {
        if (!e.is_regular_file(ec) || !is_app_file(e.path())) continue;
        const std::wstring name = e.path().stem().wstring();
        if (!fs::exists(label_file(name), ec)) continue;  // not installed (yet)
        apps.push_back({name, label_of(name), e.path()});
    }
    std::sort(apps.begin(), apps.end(), [](const App& a, const App& b) {
        return CompareStringEx(LOCALE_NAME_USER_DEFAULT, NORM_IGNORECASE, a.label.c_str(), -1, b.label.c_str(), -1,
                               nullptr, nullptr, 0) == CSTR_LESS_THAN;
    });
    return apps;
}

bool write_ico(const fs::path& png, const fs::path& ico) {
    std::ifstream f(png, std::ios::binary);
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (data.size() < 24 || data[1] != 'P') return false;
    const uint32_t w = (data[16] << 24) | (data[17] << 16) | (data[18] << 8) | data[19];
    const uint32_t h = (data[20] << 24) | (data[21] << 16) | (data[22] << 8) | data[23];
    std::ofstream o(ico, std::ios::binary | std::ios::trunc);
    auto u16 = [&](uint16_t v) { o.put(static_cast<char>(v & 0xff)).put(static_cast<char>(v >> 8)); };
    auto u32 = [&](uint32_t v) { u16(static_cast<uint16_t>(v & 0xffff)); u16(static_cast<uint16_t>(v >> 16)); };
    u16(0), u16(1), u16(1);
    o.put(static_cast<char>(w >= 256 ? 0 : w)).put(static_cast<char>(h >= 256 ? 0 : h)).put(0).put(0);
    u16(1), u16(32), u32(static_cast<uint32_t>(data.size())), u32(22);
    o.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(o);
}

std::wstring run_args(const fs::path& apk) { return L"--run \"" + apk.wstring() + L"\""; }

void make_app_shortcuts(const App& app, bool desktop) {
    const fs::path ico = shortcut_icon(app.name);
    const bool has_icon = write_ico(app_dir(app.name) / L"icon.png", ico);
    const std::wstring file = file_name_safe(app.label) + L".lnk";
    const std::wstring what = app.label + L" (Android app, Win.apk)";
    make_shortcut(start_menu_apps() / file, run_args(app.apk), has_icon ? ico : fs::path(), what);
    if (desktop) {
        make_shortcut(known_folder(FOLDERID_Desktop) / file, run_args(app.apk), has_icon ? ico : fs::path(), what);
    }
}

void remove_app_shortcuts(const App& app) {
    std::error_code ec;
    const std::wstring file = file_name_safe(app.label) + L".lnk";
    fs::remove(start_menu_apps() / file, ec);
    fs::remove(known_folder(FOLDERID_Desktop) / file, ec);
}

// ---------------------------------------------------------------------------
// Running and installing

HANDLE start_runtime(const std::wstring& args, const fs::path& log) {
    std::error_code ec;
    fs::create_directories(log.parent_path(), ec);
    apply_runtime_env();
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE out = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, &sa, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = out;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    std::wstring cmd = L"\"" + runtime_exe().wstring() + L"\" " + args;
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                   root().c_str(), &si, &pi);
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    if (!ok) return nullptr;
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

DWORD wait_exit(HANDLE process) {
    WaitForSingleObject(process, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(process, &code);
    CloseHandle(process);
    return code;
}

bool run_and_wait(const fs::path& apk) {
    HANDLE p = start_runtime(L"--apk \"" + apk.wstring() + L"\"", last_run_log());
    return p && wait_exit(p) == 0;
}

struct RunJob {
    std::wstring name;
    fs::path apk;
};

SRWLOCK g_proc_lock = SRWLOCK_INIT;
std::map<std::wstring, HANDLE> g_procs;  // the runtimes of apps started from the library, for Stop

DWORD WINAPI run_thread(void* param) {
    std::unique_ptr<RunJob> job(static_cast<RunJob*>(param));
    HANDLE p = start_runtime(L"--apk \"" + job->apk.wstring() + L"\"", last_run_log());
    DWORD code = 1;
    if (p) {
        AcquireSRWLockExclusive(&g_proc_lock);
        g_procs[job->name] = p;
        ReleaseSRWLockExclusive(&g_proc_lock);
        WaitForSingleObject(p, INFINITE);
        GetExitCodeProcess(p, &code);
        AcquireSRWLockExclusive(&g_proc_lock);
        g_procs.erase(job->name);
        ReleaseSRWLockExclusive(&g_proc_lock);
        CloseHandle(p);
    }
    PostMessageW(g_main, kRunExited, code == 0, reinterpret_cast<LPARAM>(new std::wstring(job->name)));
    return 0;
}

bool stop_process(const std::wstring& name) {
    AcquireSRWLockExclusive(&g_proc_lock);
    const auto it = g_procs.find(name);
    const bool found = it != g_procs.end() && TerminateProcess(it->second, 1);
    ReleaseSRWLockExclusive(&g_proc_lock);
    return found;
}

struct InstallJob {
    fs::path source;
    std::wstring name;
    fs::path replaces;  // the app's previous APK, when it is updated from one with another extension
    bool run_after;
};

SRWLOCK g_install_lock = SRWLOCK_INIT;  // one install at a time: each one is a JVM converting the whole app

DWORD WINAPI install_thread(void* param) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::unique_ptr<InstallJob> job(static_cast<InstallJob*>(param));
    AcquireSRWLockExclusive(&g_install_lock);
    PostMessageW(g_main, kInstallStarted, 0, reinterpret_cast<LPARAM>(new std::wstring(job->name)));
    std::error_code ec;
    fs::create_directories(library_dir(), ec);
    const fs::path target = library_dir() / (job->name + job->source.extension().wstring());
    bool ok = fs::equivalent(job->source, target, ec) ||
              CopyFileW(job->source.c_str(), target.c_str(), FALSE) != 0;
    if (ok) {
        HANDLE p = start_runtime(L"--apk \"" + target.wstring() + L"\" --install-only", install_log(job->name));
        ok = p && wait_exit(p) == 0 &&
             MoveFileExW((app_dir(job->name) / L"label.txt").c_str(), label_file(job->name).c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED) != 0;
    }
    if (ok && !job->replaces.empty() && job->replaces != target) fs::remove(job->replaces, ec);
    if (ok) make_app_shortcuts({job->name, label_of(job->name), target}, setting(L"DesktopShortcut", 1) != 0);
    ReleaseSRWLockExclusive(&g_install_lock);
    PostMessageW(g_main, kInstalled, (ok ? 1 : 0) | (job->run_after ? 2 : 0),
                 reinterpret_cast<LPARAM>(new std::wstring(job->name)));
    CoUninitialize();
    return 0;
}

// ---------------------------------------------------------------------------
// Theme and drawing

struct Theme {
    bool dark;
    Color bg, card, hover, border, text, text2, accent, danger, success;
};

Color hex(uint32_t v, BYTE a = 255) { return Color(a, (v >> 16) & 255, (v >> 8) & 255, v & 255); }
Color with_alpha(const Color& c, BYTE a) { return Color(a, c.GetR(), c.GetG(), c.GetB()); }
COLORREF colorref(const Color& c) { return RGB(c.GetR(), c.GetG(), c.GetB()); }
Color mix(const Color& a, const Color& b, float t) {
    auto m = [t](BYTE x, BYTE y) { return static_cast<BYTE>(x + (y - x) * t); };
    return Color(m(a.GetA(), b.GetA()), m(a.GetR(), b.GetR()), m(a.GetG(), b.GetG()), m(a.GetB(), b.GetB()));
}
const Color kWhite(255, 255, 255, 255), kBlack(255, 0, 0, 0);

Theme g_t;

bool windows_uses_dark() {
    DWORD v = 1, size = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &size);
    return v == 0;
}

void pick_theme() {
    const DWORD pref = setting(L"Theme", 0);  // 0 as Windows, 1 light, 2 dark
    const bool dark = pref == 2 || (pref == 0 && windows_uses_dark());
    g_t = dark ? Theme{true, hex(0x1C1C21), hex(0x28282F), hex(0x33333B), hex(0x3B3B44), hex(0xF2F2F7),
                       hex(0x9D9DA6), hex(0x8E80FF), hex(0xFF6B6B), hex(0x6CCB5F)}
               : Theme{false, hex(0xF4F4F7), hex(0xFFFFFF), hex(0xE9E9EF), hex(0xDCDCE3), hex(0x1C1C1E),
                       hex(0x6E6E76), hex(0x6C5CE7), hex(0xC42B1C), hex(0x0F7B0F)};
}

// Dark title bar and, on Windows 11, a caption the colour of the window.
void theme_window(HWND hwnd) {
    const BOOL dark = g_t.dark;
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    const COLORREF caption = colorref(g_t.bg);
    DwmSetWindowAttribute(hwnd, 35 /* DWMWA_CAPTION_COLOR */, &caption, sizeof(caption));
}

// Popup menus follow the theme through uxtheme's unnamed SetPreferredAppMode
// and FlushMenuThemes (ordinals 135 and 136, Windows 10 1903 and later).
void theme_menus() {
    static HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux) return;
    using SetMode = int(WINAPI*)(int);
    using Flush = void(WINAPI*)();
    auto set_mode = reinterpret_cast<SetMode>(reinterpret_cast<void*>(GetProcAddress(ux, MAKEINTRESOURCEA(135))));
    auto flush = reinterpret_cast<Flush>(reinterpret_cast<void*>(GetProcAddress(ux, MAKEINTRESOURCEA(136))));
    if (set_mode) set_mode(g_t.dark ? 2 : 3);  // force dark, force light
    if (flush) flush();
}

struct Fonts {
    std::unique_ptr<Gdiplus::Font> title, tagline, body, body_semi, caption, caption_semi, section, tile, hero,
        monogram, monogram_small, glyph12, glyph16, glyph34, glyph48, mono;
} g_f;

// The first of `families` that is installed, else the generic one.
std::unique_ptr<Gdiplus::Font> font_from(std::initializer_list<const wchar_t*> families,
                                         const Gdiplus::FontFamily* generic, float size, int style) {
    for (const wchar_t* name : families) {
        Gdiplus::FontFamily family(name);
        if (family.GetLastStatus() == Gdiplus::Ok && family.IsAvailable()) {
            return std::make_unique<Gdiplus::Font>(&family, px(size), style, Gdiplus::UnitPixel);
        }
    }
    return std::make_unique<Gdiplus::Font>(generic, px(size), style, Gdiplus::UnitPixel);
}

std::unique_ptr<Gdiplus::Font> font(float size, bool semibold = false) {
    if (semibold) {
        Gdiplus::FontFamily family(L"Segoe UI Semibold");
        if (family.GetLastStatus() == Gdiplus::Ok && family.IsAvailable()) {
            return std::make_unique<Gdiplus::Font>(&family, px(size), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        }
    }
    return font_from({L"Segoe UI"}, Gdiplus::FontFamily::GenericSansSerif(), size,
                     semibold ? Gdiplus::FontStyleBold : Gdiplus::FontStyleRegular);
}

std::unique_ptr<Gdiplus::Font> glyph_font(float size) {
    return font_from({L"Segoe Fluent Icons", L"Segoe MDL2 Assets"}, Gdiplus::FontFamily::GenericSansSerif(), size,
                     Gdiplus::FontStyleRegular);
}

// Segoe Fluent Icons / MDL2 Assets code points.
constexpr wchar_t kGlyphSettings[] = L"\uE713", kGlyphDownload[] = L"\uE896", kGlyphSearch[] = L"\uE721",
                  kGlyphChevron[] = L"\uE70D", kGlyphCheck[] = L"\uE73E", kGlyphApps[] = L"\uE71D",
                  kGlyphError[] = L"\uE783", kGlyphClock[] = L"\uE823", kGlyphStop[] = L"\uE71A";

void make_fonts() {
    g_f.title = font(26, true);
    g_f.tagline = font(13.5f);
    g_f.body = font(14);
    g_f.body_semi = font(14, true);
    g_f.caption = font(12);
    g_f.caption_semi = font(12, true);
    g_f.section = font(16, true);
    g_f.tile = font(13);
    g_f.hero = font(20, true);
    g_f.monogram = font(30, true);
    g_f.monogram_small = font(26, true);
    g_f.glyph12 = glyph_font(12);
    g_f.glyph16 = glyph_font(16);
    g_f.glyph34 = glyph_font(34);
    g_f.glyph48 = glyph_font(48);
    g_f.mono = font_from({L"Cascadia Mono", L"Consolas"}, Gdiplus::FontFamily::GenericMonospace(), 12,
                         Gdiplus::FontStyleRegular);
    if (g_mono_font) DeleteObject(g_mono_font);
    g_mono_font = CreateFontW(-ipx(13), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                              CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
    if (g_edit_font) DeleteObject(g_edit_font);
    g_edit_font = CreateFontW(-ipx(14), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                              CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
}

RectF inset(const RectF& r, float dx, float dy) { return RectF(r.X + dx, r.Y + dy, r.Width - 2 * dx, r.Height - 2 * dy); }
bool contains(const RectF& r, float x, float y) { return x >= r.X && y >= r.Y && x < r.GetRight() && y < r.GetBottom(); }

void rounded(GraphicsPath& p, const RectF& r, float rad) {
    rad = (std::min)(rad, (std::min)(r.Width, r.Height) / 2);
    const float d = rad * 2;
    if (d <= 0) {
        p.AddRectangle(r);
        return;
    }
    p.AddArc(r.X, r.Y, d, d, 180, 90);
    p.AddArc(r.GetRight() - d, r.Y, d, d, 270, 90);
    p.AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0, 90);
    p.AddArc(r.X, r.GetBottom() - d, d, d, 90, 90);
    p.CloseFigure();
}

void fill_round(Graphics& g, const RectF& r, float rad, const Color& c) {
    GraphicsPath p;
    rounded(p, r, rad);
    Gdiplus::SolidBrush b(c);
    g.FillPath(&b, &p);
}

void stroke_round(Graphics& g, const RectF& r, float rad, const Color& c, float width = 1) {
    GraphicsPath p;
    rounded(p, inset(r, width / 2, width / 2), rad);
    Gdiplus::Pen pen(c, width);
    g.DrawPath(&pen, &p);
}

enum Align { kLeft, kCenter, kRight };

// In Hebrew the layout is mirrored, so the text is drawn without the mirror,
// right to left, in the mirrored place; `ltr` keeps it left to right (logs).
void draw_text(Graphics& g, const std::wstring& s, const Gdiplus::Font* f, const Color& c, const RectF& r,
               Align align = kLeft, bool wrap = false, bool vcenter = true, bool ltr = false) {
    Gdiplus::StringFormat sf;
    sf.SetAlignment(align == kLeft     ? Gdiplus::StringAlignmentNear
                    : align == kCenter ? Gdiplus::StringAlignmentCenter
                                       : Gdiplus::StringAlignmentFar);
    sf.SetLineAlignment(vcenter ? Gdiplus::StringAlignmentCenter : Gdiplus::StringAlignmentNear);
    sf.SetTrimming(wrap ? Gdiplus::StringTrimmingEllipsisWord : Gdiplus::StringTrimmingEllipsisCharacter);
    sf.SetFormatFlags(wrap ? Gdiplus::StringFormatFlagsLineLimit : Gdiplus::StringFormatFlagsNoWrap);
    Gdiplus::SolidBrush b(c);
    if (!g_rtl) {
        g.DrawString(s.c_str(), static_cast<INT>(s.size()), f, r, &sf, &b);
        return;
    }
    if (!ltr) sf.SetFormatFlags(sf.GetFormatFlags() | Gdiplus::StringFormatFlagsDirectionRightToLeft);
    Gdiplus::Matrix saved;
    g.GetTransform(&saved);
    g.ResetTransform();
    g.DrawString(s.c_str(), static_cast<INT>(s.size()), f, RectF(g_paint_w - r.GetRight(), r.Y, r.Width, r.Height),
                 &sf, &b);
    g.SetTransform(&saved);
}

void draw_image(Graphics& g, Gdiplus::Image* img, const RectF& r) {
    if (!g_rtl) {
        g.DrawImage(img, r);
        return;
    }
    Gdiplus::Matrix saved;
    g.GetTransform(&saved);
    g.ResetTransform();
    g.DrawImage(img, RectF(g_paint_w - r.GetRight(), r.Y, r.Width, r.Height));
    g.SetTransform(&saved);
}

float text_width(Graphics& g, const std::wstring& s, const Gdiplus::Font* f) {
    RectF out;
    g.MeasureString(s.c_str(), static_cast<INT>(s.size()), f, Gdiplus::PointF(0, 0), &out);
    return out.Width;
}

float text_height(Graphics& g, const std::wstring& s, const Gdiplus::Font* f, float width) {
    Gdiplus::StringFormat sf;
    RectF out;
    g.MeasureString(s.c_str(), static_cast<INT>(s.size()), f, RectF(0, 0, width, 100000), &sf, &out);
    return out.Height;
}

void spinner(Graphics& g, const RectF& r, const Color& c, float width) {
    const float a = static_cast<float>(GetTickCount64() % 900) * 360.0f / 900.0f;
    Gdiplus::Pen track(with_alpha(c, 50), width);
    g.DrawEllipse(&track, r);
    Gdiplus::Pen pen(c, width);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    g.DrawArc(&pen, r, a, 100);
}

void draw_monogram(Graphics& g, const RectF& r, const std::wstring& label, const Gdiplus::Font* f) {
    GraphicsPath p;
    rounded(p, r, r.Width * 0.22f);
    Gdiplus::LinearGradientBrush b(r, mix(g_t.accent, kWhite, 0.18f), mix(g_t.accent, kBlack, 0.22f),
                                   Gdiplus::LinearGradientModeForwardDiagonal);
    g.FillPath(&b, &p);
    const std::wstring ch(1, label.empty() ? L'?' : static_cast<wchar_t>(towupper(label[0])));
    draw_text(g, ch, f, kWhite, r, kCenter);
}

void draw_button(Graphics& g, const RectF& r, const std::wstring& text, bool hot, bool primary, bool destructive) {
    const Color fill = primary ? (hot ? mix(g_t.accent, g_t.dark ? kWhite : kBlack, 0.12f) : g_t.accent)
                               : (hot ? g_t.hover : mix(g_t.card, g_t.hover, 0.55f));
    fill_round(g, r, px(6), fill);
    if (!primary) stroke_round(g, r, px(6), g_t.border);
    draw_text(g, text, primary ? g_f.body_semi.get() : g_f.body.get(),
              primary ? kWhite : destructive ? g_t.danger : g_t.text, r, kCenter);
}

// Renders a buffered WM_PAINT through `draw(graphics, width, height)`.
template <class Draw>
void buffered_paint(HWND hwnd, Draw draw) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, (std::max)(1L, rc.right), (std::max)(1L, rc.bottom));
    HGDIOBJ old = SelectObject(mem, bmp);
    SetLayout(mem, 0);
    g_paint_w = static_cast<float>(rc.right);
    {
        Graphics g(mem);
        if (g_rtl) {  // mirror the layout; draw_text and draw_image put text and pictures back the right way round
            Gdiplus::Matrix mirror(-1, 0, 0, 1, g_paint_w, 0);
            g.SetTransform(&mirror);
        }
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        draw(g, static_cast<float>(rc.right), static_cast<float>(rc.bottom));
    }
    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY | (g_rtl ? NOMIRRORBITMAP : 0));
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

// ---------------------------------------------------------------------------
// State

enum StatusKind { kHint, kInfo, kSuccess, kError };

constexpr int kAddTile = -2;

struct Tile {
    int app;           // index into g_apps, -1 for an app still being installed, kAddTile for "Add app"
    std::wstring job;  // that app's name
};

std::vector<App> g_apps;
std::vector<Tile> g_tiles;                 // what the grid shows: new installs first, then the (filtered) apps
std::vector<std::wstring> g_pending;       // names queued or being installed, in order
std::wstring g_current;                    // the one being installed now
int g_batch_total = 0, g_batch_done = 0;   // installs since the queue was last empty
std::set<std::wstring> g_running;          // apps started from this window that are still running
std::set<std::wstring> g_stopping;         // running apps the user stopped: their exit is not an error
struct Job {
    std::wstring name;
    enum State { kWaiting, kInstalling, kDone, kFailed } state = kWaiting;
    ULONGLONG start = 0, end = 0;
};
std::vector<Job> g_jobs;                   // this batch of installs, for the install window
HWND g_install_wnd, g_log_wnd, g_log_edit;
std::map<std::wstring, std::unique_ptr<Gdiplus::Bitmap>> g_icons;  // rounded, at the grid's size; null: no icon
std::wstring g_selected;
std::wstring g_status;
StatusKind g_status_kind = kHint;
float g_scroll = 0;
bool g_drag_over = false, g_tracking = false;

enum HitKind { kHitNone, kHitGear, kHitDrop, kHitTile, kHitThumb, kHitTrack, kHitStop, kHitLogs };
struct Hit {
    HitKind kind = kHitNone;
    int tile = -1;
    bool operator==(const Hit& o) const { return kind == o.kind && tile == o.tile; }
};
Hit g_hot;
bool g_thumb_drag = false;
float g_thumb_y0 = 0, g_thumb_scroll0 = 0;

struct Layout {
    RectF gear, drop, section, search, grid, footer, logs, stop;
    int cols = 1;
    float tile_w = 0, tile_h = 0, gap = 0, content_h = 0, max_scroll = 0;
} g_l;

struct Sheet;
Sheet* g_sheet = nullptr;
HWND sheet_hwnd();
HWND owner() { return sheet_hwnd() ? sheet_hwnd() : g_main; }
void show_logs(const fs::path& log);
void show_install_window();

bool is_pending(const std::wstring& name) {
    return std::find(g_pending.begin(), g_pending.end(), name) != g_pending.end();
}

const App* find_app(const std::wstring& name) {
    for (const App& a : g_apps) {
        if (a.name == name) return &a;
    }
    return nullptr;
}

Gdiplus::Bitmap* icon_for(const App& app) {
    auto it = g_icons.find(app.name);
    if (it != g_icons.end()) return it->second.get();
    std::unique_ptr<Gdiplus::Bitmap> out;
    const fs::path png = app_dir(app.name) / L"icon.png";
    std::error_code ec;
    if (fs::exists(png, ec)) {
        Gdiplus::Bitmap src(png.c_str());
        const int s = ipx(72);
        if (src.GetLastStatus() == Gdiplus::Ok && src.GetWidth() > 0) {
            Gdiplus::Bitmap scaled(s, s, PixelFormat32bppPARGB);
            {
                Graphics g(&scaled);
                g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
                g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
                g.DrawImage(&src, 0, 0, s, s);
            }
            out = std::make_unique<Gdiplus::Bitmap>(s, s, PixelFormat32bppPARGB);
            Graphics g(out.get());
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
            Gdiplus::TextureBrush brush(&scaled);
            GraphicsPath p;
            rounded(p, RectF(0, 0, static_cast<float>(s), static_cast<float>(s)), s * 0.22f);
            g.FillPath(&brush, &p);
        }
    }
    Gdiplus::Bitmap* raw = out.get();
    g_icons[app.name] = std::move(out);
    return raw;
}

void draw_app_icon(Graphics& g, const App* app, const std::wstring& label, const RectF& r) {
    fill_round(g, RectF(r.X, r.Y + px(2), r.Width, r.Height), r.Width * 0.22f, Color(g_t.dark ? 90 : 30, 0, 0, 0));
    if (Gdiplus::Bitmap* bmp = app ? icon_for(*app) : nullptr) {
        draw_image(g, bmp, r);
    } else {
        draw_monogram(g, r, label,
                      r.Width >= px(72)   ? g_f.monogram.get()
                      : r.Width >= px(56) ? g_f.monogram_small.get()
                                          : g_f.section.get());
    }
}

void set_status(const std::wstring& text, StatusKind kind) {
    g_status = text;
    g_status_kind = kind;
    InvalidateRect(g_main, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// Layout

RectF tile_rect(int i) {
    const int col = i % g_l.cols, row = i / g_l.cols;
    return RectF(g_l.grid.X + col * (g_l.tile_w + g_l.gap), g_l.grid.Y + px(4) + row * (g_l.tile_h + g_l.gap) - g_scroll,
                 g_l.tile_w, g_l.tile_h);
}

RectF thumb_rect() {
    const RectF& gr = g_l.grid;
    const float h = (std::max)(px(36), gr.Height * gr.Height / g_l.content_h);
    const float y = gr.Y + (gr.Height - h) * (g_l.max_scroll > 0 ? g_scroll / g_l.max_scroll : 0);
    return RectF(gr.GetRight() - px(8), y, px(6), h);
}

void clamp_scroll() { g_scroll = (std::max)(0.0f, (std::min)(g_scroll, g_l.max_scroll)); }

void compute_layout() {
    RECT rc;
    GetClientRect(g_main, &rc);
    const float W = static_cast<float>(rc.right), H = static_cast<float>(rc.bottom), m = px(28);
    g_l.gear = RectF(W - m - px(36), px(24), px(36), px(36));
    g_l.drop = RectF(m, px(100), W - 2 * m, px(124));
    g_l.section = RectF(m, g_l.drop.GetBottom() + px(18), W - 2 * m, px(36));
    const float search_w = (std::min)(px(260), g_l.section.Width * 0.45f);
    g_l.search = RectF(g_l.section.GetRight() - search_w, g_l.section.Y + px(2), search_w, px(32));
    g_l.footer = RectF(0, H - px(34), W, px(34));
    g_l.logs = RectF(W - px(10) - px(76), g_l.footer.Y + px(5), px(76), px(24));
    g_l.stop = RectF(g_l.logs.X - px(6) - px(72), g_l.logs.Y, px(72), px(24));
    const float top = g_l.section.GetBottom() + px(8);
    g_l.grid = RectF(m - px(8), top, W - 2 * m + px(16), (std::max)(0.0f, g_l.footer.Y - top));
    g_l.tile_w = px(124);
    g_l.tile_h = px(138);
    g_l.gap = px(6);
    g_l.cols = (std::max)(1, static_cast<int>((g_l.grid.Width - px(10) + g_l.gap) / (g_l.tile_w + g_l.gap)));
    const int rows = (static_cast<int>(g_tiles.size()) + g_l.cols - 1) / g_l.cols;
    g_l.content_h = rows ? rows * (g_l.tile_h + g_l.gap) - g_l.gap + px(12) : 0;
    g_l.max_scroll = (std::max)(0.0f, g_l.content_h - g_l.grid.Height);
    clamp_scroll();
    const int edit_h = ipx(20);
    MoveWindow(g_search, static_cast<int>(g_l.search.X + px(34)),
               static_cast<int>(g_l.search.Y + (g_l.search.Height - edit_h) / 2),
               static_cast<int>(g_l.search.Width - px(44)), edit_h, TRUE);
}

void rebuild_tiles() {
    wchar_t buf[256] = {};
    GetWindowTextW(g_search, buf, 256);
    const std::wstring q = lower(buf);
    auto matches = [&](const std::wstring& a, const std::wstring& b) {
        return q.empty() || lower(a).find(q) != std::wstring::npos || lower(b).find(q) != std::wstring::npos;
    };
    g_tiles.clear();
    std::set<std::wstring> shown;
    for (const std::wstring& name : g_pending) {
        if (!find_app(name) && shown.insert(name).second && matches(name, name)) g_tiles.push_back({-1, name});
    }
    for (size_t i = 0; i < g_apps.size(); ++i) {
        if (matches(g_apps[i].label, g_apps[i].name)) g_tiles.push_back({static_cast<int>(i), L""});
    }
    if (q.empty() && !g_apps.empty()) g_tiles.push_back({kAddTile, L""});
    compute_layout();
    InvalidateRect(g_main, nullptr, FALSE);
}

void refresh() {
    g_apps = library();
    g_icons.clear();
    if (!find_app(g_selected)) g_selected.clear();
    rebuild_tiles();
}

int tile_of(const std::wstring& name) {
    for (size_t i = 0; i < g_tiles.size(); ++i) {
        if (g_tiles[i].app >= 0 && g_apps[static_cast<size_t>(g_tiles[i].app)].name == name) return static_cast<int>(i);
    }
    return -1;
}

void ensure_visible(int tile) {
    if (tile < 0) return;
    const RectF r = tile_rect(tile);
    if (r.Y < g_l.grid.Y) g_scroll -= g_l.grid.Y - r.Y + px(4);
    if (r.GetBottom() > g_l.grid.GetBottom()) g_scroll += r.GetBottom() - g_l.grid.GetBottom() + px(4);
    clamp_scroll();
}

void select_app(const std::wstring& name) {
    g_selected = name;
    ensure_visible(tile_of(name));
    InvalidateRect(g_main, nullptr, FALSE);
}

const App* selected() { return g_selected.empty() ? nullptr : find_app(g_selected); }

Hit hit_test(float x, float y) {
    if (contains(g_l.gear, x, y)) return {kHitGear, -1};
    if (contains(g_l.drop, x, y)) return {kHitDrop, -1};
    if (contains(g_l.logs, x, y)) return {kHitLogs, -1};
    if (!g_running.empty() && contains(g_l.stop, x, y)) return {kHitStop, -1};
    if (!contains(g_l.grid, x, y)) return {};
    if (g_l.max_scroll > 0 && x >= g_l.grid.GetRight() - px(12)) {
        return {contains(inset(thumb_rect(), -px(4), 0), x, y) ? kHitThumb : kHitTrack, -1};
    }
    for (size_t i = 0; i < g_tiles.size(); ++i) {
        if (contains(tile_rect(static_cast<int>(i)), x, y)) return {kHitTile, static_cast<int>(i)};
    }
    return {};
}

// ---------------------------------------------------------------------------
// Painting the library

void paint_header(Graphics& g, float W) {
    draw_text(g, L"Win.apk", g_f.title.get(), g_t.text, RectF(0, px(20), W, px(38)), kCenter);
    draw_text(g, tr(L"Run Android apps natively on Windows"), g_f.tagline.get(), g_t.text2, RectF(0, px(58), W, px(22)),
              kCenter);
    const bool hot = g_hot.kind == kHitGear;
    if (hot) fill_round(g, g_l.gear, px(8), g_t.hover);
    draw_text(g, kGlyphSettings, g_f.glyph16.get(), hot ? g_t.text : g_t.text2, g_l.gear, kCenter);
}

void paint_drop_zone(Graphics& g) {
    const RectF& r = g_l.drop;
    const bool hot = g_hot.kind == kHitDrop;
    const bool busy = !g_pending.empty();
    Color fill = g_t.card;
    if (g_drag_over) fill = mix(g_t.card, g_t.accent, g_t.dark ? 0.22f : 0.12f);
    else if (hot) fill = mix(g_t.card, g_t.accent, g_t.dark ? 0.08f : 0.04f);
    fill_round(g, r, px(14), fill);
    GraphicsPath p;
    rounded(p, inset(r, px(1), px(1)), px(13));
    Gdiplus::Pen pen(g_drag_over || hot ? g_t.accent : with_alpha(g_t.text2, 120), px(1.5f));
    const Gdiplus::REAL dashes[] = {4, 3};
    pen.SetDashPattern(dashes, 2);
    g.DrawPath(&pen, &p);

    const Color ink = g_drag_over ? g_t.accent : g_t.text;
    const float cy = r.Y + r.Height / 2;
    draw_text(g, kGlyphDownload, g_f.glyph34.get(), busy || g_drag_over ? g_t.accent : with_alpha(ink, 220),
              RectF(r.X, cy - px(50), r.Width, px(44)), kCenter);
    std::wstring title, sub;
    if (g_drag_over) {
        title = tr(L"Drop to install");
        sub = tr(L"Win.apk keeps its own copy, so the download can go");
    } else if (busy) {
        title = g_batch_total > 1 ? fmt(L"Installing {0} of {1}…",
                                        {std::to_wstring((std::min)(g_batch_done + 1, g_batch_total)),
                                         std::to_wstring(g_batch_total)})
                                  : std::wstring(tr(L"Installing…"));
        sub = g_current.empty() ? tr(L"Unpacking manifest, icon, and assets")
                                : fmt(L"{0}  ·  the first install of an app takes up to a minute", {g_current});
    } else {
        title = tr(L"Drop an APK here to install");
        sub = tr(L"or click to choose a file  ·  .apk .apkm .xapk .apks");
    }
    draw_text(g, title, g_f.body_semi.get(), ink, RectF(r.X + px(16), cy, r.Width - px(32), px(22)), kCenter);
    draw_text(g, sub, g_f.caption.get(), g_t.text2, RectF(r.X + px(16), cy + px(22), r.Width - px(32), px(18)),
              kCenter);
    if (busy) {  // indeterminate bar along the bottom edge
        const RectF track(r.X + r.Width * 0.3f, r.GetBottom() - px(14), r.Width * 0.4f, px(4));
        fill_round(g, track, px(2), with_alpha(g_t.accent, 40));
        const float t = static_cast<float>(GetTickCount64() % 1400) / 1400.0f;
        const float seg = track.Width * 0.35f;
        float x0 = track.X + (track.Width + seg) * t - seg, x1 = x0 + seg;
        x0 = (std::max)(x0, track.X);
        x1 = (std::min)(x1, track.GetRight());
        if (x1 > x0) fill_round(g, RectF(x0, track.Y, x1 - x0, track.Height), px(2), g_t.accent);
    }
}

void paint_section(Graphics& g) {
    const RectF& r = g_l.section;
    const std::wstring title = tr(L"Installed apps");
    const float tw = text_width(g, title, g_f.section.get());
    draw_text(g, title, g_f.section.get(), g_t.text, RectF(r.X, r.Y, tw + px(4), r.Height));
    const std::wstring count = std::to_wstring(g_apps.size());
    const float cw = (std::max)(px(24), text_width(g, count, g_f.caption_semi.get()) + px(12));
    const RectF pill(r.X + tw + px(8), r.Y + (r.Height - px(20)) / 2, cw, px(20));
    fill_round(g, pill, px(10), g_t.dark ? g_t.hover : mix(g_t.bg, g_t.border, 0.6f));
    draw_text(g, count, g_f.caption_semi.get(), g_t.text2, pill, kCenter);

    const bool focused = GetFocus() == g_search;
    fill_round(g, g_l.search, px(8), g_t.card);
    stroke_round(g, g_l.search, px(8), focused ? g_t.accent : g_t.border, focused ? px(1.5f) : 1);
    draw_text(g, kGlyphSearch, g_f.glyph12.get(), g_t.text2,
              RectF(g_l.search.X + px(10), g_l.search.Y, px(16), g_l.search.Height), kCenter);
}

void paint_tile(Graphics& g, int i) {
    const RectF r = tile_rect(i);
    if (r.GetBottom() < g_l.grid.Y || r.Y > g_l.grid.GetBottom()) return;
    const Tile& t = g_tiles[static_cast<size_t>(i)];
    const bool hot_tile = g_hot.kind == kHitTile && g_hot.tile == i;
    if (t.app == kAddTile) {
        if (hot_tile) fill_round(g, r, px(12), g_t.hover);
        const float s = px(56);
        const RectF box(r.X + (r.Width - s) / 2, r.Y + px(22), s, s);
        GraphicsPath p;
        rounded(p, box, px(14));
        Gdiplus::Pen dash(hot_tile ? g_t.accent : with_alpha(g_t.text2, 140), px(1.5f));
        const Gdiplus::REAL dashes[] = {3, 3};
        dash.SetDashPattern(dashes, 2);
        g.DrawPath(&dash, &p);
        Gdiplus::Pen plus(hot_tile ? g_t.accent : g_t.text2, px(2));
        plus.SetStartCap(Gdiplus::LineCapRound);
        plus.SetEndCap(Gdiplus::LineCapRound);
        const float cx = box.X + s / 2, cy = box.Y + s / 2, a = px(10);
        g.DrawLine(&plus, cx - a, cy, cx + a, cy);
        g.DrawLine(&plus, cx, cy - a, cx, cy + a);
        draw_text(g, tr(L"Add app"), g_f.tile.get(), g_t.text2,
                  RectF(r.X + px(6), box.GetBottom() + px(17), r.Width - px(12), px(20)), kCenter);
        return;
    }
    const App* app = t.app >= 0 ? &g_apps[static_cast<size_t>(t.app)] : nullptr;
    const bool sel = app && app->name == g_selected;
    const bool hot = g_hot.kind == kHitTile && g_hot.tile == i;
    if (sel) {
        fill_round(g, r, px(12), with_alpha(g_t.accent, g_t.dark ? 64 : 34));
        stroke_round(g, r, px(12), with_alpha(g_t.accent, 150));
    } else if (hot) {
        fill_round(g, r, px(12), g_t.hover);
    }
    const std::wstring& label = app ? app->label : t.job;
    const float s = px(72);
    const RectF ir(r.X + (r.Width - s) / 2, r.Y + px(14), s, s);
    draw_app_icon(g, app, label, ir);
    if (!app || is_pending(app->name)) {
        fill_round(g, ir, s * 0.22f, Color(130, 0, 0, 0));
        spinner(g, inset(ir, s * 0.3f, s * 0.3f), kWhite, px(3));
    }
    draw_text(g, label, g_f.tile.get(), app ? g_t.text : g_t.text2,
              RectF(r.X + px(6), ir.GetBottom() + px(9), r.Width - px(12), px(36)), kCenter, true, false);
    if (app && g_running.count(app->name)) {
        Gdiplus::SolidBrush dot(g_t.accent);
        g.FillEllipse(&dot, r.X + r.Width / 2 - px(3), r.GetBottom() - px(9), px(6), px(6));
    }
}

void paint_grid(Graphics& g) {
    const RectF& gr = g_l.grid;
    if (g_tiles.empty()) {
        wchar_t q[256] = {};
        GetWindowTextW(g_search, q, 256);
        const float cy = gr.Y + (std::min)(gr.Height / 2, px(110));
        std::wstring title, sub;
        if (q[0]) {
            title = fmt(L"No apps match “{0}”", {q});
            sub = tr(L"Press Esc in the search box to show every app.");
        } else {
            title = tr(L"No apps installed yet.");
            sub = tr(L"Drop an APK above (or click it to choose one) to install one, then open it from here.");
        }
        draw_text(g, q[0] ? kGlyphSearch : kGlyphApps, g_f.glyph48.get(), with_alpha(g_t.text2, 150),
                  RectF(gr.X, cy - px(70), gr.Width, px(60)), kCenter);
        draw_text(g, title, g_f.body_semi.get(), g_t.text, RectF(gr.X, cy, gr.Width, px(24)), kCenter);
        draw_text(g, sub, g_f.caption.get(), g_t.text2, RectF(gr.X + px(24), cy + px(24), gr.Width - px(48), px(36)),
                  kCenter, true, false);
        return;
    }
    g.SetClip(gr);
    for (size_t i = 0; i < g_tiles.size(); ++i) paint_tile(g, static_cast<int>(i));
    g.ResetClip();
    if (g_scroll > 0) {
        Gdiplus::Pen line(g_t.border, 1);
        g.DrawLine(&line, gr.X + px(8), gr.Y, gr.GetRight() - px(8), gr.Y);
    }
    if (g_l.max_scroll > 0) {
        const bool active = g_thumb_drag || g_hot.kind == kHitThumb || g_hot.kind == kHitTrack;
        const bool nearby = g_hot.kind == kHitTile || active;
        fill_round(g, thumb_rect(), px(3), with_alpha(g_t.text2, active ? 200 : nearby ? 120 : 70));
    }
}

// A small borderless button with a glyph, for the footer.
void draw_chip(Graphics& g, const RectF& r, const wchar_t* glyph, const std::wstring& text, const Color& c, bool hot) {
    if (hot) fill_round(g, r, px(6), g_t.hover);
    const float tw = text_width(g, text, g_f.caption.get());
    const float x = r.X + (r.Width - tw - px(18)) / 2;
    draw_text(g, glyph, g_f.glyph12.get(), c, RectF(x, r.Y, px(14), r.Height), kCenter);
    draw_text(g, text, g_f.caption.get(), c, RectF(x + px(18), r.Y, tw + px(2), r.Height));
}

const App* running_app() {
    if (const App* a = selected(); a && g_running.count(a->name)) return a;
    return g_running.empty() ? nullptr : find_app(*g_running.begin());
}

void paint_footer(Graphics& g, float W) {
    const RectF& r = g_l.footer;
    Gdiplus::SolidBrush bar(g_t.dark ? mix(g_t.bg, kBlack, 0.15f) : mix(g_t.bg, kWhite, 0.5f));
    g.FillRectangle(&bar, r);
    Gdiplus::Pen line(g_t.border, 1);
    g.DrawLine(&line, 0.0f, r.Y, W, r.Y);
    draw_chip(g, g_l.logs, L"\uE8A5", tr(L"Logs"), g_t.text2, g_hot.kind == kHitLogs);
    const App* running = running_app();
    if (running) draw_chip(g, g_l.stop, kGlyphStop, tr(L"Stop"), g_t.danger, g_hot.kind == kHitStop);
    const float right = running ? g_l.stop.X : g_l.logs.X;

    const std::wstring hint = g_apps.empty()
                                  ? tr(L"Drop an APK above to install your first app")
                                  : tr(L"Double-click to open  ·  right-click for options  ·  drop an APK above to add more");
    const bool hinted = g_status.empty() || g_status_kind == kHint;
    const Color c = hinted || g_status_kind == kInfo ? g_t.text2
                    : g_status_kind == kSuccess       ? g_t.success
                                                      : g_t.danger;
    const std::wstring count = app_count(g_apps.size());
    const float cw = text_width(g, count, g_f.caption.get()) + px(4);
    float x = px(16);
    std::wstring text = hinted ? hint : g_status;
    if (running && (hinted || g_status_kind == kInfo)) {
        spinner(g, RectF(x, r.Y + r.Height / 2 - px(6), px(12), px(12)), g_t.accent, px(2));
        x += px(20);
        text = g_running.size() > 1 ? fmt(L"{0} apps are running", {std::to_wstring(g_running.size())})
                                    : fmt(L"{0} is running", {running->label});
    } else if (!hinted && g_status_kind != kInfo) {
        Gdiplus::SolidBrush dot(c);
        g.FillEllipse(&dot, x, r.Y + r.Height / 2 - px(3), px(6), px(6));
        x += px(14);
    }
    draw_text(g, text, g_f.caption.get(), running ? g_t.text : c, RectF(x, r.Y, right - x - cw - px(24), r.Height));
    draw_text(g, count, g_f.caption.get(), g_t.text2, RectF(right - px(12) - cw, r.Y, cw, r.Height), kRight);
}

void paint_main(Graphics& g, float W, float) {
    g.Clear(g_t.bg);
    paint_header(g, W);
    paint_drop_zone(g);
    paint_section(g);
    paint_grid(g);
    paint_footer(g, W);
}

void update_timer() {
    if (!g_pending.empty() || !g_running.empty()) SetTimer(g_main, kAnimTimer, 16, nullptr);
    else KillTimer(g_main, kAnimTimer);
}

// ---------------------------------------------------------------------------
// Dialogs

int task_dialog(const std::wstring& main, const std::wstring& content, PCWSTR icon,
                std::vector<TASKDIALOG_BUTTON> buttons, bool cancel) {
    TASKDIALOGCONFIG c{};
    c.cbSize = sizeof(c);
    c.hwndParent = owner();
    c.hInstance = g_inst;
    c.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW | (g_rtl ? TDF_RTL_LAYOUT : 0);
    c.pszWindowTitle = L"Win.apk";
    c.pszMainIcon = icon;
    c.pszMainInstruction = main.c_str();
    c.pszContent = content.c_str();
    c.cButtons = static_cast<UINT>(buttons.size());
    c.pButtons = buttons.data();
    c.dwCommonButtons = cancel ? TDCBF_CANCEL_BUTTON : TDCBF_CLOSE_BUTTON;
    int pressed = 0;
    if (FAILED(TaskDialogIndirect(&c, &pressed, nullptr, nullptr))) return 0;
    return pressed;
}

bool confirm(const std::wstring& main, const std::wstring& content, const wchar_t* action) {
    return task_dialog(main, content, TD_WARNING_ICON, {{IDOK, action}}, true) == IDOK;
}

void failed_with_log(const std::wstring& main, const fs::path& log) {
    if (task_dialog(main, fmt(L"The log may say why:\n{0}", {log.wstring()}), TD_ERROR_ICON, {{100, tr(L"Open the log")}},
                    false) == 100) {
        show_logs(log);
    }
}

std::vector<fs::path> pick_files(bool multiple, const wchar_t* title) {
    std::vector<fs::path> out;
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return out;
    const COMDLG_FILTERSPEC types[] = {{tr(L"Android apps"), L"*.apk;*.apkm;*.xapk;*.apks"}, {tr(L"All files"), L"*.*"}};
    dlg->SetFileTypes(2, types);
    dlg->SetTitle(title);
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_FILEMUSTEXIST | (multiple ? static_cast<DWORD>(FOS_ALLOWMULTISELECT) : 0u));
    IShellItemArray* items = nullptr;
    if (SUCCEEDED(dlg->Show(owner())) && SUCCEEDED(dlg->GetResults(&items))) {
        DWORD n = 0;
        items->GetCount(&n);
        for (DWORD i = 0; i < n; ++i) {
            IShellItem* item = nullptr;
            PWSTR path = nullptr;
            if (SUCCEEDED(items->GetItemAt(i, &item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                out.emplace_back(path);
            }
            CoTaskMemFree(path);
            if (item) item->Release();
        }
        items->Release();
    }
    dlg->Release();
    return out;
}

// ---------------------------------------------------------------------------
// Actions

void install(const fs::path& source, bool run_after, std::wstring name = L"", fs::path replaces = {}) {
    if (name.empty()) name = file_name_safe(source.stem().wstring());
    if (g_pending.empty()) {
        g_batch_total = g_batch_done = 0;
        g_jobs.clear();
    }
    ++g_batch_total;
    g_pending.push_back(name);
    g_jobs.push_back({name});
    set_status(fmt(L"Installing {0}…", {name}), kInfo);
    CloseHandle(CreateThread(nullptr, 0, install_thread, new InstallJob{source, name, replaces, run_after}, 0,
                             nullptr));
    update_timer();
    rebuild_tiles();
    show_install_window();
}

void add_from_dialog() {
    for (const fs::path& p : pick_files(true, tr(L"Install an Android app"))) install(p, false);
}

void launch(const App& app) {
    if (is_pending(app.name)) {
        set_status(fmt(L"{0} is still being installed.", {app.label}), kInfo);
        return;
    }
    if (g_running.count(app.name)) {
        set_status(fmt(L"{0} is already open.", {app.label}), kInfo);
        return;
    }
    g_running.insert(app.name);
    set_status(fmt(L"Opening {0}…", {app.label}), kInfo);
    CloseHandle(CreateThread(nullptr, 0, run_thread, new RunJob{app.name, app.apk}, 0, nullptr));
    update_timer();
}

bool remove_app(const App& app, bool ask) {
    if (ask && !confirm(fmt(L"Remove {0}?", {app.label}),
                        tr(L"Its shortcuts go too. What the app saved stays, so installing it again picks up where it left off."),
                        tr(L"Remove"))) {
        return false;
    }
    const App copy = app;  // `app` may point into g_apps, which refresh() replaces
    std::error_code ec;
    remove_app_shortcuts(copy);
    fs::remove(copy.apk, ec);
    fs::remove(label_file(copy.name), ec);
    fs::remove(shortcut_icon(copy.name), ec);
    fs::remove_all(app_dir(copy.name), ec);
    fs::remove(data_dir() / L"android" / L"data" / L"app" / (L"." + copy.name + L".installed"), ec);
    if (ask) {
        refresh();
        set_status(fmt(L"{0} was removed.", {copy.label}), kInfo);
    }
    return true;
}

void update_from_apk(const App& app) {
    const auto files = pick_files(false, fmt(L"Update {0} from an APK", {app.label}).c_str());
    if (!files.empty()) install(files[0], false, app.name, app.apk);
}

// ---------------------------------------------------------------------------
// Sheets: the settings and Get Info windows, drawn like upstream's grouped forms

struct Row {
    enum Kind { kText, kButton, kToggle, kChoice } kind = kText;
    std::wstring title, detail, value;  // value: the button's text, or the text on the right
    bool on = false, destructive = false, primary = false, check = false;
    std::vector<std::wstring> choices;
    int choice = 0;
    std::function<void(int)> act;  // button: act(0), toggle: act(new state), choice: act(index)
};

struct Section {
    std::wstring title;
    std::vector<Row> rows;
    std::wstring footer;
};

struct Sheet {
    std::wstring title;
    std::wstring hero_app, hero_title, hero_sub;  // Get Info's heading: the app's icon, name and package
    std::function<std::vector<Section>()> build;
    std::vector<Section> sections;
    HWND hwnd = nullptr;
    float scroll = 0, content_h = 0, view_h = 0;
    struct Target {
        RectF r;
        int section, row;
    };
    std::vector<Target> targets;
    RectF done;
    int hot = -1;  // index into targets, -2 the Done button
    bool tracking = false;
};

HWND sheet_hwnd() { return g_sheet ? g_sheet->hwnd : nullptr; }

void close_sheet() {
    if (!g_sheet || !g_sheet->hwnd) return;
    EnableWindow(g_main, TRUE);  // before the sheet goes, so Windows activates the library and not another app
    DestroyWindow(g_sheet->hwnd);
}

SizeF control_size(Graphics& g, const Row& row) {
    switch (row.kind) {
        case Row::kButton: return SizeF(text_width(g, row.value, g_f.body_semi.get()) + px(32), px(32));
        case Row::kToggle: return SizeF(px(44), px(22));
        case Row::kChoice: {
            float w = 0;
            for (const auto& c : row.choices) w = (std::max)(w, text_width(g, c, g_f.body.get()));
            return SizeF((std::min)(px(250), w + px(48)), px(32));
        }
        case Row::kText:
            if (row.value.empty()) return SizeF(0, 0);
            return SizeF((std::min)(px(260), text_width(g, row.value, g_f.body.get()) + (row.check ? px(22) : 0)),
                         px(22));
    }
    return SizeF(0, 0);
}

void paint_control(Graphics& g, const Row& row, const RectF& r, bool hot) {
    switch (row.kind) {
        case Row::kButton: draw_button(g, r, row.value, hot, row.primary, row.destructive); break;
        case Row::kToggle: {
            fill_round(g, r, r.Height / 2, row.on ? (hot ? mix(g_t.accent, kWhite, 0.1f) : g_t.accent)
                                                  : (hot ? g_t.hover : g_t.card));
            if (!row.on) stroke_round(g, r, r.Height / 2, g_t.text2);
            const float k = row.on ? r.Height - px(8) : r.Height - px(10);
            const float kx = row.on ? r.GetRight() - k - px(4) : r.X + px(5);
            Gdiplus::SolidBrush knob(row.on ? kWhite : g_t.text2);
            g.FillEllipse(&knob, kx, r.Y + (r.Height - k) / 2, k, k);
            break;
        }
        case Row::kChoice: {
            fill_round(g, r, px(6), hot ? g_t.hover : mix(g_t.card, g_t.hover, 0.55f));
            stroke_round(g, r, px(6), g_t.border);
            const std::wstring& v = row.choices.empty() ? std::wstring() : row.choices[static_cast<size_t>(row.choice)];
            draw_text(g, v, g_f.body.get(), g_t.text, RectF(r.X + px(12), r.Y, r.Width - px(40), r.Height));
            draw_text(g, kGlyphChevron, g_f.glyph12.get(), g_t.text2,
                      RectF(r.GetRight() - px(30), r.Y, px(22), r.Height), kCenter);
            break;
        }
        case Row::kText: {
            float x = r.X;
            if (row.check) {
                draw_text(g, kGlyphCheck, g_f.glyph16.get(), g_t.success, RectF(x, r.Y, px(18), r.Height), kCenter);
                x += px(22);
            }
            draw_text(g, row.value, g_f.body.get(), g_t.text2, RectF(x, r.Y, r.GetRight() - x, r.Height), kRight);
            break;
        }
    }
}

void paint_sheet(Sheet& s, Graphics& g, float W, float H) {
    g.Clear(g_t.bg);
    s.targets.clear();
    const float m = px(24), pad = px(16), foot = px(64), cw = W - 2 * m;
    s.view_h = H - foot;
    g.SetClip(RectF(0, 0, W, s.view_h));
    float y = px(20) - s.scroll;
    if (!s.hero_title.empty()) {
        const RectF ir(m, y, px(64), px(64));
        draw_app_icon(g, find_app(s.hero_app), s.hero_title, ir);
        draw_text(g, s.hero_title, g_f.hero.get(), g_t.text, RectF(m + px(80), y + px(6), cw - px(80), px(30)));
        draw_text(g, s.hero_sub, g_f.caption.get(), g_t.text2, RectF(m + px(80), y + px(36), cw - px(80), px(20)));
        y += px(88);
    }
    for (size_t si = 0; si < s.sections.size(); ++si) {
        const Section& sec = s.sections[si];
        if (!sec.title.empty()) {
            draw_text(g, sec.title, g_f.caption_semi.get(), g_t.text2, RectF(m + px(4), y, cw, px(20)));
            y += px(24);
        }
        std::vector<float> heights;
        std::vector<SizeF> controls;
        float total = 0;
        for (const Row& row : sec.rows) {
            const SizeF c = control_size(g, row);
            const float tw = cw - 2 * pad - (c.Width > 0 ? c.Width + px(16) : 0);
            float h = px(20);
            if (!row.detail.empty()) h += px(2) + text_height(g, row.detail, g_f.caption.get(), tw);
            heights.push_back((std::max)(px(52), h + px(24)));
            controls.push_back(c);
            total += heights.back();
        }
        const RectF card(m, y, cw, total);
        fill_round(g, card, px(10), g_t.card);
        stroke_round(g, card, px(10), g_t.border);
        float ry = y;
        for (size_t ri = 0; ri < sec.rows.size(); ++ri) {
            const Row& row = sec.rows[ri];
            const SizeF c = controls[ri];
            const float h = heights[ri];
            if (ri > 0) {
                Gdiplus::Pen line(g_t.border, 1);
                g.DrawLine(&line, m + pad, ry, m + cw - pad, ry);
            }
            const float tw = cw - 2 * pad - (c.Width > 0 ? c.Width + px(16) : 0);
            const float dh = row.detail.empty() ? 0 : px(2) + text_height(g, row.detail, g_f.caption.get(), tw);
            const float ty = ry + (h - px(20) - dh) / 2;
            draw_text(g, row.title, g_f.body.get(), row.destructive && row.kind == Row::kText ? g_t.danger : g_t.text,
                      RectF(m + pad, ty, tw, px(20)));
            if (!row.detail.empty()) {
                draw_text(g, row.detail, g_f.caption.get(), g_t.text2, RectF(m + pad, ty + px(22), tw, dh + px(2)),
                          kLeft, true, false);
            }
            if (c.Width > 0) {
                const RectF cr(m + cw - pad - c.Width, ry + (h - c.Height) / 2, c.Width, c.Height);
                const bool interactive = row.kind != Row::kText;
                const bool hot = interactive && s.hot == static_cast<int>(s.targets.size());
                paint_control(g, row, cr, hot);
                if (interactive) s.targets.push_back({cr, static_cast<int>(si), static_cast<int>(ri)});
            }
            ry += h;
        }
        y = ry + px(8);
        if (!sec.footer.empty()) {
            const float fh = text_height(g, sec.footer, g_f.caption.get(), cw - px(8));
            draw_text(g, sec.footer, g_f.caption.get(), g_t.text2, RectF(m + px(4), y, cw - px(8), fh + px(2)), kLeft,
                      true, false);
            y += fh + px(4);
        }
        y += px(18);
    }
    s.content_h = y + s.scroll;
    g.ResetClip();

    Gdiplus::SolidBrush bar(g_t.dark ? mix(g_t.bg, kBlack, 0.15f) : mix(g_t.bg, kWhite, 0.5f));
    g.FillRectangle(&bar, RectF(0, s.view_h, W, foot));
    Gdiplus::Pen line(g_t.border, 1);
    g.DrawLine(&line, 0.0f, s.view_h, W, s.view_h);
    s.done = RectF(W - m - px(100), s.view_h + (foot - px(34)) / 2, px(100), px(34));
    draw_button(g, s.done, tr(L"Done"), s.hot == -2, true, false);
}

int sheet_hit(const Sheet& s, float x, float y) {
    if (contains(s.done, x, y)) return -2;
    if (y >= s.view_h) return -1;
    for (size_t i = 0; i < s.targets.size(); ++i) {
        if (contains(s.targets[i].r, x, y)) return static_cast<int>(i);
    }
    return -1;
}

void sheet_click(Sheet& s, int hit) {
    if (hit == -2) {
        close_sheet();
        return;
    }
    if (hit < 0 || hit >= static_cast<int>(s.targets.size())) return;
    const Sheet::Target t = s.targets[static_cast<size_t>(hit)];
    const Row row = s.sections[static_cast<size_t>(t.section)].rows[static_cast<size_t>(t.row)];  // act may rebuild
    if (!row.act) return;
    if (row.kind == Row::kButton) {
        row.act(0);
    } else if (row.kind == Row::kToggle) {
        row.act(!row.on);
    } else if (row.kind == Row::kChoice) {
        HMENU menu = CreatePopupMenu();
        for (size_t i = 0; i < row.choices.size(); ++i) {
            AppendMenuW(menu, MF_STRING | (static_cast<int>(i) == row.choice ? MF_CHECKED : 0), i + 1,
                        row.choices[i].c_str());
        }
        POINT pt{static_cast<LONG>(t.r.X), static_cast<LONG>(t.r.GetBottom() + px(2))};
        ClientToScreen(s.hwnd, &pt);
        const int pick = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_TOPALIGN | menu_flags(true), pt.x, pt.y, 0, s.hwnd,
                                        nullptr);
        DestroyMenu(menu);
        if (pick > 0) row.act(pick - 1);
    }
    if (g_sheet == &s && IsWindow(s.hwnd)) {
        s.sections = s.build();
        InvalidateRect(s.hwnd, nullptr, FALSE);
    }
}

LRESULT CALLBACK sheet_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* s = reinterpret_cast<Sheet*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_NCCREATE:
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
            break;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT:
            buffered_paint(hwnd, [&](Graphics& g, float w, float h) { paint_sheet(*s, g, w, h); });
            return 0;
        case WM_MOUSEMOVE: {
            if (!s->tracking) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
                s->tracking = TrackMouseEvent(&tme);
            }
            const int hot = sheet_hit(*s, static_cast<float>(GET_X_LPARAM(lp)), static_cast<float>(GET_Y_LPARAM(lp)));
            if (hot != s->hot) {
                s->hot = hot;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            s->tracking = false;
            s->hot = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                SetCursor(LoadCursorW(nullptr, s->hot != -1 ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_LBUTTONUP:
            sheet_click(*s, sheet_hit(*s, static_cast<float>(GET_X_LPARAM(lp)), static_cast<float>(GET_Y_LPARAM(lp))));
            return 0;
        case WM_MOUSEWHEEL: {
            s->scroll -= GET_WHEEL_DELTA_WPARAM(wp) / 120.0f * px(60);
            s->scroll = (std::max)(0.0f, (std::min)(s->scroll, (std::max)(0.0f, s->content_h - s->view_h)));
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE || wp == VK_RETURN) close_sheet();
            return 0;
        case WM_CLOSE:
            close_sheet();
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Shows a sheet over the library and returns when it is closed.
void run_sheet(Sheet sheet) {
    if (g_sheet) return;
    g_sheet = &sheet;
    sheet.sections = sheet.build();
    RECT mr;
    GetWindowRect(g_main, &mr);
    const int w = ipx(580), h = (std::min)(ipx(680), static_cast<int>(mr.bottom - mr.top) - ipx(20));
    const int x = mr.left + (mr.right - mr.left - w) / 2, y = mr.top + (mr.bottom - mr.top - h) / 2;
    sheet.hwnd = CreateWindowExW(layout_style(), L"WinApkSheet", sheet.title.c_str(), WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y,
                                 w, (std::max)(h, ipx(360)), g_main, nullptr, g_inst, &sheet);
    if (!sheet.hwnd) {
        g_sheet = nullptr;
        return;
    }
    theme_window(sheet.hwnd);
    EnableWindow(g_main, FALSE);
    ShowWindow(sheet.hwnd, SW_SHOW);
    MSG m;
    while (IsWindow(sheet.hwnd)) {
        const BOOL got = GetMessageW(&m, nullptr, 0, 0);
        if (got <= 0) {
            PostQuitMessage(static_cast<int>(m.wParam));
            close_sheet();
            break;
        }
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    EnableWindow(g_main, TRUE);
    g_sheet = nullptr;
    SetActiveWindow(g_main);
    InvalidateRect(g_main, nullptr, FALSE);
}

Row button_row(std::wstring title, std::wstring detail, std::wstring button, std::function<void()> act,
               bool destructive = false) {
    Row r;
    r.kind = Row::kButton;
    r.title = std::move(title);
    r.detail = std::move(detail);
    r.value = std::move(button);
    r.destructive = destructive;
    r.act = [act](int) { act(); };
    return r;
}

Row text_row(std::wstring title, std::wstring value, std::wstring detail = L"") {
    Row r;
    r.title = std::move(title);
    r.value = std::move(value);
    r.detail = std::move(detail);
    return r;
}

void apply_theme();
void apply_language();

std::vector<Section> settings_sections() {
    std::vector<Section> out;

    Row theme;
    theme.kind = Row::kChoice;
    theme.title = tr(L"Theme");
    theme.choices = {tr(L"Match Windows"), tr(L"Light"), tr(L"Dark")};
    theme.choice = static_cast<int>((std::min)(setting(L"Theme", 0), 2ul));
    theme.act = [](int i) {
        set_setting(L"Theme", static_cast<DWORD>(i));
        apply_theme();
    };
    Row lang;
    lang.kind = Row::kChoice;
    lang.title = tr(L"Language");
    lang.choices = {tr(L"Match Windows"), L"English", L"עברית"};
    lang.choice = static_cast<int>((std::min)(setting(L"Language", 0), 2ul));
    lang.act = [](int i) {
        set_setting(L"Language", static_cast<DWORD>(i));
        apply_language();
    };
    out.push_back({tr(L"Appearance"), {theme, lang}, L""});

    Row types;
    if (is_default_handler()) {
        types = text_row(tr(L"Open Android app files with Win.apk"), tr(L"Default"));
        types.check = true;
    } else {
        types = button_row(tr(L"Open Android app files with Win.apk"), L"", tr(L"Make default"), [] {
            register_file_types(true);
        });
    }
    out.push_back({tr(L"File types"), {types},
                   tr(L"Covers .apk, .apkm, .xapk and .apks. Double-clicking one in Explorer installs it and opens it.")});

    Row size;
    size.kind = Row::kChoice;
    size.title = tr(L"Screen size");
    for (const auto& p : kPhonePresets) size.choices.push_back(tr(p.label));
    size.choice = static_cast<int>(setting(L"PhoneSize", 0));
    if (size.choice >= static_cast<int>(size.choices.size())) size.choice = 0;
    size.act = [](int i) { set_setting(L"PhoneSize", static_cast<DWORD>(i)); };
    Row orient;
    orient.kind = Row::kChoice;
    orient.title = tr(L"Orientation");
    orient.choices = {tr(L"Portrait"), tr(L"Landscape")};
    orient.choice = setting(L"Landscape", 0) ? 1 : 0;
    orient.act = [](int i) { set_setting(L"Landscape", static_cast<DWORD>(i)); };
    out.push_back({tr(L"Screen"), {size, orient},
                   tr(L"Used the next time an app opens, from here or from its shortcut. Sizes are in dp, as on a phone.")});

    Row desktop;
    desktop.kind = Row::kToggle;
    desktop.title = tr(L"Desktop shortcut for new apps");
    desktop.detail = tr(L"Every app also gets one in Start > Win.apk Apps.");
    desktop.on = setting(L"DesktopShortcut", 1) != 0;
    desktop.act = [](int on) { set_setting(L"DesktopShortcut", static_cast<DWORD>(on)); };
    Row redo = button_row(tr(L"Shortcuts"), tr(L"Make the Start menu shortcuts of every app again, with their icons."),
                          tr(L"Recreate"), [] {
                              for (const App& a : g_apps) make_app_shortcuts(a, false);
                              set_status(tr(L"The Start menu shortcuts were made again."), kSuccess);
                          });
    out.push_back({tr(L"Shortcuts"), {desktop, redo}, L""});

    out.push_back({tr(L"Logs"),
                   {button_row(tr(L"Diagnostics"),
                               tr(L"install-<app>.log records each install, last-run.log the app opened last."),
                               tr(L"View logs"), [] { show_logs(last_run_log()); }),
                    button_row(tr(L"Logs folder"), logs_dir().wstring(), tr(L"Open folder"), [] { open_path(logs_dir()); })},
                   tr(L"Attach the log to a bug report: it says what the runtime did and where it stopped.")});
    out.push_back({tr(L"App data"),
                   {button_row(tr(L"Saves, caches and files"),
                               tr(L"Each installed app keeps its data here. Removing an app keeps what it saved."),
                               tr(L"Open folder"), [] { open_path(data_dir()); })},
                   L""});

    Section about{tr(L"About"), {text_row(L"Win.apk", fmt(L"Version {0}", {version()}),
                                      tr(L"An unofficial port of Mac.apk to Windows. Not affiliated with or endorsed by the Mac.apk project."))},
                  L""};
    if (!g_apps.empty()) {
        about.rows.push_back(button_row(tr(L"Remove all apps"), tr(L"Their shortcuts go too. What they saved stays."),
                                        tr(L"Remove all…"), [] {
                                            if (!confirm(fmt(L"Remove all {0} apps?", {std::to_wstring(g_apps.size())}),
                                                         tr(L"Their shortcuts go too. What they saved stays."),
                                                         tr(L"Remove all"))) {
                                                return;
                                            }
                                            const std::vector<App> apps = g_apps;
                                            for (const App& a : apps) remove_app(a, false);
                                            refresh();
                                            set_status(tr(L"Every app was removed."), kInfo);
                                        },
                                        true));
    }
    out.push_back(std::move(about));
    return out;
}

void show_settings() {
    Sheet s;
    s.title = tr(L"Settings");
    s.build = settings_sections;
    run_sheet(std::move(s));
}

void show_info(const App& app) {
    const App a = app;
    std::error_code ec;
    const std::wstring apk_size = format_bytes(fs::file_size(a.apk, ec));
    const std::wstring files_size = format_bytes(folder_size(app_dir(a.name)));
    const std::wstring installed = file_date(label_file(a.name));
    Sheet s;
    s.title = a.label;
    s.hero_app = a.name;
    s.hero_title = a.label;
    s.hero_sub = fmt(L"{0}  ·  Android app", {a.name});
    s.build = [a, apk_size, files_size, installed] {
        std::vector<Section> out;
        out.push_back({tr(L"Identity"),
                       {text_row(tr(L"Name"), a.label), text_row(tr(L"Library name"), a.name),
                        text_row(tr(L"Installed"), installed)},
                       L""});
        out.push_back({tr(L"Storage"),
                       {button_row(fmt(L"Source APK  ·  {0}", {apk_size}), a.apk.wstring(), tr(L"Show"),
                                   [a] { show_in_explorer(a.apk); }),
                        button_row(fmt(L"App files  ·  {0}", {files_size}), app_dir(a.name).wstring(), tr(L"Open"),
                                   [a] { open_path(app_dir(a.name)); }),
                        button_row(tr(L"Install log"), install_log(a.name).wstring(), tr(L"View"),
                                   [a] { show_logs(install_log(a.name)); })},
                       L""});
        Row open = button_row(fmt(L"Open {0}", {a.label}), L"", tr(L"Open"), [a] {
            close_sheet();
            if (const App* cur = find_app(a.name)) launch(*cur);
        });
        open.primary = true;
        out.push_back({tr(L"Actions"),
                       {open,
                        button_row(tr(L"Desktop shortcut"), tr(L"Put a shortcut to the app on the desktop."), tr(L"Create"),
                                   [a] {
                                       make_app_shortcuts(a, true);
                                       set_status(fmt(L"A shortcut to {0} is on the desktop.", {a.label}), kSuccess);
                                   }),
                        button_row(tr(L"Update from APK"), tr(L"Install a newer version over this one. Saves stay."),
                                   tr(L"Choose…"),
                                   [a] {
                                       update_from_apk(a);
                                       if (is_pending(a.name)) close_sheet();
                                   }),
                        button_row(tr(L"Reinstall"), tr(L"Convert the app again from its APK."), tr(L"Reinstall"),
                                   [a] {
                                       close_sheet();
                                       install(a.apk, false, a.name);
                                   }),
                        button_row(tr(L"Remove"), tr(L"Its shortcuts go too. What the app saved stays."), tr(L"Remove…"),
                                   [a] {
                                       if (remove_app(a, true)) close_sheet();
                                   },
                                   true)},
                       L""});
        return out;
    };
    run_sheet(std::move(s));
}

// ---------------------------------------------------------------------------
// Shared by the install and log windows

Color code_bg() { return g_t.dark ? hex(0x141418) : hex(0xFAFAFC); }

// The end of a log, read while the runtime may still be writing it.
std::string read_tail(const fs::path& p, size_t max_bytes) {
    HANDLE f = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER size{};
    GetFileSizeEx(f, &size);
    LARGE_INTEGER start{};
    start.QuadPart = (std::max)(0LL, static_cast<long long>(size.QuadPart) - static_cast<long long>(max_bytes));
    SetFilePointerEx(f, start, nullptr, FILE_BEGIN);
    std::string out(static_cast<size_t>(size.QuadPart - start.QuadPart), '\0');
    DWORD got = 0;
    if (!out.empty()) ReadFile(f, &out[0], static_cast<DWORD>(out.size()), &got, nullptr);
    out.resize(got);
    CloseHandle(f);
    if (start.QuadPart > 0) out.erase(0, out.find('\n') == std::string::npos ? 0 : out.find('\n') + 1);
    return out;
}

std::vector<std::wstring> last_lines(const fs::path& p, size_t n) {
    std::vector<std::wstring> lines(1);
    for (wchar_t c : utf8_to_wide(read_tail(p, 16384))) {
        if (c == L'\n') lines.emplace_back();
        else if (c != L'\r') lines.back() += c == L'\t' ? L' ' : c;
    }
    while (!lines.empty() && lines.back().empty()) lines.pop_back();
    if (lines.size() > n) lines.erase(lines.begin(), lines.end() - static_cast<long>(n));
    return lines;
}

std::wstring duration(ULONGLONG ms) {
    const ULONGLONG s = ms / 1000;
    wchar_t buf[32];
    swprintf(buf, 32, L"%llu:%02llu", s / 60, s % 60);
    return buf;
}

using Targets = std::vector<std::pair<RectF, int>>;

int find_target(const Targets& targets, LPARAM lp) {
    const float x = static_cast<float>(GET_X_LPARAM(lp)), y = static_cast<float>(GET_Y_LPARAM(lp));
    for (const auto& t : targets) {
        if (contains(t.first, x, y)) return t.second;
    }
    return 0;
}

// Hover tracking for the custom-drawn buttons of a window; true when it changed.
bool track_hot(HWND hwnd, LPARAM lp, const Targets& targets, int* hot, bool* tracking) {
    if (!*tracking) {
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
        *tracking = TrackMouseEvent(&tme);
    }
    const int now = find_target(targets, lp);
    if (now == *hot) return false;
    *hot = now;
    InvalidateRect(hwnd, nullptr, FALSE);
    return true;
}

HWND open_tool_window(const wchar_t* cls, const wchar_t* title, int w, int h) {
    RECT mr;
    GetWindowRect(g_main, &mr);
    const int x = mr.left + (mr.right - mr.left - w) / 2, y = mr.top + (std::max)(0L, (mr.bottom - mr.top - h) / 2);
    HWND hwnd = CreateWindowExW(layout_style(), cls, title, WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX, x, y, w, h, g_main, nullptr,
                                g_inst, nullptr);
    if (hwnd) {
        theme_window(hwnd);
        ShowWindow(hwnd, SW_SHOW);
    }
    return hwnd;
}

// ---------------------------------------------------------------------------
// The install window: the queue, each app's progress, and its log as it is written

struct InstallView {
    Targets targets;
    int hot = 0;
    bool tracking = false;
    std::vector<std::wstring> log;
    fs::path log_path;
    ULONGLONG log_read = 0;
} g_iv;

enum { kActClose = 1, kActLog = 2, kActOpen = 1000, kActJobLog = 2000 };

// The install being converted, else the one that finished last.
const Job* log_job() {
    for (const Job& j : g_jobs) {
        if (j.state == Job::kInstalling) return &j;
    }
    for (auto it = g_jobs.rbegin(); it != g_jobs.rend(); ++it) {
        if (it->state != Job::kWaiting) return &*it;
    }
    return nullptr;
}

void paint_install(Graphics& g, float W, float H) {
    g.Clear(g_t.bg);
    g_iv.targets.clear();
    const float m = px(24), foot = px(64), cw = W - 2 * m;
    int done = 0, failed = 0;
    for (const Job& j : g_jobs) done += j.state == Job::kDone, failed += j.state == Job::kFailed;
    const int total = static_cast<int>(g_jobs.size());
    const bool finished = g_pending.empty();
    const ULONGLONG now = GetTickCount64();

    std::wstring title, sub;
    if (!finished) {
        title = total > 1 ? fmt(L"Installing {0} of {1}",
                                {std::to_wstring((std::min)(done + failed + 1, total)), std::to_wstring(total)})
                          : std::wstring(tr(L"Installing"));
        sub = tr(L"Each app is converted for Windows once. That takes up to a minute.");
    } else if (failed == 0) {
        const App* app = total == 1 ? find_app(g_jobs[0].name) : nullptr;
        title = app ? fmt(L"{0} is ready", {app->label}) : std::wstring(tr(L"All set"));
        sub = fmt(done == 1 ? L"{0} app is in the library and in Start > Win.apk Apps."
                            : L"{0} apps are in the library and in Start > Win.apk Apps.",
                  {std::to_wstring(done)});
    } else {
        title = failed == total ? tr(L"The install failed") : tr(L"Some installs failed");
        sub = fmt(L"{0} of {1} could not be installed. The log says why.",
                  {std::to_wstring(failed), std::to_wstring(total)});
    }
    float y = px(22);
    const RectF badge(m, y, px(44), px(44));
    if (!finished) {
        fill_round(g, badge, px(22), with_alpha(g_t.accent, g_t.dark ? 50 : 28));
        spinner(g, inset(badge, px(10), px(10)), g_t.accent, px(3));
    } else {
        Gdiplus::SolidBrush b(failed ? g_t.danger : g_t.success);
        g.FillEllipse(&b, badge);
        draw_text(g, failed ? L"" : kGlyphCheck, g_f.glyph16.get(), kWhite, badge, kCenter);
    }
    draw_text(g, title, g_f.hero.get(), g_t.text, RectF(m + px(60), y - px(2), cw - px(60), px(28)));
    draw_text(g, sub, g_f.caption.get(), g_t.text2, RectF(m + px(60), y + px(26), cw - px(60), px(20)));
    y += px(70);

    const float rh = px(60);
    const int rows = (std::max)(1, (std::min)(total, static_cast<int>((H * 0.45f) / rh)));
    const RectF card(m, y, cw, rows * rh);
    fill_round(g, card, px(10), g_t.card);
    stroke_round(g, card, px(10), g_t.border);
    g.SetClip(card);
    const int first = (std::max)(0, (std::min)(total - rows, [&] {  // keep the active install in view
        for (int i = 0; i < total; ++i) {
            if (g_jobs[static_cast<size_t>(i)].state == Job::kInstalling) return i - rows + 2;
        }
        return total - rows;
    }()));
    for (int i = first; i < total && i < first + rows; ++i) {
        const Job& j = g_jobs[static_cast<size_t>(i)];
        const RectF r(m, y + (i - first) * rh, cw, rh);
        if (i > first) {
            Gdiplus::Pen line(g_t.border, 1);
            g.DrawLine(&line, r.X + px(66), r.Y, r.GetRight() - px(14), r.Y);
        }
        const App* app = find_app(j.name);
        const std::wstring label = app ? app->label : j.name;
        draw_app_icon(g, app, label, RectF(r.X + px(14), r.Y + px(10), px(40), px(40)));
        std::wstring state;
        switch (j.state) {
            case Job::kWaiting: state = tr(L"Waiting"); break;
            case Job::kInstalling: state = fmt(L"Converting  ·  {0}", {duration(now - j.start)}); break;
            case Job::kDone: state = fmt(L"Installed in {0}", {duration(j.end - j.start)}); break;
            case Job::kFailed: state = fmt(L"Failed after {0}", {duration(j.end - j.start)}); break;
        }
        const float tx = r.X + px(66), tw = r.Width - px(66) - px(120);
        draw_text(g, label, g_f.body_semi.get(), g_t.text, RectF(tx, r.Y + px(10), tw, px(20)));
        draw_text(g, state, g_f.caption.get(), j.state == Job::kFailed ? g_t.danger : g_t.text2,
                  RectF(tx, r.Y + px(31), tw, px(18)));
        const RectF right(r.GetRight() - px(14) - px(96), r.Y + (rh - px(30)) / 2, px(96), px(30));
        const int act = j.state == Job::kDone ? kActOpen + i : j.state == Job::kFailed ? kActJobLog + i : 0;
        if (j.state == Job::kWaiting) {
            draw_text(g, kGlyphClock, g_f.glyph16.get(), g_t.text2, right, kRight);
        } else if (j.state == Job::kInstalling) {
            spinner(g, RectF(right.GetRight() - px(20), right.Y + px(5), px(20), px(20)), g_t.accent, px(2.5f));
        } else {
            draw_button(g, right, j.state == Job::kDone ? tr(L"Open") : tr(L"View log"), g_iv.hot == act, false,
                        j.state == Job::kFailed);
            g_iv.targets.push_back({right, act});
        }
    }
    g.ResetClip();
    y += rows * rh + px(20);

    const Job* lj = log_job();
    draw_text(g, lj ? fmt(L"Log  ·  {0}", {lj->name}) : std::wstring(tr(L"Log")), g_f.caption_semi.get(), g_t.text2,
              RectF(m + px(4), y, cw, px(18)));
    y += px(24);
    const RectF box(m, y, cw, H - foot - px(18) - y);
    if (box.Height > px(48)) {
        fill_round(g, box, px(10), code_bg());
        stroke_round(g, box, px(10), g_t.border);
        const float lh = px(17);
        const size_t fit = static_cast<size_t>((std::max)(0.0f, (box.Height - px(20)) / lh));
        if (g_iv.log.empty()) {
            draw_text(g, lj ? tr(L"Starting the runtime…") : tr(L"The install's output appears here."), g_f.caption.get(),
                      g_t.text2, box, kCenter);
        } else {
            const size_t from = g_iv.log.size() > fit ? g_iv.log.size() - fit : 0;
            float ly = box.Y + px(10);
            for (size_t i = from; i < g_iv.log.size(); ++i, ly += lh) {
                const std::wstring& l = g_iv.log[i];
                const bool bad = l.find(L"rror") != std::wstring::npos || l.find(L"xception") != std::wstring::npos;
                draw_text(g, l, g_f.mono.get(), bad ? g_t.danger : g_t.text2,
                          RectF(box.X + px(12), ly, box.Width - px(24), lh), kLeft, false, true, true);
            }
        }
    }

    Gdiplus::SolidBrush bar(g_t.dark ? mix(g_t.bg, kBlack, 0.15f) : mix(g_t.bg, kWhite, 0.5f));
    g.FillRectangle(&bar, RectF(0, H - foot, W, foot));
    Gdiplus::Pen line(g_t.border, 1);
    g.DrawLine(&line, 0.0f, H - foot, W, H - foot);
    const RectF close(W - m - px(100), H - foot + (foot - px(34)) / 2, px(100), px(34));
    draw_button(g, close, finished ? tr(L"Done") : tr(L"Hide"), g_iv.hot == kActClose, true, false);
    g_iv.targets.push_back({close, kActClose});
    if (lj) {
        const RectF full(m, close.Y, px(140), px(34));
        draw_button(g, full, tr(L"Open full log"), g_iv.hot == kActLog, false, false);
        g_iv.targets.push_back({full, kActLog});
    }
}

void install_action(HWND hwnd, int act) {
    if (act == kActClose) {
        DestroyWindow(hwnd);
    } else if (act == kActLog) {
        if (const Job* j = log_job()) show_logs(install_log(j->name));
    } else if (act >= kActJobLog) {
        const size_t i = static_cast<size_t>(act - kActJobLog);
        if (i < g_jobs.size()) show_logs(install_log(g_jobs[i].name));
    } else if (act >= kActOpen) {
        const size_t i = static_cast<size_t>(act - kActOpen);
        if (i < g_jobs.size()) {
            if (const App* a = find_app(g_jobs[i].name)) launch(*a);
        }
    }
}

LRESULT CALLBACK install_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: SetTimer(hwnd, 1, 33, nullptr); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: buffered_paint(hwnd, paint_install); return 0;
        case WM_TIMER: {
            const Job* j = log_job();
            if (j && GetTickCount64() - g_iv.log_read > 400) {
                g_iv.log = last_lines(install_log(j->name), 80);
                g_iv.log_read = GetTickCount64();
            } else if (!j) {
                g_iv.log.clear();
            }
            if (!g_pending.empty() || g_iv.log_read + 1000 > GetTickCount64()) InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_MOUSEMOVE: track_hot(hwnd, lp, g_iv.targets, &g_iv.hot, &g_iv.tracking); return 0;
        case WM_MOUSELEAVE:
            g_iv.tracking = false;
            g_iv.hot = 0;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                SetCursor(LoadCursorW(nullptr, g_iv.hot ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_LBUTTONUP: install_action(hwnd, find_target(g_iv.targets, lp)); return 0;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE || wp == VK_RETURN) DestroyWindow(hwnd);
            return 0;
        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {ipx(420), ipx(440)};
            return 0;
        case WM_DESTROY:
            g_install_wnd = nullptr;
            g_iv = InstallView{};
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void show_install_window() {
    if (g_install_wnd) {
        InvalidateRect(g_install_wnd, nullptr, FALSE);
        return;
    }
    g_install_wnd = open_tool_window(L"WinApkInstall", tr(L"Installing - Win.apk"), ipx(540), ipx(600));
}

// ---------------------------------------------------------------------------
// The log window: any of the logs, following the end as the runtime writes it

struct LogView {
    fs::path path;
    uintmax_t size = static_cast<uintmax_t>(-1);
    Targets targets;
    int hot = 0;
    bool tracking = false;
} g_lv;

enum { kLogPick = 1, kLogCopy, kLogNotepad, kLogFolder };

std::wstring log_title(const fs::path& p) {
    const std::wstring stem = p.stem().wstring();
    if (stem == L"last-run") return tr(L"Last run");
    if (stem.rfind(L"install-", 0) == 0) return fmt(L"Install  ·  {0}", {stem.substr(8)});
    return stem;
}

std::vector<fs::path> log_files() {
    std::vector<std::pair<fs::file_time_type, fs::path>> found;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(logs_dir(), ec)) {
        if (e.path().extension() == L".log") found.push_back({e.last_write_time(ec), e.path()});
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<fs::path> out;
    for (auto& f : found) out.push_back(std::move(f.second));
    return out;
}

void load_log(bool force) {
    std::error_code ec;
    const uintmax_t size = fs::file_size(g_lv.path, ec);
    if (!force && (ec ? 0 : size) == g_lv.size) return;
    g_lv.size = ec ? 0 : size;
    std::wstring text;
    if (ec) {
        text = g_lv.path.stem() == L"last-run" ? tr(L"No app has run yet. This log is written when one does.")
                                               : tr(L"This log is written the next time the app is installed.");
    } else {
        const std::wstring raw = utf8_to_wide(read_tail(g_lv.path, 1 << 20));
        text.reserve(raw.size() + raw.size() / 32);
        for (wchar_t c : raw) {
            if (c == L'\n') text += L"\r\n";
            else if (c != L'\r') text += c;
        }
    }
    SCROLLINFO si{sizeof(si), SIF_ALL};
    GetScrollInfo(g_log_edit, SB_VERT, &si);
    const bool at_end = force || si.nPos + static_cast<int>(si.nPage) >= si.nMax;
    const LRESULT first = SendMessageW(g_log_edit, EM_GETFIRSTVISIBLELINE, 0, 0);
    SetWindowTextW(g_log_edit, text.c_str());
    if (at_end) {
        const int len = GetWindowTextLengthW(g_log_edit);
        SendMessageW(g_log_edit, EM_SETSEL, len, len);
        SendMessageW(g_log_edit, EM_SCROLLCARET, 0, 0);
        SendMessageW(g_log_edit, WM_HSCROLL, SB_LEFT, 0);  // the caret's scroll may have moved sideways too
    } else {
        SendMessageW(g_log_edit, EM_LINESCROLL, 0, first);
    }
    InvalidateRect(g_log_wnd, nullptr, FALSE);
}

RectF log_text_rect(float W, float H) { return RectF(px(20), px(64), W - px(40), H - px(64) - px(40)); }

void paint_logs(Graphics& g, float W, float H) {
    g.Clear(g_t.bg);
    g_lv.targets.clear();
    const float m = px(20);
    draw_text(g, tr(L"Logs"), g_f.hero.get(), g_t.text, RectF(m, px(14), px(70), px(36)));
    const RectF pick(m + px(76), px(16), (std::min)(px(300), W * 0.4f), px(32));
    fill_round(g, pick, px(6), g_lv.hot == kLogPick ? g_t.hover : mix(g_t.card, g_t.hover, 0.55f));
    stroke_round(g, pick, px(6), g_t.border);
    draw_text(g, log_title(g_lv.path), g_f.body.get(), g_t.text, RectF(pick.X + px(12), pick.Y, pick.Width - px(40), pick.Height));
    draw_text(g, kGlyphChevron, g_f.glyph12.get(), g_t.text2, RectF(pick.GetRight() - px(30), pick.Y, px(22), pick.Height),
              kCenter);
    g_lv.targets.push_back({pick, kLogPick});
    float x = W - m;
    for (auto [label, act] : {std::pair<const wchar_t*, int>{tr(L"Folder"), kLogFolder}, {tr(L"Notepad"), kLogNotepad},
                              {tr(L"Copy"), kLogCopy}}) {
        const float w = text_width(g, label, g_f.body_semi.get()) + px(32);
        x -= w;
        const RectF r(x, px(16), w, px(32));
        if (r.X > pick.GetRight() + px(8)) {
            draw_button(g, r, label, g_lv.hot == act, false, false);
            g_lv.targets.push_back({r, act});
        }
        x -= px(8);
    }
    const RectF box = log_text_rect(W, H);
    fill_round(g, box, px(10), code_bg());
    stroke_round(g, box, px(10), g_t.border);
    const bool live = !g_running.empty() || !g_pending.empty();
    float fx = m;
    if (live) {
        Gdiplus::SolidBrush dot(g_t.success);
        g.FillEllipse(&dot, fx, H - px(20) - px(3), px(6), px(6));
        fx += px(14);
    }
    const std::wstring info = (live ? std::wstring(tr(L"Live")) + L"  ·  " : std::wstring()) + format_bytes(g_lv.size) + L"  ·  " + g_lv.path.wstring();
    draw_text(g, info, g_f.caption.get(), g_t.text2, RectF(fx, H - px(40), W - fx - m, px(40)));
}

void log_action(HWND hwnd, int act) {
    if (act == kLogPick) {
        const auto files = log_files();
        if (files.empty()) return;
        HMENU menu = CreatePopupMenu();
        for (size_t i = 0; i < files.size() && i < 40; ++i) {
            AppendMenuW(menu, MF_STRING | (files[i] == g_lv.path ? MF_CHECKED : 0), i + 1, log_title(files[i]).c_str());
        }
        const RectF& r = g_lv.targets[0].first;
        POINT pt{static_cast<LONG>(r.X), static_cast<LONG>(r.GetBottom() + px(2))};
        ClientToScreen(hwnd, &pt);
        const int pick = TrackPopupMenu(menu, TPM_RETURNCMD | menu_flags(true), pt.x, pt.y, 0, hwnd, nullptr);
        DestroyMenu(menu);
        if (pick > 0) {
            g_lv.path = files[static_cast<size_t>(pick - 1)];
            load_log(true);
        }
    } else if (act == kLogCopy) {
        const int len = GetWindowTextLengthW(g_log_edit);
        HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (static_cast<size_t>(len) + 1) * sizeof(wchar_t));
        if (!mem || !OpenClipboard(hwnd)) {
            if (mem) GlobalFree(mem);
            return;
        }
        GetWindowTextW(g_log_edit, static_cast<wchar_t*>(GlobalLock(mem)), len + 1);
        GlobalUnlock(mem);
        EmptyClipboard();
        if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
        CloseClipboard();
        set_status(tr(L"The log was copied."), kSuccess);
    } else if (act == kLogNotepad) {
        open_log(g_lv.path);
    } else if (act == kLogFolder) {
        std::error_code ec;
        if (fs::exists(g_lv.path, ec)) show_in_explorer(g_lv.path);
        else open_path(logs_dir());
    }
}

LRESULT CALLBACK log_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE:
            g_log_edit = CreateWindowExW(0, L"EDIT", L"",
                                         WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY |
                                             ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_NOHIDESEL,
                                         0, 0, 0, 0, hwnd, nullptr, g_inst, nullptr);
            SendMessageW(g_log_edit, WM_SETFONT, reinterpret_cast<WPARAM>(g_mono_font), FALSE);
            SendMessageW(g_log_edit, EM_SETLIMITTEXT, 0, 0);
            SetWindowTheme(g_log_edit, g_t.dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
            SetTimer(hwnd, 1, 700, nullptr);
            return 0;
        case WM_SIZE: {
            const RectF r = inset(log_text_rect(LOWORD(lp), HIWORD(lp)), px(10), px(8));
            MoveWindow(g_log_edit, static_cast<int>(r.X), static_cast<int>(r.Y), static_cast<int>(r.Width),
                       static_cast<int>(r.Height), TRUE);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: buffered_paint(hwnd, paint_logs); return 0;
        case WM_CTLCOLORSTATIC:  // a read-only edit asks with this one
            SetTextColor(reinterpret_cast<HDC>(wp), colorref(g_t.text));
            SetBkColor(reinterpret_cast<HDC>(wp), colorref(code_bg()));
            return reinterpret_cast<LRESULT>(g_code_brush);
        case WM_TIMER: load_log(false); return 0;
        case WM_MOUSEMOVE: track_hot(hwnd, lp, g_lv.targets, &g_lv.hot, &g_lv.tracking); return 0;
        case WM_MOUSELEAVE:
            g_lv.tracking = false;
            g_lv.hot = 0;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT && reinterpret_cast<HWND>(wp) == hwnd) {
                SetCursor(LoadCursorW(nullptr, g_lv.hot ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_LBUTTONUP: log_action(hwnd, find_target(g_lv.targets, lp)); return 0;
        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {ipx(560), ipx(360)};
            return 0;
        case WM_DESTROY:
            g_log_wnd = g_log_edit = nullptr;
            g_lv.targets.clear();
            g_lv.hot = 0;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void show_logs(const fs::path& log) {
    g_lv.path = log;
    if (!g_log_wnd) {
        g_log_wnd = open_tool_window(L"WinApkLogs", tr(L"Logs - Win.apk"), ipx(860), ipx(620));
        if (!g_log_wnd) return;
    } else {
        ShowWindow(g_log_wnd, SW_RESTORE);
        SetForegroundWindow(g_log_wnd);
    }
    load_log(true);
}

// ---------------------------------------------------------------------------
// Drag and drop, through OLE so the drop zone can light up while dragging

std::vector<fs::path> dropped_files(IDataObject* data) {
    std::vector<fs::path> out;
    FORMATETC fe{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM m{};
    if (FAILED(data->GetData(&fe, &m))) return out;
    if (auto drop = static_cast<HDROP>(GlobalLock(m.hGlobal))) {
        const UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n; ++i) {
            wchar_t path[MAX_PATH * 4];
            if (DragQueryFileW(drop, i, path, MAX_PATH * 4) && is_app_file(path)) out.emplace_back(path);
        }
        GlobalUnlock(m.hGlobal);
    }
    ReleaseStgMedium(&m);
    return out;
}

class DropTarget : public IDropTarget {
public:
    virtual ~DropTarget() = default;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (riid == IID_IUnknown || riid == IID_IDropTarget) {
            *out = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG n = InterlockedDecrement(&refs_);
        if (n == 0) delete this;
        return static_cast<ULONG>(n);
    }
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD, POINTL, DWORD* effect) override {
        accept_ = !g_sheet && !dropped_files(data).empty();
        set_over(accept_);
        *effect = accept_ ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD* effect) override {
        *effect = accept_ ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override {
        set_over(false);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD, POINTL, DWORD* effect) override {
        set_over(false);
        *effect = accept_ ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        if (accept_) {
            for (const fs::path& p : dropped_files(data)) install(p, false);
        }
        return S_OK;
    }

private:
    static void set_over(bool over) {
        g_drag_over = over;
        InvalidateRect(g_main, nullptr, FALSE);
    }
    LONG refs_ = 1;
    bool accept_ = false;
};

DropTarget* g_drop_target;

// ---------------------------------------------------------------------------
// The library window

void apply_theme() {
    pick_theme();
    theme_window(g_main);
    if (sheet_hwnd()) {
        theme_window(sheet_hwnd());
        InvalidateRect(sheet_hwnd(), nullptr, FALSE);
    }
    theme_menus();
    if (g_edit_brush) DeleteObject(g_edit_brush);
    g_edit_brush = CreateSolidBrush(colorref(g_t.card));
    if (g_code_brush) DeleteObject(g_code_brush);
    g_code_brush = CreateSolidBrush(colorref(code_bg()));
    for (HWND w : {g_install_wnd, g_log_wnd}) {
        if (!w) continue;
        theme_window(w);
        InvalidateRect(w, nullptr, FALSE);
    }
    if (g_log_edit) {
        SetWindowTheme(g_log_edit, g_t.dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
        InvalidateRect(g_log_edit, nullptr, TRUE);
    }
    InvalidateRect(g_main, nullptr, FALSE);
    InvalidateRect(g_search, nullptr, TRUE);
}

void show_context_menu(const App& app, POINT pt) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kCmdOpen, tr(L"&Open"));
    AppendMenuW(menu, MF_STRING, kCmdInfo, tr(L"Get &Info…"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdShortcut, tr(L"Create &desktop shortcut"));
    AppendMenuW(menu, MF_STRING, kCmdShowApk, tr(L"Show the &APK in Explorer"));
    AppendMenuW(menu, MF_STRING, kCmdAppFolder, tr(L"Open the app's &files"));
    AppendMenuW(menu, MF_STRING, kCmdLog, tr(L"Show the last &log"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdUpdate, tr(L"&Update from APK…"));
    AppendMenuW(menu, MF_STRING, kCmdReinstall, tr(L"Rein&stall"));
    AppendMenuW(menu, MF_STRING, kCmdRemove, tr(L"&Remove…"));
    SetMenuDefaultItem(menu, kCmdOpen, FALSE);
    if (is_pending(app.name)) {
        for (int id : {kCmdOpen, kCmdUpdate, kCmdReinstall, kCmdRemove}) EnableMenuItem(menu, id, MF_GRAYED);
    }
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | menu_flags(false), pt.x, pt.y, 0, g_main, nullptr);
    DestroyMenu(menu);
}

void command(int id) {
    const App* sel = selected();
    if (!sel) return;
    const App a = *sel;
    switch (id) {
        case kCmdOpen: launch(a); break;
        case kCmdInfo: show_info(a); break;
        case kCmdShortcut:
            make_app_shortcuts(a, true);
            set_status(fmt(L"A shortcut to {0} is on the desktop.", {a.label}), kSuccess);
            break;
        case kCmdShowApk: show_in_explorer(a.apk); break;
        case kCmdAppFolder: open_path(app_dir(a.name)); break;
        case kCmdLog: show_logs(last_run_log()); break;
        case kCmdUpdate: update_from_apk(a); break;
        case kCmdReinstall: install(a.apk, false, a.name); break;
        case kCmdRemove: remove_app(a, true); break;
    }
}

void move_selection(int dx, int dy) {
    int i = tile_of(g_selected);
    if (i < 0) {
        for (size_t t = 0; t < g_tiles.size() && i < 0; ++t) {
            if (g_tiles[t].app >= 0) i = static_cast<int>(t);
        }
    } else {
        i += dx + dy * g_l.cols;
    }
    if (i < 0 || i >= static_cast<int>(g_tiles.size()) || g_tiles[static_cast<size_t>(i)].app < 0) return;
    select_app(g_apps[static_cast<size_t>(g_tiles[static_cast<size_t>(i)].app)].name);
}

LRESULT CALLBACK search_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) {
        SetWindowTextW(hwnd, L"");
        SetFocus(g_main);
        return 0;
    }
    if (msg == WM_KEYDOWN && (wp == VK_DOWN || wp == VK_RETURN)) {
        SetFocus(g_main);
        if (!selected()) move_selection(0, 0);
        return 0;
    }
    if (msg == WM_CHAR && (wp == VK_RETURN || wp == VK_ESCAPE)) return 0;  // no beep
    return DefSubclassProc(hwnd, msg, wp, lp);
}

// The search box, made again when the language changes so it takes the new direction.
void create_search() {
    wchar_t text[256] = {};
    if (g_search) {
        GetWindowTextW(g_search, text, 256);
        DestroyWindow(g_search);
    }
    g_search = CreateWindowExW(g_rtl ? WS_EX_RTLREADING : 0, L"EDIT", text, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                               0, 0, 0, 0, g_main, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdSearch)), g_inst,
                               nullptr);
    SendMessageW(g_search, WM_SETFONT, reinterpret_cast<WPARAM>(g_edit_font), FALSE);
    SendMessageW(g_search, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(tr(L"Search apps")));
    SetWindowSubclass(g_search, search_proc, 1, 0);
}

void apply_language() {
    pick_language();
    for (HWND w : {g_main, sheet_hwnd(), g_install_wnd}) {
        if (!w) continue;
        const LONG_PTR ex = GetWindowLongPtrW(w, GWL_EXSTYLE);
        SetWindowLongPtrW(w, GWL_EXSTYLE, g_rtl ? (ex | WS_EX_LAYOUTRTL) : (ex & ~WS_EX_LAYOUTRTL));
        SetWindowPos(w, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        InvalidateRect(w, nullptr, FALSE);
    }
    if (sheet_hwnd()) SetWindowTextW(sheet_hwnd(), tr(L"Settings"));  // the language is picked there
    if (g_install_wnd) SetWindowTextW(g_install_wnd, tr(L"Installing - Win.apk"));
    if (g_log_wnd) DestroyWindow(g_log_wnd);
    create_search();
    rebuild_tiles();
}

void set_hot(const Hit& h) {
    if (h == g_hot) return;
    g_hot = h;
    InvalidateRect(g_main, nullptr, FALSE);
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            g_main = hwnd;
            g_dpi = static_cast<float>(GetDpiForWindow(hwnd));
            make_fonts();
            create_search();
            apply_theme();
            g_drop_target = new DropTarget;
            RegisterDragDrop(hwnd, g_drop_target);
            refresh();
            return 0;
        }
        case WM_SIZE:
            compute_layout();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_DPICHANGED: {
            g_dpi = HIWORD(wp);
            make_fonts();
            SendMessageW(g_search, WM_SETFONT, reinterpret_cast<WPARAM>(g_edit_font), FALSE);
            g_icons.clear();
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            compute_layout();
            return 0;
        }
        case WM_SETTINGCHANGE:
            if (lp && wcscmp(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorSet") == 0) apply_theme();
            break;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: buffered_paint(hwnd, paint_main); return 0;
        case WM_TIMER:
            if (wp == kAnimTimer) InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_CTLCOLOREDIT:
            SetTextColor(reinterpret_cast<HDC>(wp), colorref(g_t.text));
            SetBkColor(reinterpret_cast<HDC>(wp), colorref(g_t.card));
            return reinterpret_cast<LRESULT>(g_edit_brush);
        case WM_COMMAND:
            if (LOWORD(wp) == kIdSearch) {
                if (HIWORD(wp) == EN_CHANGE) {
                    g_scroll = 0;
                    rebuild_tiles();
                } else if (HIWORD(wp) == EN_SETFOCUS || HIWORD(wp) == EN_KILLFOCUS) {
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }
            command(LOWORD(wp));
            return 0;
        case WM_MOUSEMOVE: {
            const float x = static_cast<float>(GET_X_LPARAM(lp)), y = static_cast<float>(GET_Y_LPARAM(lp));
            if (!g_tracking) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
                g_tracking = TrackMouseEvent(&tme);
            }
            if (g_thumb_drag) {
                const float room = g_l.grid.Height - thumb_rect().Height;
                if (room > 0) g_scroll = g_thumb_scroll0 + (y - g_thumb_y0) * g_l.max_scroll / room;
                clamp_scroll();
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            set_hot(hit_test(x, y));
            return 0;
        }
        case WM_MOUSELEAVE:
            g_tracking = false;
            if (!g_thumb_drag) set_hot({});
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT && reinterpret_cast<HWND>(wp) == hwnd) {
                const bool hand = g_hot.kind == kHitGear || g_hot.kind == kHitDrop || g_hot.kind == kHitLogs ||
                                  g_hot.kind == kHitStop ||
                                  (g_hot.kind == kHitTile && g_tiles[static_cast<size_t>(g_hot.tile)].app == kAddTile);
                SetCursor(LoadCursorW(nullptr, hand ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_LBUTTONDOWN: {
            SetFocus(hwnd);
            const float x = static_cast<float>(GET_X_LPARAM(lp)), y = static_cast<float>(GET_Y_LPARAM(lp));
            const Hit h = hit_test(x, y);
            switch (h.kind) {
                case kHitGear: show_settings(); break;
                case kHitLogs: {
                    const auto logs = log_files();
                    show_logs(logs.empty() ? last_run_log() : logs.front());
                    break;
                }
                case kHitStop:
                    if (const App* a = running_app()) {
                        if (stop_process(a->name)) g_stopping.insert(a->name);
                    }
                    break;
                case kHitDrop: add_from_dialog(); break;
                case kHitThumb:
                    g_thumb_drag = true;
                    g_thumb_y0 = y;
                    g_thumb_scroll0 = g_scroll;
                    SetCapture(hwnd);
                    break;
                case kHitTrack:
                    g_scroll += (y < thumb_rect().Y ? -1 : 1) * g_l.grid.Height * 0.9f;
                    clamp_scroll();
                    InvalidateRect(hwnd, nullptr, FALSE);
                    break;
                case kHitTile: {
                    const Tile& t = g_tiles[static_cast<size_t>(h.tile)];
                    if (t.app >= 0) select_app(g_apps[static_cast<size_t>(t.app)].name);
                    else if (t.app == kAddTile) add_from_dialog();
                    break;
                }
                case kHitNone:
                    g_selected.clear();
                    InvalidateRect(hwnd, nullptr, FALSE);
                    break;
            }
            return 0;
        }
        case WM_LBUTTONUP:
            if (g_thumb_drag) {
                g_thumb_drag = false;
                ReleaseCapture();
                set_hot(hit_test(static_cast<float>(GET_X_LPARAM(lp)), static_cast<float>(GET_Y_LPARAM(lp))));
            }
            return 0;
        case WM_LBUTTONDBLCLK: {
            const Hit h = hit_test(static_cast<float>(GET_X_LPARAM(lp)), static_cast<float>(GET_Y_LPARAM(lp)));
            if (h.kind == kHitTile && g_tiles[static_cast<size_t>(h.tile)].app >= 0) {
                launch(g_apps[static_cast<size_t>(g_tiles[static_cast<size_t>(h.tile)].app)]);
            } else if (h.kind == kHitDrop) {
                add_from_dialog();
            }
            return 0;
        }
        case WM_RBUTTONUP: {
            const Hit h = hit_test(static_cast<float>(GET_X_LPARAM(lp)), static_cast<float>(GET_Y_LPARAM(lp)));
            if (h.kind != kHitTile || g_tiles[static_cast<size_t>(h.tile)].app < 0) return 0;
            const App& app = g_apps[static_cast<size_t>(g_tiles[static_cast<size_t>(h.tile)].app)];
            select_app(app.name);
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ClientToScreen(hwnd, &pt);
            show_context_menu(app, pt);
            return 0;
        }
        case WM_CONTEXTMENU:
            if (GET_X_LPARAM(lp) == -1 && selected()) {  // the keyboard's menu key
                const RectF r = tile_rect(tile_of(g_selected));
                POINT pt{static_cast<LONG>(r.X + r.Width / 2), static_cast<LONG>(r.Y + r.Height / 2)};
                ClientToScreen(hwnd, &pt);
                show_context_menu(*selected(), pt);
                return 0;
            }
            break;
        case WM_MOUSEWHEEL:
            g_scroll -= GET_WHEEL_DELTA_WPARAM(wp) / 120.0f * px(60);
            clamp_scroll();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_KEYDOWN: {
            const bool ctrl = GetKeyState(VK_CONTROL) < 0;
            switch (wp) {
                case VK_LEFT: move_selection(-1, 0); return 0;
                case VK_RIGHT: move_selection(1, 0); return 0;
                case VK_UP: move_selection(0, -1); return 0;
                case VK_DOWN: move_selection(0, 1); return 0;
                case VK_RETURN: if (const App* a = selected()) launch(*a); return 0;
                case VK_DELETE: if (const App* a = selected()) remove_app(*a, true); return 0;
                case VK_F5: refresh(); return 0;
                case VK_ESCAPE:
                    g_selected.clear();
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                case 'F': if (ctrl) SetFocus(g_search); return 0;
                case 'O': if (ctrl) add_from_dialog(); return 0;
                case 'I': if (ctrl && selected()) show_info(*selected()); return 0;
                case VK_OEM_COMMA: if (ctrl) show_settings(); return 0;
            }
            break;
        }
        case WM_CHAR:  // typing in the library searches it
            if (wp > 32 && GetKeyState(VK_CONTROL) >= 0) {
                SetFocus(g_search);
                SendMessageW(g_search, WM_CHAR, wp, lp);
            }
            return 0;
        case kInstallStarted: {
            std::unique_ptr<std::wstring> name(reinterpret_cast<std::wstring*>(lp));
            g_current = *name;
            for (Job& j : g_jobs) {
                if (j.name == *name && j.state == Job::kWaiting) {
                    j.state = Job::kInstalling;
                    j.start = GetTickCount64();
                    break;
                }
            }
            if (g_install_wnd) InvalidateRect(g_install_wnd, nullptr, FALSE);
            set_status(fmt(L"Installing {0}… The first install of an app takes up to a minute.", {*name}), kInfo);
            return 0;
        }
        case kInstalled: {
            std::unique_ptr<std::wstring> name(reinterpret_cast<std::wstring*>(lp));
            const auto it = std::find(g_pending.begin(), g_pending.end(), *name);
            if (it != g_pending.end()) g_pending.erase(it);
            ++g_batch_done;
            if (g_pending.empty()) g_current.clear();
            for (Job& j : g_jobs) {
                if (j.name == *name && j.state == Job::kInstalling) {
                    j.state = (wp & 1) ? Job::kDone : Job::kFailed;
                    j.end = GetTickCount64();
                    break;
                }
            }
            if (g_install_wnd) InvalidateRect(g_install_wnd, nullptr, FALSE);
            update_timer();
            refresh();
            if (wp & 1) {
                const std::wstring label = label_of(*name);
                select_app(*name);
                set_status(fmt(setting(L"DesktopShortcut", 1)
                                   ? L"{0} is installed: it is in the Start menu and on the desktop."
                                   : L"{0} is installed: it is in the Start menu.",
                               {label}),
                           kSuccess);
                if (wp & 2) {
                    if (const App* a = find_app(*name)) launch(*a);
                }
            } else {
                set_status(fmt(L"Installing {0} failed.", {*name}), kError);
                if (!g_install_wnd) failed_with_log(fmt(L"Installing {0} failed", {*name}), install_log(*name));
            }
            return 0;
        }
        case kRunExited: {
            std::unique_ptr<std::wstring> name(reinterpret_cast<std::wstring*>(lp));
            g_running.erase(*name);
            update_timer();
            const std::wstring label = label_of(*name);
            if (g_stopping.erase(*name)) {
                set_status(fmt(L"{0} was stopped.", {label}), kInfo);
            } else if (wp) {
                set_status(L"", kHint);
            } else {
                set_status(fmt(L"{0} stopped with an error.", {label}), kError);
                failed_with_log(fmt(L"{0} stopped with an error", {label}), last_run_log());
            }
            return 0;
        }
        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {ipx(560), ipx(520)};
            return 0;
        case WM_DESTROY:
            RevokeDragDrop(hwnd);
            if (g_drop_target) g_drop_target->Release();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    g_inst = inst;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    OleInitialize(nullptr);
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    // A shortcut: run the app, and only show anything if it fails.
    if (argc >= 3 && wcscmp(argv[1], L"--run") == 0) {
        if (!run_and_wait(argv[2])) {
            const std::wstring text = fmt(L"The app stopped with an error.\n\nThe log is in:\n{0}", {last_run_log().wstring()});
            MessageBoxW(nullptr, text.c_str(), L"Win.apk", MB_OK | MB_ICONWARNING);
            return 1;
        }
        return 0;
    }

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    ULONG_PTR gdiplus = 0;
    Gdiplus::GdiplusStartupInput gsi;
    Gdiplus::GdiplusStartup(&gdiplus, &gsi, nullptr);
    integrate_with_windows();
    pick_language();
    pick_theme();
    theme_menus();

    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"WinApkLibrary";
    RegisterClassExW(&wc);
    WNDCLASSEXW sc{sizeof(sc)};
    sc.lpfnWndProc = sheet_proc;
    sc.hInstance = inst;
    sc.hIcon = wc.hIcon;
    sc.hCursor = wc.hCursor;
    sc.lpszClassName = L"WinApkSheet";
    RegisterClassExW(&sc);
    sc.lpfnWndProc = install_proc;
    sc.lpszClassName = L"WinApkInstall";
    RegisterClassExW(&sc);
    sc.lpfnWndProc = log_proc;
    sc.lpszClassName = L"WinApkLogs";
    RegisterClassExW(&sc);
    const UINT dpi = GetDpiForSystem();
    HWND hwnd = CreateWindowExW(layout_style(), wc.lpszClassName, L"Win.apk", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                CW_USEDEFAULT, CW_USEDEFAULT, MulDiv(980, static_cast<int>(dpi), 96),
                                MulDiv(720, static_cast<int>(dpi), 96), nullptr, nullptr, inst, nullptr);
    ShowWindow(hwnd, show);

    // An .apk opened from Explorer, or dropped on the program: install and run.
    for (int i = 1; i < argc; ++i) {
        if (is_app_file(argv[i])) install(argv[i], true);
    }
    LocalFree(argv);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    g_icons.clear();
    g_f = Fonts{};
    Gdiplus::GdiplusShutdown(gdiplus);
    OleUninitialize();
    return 0;
}
