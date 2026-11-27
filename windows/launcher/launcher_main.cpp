// launcher_main.cpp -- Win.apk.exe, the app library.
//
// The Windows counterpart of upstream's launcher (launcher/*.swift), laid out
// as a Windows 11 app: a navigation pane (Library, Store, Google Play, and
// the Google account and Settings at its foot), the page's title and toolbar
// (Install app) over a grid of the installed
// apps with their icons, and an InfoBar for what just happened. Settings and
// an app's details are pages in the same content layer. The whole window
// takes a dropped APK. The windows are drawn with GDI+ and follow Windows'
// light or dark mode (or the one picked in the settings), in English or, laid
// out right to left, in Hebrew.
//
// An APK dropped on the window, chosen through Install app, double-clicked in
// Explorer (the launcher registers itself for .apk files) or passed on the
// command line is first read (winapk-runtime --verify-apk: its package and versionCode, which say which app it
// is and whether it is older than the installed one: installs.h place_install), then installed, one at a time, in
// a small install window:
//
//   1. copied into %LOCALAPPDATA%\Win.apk\library, so the app stays when the
//      download is deleted;
//   2. installed and converted once by winapk-runtime --install-only, which
//      also writes the app's icon and name (apps\<name>\icon.png, label.txt;
//      the name moves next to the APK as library\<name>.label, because the
//      app's first run moves files it does not know out of apps\<name>); a
//      bundle is then kept as library\<name>.apk and <name>.splits\, hard
//      links to the APKs it installed, not whole (library_files.h);
//   3. given a Start menu shortcut (Start > Win.apk Apps) and, unless turned
//      off in the settings, a desktop shortcut, with the app's own icon, that
//      start it through this program.
//
// One process serves it all: another start hands its files to the one
// running (forward_to_running), and an APK from Explorer shows only the
// install window, not the library.
//
// Double-clicking an app runs it (winapk-runtime --apk, without a console)
// behind a splash that stays until the runtime says its window is up
// (WINAPK_READY_EVENT); if the app ends first the splash says so, with the
// log. The app window size and orientation picked in the settings reach the
// runtime as WINAPK_PHONE_SIZE. Apps open in landscape, the shape of a PC
// screen, unless the settings say portrait; an app's details page turns one
// app the other way (library\<name>.orientation), and an app whose manifest
// fixes its launched activity's orientation always gets that one.
//
// The Store, next to the library, is Win.apk's own store app (israel2176/
// win.apk-store, package app.apkstore: Google Play's catalogue searched, free
// apps downloaded and installed through Android's PackageInstaller), shown
// inside the library window as Google Play is. Win.apk has it as a system
// component, no tile and no shortcut: the first opening of the Store downloads
// the newest release's apk-store.apk from GitHub, held to GitHub's size and
// SHA-256 of it (store_app.h), and installs it quietly; the daily update check
// updates it the same way. Its installs go through the install helper below,
// and its "Install unknown apps" is on from its first install, as a device's
// preinstalled store is allowed.
//
//   Win.apk.exe                      the library
//   Win.apk.exe <app.apk> [...]      the install window, for these files
//   Win.apk.exe --run <library apk>  run an installed app (what shortcuts do)
//   Win.apk.exe install --apk <file> --source android [--requestor-label <app>] --json
//   Win.apk.exe uninstall --package <name> --source android [--requestor-label <app>] --json
//   Win.apk.exe runtime-permission | special-access | app-details ... --source android --json
//   Win.apk.exe app-not-responding --source android --json
//   Win.apk.exe remove-shortcuts     the uninstaller's: the apps' shortcuts (remove_shortcuts_main)
//
// The last two are the install helper, upstream's macapk-installhelper: an app
// that installs apps (the Store, through PackageInstaller) hands its APK to
// PackageInstallerBackend, which runs this program. It asks the user as
// Android's package installer does (an app whose "Install unknown apps" is off
// is first sent to that switch, then "Do you want to install this app?"),
// shows the install in the install window and its end there ("App installed."
// with Open and Done, or "App not installed." and why), installs as above, and
// keeps installed_apps.json, the installed-apps registry that PackageManager
// answers apps from, so a store sees what it installed. Its output keeps the
// helper's contract: "[helper] progress <fraction> <stage>" lines, then one
// JSON object, then (once its window shows the end) "[helper] exit <code>",
// which is when the app gets its result; exit 0 done, 2 bad arguments,
// 3 declined or cancelled, 5 failed, 6 not an app. The third line is the
// permission dialogs (see "Permissions" below).

#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <windowsx.h>
#include <winhttp.h>
#include <richedit.h>
#include <bcrypt.h>
#include <tlhelp32.h>
#include <wincrypt.h>
#include <winternl.h>
#include <netlistmgr.h>
#include <softpub.h>
#include <wintrust.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cwctype>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "app_runs.h"
#include "debug_threads.h"
#include "google_apps.h"
#include "installs.h"
#include "library_files.h"
#include "maintenance.h"
#include "settings.h"
#include "store_app.h"
#include "updates.h"
#include "../host/window_fit.h"
#include "../include/winapk/aot_stamp.h"
#include "../include/winapk/package_events.h"

namespace fs = std::filesystem;

namespace {

using Gdiplus::Color;
using Gdiplus::Graphics;
using Gdiplus::GraphicsPath;
using Gdiplus::RectF;
using Gdiplus::SizeF;

constexpr UINT kInstalled = WM_APP + 1;       // an install ended (finish_install says how); lParam: new std::wstring (the name)
constexpr UINT kRunExited = WM_APP + 2;       // the app's run ended (finish_run says how); lParam: new std::wstring (the app's name)
constexpr UINT kInstallStarted = WM_APP + 3;  // lParam: new std::wstring (the app's name)
constexpr UINT kRefresh = WM_APP + 4;         // the install helper changed the library
constexpr UINT kStoreAppChecked = WM_APP + 5;     // lParam: new StoreAppCheck (the Store's newest release, as read)
constexpr UINT kStoreAppProgress = WM_APP + 6;    // wParam: percent of apk-store.apk downloaded
constexpr UINT kStoreAppDownloaded = WM_APP + 7;  // wParam: a StoreAppDownload
constexpr UINT kGmsDone = WM_APP + 10;       // the Google sign-in window closed; wParam: exit code, lParam: new std::wstring (stdout)
constexpr UINT kRunReady = WM_APP + 11;       // the app's window is up; lParam: new std::wstring (the app's name)
constexpr UINT kGmsAccount = WM_APP + 12;     // lParam: new std::wstring (the signed-in account, or empty)
constexpr UINT kUpdateChecked = WM_APP + 13;  // lParam: new UpdateCheck (the check, with its result)
constexpr UINT kUpdateProgress = WM_APP + 14; // wParam: percent of Win.apk-Setup.exe downloaded
constexpr UINT kUpdateDownloaded = WM_APP + 15;  // wParam: 0 broke off, else updates.h SetupVerdict + 1
constexpr UINT kSetupChanged = WM_APP + 16;   // the Downloads folder changed while the Google services setup is open
constexpr UINT kSetupChecked = WM_APP + 17;   // lParam: new SetupCheck (winapk-runtime --verify-apk's answer)
constexpr UINT kMaintenanceEnded = WM_APP + 18;  // the maintenance window the launcher opened ended
constexpr UINT kSetupFetched = WM_APP + 22;   // lParam: new SetupFetch (apkeep's download of one of Google's apps)
constexpr UINT kInstallSets = WM_APP + 23;    // lParam: new std::vector<InstallSet> (a batch's files as installs)
constexpr UINT kQuitIfIdle = WM_APP + 24;     // maybe_quit found nothing left: end, if that is still so
constexpr UINT kInstallProbed = WM_APP + 25;  // lParam: new InstallProbe (a file read before its install)
constexpr UINT_PTR kInfoBarTimer = 2;  // the InfoBar closes itself
constexpr UINT_PTR kUpdateTimer = 3;   // hourly: is the daily update check due?
constexpr UINT_PTR kSetupScanTimer = 4;  // the Downloads folder settled: look for new files in it
constexpr UINT_PTR kMaintenanceTimer = 5;  // shown or hidden: is a maintenance window due? (maintenance.h)
constexpr UINT_PTR kQuitTimer = 6;  // hidden: an app window it did not start is still open (maybe_quit)
constexpr UINT_PTR kRunningTimer = 7;  // shown: which apps run, started from here or not (refresh_running)
constexpr ULONG_PTR kCopyOpen = 0x57415031;  // WM_COPYDATA from another start of Win.apk.exe (forward_to_running)
constexpr UINT_PTR kAnimTimer = 1;
enum Cmd { kCmdOpen = 200, kCmdStop, kCmdInfo, kCmdRemove };

HINSTANCE g_inst;
HWND g_main;
bool g_quitting = false;  // the library's window is being destroyed: the process is ending (maybe_quit)
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
    g_rtl = winapk::launcher::settings::language_hebrew(setting(L"Language", 0),
                                                       PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_HEBREW);
}

// The Hebrew for an English string, keyed by the English itself.
const wchar_t* tr(const wchar_t* en) {
    static const std::map<std::wstring, const wchar_t*> he = {
        {L"Drop to install", L"יש לשחרר כאן כדי להתקין"},
        {L"Win.apk keeps its own copy, so the downloaded file can be deleted", L"Win.apk שומרת עותק משלה, כך שניתן למחוק את הקובץ שהורד"},
        {L"Installing…", L"מתקין…"},
        {L"Installing {0} of {1}", L"מתקין {0} מתוך {1}"},
        {L"Install app", L"התקנת אפליקציה"},
        {L"No apps installed yet", L"עדיין לא הותקנו אפליקציות"},
        {L"Logs", L"יומנים"},
        {L"Stop", L"עצירה"},
        {L"{0} running", L"{0} פועלות"},
        {L"Running", L"פועלת"},
        {L"{0} app", L"אפליקציה אחת"},
        {L"{0} apps", L"{0} אפליקציות"},
        {L"Android apps", L"אפליקציות Android"},
        {L"All files", L"כל הקבצים"},
        {L"Install an Android app", L"התקנת אפליקציית Android"},
        {L"Installing {0}…", L"מתקין את {0}…"},
        {L"{0} is still being installed.", L"{0} עדיין בהתקנה."},
        {L"Remove {0}?", L"להסיר את {0}?"},
        {L"Remove", L"הסרה"},
        {L"{0} was removed.", L"{0} הוסרה."},
        {L"Done", L"סיום"},
        {L"Theme", L"ערכת נושא"},
        {L"Follow Windows", L"בהתאם ל-Windows"},
        {L"Light", L"בהירה"},
        {L"Dark", L"כהה"},
        {L"Appearance", L"מראה"},
        {L"Language", L"שפה"},
        {L"Default", L"ברירת מחדל"},
        {L"Make default", L"הגדרה כברירת מחדל"},
        {L"App window size", L"גודל חלון האפליקציות"},
        {L"The size apps open at. An app whose window you resized opens at its saved size.",
         L"הגודל שבו אפליקציות נפתחות. אפליקציה ששינית את גודל החלון שלה תיפתח בגודל ששמרה."},
        {L"Auto (fits the screen)", L"אוטומטי (מותאם למסך)"},
        {L"320 × 480  (small, older apps)", L"320 × 480  (קטן, לאפליקציות ישנות)"},
        {L"360 × 640  (5\" phone)", L"360 × 640  (טלפון 5 אינץ')"},
        {L"360 × 780  (6.5\" phone)", L"360 × 780  (טלפון 6.5 אינץ')"},
        {L"480 × 800  (6\" HDPI)", L"480 × 800  (‏6 אינץ' HDPI)"},
        {L"540 × 960  (large phone)", L"540 × 960  (טלפון גדול)"},
        {L"600 × 960  (7\" tablet)", L"600 × 960  (טאבלט 7 אינץ')"},
        {L"800 × 1280  (10\" tablet)", L"800 × 1280  (טאבלט 10 אינץ')"},
        {L"Orientation", L"כיוון"},
        {L"Default orientation", L"כיוון ברירת המחדל"},
        {L"Apps that require an orientation always open in it. You can change it per app on its details page.",
         L"אפליקציה שדורשת כיוון מסוים תיפתח בו תמיד. אפשר לשנות לכל אפליקציה בדף הפרטים שלה."},
        {L"Portrait", L"לאורך"},
        {L"Landscape", L"לרוחב"},
        {L"Desktop shortcut for new apps", L"קיצור דרך בשולחן העבודה לאפליקציות חדשות"},
        {L"Every app also gets one in Start > Win.apk Apps.", L"כל אפליקציה מקבלת גם קיצור דרך בתפריט התחל > Win.apk Apps."},
        {L"Open folder", L"פתיחת התיקייה"},
        {L"Use my Windows folders", L"שימוש בתיקיות של Windows"},
        {L"Speed up ARM apps", L"האצת אפליקציות ARM"},
        {L"Translates ARM code to your PC's code in the background so apps run faster. Runs at low priority.",
         L"ממיר ברקע קוד ARM לקוד של המחשב, כדי שהאפליקציות ירוצו מהר יותר. פועל בעדיפות נמוכה."},
        {L"Report Google Play as the installer", L"דיווח על Google Play כמקור ההתקנה"},
        {L"Update apps in the background", L"עדכון אפליקציות ברקע"},
        {L"While Win.apk is open, even with only an app showing, Google services start for a few minutes every few "
         L"hours so the Play Store can update your apps, as a phone does. Only on mains power. Closing Win.apk stops "
         L"them.",
         L"כל עוד Win.apk פתוחה, גם כשמוצגת רק אפליקציה, שירותי Google פועלים לכמה דקות כל כמה שעות כדי שחנות Play "
         L"תוכל לעדכן את האפליקציות שלך, כמו בטלפון. רק כשהמחשב מחובר לחשמל. סגירת Win.apk עוצרת אותם."},
        {L"Apps you install appear to come from Google Play, as some banking apps require.",
         L"האפליקציות שמתקינים נראות כאילו הותקנו מ-Google Play, כפי שחלק מאפליקציות הבנקים דורשות."},
        {L"Performance", L"ביצועים"},
        {L"Optimizing for this PC…", L"מבצע אופטימיזציה למחשב הזה…"},
        {L"{0} of {1} parts", L"{0} מתוך {1} חלקים"},
        {L"The app's ARM code is being translated for this processor, at the lowest priority. Until then the app "
         L"runs it more slowly.",
         L"קוד ה-ARM של האפליקציה מתורגם למעבד הזה, בעדיפות הנמוכה ביותר. עד אז האפליקציה מריצה אותו לאט יותר."},
        {L"This app has only ARM code", L"לאפליקציה הזו יש רק קוד ARM"},
        {L"It runs translated, which uses more CPU. An x86_64 version of the app, if one exists, runs faster.",
         L"היא רצה מתורגמת, וזה דורש יותר מעבד. גרסת x86_64 של האפליקציה, אם יש כזו, רצה מהר יותר."},
        {L"App data", L"נתוני אפליקציות"},
        {L"Google account", L"חשבון Google"},
        {L"Use Google services (experimental)", L"שימוש בשירותי Google (ניסיוני)"},
        {L"Google Play", L"Google Play"},
        {L"Download", L"הורדה"},
        {L"Google services setup", L"הגדרת שירותי Google"},
        {L"Play services, Services Framework and Play Store are installed.", L"שירותי Play,\x200F Services Framework וחנות Play מותקנים."},
        {L"Install Play services, Services Framework and Play Store from their downloads.", L"התקנת שירותי Play,\x200F Services Framework וחנות Play מההורדות שלהם."},
        {L"Google Play services", L"שירותי Google Play"},
        {L"Google Play Store", L"חנות Google Play"},
        {L"Pick version {0}, for Android 14 or later.", L"יש לבחור בגרסה {0}, ל-Android 14 ומעלה."},
        {L"Pick version {0}: the x86 + x86_64 variant, nodpi, for Android 7.0 or later.", L"יש לבחור בגרסה {0}: גרסת x86 + x86_64,\x200F nodpi, ל-Android 7.0 ומעלה."},
        {L"Pick version {0}: the universal variant, nodpi, for Android 7.0 or later.", L"יש לבחור בגרסה {0}: הגרסה האוניברסלית,\x200F nodpi, ל-Android 7.0 ומעלה."},
        {L"The file could not be checked", L"לא ניתן היה לבדוק את הקובץ"},
        {L"Win.apk's runtime did not start.", L"סביבת ההרצה של Win.apk לא הופעלה."},
        {L"{0} is not one of Google's three apps. To add another app, drop it on the library.", L"{0} אינו אחת משלוש האפליקציות של Google. כדי להוסיף אפליקציה אחרת, יש לשחרר אותה בספרייה."},
        {L"The signature of {0} does not verify, so the file may have been changed after it was signed. Download it again from APKMirror.", L"החתימה של {0} אינה תקפה, ולכן ייתכן שהקובץ שונה אחרי שנחתם. יש להוריד אותו שוב מ-APKMirror."},
        {L"{0} is not signed by Google, so Win.apk does not trust it as Google's app. Download it from its APKMirror page.", L"{0} אינו חתום על ידי Google, ולכן Win.apk לא סומכת עליו כאפליקציה של Google. יש להוריד אותו מהדף שלו ב-APKMirror."},
        {L"Download all", L"הורדת הכול"},
        {L"Waiting to download", L"ממתינה להורדה"},
        {L"Downloading…", L"מוריד…"},
        {L"Checking…", L"בודקת…"},
        {L"Win.apk checks that Google signed it.", L"Win.apk בודקת ש-Google חתמה עליה."},
        {L"APKMirror", L"APKMirror"},
        {L"From APKPure, version {0}.", L"מ-APKPure, גרסה {0}."},
        {L"Win.apk downloads it and checks that Google signed it.", L"Win.apk מורידה אותה ובודקת ש-Google חתמה עליה."},
        {L"{0} could not be downloaded", L"לא ניתן היה להוריד את {0}"},
        {L"APKPure did not give version {0} in three tries. The app's row now opens its page on APKMirror, where you can download that version yourself.", L"גרסה {0} לא התקבלה מ-APKPure בשלושה ניסיונות. השורה של האפליקציה פותחת עכשיו את הדף שלה ב-APKMirror, שממנו אפשר להוריד את הגרסה הזו בעצמך."},
        {L"Google services are ready", L"שירותי Google מוכנים"},
        {L"From now on the Play Store keeps itself and Play services up to date, as on a phone.", L"מעכשיו חנות Play מעדכנת את עצמה ואת שירותי Play, כמו בטלפון."},
        {L"Download each app to your Downloads folder, or drop it here. Win.apk checks that Google signed it, "
         L"then installs it.",
         L"יש להוריד כל אפליקציה לתיקיית ההורדות, או לשחרר אותה כאן. Win.apk בודקת ש-Google חתמה עליה, ואז "
         L"מתקינה אותה."},
        {L"Download all fetches the apps; you can also download each one yourself or drop it here. Win.apk checks "
         L"that Google signed it, then installs it.",
         L"\"הורדת הכול\" מורידה את האפליקציות; אפשר גם להוריד כל אחת בעצמך או לשחרר אותה כאן. Win.apk בודקת "
         L"ש-Google חתמה עליה, ואז מתקינה אותה."},
        {L"Checking {0}…", L"בודק את {0}…"},
        {L"To use Google Play, install the following apps as described.", L"כדי להפעיל את חנות Google Play, יש להתקין את האפליקציות הבאות לפי ההנחיות."},
        {L"{0} has no code for x86_64 processors, only for {1}. On APKMirror, download its x86_64 variant.", L"ב-{0} אין קוד למעבדי x86_64, רק ל-{1}. יש להוריד מ-APKMirror את גרסת x86_64 שלו."},
        {L"{0} needs API level {1}, and Win.apk presents API level {2}. Download the version its row names.", L"{0} דורש רמת API ‏{1}, ו-Win.apk מציגה רמת API ‏{2}. יש להוריד את הגרסה שהשורה שלו מציינת."},
        {L"{0} is built for a preview of Android, which Android does not install on a released version. Download the version its row names.", L"{0} נבנה לגרסת תצוגה מקדימה של Android, ו-Android לא מתקינה אותו בגרסה שיצאה. יש להוריד את הגרסה שהשורה שלו מציינת."},
        {L"It's part of Google services.", L"היא חלק משירותי Google."},
        {L"Starting Google Play…", L"מפעיל את Google Play…"},
        {L"Google Play closed", L"Google Play נסגרה"},
        {L"Google Play didn't start", L"Google Play לא נפתחה"},
        {L"Open Google Play", L"פתיחת Google Play"},
        {L"Google's apps", L"האפליקציות של Google"},
        {L"Installed, {0}", L"מותקנת, {0}"},
        {L"Not installed.", L"לא מותקנת."},
        {L"Win.apk checks that Google signed it, then installs it", L"Win.apk בודקת ש-Google חתמה עליו, ואז מתקינה אותו"},
        {L"Runs the Google Play services, Services Framework and Play Store you install yourself, as apps other "
         L"apps can reach. Only copies signed by Google are trusted. Takes effect when an app starts.",
         L"מפעיל את שירותי Google Play, ‏Services Framework וחנות Play שהתקנת בעצמך, כאפליקציות שאפליקציות "
         L"אחרות יכולות לפנות אליהן. רק עותקים חתומים על ידי Google נחשבים מהימנים. נכנס לתוקף כשאפליקציה "
         L"מופעלת."},
        {L"Signing in…", L"מתבצעת כניסה…"},
        {L"Complete the sign-in in the Google window, or close it to cancel.", L"יש להשלים את הכניסה בחלון של Google, או לסגור אותו כדי לבטל."},
        {L"Sign out", L"התנתקות"},
        {L"Sign in", L"כניסה"},
        {L"Sign in with Google", L"כניסה עם חשבון Google"},
        {L"Add another account", L"הוספת חשבון נוסף"},
        {L"One account for every app: Google sign-in, Play license checks and push notifications.",
         L"חשבון אחד לכל האפליקציות: כניסה עם Google, בדיקת רישיון Play והתראות push."},
        {L"Sign in…", L"כניסה…"},
        {L"Sign-in takes place on Google's own page, in a separate window; Win.apk never sees your password. The "
         L"account's tokens are stored only on this PC.",
         L"הכניסה מתבצעת בדף של Google עצמה, בחלון נפרד, ו-Win.apk אינה נחשפת לסיסמה. אסימוני החשבון "
         L"נשמרים במחשב זה בלבד."},
        {L"Sign out of {0}?", L"להתנתק מהחשבון {0}?"},
        {L"The account and its tokens are removed from this PC. Apps that used it will ask you to sign in again.",
         L"החשבון והאסימונים שלו יוסרו מהמחשב. אפליקציות שהשתמשו בו יבקשו להתחבר שוב."},
        {L"Signed out of {0}.", L"החשבון {0} נותק."},
        {L"Could not sign out", L"ההתנתקות נכשלה"},
        {L"{0} could not be read as an Android app. It may be damaged or not fully downloaded; download it again.", L"לא ניתן היה לקרוא את {0} כאפליקציית אנדרואיד. ייתכן שהקובץ פגום או שההורדה לא הושלמה; הורד אותו שוב."},
        {L"Win.apk could not remove the account from this PC. The log says why.", L"Win.apk לא הצליחה להסיר את החשבון מהמחשב הזה. הסיבה כתובה ביומן."},
        {L"Signed in to Google.", L"הכניסה ל-Google הושלמה."},
        {L"Signed in as {0}.", L"מחובר בתור {0}."},
        {L"No account was added.", L"לא נוסף חשבון."},
        {L"Could not sign in to Google", L"הכניסה ל-Google נכשלה"},
        {L"Google sign-in failed (error: {0}).", L"ההתחברות עם Google נכשלה (שגיאה: {0})."},
        {L"Version {0}", L"גרסה {0}"},
        {L"About", L"אודות"},
        {L"Android apps on Windows.", L"אפליקציות Android ב-Windows."},
        {L"View", L"הצגה"},
        {L"Project page", L"דף הפרויקט"},
        {L"Credits and licenses", L"קרדיטים ורישיונות"},
        {L"Mac.apk, which Win.apk is built on, the other projects it uses, and their licenses.",
         L"Mac.apk, שעליה Win.apk מבוססת, שאר הפרויקטים שבהם היא משתמשת, והרישיונות שלהם."},
        {L"Clear library", L"ניקוי הספרייה"},
        {L"Uninstalls every app and removes its shortcuts. App data is kept.",
         L"מסיר את כל האפליקציות ואת קיצורי הדרך שלהן. נתוני האפליקציות נשמרים."},
        {L"Clear…", L"ניקוי…"},
        {L"Clear the library?", L"לנקות את הספרייה?"},
        {L"All {0} apps and their shortcuts will be removed. App data is kept.",
         L"כל {0} האפליקציות וקיצורי הדרך שלהן יוסרו. נתוני האפליקציות נשמרים."},
        {L"The library was cleared.", L"הספרייה נוקתה."},
        {L"Reading {0}…", L"קריאת {0}…"},
        {L"Replace {0} with an older version?", L"להחליף את {0} בגרסה ישנה יותר?"},
        {L"The installed version is {0}, and this file is version {1}. What the app saved with the newer version may "
         L"not work with the older one.",
         L"הגרסה המותקנת היא {0}, והקובץ הזה הוא גרסה {1}. ייתכן שמה שהאפליקציה שמרה בגרסה החדשה לא יעבוד בגרסה "
         L"הישנה."},
        {L"Replace", L"החלפה"},
        {L"{0} was not replaced: the installed version is newer.", L"{0} לא הוחלפה: הגרסה המותקנת חדשה יותר."},
        {L"The library was cleared, except {0}. Close it, then try again.",
         L"הספרייה נוקתה, מלבד {0}. יש לסגור את האפליקציה ולנסות שוב."},
        {L"The library was cleared, except {0} apps: {1}. Close them, then try again.",
         L"הספרייה נוקתה, מלבד {0} אפליקציות: {1}. יש לסגור אותן ולנסות שוב."},
        {L"{0} could not be removed: a file of it is in use. Close it, then try again.",
         L"לא ניתן היה להסיר את {0}: אחד מהקבצים שלה בשימוש. יש לסגור אותה ולנסות שוב."},
        {L"Every app's install and run logs, to attach to a bug report. Each app's own is on its details page.",
         L"יומני ההתקנה וההפעלה של כל האפליקציות, לצירוף לדיווח על תקלה. היומן של כל אפליקציה נמצא גם בפרטי האפליקציה."},
        {L"Log", L"יומן"},
        {L"View the app's recent logs", L"צפייה ביומנים האחרונים של האפליקציה"},
        {L"The files and settings each app has saved.", L"הקבצים וההגדרות שכל אפליקציה שמרה."},
        {L"Google account: not signed in", L"חשבון Google: לא מחובר"},
        {L"Google account: {0}", L"חשבון Google: {0}"},
        {L"Settings", L"הגדרות"},
        {L"Installed", L"הותקנה"},
        {L"Storage", L"אחסון"},
        {L"Show", L"הצגה"},
        {L"Open", L"פתיחה"},
        {L"Desktop shortcut", L"קיצור דרך בשולחן העבודה"},
        {L"Remove…", L"הסרה…"},
        {L"{0} is ready", L"{0} מוכנה"},
        {L"Waiting", L"ממתינה"},
        {L"Copy", L"העתקה"},
        {L"Portrait – required by the app", L"לאורך – לפי דרישת האפליקציה"},
        {L"Landscape – required by the app", L"לרוחב – לפי דרישת האפליקציה"},
        {L"Settings' default", L"ברירת המחדל שבהגדרות"},
        {L"The settings' default is landscape.", L"ברירת המחדל שבהגדרות היא לרוחב."},
        {L"The settings' default is portrait.", L"ברירת המחדל שבהגדרות היא לאורך."},
        {L"{0} opens in landscape from now on.", L"{0} תיפתח לרוחב מעתה."},
        {L"{0} opens in portrait from now on.", L"{0} תיפתח לאורך מעתה."},
        {L"{0} stopped with an error", L"{0} נעצרה עם שגיאה"},
        {L"An app", L"אפליקציה"},
        {L"{0} asks to remove it.", L"{0} מבקשת להסיר אותה."},
        {L"Do you want to update this app?", L"האם לעדכן את האפליקציה הזו?"},
        {L"{0} asks to update it with the copy it has ({1}).", L"{0} מבקשת לעדכן אותה בעותק שיש לה ({1})."},
        {L"Do you want to install this app?", L"האם להתקין את האפליקציה הזו?"},
        {L"{0} asks to install it.", L"{0} מבקשת להתקין אותה."},
        {L"App installed.", L"האפליקציה הותקנה."},
        {L"App updated.", L"האפליקציה עודכנה."},
        {L"App not installed.", L"האפליקציה לא הותקנה."},
        {L"For your security, your PC currently isn't allowed to install unknown apps from this source.",
         L"מטעמי אבטחה, המחשב שלך לא מורשה כרגע להתקין אפליקציות לא מוכרות מהמקור הזה."},
        {L"You can change this in Settings.", L"אפשר לשנות זאת בהגדרות."},
        {L"bytes", L"בתים"},
        {L"Library", L"ספרייה"},
        {L"Store", L"חנות"},
        {L"Starting the Store…", L"מפעיל את החנות…"},
        {L"Getting the Store…", L"מוריד את החנות…"},
        {L"Downloading the Store… {0}%", L"מוריד את החנות… {0}%"},
        {L"Installing the Store…", L"מתקין את החנות…"},
        {L"Win.apk's own store, from its page on GitHub. This happens once.",
         L"החנות של Win.apk, מהדף שלה ב-GitHub. זה קורה פעם אחת."},
        {L"The Store couldn't be set up", L"לא ניתן היה להכין את החנות"},
        {L"GitHub could not be reached, or did not answer. Check the internet connection, then try again later.",
         L"לא ניתן היה להתחבר ל-GitHub, או שלא התקבלה תשובה. בדוק את החיבור לאינטרנט ונסה שוב מאוחר יותר."},
        {L"GitHub lists no release of the Store that Win.apk can check.",
         L"ב-GitHub אין גרסה של החנות ש-Win.apk יכולה לבדוק."},
        {L"The downloaded file is not the one GitHub lists. It was deleted and not installed.",
         L"הקובץ שהורד אינו זה ש-GitHub מפרט. הוא נמחק ולא הותקן."},
        {L"Installing it failed. Its log says why.", L"ההתקנה נכשלה. היומן שלה מסביר למה."},
        {L"The Store closed", L"החנות נסגרה"},
        {L"The Store didn't start", L"החנות לא נפתחה"},
        {L"Open the Store", L"פתיחת החנות"},
        {L"The Store opened in its own window.", L"החנות נפתחה בחלון נפרד."},
        {L"Google Play opened in its own window.", L"Google Play נפתחה בחלון נפרד."},
        {L"It's part of Win.apk.", L"היא חלק מ-Win.apk."},
        {L"Install", L"התקנה"},
        {L"Update", L"עדכון"},
        {L"Try again", L"ניסיון חוזר"},
        {L"unknown", L"לא ידוע"},
        {L"Allow", L"אישור"},
        {L"Don't allow", L"לא לאשר"},
        {L"Allow {0} to manage media?", L"לאפשר ל-{0} לנהל מדיה?"},
        {L"Photos and videos", L"תמונות וסרטונים"},
        {L"Music and audio", L"מוזיקה ואודיו"},
        {L"Files and media", L"קבצים ומדיה"},
        {L"Files", L"קבצים"},
        {L"Photos, videos, music, and audio", L"תמונות, סרטונים, מוזיקה ואודיו"},
        {L"Location", L"מיקום"},
        {L"All files access", L"גישה לכל הקבצים"},
        {L"Media management", L"ניהול מדיה"},
        {L"The permission could not be changed: {0}", L"לא ניתן היה לשנות את ההרשאה: {0}"},
        {L"Permissions", L"הרשאות"},
        {L"Reset permissions", L"איפוס הרשאות"},
        {L"The app asks again the next time it needs them.", L"האפליקציה תשאל שוב בפעם הבאה שתצטרך אותן."},
        {L"Reset", L"איפוס"},
        {L"Window", L"חלון"},
        {L"Window size  ·  {0} × {1} dp", L"גודל החלון  ·  {0} × {1} dp"},
        {L"The size you last gave the app's window. After a reset, it opens at the app window size in Settings again.",
         L"הגודל האחרון שנתת לחלון של האפליקציה. אחרי איפוס היא תיפתח שוב בגודל חלון האפליקציות שבהגדרות."},
        {L"{0} opens at the app window size in Settings again.", L"{0} תיפתח שוב בגודל חלון האפליקציות שבהגדרות."},
        {L"{0} will ask for its permissions again.", L"{0} תשאל שוב על ההרשאות שלה."},
        {L"Allow {0} to take pictures and record video?", L"לאפשר ל-{0} לצלם תמונות ולהקליט וידאו?"},
        {L"Allow {0} to record audio?", L"לאפשר ל-{0} להקליט אודיו?"},
        {L"Allow {0} to access your contacts?", L"לאפשר ל-{0} לגשת לאנשי הקשר שלך?"},
        {L"Allow {0} to access your calendar?", L"לאפשר ל-{0} לגשת ליומן שלך?"},
        {L"Allow {0} to make and manage phone calls?", L"לאפשר ל-{0} לבצע ולנהל שיחות טלפון?"},
        {L"Allow {0} to access your phone call logs?", L"לאפשר ל-{0} לגשת ליומני השיחות שלך?"},
        {L"Allow {0} to send and view SMS messages?", L"לאפשר ל-{0} לשלוח ולהציג הודעות SMS?"},
        {L"Allow {0} to send you notifications?", L"לאפשר ל-{0} לשלוח לך התראות?"},
        {L"Allow {0} to find, connect to, and determine the relative position of nearby devices?",
         L"לאפשר ל-{0} לאתר מכשירים בקרבת מקום, להתחבר אליהם ולקבוע את מיקומם היחסי?"},
        {L"Allow {0} to access your physical activity?", L"לאפשר ל-{0} לגשת לנתוני הפעילות הגופנית שלך?"},
        {L"Allow {0} to access sensor data about your vital signs?", L"לאפשר ל-{0} לגשת לנתוני חיישנים על הסימנים החיוניים שלך?"},
        {L"Allow {0} to access sensor data about your vital signs all the time?",
         L"לאפשר ל-{0} לגשת לנתוני חיישנים על הסימנים החיוניים שלך כל הזמן?"},
        {L"Location, all the time", L"מיקום, כל הזמן"},
        {L"Camera", L"מצלמה"},
        {L"Microphone", L"מיקרופון"},
        {L"Contacts", L"אנשי קשר"},
        {L"Calendar", L"יומן"},
        {L"Phone", L"טלפון"},
        {L"Call logs", L"יומני שיחות"},
        {L"SMS", L"SMS"},
        {L"Notifications", L"התראות"},
        {L"Nearby devices", L"מכשירים בקרבת מקום"},
        {L"Physical activity", L"פעילות גופנית"},
        {L"Body sensors", L"חיישני גוף"},
        {L"Body sensors, all the time", L"חיישני גוף, כל הזמן"},
        {L"Display over other apps", L"הצגה מעל אפליקציות אחרות"},
        {L"Install unknown apps", L"התקנת אפליקציות לא מוכרות"},
        {L"Allow {0} to display over other apps?", L"לאפשר ל-{0} להציג מעל אפליקציות אחרות?"},
        {L"Close", L"סגירה"},
        {L"Notification access", L"גישה להתראות"},
        {L"Win.apk can't give {0} notification access. Windows has no feed of other apps' notifications for an Android app to read.",
         L"Win.apk לא יכולה לתת ל-{0} גישה להתראות. ב-Windows אין זרם התראות של אפליקציות אחרות שאפליקציית Android תוכל לקרוא."},
        {L"Accessibility", L"נגישות"},
        {L"Win.apk can't run {0} as an accessibility service. There is no Android accessibility framework on Windows for it to observe or act through.",
         L"Win.apk לא יכולה להריץ את {0} כשירות נגישות. ב-Windows אין מסגרת נגישות של Android שדרכה יוכל לצפות או לפעול."},
        {L"Usage access", L"גישה לנתוני שימוש"},
        {L"Win.apk can't give {0} usage access. Each Android app runs on its own here, so there is no usage history of other apps to read.",
         L"Win.apk לא יכולה לתת ל-{0} גישה לנתוני שימוש. כל אפליקציית Android רצה כאן בנפרד, כך שאין היסטוריית שימוש של אפליקציות אחרות לקרוא."},
        {L"Do Not Disturb access", L"גישה למצב נא לא להפריע"},
        {L"Win.apk has no Do Not Disturb mode for {0} to control.", L"ב-Win.apk אין מצב נא לא להפריע ש-{0} תוכל לשלוט בו."},
        {L"Battery optimization", L"אופטימיזציית סוללה"},
        {L"Win.apk never limits {0} in the background, so it already runs unrestricted.",
         L"Win.apk אף פעם לא מגבילה את {0} ברקע, כך שהיא כבר רצה ללא הגבלה."},
        {L"Alarms and reminders", L"התראות ותזכורות"},
        {L"Win.apk lets {0} schedule exact alarms; there is nothing to turn on.",
         L"Win.apk מאפשרת ל-{0} לתזמן התראות מדויקות; אין מה להפעיל."},
        {L"Modify system settings", L"שינוי הגדרות מערכת"},
        {L"There are no Android system settings for {0} to modify on Windows; it is treated as allowed.",
         L"ב-Windows אין הגדרות מערכת של Android ש-{0} תוכל לשנות; זה נחשב כמותר."},
        {L"{0} was stopped, as Android does when a permission is revoked.",
         L"{0} נעצרה, כפי ש-Android עושה כשהרשאה נשללת."},
        {L"Allow {0} to access music and audio on this PC?", L"לאפשר ל-{0} גישה למוזיקה ולאודיו במחשב הזה?"},
        {L"Allow {0} to access photos and videos on this PC?", L"לאפשר ל-{0} גישה לתמונות ולסרטונים במחשב הזה?"},
        {L"Allow {0} to access photos, media, and files on this PC?", L"לאפשר ל-{0} גישה לתמונות, למדיה ולקבצים במחשב הזה?"},
        {L"Allow {0} to access photos, videos, music, and audio on this PC?", L"לאפשר ל-{0} גישה לתמונות, לסרטונים, למוזיקה ולאודיו במחשב הזה?"},
        {L"Allow {0} to access photos, videos, music, audio, and other files on this PC?", L"לאפשר ל-{0} גישה לתמונות, לסרטונים, למוזיקה, לאודיו ולקבצים אחרים במחשב הזה?"},
        {L"Allow {0} to access this PC's location all the time?", L"לאפשר ל-{0} גישה למיקום של המחשב הזה כל הזמן?"},
        {L"Allow {0} to access this PC's location?", L"לאפשר ל-{0} גישה למיקום של המחשב הזה?"},
        {L"Allow {0} to install other apps?", L"לאפשר ל-{0} להתקין אפליקציות אחרות?"},
        {L"Allow {0} to manage all files?", L"לאפשר ל-{0} לנהל את כל הקבצים?"},
        {L"Also remove accounts and data saved by the app", L"להסיר גם חשבונות ונתונים שהאפליקציה שמרה"},
        {L"Removes the app and its shortcuts.", L"מסיר את האפליקציה ואת קיצורי הדרך שלה."},
        {L"App details", L"פרטי האפליקציה"},
        {L"Apps", L"אפליקציות"},
        {L"Apps save to and open from your Downloads, Pictures, Documents, Music and Videos.", L"אפליקציות שומרות בתיקיות ההורדות, התמונות, המסמכים, המוזיקה והסרטונים שלך, ופותחות מהן."},
        {L"Cancel", L"ביטול"},
        {L"Which screen should open?", L"איזה מסך לפתוח?"},
        {L"{0} has no screen to open from a launcher, so on a phone it has no icon either. These are the screens "
         L"other apps can open; the one you choose opens from now on, and App details can change it.",
         L"ל-{0} אין מסך שנפתח ממסך הבית, ולכן גם בטלפון אין לה סמל. אלה המסכים שאפליקציות אחרות יכולות לפתוח; "
         L"המסך שייבחר ייפתח מעכשיו, ואפשר לשנות אותו בפרטי האפליקציה."},
        {L"{0} cannot be opened", L"לא ניתן לפתוח את {0}"},
        {L"It has no screen to open from a launcher, and none that other apps may open. It works for other apps "
         L"(as a plug-in or a service), which open it themselves.",
         L"אין לה מסך שנפתח ממסך הבית, ואף מסך שאפליקציות אחרות רשאיות לפתוח. היא פועלת בשביל אפליקציות אחרות "
         L"(כתוסף או כשירות), והן פותחות אותה בעצמן."},
        {L"OK", L"אישור"},
        {L"Screen it opens", L"המסך שנפתח"},
        {L"Asked when it opens", L"ייבחר בעת הפתיחה"},
        {L"The app has no screen to open from a launcher, so it opens this one of the screens other apps can "
         L"open.",
         L"לאפליקציה אין מסך שנפתח ממסך הבית; זה המסך שנפתח, מבין אלה שאפליקציות אחרות יכולות לפתוח."},
        {L"Change…", L"שינוי…"},
        {L"{0} opens {1} from now on.", L"{0} תיפתח מעכשיו במסך „{1}”."},
        {L"Cancelled", L"בוטלה"},
        {L"Close {0} first, then install it again.", L"יש לסגור קודם את {0}, ואז להתקין אותה שוב."},
        {L"Close {0} first, then remove it.", L"יש לסגור קודם את {0}, ואז להסיר אותה."},
        {L"Closes the app at once, as Force stop does on a phone.", L"סוגר את האפליקציה מיד, כמו \"עצירה בכפייה\" בטלפון."},
        {L"Run files  ·  {0}", L"קבצי ההרצה  ·  {0}"},
        {L"What Win.apk prepared to run the app: {0}", L"מה ש-Win.apk הכינה כדי להריץ את האפליקציה: {0}"},
        {L"Copy details", L"העתקת הפרטים"},
        {L"Report a problem", L"דיווח על בעיה"},
        {L"Report", L"דיווח"},
        {L"Report a problem with {0}", L"דיווח על בעיה ב-{0}"},
        {L"Open report", L"פתיחת דיווח"},
        {L"Check whether it was already reported", L"בדיקה אם הבעיה כבר דווחה"},
        {L"Opens a GitHub issue with the app's details and log", L"פותח דיווח ב-GitHub עם פרטי האפליקציה והיומן"},
        {L"A GitHub form opens with the version and system details filled in. The log is selected in its folder "
         L"for you to attach; nothing is sent automatically.",
         L"ייפתח טופס ב-GitHub עם פרטי הגרסה והמערכת. היומן יסומן בתיקייה לצירוף; שום דבר לא נשלח אוטומטית."},
        {L"Couldn't install", L"ההתקנה נכשלה"},
        {L"This app is built only for 32-bit ARM, which Win.apk cannot run. Download its x86_64 or arm64-v8a build.",
         L"האפליקציה הזו בנויה רק לארם 32 ביט, ו-Win.apk לא יכול להריץ אותה. הורד את הגרסה שלה ל-x86_64 או ל-arm64-v8a."},
        {L"This app has code only for {0}, which Win.apk cannot run. Download its x86_64 or arm64-v8a build.",
         L"לאפליקציה הזו יש קוד רק ל-{0}, ו-Win.apk לא יכול להריץ אותו. הורד את הגרסה שלה ל-x86_64 או ל-arm64-v8a."},
        {L"This file is only a part of an app. Install the app's whole bundle (.apkm, .xapk or .apks), or all of its "
         L".apk files together.",
         L"הקובץ הזה הוא רק חלק מאפליקציה. יש להתקין את החבילה המלאה של האפליקציה (‎.apkm, ‎.xapk או ‎.apks), או את כל "
         L"קובצי ה-‎.apk‎ שלה יחד."},
        {L"This app comes in parts, and some are missing. Install the app's whole bundle (.apkm, .xapk or .apks), or "
         L"all of its .apk files together.",
         L"האפליקציה הזו מורכבת מכמה חלקים, וחלק מהם חסרים. יש להתקין את החבילה המלאה של האפליקציה (‎.apkm, ‎.xapk או "
         L"‎.apks), או את כל קובצי ה-‎.apk‎ שלה יחד."},
        {L"These files are not the parts of one version of one app.",
         L"הקבצים האלה אינם חלקים של גרסה אחת של אפליקציה אחת."},
        {L"Couldn't install {0}", L"לא ניתן היה להתקין את {0}"},
        {L"Drop an Android app here", L"אפשר לגרור לכאן אפליקציית Android"},
        {L"The reason for each failure is shown below.", L"סיבת הכישלון מוצגת ליד כל אחת."},
        {L"Enter", L"Enter"},
        {L"Google did not answer. Check the internet connection, then try again.", L"Google לא ענתה. יש לבדוק את החיבור לאינטרנט ולנסות שוב."},
        {L"Google sign-in", L"כניסה ל-Google"},
        {L"Hide details", L"הסתרת הפרטים"},
        {L"Installing {0}", L"מתקין את {0}"},
        {L"Installs asked for by apps", L"התקנות שאפליקציות ביקשו"},
        {L"It can change or delete photos, videos and music that other apps made, without asking you.", L"היא תוכל לשנות או למחוק תמונות, סרטונים ומוזיקה שאפליקציות אחרות יצרו, בלי לשאול אותך."},
        {L"It can read, change and delete every file in your Downloads, Pictures, Documents, Music and Videos.", L"היא תוכל לקרוא, לשנות ולמחוק כל קובץ בתיקיות ההורדות, התמונות, המסמכים, המוזיקה והסרטונים שלך."},
        {L"It's in the Start menu and on the desktop.", L"היא בתפריט התחל ובשולחן העבודה."},
        {L"It's in the Start menu.", L"היא בתפריט התחל."},
        {L"Keep installing", L"המשך ההתקנה"},
        {L"Log - Win.apk", L"יומן - Win.apk"},
        {L"Manage", L"ניהול"},
        {L"No matches", L"אין התאמות"},
        {L"OK", L"אישור"},
        {L"On Windows such windows stay inside the app's own window.", L"ב-Windows חלונות כאלה נשארים בתוך החלון של האפליקציה."},
        {L"Open .apk, .apkm, .xapk and .apks files with Win.apk", L"פתיחת קובצי ‎.apk, ‎.apkm, ‎.xapk ו-‎.apks באמצעות Win.apk"},
        {L"Precise location", L"מיקום מדויק"},
        {L"Ready", L"מוכנה"},
        {L"Search the log", L"חיפוש ביומן"},
        {L"Shortcut", L"קיצור דרך"},
        {L"Show details", L"הצגת הפרטים"},
        {L"Show in library", L"הצגה בספרייה"},
        {L"Show log", L"הצגת היומן"},
        // Android's own Hebrew (framework-res, iw): aerr_application, aerr_application_repeated, app_info,
        // aerr_close_app, anr_application_process, aerr_wait.
        {L"{0} has stopped", L"האפליקציה {0} הפסיקה"},
        {L"{0} keeps stopping", L"האפליקציה {0} נעצרת שוב ושוב"},
        {L"App info", L"פרטי אפליקציה"},
        {L"Close app", L"סגירת האפליקציה"},
        {L"{0} isn't responding", L"האפליקציה {0} לא מגיבה"},
        {L"Wait", L"להמתין"},
        {L"Starting…", L"מתחיל…"},
        {L"Still starting. The first start can take a minute.", L"עדיין בהפעלה. ההפעלה הראשונה עשויה להימשך כדקה."},
        {L"Stop installing", L"עצירת ההתקנה"},
        {L"Stop installing?", L"לעצור את ההתקנה?"},
        {L"The app is in Start > Win.apk Apps either way.", L"האפליקציה נמצאת בכל מקרה בתפריט התחל > Win.apk Apps."},
        {L"The app stopped with an error.", L"האפליקציה נעצרה עם שגיאה."},
        {L"The app's file  ·  {0}", L"קובץ האפליקציה  ·  {0}"},
        {L"The app's files  ·  {0}", L"קובצי האפליקציה  ·  {0}"},
        {L"The install was cancelled.", L"ההתקנה בוטלה."},
        {L"The install was cancelled. The app stays as it was.", L"ההתקנה בוטלה. האפליקציה נשארת כפי שהייתה."},
        {L"They're in the Start menu.", L"הן בתפריט התחל."},
        {L"This app has no permissions to set.", L"לאפליקציה הזו אין הרשאות להגדיר."},
        {L"This file has no code for PCs. Install the full bundle (.apkm, .xapk or .apks) that includes the arm64-v8a part.", L"בקובץ הזה אין קוד למחשב. יש להתקין את החבילה המלאה (‎.apkm, ‎.xapk או ‎.apks) שכוללת את החלק arm64-v8a."},
        {L"This log is written the next time the app is opened or installed.", L"היומן ייכתב בפעם הבאה שהאפליקציה תיפתח או תותקן."},
        {L"Troubleshooting", L"פתרון בעיות"},
        {L"Turning a permission off closes {0}.", L"כיבוי הרשאה סוגר את {0}."},
        {L"What was not installed yet is left out. An app that was being updated stays as it was.", L"מה שעוד לא הותקן לא יותקן. אפליקציה שהייתה באמצע עדכון נשארת כפי שהייתה."},
        {L"Win.apk could not convert this app. See the log for details.", L"Win.apk לא הצליחה להמיר את האפליקציה. פרטים נוספים ביומן."},
        {L"Win.apk could not copy this file into its library. Check that the disk has free space, then try again.", L"Win.apk לא הצליחה להעתיק את הקובץ לספרייה שלה. בדוק שיש מקום פנוי בדיסק ונסה שוב."},
        {L"Win.apk could not start its runtime. See the log for details.", L"Win.apk לא הצליחה להפעיל את סביבת ההרצה שלה. פרטים נוספים ביומן."},
        {L"A part of Win.apk is missing or does not load. Install Win.apk again.", L"חלק מ־Win.apk חסר או לא נטען. התקן את Win.apk מחדש."},
        {L"Win.apk still asks you before each app it installs.", L"Win.apk עדיין תשאל אותך לפני כל אפליקציה שתותקן."},
        {L"Win.apk's runtime is missing. Install Win.apk again.", L"סביבת ההרצה של Win.apk חסרה. יש להתקין את Win.apk מחדש."},
        {L"Removes the app and its shortcuts. You can also remove what it saved.", L"מסיר את האפליקציה ואת קיצורי הדרך שלה. אפשר להסיר גם את מה שהיא שמרה."},
        {L"or choose Install app  ·  .apk .apkm .xapk .apks", L"או לבחור „התקנת אפליקציה”  ·  apk, apkm, xapk, apks"},
        {L"{0}  ·  install", L"{0}  ·  התקנה"},
        {L"{0}  ·  last run", L"{0}  ·  הפעלה אחרונה"},
        {L"{0}  ·  run before", L"{0}  ·  ההפעלה הקודמת"},
        {L"{0} and what it saved were removed.", L"{0} ומה שהיא שמרה הוסרו."},
        {L"{0} apps are ready", L"{0} אפליקציות מוכנות"},
        {L"{0} could not be installed.", L"לא ניתן היה להתקין את {0}."},
        {L"{0} didn't start", L"{0} לא נפתחה"},
        {L"{0} is installed.", L"{0} הותקנה."},
        {L"{0} of {1}", L"{0} מתוך {1}"},
        {L"{0} of {1} apps are installed", L"{0} מתוך {1} אפליקציות הותקנו"},
        {L"{0} was not installed", L"{0} לא הותקנה"},
        {L"{0}'s permissions", L"ההרשאות של {0}"},
        {L"Preparing…", L"בהכנה…"},
        {L"Converting the app's code. This happens once.", L"ממיר את קוד האפליקציה. זה קורה פעם אחת."},
        {L"Getting the app's name and icon…", L"קורא את שם האפליקציה והסמל שלה…"},
        {L"Getting the app ready. This happens once.", L"מכין את האפליקציה. זה קורה פעם אחת."},
        {L"Getting Win.apk ready. This happens once.", L"מכין את Win.apk. זה קורה פעם אחת."},
        {L"Windows' location settings apply too.", L"גם הגדרות המיקום של Windows חלות."},
        {L"Windows' camera settings apply too.", L"גם הגדרות המצלמה של Windows חלות."},
        {L"Windows' microphone settings apply too.", L"גם הגדרות המיקרופון של Windows חלות."},
        {L"They show as Windows notifications.", L"הן יוצגו כהתראות של Windows."},
        {L"This PC has none of these for the app to find.", L"אין במחשב הזה דבר כזה שהאפליקציה תוכל למצוא."},
        {L"This PC has no such sensors.", L"אין במחשב הזה חיישנים כאלה."},
        {L"These are the Bluetooth devices near this PC.", L"אלה מכשירי ה-Bluetooth שליד המחשב הזה."},
        {L"Updates", L"עדכונים"},
        {L"Check", L"בדיקה"},
        {L"Checking…", L"בודק…"},
        {L"Checking for updates…", L"בודק אם יש עדכונים…"},
        {L"Check for updates automatically", L"בדיקת עדכונים אוטומטית"},
        {L"Once a day, while the library is open.", L"פעם ביום, כשהספרייה פתוחה."},
        {L"Not checked yet.", L"טרם בוצעה בדיקה."},
        {L"Last checked {0}.", L"בדיקה אחרונה: {0}."},
        {L"Win.apk is up to date. Last checked {0}.", L"Win.apk מעודכנת. בדיקה אחרונה: {0}."},
        {L"Win.apk is up to date (version {0}).", L"Win.apk מעודכנת (גרסה {0})."},
        {L"Version {0} is available.", L"גרסה {0} זמינה."},
        {L"A new version of Win.apk is available: {0}.", L"יש גרסה חדשה של Win.apk: {0}."},
        {L"Couldn't check for updates. Check the internet connection.", L"לא ניתן היה לבדוק אם יש עדכונים. יש לבדוק את החיבור לאינטרנט."},
        {L"What's new", L"מה חדש"},
        {L"What's new in {0}", L"מה חדש בגרסה {0}"},
        {L"Released {0}", L"תאריך פרסום: {0}"},
        {L"Release notes", L"הערות גרסה"},
        {L"This release has no notes.", L"לגרסה זו אין הערות."},
        {L"Release page", L"דף הגרסה"},
        {L"Win.apk closes and the installer replaces it. Your apps and their data are kept.", L"Win.apk תיסגר ותוכנית ההתקנה תחליף אותה. האפליקציות שלך והנתונים שלהן יישמרו."},
        {L"This release has no installer; its page on GitHub has its files.", L"לגרסה זו אין תוכנית התקנה; הקבצים שלה נמצאים בדף הגרסה ב-GitHub."},
        {L"The check reads the list of releases on GitHub. Nothing about you or your apps is sent.", L"הבדיקה קוראת את רשימת הגרסאות ב-GitHub. שום מידע עליך או על האפליקציות שלך אינו נשלח."},
        {L"Downloading Win.apk {0}…", L"מוריד את Win.apk {0}…"},
        {L"Downloading… {0}%", L"מוריד… {0}%"},
        {L"Couldn't download the update. Check the internet connection.", L"לא ניתן היה להוריד את העדכון. יש לבדוק את החיבור לאינטרנט."},
        {L"Checking the Win.apk {0} installer…", L"בודק את תוכנית ההתקנה של Win.apk {0}…"},
        {L"The downloaded installer is not the one GitHub lists (its SHA-256 differs). It was deleted and not run.", L"תוכנית ההתקנה שהורדה אינה זו ש-GitHub מפרט (סיכום הביקורת SHA-256 שלה שונה). היא נמחקה ולא הופעלה."},
        {L"The downloaded installer is not of the version offered. It was deleted and not run.", L"תוכנית ההתקנה שהורדה אינה של הגרסה שהוצעה. היא נמחקה ולא הופעלה."},
        {L"The downloaded installer's signature does not hold. It was deleted and not run.", L"החתימה של תוכנית ההתקנה שהורדה אינה תקפה. היא נמחקה ולא הופעלה."},
        {L"GitHub gives no SHA-256 checksum of this release's installer, so Win.apk does not download it; its page on GitHub has its files.", L"GitHub אינו מפרסם סיכום ביקורת SHA-256 לתוכנית ההתקנה של גרסה זו, ולכן Win.apk אינה מורידה אותה; הקבצים שלה נמצאים בדף הגרסה ב-GitHub."},
        {L"Win.apk's installers are not code-signed: an update runs only when its size, its SHA-256 checksum and its version are the ones GitHub lists for the release.", L"תוכניות ההתקנה של Win.apk אינן חתומות בחתימת קוד: עדכון מופעל רק כשהגודל שלו, סיכום הביקורת SHA-256 שלו והגרסה שלו הם אלה ש-GitHub מפרט לגרסה."},
        {L"Wait for the installs to finish, then update Win.apk.", L"יש להמתין לסיום ההתקנות, ואז לעדכן את Win.apk."},
        {L"Close {0} first: the update replaces files it uses.", L"יש לסגור קודם את {0}: העדכון מחליף קבצים שהיא משתמשת בהם."},
        {L"Close the running apps first: the update replaces files they use.", L"יש לסגור קודם את האפליקציות הפועלות: העדכון מחליף קבצים שהן משתמשות בהם."},
        {L"The installer could not be started.", L"לא ניתן היה להפעיל את תוכנית ההתקנה."},
        {L"Debug mode", L"מצב דיבוג"},
        {L"For developers. Apps opened from now on report every thread they have, with its Java and native stack.", L"למפתחים. אפליקציות שייפתחו מעכשיו ידווחו על כל תהליכון שלהן, עם מחסנית ה-Java והמחסנית המקורית שלו."},
        {L"Threads", L"תהליכונים"},
        {L"Every thread of the running apps, live. Each app's own is on its details page.", L"כל התהליכונים של האפליקציות הפועלות, בזמן אמת. אלה של כל אפליקציה נמצאים בדף הפרטים שלה."},
        {L"Every thread of the app's processes, live, with its stack", L"כל התהליכונים של תהליכי האפליקציה, בזמן אמת, עם המחסנית"},
        {L"Threads - Win.apk", L"תהליכונים - Win.apk"},
        {L"All apps", L"כל האפליקציות"},
        {L"Win.apk services", L"שירותי Win.apk"},
        {L"App native", L"קוד מקורי של האפליקציה"},
        {L"System", L"מערכת"},
        {L"Not identified", L"לא מזוהים"},
        {L"Looking for running apps…", L"מחפש אפליקציות פועלות…"},
        {L"No app is running. Open an app and its threads appear here.", L"אין אפליקציה פועלת. כשתיפתח אפליקציה, התהליכונים שלה יופיעו כאן."},
        {L"{0} is not running. Open it and its threads appear here.", L"{0} אינה פועלת. כשהיא תיפתח, התהליכונים שלה יופיעו כאן."},
        {L"Threads: {0}  ·  Processes: {1}", L"תהליכונים: {0}  ·  תהליכים: {1}"},
        {L"{0} shown", L"מוצגים {0}"},
        {L"Reopen the app for Java names and stacks", L"יש לפתוח את האפליקציה מחדש כדי לראות שמות ומחסניות של Java"},
        {L"Paused", L"מושהה"},
        {L"Pause", L"השהיה"},
        {L"Resume", L"המשך"},
        {L"Select a thread to see its stack.", L"יש לבחור תהליכון כדי לראות את המחסנית שלו."},
        {L"Paused. Resume to see this thread's stack.", L"מושהה. יש ללחוץ על „המשך” כדי לראות את המחסנית של התהליכון."},
        {L"The thread has ended.", L"התהליכון הסתיים."},
        {L"This process has no debug agent: it was opened before debug mode was turned on, or it does not answer. Stop the app and open it again to see Java names and stacks.", L"לתהליך הזה אין סוכן דיבוג: הוא נפתח לפני שמצב הדיבוג הופעל, או שאינו עונה. יש לעצור את האפליקציה ולפתוח אותה שוב כדי לראות שמות ומחסניות של Java."},
        {L"Stacks are shown for the processes that run the app's code. This one is a helper the app started.", L"מחסניות מוצגות לתהליכים שמריצים את קוד האפליקציה. זהו תהליך עזר שהאפליקציה הפעילה."},
        {L"Search threads: a name, a type, a library", L"חיפוש תהליכונים: שם, סוג, ספרייה"},
        {L"Process", L"תהליך"},
        {L"Thread ID", L"מזהה תהליכון"},
        {L"Name", L"שם"},
        {L"Type", L"סוג"},
        {L"State", L"מצב"},
        {L"CPU %", L"‏% מעבד"},
        {L"CPU time", L"זמן מעבד"},
        {L"Priority", L"עדיפות"},
        {L"Started in", L"התחיל ב-"}
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
// The task the runtime keeps for the app's next start (the activities' saved state, sealed, as system_server
// keeps a dead process's ActivityRecords: aosp_app/.../SavedTask.java). Stop and an install drop it.
fs::path task_state_file(const std::wstring& name) { return app_dir(name) / L"task-state.parcel"; }
fs::path logs_dir() { return data_dir() / L"logs"; }
fs::path last_run_log() { return logs_dir() / L"last-run.log"; }
fs::path run_log(const std::wstring& name) { return logs_dir() / (L"run-" + name + L".log"); }
fs::path previous_run_log(const std::wstring& name) { return logs_dir() / (L"run-" + name + L".1.log"); }
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

std::string wide_to_utf8(const std::wstring& w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring file_name_safe(std::wstring s) {
    for (auto& c : s) {
        if (c < 32 || wcschr(L"<>:\"/\\|?*", c)) c = L'_';
    }
    while (!s.empty() && (s.back() == L'.' || s.back() == L' ')) s.pop_back();
    return s.empty() ? L"App" : s;
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

std::wstring long_date(const FILETIME& when) {
    SYSTEMTIME utc, local;
    wchar_t buf[128] = {};
    if (!FileTimeToSystemTime(&when, &utc) || !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local) ||
        !GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_LONGDATE, &local, nullptr, buf, 128, nullptr)) {
        return tr(L"unknown");
    }
    return buf;
}

// FILETIME counts 100 ns from 1601, Unix time from 1970.
constexpr unsigned long long kUnixEpochFiletime = 116444736000000000ull;

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

// Puts `text` on the clipboard.
bool copy_text(HWND owner, const std::wstring& text) {
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
    if (!mem) return false;
    memcpy(GlobalLock(mem), text.c_str(), (text.size() + 1) * sizeof(wchar_t));
    GlobalUnlock(mem);
    if (!OpenClipboard(owner)) {
        GlobalFree(mem);
        return false;
    }
    EmptyClipboard();
    const bool ok = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
    if (!ok) GlobalFree(mem);
    CloseClipboard();
    return ok;
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

// The orientation apps open in. The settings' default is landscape, the
// shape of a PC screen: `Landscape` absent means landscape (earlier versions
// opened portrait, so an explicit 0 from them still means portrait). An app can be
// turned the other way on its own, and an app whose manifest fixes the
// orientation of its launched activity gets that whatever was picked.
enum Orientation { kOrientUnset = -1, kPortrait = 0, kLandscape = 1 };

bool landscape_default() { return setting(L"Landscape", 1) != 0; }

std::vector<std::string> package_of(const std::wstring& name);

// The user's choice for one app, library\<name>.orientation ("portrait" or
// "landscape"; beside the app's other library files, as the window size is).
fs::path orientation_file(const std::wstring& name) { return library_dir() / (name + L".orientation"); }

int app_orientation(const std::wstring& name) {
    std::ifstream f(orientation_file(name));
    std::string s;
    f >> s;
    return s == "landscape" ? kLandscape : s == "portrait" ? kPortrait : kOrientUnset;
}

void set_app_orientation(const std::wstring& name, int o) {
    std::error_code ec;
    if (o == kOrientUnset) {
        fs::remove(orientation_file(name), ec);
        return;
    }
    std::ofstream f(orientation_file(name), std::ios::trunc);
    f << (o == kLandscape ? "landscape" : "portrait") << "\n";
}

// The orientation the app's manifest fixes for its launched activity (the
// fourth line of library\<name>.package, AppMeta's ActivityInfo
// screenOrientation): the landscape and portrait families, as the runtime
// reads them (apply_screen_orientation); kOrientUnset when it fixes none, or
// the app was installed before the line was written.
int manifest_orientation(const std::wstring& name) {
    const std::vector<std::string> lines = package_of(name);
    if (lines.size() < 4) return kOrientUnset;
    const int o = std::atoi(lines[3].c_str());
    if (o == 0 || o == 6 || o == 8 || o == 11) return kLandscape;
    if (o == 1 || o == 7 || o == 9 || o == 12) return kPortrait;
    return kOrientUnset;
}

// What the app opens in: what its manifest fixes, else its own choice, else the settings'.
int effective_orientation(const std::wstring& name) {
    if (const int m = manifest_orientation(name); m != kOrientUnset) return m;
    if (const int o = app_orientation(name); o != kOrientUnset) return o;
    return landscape_default() ? kLandscape : kPortrait;
}

// The settings' screen size and the app's orientation, in dp: what the runtime
// started from here is told (apply_runtime_env), and what the starting window
// is sized by. "Auto" is 360x800 dp, turned to the orientation; `name` empty
// (an install) takes the settings' orientation.
void runtime_phone_size(const std::wstring& name, int* pw, int* ph) {
    const DWORD i = winapk::launcher::settings::choice(setting(L"PhoneSize", 0), std::size(kPhonePresets));
    const bool landscape = (name.empty() ? landscape_default() : effective_orientation(name) == kLandscape);
    int w = 360, h = 800;
    if (i > 0 && i < std::size(kPhonePresets)) {
        w = kPhonePresets[i].w;
        h = kPhonePresets[i].h;
    }
    if (landscape != (w > h)) std::swap(w, h);
    *pw = w;
    *ph = h;
}

// For the runtime started from here: it inherits this process's environment.
void apply_runtime_env(const std::wstring& name) {
    int w = 360, h = 800;
    runtime_phone_size(name, &w, &h);
    SetEnvironmentVariableW(L"WINAPK_PHONE_SIZE", (std::to_wstring(w) + L"x" + std::to_wstring(h)).c_str());
}

// ---------------------------------------------------------------------------
// Windows integration: file types and shortcuts

// A string value of HKCU\<sub> (its default value when name is null).
void set_reg_string(const std::wstring& sub, const wchar_t* name, const std::wstring& value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, sub.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) ==
        ERROR_SUCCESS) {
        RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                       static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(key);
    }
}
void set_reg_default(const std::wstring& sub, const std::wstring& value) { set_reg_string(sub, nullptr, value); }

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
    // Each file's own app icon as its thumbnail (bin\winapk-shell.dll, windows/shell/apk_thumbnail.cpp), on the
    // file type so it goes with it; per user, and run by the shell in its isolated surrogate process (no
    // DisableProcessIsolation). The installer writes the same keys (winapk.iss).
    const fs::path shell_dll = root() / L"bin" / L"winapk-shell.dll";
    std::error_code ec;
    if (fs::exists(shell_dll, ec)) {
        const std::wstring clsid = L"{B5A3C1E2-7D4F-4C8B-9E61-2F0A8D3C5B17}";
        const std::wstring server = L"Software\\Classes\\CLSID\\" + clsid;
        set_reg_default(server, L"Win.apk app file thumbnails");
        set_reg_default(server + L"\\InprocServer32", shell_dll.wstring());
        set_reg_string(server + L"\\InprocServer32", L"ThreadingModel", L"Apartment");
        // IThumbnailProvider's handler key.
        set_reg_default(L"Software\\Classes\\WinApk.Package\\ShellEx\\{e357fccd-a995-4576-b01f-234630154e96}", clsid);
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

// The AppUserModelID of an app's process and of its Start menu shortcut: the
// runtime gives its process the id (SetCurrentProcessExplicitAppUserModelID)
// and the shortcut carries it (System.AppUserModel.ID), which is how a classic
// program's notifications (toasts) get the shortcut's name and icon in the
// Action Center, and its window the shortcut's identity on the taskbar.
// Derived from the library name alone; windows/host/native_notifications.cpp
// (app_user_model_id) computes the very same string: keep them in step.
// Letters, digits, '.', '-' and '_' pass; anything else becomes '_' and a hash
// of the whole name is appended, so names that differ only there stay apart;
// at most 128 characters and no spaces, as the id's rules say.
std::wstring app_user_model_id(const std::wstring& name) {
    std::wstring id = L"WinApk.App.";
    bool mangled = false;
    uint64_t hash = 14695981039346656037ULL;  // FNV-1a
    for (wchar_t c : name) {
        hash = (hash ^ static_cast<uint64_t>(c)) * 1099511628211ULL;
        const bool plain = (c >= L'0' && c <= L'9') || (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') ||
                           c == L'.' || c == L'-' || c == L'_';
        id += plain ? c : L'_';
        mangled = mangled || !plain;
    }
    if (mangled || id.size() > 100) {
        id = id.substr(0, 100) + L'.';
        for (int i = 60; i >= 0; i -= 4) id += L"0123456789abcdef"[(hash >> i) & 0xf];
    }
    return id;
}

bool make_shortcut(const fs::path& lnk, const std::wstring& args, const fs::path& icon, const std::wstring& what,
                   const std::wstring& aumid) {
    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) return false;
    link->SetPath(exe_path().c_str());
    link->SetArguments(args.c_str());
    link->SetWorkingDirectory(root().c_str());
    link->SetDescription(what.c_str());
    if (!icon.empty()) link->SetIconLocation(icon.c_str(), 0);
    if (!aumid.empty()) {
        // System.AppUserModel.ID (propkey.h's PKEY_AppUserModel_ID, spelled out
        // so no property-system library is needed).
        static const PROPERTYKEY kAppUserModelId = {
            {0x9f4c2855, 0x9f79, 0x4b39, {0xa8, 0xd0, 0xe1, 0xd4, 0x2d, 0xe1, 0xd5, 0xf3}}, 5};
        IPropertyStore* props = nullptr;
        if (SUCCEEDED(link->QueryInterface(IID_IPropertyStore, reinterpret_cast<void**>(&props))) && props) {
            PROPVARIANT v{};
            v.vt = VT_LPWSTR;
            v.pwszVal = const_cast<wchar_t*>(aumid.c_str());  // read, not freed: no PropVariantClear
            if (SUCCEEDED(props->SetValue(kAppUserModelId, v))) props->Commit();
            props->Release();
        }
    }
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
    if (!fs::exists(self, ec)) make_shortcut(self, L"", fs::path(), L"Android apps on Windows", L"");
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
// The app's package, versionCode and versionName, one per line (AppMeta's package.txt).
fs::path package_file(const std::wstring& name) { return library_dir() / (name + L".package"); }
fs::path shortcut_icon(const std::wstring& name) { return library_dir() / (name + L".ico"); }

std::wstring label_of(const std::wstring& name) {
    std::ifstream f(label_file(name), std::ios::binary);
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::wstring w = utf8_to_wide(s);
    // One line: a label written with a line end (or edited by hand) would wrap or cut in menus and lists.
    w.erase(0, w.find_first_not_of(L" \t\r\n"));
    if (const size_t end = w.find_first_of(L"\r\n"); end != std::wstring::npos) w.resize(end);
    while (!w.empty() && (w.back() == L' ' || w.back() == L'\t')) w.pop_back();
    return w.empty() ? name : w;
}

std::vector<App> library() {
    std::vector<App> apps;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(library_dir(), ec)) {
        if (!e.is_regular_file(ec) || !is_app_file(e.path())) continue;
        const std::wstring name = e.path().stem().wstring();
        if (!fs::exists(label_file(name), ec)) continue;  // not installed (yet)
        // A bundle beside the app's .apk was being replaced by it (library_files.h): the .apk is the app.
        if (winapk::launcher::is_bundle_file(e.path()) && fs::exists(library_dir() / (name + L".apk"), ec)) continue;
        apps.push_back({name, label_of(name), e.path()});
    }
    std::sort(apps.begin(), apps.end(), [](const App& a, const App& b) {
        return CompareStringEx(LOCALE_NAME_USER_DEFAULT, NORM_IGNORECASE, a.label.c_str(), -1, b.label.c_str(), -1,
                               nullptr, nullptr, 0) == CSTR_LESS_THAN;
    });
    return apps;
}

// Google's Services Framework, Play services and Play Store are system components of Win.apk's Google services,
// and the Store's app (store_app.h) one of Win.apk itself: not apps the user opens from the library. The library,
// its tiles and the shortcuts leave them out (installed_apps.json keeps them, for the runtime and the system
// broker); Google's versions show on the Google services setup page, Google Play in the pane is the Play Store's
// one way in, and the Store in the pane the Store's.
bool google_package(const std::string& pkg) {
    return pkg == "com.google.android.gsf" || pkg == "com.google.android.gms" || pkg == "com.android.vending";
}

bool system_package(const std::string& pkg) { return google_package(pkg) || pkg == winapk::launcher::kStorePackage; }

bool system_app(const std::wstring& name) {
    const std::vector<std::string> p = package_of(name);
    return !p.empty() && system_package(p[0]);
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

void remove_app_shortcuts(const App& app);

void make_app_shortcuts(const App& app, bool desktop) {
    if (system_app(app.name)) {  // a system component has no shortcut (and loses one an older launcher made)
        remove_app_shortcuts(app);
        return;
    }
    const fs::path ico = shortcut_icon(app.name);
    const bool has_icon = write_ico(app_dir(app.name) / L"icon.png", ico);
    const std::wstring file = file_name_safe(app.label) + L".lnk";
    const std::wstring what = app.label + L" (Android app, Win.apk)";
    const std::wstring aumid = app_user_model_id(app.name);
    make_shortcut(start_menu_apps() / file, run_args(app.apk), has_icon ? ico : fs::path(), what, aumid);
    if (desktop) {
        make_shortcut(known_folder(FOLDERID_Desktop) / file, run_args(app.apk), has_icon ? ico : fs::path(), what,
                      aumid);
    }
}

void remove_app_shortcuts(const App& app) {
    std::error_code ec;
    const std::wstring file = file_name_safe(app.label) + L".lnk";
    fs::remove(start_menu_apps() / file, ec);
    fs::remove(known_folder(FOLDERID_Desktop) / file, ec);
}

// Shortcuts written by an older launcher lack the AppUserModelID, without which
// an app's notifications show no name or icon: once (the ShortcutVersion
// setting), every app's shortcuts are written again. Bump the version when
// what a shortcut holds changes. 2: Google's apps lost theirs. They are also
// written again when Start > Win.apk Apps is gone while the library has an app
// that has one there: an uninstall removes the folder and keeps the library,
// so a reinstall puts the apps back in Start, as a phone's launcher lists
// every installed app (the uninstaller also deletes ShortcutVersion).
constexpr DWORD kShortcutVersion = 2;

void refresh_app_shortcuts() {
    std::error_code ec;
    bool missing = false;
    if (!fs::exists(start_menu_apps(), ec)) {
        for (const App& app : library()) missing = missing || !system_app(app.name);
    }
    if (setting(L"ShortcutVersion", 0) >= kShortcutVersion && !missing) return;
    for (const App& app : library()) {
        const fs::path desktop = known_folder(FOLDERID_Desktop) / (file_name_safe(app.label) + L".lnk");
        make_app_shortcuts(app, fs::exists(desktop, ec));
    }
    set_setting(L"ShortcutVersion", kShortcutVersion);
}

// ---------------------------------------------------------------------------
// Running and installing

// The runtime compiles an app's arm64 code with clang (WINAPK_AOT_CLANG, Win.apk\clang, or the PATH);
// without one it falls back to the slower JIT. A clang installed elsewhere, by MSYS2 or the LLVM
// installer, is handed over so the code conversion works without changing the PATH.
void apply_aot_clang_env() {
    wchar_t have[8];
    if (GetEnvironmentVariableW(L"WINAPK_AOT_CLANG", have, 8) > 0) return;
    std::error_code ec;
    if (fs::exists(root() / L"clang" / L"bin" / L"clang.exe", ec)) return;
    if (SearchPathW(nullptr, L"clang.exe", nullptr, 0, nullptr, nullptr) > 0) return;
    std::vector<fs::path> candidates;
    for (const wchar_t* var : {L"ProgramFiles", L"ProgramW6432"}) {
        wchar_t dir[MAX_PATH];
        if (GetEnvironmentVariableW(var, dir, MAX_PATH) > 0) {
            candidates.push_back(fs::path(dir) / L"LLVM\\bin\\clang.exe");
        }
    }
    candidates.push_back(L"C:\\msys64\\clang64\\bin\\clang.exe");
    candidates.push_back(L"C:\\msys64\\ucrt64\\bin\\clang.exe");
    for (const auto& c : candidates) {
        if (fs::exists(c, ec)) {
            SetEnvironmentVariableW(L"WINAPK_AOT_CLANG", c.c_str());
            return;
        }
    }
}

// A failure of the launcher's own, before the runtime could log anything, as a line of the run's or the install's
// log: "[E] launcher: <what>: <Windows' words> (error N)". The [E] is what log_summary and the failure sentences
// read.
std::string launcher_error_line(const std::wstring& what, DWORD err) {
    wchar_t* msg = nullptr;
    std::wstring words;
    if (FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       nullptr, err, MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), reinterpret_cast<wchar_t*>(&msg), 0,
                       nullptr) ||
        FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       nullptr, err, 0, reinterpret_cast<wchar_t*>(&msg), 0, nullptr)) {
        words = msg;
        LocalFree(msg);
        while (!words.empty() && (words.back() == L'\n' || words.back() == L'\r' || words.back() == L'.'))
            words.pop_back();
    }
    return wide_to_utf8(L"[E] launcher: " + what + L": " + (words.empty() ? L"" : words + L" ") + L"(error " +
                        std::to_wstring(err) + L")\r\n");
}

// The same line as a log of its own: `log` is replaced, since no runtime wrote it this time.
void log_launcher_error(const fs::path& log, const std::wstring& what, DWORD err) {
    std::error_code ec;
    fs::create_directories(log.parent_path(), ec);
    HANDLE f = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    const std::string line = launcher_error_line(what, err);
    DWORD n = 0;
    WriteFile(f, line.data(), static_cast<DWORD>(line.size()), &n, nullptr);
    CloseHandle(f);
}

// The runtime ended with an NTSTATUS (0xC...): Windows ended it, often before it could write a line (a DLL
// that does not load stops it before main). Appended to its log, after the runtime's own lines, with what the
// status means when it is a known one.
void log_runtime_status(const fs::path& log, DWORD code) {
    if (log.empty() || code < 0xC0000000u) return;
    const wchar_t* what = code == 0xC0000135u   ? L"a DLL it needs was not found"
                          : code == 0xC0000139u ? L"a DLL it needs lacks a function (a mismatched DLL)"
                          : code == 0xC000007Bu ? L"a DLL it needs is not a valid 64-bit image"
                          : code == 0xC0000142u ? L"a DLL it needs failed to initialize"
                          : code == 0xC0000005u ? L"an access violation"
                          : code == 0xC00000FDu ? L"a stack overflow"
                          : code == 0xC0000409u ? L"a fail-fast stop (a broken check or a stack buffer overrun)"
                                                : L"ended by Windows";
    wchar_t hex[16];
    swprintf(hex, 16, L"0x%08lX", static_cast<unsigned long>(code));
    const std::string line =
        wide_to_utf8(L"[E] launcher: winapk-runtime.exe ended with " + std::wstring(hex) + L" (" + what + L")\r\n");
    HANDLE f = CreateFileW(log.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD n = 0;
    WriteFile(f, line.data(), static_cast<DWORD>(line.size()), &n, nullptr);
    CloseHandle(f);
}

// `name` is the app the runtime opens, for its orientation; empty for an install. The runtime's
// stdout and stderr go to `log` (none: NUL). `ready`, when given, is the launch handshake's event
// (win32_window.h, take_launcher_ready_event): the child gets it, and only it and the log, through
// an explicit handle list, as WINAPK_READY_EVENT in an environment block of its own, so two starts
// at once do not see each other's. `extra` are NAME=value variables for this child alone, over the ones
// the launcher has.
HANDLE start_runtime(const std::wstring& args, const fs::path& log, const std::wstring& name = L"",
                     HANDLE ready = nullptr, const std::vector<std::wstring>& extra = {}) {
    std::error_code ec;
    if (!log.empty()) fs::create_directories(log.parent_path(), ec);
    apply_runtime_env(name);
    apply_aot_clang_env();
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE out = CreateFileW(log.empty() ? L"NUL" : log.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE,
                             &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    std::vector<HANDLE> inherit;
    if (out != INVALID_HANDLE_VALUE) inherit.push_back(out);
    if (ready) inherit.push_back(ready);
    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    std::vector<BYTE> attr_buf(attr_size);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
    const bool listed = !inherit.empty() && InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size) &&
                        UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit.data(),
                                                  inherit.size() * sizeof(HANDLE), nullptr, nullptr);
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdOutput = si.StartupInfo.hStdError = out;
    if (listed) si.lpAttributeList = attrs;
    // The environment as it is now, plus the event: NUL-separated strings, sorted as Windows keeps them,
    // ending in two NULs.
    std::vector<std::wstring> vars;
    if (wchar_t* block = GetEnvironmentStringsW()) {
        for (const wchar_t* e = block; *e; e += wcslen(e) + 1) {
            if (_wcsnicmp(e, L"WINAPK_READY_EVENT=", 19) == 0) continue;
            const bool replaced = std::any_of(extra.begin(), extra.end(), [e](const std::wstring& x) {
                const size_t eq = x.find(L'=');
                return eq != std::wstring::npos && _wcsnicmp(e, x.c_str(), eq + 1) == 0;
            });
            if (!replaced) vars.emplace_back(e);
        }
        FreeEnvironmentStringsW(block);
    }
    vars.insert(vars.end(), extra.begin(), extra.end());
    if (ready) vars.push_back(L"WINAPK_READY_EVENT=" + std::to_wstring(reinterpret_cast<uintptr_t>(ready)));
    std::stable_sort(vars.begin(), vars.end(),
                     [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
    std::wstring env;
    for (const std::wstring& v : vars) env.append(v).push_back(L'\0');
    env.push_back(L'\0');
    std::wstring cmd = L"\"" + runtime_exe().wstring() + L"\" " + args;
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT |
                                       (listed ? EXTENDED_STARTUPINFO_PRESENT : 0),
                                   &env[0], root().c_str(), &si.StartupInfo, &pi);
    const DWORD err = ok ? 0 : GetLastError();
    if (listed) DeleteProcThreadAttributeList(attrs);
    if (!ok && out != INVALID_HANDLE_VALUE) {
        // Else the log stays empty: Show log would say nothing about why the app or the install did not start.
        const std::string line = launcher_error_line(L"could not start " + runtime_exe().wstring(), err);
        DWORD n = 0;
        WriteFile(out, line.data(), static_cast<DWORD>(line.size()), &n, nullptr);
    }
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    if (!ok) return nullptr;
    // The app's window may take the foreground from the splash.
    AllowSetForegroundWindow(pi.dwProcessId);
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

bool app_running(const std::wstring& name);

// Where a run of the app logs: logs\run-<name>.log, the run before it kept as run-<name>.1.log.
// last-run.log is a hard link to the newest, so "the log of the app opened last" stays one file.
// A second start of an app that is running only brings it forward: its log goes nowhere, so
// the running one's stays whole.
fs::path start_run_log(const std::wstring& name) {
    if (app_running(name)) return {};
    const fs::path log = run_log(name);
    std::error_code ec;
    fs::create_directories(logs_dir(), ec);
    MoveFileExW(log.c_str(), previous_run_log(name).c_str(), MOVEFILE_REPLACE_EXISTING);
    HANDLE f = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    DeleteFileW(last_run_log().c_str());
    CreateHardLinkW(last_run_log().c_str(), log.c_str(), nullptr);
    return log;
}

struct RunJob {
    std::wstring name;
    fs::path apk;
    std::vector<std::wstring> env;  // for this run alone: Google Play's embedding (start_play)
};

// The runs of the apps started from here (app_runs.h: the states, and which thread makes each step).
// g_proc_lock guards the table: shared to read it, exclusive to change it. The functions below are the
// only code that touches it.
SRWLOCK g_proc_lock = SRWLOCK_INIT;
winapk::launcher::RunTable g_runs;
using winapk::launcher::RunEnd;

// The library: the app's run begins. False when it has one already.
bool begin_run(const std::wstring& name) {
    AcquireSRWLockExclusive(&g_proc_lock);
    const bool begun = g_runs.begin(name);
    ReleaseSRWLockExclusive(&g_proc_lock);
    return begun;
}

// Whether the app was started from here and the library has not heard of its end yet.
bool run_active(const std::wstring& name) {
    AcquireSRWLockShared(&g_proc_lock);
    const bool active = g_runs.active(name);
    ReleaseSRWLockShared(&g_proc_lock);
    return active;
}

bool any_runs() {
    AcquireSRWLockShared(&g_proc_lock);
    const bool any = !g_runs.empty();
    ReleaseSRWLockShared(&g_proc_lock);
    return any;
}

// The process id of the app's runtime, 0 when it has none (not started from here, still starting, over).
DWORD run_pid(const std::wstring& name) {
    AcquireSRWLockShared(&g_proc_lock);
    HANDLE p = g_runs.process(name);
    const DWORD pid = p ? GetProcessId(p) : 0;  // under the lock: run_thread closes the handle after ended()
    ReleaseSRWLockShared(&g_proc_lock);
    return pid;
}

// The library, on kRunExited: how the run ended; it is forgotten.
RunEnd finish_run(const std::wstring& name) {
    AcquireSRWLockExclusive(&g_proc_lock);
    const std::optional<RunEnd> end = g_runs.finish(name);
    ReleaseSRWLockExclusive(&g_proc_lock);
    // run_thread posts kRunExited only after ended(), so there is always one; a quiet end otherwise.
    return end ? *end : RunEnd{false, true, true};
}

// Runs the app and reports to the library's window: kRunReady when its window is up (the
// handshake's event), then kRunExited. The splash waits for the first.
DWORD WINAPI run_thread(void* param) {
    std::unique_ptr<RunJob> job(static_cast<RunJob*>(param));
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE ready = CreateEventW(&sa, TRUE, FALSE, nullptr);
    AcquireSRWLockShared(&g_proc_lock);
    const bool stopped_early = g_runs.stop_requested(job->name);
    ReleaseSRWLockShared(&g_proc_lock);
    const fs::path log = stopped_early ? fs::path() : start_run_log(job->name);
    HANDLE p = stopped_early ? nullptr
                             : start_runtime(L"--apk \"" + job->apk.wstring() + L"\"", log, job->name, ready, job->env);
    DWORD code = 1;
    AcquireSRWLockExclusive(&g_proc_lock);
    if (!p) {
        g_runs.ended(job->name, false);  // it did not start, or was stopped before it did
    } else if (g_runs.attach(job->name, p)) {
        // Stopped while it was starting (the splash's X): it ends now, before its window comes up.
        TerminateProcess(p, 1);
    }
    ReleaseSRWLockExclusive(&g_proc_lock);
    if (p) {
        HANDLE both[2] = {ready, p};
        // Index 0 wins when both are set: an app that was already running signals, then exits.
        if (ready && WaitForMultipleObjects(2, both, FALSE, INFINITE) == WAIT_OBJECT_0) {
            AcquireSRWLockExclusive(&g_proc_lock);
            g_runs.ready(job->name);
            ReleaseSRWLockExclusive(&g_proc_lock);
            PostMessageW(g_main, kRunReady, 0, reinterpret_cast<LPARAM>(new std::wstring(job->name)));
        }
        WaitForSingleObject(p, INFINITE);
        GetExitCodeProcess(p, &code);
        log_runtime_status(log, code);
        AcquireSRWLockExclusive(&g_proc_lock);
        g_runs.ended_with(job->name, code);
        ReleaseSRWLockExclusive(&g_proc_lock);
        CloseHandle(p);
    }
    if (ready) CloseHandle(ready);
    PostMessageW(g_main, kRunExited, 0, reinterpret_cast<LPARAM>(new std::wstring(job->name)));
    return 0;
}

// Stops the app; one still starting (run_thread has no process yet) is stopped as soon as it has one,
// or not started at all. True when there was one to stop: however it ends is then no error.
// TerminateProcess returns before the process has ended: with `wait_ms`, this waits that long for it
// (outside g_proc_lock, on a handle of its own), so that nothing the process was still writing lands
// after the caller's next step.
bool stop_run(const std::wstring& name, DWORD wait_ms = 0) {
    HANDLE ended = nullptr;
    AcquireSRWLockExclusive(&g_proc_lock);
    const bool found = g_runs.stop(name, [&ended, wait_ms](HANDLE p) {
        if (!TerminateProcess(p, 1)) return false;
        if (wait_ms) DuplicateHandle(GetCurrentProcess(), p, GetCurrentProcess(), &ended, SYNCHRONIZE, FALSE, 0);
        return true;
    });
    ReleaseSRWLockExclusive(&g_proc_lock);
    if (ended) {
        WaitForSingleObject(ended, wait_ms);
        CloseHandle(ended);
    }
    return found;
}

// The run was asked to stop and may be over already (the splash's X), or its runtime was just ended by
// other means on the user's word (Remove, a permission taken away, a newer copy installed over it): its
// end is no error either way.
void expect_run_end(const std::wstring& name) {
    AcquireSRWLockExclusive(&g_proc_lock);
    g_runs.expect_end(name);
    ReleaseSRWLockExclusive(&g_proc_lock);
}

// A split of an install: the file, and its name in library\<name>.splits (installs.h, InstallSet).
using SplitFile = std::pair<fs::path, std::wstring>;

struct InstallJob {
    fs::path source;
    std::wstring name;
    bool temporary;     // the Store's app as Win.apk downloaded it, removed once it is in the library
    std::vector<SplitFile> splits = {};  // the other APKs of its session (several files of one app, install_files)
};

SRWLOCK g_install_lock = SRWLOCK_INIT;  // one install at a time: each one is a JVM converting the whole app

// The installs queued from here (installs.h: the states, and which thread makes each step).
// g_installs_lock guards the table: shared to read it, exclusive to change it. The functions below,
// install_into_library and install_thread are the only code that touches it.
SRWLOCK g_installs_lock = SRWLOCK_INIT;
winapk::launcher::InstallTable g_installs;
using winapk::launcher::InstallEnd;

// The library: an install is queued. False when one of that name is pending already.
bool begin_install(const std::wstring& name) {
    AcquireSRWLockExclusive(&g_installs_lock);
    const bool begun = g_installs.begin(name);
    ReleaseSRWLockExclusive(&g_installs_lock);
    return begun;
}

// Whether an install of `name` is queued or under way, or over without the library having heard.
bool is_pending(const std::wstring& name) {
    AcquireSRWLockShared(&g_installs_lock);
    const bool pending = g_installs.pending(name);
    ReleaseSRWLockShared(&g_installs_lock);
    return pending;
}

bool any_pending() {
    AcquireSRWLockShared(&g_installs_lock);
    const bool any = !g_installs.empty();
    ReleaseSRWLockShared(&g_installs_lock);
    return any;
}

// The pending installs, in the order they were queued.
std::vector<std::wstring> pending_names() {
    AcquireSRWLockShared(&g_installs_lock);
    std::vector<std::wstring> names = g_installs.names();
    ReleaseSRWLockShared(&g_installs_lock);
    return names;
}

// The install under way, empty when there is none.
std::wstring current_install() {
    AcquireSRWLockShared(&g_installs_lock);
    std::wstring name = g_installs.current();
    ReleaseSRWLockShared(&g_installs_lock);
    return name;
}

// The library, on kInstalled: how the install ended; it is forgotten.
InstallEnd finish_install(const std::wstring& name) {
    AcquireSRWLockExclusive(&g_installs_lock);
    const std::optional<InstallEnd> end = g_installs.finish(name);
    ReleaseSRWLockExclusive(&g_installs_lock);
    // install_thread posts kInstalled only after ended(), so there is always one; a failure otherwise.
    return end ? *end : InstallEnd{};
}

// Cancels the install of `name`: one still waiting does not start, the one under way is stopped and
// undone. One that is being kept already, or is over, stays as it is.
void cancel_install(const std::wstring& name) {
    AcquireSRWLockExclusive(&g_installs_lock);
    g_installs.cancel(name, [](HANDLE p) { TerminateProcess(p, 1); });
    ReleaseSRWLockExclusive(&g_installs_lock);
}

void write_registry();
fs::path registry_apk(const std::string& pkg);

// The system broker (winapk-runtime --system, with Google services on) plays Android's system_server, and
// learns a package as PackageManagerService does: at the commit. Each install, update and removal tells it
// once the registry holds the change, with the APK the registry names for the package (a removal: the one
// it named), and waits for it to take the change in (winapk/package_events.h). With no broker running this
// does nothing: a broker reads the registry when it starts.
void tell_broker(winapk::package_events::Change change, const std::string& pkg, const fs::path& removed_apk = {},
                 bool data_removed = false) {
    const fs::path apk = change == winapk::package_events::kRemoved ? removed_apk : registry_apk(pkg);
    if (pkg.empty() || apk.empty()) return;
    winapk::package_events::tell_broker(change, utf8_to_wide(pkg), apk.wstring(), data_removed,
                                        runtime_exe().wstring());
}

// Whether the app is running: winapk-runtime holds a named mutex keyed on
// the app's data folder for as long as it runs the app (claim_single_instance
// in host/runtime_main.cpp; the key is made the same way here).
std::wstring app_instance(const std::wstring& name) {
    std::error_code ec;
    std::wstring key = fs::absolute(app_dir(name), ec).lexically_normal().wstring();
    if (ec) key = app_dir(name).wstring();
    while (!key.empty() && (key.back() == L'\\' || key.back() == L'/')) key.pop_back();
    for (wchar_t& c : key) c = static_cast<wchar_t>(towlower(c == L'/' ? L'\\' : c));
    uint64_t h = 1469598103934665603ull;  // FNV-1a over the UTF-16 units
    for (const wchar_t c : key) h = (h ^ static_cast<uint16_t>(c)) * 1099511628211ull;
    wchar_t mutex[48];
    swprintf(mutex, 48, L"Local\\WinApk-app-%016llx", static_cast<unsigned long long>(h));
    return mutex;
}

bool app_running(const std::wstring& name) {
    HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, app_instance(name).c_str());
    if (!m) return false;
    const DWORD w = WaitForSingleObject(m, 0);
    if (w == WAIT_OBJECT_0 || w == WAIT_ABANDONED) ReleaseMutex(m);
    CloseHandle(m);
    return w == WAIT_TIMEOUT;
}

// Whether the app runs now: started from here (from the first moment of its start), or in a process this launcher
// did not start (its shortcut's start, another launcher's), known by its instance mutex. Every place that says an app
// runs (its badge, the count, the tooltip, the tile's menu, the details page's Stop) goes by this, through
// g_running where it is drawn.
bool app_is_running(const std::wstring& name) { return run_active(name) || app_running(name); }

void remove_app_files(const std::wstring& name);
struct GoogleApp;
const GoogleApp* google_app(const std::string& pkg);
namespace appops {
bool stop_library_app(const std::wstring& name, UINT exit_code = 1);
}

// Stops every process of the app for its update, as PackageManagerService kills an app it replaces: with exit
// code 0, so that a library that started it takes the end as the app's own close, not as "stopped with an
// error". True once none is left.
bool stop_app_for_update(const std::wstring& name) {
    if (!appops::stop_library_app(name, 0)) return !app_running(name);
    expect_run_end(name);  // the Play Store in a window of its own: stopped for its update, not "an error"
    for (int i = 0; i < 100 && app_running(name); ++i) Sleep(100);  // its mutex goes with the process
    return !app_running(name);
}

// Whether the app's running process is one the system broker started with no window, to serve another
// app's bind or broadcast: beside the instance mutex, the runtime's "-pid" mapping holds the holder's pid
// and its flags (runtime_main.cpp claim_single_instance; 1 is kHolderServiceHost).
bool runs_as_service_host(const std::wstring& name) {
    HANDLE map = OpenFileMappingW(FILE_MAP_READ, FALSE, (app_instance(name) + L"-pid").c_str());
    if (!map) return false;
    DWORD flags = 0;
    if (const auto* p = static_cast<const DWORD*>(MapViewOfFile(map, FILE_MAP_READ, 0, 0, 2 * sizeof(DWORD)))) {
        flags = p[1];
        UnmapViewOfFile(p);
    }
    CloseHandle(map);
    return (flags & 1) != 0;
}

// Android kills every process of a package before an install replaces its code (the install's freeze), and
// the install here would find a running copy's files in use. What runs with no window of the user's goes
// first: a service host the system broker started (the app's main process or another android:process of
// it), and Google's three apps, which are not in the library, so the user has no way to close them (Play
// services runs whenever an app or the Play Store uses it). An app the user has open in a window is left
// to the user, who closes it and then updates it: false.
bool stop_app_for_install(const std::wstring& name) {
    std::string pkg;
    for (const wchar_t c : name) pkg += static_cast<char>(c < 0x80 ? c : '?');
    if (app_running(name) && !google_app(pkg) && !runs_as_service_host(name)) return false;
    if (appops::stop_library_app(name))
        expect_run_end(name);  // the Play Store in a window of its own: stopped for its update, not "an error"
    for (int i = 0; i < 100 && app_running(name); ++i) Sleep(100);  // its mutex goes with the process
    return !app_running(name);
}

// An install of `name` is under way in some process (the library's, or an install helper's): a named
// mutex held from the first file moved to the last. Windows lets go of it when its process dies, so a
// mutex that can be had means no install is alive, whatever the files say.
struct InstallClaim {
    HANDLE mutex = nullptr;
    bool held = false;
    // `wait`: for the install before this one to end; else only if it is free now.
    InstallClaim(const std::wstring& name, bool wait) {
        std::wstring key = name;
        for (wchar_t& c : key) c = static_cast<wchar_t>(towlower(c));
        uint64_t h = 1469598103934665603ull;  // FNV-1a over the UTF-16 units
        for (const wchar_t c : key) h = (h ^ static_cast<uint16_t>(c)) * 1099511628211ull;
        wchar_t id[48];
        swprintf(id, 48, L"Local\\WinApk-install-%016llx", static_cast<unsigned long long>(h));
        mutex = CreateMutexW(nullptr, FALSE, id);
        if (!mutex) return;
        const DWORD w = WaitForSingleObject(mutex, wait ? INFINITE : 0);
        held = w == WAIT_OBJECT_0 || w == WAIT_ABANDONED;
    }
    ~InstallClaim() {
        if (held) ReleaseMutex(mutex);
        if (mutex) CloseHandle(mutex);
    }
    InstallClaim(const InstallClaim&) = delete;
    InstallClaim& operator=(const InstallClaim&) = delete;
};

// The install's record (android\data\app\.<name>.installed: the installed base.apk's path, in UTF-8), which
// the runtime writes once it has installed the app from its library file (runtime_main.cpp, install_app).
fs::path install_record(const std::wstring& name) {
    return data_dir() / L"android" / L"data" / L"app" / (L"." + name + L".installed");
}

// The installed copy the record names (the folder of its base.apk); empty when there is none.
fs::path installed_copy(const std::wstring& name) {
    std::ifstream f(install_record(name), std::ios::binary);
    std::string path((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (path.empty()) return {};
    const fs::path base(utf8_to_wide(path));
    std::error_code ec;
    return fs::is_regular_file(base, ec) ? base.parent_path() : fs::path();
}

// The bundle the app was installed from, replaced by hard links to its installed APKs (library_files.h)
// when its installed copy was made from this very file: AppInstall's stamp names its size and time. A copy
// made from another file (a failed update's, another library name's of the same package) is no copy of it.
// Returns the app's file now: the .apk, or the bundle when it stays.
fs::path keep_installed_apks(const std::wstring& name, const fs::path& bundle) {
    const fs::path copy = installed_copy(name);
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (copy.empty() || !GetFileAttributesExW(bundle.c_str(), GetFileExInfoStandard, &a)) return bundle;
    const unsigned long long t =
        (static_cast<unsigned long long>(a.ftLastWriteTime.dwHighDateTime) << 32) | a.ftLastWriteTime.dwLowDateTime;
    const uintmax_t size = (static_cast<uintmax_t>(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
    std::string stamp;
    {
        std::ifstream f(copy / L".installed", std::ios::binary);
        stamp.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    }
    // Java's FileTime.toMillis: milliseconds since 1970, rounded down.
    if (t < kUnixEpochFiletime ||
        !winapk::launcher::stamp_matches(stamp, size, static_cast<long long>((t - kUnixEpochFiletime) / 10000)))
        return bundle;
    const fs::path apk = winapk::launcher::keep_installed(bundle, copy);
    return apk.empty() ? bundle : apk;
}

// An update that was cut off (the process killed, the PC switched off) left the app's previous files beside
// the new ones as <entry>.previous (library_files.h), and in their place a new file that may be half copied
// and was never converted. Each such app gets its previous files (and icon) back, as a failed update does,
// and is installed again from them at its next start. A new app whose install was cut off left its file in
// the library with no name beside it, and whatever the install had made of it: those go, as after a failed
// install. An installed app still kept as a bundle (from a library older than library_files.h's layout, or
// an install cut off while its links were made) is replaced by links to its installed APKs, once its
// installed copy is of that bundle: else it stays until a start installs it again. An app whose install is
// alive in another process is left to it, and a running app as it is. True when something changed.
bool recover_interrupted_installs() {
    bool recovered = false;
    std::error_code ec;
    winapk::launcher::drop_all_taken_out(library_dir());  // a removal's files it could not delete then
    std::set<std::wstring> set_aside;
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(library_dir(), ec)) {
        if (e.path().extension() == L".previous") {
            fs::path entry = e.path();
            entry.replace_extension();  // <name><ext>
            set_aside.insert(entry.stem().wstring());
        } else if (e.is_regular_file(ec) && is_app_file(e.path())) {
            files.push_back(e.path());
        }
    }
    for (const std::wstring& name : set_aside) {
        const InstallClaim claim(name, false);
        if (!claim.held) continue;
        if (winapk::launcher::put_back(library_dir(), name)) recovered = true;
        fs::remove(install_record(name), ec);  // the installed copy may be the cut-off one's
        MoveFileExW((app_dir(name) / L"icon.png.previous").c_str(), (app_dir(name) / L"icon.png").c_str(),
                    MOVEFILE_REPLACE_EXISTING);
    }
    for (const fs::path& file : files) {
        const std::wstring name = file.stem().wstring();
        if (!fs::exists(file, ec)) continue;  // a cut-off update's, removed above
        if (fs::exists(label_file(name), ec)) {  // installed
            if (!winapk::launcher::is_bundle_file(file)) continue;
            const InstallClaim claim(name, false);
            if (!claim.held || app_running(name)) continue;
            const fs::path kept = keep_installed_apks(name, file);
            if (kept == file) continue;
            recovered = true;
            const App app{name, label_of(name), kept};  // its shortcuts named the bundle
            const fs::path desktop = known_folder(FOLDERID_Desktop) / (file_name_safe(app.label) + L".lnk");
            make_app_shortcuts(app, fs::exists(desktop, ec));
            continue;
        }
        const InstallClaim claim(name, false);
        if (!claim.held) continue;
        for (const fs::path& p : winapk::launcher::app_entries(library_dir(), name, false)) fs::remove_all(p, ec);
        recovered = true;
        remove_app_files(name);
    }
    return recovered;
}

// Copies `source` into the library as <name><ext> and installs it there: the install, the code conversion,
// the name, icon and package, the shortcuts. A bundle is then replaced by links to its installed APKs, so
// the library keeps the app once (library_files.h). The app's files from before are set aside first and
// removed once the new one is in, whatever its extension was. An app open in a window is left alone (its
// files are in use): false, with `*running` set; what runs of it with no window is stopped first
// (stop_app_for_install). An update that fails or is cancelled puts the previous files back; a new app that
// does leaves nothing behind. Once the registry has the app, a running system broker is told (tell_broker).
// `splits` are the other APKs of the session when the app came as loose files (install_files): copied in as
// library\<name>.splits\, the layout AppInstall installs a kept bundle from, so the app is installed whole.
bool install_into_library(const fs::path& source, const std::wstring& name, bool* running,
                          const std::vector<SplitFile>& splits = {}) {
    *running = !stop_app_for_install(name);
    if (*running) return false;
    const InstallClaim claim(name, true);  // until this returns: see recover_interrupted_installs
    std::error_code ec;
    fs::create_directories(library_dir(), ec);
    const fs::path target = library_dir() / (name + source.extension().wstring());
    const bool update = fs::exists(label_file(name), ec);
    // The library's own file again (opened from the library folder): installed again as it is, with its splits.
    const bool same = fs::equivalent(source, target, ec);
    // The install must run: a record newer than the incoming file's date (a copy keeps the date it was
    // downloaded on) would have the runtime take the installed copy for this file's.
    fs::remove(install_record(name), ec);
    // Never a write over a kept file: it is the installed APK itself (library_files.h).
    bool ok = same;
    // The runtime does not run when these fail, so nothing else would write this install's log.
    if (!ok && !winapk::launcher::set_aside(library_dir(), name)) {
        log_launcher_error(install_log(name), L"could not set the app's present files aside in " +
                                                  library_dir().wstring(), GetLastError());
    } else if (!ok && !(ok = CopyFileW(source.c_str(), target.c_str(), TRUE) != 0)) {
        log_launcher_error(install_log(name), L"could not copy " + source.wstring() + L" to " + target.wstring(),
                           GetLastError());
    }
    if (ok && !same && !splits.empty()) {
        const fs::path dir = library_dir() / (name + L".splits");
        fs::create_directories(dir, ec);
        for (const SplitFile& f : splits) {
            const fs::path to = dir / f.second;
            if (!CopyFileW(f.first.c_str(), to.c_str(), TRUE)) {
                log_launcher_error(install_log(name), L"could not copy " + f.first.wstring() + L" to " + to.wstring(),
                                   GetLastError());
                ok = false;
                break;
            }
        }
    }
    // The install writes the new version's icon first (so that it shows while the code is converted):
    // an update keeps the app's own beside it, to put back if the update does not go through.
    const fs::path icon = app_dir(name) / L"icon.png", icon_kept = app_dir(name) / L"icon.png.previous";
    const bool icon_backed_up = update && CopyFileW(icon.c_str(), icon_kept.c_str(), FALSE) != 0;
    if (ok) {
        HANDLE p = start_runtime(L"--apk \"" + target.wstring() + L"\" --install-only", install_log(name));
        DWORD code = 1;
        if (p) {
            AcquireSRWLockExclusive(&g_installs_lock);
            if (g_installs.attach(name, p)) TerminateProcess(p, 1);  // cancelled while it was starting
            ReleaseSRWLockExclusive(&g_installs_lock);
            WaitForSingleObject(p, INFINITE);
            GetExitCodeProcess(p, &code);
            log_runtime_status(install_log(name), code);
            AcquireSRWLockExclusive(&g_installs_lock);
            g_installs.detach(name);
            ReleaseSRWLockExclusive(&g_installs_lock);
            CloseHandle(p);
        }
        *running = code == 3;  // the app was started meanwhile
        // commit(): kept unless it was cancelled, and from here on a cancel is refused.
        AcquireSRWLockExclusive(&g_installs_lock);
        const bool keep = code == 0 && g_installs.commit(name);
        ReleaseSRWLockExclusive(&g_installs_lock);
        // An install or update of the app ends its task, as Android force-stops a replaced package and
        // drops its activities: nothing of the task the runtime kept for the previous copy's next start
        // comes back (task_state_file). The runtime's own version check stays as a backstop.
        if (keep) fs::remove(task_state_file(name), ec);
        ok = keep &&
             MoveFileExW((app_dir(name) / L"label.txt").c_str(), label_file(name).c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED) != 0;
        if (ok) MoveFileExW((app_dir(name) / L"package.txt").c_str(), package_file(name).c_str(),
                            MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED);
    }
    if (icon_backed_up && !ok) MoveFileExW(icon_kept.c_str(), icon.c_str(), MOVEFILE_REPLACE_EXISTING);
    else if (icon_backed_up) fs::remove(icon_kept, ec);
    if (ok) {
        const fs::path kept = winapk::launcher::is_bundle_file(target) ? keep_installed_apks(name, target) : target;
        winapk::launcher::drop_previous(library_dir(), name);
        make_app_shortcuts({name, label_of(name), kept}, setting(L"DesktopShortcut", 1) != 0);
    } else if (!same) {
        // Installed again from the files put back at their next start: the installed copy may be this one's.
        fs::remove(install_record(name), ec);
        if (!winapk::launcher::put_back(library_dir(), name)) {
            for (const fs::path& p : winapk::launcher::app_entries(library_dir(), name, false))
                fs::remove_all(p, ec);
            if (!update) remove_app_files(name);
        }
    }
    write_registry();
    if (ok) {
        const std::vector<std::string> pkg = package_of(name);
        if (!pkg.empty() && !pkg[0].empty())
            tell_broker(update ? winapk::package_events::kReplaced : winapk::package_events::kAdded, pkg[0]);
    }
    return ok;
}

DWORD WINAPI install_thread(void* param) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::unique_ptr<InstallJob> job(static_cast<InstallJob*>(param));
    AcquireSRWLockExclusive(&g_install_lock);
    bool ok = false, running = false;
    AcquireSRWLockExclusive(&g_installs_lock);
    const bool started = g_installs.start(job->name);  // false: cancelled while it waited
    ReleaseSRWLockExclusive(&g_installs_lock);
    if (started) {
        PostMessageW(g_main, kInstallStarted, 0, reinterpret_cast<LPARAM>(new std::wstring(job->name)));
        ok = install_into_library(job->source, job->name, &running, job->splits);
    }
    AcquireSRWLockExclusive(&g_installs_lock);
    g_installs.ended(job->name, ok, running);
    ReleaseSRWLockExclusive(&g_installs_lock);
    ReleaseSRWLockExclusive(&g_install_lock);
    std::error_code ec;
    if (job->temporary) fs::remove(job->source, ec);
    PostMessageW(g_main, kInstalled, 0, reinterpret_cast<LPARAM>(new std::wstring(job->name)));
    CoUninitialize();
    return 0;
}

// ---------------------------------------------------------------------------
// HTTP: WinHTTP downloads, and a reader for the JSON GitHub answers with

// A WinHTTP session; `agent` is the User-Agent its requests send.
HINTERNET open_http_session(const wchar_t* agent) {
    HINTERNET s = WinHttpOpen(agent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) {  // before Windows 8.1
        s = WinHttpOpen(agent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (s) {
        DWORD decompress = WINHTTP_DECOMPRESSION_FLAG_ALL;  // GitHub's JSON comes gzipped
        WinHttpSetOption(s, WINHTTP_OPTION_DECOMPRESSION, &decompress, sizeof(decompress));
        DWORD per_server = 8;
        WinHttpSetOption(s, WINHTTP_OPTION_MAX_CONNS_PER_SERVER, &per_server, sizeof(per_server));
        WinHttpSetTimeouts(s, 15000, 15000, 30000, 60000);
    }
    return s;
}

// The default WinHTTP session, for a request that names none.
HINTERNET http_session() {
    static const HINTERNET session = open_http_session(L"Win.apk");
    return session;
}

// GETs `url` into `out`, through <out>.part so a broken download never looks
// whole. With an `etag` the server may answer 304, not modified. `progress`
// gets the bytes so far and the total (0 when unknown); `headers` are more
// request headers ("Name: value", CRLF between); `session` defaults to
// http_session(). Returns the HTTP status, 0 when the server could not be reached or
// the body broke off.
int http_get(const std::wstring& url, const fs::path& out, const std::wstring& etag, std::wstring* new_etag,
             const std::function<void(uint64_t, uint64_t)>& progress, const std::wstring& headers = L"",
             HINTERNET session = nullptr) {
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    uc.dwHostNameLength = uc.dwUrlPathLength = uc.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return 0;
    const std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
    const std::wstring path = std::wstring(uc.lpszUrlPath, uc.dwUrlPathLength) +
                              std::wstring(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    if (!session) session = http_session();
    if (!session) return 0;
    HINTERNET connection = WinHttpConnect(session, host.c_str(), uc.nPort, 0);
    HINTERNET request = connection ? WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                        WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                        uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                                   : nullptr;
    int status = 0;
    std::wstring header = etag.empty() ? L"" : L"If-None-Match: " + etag;
    if (!headers.empty()) header += (header.empty() ? L"" : L"\r\n") + headers;
    if (request &&
        WinHttpSendRequest(request, header.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : header.c_str(),
                           header.empty() ? 0 : static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr)) {
        DWORD code = 0, size = sizeof(code);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX);
        status = static_cast<int>(code);
        if (status == 200) {
            // What the body must come to (updates.h expected_body): the framing headers read as text, whole (a
            // number query stops at 4 GB, and a repeated header comes joined with commas). The session inflates
            // gzip and deflate, so an encoded body's Content-Length is not the bytes read.
            auto header = [request](DWORD query) {
                DWORD bytes = 0;
                WinHttpQueryHeaders(request, query, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &bytes,
                                    WINHTTP_NO_HEADER_INDEX);
                if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes == 0 || bytes > 64 * 1024)
                    return std::wstring();
                std::wstring s(bytes / sizeof(wchar_t) + 1, L'\0');
                if (!WinHttpQueryHeaders(request, query, WINHTTP_HEADER_NAME_BY_INDEX, s.data(), &bytes,
                                         WINHTTP_NO_HEADER_INDEX))
                    return std::wstring();
                s.resize(bytes / sizeof(wchar_t));
                return s;
            };
            const winapk::launcher::BodyLength body = winapk::launcher::expected_body(
                header(WINHTTP_QUERY_CONTENT_LENGTH), header(WINHTTP_QUERY_CONTENT_ENCODING),
                header(WINHTTP_QUERY_TRANSFER_ENCODING), /*decoded=*/true);
            const uint64_t total = body.exact() ? body.bytes : 0;  // for progress: 0 when unknown
            if (new_etag) {
                wchar_t tag[256] = {};
                size = sizeof(tag);
                if (WinHttpQueryHeaders(request, WINHTTP_QUERY_ETAG, WINHTTP_HEADER_NAME_BY_INDEX, tag, &size,
                                        WINHTTP_NO_HEADER_INDEX)) {
                    *new_etag = tag;
                }
            }
            std::error_code ec;
            fs::create_directories(out.parent_path(), ec);
            const fs::path part = out.wstring() + L".part";
            HANDLE f =
                CreateFileW(part.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            bool ok = f != INVALID_HANDLE_VALUE && body.kind != winapk::launcher::BodyLength::kBroken;
            uint64_t got = 0;
            std::vector<char> buf(1 << 16);
            while (ok) {
                DWORD n = 0;
                ok = WinHttpReadData(request, buf.data(), static_cast<DWORD>(buf.size()), &n) != 0;
                if (!ok || n == 0) break;
                got += n;
                // More than the server announced: not the file asked for; stop reading it at once.
                if (body.exact() && got > body.bytes) ok = false;
                DWORD written = 0;
                ok = ok && WriteFile(f, buf.data(), n, &written, nullptr) && written == n;
                if (ok && progress) progress(got, total);
            }
            if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
            // Kept only whole: never empty, and at the announced length when there is one (a connection closed
            // early ends a read without an error).
            ok = ok && got > 0 && body.whole(got) &&
                 MoveFileExW(part.c_str(), out.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
            if (!ok) {
                fs::remove(part, ec);
                status = 0;
            }
        }
    }
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    return status;
}

// The file's SHA-256 in lower-case hex, as GitHub's asset digests give it.
std::string sha256_of(const fs::path& file) {
    std::string out;
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    HANDLE f = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (f != INVALID_HANDLE_VALUE &&
        BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) &&
        BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0))) {
        std::vector<UCHAR> buf(1 << 16);
        DWORD n = 0;
        bool ok = true;
        while (ok && ReadFile(f, buf.data(), static_cast<DWORD>(buf.size()), &n, nullptr) && n > 0) {
            ok = BCRYPT_SUCCESS(BCryptHashData(hash, buf.data(), n, 0));
        }
        UCHAR digest[32];
        if (ok && BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, sizeof(digest), 0))) {
            for (const UCHAR b : digest) {
                char hex[3];
                std::snprintf(hex, sizeof hex, "%02x", b);
                out += hex;
            }
        }
    }
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    return out;
}

// A pull parser for the JSON the launcher reads (GitHub's releases, the
// installed-apps registry): it walks the text once without building a tree,
// and the callers keep only the fields they need.
struct Json {
    const char* p;
    const char* end;
    bool ok = true;

    void ws() {
        while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) ++p;
    }
    bool eat(char c) {
        ws();
        if (p >= end || *p != c) return false;
        ++p;
        return true;
    }
    unsigned hex4() {
        unsigned v = 0;
        for (int i = 0; i < 4; ++i, ++p) {
            if (p >= end || !isxdigit(static_cast<unsigned char>(*p))) {
                ok = false;
                return 0;
            }
            const char c = *p;
            v = v * 16 + static_cast<unsigned>(c >= '0' && c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
        }
        return v;
    }
    static void put_utf8(std::string* out, unsigned cp) {
        if (cp < 0x80) {
            *out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            *out += static_cast<char>(0xC0 | (cp >> 6));
            *out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            *out += static_cast<char>(0xE0 | (cp >> 12));
            *out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            *out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            *out += static_cast<char>(0xF0 | (cp >> 18));
            *out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            *out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            *out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    // A string with its escapes decoded; `out` may be null to skip it.
    void str(std::string* out) {
        if (!eat('"')) {
            ok = false;
            return;
        }
        while (ok && p < end && *p != '"') {
            if (*p != '\\') {
                if (out) *out += *p;
                ++p;
                continue;
            }
            if (++p >= end) break;
            const char e = *p++;
            if (e == 'u') {
                unsigned cp = hex4();
                if (cp >= 0xD800 && cp < 0xDC00 && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                    p += 2;
                    const unsigned lo = hex4();
                    cp = lo >= 0xDC00 && lo < 0xE000 ? 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00) : 0xFFFD;
                }
                if (out) put_utf8(out, cp);
            } else if (out) {
                *out += e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e == 'b' ? '\b' : e == 'f' ? '\f' : e;
            }
        }
        if (p >= end) ok = false;
        else ++p;
    }
    // A string, number, true, false or null, as its text.
    void scalar(std::string* out) {
        ws();
        if (p < end && *p == '"') return str(out);
        const char* from = p;
        while (p < end && !strchr(",}] \n\r\t", *p)) ++p;
        if (p == from) ok = false;
        else if (out) out->assign(from, p);
    }
    template <class Each>
    void object(Each each) {
        if (!eat('{')) {
            ok = false;
            return;
        }
        if (eat('}')) return;
        std::string key;
        do {
            key.clear();
            str(&key);
            if (!ok || !eat(':')) {
                ok = false;
                return;
            }
            each(key);
        } while (ok && eat(','));
        if (ok && !eat('}')) ok = false;
    }
    template <class Each>
    void array(Each each) {
        if (!eat('[')) {
            ok = false;
            return;
        }
        if (eat(']')) return;
        do each();
        while (ok && eat(','));
        if (ok && !eat(']')) ok = false;
    }
    void skip() {
        ws();
        if (p < end && *p == '{') object([this](const std::string&) { skip(); });
        else if (p < end && *p == '[') array([this] { skip(); });
        else scalar(nullptr);
    }
};

// ---------------------------------------------------------------------------
// Software updates: the newest release on GitHub and its installer
//
// Releases are pre-releases tagged v0.2.0-preview, so /releases/latest (which
// skips them) will not do: the list is read and its highest version taken
// (updates.h pick_release: never a draft, a pre-release only while there is no
// full release). Its installer is downloaded only with GitHub's SHA-256 of it,
// and runs only when its size, SHA-256, signature and version are the
// release's (updates.h judge_setup), also a copy downloaded before.
// The request says nothing about the user, and its answer is kept with its ETag:
// a check that finds nothing new costs GitHub a 304, and the library shows the
// last result at start without asking again within the day.

constexpr wchar_t kReleasesUrl[] = L"https://api.github.com/repos/israel2176/win.apk/releases?per_page=20";
constexpr wchar_t kReleasesPage[] = L"https://github.com/israel2176/win.apk/releases";
constexpr wchar_t kSetupAsset[] = L"Win.apk-Setup.exe";
constexpr DWORD kUpdateInterval = 24 * 3600;  // seconds between automatic checks

fs::path updates_dir() { return data_dir() / L"updates"; }
fs::path releases_file() { return updates_dir() / L"releases.json"; }
fs::path releases_etag_file() { return updates_dir() / L"releases.etag"; }
// The installer of one version, so that a download kept from an older release is never run for a newer one.
fs::path setup_file(const std::wstring& v) {
    return updates_dir() / (L"Win.apk-Setup-" + file_name_safe(v) + L".exe");
}

// GitHub's API wants a User-Agent; this one names the program and nothing about the user.
HINTERNET updater_session() {
    static const HINTERNET session = open_http_session(L"Win.apk-updater");
    return session;
}

using winapk::launcher::compare_versions;  // updates.h: the version rules, tested there

// Release notes in Markdown as plain text: no headings' #, no ** or `, "• " for list items,
// [text](link) as its text, and no runs of blank lines.
std::string markdown_to_text(const std::string& md) {
    std::string out;
    size_t at = 0;
    int blanks = 0;
    while (at <= md.size()) {
        size_t end = md.find('\n', at);
        if (end == std::string::npos) end = md.size();
        std::string line = md.substr(at, end - at);
        at = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t lead = line.find_first_not_of(" \t");
        if (lead == std::string::npos) {
            if (!out.empty() && blanks++ == 0) out += '\n';
            continue;
        }
        blanks = 0;
        std::string indent(lead > 0 ? 2 * ((lead + 1) / 2) : 0, ' ');
        line.erase(0, lead);
        if (line[0] == '#') {
            line.erase(0, line.find_first_not_of('#'));
        } else if ((line[0] == '-' || line[0] == '*' || line[0] == '+') && line.size() > 1 && line[1] == ' ') {
            line = "\xE2\x80\xA2 " + line.substr(2);  // a bullet
        } else if (line.rfind("---", 0) == 0 && line.find_first_not_of('-') == std::string::npos) {
            continue;  // a rule
        }
        std::string plain;
        for (size_t i = 0; i < line.size(); ++i) {
            if (line.compare(i, 2, "**") == 0 || line.compare(i, 2, "__") == 0) {
                ++i;
                continue;
            }
            if (line[i] == '`') continue;
            if (line[i] == '[') {  // [text](url) keeps the text
                const size_t close = line.find("](", i);
                const size_t paren = close == std::string::npos ? close : line.find(')', close);
                if (paren != std::string::npos) {
                    plain += line.substr(i + 1, close - i - 1);
                    i = paren;
                    continue;
                }
            }
            plain += line[i];
        }
        const size_t first = plain.find_first_not_of(' ');
        if (first == std::string::npos) continue;
        if (!out.empty()) out += '\n';
        out += indent + plain.substr(first);
    }
    while (!out.empty() && out.back() == '\n') out.pop_back();
    return out;
}

struct Release {
    std::wstring tag, version, notes, page;  // version: the tag without its v; notes: plain text
    std::wstring url;                        // Win.apk-Setup.exe's download, empty when there is none to check
    std::string sha256;                      // GitHub's SHA-256 of the asset, lower-case hex
    bool unchecked = false;                  // the release has an installer, but GitHub gives no SHA-256 of it
    uint64_t size = 0;                       // the asset's bytes
    FILETIME published{};
};

// "2026-10-01T12:00:00Z" as a FILETIME; zero when it is not one.
FILETIME iso_filetime(const std::string& s) {
    SYSTEMTIME st{};
    FILETIME ft{};
    unsigned y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
    if (std::sscanf(s.c_str(), "%4u-%2u-%2uT%2u:%2u:%2u", &y, &mo, &d, &h, &mi, &se) == 6) {
        st = {static_cast<WORD>(y), static_cast<WORD>(mo), 0, static_cast<WORD>(d),
              static_cast<WORD>(h), static_cast<WORD>(mi), static_cast<WORD>(se), 0};
        if (!SystemTimeToFileTime(&st, &ft)) ft = {};
    }
    return ft;
}

// The release to offer from GitHub's list (updates.h pick_release), with its installer asset when GitHub lists
// a SHA-256 of it; `found` says whether there is one. Returns whether the list could be read.
bool newest_release(const std::string& json, Release* best, bool* found_out) {
    using winapk::launcher::ListedRelease;
    Json j{json.data(), json.data() + json.size()};
    auto text = [&j](std::string* out) {  // a string, or skips a null
        j.ws();
        if (j.p < j.end && *j.p == '"') j.str(out);
        else j.skip();
    };
    std::vector<Release> releases;
    std::vector<ListedRelease> listed;
    j.array([&] {
        Release r;
        ListedRelease l;
        std::wstring url;
        j.object([&](const std::string& key) {
            std::string v;
            if (key == "draft" || key == "prerelease") {
                j.scalar(&v);
                (key == "draft" ? l.draft : l.prerelease) = v == "true";
            } else if (key == "tag_name") {
                text(&v);
                r.tag = l.tag = utf8_to_wide(v);
            } else if (key == "body") {
                text(&v);
                r.notes = utf8_to_wide(markdown_to_text(v));
            } else if (key == "html_url") {
                text(&v);
                r.page = utf8_to_wide(v);
            } else if (key == "published_at") {
                text(&v);
                r.published = iso_filetime(v);
            } else if (key == "assets") {
                j.array([&] {
                    std::string name, link, size, digest, state;
                    j.object([&](const std::string& k) {
                        if (k == "name") text(&name);
                        else if (k == "browser_download_url") text(&link);
                        else if (k == "size") j.scalar(&size);
                        else if (k == "digest") text(&digest);
                        else if (k == "state") text(&state);
                        else j.skip();
                    });
                    if (utf8_to_wide(name) == kSetupAsset && !link.empty() && !l.asset) {
                        l.asset = true;
                        l.asset_state = state;
                        l.digest = digest;
                        l.size = std::strtoull(size.c_str(), nullptr, 10);
                        url = utf8_to_wide(link);
                    }
                });
            } else {
                j.skip();
            }
        });
        if (!j.ok) return;
        r.version = r.tag.substr(!r.tag.empty() && (r.tag[0] == L'v' || r.tag[0] == L'V') ? 1 : 0);
        if (winapk::launcher::installer_checkable(l)) {
            r.url = url;
            r.sha256 = winapk::launcher::asset_sha256(l.digest);
            r.size = l.size;
        } else {
            r.unchecked = l.asset;  // an installer Win.apk cannot check is not downloaded: its page is offered
        }
        releases.push_back(std::move(r));
        listed.push_back(std::move(l));
    });
    const int pick = j.ok ? winapk::launcher::pick_release(listed) : -1;
    if (pick >= 0) *best = std::move(releases[static_cast<size_t>(pick)]);
    *found_out = pick >= 0;
    return j.ok;
}

// One check, from the library's thread to the update thread and back (kUpdateChecked): what was asked
// and what came of it (updates.h), and the newest release when one was found.
struct UpdateCheck : winapk::launcher::CheckResult {
    Release latest;
};

DWORD WINAPI update_check_thread(void* param) {
    std::unique_ptr<UpdateCheck> c(static_cast<UpdateCheck*>(param));
    std::error_code ec;
    const bool have = fs::exists(releases_file(), ec);
    bool read = have && !c->fetch;
    if (c->fetch) {
        std::wstring etag, new_etag;
        if (have) {
            std::ifstream f(releases_etag_file(), std::ios::binary);
            std::string s;
            std::getline(f, s);
            etag = utf8_to_wide(s);
        }
        const int status = http_get(kReleasesUrl, releases_file(), etag, &new_etag, {},
                                    L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28",
                                    updater_session());
        if (status == 200) {
            std::ofstream(releases_etag_file(), std::ios::binary | std::ios::trunc) << wide_to_utf8(new_etag);
        }
        c->reached = status == 200 || (status == 304 && have);
        read = c->reached || have;  // unreached, the answer kept still shows a newer version found before
    }
    if (read) {
        std::ifstream f(releases_file(), std::ios::binary);
        const std::string json((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        c->ok = newest_release(json, &c->latest, &c->found);
    }
    UpdateCheck* out = c.release();
    if (!PostMessageW(g_main, kUpdateChecked, 0, reinterpret_cast<LPARAM>(out))) delete out;
    return 0;
}

// A file's Authenticode signature, as WinVerifyTrust judges it with the generic verify action (WINTRUST_ACTION_
// GENERIC_VERIFY_V2), and who signed it when it verifies. No revocation check and nothing fetched from the network
// (WTD_CACHE_ONLY_URL_RETRIEVAL): the answer does not wait on a CRL server. Per WinVerifyTrust's documentation,
// TRUST_E_NOSIGNATURE, TRUST_E_SUBJECT_FORM_UNKNOWN and TRUST_E_PROVIDER_UNKNOWN mean the file carries no
// signature this provider knows; every other failure (a bad digest, an untrusted root, an explicit distrust, a
// signature policy) is a signature that does not hold. The signer is the leaf certificate's subject and issuer as
// X.500 names: a name alone could be had from another authority, a thumbprint would change at every renewal.
winapk::launcher::Signature authenticode(const fs::path& file, std::wstring* signer) {
    using winapk::launcher::Signature;
    if (signer) signer->clear();
    const std::wstring path = file.wstring();
    WINTRUST_FILE_INFO info{};
    info.cbStruct = sizeof(info);
    info.pcwszFilePath = path.c_str();
    WINTRUST_DATA data{};
    data.cbStruct = sizeof(data);
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_NONE;
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &info;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_REVOCATION_CHECK_NONE;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG status = WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
    Signature out;
    if (status == ERROR_SUCCESS) {
        out = Signature::kValid;
    } else if (status == TRUST_E_NOSIGNATURE || status == TRUST_E_SUBJECT_FORM_UNKNOWN ||
               status == TRUST_E_PROVIDER_UNKNOWN) {
        out = Signature::kUnsigned;
    } else {
        out = Signature::kInvalid;
    }
    if (out == Signature::kValid && signer) {
        // The state WinVerifyTrust kept (WTD_STATEACTION_VERIFY) holds the chain it built: its first signer's
        // first certificate is the one that signed.
        CRYPT_PROVIDER_DATA* prov = WTHelperProvDataFromStateData(data.hWVTStateData);
        CRYPT_PROVIDER_SGNR* sgnr = prov ? WTHelperGetProvSignerFromChain(prov, 0, FALSE, 0) : nullptr;
        const CERT_CONTEXT* cert = sgnr && sgnr->csCertChain > 0 && sgnr->pasCertChain
                                       ? sgnr->pasCertChain[0].pCert
                                       : nullptr;
        auto name = [cert](DWORD flags) {
            if (!cert) return std::wstring();
            DWORD type = CERT_X500_NAME_STR | CERT_NAME_STR_REVERSE_FLAG;
            const DWORD n = CertGetNameStringW(cert, CERT_NAME_RDN_TYPE, flags, &type, nullptr, 0);
            if (n <= 1) return std::wstring();
            std::wstring s(n, L'\0');
            CertGetNameStringW(cert, CERT_NAME_RDN_TYPE, flags, &type, s.data(), n);
            s.resize(n - 1);
            return s;
        };
        const std::wstring subject = name(0), issuer = name(CERT_NAME_ISSUER_FLAG);
        if (subject.empty() || issuer.empty()) {
            out = Signature::kInvalid;  // verified, yet with no certificate to name: not a signer to trust
        } else {
            *signer = subject + L" | " + issuer;
        }
    }
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
    return out;
}

// Who signed Win.apk.exe itself; empty when this build is not code-signed (no build is, so far). Asked once.
const std::wstring& own_signer() {
    static const std::wstring who = [] {
        std::wstring self(32768, L'\0');
        const DWORD n = GetModuleFileNameW(nullptr, self.data(), static_cast<DWORD>(self.size()));
        if (n == 0 || n >= self.size()) return std::wstring();
        self.resize(n);
        std::wstring name;
        return authenticode(self, &name) == winapk::launcher::Signature::kValid ? name : std::wstring();
    }();
    return who;
}

// The ProductVersion string of a file's version resource; empty when it has none. The languages the resource
// lists (\VarFileInfo\Translation) are tried in order, then US English in Unicode and in Windows-1252, which is
// how resources without a translation table are usually labelled.
std::wstring product_version(const fs::path& file) {
    DWORD unused = 0;
    const DWORD size = GetFileVersionInfoSizeW(file.c_str(), &unused);
    if (size == 0) return {};
    std::vector<BYTE> block(size);
    if (!GetFileVersionInfoW(file.c_str(), 0, size, block.data())) return {};
    std::vector<DWORD> langs;
    struct LangCodepage {
        WORD lang, codepage;
    };
    LangCodepage* table = nullptr;
    UINT bytes = 0;
    if (VerQueryValueW(block.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&table), &bytes) &&
        table) {
        for (UINT i = 0; i < bytes / sizeof(LangCodepage); ++i)
            langs.push_back(static_cast<DWORD>(table[i].lang) << 16 | table[i].codepage);
    }
    langs.push_back(0x040904b0);
    langs.push_back(0x040904e4);
    for (DWORD lc : langs) {
        wchar_t key[64];
        swprintf(key, 64, L"\\StringFileInfo\\%08lx\\ProductVersion", static_cast<unsigned long>(lc));
        wchar_t* value = nullptr;
        UINT chars = 0;
        if (VerQueryValueW(block.data(), key, reinterpret_cast<void**>(&value), &chars) && value && chars > 0) {
            std::wstring s(value, chars);
            while (!s.empty() && s.back() == L'\0') s.pop_back();
            return s;
        }
    }
    return {};
}

// Whether the installer at `file` may run as the update to `r` (updates.h judge_setup). Its size is looked at
// first: a file of another size is refused without being hashed or verified.
winapk::launcher::SetupVerdict judge_setup_file(const fs::path& file, const Release& r) {
    winapk::launcher::SetupFacts f;
    std::error_code ec;
    f.size = fs::file_size(file, ec);
    if (ec) f.size = 0;
    if (f.size != 0 && f.size == r.size) {
        f.sha256 = sha256_of(file);
        if (!f.sha256.empty() && f.sha256 == r.sha256) {
            f.signature = authenticode(file, &f.signer);
            f.product_version = product_version(file);
        }
    }
    return winapk::launcher::judge_setup(f, r.size, r.sha256, r.version, own_signer());
}

// A copy kept from before is checked again as a download is; if it does not hold, or there is none, the release's
// Win.apk-Setup.exe is downloaded to updates\ and kept only when judge_setup says it may run. wParam of
// kUpdateDownloaded: 0 when the download broke off, else the verdict + 1.
DWORD WINAPI update_download_thread(void* param) {
    std::unique_ptr<Release> r(static_cast<Release*>(param));
    using winapk::launcher::SetupVerdict;
    const fs::path file = setup_file(r->version);
    std::error_code ec;
    if (fs::exists(file, ec)) {
        const SetupVerdict kept = judge_setup_file(file, *r);
        if (winapk::launcher::setup_runs(kept)) {
            PostMessageW(g_main, kUpdateDownloaded, static_cast<WPARAM>(kept) + 1, 0);
            return 0;
        }
        fs::remove(file, ec);
    }
    int shown = -1;
    const int status = http_get(r->url, file, L"", nullptr, [&](uint64_t got, uint64_t total) {
        if (!total) total = r->size;
        const int pct = total ? static_cast<int>((std::min)(got * 100 / total, uint64_t{99})) : 0;
        if (pct == shown) return;
        shown = pct;
        PostMessageW(g_main, kUpdateProgress, static_cast<WPARAM>(pct), 0);
    }, L"", updater_session());
    WPARAM result = 0;
    if (status == 200) {
        const SetupVerdict v = judge_setup_file(file, *r);
        result = static_cast<WPARAM>(v) + 1;
        if (!winapk::launcher::setup_runs(v)) fs::remove(file, ec);
    } else {
        fs::remove(file, ec);
    }
    PostMessageW(g_main, kUpdateDownloaded, result, 0);
    return 0;
}

// ---------------------------------------------------------------------------
// Theme and drawing
//
// Spacing follows a 4 px scale (4, 8, 12, 16, 24, 32), radii are kRadius*,
// and every colour is one of the theme's tokens, so light and dark stay
// consistent. Everything is in device-independent pixels through px().

constexpr float kRadiusControl = 6, kRadiusCard = 8, kRadiusTile = 10;

struct Theme {
    bool dark;
    Color bg;           // the window: the navigation pane and the status bar
    Color layer;        // the content area, a surface over bg (Windows 11's layer)
    Color card;         // cards and controls on the layer
    Color hover;        // a hovered control or tile
    Color border;       // outlines of cards and controls
    Color divider;      // hairlines inside a card
    Color text, text2;  // primary and secondary text
    Color text3;        // placeholders and disabled text
    Color accent;       // the one accent colour: selection, primary buttons, progress
    Color accent_text;  // text on the accent
    Color danger, success;
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
    const bool dark = winapk::launcher::settings::theme_dark(setting(L"Theme", 0), windows_uses_dark());
    // Windows 11's own neutrals; the accent is the blue of the program's icon,
    // on dark backgrounds the lighter tint Windows uses there, with dark text.
    g_t = dark ? Theme{true,         hex(0x202020), hex(0x272727), hex(0x2D2D2D), hex(0x383838), hex(0x3D3D3D),
                       hex(0x363636), hex(0xFFFFFF), hex(0xC9C9C9), hex(0x8A8A8A), hex(0x4CC2FF), hex(0x0B1A26),
                       hex(0xFF99A4), hex(0x6CCB5F)}
               : Theme{false,        hex(0xF3F3F3), hex(0xFBFBFB), hex(0xFFFFFF), hex(0xEDEDED), hex(0xE1E1E1),
                       hex(0xECECEC), hex(0x1B1B1B), hex(0x5C5C5C), hex(0x9A9A9A), hex(0x1569C7), hex(0xFFFFFF),
                       hex(0xC42B1C), hex(0x0F7B0F)};
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

// The type scale: title (page and sheet headings), body, caption, and the
// tiles' labels; glyphs come in the sizes the controls use.
struct Fonts {
    std::unique_ptr<Gdiplus::Font> title, body, body_semi, caption, caption_semi, tile, monogram, monogram_small,
        glyph12, glyph16, glyph24, glyph48, mono;
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
                  kGlyphChevron[] = L"\uE70D", kGlyphCheck[] = L"\uE73E", kGlyphError[] = L"\uE783",
                  kGlyphClock[] = L"\uE823", kGlyphStop[] = L"\uE71A", kGlyphLibrary[] = L"\uE8F1",
                  kGlyphStore[] = L"\uE719", kGlyphAdd[] = L"\uE710",
                  kGlyphClear[] = L"\uE894", kGlyphLog[] = L"\uE8A5", kGlyphPackage[] = L"\uE7B8",
                  kGlyphChevronUp[] = L"\uE70E", kGlyphBack[] = L"\uE72B", kGlyphForward[] = L"\uE72A",
                  kGlyphContact[] = L"\uE77B", kGlyphPlay[] = L"\uE768", kGlyphInfo[] = L"\uE946",
                  kGlyphDelete[] = L"\uE74D";

void make_fonts() {
    g_f.title = font(20, true);
    g_f.body = font(14);
    g_f.body_semi = font(14, true);
    g_f.caption = font(12);
    g_f.caption_semi = font(12, true);
    g_f.tile = font(13);
    g_f.monogram = font(30, true);
    g_f.monogram_small = font(26, true);
    g_f.glyph12 = glyph_font(12);
    g_f.glyph16 = glyph_font(16);
    g_f.glyph24 = glyph_font(24);
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
void draw_text(Graphics& g, const std::wstring& s, const Gdiplus::Font* f, const Color& c, const RectF& box,
               Align align = kLeft, bool wrap = false, bool vcenter = true, bool ltr = false) {
    Gdiplus::StringFormat sf;
    sf.SetAlignment(align == kLeft     ? Gdiplus::StringAlignmentNear
                    : align == kCenter ? Gdiplus::StringAlignmentCenter
                                       : Gdiplus::StringAlignmentFar);
    sf.SetLineAlignment(vcenter ? Gdiplus::StringAlignmentCenter : Gdiplus::StringAlignmentNear);
    // One character (an icon glyph, a monogram, "!") is never trimmed: in a box as wide as its font is
    // high, GDI+'s padding either side would turn it into an ellipsis the icon font has no glyph for (or,
    // in some GDI+, into nothing). Centred, it gets the room either side.
    const bool one = s.size() == 1;
    const RectF r = one && align == kCenter ? inset(box, -f->GetSize(), 0) : box;
    sf.SetTrimming(one    ? Gdiplus::StringTrimmingNone
                   : wrap ? Gdiplus::StringTrimmingEllipsisWord
                          : Gdiplus::StringTrimmingEllipsisCharacter);
    sf.SetFormatFlags(wrap ? Gdiplus::StringFormatFlagsLineLimit
                           : Gdiplus::StringFormatFlagsNoWrap | (one ? Gdiplus::StringFormatFlagsNoClip : 0));
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

// A standard, accent (primary) or destructive button; `glyph` goes before the text.
void draw_button(Graphics& g, const RectF& r, const std::wstring& text, bool hot, bool primary, bool destructive,
                 const wchar_t* glyph = nullptr) {
    const Color fill = primary ? (hot ? mix(g_t.accent, g_t.dark ? kWhite : kBlack, 0.1f) : g_t.accent)
                               : (hot ? g_t.hover : g_t.card);
    fill_round(g, r, px(kRadiusControl), fill);
    if (!primary) stroke_round(g, r, px(kRadiusControl), g_t.border);
    const Color ink = primary ? g_t.accent_text : destructive ? g_t.danger : g_t.text;
    const Gdiplus::Font* f = primary ? g_f.body_semi.get() : g_f.body.get();
    if (!glyph) {
        draw_text(g, text, f, ink, r, kCenter);
        return;
    }
    const float tw = text_width(g, text, f), x = r.X + (r.Width - tw - px(22)) / 2;
    draw_text(g, glyph, g_f.glyph12.get(), ink, RectF(x, r.Y, px(16), r.Height), kCenter);
    draw_text(g, text, f, ink, RectF(x + px(22), r.Y, tw + px(4), r.Height));
}

// A square button that is only a glyph (it gets a tooltip).
void draw_icon_button(Graphics& g, const RectF& r, const wchar_t* glyph, bool hot, const Color& ink) {
    if (hot) fill_round(g, r, px(kRadiusControl), g_t.hover);
    draw_text(g, glyph, g_f.glyph16.get(), ink, r, kCenter);
}

// A button's width: its text with room either side, never narrower than Windows' 96.
float button_width(Graphics& g, const std::wstring& text, bool primary) {
    return (std::max)(px(96), text_width(g, text, primary ? g_f.body_semi.get() : g_f.body.get()) + px(32));
}

// Windows' toggle switch.
void draw_switch(Graphics& g, bool on, const RectF& r, bool hot) {
    const Color fill = hot ? mix(g_t.accent, g_t.dark ? kWhite : kBlack, 0.1f) : g_t.accent;
    fill_round(g, r, r.Height / 2, on ? fill : hot ? g_t.hover : g_t.card);
    if (!on) stroke_round(g, r, r.Height / 2, g_t.text2);
    const float k = on ? r.Height - px(8) : r.Height - px(10);
    const float kx = on ? r.GetRight() - k - px(4) : r.X + px(5);
    Gdiplus::SolidBrush knob(on ? g_t.accent_text : g_t.text2);
    g.FillEllipse(&knob, kx, r.Y + (r.Height - k) / 2, k, k);
}

// Windows' check box.
void draw_check_box(Graphics& g, bool on, const RectF& r, bool hot) {
    if (on) {
        fill_round(g, r, px(4), hot ? mix(g_t.accent, g_t.dark ? kWhite : kBlack, 0.1f) : g_t.accent);
        draw_text(g, kGlyphCheck, g_f.glyph12.get(), g_t.accent_text, r, kCenter);
    } else {
        fill_round(g, r, px(4), hot ? g_t.hover : g_t.card);
        stroke_round(g, r, px(4), g_t.text2);
    }
}

using Targets = std::vector<std::pair<RectF, int>>;

// A screen point in an lParam (WM_NCHITTEST's, WM_MOUSEWHEEL's) in the coordinates the window's
// targets and mouse messages use. In a mirrored (Hebrew) window ScreenToClient already mirrors x, as
// the mouse messages' lParam is: mirroring it again would put every target on the other side.
// (No window hit-tests its own targets at the moment; the next one that does goes through this.)
[[maybe_unused]] POINT client_point(HWND hwnd, LPARAM screen) {
    POINT pt{GET_X_LPARAM(screen), GET_Y_LPARAM(screen)};
    ScreenToClient(hwnd, &pt);
    return pt;
}

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

// Windows' keyboard focus ring: two strokes, the outer one the text colour.
void draw_focus_ring(Graphics& g, const RectF& r, float rad) {
    stroke_round(g, inset(r, -px(3), -px(3)), rad + px(3), g_t.text, px(2));
    stroke_round(g, inset(r, -px(1), -px(1)), rad + px(1), g_t.layer, px(2));
}

// An indeterminate progress line, as Windows' ProgressBar draws it.
void draw_progress_line(Graphics& g, const RectF& track) {
    fill_round(g, track, track.Height / 2, with_alpha(g_t.accent, 50));
    const float t = static_cast<float>(GetTickCount64() % 1600) / 1600.0f;
    const float seg = track.Width * 0.3f;
    float x0 = track.X + (track.Width + seg) * t - seg, x1 = x0 + seg;
    x0 = (std::max)(x0, track.X);
    x1 = (std::min)(x1, track.GetRight());
    if (x1 > x0) fill_round(g, RectF(x0, track.Y, x1 - x0, track.Height), track.Height / 2, g_t.accent);
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

struct Tile {
    int app;           // index into g_apps, -1 for an app still being installed
    std::wstring job;  // that app's name
};

std::vector<App> g_apps;
std::set<std::wstring> g_running;  // the library's apps that ran at the last look (refresh_running): what is drawn
void refresh_running();
std::vector<Tile> g_tiles;                 // what the grid shows: new installs first, then the (filtered) apps
struct Job {
    std::wstring name;
    enum State { kWaiting, kInstalling, kDone, kFailed, kCancelled } state = kWaiting;
    ULONGLONG start = 0, end = 0;
    std::wstring file;    // the file it came from, its name until the app's own is known
    bool update = false;  // the library had the app already
    int phase = 0;        // how far the install log says it got (install_phase)
    std::wstring reason;  // why it failed, in a sentence
    std::wstring label;   // the app's own name once the install has written it (a new app's)
    std::shared_ptr<Gdiplus::Bitmap> icon;  // the app's icon once the install has written it
};
std::vector<Job> g_jobs;                   // this batch of installs, for the install window
std::map<std::wstring, std::wstring> g_job_labels;  // installs that are not in the library yet: their names to show
HWND g_install_wnd, g_log_wnd, g_log_edit, g_tip;
HWND g_debug_wnd;  // the Threads window (debug mode)
std::map<std::wstring, std::unique_ptr<Gdiplus::Bitmap>> g_icons;  // rounded, at the grid's size; null: no icon
std::unique_ptr<Gdiplus::Bitmap> g_brand;  // the program's icon, for the navigation pane
std::wstring g_selected;
std::wstring g_status;
StatusKind g_status_kind = kHint;
std::wstring g_status_link;          // the InfoBar's action, a link before its close button
std::function<void()> g_status_act;
float g_scroll = 0;
bool g_drag_over = false, g_tracking = false;
bool g_focus_visible = false;  // the selection was reached with the keyboard: it shows a focus ring

// The library's grid, a page (Settings, an app's details, the Google services setup), or an app shown in the window
// (the Store, Google Play).
enum View { kLibraryView, kStoreView, kSettingsView, kAppView, kGmsSetupView, kPlayView } g_view = kLibraryView;
void set_view(View view);
bool g_setup_from_settings = false;      // the Google services setup was opened from Settings (it goes back there)
std::set<std::wstring> g_setup_names;    // installs the setup page started: system components, never a tile
bool google_services_on();
void open_google_play();

// An app shown in the window (see "Apps in the window" below): the Store and Google Play.
struct Embedded {
    View view;              // the view it fills
    const wchar_t* title;   // that view's title
    const wchar_t* starting;  // under the spinner while it starts
    HWND panel = nullptr;   // the host panel over the content area; the app's window is its child
    std::wstring name;      // the app's library name, while it runs (or last ran) in the panel
    enum State { kIdle, kStarting, kShown, kClosed, kFailed } state = kIdle;
    bool up() const { return state == kStarting || state == kShown; }
};
Embedded g_store_app{kStoreView, L"Store", L"Starting the Store…"};
Embedded g_play{kPlayView, L"Google Play", L"Starting Google Play…"};

// Where getting and updating the Store's app stands (store_app.h). The library's thread only.
winapk::launcher::StoreSetup g_store_setup;

// What the mouse is over. kHitNav's `tile` is the pane's item: 0 Library, 1 Store, 2 Settings, 3 Google Play.
// kHitRetry: the Store's Try again; kHitPanelOpen and kHitPanelLog: an app shown in the window that ended, opened again
// or its log.
enum HitKind { kHitNone, kHitNav, kHitAdd, kHitTile, kHitThumb, kHitTrack, kHitRetry, kHitInfoClose, kHitInfoLink,
               kHitBack, kHitPage, kHitAccount, kHitPanelOpen, kHitPanelLog };
enum NavItem { kNavLibrary, kNavStore, kNavSettings, kNavPlay, kNavCount };
struct Hit {
    HitKind kind = kHitNone;
    int tile = -1;
    bool operator==(const Hit& o) const { return kind == o.kind && tile == o.tile; }
};
Hit g_hot;
bool g_thumb_drag = false;
float g_thumb_y0 = 0, g_thumb_scroll0 = 0;

// The window: a navigation pane (Library, Store, Settings at the bottom) and
// the content layer beside it, with the page's title and toolbar above the
// grid, or a page (Settings, an app's details) in its place.
struct Layout {
    RectF rail, nav[kNavCount], content, title, account, add, grid, drop, retry, infobar,
        info_close, info_link,  // info_link: measured when the InfoBar is painted
        panel, panel_open, panel_log;  // an app shown in the window, and its Open and Show log once it closed
    bool compact = false;  // a narrow window: the pane shows icons only
    int cols = 1;
    float tile_w = 0, tile_h = 0, gap = 0, content_h = 0, max_scroll = 0;  // the library's tiles
} g_l;

// The signed-in Google account the toolbar's account button shows; empty when there is none
// (refresh_account asks the runtime in the background).
std::wstring g_account;
void refresh_account();
// The account's picture, round, at the button's size: what the runtime saved at sign-in
// (com.macapk.gms.AccountPhoto), loaded once per account and size. Null: none, the initial shows.
std::unique_ptr<Gdiplus::Bitmap> g_account_photo;
std::wstring g_account_photo_key;  // account + size it was loaded for; cleared to load again

HWND owner() { return g_main; }
void show_logs(const fs::path& log);
bool debug_mode();
void show_debugger(const std::wstring& app);
void show_install_window();
void fit_install_window();
std::wstring install_failure_reason(const fs::path& log);
void show_library(int show);
void maybe_quit();
struct Splash;

const App* find_app(const std::wstring& name) {
    for (const App& a : g_apps) {
        if (a.name == name) return &a;
    }
    return nullptr;
}

// The picture in `png` at s x s, its corners rounded by `radius` (s / 2: a circle); null if there is none.
std::unique_ptr<Gdiplus::Bitmap> rounded_picture(const fs::path& png, int s, float radius) {
    std::unique_ptr<Gdiplus::Bitmap> out;
    std::error_code ec;
    if (fs::exists(png, ec)) {
        Gdiplus::Bitmap src(png.c_str());
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
            rounded(p, RectF(0, 0, static_cast<float>(s), static_cast<float>(s)), radius);
            g.FillPath(&brush, &p);
        }
    }
    return out;
}

// The picture in `png` at s x s, with the icons' rounded corners; null if there is none.
std::unique_ptr<Gdiplus::Bitmap> rounded_icon(const fs::path& png, int s) { return rounded_picture(png, s, s * 0.22f); }

// The program's icon (launcher.rc's PNG) for the pane, at the pane's size.
Gdiplus::Bitmap* brand_icon() {
    if (g_brand) return g_brand.get();
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(2), RT_RCDATA);
    HGLOBAL mem = res ? LoadResource(nullptr, res) : nullptr;
    const void* bytes = mem ? LockResource(mem) : nullptr;
    const DWORD size = res ? SizeofResource(nullptr, res) : 0;
    if (!bytes || !size) return nullptr;
    HGLOBAL copy = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!copy) return nullptr;
    memcpy(GlobalLock(copy), bytes, size);
    GlobalUnlock(copy);
    IStream* stream = nullptr;
    if (SUCCEEDED(CreateStreamOnHGlobal(copy, TRUE, &stream))) {  // the stream frees `copy`
        Gdiplus::Bitmap src(stream);
        if (src.GetLastStatus() == Gdiplus::Ok) {
            const int s = ipx(24);
            g_brand = std::make_unique<Gdiplus::Bitmap>(s, s, PixelFormat32bppPARGB);
            Graphics g(g_brand.get());
            g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
            g.DrawImage(&src, 0, 0, s, s);
        }
        stream->Release();
    } else {
        GlobalFree(copy);
    }
    return g_brand.get();
}

Gdiplus::Bitmap* icon_for(const App& app) {
    auto it = g_icons.find(app.name);
    if (it != g_icons.end()) return it->second.get();
    std::unique_ptr<Gdiplus::Bitmap> out = rounded_icon(app_dir(app.name) / L"icon.png", ipx(72));
    Gdiplus::Bitmap* raw = out.get();
    g_icons[app.name] = std::move(out);
    return raw;
}

// An icon with its shadow; the monogram stands in when there is no picture.
void draw_icon(Graphics& g, Gdiplus::Bitmap* bmp, const std::wstring& label, const RectF& r) {
    fill_round(g, RectF(r.X, r.Y + px(2), r.Width, r.Height), r.Width * 0.22f, Color(g_t.dark ? 90 : 30, 0, 0, 0));
    if (bmp) {
        draw_image(g, bmp, r);
    } else {
        draw_monogram(g, r, label,
                      r.Width >= px(72)   ? g_f.monogram.get()
                      : r.Width >= px(56) ? g_f.monogram_small.get()
                                          : g_f.body_semi.get());
    }
}

void draw_app_icon(Graphics& g, const App* app, const std::wstring& label, const RectF& r) {
    draw_icon(g, app ? icon_for(*app) : nullptr, label, r);
}

// What the install of a new app has written of it so far: its icon and name (apps\<name>\icon.png and
// label.txt, which the runtime writes before it converts the code), so that the install shows the real
// ones while it runs, as Android's does. Looked for twice a second, and forgotten with the batch.
struct PendingLook {
    ULONGLONG looked = 0;
    uintmax_t icon_size = 0;  // the file's size at the last look: read once it has stopped growing
    std::shared_ptr<Gdiplus::Bitmap> icon;
    std::wstring label;
};
std::map<std::wstring, PendingLook> g_pending_looks;

const PendingLook& pending_look(const std::wstring& name) {
    PendingLook& look = g_pending_looks[name];
    const ULONGLONG now = GetTickCount64();
    if ((look.icon && !look.label.empty()) || now - look.looked < 500) return look;
    look.looked = now;
    std::error_code ec;
    if (!look.icon) {
        const fs::path png = app_dir(name) / L"icon.png";
        const uintmax_t size = fs::file_size(png, ec);
        if (!ec && size > 0 && size == look.icon_size) look.icon = rounded_icon(png, ipx(72));
        look.icon_size = ec ? 0 : size;
    }
    if (look.label.empty()) {
        std::ifstream f(app_dir(name) / L"label.txt", std::ios::binary);
        std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        look.label = utf8_to_wide(s);
        look.label.erase(0, look.label.find_first_not_of(L" \t\r\n"));
        if (const size_t end = look.label.find_first_of(L"\r\n"); end != std::wstring::npos) look.label.resize(end);
        // The library's and the tiles' name for it too, unless the store gave it one.
        if (!look.label.empty()) g_job_labels.emplace(name, look.label);
    }
    return look;
}

// The name an install shows before the app is in the library: the store's
// name for it, else the app's own once its install has written it, else the library name.
std::wstring job_label(const std::wstring& name) {
    if (const App* a = find_app(name)) return a->label;
    const auto it = g_job_labels.find(name);
    return it == g_job_labels.end() ? name : it->second;
}

// Shows a message in the InfoBar; a success or a note closes itself after six seconds. A `link` is an action
// at its end (`act`), and keeps it open until it is used or closed.
void info(const std::wstring& text, StatusKind kind, const std::wstring& link = L"", std::function<void()> act = {}) {
    g_status = text;
    g_status_kind = kind;
    g_status_link = act ? link : L"";
    g_status_act = std::move(act);
    if (!g_main) return;
    if (kind == kError || text.empty() || g_status_act) KillTimer(g_main, kInfoBarTimer);
    else SetTimer(g_main, kInfoBarTimer, 6000, nullptr);
    InvalidateRect(g_main, nullptr, FALSE);
}

// Which library app has each package: an install of a package the library has updates that app, and Google's
// apps and the Store are found by package.
std::map<std::string, std::wstring> g_packages;  // package: library name

void scan_packages() {
    g_packages.clear();
    for (const App& app : g_apps) {
        const std::vector<std::string> p = package_of(app.name);
        if (!p.empty() && !p[0].empty()) g_packages[p[0]] = app.name;
    }
}

// ---------------------------------------------------------------------------
// Layout

RectF tile_rect(int i) {
    const int col = i % g_l.cols, row = i / g_l.cols;
    return RectF(g_l.grid.X + px(8) + col * (g_l.tile_w + g_l.gap),
                 g_l.grid.Y + px(4) + row * (g_l.tile_h + g_l.gap) - g_scroll, g_l.tile_w, g_l.tile_h);
}

RectF thumb_rect() {
    const RectF& gr = g_l.grid;
    const float h = (std::max)(px(36), gr.Height * gr.Height / g_l.content_h);
    const float y = gr.Y + (gr.Height - h) * (g_l.max_scroll > 0 ? g_scroll / g_l.max_scroll : 0);
    return RectF(gr.GetRight() - px(8), y, px(6), h);
}

void clamp_scroll() { g_scroll = (std::max)(0.0f, (std::min)(g_scroll, g_l.max_scroll)); }

// A text's width outside a paint, for the layout, measured on the window's own DC.
float measure(const std::wstring& s, const Gdiplus::Font* f) {
    if (!f) return 0;
    HDC dc = GetDC(g_main);
    float w = 0;
    {
        Graphics g(dc);
        w = text_width(g, s, f);
    }
    ReleaseDC(g_main, dc);
    return w;
}

void place_panels();
bool panel_shows_log();

void compute_layout() {
    RECT rc;
    GetClientRect(g_main, &rc);
    const float W = static_cast<float>(rc.right), H = static_cast<float>(rc.bottom);

    // The pane: the brand row, Library and Store (and Google Play under it while Google services are on),
    // Settings at the bottom.
    g_l.compact = W < px(900);
    const float rail_w = g_l.compact ? px(56) : px(200);
    g_l.rail = RectF(0, 0, rail_w, H);
    const float item_h = px(36), item_w = rail_w - px(16);
    for (int i = 0; i < 2; ++i) g_l.nav[i] = RectF(px(8), px(60) + i * (item_h + px(4)), item_w, item_h);
    g_l.nav[kNavPlay] = google_services_on() ? RectF(px(8), px(60) + 2 * (item_h + px(4)), item_w, item_h) : RectF();
    g_l.nav[kNavSettings] = RectF(px(8), g_l.rail.GetBottom() - px(12) - item_h, item_w, item_h);
    // The Google account above Settings, at the pane's foot, where the Microsoft Store keeps the profile: only
    // its picture, a square the items' height (the address is its tooltip), where the items' glyphs are.
    g_l.account = RectF(g_l.compact ? px(8) + (item_w - item_h) / 2 : px(8), g_l.nav[kNavSettings].Y - px(4) - item_h,
                        item_h, item_h);

    // The content layer: the title block with the toolbar's controls at its end, the grid below.
    g_l.content = RectF(rail_w, 0, W - rail_w, H);
    const float m = px(24), cx = g_l.content.X + m, cw = g_l.content.Width - 2 * m;
    g_l.title = RectF(cx, px(20), cw, px(44));
    const float ch = px(32), cy = g_l.title.Y + (g_l.title.Height - ch) / 2;
    const float add_w = measure(tr(L"Install app"), g_f.body_semi.get()) + px(50);
    g_l.add = RectF(cx + cw - add_w, cy, add_w, ch);
    const float top = g_l.title.GetBottom() + px(16);
    g_l.grid = RectF(cx - px(8), top, cw + px(16), (std::max)(0.0f, g_l.content.GetBottom() - top));

    // The empty library's drop card, and the Store's Try again.
    const float dw = (std::min)(px(520), g_l.grid.Width - px(32)), dh = px(200);
    const float dy = g_l.grid.Y + (std::max)(px(8), (g_l.grid.Height - dh) / 2 - px(40));
    g_l.drop = RectF(g_l.grid.X + (g_l.grid.Width - dw) / 2, dy, dw, dh);
    // The InfoBar: a message at the bottom of the content, over what is there.
    const float iw = (std::min)(px(560), cw);
    g_l.infobar = RectF(cx + (cw - iw) / 2, H - px(16) - px(48), iw, px(48));
    g_l.info_close = RectF(g_l.infobar.GetRight() - px(40), g_l.infobar.Y + px(8), px(32), px(32));
    g_l.retry = RectF(g_l.grid.X + g_l.grid.Width / 2 - px(60),
                      g_l.grid.Y + (std::min)(g_l.grid.Height / 2, px(110)) + px(66), px(120), ch);
    // The Store and Google Play: the app's own window, a child of the panel filling the whole content layer, as
    // an app fills a phone's screen: no title over it (the app has its own). The title shows only while the app
    // is not up (getting the Store, starting, ended). Once it ended (or the Store could not be set up), Show log
    // beside Open; else Open alone, centred.
    g_l.panel = RectF(g_l.content.X + 1, g_l.content.Y, g_l.content.Width - 1, (std::max)(1.0f, g_l.content.Height));
    const bool panel_log = panel_shows_log();
    g_l.panel_open = RectF(g_l.grid.X + g_l.grid.Width / 2 - (panel_log ? px(140) : px(80)), g_l.retry.Y, px(160), ch);
    g_l.panel_log = RectF(g_l.panel_open.GetRight() + px(8), g_l.retry.Y, px(112), ch);
    place_panels();

    const size_t items = g_tiles.size();
    g_l.tile_w = px(112);
    g_l.tile_h = px(132);
    g_l.gap = px(8);
    g_l.cols = (std::max)(1, static_cast<int>((g_l.grid.Width - px(26) + g_l.gap) / (g_l.tile_w + g_l.gap)));
    const int rows = (static_cast<int>(items) + g_l.cols - 1) / g_l.cols;
    g_l.content_h = rows ? rows * (g_l.tile_h + g_l.gap) - g_l.gap + px(12) : 0;
    g_l.max_scroll = (std::max)(0.0f, g_l.content_h - g_l.grid.Height);
    clamp_scroll();
}

void rebuild_tiles() {
    g_tiles.clear();
    std::set<std::wstring> shown;
    for (const std::wstring& name : pending_names()) {
        if (!find_app(name) && !g_setup_names.count(name) && shown.insert(name).second) g_tiles.push_back({-1, name});
    }
    for (size_t i = 0; i < g_apps.size(); ++i) {
        if (!system_app(g_apps[i].name)) g_tiles.push_back({static_cast<int>(i), L""});
    }
    compute_layout();
    InvalidateRect(g_main, nullptr, FALSE);
}

// What the library folder holds, in one number that changes when an app is added, removed or renamed.
uint64_t library_stamp() {
    uint64_t h = 1469598103934665603ull;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(library_dir(), ec)) {
        const uint64_t t = static_cast<uint64_t>(e.last_write_time(ec).time_since_epoch().count());
        for (const wchar_t c : e.path().filename().wstring()) h = (h ^ static_cast<uint16_t>(c)) * 1099511628211ull;
        h = (h ^ t) * 1099511628211ull;
    }
    return h;
}
uint64_t g_library_stamp = 0;

void refresh() {
    g_library_stamp = library_stamp();
    g_apps = library();
    scan_packages();
    g_icons.clear();
    if (!find_app(g_selected)) g_selected.clear();
    rebuild_tiles();
    refresh_running();
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
    if (g_view == kLibraryView) ensure_visible(tile_of(name));
    InvalidateRect(g_main, nullptr, FALSE);
}

const App* selected() { return g_selected.empty() ? nullptr : find_app(g_selected); }

// The empty library's drop card is shown when nothing is installed or queued.
bool empty_library() { return g_view == kLibraryView && g_tiles.empty(); }

bool page_view() {
    return g_view == kSettingsView || g_view == kAppView || g_view == kGmsSetupView;
}

Hit panel_hit(float x, float y);

Hit hit_test(float x, float y) {
    for (int i = 0; i < kNavCount; ++i) {
        if (g_l.nav[i].Width > 0 && contains(g_l.nav[i], x, y)) return {kHitNav, i};
    }
    if (contains(g_l.account, x, y)) return {kHitAccount, -1};
    if (!g_status.empty() && contains(g_l.infobar, x, y)) {
        if (!g_status_link.empty() && contains(g_l.info_link, x, y)) return {kHitInfoLink, -1};
        return {contains(g_l.info_close, x, y) ? kHitInfoClose : kHitNone, -1};
    }
    if (page_view()) {
        if (contains(g_l.content, x, y)) return {kHitPage, -1};
        return {};
    }
    if (g_view == kPlayView || g_view == kStoreView) return panel_hit(x, y);
    if (contains(g_l.add, x, y)) return {kHitAdd, -1};
    if (!contains(g_l.grid, x, y)) return {};
    if (empty_library()) return {};
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

void paint_account_button(Graphics& g);

// The navigation pane: the brand, Library and Store, and the Google account and Settings at the bottom.
void paint_rail(Graphics& g) {
    const RectF& r = g_l.rail;
    if (Gdiplus::Bitmap* brand = brand_icon()) {
        const float s = px(24);
        draw_image(g, brand, RectF(g_l.compact ? (r.Width - s) / 2 : px(16), px(16), s, s));
    }
    if (!g_l.compact) {
        draw_text(g, L"Win.apk", g_f.body_semi.get(), g_t.text, RectF(px(48), px(14), r.Width - px(56), px(28)));
    }
    struct Item {
        const wchar_t* glyph;
        const wchar_t* label;
    };
    const Item items[] = {{kGlyphLibrary, L"Library"}, {kGlyphStore, L"Store"}, {kGlyphSettings, L"Settings"},
                          {kGlyphPlay, L"Google Play"}};
    for (int i = 0; i < kNavCount; ++i) {
        const RectF& it = g_l.nav[i];
        if (it.Width <= 0) continue;  // Google Play while Google services are off
        // The setup page belongs to Google Play when that opened it, and to Settings when its switch did.
        const bool setup = g_view == kGmsSetupView;
        const bool on = (i == kNavLibrary && (g_view == kLibraryView || g_view == kAppView)) ||
                        (i == kNavStore && g_view == kStoreView) ||
                        (i == kNavSettings && (g_view == kSettingsView || (setup && g_setup_from_settings))) ||
                        (i == kNavPlay && ((setup && !g_setup_from_settings) || g_view == kPlayView));
        const bool hot = g_hot.kind == kHitNav && g_hot.tile == i;
        if (on || hot) fill_round(g, it, px(kRadiusControl), mix(g_t.bg, g_t.text, on ? 0.07f : 0.04f));
        if (on) fill_round(g, RectF(it.X, it.Y + (it.Height - px(16)) / 2, px(3), px(16)), px(1.5f), g_t.accent);
        const Color ink = on || hot ? g_t.text : g_t.text2;
        const float gx = g_l.compact ? it.X + (it.Width - px(16)) / 2 : it.X + px(12);
        draw_text(g, items[i].glyph, g_f.glyph16.get(), ink, RectF(gx, it.Y, px(16), it.Height), kCenter);
        if (!g_l.compact) {
            draw_text(g, tr(items[i].label), on ? g_f.body_semi.get() : g_f.body.get(), ink,
                      RectF(it.X + px(40), it.Y, it.Width - px(48), it.Height));
        }
    }
    paint_account_button(g);
}

// The content layer: a lighter surface with a rounded corner towards the pane, as Windows 11 lays out.
void paint_layer(Graphics& g) {
    const RectF& c = g_l.content;
    const RectF card(c.X, c.Y + px(8), c.Width + px(16), c.Height + px(16));  // its other corners are off-screen
    g.SetClip(c);
    fill_round(g, card, px(kRadiusCard), g_t.layer);
    stroke_round(g, card, px(kRadiusCard), g_t.border);
    g.ResetClip();
}

fs::path gms_dir();

// Where the runtime keeps the account's picture: com.macapk.gms.AccountPhoto.file, the same name
// (lower case; anything but a-z 0-9 . _ @ - as '_').
fs::path account_photo_path(const std::wstring& account) {
    std::wstring name = L"photo-";
    for (wchar_t ch : account) {
        if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch - L'A' + L'a');
        const bool ok = (ch >= L'a' && ch <= L'z') || (ch >= L'0' && ch <= L'9') || ch == L'.' || ch == L'_' ||
                        ch == L'@' || ch == L'-';
        name += ok ? ch : L'_';
    }
    return gms_dir() / (name + L".img");
}

Gdiplus::Bitmap* account_photo(int s) {
    const std::wstring key = g_account + L"|" + std::to_wstring(s);
    if (key != g_account_photo_key) {
        g_account_photo_key = key;
        g_account_photo = rounded_picture(account_photo_path(g_account), s, s / 2.0f);
    }
    return g_account_photo.get();
}

// The Google account's item in the pane, above Settings: the account's picture when Google gave one at
// sign-in, round with a thin ring; else its initial in an accent circle; signed out, the person glyph. Only the
// picture: the address (or "not signed in") is its tooltip. It opens Settings, where the account is.
void paint_account_button(Graphics& g) {
    const RectF& it = g_l.account;
    if (it.Width <= 0) return;
    const bool hot = g_hot.kind == kHitAccount;
    if (hot) fill_round(g, it, px(kRadiusControl), mix(g_t.bg, g_t.text, 0.04f));
    const Color ink = hot ? g_t.text : g_t.text2;
    const float s = px(26);
    const RectF c(it.X + (it.Width - s) / 2, it.Y + (it.Height - s) / 2, s, s);
    const int side = static_cast<int>(std::lround(s));
    Gdiplus::Bitmap* photo = g_account.empty() ? nullptr : account_photo(side);
    if (photo) {
        draw_image(g, photo, RectF(c.X, c.Y, static_cast<float>(side), static_cast<float>(side)));
        Gdiplus::Pen ring(hot ? g_t.accent : with_alpha(g_t.text, g_t.dark ? 70 : 45), px(1));
        g.DrawEllipse(&ring, inset(c, px(0.5f), px(0.5f)));
    } else if (!g_account.empty()) {
        Gdiplus::SolidBrush disc(g_t.accent);
        g.FillEllipse(&disc, c);
        const std::wstring initial(1, static_cast<wchar_t>(towupper(g_account[0])));
        draw_text(g, initial, g_f.body_semi.get(), Color(255, 255, 255, 255), c, kCenter);
    } else {
        draw_text(g, kGlyphContact, g_f.glyph16.get(), ink, it, kCenter);
    }
}

// The page's title and what it holds, with the toolbar's controls at the end of the row.
void paint_toolbar(Graphics& g) {
    const RectF& t = g_l.title;
    size_t shown = 0, running = 0;  // the library's own apps: not the system components
    for (const Tile& tile : g_tiles) shown += tile.app >= 0;
    for (const std::wstring& r : g_running) running += !system_app(r);
    std::wstring sub = shown == 0 ? std::wstring(tr(L"No apps installed yet")) : app_count(shown);
    if (running) sub += L"  ·  " + fmt(L"{0} running", {std::to_wstring(running)});
    if (const std::wstring now = current_install(); !now.empty() && !system_app(now) && !g_setup_names.count(now)) {
        sub += L"  ·  " + fmt(L"Installing {0}…", {job_label(now)});
    }
    const float tw = (std::max)(px(60), g_l.add.X - t.X - px(16));
    draw_text(g, tr(L"Library"), g_f.title.get(), g_t.text, RectF(t.X, t.Y, tw, px(26)));
    draw_text(g, sub, g_f.caption.get(), g_t.text2, RectF(t.X, t.Y + px(26), tw, px(18)));
    draw_button(g, g_l.add, tr(L"Install app"), g_hot.kind == kHitAdd, true, false, kGlyphAdd);
}

void paint_scrollbar(Graphics& g, bool nearby) {
    if (g_l.max_scroll <= 0) return;
    const bool active = g_thumb_drag || g_hot.kind == kHitThumb || g_hot.kind == kHitTrack;
    fill_round(g, thumb_rect(), px(3), with_alpha(g_t.text2, active ? 200 : nearby ? 120 : 70));
}

void paint_tile(Graphics& g, int i) {
    const RectF r = tile_rect(i);
    if (r.GetBottom() < g_l.grid.Y || r.Y > g_l.grid.GetBottom()) return;
    const Tile& t = g_tiles[static_cast<size_t>(i)];
    const App* app = t.app >= 0 ? &g_apps[static_cast<size_t>(t.app)] : nullptr;
    const bool sel = app && app->name == g_selected;
    const bool hot = g_hot.kind == kHitTile && g_hot.tile == i;
    const bool running = app && g_running.count(app->name);
    if (sel) {
        fill_round(g, r, px(kRadiusTile), with_alpha(g_t.accent, g_t.dark ? 56 : 30));
        stroke_round(g, r, px(kRadiusTile), with_alpha(g_t.accent, 170));
        if (g_focus_visible && GetFocus() == g_main) draw_focus_ring(g, r, px(kRadiusTile));
    } else if (hot) {
        fill_round(g, r, px(kRadiusTile), g_t.card);
        stroke_round(g, r, px(kRadiusTile), g_t.border);
    }
    const std::wstring label = app ? app->label : job_label(t.job);
    const float s = px(72);
    const RectF ir(r.X + (r.Width - s) / 2, r.Y + px(12), s, s);
    // A new app being installed: its own icon as soon as the install has it, under the veil.
    draw_icon(g, app ? icon_for(*app) : pending_look(t.job).icon.get(), label, ir);
    if (!app || is_pending(app->name)) {
        fill_round(g, ir, s * 0.22f, Color(130, 0, 0, 0));
        spinner(g, inset(ir, s * 0.3f, s * 0.3f), kWhite, px(3));
    }
    if (running) {  // a badge on the icon's corner, ringed in the tile's colour so it stands off the picture
        const float d = px(22);
        const RectF badge(ir.GetRight() - d + px(6), ir.GetBottom() - d + px(6), d, d);
        Gdiplus::SolidBrush ring(sel   ? mix(g_t.layer, g_t.accent, g_t.dark ? 0.22f : 0.12f)
                                 : hot ? g_t.card
                                       : g_t.layer);
        g.FillEllipse(&ring, badge);
        Gdiplus::SolidBrush dot(g_t.success);
        g.FillEllipse(&dot, inset(badge, px(3), px(3)));
        Gdiplus::SolidBrush core(kWhite);
        g.FillEllipse(&core, inset(badge, px(8), px(8)));
    }
    draw_text(g, label, g_f.tile.get(), app ? g_t.text : g_t.text2,
              RectF(r.X + px(6), ir.GetBottom() + px(8), r.Width - px(12), px(36)), kCenter, true, false);
}

// The middle of an empty grid: a glyph, a line and a word of advice.
void paint_empty(Graphics& g, const wchar_t* glyph, const std::wstring& title, const std::wstring& sub) {
    const RectF& gr = g_l.grid;
    const float cy = gr.Y + (std::min)(gr.Height / 2, px(110));
    if (glyph) {
        draw_text(g, glyph, g_f.glyph48.get(), with_alpha(g_t.text2, 150), RectF(gr.X, cy - px(70), gr.Width, px(60)),
                  kCenter);
    }
    draw_text(g, title, g_f.body_semi.get(), g_t.text, RectF(gr.X, cy, gr.Width, px(24)), kCenter);
    draw_text(g, sub, g_f.caption.get(), g_t.text2, RectF(gr.X + px(24), cy + px(24), gr.Width - px(48), px(36)),
              kCenter, true, false);
}

// Nothing installed yet: a card that says how to add the first app (Install app is in the toolbar).
void paint_drop_card(Graphics& g) {
    const RectF& r = g_l.drop;
    fill_round(g, r, px(kRadiusTile), g_t.card);
    GraphicsPath p;
    rounded(p, inset(r, px(1), px(1)), px(kRadiusTile) - 1);
    Gdiplus::Pen pen(with_alpha(g_t.text2, 140), px(1.5f));
    const Gdiplus::REAL dashes[] = {4, 3};
    pen.SetDashPattern(dashes, 2);
    g.DrawPath(&pen, &p);
    const float cy = r.Y + r.Height / 2;
    draw_text(g, kGlyphDownload, g_f.glyph48.get(), with_alpha(g_t.text2, 200), RectF(r.X, cy - px(66), r.Width, px(56)),
              kCenter);
    draw_text(g, tr(L"Drop an Android app here"), g_f.body_semi.get(), g_t.text,
              RectF(r.X + px(16), cy + px(2), r.Width - px(32), px(24)), kCenter);
    draw_text(g, tr(L"or choose Install app  ·  .apk .apkm .xapk .apks"), g_f.caption.get(), g_t.text2,
              RectF(r.X + px(16), cy + px(26), r.Width - px(32), px(20)), kCenter);
}

void paint_grid(Graphics& g) {
    const RectF& gr = g_l.grid;
    if (g_tiles.empty()) {
        paint_drop_card(g);
        return;
    }
    g.SetClip(gr);
    for (size_t i = 0; i < g_tiles.size(); ++i) paint_tile(g, static_cast<int>(i));
    g.ResetClip();
    if (g_scroll > 0) {
        Gdiplus::Pen line(g_t.border, 1);
        g.DrawLine(&line, gr.X + px(8), gr.Y, gr.GetRight() - px(8), gr.Y);
    }
    paint_scrollbar(g, g_hot.kind == kHitTile);
}

// While a file is dragged over the window the whole content area is the drop target.
void paint_drop_overlay(Graphics& g) {
    const RectF& c = g_l.content;
    Gdiplus::SolidBrush veil(with_alpha(g_t.layer, 225));
    g.FillRectangle(&veil, c);
    const RectF r = inset(c, px(16), px(16));
    fill_round(g, r, px(kRadiusTile), with_alpha(g_t.accent, g_t.dark ? 40 : 22));
    GraphicsPath p;
    rounded(p, inset(r, px(1), px(1)), px(kRadiusTile) - 1);
    Gdiplus::Pen pen(g_t.accent, px(2));
    const Gdiplus::REAL dashes[] = {4, 3};
    pen.SetDashPattern(dashes, 2);
    g.DrawPath(&pen, &p);
    const float cy = r.Y + r.Height / 2;
    draw_text(g, kGlyphDownload, g_f.glyph48.get(), g_t.accent, RectF(r.X, cy - px(76), r.Width, px(60)), kCenter);
    draw_text(g, tr(L"Drop to install"), g_f.title.get(), g_t.text, RectF(r.X, cy - px(8), r.Width, px(30)), kCenter);
    draw_text(g, tr(g_view == kGmsSetupView ? L"Win.apk checks that Google signed it, then installs it"
                                            : L"Win.apk keeps its own copy, so the downloaded file can be deleted"),
              g_f.body.get(), g_t.text2, RectF(r.X, cy + px(24), r.Width, px(24)), kCenter);
}

// The InfoBar: what just happened, at the bottom of the content, until it closes itself (a success or a
// note, after a few seconds) or is closed (an error).
void paint_infobar(Graphics& g) {
    if (g_status.empty()) return;
    const RectF& r = g_l.infobar;
    fill_round(g, RectF(r.X, r.Y + px(2), r.Width, r.Height), px(kRadiusCard), Color(g_t.dark ? 90 : 26, 0, 0, 0));
    const Color tint = g_status_kind == kError ? g_t.danger : g_status_kind == kSuccess ? g_t.success : g_t.accent;
    fill_round(g, r, px(kRadiusCard), mix(g_t.card, tint, g_t.dark ? 0.16f : 0.08f));
    stroke_round(g, r, px(kRadiusCard), mix(g_t.border, tint, 0.35f));
    const RectF dot(r.X + px(16), r.Y + (r.Height - px(16)) / 2, px(16), px(16));
    Gdiplus::SolidBrush b(tint);
    g.FillEllipse(&b, dot);
    draw_text(g, g_status_kind == kError ? L"!" : g_status_kind == kSuccess ? kGlyphCheck : L"i",
              g_status_kind == kSuccess ? g_f.glyph12.get() : g_f.caption_semi.get(), kWhite, dot, kCenter);
    float text_w = r.Width - px(96);
    g_l.info_link = RectF();
    if (!g_status_link.empty()) {
        const float lw = text_width(g, g_status_link, g_f.body_semi.get()) + px(24);
        g_l.info_link = RectF(g_l.info_close.X - px(4) - lw, r.Y + px(8), lw, px(32));
        if (g_hot.kind == kHitInfoLink) fill_round(g, g_l.info_link, px(kRadiusControl), g_t.hover);
        draw_text(g, g_status_link, g_f.body_semi.get(), g_t.accent, g_l.info_link, kCenter);
        text_w = g_l.info_link.X - px(8) - (dot.GetRight() + px(12));
    }
    draw_text(g, g_status, g_f.body.get(), g_t.text, RectF(dot.GetRight() + px(12), r.Y, text_w, r.Height));
    const bool hot = g_hot.kind == kHitInfoClose;
    if (hot) fill_round(g, g_l.info_close, px(kRadiusControl), g_t.hover);
    draw_text(g, kGlyphClear, g_f.glyph12.get(), g_t.text2, g_l.info_close, kCenter);
}

void paint_page(Graphics& g);

void paint_play(Graphics& g);
void paint_store(Graphics& g);

void paint_main(Graphics& g, float, float) {
    g.Clear(g_t.bg);
    paint_rail(g);
    paint_layer(g);
    if (page_view()) {
        paint_page(g);
    } else if (g_view == kPlayView) {
        paint_play(g);
    } else if (g_view == kStoreView) {
        paint_store(g);
    } else {
        paint_toolbar(g);
        paint_grid(g);
    }
    if (g_drag_over) paint_drop_overlay(g);
    paint_infobar(g);
}

// Only what spins: an install's veil, the Store being set up, the Store or Google Play starting. A running app's
// badge is still, and its starting window keeps its own timer, so a run alone repaints nothing.
void update_timer() {
    if (any_pending() || g_store_setup.busy() || g_store_app.state == Embedded::kStarting ||
        g_play.state == Embedded::kStarting) {
        SetTimer(g_main, kAnimTimer, 16, nullptr);
    }
    else KillTimer(g_main, kAnimTimer);
}

// ---------------------------------------------------------------------------
// Dialogs

// The launcher's one dialog, for its own questions and for the ones apps ask: in its theme, light or
// dark, mirrored in Hebrew (TaskDialog and MessageBox follow neither). An app's icon and name head it
// when it is about one app; then the question, at most a sentence saying what it means, switches or a
// check box when there is something to pick, and the buttons: the accent one first, as Windows orders
// them, stacked full width for an app's question as Android's are, else right-aligned in a footer.
// Esc and the caption's close are no answer (0).

constexpr UINT kAskTimeoutMs = 300 * 1000;  // an app's question: no answer in five minutes changes nothing

constexpr int kPromptLink = 0x7000;  // the link's target id: past any switch's, and no button uses it
constexpr int kPromptLink2 = 0x7001;  // the second link's
// A switch's target id: kPromptSwitch + its index, past every button's. Buttons use IDOK (1), IDCANCEL (2) and
// the like, so switches numbered from 1 took a click on Remove for a click on the first switch.
constexpr int kPromptSwitch = 0x7100;

struct Prompt {
    std::wstring app, question, note;
    std::wstring caption;  // the window's title; the app's name when empty
    std::unique_ptr<Gdiplus::Bitmap> icon;
    struct Switch {
        std::wstring title;
        bool on;
        bool check = false;  // a check box under the text, else a switch in a card
    };
    std::vector<Switch> switches;
    std::vector<std::pair<int, std::wstring>> buttons;  // the first is the accent one
    std::wstring link;              // a link under the note, which does `on_link` and leaves the dialog open
    std::function<void()> on_link;
    std::wstring link2;             // a second link beside it
    std::function<void()> on_link2;
    bool plain = false;    // no accent button: Android's list of a system dialog's actions (the crash and
                           // not-responding dialogs), none of which is the one to take
    bool footer = false;   // the buttons in a footer at the end, else stacked full width
    bool timeout = false;  // closes itself after kAskTimeoutMs
    int default_id = 0;    // Enter
    int pressed = 0;       // the answer; 0: closed or timed out
    HWND window = nullptr, near_window = nullptr, owner = nullptr;
    Targets targets;       // ids: a button's own, or 1 + index for a switch
    int hot = 0, focus = 0;
    bool tracking = false;
};

// Lays the prompt out, and draws it when `draw`; returns the height it needs.
float layout_prompt(Graphics& g, Prompt& p, float W, bool draw) {
    if (draw) g.Clear(g_t.layer);
    p.targets.clear();
    const float m = px(24), cw = W - 2 * m;
    float y = px(24);
    if (!p.app.empty()) {
        const RectF ir(m, px(20), px(40), px(40));
        if (draw) {
            draw_icon(g, p.icon.get(), p.app, ir);
            draw_text(g, p.app, g_f.body.get(), g_t.text2, RectF(ir.GetRight() + px(12), ir.Y, cw - px(52), ir.Height));
        }
        y = ir.GetBottom() + px(14);
    }
    const float qh = text_height(g, p.question, g_f.title.get(), cw) + px(2);
    if (draw) draw_text(g, p.question, g_f.title.get(), g_t.text, RectF(m, y, cw, qh), kLeft, true, false);
    y += qh + px(6);
    if (!p.note.empty()) {
        const float nh = text_height(g, p.note, g_f.body.get(), cw) + px(2);
        if (draw) draw_text(g, p.note, g_f.body.get(), g_t.text2, RectF(m, y, cw, nh), kLeft, true, false);
        y += nh;
    }
    if (!p.link.empty()) {
        const RectF r(m, y + px(8), (std::min)(cw, text_width(g, p.link, g_f.body.get()) + px(4)), px(22));
        p.targets.push_back({r, kPromptLink});
        if (draw) {
            const Color ink = p.hot == kPromptLink ? mix(g_t.accent, g_t.text, 0.3f) : g_t.accent;
            draw_text(g, p.link, g_f.body.get(), ink, r);
            if (p.focus == kPromptLink) draw_focus_ring(g, r, px(4));
        }
        if (!p.link2.empty()) {
            const float x2 = r.GetRight() + px(20);
            const RectF r2(x2, r.Y, (std::max)(0.0f, (std::min)(m + cw - x2, text_width(g, p.link2, g_f.body.get()) + px(4))),
                           px(22));
            p.targets.push_back({r2, kPromptLink2});
            if (draw) {
                const Color ink = p.hot == kPromptLink2 ? mix(g_t.accent, g_t.text, 0.3f) : g_t.accent;
                draw_text(g, p.link2, g_f.body.get(), ink, r2);
                if (p.focus == kPromptLink2) draw_focus_ring(g, r2, px(4));
            }
        }
        y += px(30);
    }
    const bool checks = !p.switches.empty() && p.switches[0].check;
    if (checks) {
        y += px(10);
        for (size_t i = 0; i < p.switches.size(); ++i) {
            const int id = kPromptSwitch + static_cast<int>(i);
            const float th = (std::max)(px(20), text_height(g, p.switches[i].title, g_f.body.get(), cw - px(30)) + px(2));
            const RectF row(m, y, cw, th + px(8)), box(m, y + px(4), px(20), px(20));
            p.targets.push_back({row, id});
            if (draw) {
                draw_check_box(g, p.switches[i].on, box, p.hot == id);
                draw_text(g, p.switches[i].title, g_f.body.get(), g_t.text, RectF(m + px(30), y + px(4), cw - px(30), th),
                          kLeft, true, false);
                if (p.focus == id) draw_focus_ring(g, box, px(4));
            }
            y += row.Height;
        }
    } else if (!p.switches.empty()) {
        y += px(14);
        const float rh = px(44);
        const RectF card(m, y, cw, rh * p.switches.size());
        if (draw) fill_round(g, card, px(kRadiusCard), g_t.card);
        for (size_t i = 0; i < p.switches.size(); ++i) {
            const RectF r(m, y + i * rh, cw, rh);
            const int id = kPromptSwitch + static_cast<int>(i);
            p.targets.push_back({r, id});
            if (!draw) continue;
            if (p.hot == id) {
                GraphicsPath clip;
                rounded(clip, card, px(kRadiusCard));
                g.SetClip(&clip);
                Gdiplus::SolidBrush b(g_t.hover);
                g.FillRectangle(&b, r);
                g.ResetClip();
            }
            if (i > 0) {
                Gdiplus::Pen line(g_t.divider, 1);
                g.DrawLine(&line, r.X + px(14), r.Y, r.GetRight() - px(14), r.Y);
            }
            draw_text(g, p.switches[i].title, g_f.body.get(), g_t.text, RectF(r.X + px(14), r.Y, r.Width - px(80), rh));
            const RectF sr(r.GetRight() - px(14) - px(44), r.Y + (rh - px(22)) / 2, px(44), px(22));  // as Settings
            draw_switch(g, p.switches[i].on, sr, p.hot == id);
            if (p.focus == id) draw_focus_ring(g, sr, sr.Height / 2);
        }
        if (draw) stroke_round(g, card, px(kRadiusCard), g_t.border);
        y += card.Height;
    }
    if (p.footer) {
        // As the install window's footer: a band of the window colour, the buttons right-aligned in it.
        y += px(24);
        const float foot = px(64);
        if (draw) {
            Gdiplus::SolidBrush bar(g_t.bg);
            g.FillRectangle(&bar, RectF(0, y, W, foot));
            Gdiplus::Pen line(g_t.border, 1);
            g.DrawLine(&line, 0.0f, y, W, y);
        }
        float total = 0;
        for (size_t i = 0; i < p.buttons.size(); ++i) total += button_width(g, p.buttons[i].second, i == 0) + (i ? px(8) : 0);
        float x = W - m - total;
        for (size_t i = 0; i < p.buttons.size(); ++i) {
            const float bw = button_width(g, p.buttons[i].second, i == 0);
            const RectF r(x, y + (foot - px(32)) / 2, bw, px(32));
            p.targets.push_back({r, p.buttons[i].first});
            if (draw) {
                draw_button(g, r, p.buttons[i].second, p.hot == p.buttons[i].first, i == 0, false);
                if (p.focus == p.buttons[i].first) draw_focus_ring(g, r, px(kRadiusControl));
            }
            x += bw + px(8);
        }
        return y + foot;
    }
    y += px(22);
    for (size_t i = 0; i < p.buttons.size(); ++i) {
        const RectF r(m, y, cw, px(32));
        p.targets.push_back({r, p.buttons[i].first});
        if (draw) {
            draw_button(g, r, p.buttons[i].second, p.hot == p.buttons[i].first, i == 0 && !p.plain, false);
            if (p.focus == p.buttons[i].first) draw_focus_ring(g, r, px(kRadiusControl));
        }
        y += px(40);
    }
    return y + px(16);
}

// The owner takes input again before the dialog goes, so Windows hands it the activation.
void close_prompt(Prompt& p) {
    if (p.owner) EnableWindow(p.owner, TRUE);
    if (p.window) DestroyWindow(p.window);
}

void prompt_answer(Prompt& p, int id) {
    if (id <= 0) return;
    if (id == kPromptLink) {
        if (p.on_link) p.on_link();
        return;
    }
    if (id == kPromptLink2) {
        if (p.on_link2) p.on_link2();
        return;
    }
    if (id >= kPromptSwitch && id < kPromptSwitch + static_cast<int>(p.switches.size())) {
        p.switches[static_cast<size_t>(id - kPromptSwitch)].on = !p.switches[static_cast<size_t>(id - kPromptSwitch)].on;
        InvalidateRect(p.window, nullptr, FALSE);
        return;
    }
    p.pressed = id;
    close_prompt(p);
}

LRESULT CALLBACK prompt_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* p = reinterpret_cast<Prompt*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_NCCREATE:
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
            break;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT:
            buffered_paint(hwnd, [&](Graphics& g, float w, float) { layout_prompt(g, *p, w, true); });
            return 0;
        case WM_MOUSEMOVE: track_hot(hwnd, lp, p->targets, &p->hot, &p->tracking); return 0;
        case WM_MOUSELEAVE:
            p->tracking = false;
            p->hot = 0;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                SetCursor(LoadCursorW(nullptr, p->hot ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_LBUTTONDOWN: p->focus = 0; return 0;
        case WM_LBUTTONUP: prompt_answer(*p, find_target(p->targets, lp)); return 0;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) {
                close_prompt(*p);
            } else if (wp == VK_RETURN) {
                prompt_answer(*p, p->focus ? p->focus : p->default_id);
            } else if (wp == VK_SPACE && p->focus) {
                prompt_answer(*p, p->focus);
            } else if (wp == VK_TAB || wp == VK_UP || wp == VK_DOWN || wp == VK_LEFT || wp == VK_RIGHT) {
                std::vector<int> ids;
                for (const auto& t : p->targets) ids.push_back(t.second);
                if (!ids.empty()) {
                    // Left and right go by what the eye sees: mirrored in Hebrew.
                    const bool back = wp == VK_UP || (wp == VK_TAB && GetKeyState(VK_SHIFT) < 0) ||
                                      wp == (g_rtl ? VK_RIGHT : VK_LEFT);
                    const auto it = std::find(ids.begin(), ids.end(), p->focus);
                    int i = it == ids.end() ? (back ? 0 : -1) : static_cast<int>(it - ids.begin());
                    i = ((back ? i - 1 : i + 1) % static_cast<int>(ids.size()) + static_cast<int>(ids.size())) %
                        static_cast<int>(ids.size());
                    p->focus = ids[static_cast<size_t>(i)];
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            return 0;
        case WM_CLOSE: close_prompt(*p); return 0;
        case WM_TIMER: close_prompt(*p); return 0;  // no answer in time: nothing changes
        case WM_DESTROY: p->window = nullptr; return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Shows `p` centred over `over` (screen coordinates) and returns the button pressed (0: none). With an
// owner the dialog is modal to it; without one it stays above every window, as an app's question must.
int show_prompt(Prompt& p, const RECT& over) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = prompt_proc;
        wc.hInstance = g_inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(g_inst, MAKEINTRESOURCEW(1));
        wc.lpszClassName = L"WinApkPrompt";
        RegisterClassExW(&wc);
        registered = true;
    }
    const int w = ipx(p.footer ? 440 : 400);
    HDC dc = GetDC(nullptr);
    float need = 0;
    {
        Graphics g(dc);
        need = layout_prompt(g, p, static_cast<float>(w), false);
    }
    ReleaseDC(nullptr, dc);
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    const DWORD ex = layout_style() | (p.owner ? 0 : WS_EX_TOPMOST);
    RECT frame{0, 0, w, static_cast<LONG>(std::lround(need))};
    AdjustWindowRectEx(&frame, style, FALSE, ex);
    const int fw = frame.right - frame.left, fh = frame.bottom - frame.top;
    const int x = (over.left + over.right - fw) / 2;
    const int y = (std::max)(static_cast<int>(over.top), static_cast<int>((over.top + over.bottom - fh) / 2));
    const std::wstring& caption = p.caption.empty() ? p.app : p.caption;
    p.window = CreateWindowExW(ex, L"WinApkPrompt", caption.c_str(), style, x, y, fw, fh, p.owner, nullptr, g_inst, &p);
    if (!p.window) return 0;
    theme_window(p.window);
    const int corner = 2;  // DWMWCP_ROUND, Windows 11, as the splash and the menus
    DwmSetWindowAttribute(p.window, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corner, sizeof(corner));
    if (p.timeout) SetTimer(p.window, 1, kAskTimeoutMs, nullptr);
    // The ring starts on what Enter answers, as a Windows dialog marks its default button: where that is
    // Cancel under an accent Remove, the eye otherwise takes the accent one for Enter's.
    if (!p.focus) p.focus = p.default_id;
    if (p.owner) EnableWindow(p.owner, FALSE);
    ShowWindow(p.window, SW_SHOW);
    SetForegroundWindow(p.window);
    MSG m;
    while (p.window) {
        const BOOL got = GetMessageW(&m, nullptr, 0, 0);
        if (got <= 0) {
            if (got == 0) PostQuitMessage(static_cast<int>(m.wParam));  // for the loop that ends the program
            close_prompt(p);
            break;
        }
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return p.pressed;
}

RECT work_area_with_cursor() {
    POINT cursor;
    GetCursorPos(&cursor);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY), &mi);
    return mi.rcWork;
}

BOOL CALLBACK find_app_window(HWND w, LPARAM lp) {
    auto* p = reinterpret_cast<Prompt*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (pid == static_cast<DWORD>(reinterpret_cast<uintptr_t>(p->near_window)) && IsWindowVisible(w) &&
        !GetWindow(w, GW_OWNER)) {
        p->near_window = w;
        return FALSE;
    }
    return TRUE;
}

// An app's question: over the window of process `pid`, not owned by it (that would tie two processes'
// input together) but kept above it.
int run_prompt(Prompt& p, DWORD pid) {
    p.near_window = reinterpret_cast<HWND>(static_cast<uintptr_t>(pid));
    EnumWindows(find_app_window, reinterpret_cast<LPARAM>(&p));
    if (reinterpret_cast<uintptr_t>(p.near_window) == pid) p.near_window = nullptr;  // no window found
    p.owner = nullptr;
    p.timeout = true;
    RECT near_r;
    if (!p.near_window || !GetWindowRect(p.near_window, &near_r)) near_r = work_area_with_cursor();
    return show_prompt(p, near_r);
}

// The launcher's own question, modal to `owner` while that is on screen; else on its own, in front, with
// a taskbar button, where the mouse is (an install from Explorer shows no library window).
int run_dialog(Prompt& p, HWND owner) {
    if (p.caption.empty()) p.caption = L"Win.apk";
    p.footer = true;
    p.owner = owner && IsWindowVisible(owner) && !IsIconic(owner) ? owner : nullptr;
    RECT near_r;
    if (!p.owner || !GetWindowRect(p.owner, &near_r)) near_r = work_area_with_cursor();
    return show_prompt(p, near_r);
}

// Asks before doing `action` (a verb: "Sign out", "Clear library"). Enter is Cancel: these undo nothing.
bool confirm(const std::wstring& main, const std::wstring& content, const wchar_t* action) {
    Prompt p;
    p.question = main;
    p.note = content;
    p.buttons = {{IDOK, action}, {IDCANCEL, tr(L"Cancel")}};
    p.default_id = IDCANCEL;
    return run_dialog(p, owner()) == IDOK;
}

// ---------------------------------------------------------------------------
// Android's dialogs about an app's end: "<app> has stopped" / "<app> keeps stopping" when it crashed, and
// "<app> isn't responding" when its main thread has not taken input for five seconds. Their titles and
// buttons are AOSP 14's (framework-res: aerr_application, aerr_application_repeated, app_info,
// aerr_close_app, anr_application_process, aerr_wait), the buttons a plain list as AppErrorDialog's and
// AppNotRespondingDialog's are.

void report_problem(HWND parent, const std::wstring& name, const std::wstring& label, const fs::path& log,
                    const std::wstring& what);
void show_info(const App& app);

enum { kAppInfo = 300, kCloseApp, kWait };

// A dialog about the app `name`: its icon and name head it, as AOSP's do not (Android's has the name in the
// title alone; on a PC the app's icon says which window it is about).
Prompt app_end_prompt(const std::wstring& name, const std::wstring& label, const std::wstring& question) {
    Prompt p;
    p.app = label;
    p.icon = rounded_icon(app_dir(name) / L"icon.png", ipx(40));
    p.question = question;
    p.plain = true;
    return p;
}

// ActivityManager's AppNotRespondingDialog: Close app ends the app, Wait leaves it. Enter, Esc and the
// caption's close are Wait, as Android's back is (the dialog's cancel is Wait). Over `over` (screen
// coordinates); modal to `owner` when there is one, else above every window.
void not_responding_choices(Prompt& p) {
    p.buttons = {{kCloseApp, tr(L"Close app")}, {kWait, tr(L"Wait")}};
    p.default_id = kWait;
}

int ask_not_responding(Prompt& p, HWND owner, const RECT& over) {
    not_responding_choices(p);
    p.owner = owner;
    const int pressed = show_prompt(p, over);
    return pressed == kCloseApp ? kCloseApp : kWait;
}

// ProcessList.MIN_CRASH_INTERVAL: a crash this soon after the app's last one is a repeat ("keeps stopping").
constexpr long long kMinCrashIntervalSeconds = 60;

// Whether the app crashed within kMinCrashIntervalSeconds before now, which is recorded as its last crash
// (AppErrors' persistent crash times, in apps\<name>\last-crash, as the launcher may not live between).
bool crash_repeats(const std::wstring& name) {
    const fs::path f = app_dir(name) / L"last-crash";
    const long long now = static_cast<long long>(std::time(nullptr));
    long long last = 0;
    if (std::ifstream in(f); in) in >> last;
    std::error_code ec;
    fs::create_directories(f.parent_path(), ec);
    if (std::ofstream out(f, std::ios::trunc); out) out << now;
    return last > 0 && now >= last && now - last < kMinCrashIntervalSeconds;
}

// AppErrors' crash dialog for an app that crashed (RunEnd::kCrashed): App info opens its details page,
// Close app (Enter, Esc, or five minutes without an answer: AppErrorDialog's DISMISS_TIMEOUT) leaves it
// closed. Show log and Report a problem are links under the title, as on the failed starting window.
// "Open app again" is not offered: AOSP shows it only with the global show_restart_in_crash_dialog on.
// Android 14 shows the dialog on a repeated crash and, with "Always show crash dialog" on (Developer
// options), on the first: Win.apk shows it each time, as there is no home screen for a crashed app to
// leave the user on. Over `over` (screen coordinates), above every window.
void show_crash_dialog(const std::wstring& name, const RECT& over) {
    const std::wstring label = label_of(name);
    const std::wstring title = fmt(crash_repeats(name) ? L"{0} keeps stopping" : L"{0} has stopped", {label});
    Prompt p = app_end_prompt(name, label, title);
    p.buttons = {{kAppInfo, tr(L"App info")}, {kCloseApp, tr(L"Close app")}};
    p.default_id = kCloseApp;
    p.timeout = true;
    const fs::path log = run_log(name);
    p.link = tr(L"Show log");
    p.on_link = [&p, log] {  // the log in its window, the dialog answered as Close app
        p.pressed = kCloseApp;
        close_prompt(p);
        show_logs(log);
    };
    p.link2 = tr(L"Report a problem");
    p.on_link2 = [&p, name, label, log, title] { report_problem(p.window, name, label, log, title); };
    if (show_prompt(p, over) == kAppInfo) {
        if (const App* app = find_app(name)) {
            show_library(SW_SHOWNORMAL);
            show_info(*app);
        }
    }
}


std::string read_tail(const fs::path& p, size_t max_bytes);

// A failure of Win.apk itself rather than of the app, from the log's last lines: the runtime did not start
// (launcher_error_line), or a part of the install is missing or does not load (host/aosp_gfx.cpp's error
// lines). Empty when it is not one: the app's own failure.
std::wstring runtime_failure_reason(const fs::path& log) {
    const std::string tail = read_tail(log, 64 * 1024);
    if (tail.find("[E] launcher: could not start ") != std::string::npos)
        return tr(L"Win.apk could not start its runtime. See the log for details.");
    if (tail.find("[win.apk] error: AOSP graphics:") != std::string::npos ||
        tail.find("[E] launcher: winapk-runtime.exe ended with 0xC0000135") != std::string::npos ||
        tail.find("[E] launcher: winapk-runtime.exe ended with 0xC0000139") != std::string::npos ||
        tail.find("[E] launcher: winapk-runtime.exe ended with 0xC000007B") != std::string::npos)
        return tr(L"A part of Win.apk is missing or does not load. Install Win.apk again.");
    return {};
}

// The lines of a log that say why an app failed: its errors, exceptions and the exit. Each line starts
// with a left-to-right mark, so a Hebrew (mirrored) dialog keeps an English log line's own order.
std::wstring log_summary(const fs::path& log, bool* no_native_code) {
    std::vector<std::wstring> hits;
    const std::wstring raw = utf8_to_wide(read_tail(log, 256 * 1024));
    size_t at = 0;
    while (at < raw.size()) {
        size_t end = raw.find(L'\n', at);
        if (end == std::wstring::npos) end = raw.size();
        std::wstring l = raw.substr(at, end - at);
        at = end + 1;
        if (!l.empty() && l.back() == L'\r') l.pop_back();
        if (l.find(L"not shipped in this APK") != std::wstring::npos) *no_native_code = true;
        const bool why = l.rfind(L"[E]", 0) == 0 || l.rfind(L"[F]", 0) == 0 || l.find(L"FATAL") != std::wstring::npos ||
                         l.find(L"Exception:") != std::wstring::npos || l.find(L"Error:") != std::wstring::npos ||
                         l.find(L"System.exit(") != std::wstring::npos;
        if (!why) continue;
        if (l.size() > 220) l = l.substr(0, 220) + L"\x2026";
        hits.push_back(std::move(l));
    }
    std::wstring out;
    for (size_t i = hits.size() > 12 ? hits.size() - 12 : 0; i < hits.size(); ++i) out += L"\x200E" + hits[i] + L"\n";
    if (!out.empty()) out.pop_back();
    return out;
}

// An error the user can do nothing about but read: the sentence, and Show log.
// Close and Show log, as the install window's and the splash's failures have them.
void error_with_log(const std::wstring& main, const std::wstring& content, const fs::path& log) {
    Prompt p;
    p.question = main;
    p.note = content;
    p.buttons = {{IDCLOSE, tr(L"Close")}, {IDRETRY, tr(L"Show log")}};
    p.default_id = IDCLOSE;
    if (run_dialog(p, owner()) == IDRETRY) show_logs(log);
}

// ---------------------------------------------------------------------------
// Report a problem: GitHub's "An app does not work" form (.github/ISSUE_TEMPLATE/app_problem.yml) opened in the
// browser with what the launcher knows filled in (its one field, by the field's id), and the logs folder with the
// app's log (and its newest crash dump) selected to drag in. Nothing is uploaded by itself: a log can hold the user's name and paths,
// so what goes in the report is the user's choice. The log stays out of the URL too, which GitHub cuts at about 8 KB.

constexpr wchar_t kIssuesUrl[] = L"https://github.com/israel2176/win.apk/issues";

// A query value: UTF-8, everything but the unreserved characters percent-encoded.
std::wstring url_escape(const std::wstring& s) {
    static const wchar_t hex[] = L"0123456789ABCDEF";
    std::wstring out;
    for (const unsigned char c : wide_to_utf8(s)) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            out += static_cast<wchar_t>(c);
        } else {
            out += L'%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

// "Windows 11 23H2 (build 22631), x64". RtlGetVersion, since GetVersionEx answers what the manifest claims; 11 is a
// build of 10.0 from 22000 on.
std::wstring windows_version() {
    OSVERSIONINFOW v{};
    v.dwOSVersionInfoSize = sizeof(v);
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    if (auto get = reinterpret_cast<RtlGetVersionFn>(
            reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")))) {
        get(&v);
    }
    std::wstring out = L"Windows ";
    if (v.dwMajorVersion == 10 && v.dwBuildNumber >= 22000) out += L"11";
    else out += std::to_wstring(v.dwMajorVersion) + (v.dwMinorVersion ? L"." + std::to_wstring(v.dwMinorVersion) : L"");
    wchar_t display[64] = {};
    DWORD size = sizeof(display);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"DisplayVersion",
                     RRF_RT_REG_SZ, nullptr, display, &size) == ERROR_SUCCESS && display[0]) {
        out += L" " + std::wstring(display);
    }
    out += L" (build " + std::to_wstring(v.dwBuildNumber) + L")";
    // The machine itself, not the x64 that Windows on Arm emulates for us.
    USHORT process = 0, native = 0;
    using IsWow64Process2Fn = BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*);
    auto wow = reinterpret_cast<IsWow64Process2Fn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process2")));
    constexpr USHORT kArm64 = 0xAA64;  // IMAGE_FILE_MACHINE_ARM64
    const bool arm = wow && wow(GetCurrentProcess(), &process, &native) && native == kArm64;
    return out + (arm ? L", Arm64" : L", x64");
}

// The primary display's adapter, as Device Manager names it.
std::wstring gpu_name() {
    DISPLAY_DEVICEW d{};
    d.cb = sizeof(d);
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &d, 0); ++i) {
        if (d.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) return d.DeviceString;
        d.cb = sizeof(d);
    }
    return {};
}

// The newest logs\crash-<name>-<time>.dmp, if the runtime wrote one.
fs::path newest_dump(const std::wstring& name) {
    const std::wstring prefix = L"crash-" + name + L"-";
    fs::path best;
    fs::file_time_type best_time{};
    std::error_code ec;
    for (fs::directory_iterator it(logs_dir(), ec), end; !ec && it != end; it.increment(ec)) {
        const std::wstring f = it->path().filename().wstring();
        if (f.rfind(prefix, 0) != 0 || it->path().extension() != L".dmp") continue;
        std::error_code ec2;
        const auto t = fs::last_write_time(it->path(), ec2);
        if (!ec2 && (best.empty() || t > best_time)) {
            best = it->path();
            best_time = t;
        }
    }
    return best;
}

// Explorer on the folder of `files`, with all of them selected (show_in_explorer's /select takes only one).
void select_in_explorer(const std::vector<fs::path>& files) {
    if (files.empty()) return;
    PIDLIST_ABSOLUTE folder = ILCreateFromPathW(files.front().parent_path().c_str());
    std::vector<PIDLIST_ABSOLUTE> items;
    for (const fs::path& f : files)
        if (PIDLIST_ABSOLUTE p = ILCreateFromPathW(f.c_str())) items.push_back(p);
    std::vector<PCUITEMID_CHILD> children;
    for (PIDLIST_ABSOLUTE p : items) children.push_back(ILFindLastID(p));
    if (!folder || children.empty() ||
        FAILED(SHOpenFolderAndSelectItems(folder, static_cast<UINT>(children.size()), children.data(), 0))) {
        show_in_explorer(files.front());
    }
    for (PIDLIST_ABSOLUTE p : items) ILFree(p);
    if (folder) ILFree(folder);
}

// Asks first, with a link to the open reports on the same app so a second one is not opened, then opens the form and
// the logs. `name` is the library name, `log` the log that tells what happened, `what` the sentence the user saw.
void report_problem(HWND parent, const std::wstring& name, const std::wstring& label, const fs::path& log,
                    const std::wstring& what) {
    const std::vector<std::string> pkg = package_of(name);
    const std::wstring package = pkg.empty() ? std::wstring() : utf8_to_wide(pkg[0]);
    const std::wstring app_version = pkg.size() > 2 ? utf8_to_wide(pkg[2]) : std::wstring();

    const std::wstring search = std::wstring(kIssuesUrl) + L"?q=" +
                                url_escape(L"is:issue is:open \"" + (package.empty() ? label : package) + L"\"");
    Prompt p;
    p.question = fmt(L"Report a problem with {0}", {label});
    p.note = tr(L"A GitHub form opens with the version and system details filled in. The log is selected in its "
                L"folder for you to attach; nothing is sent automatically.");
    p.link = tr(L"Check whether it was already reported");
    p.on_link = [search] { ShellExecuteW(nullptr, L"open", search.c_str(), nullptr, nullptr, SW_SHOWNORMAL); };
    p.buttons = {{IDOK, tr(L"Open report")}, {IDCANCEL, tr(L"Cancel")}};
    p.default_id = IDOK;
    if (run_dialog(p, parent) != IDOK) return;

    // The title: "<App name> <version>: <what goes wrong>".
    const std::wstring app_line = label + (app_version.empty() ? L"" : L" " + app_version) +
                                  (package.empty() ? L"" : L" (" + package + L")");
    std::wstring url = std::wstring(kIssuesUrl) + L"/new?template=app_problem.yml";
    url += L"&title=" + url_escape(label + (app_version.empty() ? L"" : L" " + app_version) + L": " + what);
    // The form has one field; the versions go under the user's sentence in it.
    std::wstring body = what + (what.empty() ? L"" : L"\n\n") + L"App: " + app_line + L"\nWin.apk: " + version() +
                        L"\nWindows: " + windows_version();
    if (const std::wstring gpu = gpu_name(); !gpu.empty()) body += L"\nGraphics: " + gpu;
    url += L"&what-happens=" + url_escape(body);
    ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);

    std::vector<fs::path> files;
    std::error_code ec;
    if (!log.empty() && fs::exists(log, ec)) files.push_back(log);
    if (const fs::path dump = newest_dump(name); !dump.empty()) files.push_back(dump);
    if (files.empty()) open_path(logs_dir());
    else select_in_explorer(files);
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

// The library name for a file, which names its folders (apps\<name>, library\<name>.apk): the
// package when the file is named after it, as the sites that share APKs name them
// ("com.waze_5.26.5.0-1030744_1arch_2dpi_24lang_<hash>_apkmirror.com.apkm" -> "com.waze"); else
// the file's name up to its version ("WhatsApp_2.24.1" -> "WhatsApp"). An app the library has
// under that package keeps its name, so the file updates it.
std::wstring install_name(const fs::path& source) {
    const std::wstring stem = source.stem().wstring();
    auto letter = [](wchar_t c) { return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z'); };
    auto digit = [](wchar_t c) { return c >= L'0' && c <= L'9'; };
    // A leading package: dot-separated identifiers, each starting with a letter.
    size_t end = 0;
    int parts = 0;
    while (end < stem.size() && letter(stem[end])) {
        size_t j = end + 1;
        while (j < stem.size() && (letter(stem[j]) || digit(stem[j]) || stem[j] == L'_')) ++j;
        ++parts;
        end = j;
        if (end + 1 < stem.size() && stem[end] == L'.' && letter(stem[end + 1])) ++end;
        else break;
    }
    std::wstring name;
    if (parts >= 2) {
        name = stem.substr(0, end);
        // "waze_5" took the version's first number in: an identifier ends where "_<digit>" starts.
        for (size_t k = name.rfind(L'.') + 1; k + 1 < name.size(); ++k) {
            if (name[k] == L'_' && digit(name[k + 1])) {
                name.resize(k);
                break;
            }
        }
        const wchar_t next = end < stem.size() ? stem[end] : L'_';
        if (!wcschr(L"_- (.", next)) name.clear();  // "com.example.app!" is not a package
    }
    if (name.empty()) {
        name = stem;
        for (size_t k = 1; k + 1 < name.size(); ++k) {
            const bool sep = name[k] == L'_' || name[k] == L' ' || name[k] == L'-';
            const size_t v = k + 1 + ((name[k + 1] == L'v' || name[k + 1] == L'V') ? 1 : 0);
            if (sep && v < name.size() && digit(name[v])) {
                name.resize(k);
                break;
            }
        }
        while (!name.empty() && name.back() == L' ') name.pop_back();
        if (name.empty()) name = stem;
    }
    const auto it = g_packages.find(wide_to_utf8(name));
    if (it != g_packages.end() && find_app(it->second)) return it->second;
    return file_name_safe(name);
}

// Queues an install. A quiet one (the Store's own app, which Win.apk downloads)
// installs a download it then removes, and shows no install window: the Store's
// page shows its progress.
void install(const fs::path& source, std::wstring name = L"", bool quiet = false,
             std::vector<SplitFile> splits = {}) {
    if (name.empty()) name = install_name(source);
    if (is_pending(name)) return;  // the same file twice
    if (!any_pending() && !g_install_wnd) g_jobs.clear();
    begin_install(name);
    Job job{name};
    job.file = source.filename().wstring();
    job.update = find_app(name) != nullptr;
    g_jobs.push_back(job);
    CloseHandle(CreateThread(nullptr, 0, install_thread, new InstallJob{source, name, quiet, std::move(splits)}, 0,
                             nullptr));
    update_timer();
    rebuild_tiles();
    if (!quiet || g_install_wnd) show_install_window();
}

// The installs of a batch of files opened at once. Several .apk files are first asked which app each is a
// part of (winapk-runtime --apk-set-to, on a thread: a JVM's start), and those of one app are installed as
// one session, as `adb install-multiple` installs them (installs.h, group_install_sets); a lone .apk and a
// bundle install as they are. Should the question get no answer, each file installs alone, and AppInstall
// refuses a split without its base as Android does.
DWORD WINAPI install_sets_thread(void* param) {
    std::unique_ptr<std::vector<fs::path>> apks(static_cast<std::vector<fs::path>*>(param));
    wchar_t tmp_dir[MAX_PATH + 1];
    wchar_t tmp[MAX_PATH + 1];
    std::wstring text;
    if (GetTempPathW(MAX_PATH + 1, tmp_dir) && GetTempFileNameW(tmp_dir, L"wap", 0, tmp)) {
        std::wstring args = L"--apk-set-to \"" + std::wstring(tmp) + L"\"";
        for (const fs::path& f : *apks) args += L" \"" + f.wstring() + L"\"";
        HANDLE p = start_runtime(args, logs_dir() / L"install-set.log");
        if (p && wait_exit(p) == 0) {
            std::ifstream in(tmp, std::ios::binary);
            text = utf8_to_wide(std::string(std::istreambuf_iterator<char>(in), {}));
        }
        DeleteFileW(tmp);
    }
    std::vector<winapk::launcher::ApkPart> parts = winapk::launcher::parse_apk_set(text);
    if (parts.size() != apks->size()) {  // no answer: each file alone
        parts.clear();
        for (const fs::path& f : *apks) parts.push_back({f.wstring(), L"", L""});
    }
    auto* sets = new std::vector<winapk::launcher::InstallSet>(winapk::launcher::group_install_sets(parts));
    if (!PostMessageW(g_main, kInstallSets, 0, reinterpret_cast<LPARAM>(sets))) delete sets;
    return 0;
}

void probe_install(const fs::path& file, std::vector<SplitFile> splits = {});

void install_files(const std::vector<fs::path>& files) {
    auto* apks = new std::vector<fs::path>;
    for (const fs::path& f : files) {
        std::wstring ext = f.extension().wstring();
        for (wchar_t& c : ext) c = static_cast<wchar_t>(towlower(c));
        if (ext == L".apk") apks->push_back(f);
        else probe_install(f);
    }
    if (apks->size() < 2) {
        for (const fs::path& f : *apks) probe_install(f);
        delete apks;
        return;
    }
    CloseHandle(CreateThread(nullptr, 0, install_sets_thread, apks, 0, nullptr));
}

void add_from_dialog() {
    install_files(pick_files(true, tr(L"Install an Android app")));
}

Splash* show_splash(const std::wstring& name, bool compact = false);
HWND start_window(const std::wstring& name);

// Brings the window of an app started from here to the front.
void bring_forward(const std::wstring& name) {
    const DWORD pid = run_pid(name);
    struct Find {
        DWORD pid;
        HWND found;
    } f{pid, nullptr};
    if (pid) {
        EnumWindows([](HWND w, LPARAM lp) -> BOOL {
            auto* f = reinterpret_cast<Find*>(lp);
            DWORD p = 0;
            GetWindowThreadProcessId(w, &p);
            if (p != f->pid || !IsWindowVisible(w) || GetWindow(w, GW_OWNER)) return TRUE;
            f->found = w;
            return FALSE;
        }, reinterpret_cast<LPARAM>(&f));
    }
    if (f.found) {
        if (IsIconic(f.found)) ShowWindow(f.found, SW_RESTORE);
        SetForegroundWindow(f.found);
    }
}

// Opens the app: its splash at once, the runtime on a thread (run_thread), which reports back.
// ---- an app with no launcher activity ----
//
// On a device an app whose manifest has no MAIN/LAUNCHER activity (a plug-in, a companion, an app another
// app or a link opens) has no icon; its screens are reached by an explicit start of an activity other apps
// may start, as `am start -n <package>/<activity>` and the "activity launcher" tools do. Here the library
// has its tile, so opening it offers those activities (AppMeta lists them in library\<name>.package after
// the empty fifth line) and starts the one chosen the same way: MACAPK_LAUNCH_ACTIVITY, which the runtime
// honours only for an activity the manifest declares. The choice is kept, as a shortcut to that activity
// would be, in library\<name>.activity; App details changes it.

std::vector<std::string> package_of(const std::wstring& name);

struct StartableActivity {
    std::wstring cls, label;
};

// Line 5 of the package file is the launcher activity, empty when the app has none (AppMeta). An app
// installed before the line was written has four lines: it is opened as before.
bool has_no_launcher(const std::vector<std::string>& lines) { return lines.size() >= 5 && lines[4].empty(); }

std::vector<StartableActivity> startable_activities(const std::vector<std::string>& lines) {
    std::vector<StartableActivity> out;
    for (size_t i = 5; i < lines.size(); ++i) {
        if (lines[i].empty()) continue;
        const size_t tab = lines[i].find('\t');
        StartableActivity a;
        a.cls = utf8_to_wide(lines[i].substr(0, tab));
        if (tab != std::string::npos) a.label = utf8_to_wide(lines[i].substr(tab + 1));
        out.push_back(std::move(a));
    }
    return out;
}

// Its label, else the class's own name.
std::wstring activity_title(const StartableActivity& a) {
    if (!a.label.empty()) return a.label;
    const size_t dot = a.cls.find_last_of(L'.');
    return dot == std::wstring::npos ? a.cls : a.cls.substr(dot + 1);
}

fs::path start_activity_file(const std::wstring& name) { return library_dir() / (name + L".activity"); }

// The activity chosen for the app, if it is still one the app lets others start.
std::wstring chosen_activity(const std::wstring& name, const std::vector<StartableActivity>& list) {
    std::ifstream f(start_activity_file(name), std::ios::binary);
    std::string line;
    if (!std::getline(f, line)) return L"";
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const std::wstring cls = utf8_to_wide(line);
    for (const StartableActivity& a : list) {
        if (a.cls == cls) return cls;
    }
    return L"";
}

// Asks which activity to open, and keeps the answer. False when there is none or the user cancels.
bool choose_activity(const App& app, const std::vector<StartableActivity>& list, std::wstring* cls) {
    Prompt p;
    p.app = app.label;
    p.icon = rounded_icon(app_dir(app.name) / L"icon.png", ipx(40));
    p.caption = app.label;
    if (list.empty()) {
        p.question = fmt(L"{0} cannot be opened", {app.label});
        p.note = tr(L"It has no screen to open from a launcher, and none that other apps may open. It works for other "
                    L"apps (as a plug-in or a service), which open it themselves.");
        p.buttons = {{IDOK, tr(L"OK")}};
        run_dialog(p, owner());
        return false;
    }
    p.question = tr(L"Which screen should open?");
    p.note = fmt(L"{0} has no screen to open from a launcher, so on a phone it has no icon either. These are the "
                 L"screens other apps can open; the one you choose opens from now on, and App details can change it.",
                 {app.label});
    constexpr int kFirst = 0x6000;
    for (size_t i = 0; i < list.size(); ++i) {
        p.buttons.push_back({kFirst + static_cast<int>(i), activity_title(list[i])});
    }
    p.buttons.push_back({IDCANCEL, tr(L"Cancel")});
    p.default_id = kFirst;
    // Stacked full width, one per activity: run_dialog would put them in a footer row.
    const HWND own = owner();
    p.owner = own && IsWindowVisible(own) && !IsIconic(own) ? own : nullptr;
    RECT near_r;
    if (!p.owner || !GetWindowRect(p.owner, &near_r)) near_r = work_area_with_cursor();
    const int pressed = show_prompt(p, near_r);
    if (pressed < kFirst || pressed >= kFirst + static_cast<int>(list.size())) return false;
    *cls = list[static_cast<size_t>(pressed - kFirst)].cls;
    std::ofstream f(start_activity_file(app.name), std::ios::binary | std::ios::trunc);
    f << wide_to_utf8(*cls) << "\n";
    return true;
}

void launch(const App& app) {
    if (is_pending(app.name)) {
        info(fmt(L"{0} is still being installed.", {app.label}), kInfo);
        return;
    }
    // An app with no launcher activity opens the activity chosen for it (see above).
    std::wstring start_cls;
    if (const std::vector<std::string> lines = package_of(app.name); has_no_launcher(lines)) {
        const std::vector<StartableActivity> list = startable_activities(lines);
        start_cls = chosen_activity(app.name, list);
        if (start_cls.empty() && !choose_activity(app, list, &start_cls)) return;
    }
    if (!begin_run(app.name)) {
        bring_forward(app.name);
        return;
    }
    refresh_running();
    // The app's window opens over the starting window, and takes its place (host/win32_window.cpp).
    auto* job = new RunJob{app.name, app.apk, {}};
    if (!start_cls.empty()) job->env.push_back(L"MACAPK_LAUNCH_ACTIVITY=" + start_cls);
    if (const HWND start = start_window(app.name)) {
        job->env.push_back(L"WINAPK_START_WINDOW=" + std::to_wstring(reinterpret_cast<uintptr_t>(start)));
    }
    CloseHandle(CreateThread(nullptr, 0, run_thread, job, 0, nullptr));
    update_timer();
}

void appops_forget(const std::string& pkg);
std::vector<std::string> package_of(const std::wstring& name);
namespace appops {
bool stop_runtime_of(const fs::path& apk, DWORD wait_ms = 0);
}

// How long Stop waits for the app's process to end before it deletes the task's saved state: a write of
// it under way (SavedTask writes a .part and moves it in place) would otherwise put it back afterwards.
constexpr DWORD kForceStopWaitMs = 5000;

// Stop, Android's Force stop: on a device it ends the app's activities with its process
// (ActivityTaskManagerService.onForceStopPackage), so the task the runtime keeps for the app's next start
// (task_state_file) goes too; the next start is a fresh one. The file is deleted once the process has
// ended (or after kForceStopWaitMs).
void force_stop(const App& a) {
    if (!stop_run(a.name, kForceStopWaitMs)) appops::stop_runtime_of(a.apk, kForceStopWaitMs);
    std::error_code ec;
    fs::remove(task_state_file(a.name), ec);
}

// Deletes a folder and what is in it without going through a junction or link inside it (the
// shared-storage folders are junctions to the user's own folders): the link goes, not its target.
void remove_tree(const fs::path& dir) {
    const DWORD attrs = GetFileAttributesW(dir.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return;
    if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        SetFileAttributesW(dir.c_str(), FILE_ATTRIBUTE_NORMAL);
        DeleteFileW(dir.c_str());
        return;
    }
    if (!(attrs & FILE_ATTRIBUTE_REPARSE_POINT)) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir / L"*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (wcscmp(fd.cFileName, L".") != 0 && wcscmp(fd.cFileName, L"..") != 0) remove_tree(dir / fd.cFileName);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    RemoveDirectoryW(dir.c_str());
}

// The app's own files: its library entry, installed copy, converted code and shortcuts' icon. What it saved
// stays.
void remove_app_files(const std::wstring& name) {
    std::error_code ec;
    fs::remove(label_file(name), ec);
    fs::remove(package_file(name), ec);
    fs::remove(shortcut_icon(name), ec);
    fs::remove(library_dir() / (name + L".window"), ec);
    fs::remove(start_activity_file(name), ec);
    fs::remove(orientation_file(name), ec);
    remove_tree(app_dir(name));
    // Its installed copy (android\data\app\~~<nonce>\<package>-<nonce>), as a phone's uninstall removes it. There
    // is one per package, so it stays while another library name has the same package.
    const fs::path copy = installed_copy(name);
    fs::remove(install_record(name), ec);
    if (!copy.empty()) {
        const std::wstring dir = copy.filename().wstring();
        const std::string pkg = wide_to_utf8(dir.substr(0, dir.rfind(L'-')));
        bool other = false;
        for (const App& a : library()) {
            const std::vector<std::string> p = a.name == name ? std::vector<std::string>{} : package_of(a.name);
            other = other || (!p.empty() && p[0] == pkg);
        }
        if (!other) remove_tree(copy);
    }
}

// What the app saved, as Android's "Clear storage" and uninstall take it: its data folder
// (android\data\data\<package>, and the <package>_<version> folders older runs made) and its folders
// in shared storage (Android\data and Android\obb).
void remove_app_data(const std::string& pkg) {
    if (pkg.empty() || pkg.find_first_of("/\\:") != std::string::npos || pkg == "." || pkg == "..") return;
    const std::wstring p = utf8_to_wide(pkg);
    const fs::path android = data_dir() / L"android";
    remove_tree(android / L"data" / L"data" / p);
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(android / L"data" / L"data", ec)) {
        if (e.path().filename().wstring().rfind(p + L"_", 0) == 0) remove_tree(e.path());
    }
    remove_tree(android / L"sdcard" / L"Android" / L"data" / p);
    remove_tree(android / L"sdcard" / L"Android" / L"obb" / p);
}

// Asks before removing: Remove, with a box for what the app saved (off: it stays, so
// installing the app again picks up where it left off).
bool confirm_remove(const App& a, bool* with_data) {
    Prompt p;
    p.caption = L"Win.apk";
    p.question = fmt(L"Remove {0}?", {a.label});
    p.note = tr(L"Removes the app and its shortcuts.");
    p.switches = {{tr(L"Also remove accounts and data saved by the app"), false, true}};
    p.buttons = {{IDOK, tr(L"Remove")}, {IDCANCEL, tr(L"Cancel")}};
    p.default_id = IDCANCEL;
    if (run_dialog(p, owner()) != IDOK) return false;
    *with_data = p.switches[0].on;
    return true;
}

// Removes the app (asking first when `ask`); `with_data` also deletes what it saved. False when it could not be
// removed, and then nothing of it was: it is still running, or a file of it is in use.
bool remove_app(const App& app, bool ask, bool with_data = false) {
    if (ask && !confirm_remove(app, &with_data)) return false;
    const App copy = app;  // `app` may point into g_apps, which refresh() replaces
    if (app_running(copy.name)) {
        if (!appops::stop_runtime_of(copy.apk)) {
            info(fmt(L"Close {0} first, then remove it.", {copy.label}), kError);
            return false;
        }
        expect_run_end(copy.name);  // started from here: its exit is the removal, not a failure
    }
    // Its files in the library first, the one step that can fail (a file in use): the app is then left whole,
    // rather than without its name, permissions and shortcuts but still on disk.
    if (!winapk::launcher::take_out(library_dir(), copy.name)) {
        info(fmt(L"{0} could not be removed: a file of it is in use. Close it, then try again.", {copy.label}),
             kError);
        return false;
    }
    // Its permissions go with it, unless another copy in the library is the same package.
    const std::vector<std::string> pkg = package_of(copy.name);
    const fs::path registered = pkg.empty() ? fs::path() : registry_apk(pkg[0]);
    bool other = false;
    if (!pkg.empty()) {
        for (const App& a : library()) {
            const std::vector<std::string> p = a.name == copy.name ? std::vector<std::string>{} : package_of(a.name);
            other = other || (!p.empty() && p[0] == pkg[0]);
        }
        if (!other) appops_forget(pkg[0]);
        if (!other && with_data) remove_app_data(pkg[0]);
    }
    remove_app_shortcuts(copy);
    remove_app_files(copy.name);
    winapk::launcher::drop_taken_out(library_dir(), copy.name);  // what is left is deleted at the next start
    write_registry();
    // The package stays when another copy in the library is it; the registry then names that copy.
    if (!pkg.empty() && !other) tell_broker(winapk::package_events::kRemoved, pkg[0], registered, with_data);
    else if (!pkg.empty() && registered == copy.apk) tell_broker(winapk::package_events::kReplaced, pkg[0]);
    if (ask) {
        refresh();
        if (g_view == kAppView) set_view(kLibraryView);
        info(fmt(with_data ? L"{0} and what it saved were removed." : L"{0} was removed.", {copy.label}), kInfo);
    }
    return true;
}

// Opens this app in the given orientation from now on (kOrientUnset: the
// settings' again). The window size the user gave it was for the other
// shape, so it goes too, and the app opens at the settings' screen size.
void set_orientation(const App& app, int o) {
    std::error_code ec;
    set_app_orientation(app.name, o);
    fs::remove(library_dir() / (app.name + L".window"), ec);
    const int now = effective_orientation(app.name);
    info(fmt(now == kLandscape ? L"{0} opens in landscape from now on." : L"{0} opens in portrait from now on.",
                   {app.label}),
               kSuccess);
}

// ---------------------------------------------------------------------------
// Menus
//
// The library's own popup menu, drawn as Windows 11 draws a flyout menu: a rounded card with a
// shadow, a glyph per item, its shortcut at the end, a destructive item in red, and over the
// items, for an app's menu, the app's icon and name. The system menu ignores the theme and draws
// no pictures. It runs modally, as TrackPopupMenu does: the owner keeps the focus and so the
// keyboard, the menu holds the mouse capture, and a click elsewhere or another window closes it.

struct MenuItem {
    int id = 0;  // 0: a separator
    std::wstring text, shortcut;
    const wchar_t* glyph = nullptr;
    bool destructive = false, disabled = false, checked = false;
};

struct PopupMenu {
    std::vector<MenuItem> items;
    const App* app = nullptr;  // a header with its icon and name
    std::wstring title, sub;   // the header's lines
    std::vector<RectF> rects;  // where each item is, in the menu's client area
    float header_h = 0, w = 0, h = 0;
    int hot = -1, result = 0;
    bool done = false;
};
PopupMenu* g_menu = nullptr;

constexpr wchar_t kMenuClass[] = L"WinApkMenu";

bool menu_item_usable(const MenuItem& it) { return it.id != 0 && !it.disabled; }

void lay_out_menu(PopupMenu& m) {
    const float pad = px(4), item_h = px(34), sep_h = px(9);
    float text_w = 0, key_w = 0;
    for (const MenuItem& it : m.items) {
        if (!it.id) continue;
        text_w = (std::max)(text_w, measure(it.text, g_f.body.get()));
        if (!it.shortcut.empty()) key_w = (std::max)(key_w, measure(it.shortcut, g_f.caption.get()));
    }
    m.w = (std::max)(px(200), px(44) + text_w + (key_w > 0 ? px(28) + key_w : 0) + px(16)) + 2 * pad;
    if (!m.title.empty()) {
        const float tw = (std::max)(measure(m.title, g_f.body_semi.get()), measure(m.sub, g_f.caption.get()));
        m.w = (std::min)(px(340), (std::max)(m.w, px(64) + tw + px(16)));
        m.header_h = px(60);
    }
    float y = pad + m.header_h;
    m.rects.clear();
    for (const MenuItem& it : m.items) {
        const float ih = it.id ? item_h : sep_h;
        m.rects.emplace_back(pad, y, m.w - 2 * pad, ih);
        y += ih;
    }
    m.h = y + pad;
}

void paint_menu(Graphics& g, float W, float H) {
    const PopupMenu& m = *g_menu;
    g.Clear(g_t.dark ? Color(255, 44, 44, 44) : Color(255, 249, 249, 249));
    Gdiplus::Pen edge(g_t.border, 1);
    g.DrawRectangle(&edge, RectF(0, 0, W - 1, H - 1));
    if (m.header_h > 0) {
        const float s = px(36);
        const RectF ir(px(14), (m.header_h - s) / 2 + px(2), s, s);
        draw_app_icon(g, m.app, m.title, ir);
        const float tx = ir.GetRight() + px(12), tw = W - tx - px(12);
        draw_text(g, m.title, g_f.body_semi.get(), g_t.text, RectF(tx, ir.Y - px(1), tw, px(20)), kLeft, false, true);
        draw_text(g, m.sub, g_f.caption.get(), g_t.text2, RectF(tx, ir.Y + px(19), tw, px(18)), kLeft, false, true);
        Gdiplus::Pen line(g_t.divider, 1);
        g.DrawLine(&line, 0.0f, m.header_h, W, m.header_h);
    }
    for (size_t i = 0; i < m.items.size(); ++i) {
        const MenuItem& it = m.items[i];
        const RectF& r = m.rects[i];
        if (!it.id) {
            Gdiplus::Pen line(g_t.divider, 1);
            g.DrawLine(&line, r.X + px(8), r.Y + r.Height / 2, r.GetRight() - px(8), r.Y + r.Height / 2);
            continue;
        }
        const bool hot = static_cast<int>(i) == m.hot && !it.disabled;
        if (hot) fill_round(g, r, px(4), g_t.dark ? Color(255, 61, 61, 61) : Color(255, 234, 234, 234));
        const Color ink = it.disabled ? g_t.text3 : it.destructive ? g_t.danger : g_t.text;
        const wchar_t* glyph = it.checked ? kGlyphCheck : it.glyph;
        if (glyph) {
            draw_text(g, glyph, g_f.glyph16.get(), it.checked ? g_t.accent : ink,
                      RectF(r.X + px(10), r.Y, px(20), r.Height), kCenter);
        }
        draw_text(g, it.text, g_f.body.get(), ink, RectF(r.X + px(40), r.Y, r.Width - px(48), r.Height), kLeft,
                  false, true);
        if (!it.shortcut.empty()) {  // at the item's end, sized to it: keys read left to right in Hebrew too
            const float kw = text_width(g, it.shortcut, g_f.caption.get()) + px(2);
            draw_text(g, it.shortcut, g_f.caption.get(), it.disabled ? g_t.text3 : g_t.text2,
                      RectF(r.GetRight() - px(12) - kw, r.Y, kw, r.Height), kLeft, false, true, true);
        }
    }
}

int menu_item_at(const PopupMenu& m, float x, float y) {
    for (size_t i = 0; i < m.rects.size(); ++i) {
        if (contains(m.rects[i], x, y)) return static_cast<int>(i);
    }
    return -1;
}

LRESULT CALLBACK menu_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    PopupMenu* m = g_menu;
    switch (msg) {
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_PAINT:
            if (m) buffered_paint(hwnd, paint_menu);
            else ValidateRect(hwnd, nullptr);
            return 0;
        case WM_ERASEBKGND: return 1;
        case WM_MOUSEMOVE:
            if (m) {
                const int hot = menu_item_at(*m, static_cast<float>(GET_X_LPARAM(lp)), static_cast<float>(GET_Y_LPARAM(lp)));
                if (hot != m->hot) {
                    m->hot = hot;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            return 0;
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN: {
            RECT rc;
            GetClientRect(hwnd, &rc);
            const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (m && !PtInRect(&rc, pt)) m->done = true;  // a click outside closes the menu, as a menu's does
            return 0;
        }
        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
            if (m) {
                const int i = menu_item_at(*m, static_cast<float>(GET_X_LPARAM(lp)), static_cast<float>(GET_Y_LPARAM(lp)));
                if (i >= 0 && menu_item_usable(m->items[static_cast<size_t>(i)])) {
                    m->result = m->items[static_cast<size_t>(i)].id;
                    m->done = true;
                }
            }
            return 0;
        case WM_CAPTURECHANGED:
            if (m && reinterpret_cast<HWND>(lp) != hwnd) m->done = true;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void menu_move(PopupMenu& m, int step) {
    const int n = static_cast<int>(m.items.size());
    int i = m.hot;
    for (int k = 0; k < n; ++k) {
        i = i < 0 ? (step > 0 ? 0 : n - 1) : (i + step + n) % n;
        if (menu_item_usable(m.items[static_cast<size_t>(i)])) {
            m.hot = i;
            return;
        }
    }
}

// Shows `m` at the screen point `pt` (its top start corner there) and returns the item picked, or 0.
// `below`: the top of the control it opens under, for a menu that does not fit below it.
int run_menu(PopupMenu& m, POINT pt, LONG below = LONG_MIN, bool keyboard = false) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.style = CS_DROPSHADOW;
        wc.lpfnWndProc = menu_proc;
        wc.hInstance = g_inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kMenuClass;
        registered = RegisterClassW(&wc) != 0;
    }
    lay_out_menu(m);
    const int w = static_cast<int>(std::ceil(m.w)), h = static_cast<int>(std::ceil(m.h));
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
    const RECT& wa = mi.rcWork;
    int x = g_rtl ? pt.x - w : pt.x, y = pt.y;
    if (x + w > wa.right) x = (std::max)(wa.left, static_cast<LONG>(g_rtl ? wa.right - w : pt.x - w));
    if (x < wa.left) x = wa.left;
    if (y + h > wa.bottom) y = (std::max)(wa.top, (below != LONG_MIN ? below : pt.y) - h);
    if (keyboard) menu_move(m, 1);
    g_menu = &m;
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | layout_style(), kMenuClass,
                                L"", WS_POPUP, x, y, w, h, g_main, nullptr, g_inst, nullptr);
    if (!hwnd) {
        g_menu = nullptr;
        return 0;
    }
    const DWORD round = 2;  // DWMWCP_ROUND: Windows 11 rounds the corners; older Windows ignores it
    DwmSetWindowAttribute(hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &round, sizeof round);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    SetCapture(hwnd);
    MSG msg;
    bool quit = false;
    while (!m.done) {
        if (!GetMessageW(&msg, nullptr, 0, 0)) {
            quit = true;
            break;
        }
        if (msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN) {
            switch (msg.wParam) {
                case VK_ESCAPE:
                case VK_MENU:
                case VK_F10: m.done = true; break;
                case VK_UP: menu_move(m, -1); break;
                case VK_DOWN: menu_move(m, 1); break;
                case VK_HOME: m.hot = -1; menu_move(m, 1); break;
                case VK_END: m.hot = -1; menu_move(m, -1); break;
                case VK_RETURN:
                case VK_SPACE:
                    if (m.hot >= 0 && menu_item_usable(m.items[static_cast<size_t>(m.hot)])) {
                        m.result = m.items[static_cast<size_t>(m.hot)].id;
                        m.done = true;
                    }
                    break;
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            continue;
        }
        if (msg.message == WM_CHAR || msg.message == WM_SYSCHAR || msg.message == WM_KEYUP ||
            msg.message == WM_SYSKEYUP) {
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        if (GetForegroundWindow() != g_main) m.done = true;  // Alt+Tab, another program in front
    }
    if (GetCapture() == hwnd) ReleaseCapture();
    DestroyWindow(hwnd);
    g_menu = nullptr;
    if (quit) PostQuitMessage(static_cast<int>(msg.wParam));
    return m.result;
}

// A choice list under a control: the choices, the current one checked.
int run_choice_menu(const std::vector<std::wstring>& choices, int current, const RectF& under) {
    PopupMenu m;
    for (size_t i = 0; i < choices.size(); ++i) {
        MenuItem it;
        it.id = static_cast<int>(i) + 1;
        it.text = choices[i];
        it.checked = static_cast<int>(i) == current;
        m.items.push_back(it);
    }
    POINT pt{static_cast<LONG>(under.X), static_cast<LONG>(under.GetBottom() + px(4))};
    POINT top{static_cast<LONG>(under.X), static_cast<LONG>(under.Y - px(4))};
    ClientToScreen(g_main, &pt);
    ClientToScreen(g_main, &top);
    return run_menu(m, pt, top.y);
}

// ---------------------------------------------------------------------------
// Pages: Settings, an app's details and a store app's details, drawn in the
// content layer in place of the grid, as rows grouped in cards (Windows 11's
// Settings, upstream's grouped forms).

struct Row {
    enum Kind { kText, kButton, kToggle, kChoice } kind = kText;
    std::wstring title, detail, value;  // value: the button's text, or the text on the right
    bool on = false, destructive = false, primary = false, check = false;
    // A control whose action is under way (or cannot run now): drawn greyed, and neither clicked nor focused, so
    // a second click cannot start it twice.
    bool disabled = false;
    std::vector<std::wstring> choices;
    int choice = 0;
    std::function<void(int)> act;  // button: act(0), toggle: act(new state), choice: act(index)
    const char* key = nullptr;     // names a row that an event changes in place (update_progress)
    std::wstring link;             // a text link under the detail: a way on that is not the row's action
    std::function<void()> link_act;
};

struct Section {
    std::wstring title;
    std::vector<Row> rows;
    std::wstring footer;
};

struct Page {
    std::wstring title, sub;                   // the heading: Settings, or the app's name and what it is
    std::wstring app;                          // the library app whose icon heads it
    std::function<Gdiplus::Bitmap*()> icon;    // or a store app's icon
    bool back = false;                         // a details page: the arrow goes back to `from`
    View from = kLibraryView;
    std::function<std::vector<Section>()> build;
    std::vector<Section> sections;
    float scroll = 0, content_h = 0;
    struct Target {
        RectF r;
        int section, row;
        bool link = false;  // the row's link, not its control
    };
    std::vector<Target> targets;
    RectF back_r, thumb, view;
    int hot = -1;    // index into targets, kPageBack, kPageThumb
    int focus = -1;  // the target reached with Tab: it shows a focus ring
    bool thumb_drag = false;
    float thumb_y0 = 0, thumb_scroll0 = 0;
    int generation = 0;  // a new page was opened: an action's rebuild is not for it
    int reveal = -1;     // a section to scroll to the top on the next paint
} g_page;

constexpr int kPageBack = -2, kPageThumb = -3;

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
        case Row::kButton:
            if (row.disabled) {
                stroke_round(g, r, px(kRadiusControl), g_t.border);
                draw_text(g, row.value, g_f.body.get(), g_t.text3, r, kCenter);
                break;
            }
            draw_button(g, r, row.value, hot, row.primary, row.destructive);
            break;
        case Row::kToggle: draw_switch(g, row.on, r, hot); break;
        case Row::kChoice: {
            fill_round(g, r, px(kRadiusControl), hot ? g_t.hover : g_t.card);
            stroke_round(g, r, px(kRadiusControl), g_t.border);
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

float page_max_scroll() { return (std::max)(0.0f, g_page.content_h - g_page.view.Height); }

void paint_page(Graphics& g) {
    Page& s = g_page;
    s.targets.clear();
    const RectF& c = g_l.content;
    const float m = c.X + px(28), pad = px(16);
    const float cw = (std::min)(px(860), c.Width - px(56));
    float x = m;
    if (s.back) {
        s.back_r = RectF(m - px(6), px(22), px(36), px(36));
        draw_icon_button(g, s.back_r, g_rtl ? kGlyphForward : kGlyphBack, s.hot == kPageBack, g_t.text);
        if (s.focus == kPageBack) draw_focus_ring(g, s.back_r, px(kRadiusControl));
        x += px(44);
    } else {
        s.back_r = RectF();
    }
    float header = px(64);
    if (!s.app.empty() || s.icon) {
        const RectF ir(x, px(18), px(48), px(48));
        if (s.icon) draw_icon(g, s.icon(), s.title, ir);
        else draw_app_icon(g, find_app(s.app), s.title, ir);
        draw_text(g, s.title, g_f.title.get(), g_t.text, RectF(ir.GetRight() + px(14), px(18), c.GetRight() - ir.GetRight() - px(40), px(28)));
        draw_text(g, s.sub, g_f.caption.get(), g_t.text2, RectF(ir.GetRight() + px(14), px(46), c.GetRight() - ir.GetRight() - px(40), px(20)));
        header = px(82);
    } else {
        draw_text(g, s.title, g_f.title.get(), g_t.text, RectF(x, px(24), cw, px(28)));
    }
    s.view = RectF(c.X, header, c.Width, (std::max)(0.0f, c.GetBottom() - header));
    s.scroll = (std::min)(s.scroll, page_max_scroll());
    g.SetClip(s.view);
    float y = s.view.Y + px(8) - s.scroll;
    float reveal_at = -1;
    for (size_t si = 0; si < s.sections.size(); ++si) {
        const Section& sec = s.sections[si];
        if (static_cast<int>(si) == s.reveal) reveal_at = y + s.scroll - s.view.Y - px(8);
        if (!sec.title.empty()) {
            draw_text(g, sec.title, g_f.body_semi.get(), g_t.text, RectF(m, y, cw, px(22)));
            y += px(28);
        }
        std::vector<float> heights;
        std::vector<SizeF> controls;
        float total = 0;
        for (const Row& row : sec.rows) {
            const SizeF cs = control_size(g, row);
            const float tw = cw - 2 * pad - (cs.Width > 0 ? cs.Width + px(16) : 0);
            float h = px(20);
            if (!row.detail.empty()) h += px(2) + text_height(g, row.detail, g_f.caption.get(), tw);
            if (!row.link.empty()) h += px(24);
            heights.push_back((std::max)(px(56), h + px(24)));
            controls.push_back(cs);
            total += heights.back();
        }
        const RectF card(m, y, cw, total);
        fill_round(g, card, px(kRadiusCard), g_t.card);
        float ry = y;
        for (size_t ri = 0; ri < sec.rows.size(); ++ri) {
            const Row& row = sec.rows[ri];
            const SizeF cs = controls[ri];
            const float h = heights[ri];
            const RectF rr(m, ry, cw, h);
            // A switch takes the whole row (its label turns it too); a button or a list only itself.
            const bool interactive = row.kind != Row::kText && !row.disabled;
            const int index = static_cast<int>(s.targets.size());
            const bool hot = interactive && s.hot == index, focused = interactive && s.focus == index;
            if (row.kind == Row::kToggle && hot) {
                GraphicsPath clip;
                rounded(clip, card, px(kRadiusCard));
                g.SetClip(&clip, Gdiplus::CombineModeIntersect);
                Gdiplus::SolidBrush b(g_t.hover);
                g.FillRectangle(&b, rr);
                g.SetClip(s.view);
            }
            if (ri > 0) {
                Gdiplus::Pen line(g_t.divider, 1);
                g.DrawLine(&line, m + pad, ry, m + cw - pad, ry);
            }
            const float tw = cw - 2 * pad - (cs.Width > 0 ? cs.Width + px(16) : 0);
            const float dh = row.detail.empty() ? 0 : px(2) + text_height(g, row.detail, g_f.caption.get(), tw);
            const float lh = row.link.empty() ? 0 : px(24);
            const float ty = ry + (h - px(20) - dh - lh) / 2;
            draw_text(g, row.title, g_f.body.get(), row.destructive && row.kind == Row::kText ? g_t.danger : g_t.text,
                      RectF(m + pad, ty, tw, px(20)));
            if (!row.detail.empty()) {
                draw_text(g, row.detail, g_f.caption.get(), g_t.text2, RectF(m + pad, ty + px(22), tw, dh + px(2)),
                          kLeft, true, false);
            }
            if (cs.Width > 0) {
                const RectF cr(m + cw - pad - cs.Width, ry + (h - cs.Height) / 2, cs.Width, cs.Height);
                paint_control(g, row, cr, hot);
                if (focused) draw_focus_ring(g, cr, row.kind == Row::kToggle ? cr.Height / 2 : px(kRadiusControl));
                if (interactive) s.targets.push_back({row.kind == Row::kToggle ? rr : cr, static_cast<int>(si),
                                                      static_cast<int>(ri)});
            }
            if (!row.link.empty()) {
                // Accent text under the detail, as the install window's links (draw_link).
                const int li = static_cast<int>(s.targets.size());
                const RectF lr(m + pad, ty + px(22) + dh + px(2),
                               (std::min)(tw, text_width(g, row.link, g_f.body.get()) + px(4)), px(22));
                draw_text(g, row.link, g_f.body.get(), s.hot == li ? mix(g_t.accent, g_t.text, 0.3f) : g_t.accent, lr);
                if (s.focus == li) draw_focus_ring(g, inset(lr, -px(3), -px(1)), px(kRadiusControl));
                s.targets.push_back({lr, static_cast<int>(si), static_cast<int>(ri), true});
            }
            ry += h;
        }
        stroke_round(g, card, px(kRadiusCard), g_t.border);
        y = ry + px(8);
        if (!sec.footer.empty()) {
            const float fh = text_height(g, sec.footer, g_f.caption.get(), cw - px(8));
            draw_text(g, sec.footer, g_f.caption.get(), g_t.text2, RectF(m + px(4), y, cw - px(8), fh + px(2)), kLeft,
                      true, false);
            y += fh + px(4);
        }
        y += px(20);
    }
    s.content_h = y + s.scroll - s.view.Y + px(56);  // room for the InfoBar at the end
    g.ResetClip();
    if (s.reveal >= 0) {  // known only now that the page is laid out: painted again there
        s.reveal = -1;
        s.scroll = (std::max)(0.0f, (std::min)(reveal_at, page_max_scroll()));
        InvalidateRect(g_main, nullptr, FALSE);
    }
    if (s.content_h > s.view.Height) {
        const float h = (std::max)(px(36), s.view.Height * s.view.Height / s.content_h);
        const float ty = s.view.Y + px(4) + (s.view.Height - px(8) - h) * s.scroll / page_max_scroll();
        s.thumb = RectF(c.GetRight() - px(10), ty, px(6), h);
        fill_round(g, s.thumb, px(3), with_alpha(g_t.text2, s.thumb_drag || s.hot == kPageThumb ? 200 : 90));
        if (s.scroll > 0) {
            Gdiplus::Pen line(g_t.border, 1);
            g.DrawLine(&line, c.X + px(16), s.view.Y, c.GetRight() - px(16), s.view.Y);
        }
    } else {
        s.thumb = RectF();
    }
}

int page_hit(float x, float y) {
    const Page& s = g_page;
    if (s.back && contains(s.back_r, x, y)) return kPageBack;
    if (!contains(s.view, x, y)) return -1;
    if (s.thumb.Width > 0 && contains(inset(s.thumb, -px(4), 0), x, y)) return kPageThumb;
    for (size_t i = 0; i < s.targets.size(); ++i) {
        if (contains(s.targets[i].r, x, y)) return static_cast<int>(i);
    }
    return -1;
}

void page_scroll(float by) {
    g_page.scroll = (std::max)(0.0f, (std::min)(g_page.scroll + by, page_max_scroll()));
    InvalidateRect(g_main, nullptr, FALSE);
}

// Builds the page again from what is true now (after an action, or when the language changed).
void rebuild_page() {
    if (!page_view() || !g_page.build) return;
    g_page.sections = g_page.build();
    InvalidateRect(g_main, nullptr, FALSE);
}

// Looks again at which of the library's apps run (app_is_running): when a run of the app started from here begins or
// ends, and every kRunningTimer while the library is shown, since an app started from its shortcut runs in a process
// of its own whose start and end the launcher is not told of. What changed is drawn again, and an open App details
// page of an app that started or stopped is built again, so that its Stop row comes and goes with the app.
void refresh_running() {
    std::set<std::wstring> now;
    for (const App& a : g_apps) {
        if (app_is_running(a.name)) now.insert(a.name);
    }
    if (now == g_running) return;
    const bool page_changed = g_view == kAppView && now.count(g_page.app) != g_running.count(g_page.app);
    g_running = std::move(now);
    if (page_changed) rebuild_page();
    if (g_main) InvalidateRect(g_main, nullptr, FALSE);
}

void page_back() {
    if (g_page.back) set_view(g_page.from);
}

// Activates a target: the back arrow, a button row, a switch, or a list (its menu opens under the control).
void page_click(int hit) {
    Page& s = g_page;
    if (hit == kPageBack) {
        page_back();
        return;
    }
    if (hit < 0 || hit >= static_cast<int>(s.targets.size())) return;
    const Page::Target t = s.targets[static_cast<size_t>(hit)];
    const Row row = s.sections[static_cast<size_t>(t.section)].rows[static_cast<size_t>(t.row)];  // act may rebuild
    if (t.link) {
        if (row.link_act) row.link_act();
        return;
    }
    if (!row.act) return;
    const int generation = s.generation;
    if (row.kind == Row::kButton) {
        row.act(0);
    } else if (row.kind == Row::kToggle) {
        row.act(!row.on);
    } else if (row.kind == Row::kChoice) {
        const int pick = run_choice_menu(row.choices, row.choice, t.r);
        if (pick > 0) row.act(pick - 1);
    }
    if (g_page.generation == generation) rebuild_page();
}

// Tab moves the keyboard focus through the back arrow and the controls, and brings the one reached into view.
void page_tab(bool back) {
    Page& s = g_page;
    std::vector<int> order;
    if (s.back) order.push_back(kPageBack);
    for (size_t i = 0; i < s.targets.size(); ++i) order.push_back(static_cast<int>(i));
    if (order.empty()) return;
    const auto it = std::find(order.begin(), order.end(), s.focus);
    int i = it == order.end() ? (back ? static_cast<int>(order.size()) : -1) : static_cast<int>(it - order.begin());
    i = back ? i - 1 : i + 1;
    i = (i % static_cast<int>(order.size()) + static_cast<int>(order.size())) % static_cast<int>(order.size());
    s.focus = order[static_cast<size_t>(i)];
    if (s.focus >= 0) {
        const RectF& r = s.targets[static_cast<size_t>(s.focus)].r;
        if (r.Y < s.view.Y + px(8)) page_scroll(r.Y - s.view.Y - px(8));
        else if (r.GetBottom() > s.view.GetBottom() - px(64)) page_scroll(r.GetBottom() - (s.view.GetBottom() - px(64)));
    }
    InvalidateRect(g_main, nullptr, FALSE);
}

// Opens a page in the content layer.
void open_page(View view, Page page) {
    page.generation = g_page.generation + 1;
    g_page = std::move(page);
    g_page.sections = g_page.build();
    set_view(view);
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

// ---- The Google account (upstream's Accounts panel) --------------------------
//
// Apps that sign in with Google, check a Play licence or register for push
// use the account the GMS layer (runtime/java/com/macapk/gms) holds. The
// layer's state is the runtime's (<data>\gms, MACAPK_GMS_DIR): the launcher
// never reads the credentials file, it asks `winapk-runtime --gms` as
// Mac.apk's launcher asks `macapk-runtime --gms`. The exit codes are
// GmsCli.run's contract: 0 done, 1 an ERROR line, 2 cancelled. The one cheap
// answer is the `accounts.present` marker GmsStore keeps next to its file:
// present iff an account is signed in, which decides whether opening Settings
// pays a JVM start for the account list.

fs::path gms_dir() { return data_dir() / L"gms"; }
fs::path gms_log() { return logs_dir() / L"gms.log"; }

bool google_account_present() {
    std::error_code ec;
    return fs::exists(gms_dir() / L"accounts.present", ec);
}

// A failure's reason is an `ERROR\t<message>` line on the runtime's stdout,
// which only the caller reads; the error dialog's Show log opens gms.log,
// which had only stderr, so "PKIX path building failed" was nowhere to be
// seen. Copied there, and only those lines: the rest of stdout holds the
// account's email. Called after the runtime exited, as its stderr handle
// shares no write.
void append_gms_errors(const std::string& out) {
    std::string lines;
    for (size_t at = 0; at < out.size();) {
        size_t end = out.find('\n', at);
        if (end == std::string::npos) end = out.size();
        if (out.compare(at, 6, "ERROR\t") == 0) lines.append(out, at, end - at).append("\n");
        at = end + 1;
    }
    if (lines.empty()) return;
    HANDLE f = CreateFileW(gms_log().c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD n = 0;
    WriteFile(f, lines.data(), static_cast<DWORD>(lines.size()), &n, nullptr);
    CloseHandle(f);
}

// Runs winapk-runtime <args>, its stdout into *out (UTF-8, as the runtime's
// manifest makes the code page) and its stderr into `log`. The exit code, or
// -1 when it did not start; *raw (if asked) gets stdout's bytes.
int run_runtime_captured(const std::wstring& args, std::wstring* out, const fs::path& log, std::string* raw = nullptr) {
    std::error_code ec;
    fs::create_directories(logs_dir(), ec);
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE read = nullptr, write = nullptr;
    if (!CreatePipe(&read, &write, &sa, 0)) return -1;
    SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
    HANDLE err = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, &sa, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = write;
    si.hStdError = err != INVALID_HANDLE_VALUE ? err : write;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    std::wstring cmd = L"\"" + runtime_exe().wstring() + L"\" " + args;
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                   root().c_str(), &si, &pi);
    if (!ok && err != INVALID_HANDLE_VALUE) {
        // The log would stay empty: the one line that says why.
        const std::string line = launcher_error_line(L"could not start " + runtime_exe().wstring(), GetLastError());
        DWORD n = 0;
        WriteFile(err, line.data(), static_cast<DWORD>(line.size()), &n, nullptr);
    }
    CloseHandle(write);
    if (err != INVALID_HANDLE_VALUE) CloseHandle(err);
    std::string bytes;
    if (ok) {
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(read, buf, sizeof buf, &n, nullptr) && n > 0) bytes.append(buf, n);
    }
    CloseHandle(read);
    if (!ok) return -1;
    if (out) *out = utf8_to_wide(bytes);
    if (raw) *raw = bytes;
    CloseHandle(pi.hThread);
    return static_cast<int>(wait_exit(pi.hProcess));
}

// Runs winapk-runtime --gms <args>: stderr into logs\gms.log, with the ERROR lines of stdout.
int run_gms(const std::wstring& args, std::wstring* out) {
    std::string bytes;
    const int code = run_runtime_captured(L"--gms " + args, out, gms_log(), &bytes);
    append_gms_errors(bytes);
    return code;
}

// One argument for the runtime's command line, as CommandLineToArgvW splits it.
std::wstring gms_arg(const std::wstring& a) {
    std::wstring q = L"\"";
    size_t slashes = 0;
    for (wchar_t c : a) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        q.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        q += c;
    }
    q.append(slashes * 2, L'\\');
    return q + L"\"";
}

// ---- a file the user opened, read before its install ----
//
// Its package and versionCode (winapk-runtime --verify-apk) decide which app it is (installs.h, place_install): the
// app of its package whatever the file is called, never another package's app that has the same name, and an older
// version only once the user agrees. One file at a time, in the order opened, so that the installs queue in that
// order and no two JVMs start at once.

struct InstallProbe {
    fs::path file;
    std::vector<SplitFile> splits;  // a batch's other APKs of the same app (install_sets_thread)
    winapk::launcher::FileIdentity id;
};

std::deque<std::unique_ptr<InstallProbe>> g_probe_queue;
bool g_probe_running = false;
// The package of each install queued from a probe, until it ends: a second file of the same name and another
// package is then another app, and the same file twice is one install.
std::map<std::wstring, std::wstring> g_queued_packages;

bool probes_pending() { return g_probe_running || !g_probe_queue.empty(); }

DWORD WINAPI install_probe_thread(void* param) {
    auto* p = static_cast<InstallProbe*>(param);
    std::wstring out;
    // Its exit code says whether the signature verifies; the package and version are read either way, and an
    // answer with no package installs the file as before (its install then says what is wrong with it).
    run_runtime_captured(L"--verify-apk " + gms_arg(p->file.wstring()), &out, logs_dir() / L"install-check.log");
    p->id = winapk::launcher::parse_file_identity(out);
    if (!PostMessageW(g_main, kInstallProbed, 0, reinterpret_cast<LPARAM>(p))) delete p;
    return 0;
}

void probe_next() {
    if (g_probe_running || g_probe_queue.empty()) return;
    InstallProbe* p = g_probe_queue.front().release();
    g_probe_queue.pop_front();
    if (HANDLE t = CreateThread(nullptr, 0, install_probe_thread, p, 0, nullptr)) {
        CloseHandle(t);
        g_probe_running = true;
        info(fmt(L"Reading {0}…", {p->file.filename().wstring()}), kInfo);
    } else {
        install(p->file, L"", false, std::move(p->splits));  // unread: as before
        delete p;
        probe_next();
    }
}

void probe_install(const fs::path& file, std::vector<SplitFile> splits) {
    auto p = std::make_unique<InstallProbe>();
    p->file = file;
    p->splits = std::move(splits);
    g_probe_queue.push_back(std::move(p));
    probe_next();
}

std::wstring version_text(const std::wstring& name, long long code) {
    return !name.empty() ? name : code >= 0 ? std::to_wstring(code) : std::wstring(tr(L"unknown"));
}

// Android's installer refuses an older version of an installed app (INSTALL_FAILED_VERSION_DOWNGRADE); here the
// user may still choose it, warned of what that does to the app's data. Cancel is the answer on Enter.
bool confirm_downgrade(const App& app, const winapk::launcher::FileIdentity& file) {
    const std::vector<std::string> lines = package_of(app.name);
    const std::wstring installed = version_text(lines.size() > 2 ? utf8_to_wide(lines[2]) : L"",
                                                lines.size() > 1 ? _wtoi64(utf8_to_wide(lines[1]).c_str()) : -1);
    Prompt p;
    p.caption = L"Win.apk";
    p.question = fmt(L"Replace {0} with an older version?", {app.label});
    p.note = fmt(L"The installed version is {0}, and this file is version {1}. What the app saved with the newer "
                 L"version may not work with the older one.",
                 {installed, version_text(file.version_name, file.version)});
    p.buttons = {{IDOK, tr(L"Replace")}, {IDCANCEL, tr(L"Cancel")}};
    p.default_id = IDCANCEL;
    return run_dialog(p, owner()) == IDOK;
}

void install_probed(InstallProbe& p) {
    using winapk::launcher::Placement;
    std::vector<winapk::launcher::LibraryApp> library;
    for (const App& a : g_apps) {
        const std::vector<std::string> lines = package_of(a.name);
        winapk::launcher::LibraryApp l;
        l.name = a.name;
        if (!lines.empty()) l.package = utf8_to_wide(lines[0]);
        if (lines.size() > 1 && !lines[1].empty()) l.version = _wtoi64(utf8_to_wide(lines[1]).c_str());
        library.push_back(l);
    }
    for (const auto& [name, pkg] : g_queued_packages) {
        if (!find_app(name)) library.push_back({name, pkg, -1});
    }
    const winapk::launcher::PlaceResult r = winapk::launcher::place_install(install_name(p.file), p.id, library,
                                                                            pending_names());
    if (r.how == Placement::kDowngrade) {
        const App* app = find_app(r.name);
        if (app && !confirm_downgrade(*app, p.id)) {
            info(fmt(L"{0} was not replaced: the installed version is newer.", {app->label}), kInfo);
            return;
        }
    }
    if (!p.id.package.empty()) g_queued_packages[r.name] = p.id.package;
    install(p.file, r.name, false, std::move(p.splits));
}

// The signed-in accounts: `ACCOUNT<tab>email<tab>services` lines from
// `--gms accounts`. Only asked when the marker says there is one.
std::vector<std::wstring> google_accounts() {
    std::vector<std::wstring> out;
    if (!google_account_present()) return out;
    std::wstring text;
    if (run_gms(L"accounts", &text) != 0) return out;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find(L'\n', at);
        if (end == std::wstring::npos) end = text.size();
        std::wstring line = text.substr(at, end - at);
        at = end + 1;
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (line.rfind(L"ACCOUNT\t", 0) != 0) continue;
        const size_t tab = line.find(L'\t', 8);
        out.push_back(line.substr(8, tab == std::wstring::npos ? std::wstring::npos : tab - 8));
    }
    return out;
}

// The toolbar's account: none without the marker; else the first account, asked on a thread
// (a JVM start) and posted to the library's window.
DWORD WINAPI account_thread(void*) {
    const std::vector<std::wstring> accounts = google_accounts();
    auto* out = new std::wstring(accounts.empty() ? L"" : accounts[0]);
    if (!PostMessageW(g_main, kGmsAccount, 0, reinterpret_cast<LPARAM>(out))) delete out;
    return 0;
}

void refresh_account() {
    if (!google_account_present()) {
        g_account.clear();
        g_account_photo_key.clear();
        if (g_main) InvalidateRect(g_main, nullptr, FALSE);
        return;
    }
    HANDLE t = CreateThread(nullptr, 0, account_thread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
}

// The sign-in window is the runtime's (--gms signin-window: Google's own
// page in a WebView2 window; the launcher never sees the password). It takes
// as long as the user takes, so it runs on a thread and reports to the
// library's window, which rebuilds the Settings sheet if it is still open.
bool g_gms_busy = false;

DWORD WINAPI gms_signin_thread(void*) {
    auto* out = new std::wstring();
    const int code = run_gms(L"signin-window", out);
    PostMessageW(g_main, kGmsDone, static_cast<WPARAM>(static_cast<unsigned>(code)), reinterpret_cast<LPARAM>(out));
    return 0;
}

void start_google_signin() {
    if (g_gms_busy) return;
    g_gms_busy = true;
    HANDLE t = CreateThread(nullptr, 0, gms_signin_thread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
    else g_gms_busy = false;
}

void google_signout(const std::wstring& account) {
    if (!confirm(fmt(L"Sign out of {0}?", {account}),
                 tr(L"The account and its tokens are removed from this PC. Apps that used it will ask you to "
                    L"sign in again."),
                 tr(L"Sign out"))) {
        return;
    }
    std::wstring text;
    if (run_gms(L"signout " + gms_arg(account), &text) == 0) {
        info(fmt(L"Signed out of {0}.", {account}), kInfo);
        refresh_account();
    } else {
        // Signing out only removes what this PC keeps (GmsCli signout): no network is asked, so the cause is here.
        error_with_log(tr(L"Could not sign out"),
                       tr(L"Win.apk could not remove the account from this PC. The log says why."), gms_log());
    }
}

// An older install kept the account's master token in plain text in gms_state.properties. The runtime's
// owner side moves it into the sealed tokens.dat the first time it reads the file (com.macapk.gms.GmsStore);
// an app's process has the token service do that first, which costs it a JVM start. So at its start the
// launcher has `--gms accounts` read the file once, on a thread, when there is a file and no tokens.dat yet
// (two stats: the launcher never reads either file). Nothing is shown.
DWORD WINAPI seal_gms_tokens_thread(void*) {
    std::wstring out;
    run_gms(L"accounts", &out);
    return 0;
}

void seal_gms_tokens() {
    std::error_code ec;
    if (!fs::exists(gms_dir() / L"gms_state.properties", ec) || fs::exists(gms_dir() / L"tokens.dat", ec)) return;
    if (HANDLE t = CreateThread(nullptr, 0, seal_gms_tokens_thread, nullptr, 0, nullptr)) CloseHandle(t);
}

bool google_apps_ready();
bool background_updates_on();
void maintenance_check();
void show_gms_setup(bool from_settings);

Section google_account_section() {
    Section sec{tr(L"Google account"), {}, L""};
    if (g_gms_busy) {
        sec.rows.push_back(text_row(tr(L"Google account"), tr(L"Signing in…"),
                                    tr(L"Complete the sign-in in the Google window, or close it to cancel.")));
    } else {
        const std::vector<std::wstring> accounts = google_accounts();
        g_account = accounts.empty() ? L"" : accounts[0];
        for (const std::wstring& a : accounts) {
            sec.rows.push_back(button_row(a, L"", tr(L"Sign out"), [a] { google_signout(a); }, true));
        }
        sec.rows.push_back(button_row(accounts.empty() ? tr(L"Sign in with Google") : tr(L"Add another account"),
                                      tr(L"One account for every app: Google sign-in, Play license checks and "
                                         L"push notifications."),
                                      tr(L"Sign in…"), [] { start_google_signin(); }));
    }
    // The real Google Play services, Services Framework and Play Store, installed by the user, running as
    // apps through the system broker (winapk-runtime --system). The runtime reads the value at an app's start.
    Row gms;
    gms.kind = Row::kToggle;
    gms.title = tr(L"Use Google services (experimental)");
    gms.detail = tr(L"Runs the Google Play services, Services Framework and Play Store you install yourself, "
                    L"as apps other apps can reach. Only copies signed by Google are trusted. Takes effect "
                    L"when an app starts.");
    gms.on = google_services_on();
    gms.act = [](int on) {
        set_setting(L"GoogleServices", static_cast<DWORD>(on));
        rebuild_tiles();  // Google Play in the pane comes and goes with it
        maintenance_check();
        if (on && !google_apps_ready()) show_gms_setup(true);
    };
    sec.rows.push_back(gms);
    // The launcher's maintenance windows while it runs (background_updates_on, maintenance_check).
    if (gms.on) {
        Row bg;
        bg.kind = Row::kToggle;
        bg.title = tr(L"Update apps in the background");
        bg.detail = tr(L"While Win.apk is open, even with only an app showing, Google services start for a few "
                       L"minutes every few hours so the Play Store can update your apps, as a phone does. Only on "
                       L"mains power. Closing Win.apk stops them.");
        bg.on = setting(L"BackgroundUpdates", 1) != 0;
        bg.act = [](int on) {
            set_setting(L"BackgroundUpdates", static_cast<DWORD>(on));
            maintenance_check();
        };
        sec.rows.push_back(bg);
    }
    // The setup's one home is its page; this row only leads there.
    if (gms.on) {
        sec.rows.push_back(button_row(tr(L"Google services setup"),
                                      google_apps_ready() ? tr(L"Play services, Services Framework and Play Store "
                                                               L"are installed.")
                                                          : tr(L"Install Play services, Services Framework and "
                                                               L"Play Store from their downloads."),
                                      tr(L"Open"), [] { show_gms_setup(true); }));
    }
    sec.footer = tr(L"Sign-in takes place on Google's own page, in a separate window; Win.apk never sees your "
                    L"password. The account's tokens are stored only on this PC.");
    return sec;
}


// ---- Google services setup -----------------------------------------------------
//
// Google's own Play services, Services Framework and Play Store, which "Use Google services" runs, are not
// shipped with Win.apk (copies published here could be taken down). The page has a row per app with
// Download, which runs apkeep (bin\tools\apkeep.exe, windows/scripts/fetch_apkeep.sh) to fetch the version
// google_apps.h pins, x86_64 variant, from APKPure into downloads\google\. The pins set only the first install:
// the Play Store then updates itself and Play services, as on a phone. APKPure has served altered apps, so what
// it gives is trusted no more than any other file: every file, fetched or not, is checked by `winapk-runtime
// --verify-apk` (com.winapk.tools.ApkCheck: its package, and whether its signature verifies and Google
// signed it, as the runtime checks it before it treats the app as Google's), and only one of the three apps
// that Google signed is installed, through the same install as a dropped APK. When apkeep is missing, or its
// copy fails or does not pass, the row's button opens the app's APKMirror page in the browser instead
// (APKMirror forbids automated downloads: its own button downloads it there); while the page is open the
// launcher watches the Downloads folder for that (and takes a file dropped on the page).

struct GoogleApp {
    const char* pkg;
    const wchar_t* name;
    const wchar_t* url;   // APKMirror's page for the app: the user picks the release and variant there
    const wchar_t* pick;  // which release and variant to pick there, in a sentence; {0} is the pinned version
};
const GoogleApp kGoogleApps[] = {
    {"com.google.android.gsf", L"Google Services Framework",
     L"https://www.apkmirror.com/apk/google-inc/google-services-framework/",
     L"Pick version {0}, for Android 14 or later."},
    {"com.google.android.gms", L"Google Play services",
     L"https://www.apkmirror.com/apk/google-inc/google-play-services/",
     L"Pick version {0}: the x86 + x86_64 variant, nodpi, for Android 7.0 or later."},
    {"com.android.vending", L"Google Play Store", L"https://www.apkmirror.com/apk/google-inc/google-play-store/",
     L"Pick version {0}: the universal variant, nodpi, for Android 7.0 or later."},
};

// The version google_apps.h pins for a package; empty for none.
std::string pinned_version(const std::string& pkg) {
    for (const GooglePin& p : kGooglePins) {
        if (pkg == p.pkg) return p.version;
    }
    return {};
}

// The pinned version as a left-to-right run, so that a Hebrew sentence keeps "[0] [PR]" and "(040800-...)" as
// they are written.
std::wstring pinned_text(const GoogleApp& a) {
    return L"\x202A" + utf8_to_wide(pinned_version(a.pkg)) + L"\x202C";
}

const GoogleApp* google_app(const std::string& pkg) {
    for (const GoogleApp& a : kGoogleApps) {
        if (pkg == a.pkg) return &a;
    }
    return nullptr;
}

bool google_services_on() { return setting(L"GoogleServices", 0) != 0; }

// The library app that is this package, or null.
const App* app_of_package(const std::string& pkg) {
    const auto it = g_packages.find(pkg);
    return it == g_packages.end() ? nullptr : find_app(it->second);
}

bool google_apps_ready() {
    for (const GoogleApp& a : kGoogleApps) {
        if (!app_of_package(a.pkg)) return false;
    }
    return true;
}

// ---- Background updates ---------------------------------------------------------
//
// On a phone the Play Store keeps apps up to date with nothing on screen: JobScheduler starts its process for
// its daily jobs (an update check, then the installs), at times Android picks (doze's maintenance windows), on
// mains power and an unmetered network. Win.apk's system broker holds the apps' jobs (BrokerJobs) and starts
// the Play Store's process for a job that is due, but it runs only while an app is open. So with "Use Google
// services" and Settings > Google account > "Update apps in the background" on, the launcher opens a
// maintenance window every few hours while it runs, shown or hidden: `winapk-runtime --system --maintenance`
// boots Google services, runs the jobs that are due (their own constraints decide: network, unmetered,
// charging) and stops a few minutes after the last. The rule: only while Win.apk runs, never from a
// Windows scheduled task or a service; the launcher is not kept alive for it, but a launcher never shown waits
// for a window it opened (maybe_quit). Closing the library is closing Win.apk: from then on no window is
// opened, and once no app window is on screen the launcher quits without waiting, which ends the window's
// broker and the apps it started (end_background_work; the broker writes its files whole, and a Play Store
// install under way is cut off as a phone's power-off cuts it). When is decided in maintenance.h; the
// window's conditions are Android's too: only on mains power, with a network. Logs: logs\maintenance.log.

namespace mw = winapk::launcher::maintenance;

DWORD unix_now();
bool any_splash();

// The scheduled task earlier builds made. Removed once, at the first start of a build without it; the
// uninstaller removes it too (winapk.iss).
constexpr wchar_t kMaintenanceTask[] = L"Win.apk\\Background updates";

HANDLE g_maintenance = nullptr;  // the window this launcher opened, until kMaintenanceEnded
// The user closed the library (WM_CLOSE) and has not shown it again (show_library): Win.apk is on its way out,
// so no maintenance window is opened and a running one is not waited for (maintenance.h).
bool g_closed_by_user = false;

bool background_updates_on() { return google_services_on() && setting(L"BackgroundUpdates", 1) != 0; }

// Runs schtasks.exe (no window) with `args`; its exit code, or -1 when it did not run.
long run_schtasks(const std::wstring& args) {
    wchar_t sys[MAX_PATH] = {};
    GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(sys) + L"\\schtasks.exe\" " + args;
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return -1;
    CloseHandle(pi.hThread);
    // Not run when it did not end in time: its exit code would read STILL_ACTIVE (259), taken for an answer.
    const bool ended = WaitForSingleObject(pi.hProcess, 30000) == WAIT_OBJECT_0;
    DWORD code = 1;
    if (ended) GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return ended ? static_cast<long>(code) : -1;
}

// Off the library's thread: schtasks takes a moment. Noted as done once schtasks ran, whether or not there was
// a task (it exits 1 when there is none).
DWORD WINAPI remove_maintenance_task_thread(void*) {
    if (run_schtasks(std::wstring(L"/Delete /TN \"") + kMaintenanceTask + L"\" /F") >= 0)
        set_setting(L"MaintenanceTaskRemoved", 1);
    return 0;
}

void remove_maintenance_task() {
    if (setting(L"MaintenanceTaskRemoved", 0)) return;
    if (HANDLE t = CreateThread(nullptr, 0, remove_maintenance_task_thread, nullptr, 0, nullptr)) CloseHandle(t);
}

// Not on battery: AC online, or a PC that does not know (a desktop with no battery reports online already).
bool on_mains_power() {
    SYSTEM_POWER_STATUS s{};
    return !GetSystemPowerStatus(&s) || s.ACLineStatus != 0;
}

// Connected to some network, as the scheduled task's RunOnlyIfNetworkAvailable was. Network List Manager is
// asked on the library's thread, where COM is up (OleInitialize); when it cannot be asked, the window is not
// held back: the broker's jobs check their own network constraints.
bool on_network() {
    INetworkListManager* nlm = nullptr;
    if (FAILED(CoCreateInstance(CLSID_NetworkListManager, nullptr, CLSCTX_ALL, IID_INetworkListManager,
                                reinterpret_cast<void**>(&nlm))) ||
        !nlm)
        return true;
    // GetConnectivity: the one name MinGW's header and the SDK's agree on (IsConnected is get_IsConnected there).
    NLM_CONNECTIVITY connectivity = NLM_CONNECTIVITY_DISCONNECTED;
    const bool asked = SUCCEEDED(nlm->GetConnectivity(&connectivity));
    nlm->Release();
    return !asked || connectivity != NLM_CONNECTIVITY_DISCONNECTED;
}

// A system broker runs in this user session: an app's, or a window another launcher opened. Its mutex is the
// runtime's own (runtime_main.cpp, --system).
bool broker_running() {
    HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\WinApk-system");
    if (!m) return false;
    CloseHandle(m);
    return true;
}

// Waits for the window's broker, then tells the library (kMaintenanceEnded), which forgets it.
DWORD WINAPI maintenance_wait_thread(void* p) {
    WaitForSingleObject(static_cast<HANDLE>(p), INFINITE);
    if (g_main) PostMessageW(g_main, kMaintenanceEnded, 0, 0);
    return 0;
}

// At the launcher's start, every mw::kCheckMinutes, and when the setting or the setup changes: opens a
// maintenance window when one is due and may run now. The library's thread only.
void maintenance_check() {
    if (!g_main) return;
    mw::Conditions c;
    c.setting_on = background_updates_on();
    c.apps_ready = c.setting_on && google_apps_ready();
    c.window_running = g_maintenance != nullptr;
    c.app_starting = any_splash();
    c.closed_by_user = g_closed_by_user;
    if (c.setting_on && c.apps_ready && !c.window_running) {
        c.broker_running = broker_running();
        c.on_mains = on_mains_power();
        c.on_network = on_network();
    }
    const DWORD now = unix_now();
    if (mw::decide(now, setting(L"LastMaintenance", 0), c) != mw::Decision::kStart) return;
    HANDLE p = start_runtime(L"--system --maintenance", logs_dir() / L"maintenance.log");
    if (!p) return;
    set_setting(L"LastMaintenance", now);
    HANDLE t = CreateThread(nullptr, 0, maintenance_wait_thread, p, 0, nullptr);
    if (!t) {
        CloseHandle(p);  // not waited for: the launcher does not hold its exit for it
        return;
    }
    CloseHandle(t);
    g_maintenance = p;
}

// kMaintenanceEnded: the window's broker exited.
void maintenance_ended() {
    if (g_maintenance) CloseHandle(g_maintenance);
    g_maintenance = nullptr;
    maybe_quit();
}

// Whether `lnk` is a shortcut this launcher made for an app: its target is `exe` and its arguments `--run <apk>`
// (run_args). A shortcut to anything else, or to the library itself, is not.
bool app_shortcut_of(const fs::path& lnk, const fs::path& exe) {
    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) return false;
    bool ours = false;
    IPersistFile* file = nullptr;
    if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file)))) {
        wchar_t target[MAX_PATH * 4] = {}, args[MAX_PATH * 4] = {};
        if (SUCCEEDED(file->Load(lnk.c_str(), STGM_READ)) &&
            SUCCEEDED(link->GetPath(target, MAX_PATH * 4, nullptr, SLGP_RAWPATH)) &&
            SUCCEEDED(link->GetArguments(args, MAX_PATH * 4))) {
            ours = CompareStringOrdinal(target, -1, exe.c_str(), -1, TRUE) == CSTR_EQUAL &&
                   wcsncmp(args, L"--run ", 6) == 0;
        }
        file->Release();
    }
    link->Release();
    return ours;
}

// `Win.apk.exe remove-shortcuts`, the uninstaller's step (winapk.iss [UninstallRun], before the files go): the
// shortcuts this launcher made for the apps, on the desktop and in Start > Win.apk Apps, which would otherwise
// start a Win.apk.exe that is gone. Found by what they run (app_shortcut_of), not by the library's names, so a
// shortcut of an app renamed since, or of a library app removed by hand, goes too; the user's own shortcuts stay.
// The library and the apps' data are kept, as the uninstall keeps them.
int remove_shortcuts_main() {
    const fs::path exe = exe_path();
    std::error_code ec;
    for (const fs::path& dir : {known_folder(FOLDERID_Desktop), start_menu_apps()}) {
        std::vector<fs::path> gone;
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            std::wstring ext = it->path().extension().wstring();
            for (wchar_t& c : ext) c = static_cast<wchar_t>(towlower(c));
            if (ext == L".lnk" && app_shortcut_of(it->path(), exe)) gone.push_back(it->path());
        }
        for (const fs::path& p : gone) fs::remove(p, ec);
        ec.clear();
    }
    fs::remove(start_menu_apps(), ec);  // when empty
    SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATHW, known_folder(FOLDERID_Desktop).c_str(), nullptr);
    return 0;
}

// `Win.apk.exe maintenance`, which the scheduled task of earlier builds ran: that task should not be there, so
// it is removed and no window is opened (the launcher opens them now, while it runs).
int maintenance_main() {
    remove_maintenance_task_thread(nullptr);
    return 0;
}

fs::path setup_log() { return logs_dir() / L"google-setup.log"; }

// The MINSDK --verify-apk gives a minSdkVersion that is a codename (ApkCheck.PREVIEW_MIN_SDK): a preview's build,
// which a released Android refuses.
constexpr int kPreviewMinSdk = 10000;

// winapk-runtime --verify-apk's answer for one file.
struct SetupCheck {
    fs::path file;
    bool dropped = false;  // dropped on the page: say why when it is not one of the three
    int code = -1;
    std::string pkg;
    bool verified = false, google = false;
    std::wstring reason;
    std::wstring abis;  // the ABIs its native code is for, comma-separated; empty: none
    int min_sdk = 0;    // its minSdkVersion (0: unknown; kPreviewMinSdk and above: a preview's codename)
    int sdk = 0;        // the API level Win.apk presents at most (PresentedSdk's ceiling; 0: unknown)
};

struct SetupState {
    fs::path folder;                                   // the Downloads folder being watched
    HANDLE stop = nullptr, thread = nullptr;           // the watch thread and what ends it
    std::map<std::wstring, std::pair<long long, uintmax_t>> seen;  // file: time and size already looked at
    std::deque<std::pair<fs::path, bool>> queue;       // files to check, dropped or not
    std::wstring checking;                             // the file being checked now
    std::map<std::string, std::wstring> installing;    // package: the library name its install has
    std::set<std::string> fetching;                    // packages apkeep is downloading, or queued to
    std::deque<std::string> fetch_queue;               // packages waiting for their turn (one download at a time)
    bool fetch_running = false;                        // a download thread is running
    std::set<std::string> fetch_failed;                // packages whose APKPure copy failed: APKMirror instead
    std::set<std::wstring> fetched;                    // files apkeep downloaded (their failures fall back)
    std::set<std::string> verifying;                   // packages whose download is being checked
} g_setup;

DWORD WINAPI setup_watch_thread(void* param) {
    std::unique_ptr<std::pair<fs::path, HANDLE>> p(static_cast<std::pair<fs::path, HANDLE>*>(param));
    HANDLE change = FindFirstChangeNotificationW(p->first.c_str(), FALSE,
                                                 FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE |
                                                     FILE_NOTIFY_CHANGE_LAST_WRITE);
    if (change == INVALID_HANDLE_VALUE) return 0;
    HANDLE both[2] = {p->second, change};
    while (WaitForMultipleObjects(2, both, FALSE, INFINITE) == WAIT_OBJECT_0 + 1) {
        PostMessageW(g_main, kSetupChanged, 0, 0);
        if (!FindNextChangeNotification(change)) break;
    }
    FindCloseChangeNotification(change);
    return 0;
}

// Looks for app files in the Downloads folder that are new or changed since it was last looked at. The first
// look (`snapshot`) only takes note: what was there before the page opened is the user's to drop.
void setup_scan(bool snapshot) {
    std::error_code ec;
    bool busy = false;  // a file still being written: look again in a moment
    for (fs::directory_iterator it(g_setup.folder, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path& p = it->path();
        std::error_code fe;
        if (!it->is_regular_file(fe) || !is_app_file(p)) continue;
        const std::pair<long long, uintmax_t> stamp{
            static_cast<long long>(fs::last_write_time(p, fe).time_since_epoch().count()), fs::file_size(p, fe)};
        const auto old = g_setup.seen.find(p.wstring());
        if (old != g_setup.seen.end() && old->second == stamp) continue;
        if (!snapshot) {
            // A browser renames its download into place once it is whole; another program may still be writing.
            HANDLE f = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            if (f == INVALID_HANDLE_VALUE) {
                busy = true;
                continue;
            }
            CloseHandle(f);
            g_setup.queue.emplace_back(p, false);
        }
        g_setup.seen[p.wstring()] = stamp;
    }
    if (busy && g_main) SetTimer(g_main, kSetupScanTimer, 2000, nullptr);
}

// Starts or stops watching the Downloads folder: it is watched only while the setup page is shown.
void setup_watch(bool on) {
    if (on == (g_setup.thread != nullptr)) return;
    if (!on) {
        SetEvent(g_setup.stop);
        WaitForSingleObject(g_setup.thread, 2000);
        CloseHandle(g_setup.thread);
        CloseHandle(g_setup.stop);
        g_setup.thread = g_setup.stop = nullptr;
        if (g_main) KillTimer(g_main, kSetupScanTimer);
        return;
    }
    g_setup.folder = known_folder(FOLDERID_Downloads);
    if (g_setup.folder.empty()) return;
    g_setup.seen.clear();
    setup_scan(true);
    g_setup.stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_setup.thread = g_setup.stop ? CreateThread(nullptr, 0, setup_watch_thread,
                                                 new std::pair<fs::path, HANDLE>(g_setup.folder, g_setup.stop), 0,
                                                 nullptr)
                                  : nullptr;
    if (!g_setup.thread && g_setup.stop) {
        CloseHandle(g_setup.stop);
        g_setup.stop = nullptr;
    }
}

DWORD WINAPI setup_check_thread(void* param) {
    auto* c = static_cast<SetupCheck*>(param);
    std::wstring out;
    c->code = run_runtime_captured(L"--verify-apk " + gms_arg(c->file.wstring()), &out, setup_log());
    for (size_t at = 0; at < out.size();) {
        size_t end = out.find(L'\n', at);
        if (end == std::wstring::npos) end = out.size();
        std::wstring line = out.substr(at, end - at);
        at = end + 1;
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        const size_t tab = line.find(L'\t');
        if (tab == std::wstring::npos) continue;
        const std::wstring key = line.substr(0, tab), value = line.substr(tab + 1);
        if (key == L"PACKAGE") c->pkg = wide_to_utf8(value);
        else if (key == L"VERIFIED") c->verified = value == L"1";
        else if (key == L"GOOGLE") c->google = value == L"1";
        else if (key == L"REASON") c->reason = value;
        else if (key == L"ABIS") c->abis = value;
        else if (key == L"MINSDK") c->min_sdk = _wtoi(value.c_str());
        else if (key == L"SDK") c->sdk = _wtoi(value.c_str());
    }
    if (!PostMessageW(g_main, kSetupChecked, 0, reinterpret_cast<LPARAM>(c))) delete c;
    return 0;
}

// Checks the next file in the queue, one at a time.
void setup_next() {
    if (!g_setup.checking.empty() || g_setup.queue.empty()) return;
    auto* c = new SetupCheck;
    c->file = g_setup.queue.front().first;
    c->dropped = g_setup.queue.front().second;
    g_setup.queue.pop_front();
    g_setup.checking = c->file.filename().wstring();
    HANDLE t = CreateThread(nullptr, 0, setup_check_thread, c, 0, nullptr);
    if (t) {
        CloseHandle(t);
    } else {
        delete c;
        g_setup.checking.clear();
    }
    if (g_view == kGmsSetupView) rebuild_page();
}

// A file dropped on the setup page.
void setup_offer(const fs::path& file) {
    g_setup.queue.emplace_back(file, true);
    setup_next();
}

fs::path apkeep_exe() { return root() / L"bin" / L"tools" / L"apkeep.exe"; }
fs::path setup_downloads() { return data_dir() / L"downloads" / L"google"; }
fs::path setup_download_log() { return logs_dir() / L"google-download.log"; }

bool apkeep_present() {
    std::error_code ec;
    return fs::exists(apkeep_exe(), ec);
}

// One apkeep download: the package asked for, and the app file it left (empty: none).
struct SetupFetch {
    std::string pkg;
    fs::path file;
};

// Appends one line to logs\google-download.log, prefixed with the local time.
void fetch_log(HANDLE log, const std::string& line) {
    if (log == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    char stamp[32];
    snprintf(stamp, sizeof stamp, "%04u-%02u-%02u %02u:%02u:%02u ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
             t.wSecond);
    const std::string text = stamp + line + "\n";
    DWORD n = 0;
    WriteFile(log, text.data(), static_cast<DWORD>(text.size()), &n, nullptr);
}

// Runs apkeep once for `pkg` into the empty folder `part`, its output appended to `log`; the app file it left,
// or empty. apkeep says why it skipped an app only on a console (indicatif's println is silent when its output
// is a file) and exits 0 either way, so the file alone tells success.
fs::path fetch_once(const std::string& pkg, const fs::path& part, HANDLE log, DWORD* code) {
    // apkeep's own guard: APKPure is refused unless the caller acknowledges what it has served before. Win.apk
    // does, because nothing installs here that Google did not sign (setup_checked). The version is google_apps.h's
    // pin (its name holds spaces and brackets: quoted), asked for as a device of Win.apk's Android 14 (sdk=34,
    // apkeep-patches/0002) would ask, and only a variant with x86_64 code, or none, is taken.
    std::wstring cmd = L"\"" + apkeep_exe().wstring() + L"\" -a \"" + utf8_to_wide(pkg + "@" + pinned_version(pkg)) +
                       L"\" -d apk-pure -o acknowledge_dangers=true,arch=x86_64,sdk=34 " + gms_arg(part.wstring());
    fetch_log(log, "> " + wide_to_utf8(cmd));
    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    std::vector<BYTE> attr_buf(attr_size);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
    const bool listed = log != INVALID_HANDLE_VALUE && InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size) &&
                        UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &log, sizeof(HANDLE),
                                                  nullptr, nullptr);
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    if (listed) {
        si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        si.StartupInfo.hStdOutput = si.StartupInfo.hStdError = log;
        si.lpAttributeList = attrs;
    }
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, listed, CREATE_NO_WINDOW |
                                   (listed ? EXTENDED_STARTUPINFO_PRESENT : 0), nullptr, part.c_str(),
                                   &si.StartupInfo, &pi);
    if (listed) DeleteProcThreadAttributeList(attrs);
    *code = ok ? 1 : GetLastError();
    if (!ok) return {};
    CloseHandle(pi.hThread);
    // A stalled download is given up after ten minutes; Play services is about 300 MB.
    if (WaitForSingleObject(pi.hProcess, 10 * 60 * 1000) == WAIT_TIMEOUT) TerminateProcess(pi.hProcess, 1);
    *code = wait_exit(pi.hProcess);
    std::error_code ec;
    for (fs::directory_iterator it(part, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code fe;
        if (it->is_regular_file(fe) && is_app_file(it->path()) && fs::file_size(it->path(), fe) > 0) {
            return it->path();
        }
    }
    return {};
}

// Downloads one package, trying three times (APKPure fails now and then, and apkeep tries no further), into
// downloads\google\<pkg>.part\, and keeps the file as downloads\google\<pkg>.<ext>. Every attempt and its
// outcome is in logs\google-download.log.
DWORD WINAPI setup_fetch_thread(void* param) {
    auto* f = static_cast<SetupFetch*>(param);
    std::error_code ec;
    const fs::path dir = setup_downloads();
    const fs::path part = dir / (utf8_to_wide(f->pkg) + L".part");
    fs::create_directories(logs_dir(), ec);
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE log = CreateFileW(setup_download_log().c_str(), FILE_APPEND_DATA,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &sa, OPEN_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    constexpr int kAttempts = 3;
    for (int attempt = 1; attempt <= kAttempts && f->file.empty(); ++attempt) {
        if (attempt > 1) Sleep(attempt == 2 ? 5000 : 20000);
        fs::remove_all(part, ec);
        fs::create_directories(part, ec);
        DWORD code = 1;
        const fs::path got = fetch_once(f->pkg, part, log, &code);
        if (got.empty()) {
            fetch_log(log, f->pkg + ": attempt " + std::to_string(attempt) + " of " + std::to_string(kAttempts) +
                               " left no app file (apkeep exit code " + std::to_string(code) + ")");
            continue;
        }
        // "com.google.android.gms@26.34.36 (...)@x86_64.apk" -> "com.google.android.gms.apk": the name install_name
        // reads.
        const fs::path keep = dir / (utf8_to_wide(f->pkg) + got.extension().wstring());
        std::error_code fe;
        fs::remove(keep, fe);
        fs::rename(got, keep, fe);
        if (fe) {
            fetch_log(log, f->pkg + ": could not move " + wide_to_utf8(got.wstring()) + ": " + fe.message());
            continue;
        }
        f->file = keep;
        fetch_log(log, f->pkg + ": downloaded " + wide_to_utf8(keep.wstring()) + " (" +
                           std::to_string(fs::file_size(keep, fe)) + " bytes)");
    }
    fs::remove_all(part, ec);
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    if (!PostMessageW(g_main, kSetupFetched, 0, reinterpret_cast<LPARAM>(f))) delete f;
    return 0;
}

// Starts the next queued download, one at a time: three at once from APKPure is the kind of burst it refuses.
void setup_fetch_next() {
    if (g_setup.fetch_running || g_setup.fetch_queue.empty()) return;
    auto* f = new SetupFetch;
    f->pkg = g_setup.fetch_queue.front();
    g_setup.fetch_queue.pop_front();
    HANDLE t = CreateThread(nullptr, 0, setup_fetch_thread, f, 0, nullptr);
    if (!t) {
        // No thread: the app's row offers APKMirror instead.
        g_setup.fetching.erase(f->pkg);
        g_setup.fetch_failed.insert(f->pkg);
        delete f;
        setup_fetch_next();
        return;
    }
    CloseHandle(t);
    g_setup.fetch_running = true;
}

// Download for one of Google's apps: apkeep from APKPure (queued behind any other), or, without apkeep or after
// its copy failed, the app's APKMirror page in the browser.
void setup_fetch(const GoogleApp& a) {
    if (g_setup.fetching.count(a.pkg)) return;
    if (!apkeep_present() || g_setup.fetch_failed.count(a.pkg)) {
        ShellExecuteW(nullptr, L"open", a.url, nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    g_setup.fetching.insert(a.pkg);
    g_setup.fetch_queue.push_back(a.pkg);
    setup_fetch_next();
    if (g_view == kGmsSetupView) rebuild_page();
}

// apkeep finished: check its file as any other (setup_checked), or say it failed and offer APKMirror.
void setup_fetched(std::unique_ptr<SetupFetch> f) {
    g_setup.fetching.erase(f->pkg);
    g_setup.fetch_running = false;
    setup_fetch_next();
    const GoogleApp* app = google_app(f->pkg);
    if (!f->file.empty()) {
        g_setup.fetched.insert(f->file.wstring());
        g_setup.verifying.insert(f->pkg);
        g_setup.queue.emplace_back(f->file, true);
        setup_next();
    } else if (app) {
        g_setup.fetch_failed.insert(f->pkg);
        error_with_log(fmt(L"{0} could not be downloaded", {tr(app->name)}),
                       fmt(L"APKPure did not give version {0} in three tries. The app's row now opens its page on "
                           L"APKMirror, where you can download that version yourself.",
                           {pinned_text(*app)}),
                       setup_download_log());
    }
    if (g_view == kGmsSetupView) rebuild_page();
}

// The check's answer: install the file when it is one of the three apps and Google signed it, else say why.
void setup_checked(std::unique_ptr<SetupCheck> c) {
    g_setup.checking.clear();
    if (g_view == kGmsSetupView) rebuild_page();  // no longer "Checking" behind a refusal's dialog
    const GoogleApp* app = google_app(c->pkg);
    const std::wstring file = c->file.filename().wstring();
    const bool fetched = g_setup.fetched.erase(c->file.wstring()) > 0;
    if (fetched) g_setup.verifying.erase(wide_to_utf8(c->file.stem().wstring()));  // downloads\google\<pkg>.<ext>
    bool passed = false;
    if (c->code < 0) {
        error_with_log(tr(L"The file could not be checked"), tr(L"Win.apk's runtime did not start."), setup_log());
    } else if (c->pkg.empty() && c->dropped) {
        // ApkCheck read no package: a damaged or cut-off download, not "another app".
        error_with_log(tr(L"The file could not be checked"),
                       fmt(L"{0} could not be read as an Android app. It may be damaged or not fully downloaded; "
                           L"download it again.",
                           {file}),
                       setup_log());
    } else if (!app) {
        // A download of something else is none of the page's business; a file dropped on it is.
        if (c->dropped) {
            info(fmt(L"{0} is not one of Google's three apps. To add another app, drop it on the library.", {file}),
                 kError);
        }
    } else if (!c->verified) {
        error_with_log(fmt(L"{0} was not installed", {tr(app->name)}),
                       fmt(L"The signature of {0} does not verify, so the file may have been changed after it was "
                           L"signed. Download it again from APKMirror.",
                           {file}) +
                           (c->reason.empty() ? L"" : L"\n\x200E" + c->reason),
                       setup_log());
    } else if (!c->google) {
        error_with_log(fmt(L"{0} was not installed", {tr(app->name)}),
                       fmt(L"{0} is not signed by Google, so Win.apk does not trust it as Google's app. Download "
                           L"it from its APKMirror page.",
                           {file}),
                       setup_log());
    } else if (!c->abis.empty() && (L"," + c->abis + L",").find(L",x86_64,") == std::wstring::npos) {
        // No x86_64 code (ARM's, or 32-bit x86's): Win.apk runs x86_64 code, and translating Play services is no
        // way to run it.
        error_with_log(fmt(L"{0} was not installed", {tr(app->name)}),
                       fmt(L"{0} has no code for x86_64 processors, only for {1}. On APKMirror, download its x86_64 "
                           L"variant.",
                           {file, c->abis}),
                       setup_log());
    } else if (c->sdk > 0 && c->min_sdk >= kPreviewMinSdk) {
        // A minSdkVersion that is a codename: Android installs a preview's build only on that preview.
        error_with_log(fmt(L"{0} was not installed", {tr(app->name)}),
                       fmt(L"{0} is built for a preview of Android, which Android does not install on a released "
                           L"version. Download the version its row names.",
                           {file}),
                       setup_log());
    } else if (c->sdk > 0 && c->min_sdk > c->sdk) {
        error_with_log(fmt(L"{0} was not installed", {tr(app->name)}),
                       fmt(L"{0} needs API level {1}, and Win.apk presents API level {2}. Download the version its "
                           L"row names.",
                           {file, std::to_wstring(c->min_sdk), std::to_wstring(c->sdk)}),
                       setup_log());
    } else {
        passed = true;
        const std::wstring name = install_name(c->file);
        g_setup.installing[c->pkg] = name;
        g_setup_names.insert(name);
        install(c->file, name);
    }
    // A copy from APKPure that did not pass: the row's button opens APKMirror from now on.
    if (fetched && !passed && !c->pkg.empty()) g_setup.fetch_failed.insert(c->pkg);
    if (g_view == kGmsSetupView) rebuild_page();
    setup_next();
}

std::vector<Section> gms_setup_sections() {
    std::vector<Section> out;
    const bool ready = google_apps_ready();
    Section state{L"", {}, L""};
    if (ready) {
        Row r = text_row(tr(L"Google services are ready"), tr(L"Ready"),
                         tr(L"From now on the Play Store keeps itself and Play services up to date, as on a phone."));
        r.check = true;
        state.rows.push_back(r);
    } else {
        // While the page is open the Downloads folder is watched (and a file can be dropped here): each app's row
        // says which file to download, and the files that pass the checks install themselves.
        const std::wstring now = !g_setup.checking.empty() ? fmt(L"Checking {0}…", {g_setup.checking})
                                 : !g_setup.fetching.empty() ? std::wstring(tr(L"Downloading…"))
                                 : apkeep_present()
                                     ? std::wstring(tr(L"Download all fetches the apps; you can also download each "
                                                       L"one yourself or drop it here. Win.apk checks that Google "
                                                       L"signed it, then installs it."))
                                     : std::wstring(tr(L"Download each app to your Downloads folder, or drop it "
                                                       L"here. Win.apk checks that Google signed it, then installs "
                                                       L"it."));
        if (apkeep_present()) {
            // One click for the three: each that is neither installed nor on its way is downloaded.
            Row all = button_row(tr(L"To use Google Play, install the following apps as described."), now,
                                            tr(L"Download all"), [] {
                                                for (const GoogleApp& a : kGoogleApps) {
                                                    const auto inst = g_setup.installing.find(a.pkg);
                                                    if (app_of_package(a.pkg) ||
                                                        (inst != g_setup.installing.end() &&
                                                         is_pending(inst->second)) ||
                                                        g_setup.fetch_failed.count(a.pkg)) {
                                                        continue;
                                                    }
                                                    setup_fetch(a);
                                                }
                                            });
            // One click starts it all; until every download, check and install has ended there is nothing to add.
            all.disabled = !g_setup.fetching.empty() || !g_setup.verifying.empty() || !g_setup.checking.empty() ||
                           std::any_of(g_setup.installing.begin(), g_setup.installing.end(),
                                       [](const auto& e) { return is_pending(e.second); });
            state.rows.push_back(std::move(all));
        } else {
            state.rows.push_back(
                text_row(tr(L"To use Google Play, install the following apps as described."), L"", now));
        }
    }
    out.push_back(std::move(state));

    Section apps{tr(L"Google's apps"), {}, L""};
    for (const GoogleApp& a : kGoogleApps) {
        const App* app = app_of_package(a.pkg);
        const auto inst = g_setup.installing.find(a.pkg);
        if (app) {
            const std::vector<std::string> p = package_of(app->name);
            const std::wstring v = p.size() > 2 && !p[2].empty() ? utf8_to_wide(p[2]) : L"";
            Row r = text_row(tr(a.name), v.empty() ? std::wstring(tr(L"Installed")) : fmt(L"Installed, {0}", {v}),
                             utf8_to_wide(a.pkg));
            r.check = true;
            apps.rows.push_back(r);
        } else if (inst != g_setup.installing.end() && is_pending(inst->second)) {
            apps.rows.push_back(text_row(tr(a.name), tr(L"Installing…"), utf8_to_wide(a.pkg)));
        } else if (g_setup.verifying.count(a.pkg)) {
            apps.rows.push_back(text_row(tr(a.name), tr(L"Checking…"), tr(L"Win.apk checks that Google signed it.")));
        } else if (g_setup.fetching.count(a.pkg)) {
            const bool queued = std::find(g_setup.fetch_queue.begin(), g_setup.fetch_queue.end(), a.pkg) !=
                                g_setup.fetch_queue.end();
            apps.rows.push_back(text_row(tr(a.name), queued ? tr(L"Waiting to download") : tr(L"Downloading…"),
                                         fmt(L"From APKPure, version {0}.", {pinned_text(a)})));
        } else if (apkeep_present() && !g_setup.fetch_failed.count(a.pkg)) {
            const GoogleApp* ga = &a;
            apps.rows.push_back(button_row(tr(a.name), tr(L"Not installed.") + std::wstring(L" ") +
                                               tr(L"Win.apk downloads it and checks that Google signed it."),
                                           tr(L"Download"), [ga] { setup_fetch(*ga); }));
        } else {
            const GoogleApp* ga = &a;
            apps.rows.push_back(button_row(tr(a.name), tr(L"Not installed.") + std::wstring(L" ") +
                                               fmt(a.pick, {pinned_text(a)}),
                                           tr(L"APKMirror"), [ga] { setup_fetch(*ga); }));
        }
    }
    out.push_back(std::move(apps));
    return out;
}

// The setup page: from Settings (its back arrow returns there) or from Google Play in the pane.
void show_gms_setup(bool from_settings) {
    Page p;
    p.title = tr(L"Google services setup");
    p.back = from_settings;
    p.from = kSettingsView;
    p.build = gms_setup_sections;
    g_setup_from_settings = from_settings;
    open_page(kGmsSetupView, std::move(p));
}

// ---- Apps in the window: the Store and Google Play ------------------------------------------------
//
// The Store and Google Play in the pane open their app inside the library window, in the content area, not in a
// window of its own. The launcher puts a host panel (a child window, panel_proc) over the content area under the
// title and starts the runtime with WINAPK_EMBED_PARENT=<the panel's handle> and WINAPK_PHONE_SIZE the panel's
// size in dp; the runtime then makes its window a child of the panel (win32_window.cpp, "embedded in the
// launcher") that fills it and follows its size, so the app lays itself out for the area as it does for a resized
// window. The two processes' input queues are joined, as Windows does for any parent and child, so the panel gives
// the app the keyboard focus when it is shown.
//
// Leaving for another tab hides the panel and the app keeps running, as a phone keeps an app in the background;
// its item again shows it at once. Closing the library window stops it. If it exits, the area says so, with Open
// (and Show log when it never came up). If the runtime could not embed its window (an older runtime, or one
// already running in its own window), the window is a normal one: the launcher sees the panel has no child once
// the app is up, says so and leaves the window as it is.

constexpr wchar_t kPanelClass[] = L"WinApkAppPanel";

HWND embedded_window(const Embedded& e) { return e.panel ? GetWindow(e.panel, GW_CHILD) : nullptr; }

// The app in the window that runs as `name`, or null.
Embedded* embedded_of(const std::wstring& name) {
    for (Embedded* e : {&g_store_app, &g_play}) {
        if (!e->name.empty() && e->name == name) return e;
    }
    return nullptr;
}

// The app the view is, while the view is the Store or Google Play.
Embedded* shown_embedded() {
    if (g_view == kStoreView) return &g_store_app;
    if (g_view == kPlayView) return &g_play;
    return nullptr;
}

// Each panel's place and visibility: over the whole content while its view is shown and the app is up.
void place_panels() {
    const RectF& r = g_l.panel;
    for (Embedded* e : {&g_store_app, &g_play}) {
        if (!e->panel) continue;
        MoveWindow(e->panel, static_cast<int>(r.X), static_cast<int>(r.Y), static_cast<int>(r.Width),
                   static_cast<int>(r.Height), TRUE);
        const bool show = g_view == e->view && e->up();
        if (show != (IsWindowVisible(e->panel) != FALSE)) ShowWindow(e->panel, show ? SW_SHOW : SW_HIDE);
    }
}

// Gives the shown app the keyboard.
void focus_embedded() {
    if (Embedded* e = shown_embedded()) {
        if (HWND w = embedded_window(*e)) SetFocus(w);
    }
}

// The app's window fills the panel. Asynchronous: a busy app never holds the launcher's thread up.
void fit_embedded_window(const Embedded& e) {
    HWND w = embedded_window(e);
    if (!w) return;
    RECT rc;
    GetClientRect(e.panel, &rc);
    SetWindowPos(w, nullptr, 0, 0, rc.right, rc.bottom, SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
}

LRESULT CALLBACK panel_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
    }
    Embedded* e = reinterpret_cast<Embedded*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!e) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT:  // under the app's window: the spinner until it is up
            buffered_paint(hwnd, [e](Graphics& g, float w, float h) {
                g.Clear(g_t.layer);
                if (e->state != Embedded::kStarting) return;
                const float s = px(32), cy = (std::min)(h / 2, px(220));
                spinner(g, RectF((w - s) / 2, cy - px(48), s, s), g_t.accent, px(3));
                draw_text(g, tr(e->starting), g_f.body.get(), g_t.text2, RectF(0, cy, w, px(24)), kCenter);
            });
            return 0;
        case WM_SIZE: fit_embedded_window(*e); return 0;
        case WM_PARENTNOTIFY:  // the runtime made its window: it fills the panel at once
            if (LOWORD(wp) == WM_CREATE) fit_embedded_window(*e);
            return 0;
        case WM_SETFOCUS: focus_embedded(); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void ensure_panel(Embedded& e) {
    if (e.panel) return;
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = panel_proc;
        wc.hInstance = g_inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kPanelClass;
        RegisterClassExW(&wc);
        registered = true;
    }
    // WS_EX_NOINHERITLAYOUT: the panel is mirrored with the window in Hebrew, the app's window in it never (the
    // app lays out its own right to left).
    e.panel = CreateWindowExW(layout_style() | WS_EX_NOINHERITLAYOUT | WS_EX_CONTROLPARENT, kPanelClass, e.title,
                              WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, 1, 1, g_main, nullptr, g_inst, &e);
}

void start_embedded(Embedded& e, const App& app) {
    ensure_panel(e);
    if (!e.panel) {  // no panel: the app in its own window, as any app
        launch(app);
        return;
    }
    compute_layout();
    const int w = (std::max)(1, MulDiv(static_cast<int>(g_l.panel.Width), 96, static_cast<int>(g_dpi)));
    const int h = (std::max)(1, MulDiv(static_cast<int>(g_l.panel.Height), 96, static_cast<int>(g_dpi)));
    e.name = app.name;
    e.state = Embedded::kStarting;
    begin_run(app.name);  // open_store and open_google_play start it only when it has no run
    auto* job = new RunJob{app.name, app.apk, {}};
    job->env = {L"WINAPK_EMBED_PARENT=" + std::to_wstring(reinterpret_cast<uintptr_t>(e.panel)),
                L"WINAPK_PHONE_SIZE=" + std::to_wstring(w) + L"x" + std::to_wstring(h)};
    CloseHandle(CreateThread(nullptr, 0, run_thread, job, 0, nullptr));
    place_panels();
    update_timer();
}

// Shows `app` in the window as `e`: at once when it is there already, else started there; an app running in a
// window of its own is brought forward instead.
void show_embedded(Embedded& e, const App& app) {
    const bool here = e.name == app.name && e.up();
    if (!here && run_active(app.name)) {
        bring_forward(app.name);
        return;
    }
    set_view(e.view);
    if (!here) start_embedded(e, app);
    focus_embedded();
}

// Google Play in the pane: the Play Store in the window once the three apps are installed, else the setup.
void open_google_play() {
    const App* store = app_of_package("com.android.vending");
    if (!store || !google_apps_ready()) {
        show_gms_setup(false);
        return;
    }
    show_embedded(g_play, *store);
}

// The app's window is up (kRunReady): in the panel, or, when the runtime did not embed it, on its own.
void embedded_ready(Embedded& e) {
    if (embedded_window(e)) {
        e.state = Embedded::kShown;
        InvalidateRect(e.panel, nullptr, FALSE);
        fit_embedded_window(e);
        place_panels();
        if (g_view == e.view) focus_embedded();
    } else {
        e.state = Embedded::kIdle;
        e.name.clear();
        place_panels();
        if (g_view == e.view) set_view(kLibraryView);
        info(tr(&e == &g_store_app ? L"The Store opened in its own window." : L"Google Play opened in its own window."),
             kInfo);
    }
    update_timer();
    InvalidateRect(g_main, nullptr, FALSE);
}

void store_app_may_install();

// The app's runtime ended (kRunExited); `failed`: before its window came up, and not stopped by the launcher (the
// window closing), which is simply over.
void embedded_exited(Embedded& e, bool failed) {
    e.state = failed ? Embedded::kFailed : Embedded::kClosed;
    compute_layout();
    update_timer();
    InvalidateRect(g_main, nullptr, FALSE);
    if (&e == &g_store_app) store_app_may_install();  // an update of the Store waited for it to stop
}

// The view's title, and under it what it is starting or the app's version.
void paint_embedded_title(Graphics& g, const Embedded& e, const App* app, const std::wstring& doing) {
    const RectF& t = g_l.title;
    std::wstring sub = doing;
    if (sub.empty() && e.state == Embedded::kStarting) sub = tr(e.starting);
    if (sub.empty() && app) {
        const std::vector<std::string> p = package_of(app->name);
        if (p.size() > 2 && !p[2].empty()) sub = fmt(L"Version {0}", {utf8_to_wide(p[2])});
    }
    draw_text(g, tr(e.title), g_f.title.get(), g_t.text, RectF(t.X, t.Y, t.Width, px(26)));
    draw_text(g, sub, g_f.caption.get(), g_t.text2, RectF(t.X, t.Y + px(26), t.Width, px(18)));
}

// Under the panel once the app ended: what happened, its Open, and Show log when it never came up.
void paint_embedded_end(Graphics& g, const Embedded& e, const wchar_t* glyph, const wchar_t* closed,
                        const wchar_t* failed_to_start, const wchar_t* open) {
    const bool failed = e.state == Embedded::kFailed;
    if (!failed && e.state != Embedded::kClosed) return;
    paint_empty(g, glyph, tr(failed ? failed_to_start : closed), L"");
    draw_button(g, g_l.panel_open, tr(open), g_hot.kind == kHitPanelOpen, true, false);
    if (failed) draw_button(g, g_l.panel_log, tr(L"Show log"), g_hot.kind == kHitPanelLog, false, false);
}

// The view under Google Play's panel.
void paint_play(Graphics& g) {
    paint_embedded_title(g, g_play, app_of_package("com.android.vending"), L"");
    paint_embedded_end(g, g_play, kGlyphPlay, L"Google Play closed", L"Google Play didn't start", L"Open Google Play");
}

// ---- the Store ---------------------------------------------------------------------------------------------
//
// The Store is Win.apk's own store app (israel2176/win.apk-store), a system component as Google's three apps are:
// no tile, no shortcut, the Store item its one way in. The first opening of the Store item finds no store app: the
// newest release is read from GitHub's REST API (GET /repos/israel2176/win.apk-store/releases/latest), its
// apk-store.apk downloaded and kept only at the size and SHA-256 GitHub lists for it (store_app.h), then installed
// quietly (install(..., quiet)); the page shows each step, then the app. The daily update check reads the release
// again, and a release whose apk-store.apk is not the one installed (store-app\installed.sha256) is downloaded and
// installed the same way, once the store app is not running. Nothing of what the user does is sent: the request is
// GitHub's public list of releases.
//
// What the store app installs goes through the install helper (helper_main) with no question, as a phone's
// preinstalled store installs what the user picked in it (appops::caller_is_builtin_store), and the installer of
// record its runtime writes (PackageInstallerBackend).
// Its "Install unknown apps" is allowed from its first install (grant_builtin_store), as a device's preinstalled
// store is; the user's later answer to that switch is kept across its updates.

constexpr wchar_t kStoreReleaseUrl[] = L"https://api.github.com/repos/israel2176/win.apk-store/releases/latest";

fs::path store_app_dir() { return data_dir() / L"store-app"; }
fs::path store_app_download() { return store_app_dir() / winapk::launcher::kStoreAsset; }
fs::path store_app_sha_file() { return store_app_dir() / L"installed.sha256"; }

// The release's apk-store.apk, as the check read it.
struct StoreRelease {
    std::wstring tag, url;
    std::string sha256;
    uint64_t size = 0;
};

// One check, from the library's thread to its thread and back (kStoreAppChecked).
struct StoreAppCheck {
    bool reached = false;  // GitHub answered with the release
    bool found = false;    // it has an apk-store.apk with GitHub's SHA-256 of it
    StoreRelease release;
};

// The Store's release being downloaded or waiting to be installed: the library's thread only.
StoreRelease g_store_release;

std::string installed_store_sha() {
    std::ifstream f(store_app_sha_file(), std::ios::binary);
    std::string s;
    std::getline(f, s);
    return s;
}

DWORD WINAPI store_app_check_thread(void* param) {
    std::unique_ptr<StoreAppCheck> c(static_cast<StoreAppCheck*>(param));
    const fs::path file = store_app_dir() / L"release.json";
    const int status = http_get(kStoreReleaseUrl, file, L"", nullptr, {},
                                L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28",
                                updater_session());
    if (status == 200) {
        std::ifstream f(file, std::ios::binary);
        const std::string json((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        f.close();
        Json j{json.data(), json.data() + json.size()};
        auto text = [&j](std::string* out) {  // a string, or skips a null
            j.ws();
            if (j.p < j.end && *j.p == '"') j.str(out);
            else j.skip();
        };
        winapk::launcher::ListedRelease l;
        std::string url;
        j.object([&](const std::string& key) {
            std::string v;
            if (key == "tag_name") {
                text(&v);
                l.tag = utf8_to_wide(v);
            } else if (key == "draft" || key == "prerelease") {
                j.scalar(&v);
                (key == "draft" ? l.draft : l.prerelease) = v == "true";
            } else if (key == "assets") {
                j.array([&] {
                    std::string name, link, size, digest, state;
                    j.object([&](const std::string& k) {
                        if (k == "name") text(&name);
                        else if (k == "browser_download_url") text(&link);
                        else if (k == "size") j.scalar(&size);
                        else if (k == "digest") text(&digest);
                        else if (k == "state") text(&state);
                        else j.skip();
                    });
                    if (utf8_to_wide(name) == winapk::launcher::kStoreAsset && !link.empty() && !l.asset) {
                        l.asset = true;
                        l.asset_state = state;
                        l.digest = digest;
                        l.size = std::strtoull(size.c_str(), nullptr, 10);
                        url = link;
                    }
                });
            } else {
                j.skip();
            }
        });
        c->reached = j.ok;
        // Only an https link on GitHub: the digest is GitHub's, so the file must come from GitHub too.
        const bool github = url.rfind("https://github.com/", 0) == 0;
        c->found = j.ok && !l.draft && github && winapk::launcher::store_asset_checkable(l);
        if (c->found) c->release = {l.tag, utf8_to_wide(url), winapk::launcher::asset_sha256(l.digest), l.size};
    }
    std::error_code ec;
    fs::remove(file, ec);
    StoreAppCheck* out = c.release();
    if (!PostMessageW(g_main, kStoreAppChecked, 0, reinterpret_cast<LPARAM>(out))) delete out;
    return 0;
}

enum StoreAppDownload : WPARAM { kStoreDownloadBroke, kStoreDownloadUnverified, kStoreDownloadOk };

// Downloads the release's apk-store.apk and keeps it only at the release's size and SHA-256.
DWORD WINAPI store_app_download_thread(void* param) {
    std::unique_ptr<StoreRelease> r(static_cast<StoreRelease*>(param));
    const fs::path file = store_app_download();
    int shown = -1;
    const int status = http_get(r->url, file, L"", nullptr, [&](uint64_t got, uint64_t total) {
        if (!total) total = r->size;
        const int pct = total ? static_cast<int>((std::min)(got * 100 / total, uint64_t{99})) : 0;
        if (pct == shown) return;
        shown = pct;
        PostMessageW(g_main, kStoreAppProgress, static_cast<WPARAM>(pct), 0);
    }, L"", updater_session());
    StoreAppDownload result = kStoreDownloadBroke;
    if (status == 200) {
        std::error_code ec;
        uint64_t size = fs::file_size(file, ec);
        if (ec) size = 0;
        // The size first: a file of another size is refused without being hashed.
        const std::string sha = size == r->size ? sha256_of(file) : std::string();
        result = winapk::launcher::judge_store_file(size, sha, r->size, r->sha256) == winapk::launcher::StoreFile::kOk
                     ? kStoreDownloadOk
                     : kStoreDownloadUnverified;
        if (result != kStoreDownloadOk) fs::remove(file, ec);
    }
    PostMessageW(g_main, kStoreAppDownloaded, result, 0);
    return 0;
}

const App* store_app() { return app_of_package(winapk::launcher::kStorePackage); }

// The library name the Store's app has, or is installed under.
std::wstring store_app_name() {
    const App* app = store_app();
    return app ? app->name : file_name_safe(utf8_to_wide(winapk::launcher::kStorePackage));
}

bool store_app_running() {
    const std::wstring name = store_app_name();
    return app_is_running(name);
}

// Reads the newest release: the Store item with no store app, Try again, the daily update check or its Check.
void store_app_check() {
    if (!g_store_setup.begin_check()) return;  // one is under way
    std::error_code ec;
    fs::create_directories(store_app_dir(), ec);
    auto* c = new StoreAppCheck;
    if (HANDLE t = CreateThread(nullptr, 0, store_app_check_thread, c, 0, nullptr)) {
        CloseHandle(t);
    } else {
        delete c;
        g_store_setup.checked(false, false, false, store_app() != nullptr);
    }
    update_timer();
    InvalidateRect(g_main, nullptr, FALSE);
}

void store_app_checked(const StoreAppCheck& c) {
    const bool installed = store_app() != nullptr;
    const bool same = c.found && c.release.sha256 == installed_store_sha();
    if (g_store_setup.checked(c.reached, c.found, same, installed)) {
        g_store_release = c.release;
        if (HANDLE t = CreateThread(nullptr, 0, store_app_download_thread, new StoreRelease(c.release), 0, nullptr)) {
            CloseHandle(t);
        } else {
            g_store_setup.downloaded(false, false, false);
        }
    }
    update_timer();
    InvalidateRect(g_main, nullptr, FALSE);
}

// Installs the checked download, quietly: a system component, never a tile (g_setup_names) and never a shortcut
// (system_app).
void store_app_install() {
    const std::wstring name = store_app_name();
    g_setup_names.insert(name);
    install(store_app_download(), name, true);
}

void store_app_downloaded(StoreAppDownload result) {
    if (g_store_setup.downloaded(result != kStoreDownloadBroke, result == kStoreDownloadOk, store_app_running())) {
        store_app_install();
    }
    update_timer();
    InvalidateRect(g_main, nullptr, FALSE);
}

// A checked update waited for the store app to stop: it has.
void store_app_may_install() {
    if (g_store_setup.may_install(store_app_running())) store_app_install();
}

namespace appops {
void grant_builtin_store();
}

void open_store();

// kInstalled: true when `name` was the Store's own install, which the Store's page reports, not the library. Once
// installed, its "Install unknown apps" is allowed (the first time) and the APK's SHA-256 noted for the daily check;
// the page then shows the app.
bool store_app_installed(const std::wstring& name, InstallEnd::Kind how) {
    if (g_store_setup.step() != winapk::launcher::StoreSetup::Step::kInstalling || name != store_app_name())
        return false;
    const bool ok = how == InstallEnd::kDone;
    g_store_setup.installed(ok);
    if (ok) {
        std::error_code ec;
        fs::create_directories(store_app_dir(), ec);
        std::ofstream(store_app_sha_file(), std::ios::binary | std::ios::trunc) << g_store_release.sha256;
        appops::grant_builtin_store();
        if (g_view == kStoreView && !g_store_app.up()) open_store();
    }
    InvalidateRect(g_main, nullptr, FALSE);
    return true;
}

// The Store in the pane: the store app in the window; with none, it is got first (the page shows how far).
void open_store() {
    set_view(kStoreView);
    const App* app = store_app();
    if (!app) {
        if (!g_store_setup.busy()) store_app_check();
        return;
    }
    store_app_may_install();  // an update that waited for the store app to stop
    if (g_store_setup.step() == winapk::launcher::StoreSetup::Step::kInstalling) return;  // shown once installed
    show_embedded(g_store_app, *app);
}

// Whether the area under the panel offers Show log beside Open: the app never came up, or the Store's install
// failed.
bool panel_shows_log() {
    if (g_view == kStoreView && !store_app() &&
        g_store_setup.failure() == winapk::launcher::StoreSetup::Failure::kInstall)
        return true;
    const Embedded* e = shown_embedded();
    return e && e->state == Embedded::kFailed;
}

void show_panel_log() {
    if (g_view == kStoreView && !store_app()) show_logs(install_log(store_app_name()));
    else if (const Embedded* e = shown_embedded()) show_logs(run_log(e->name));
}

// What the mouse is over in the Store's or Google Play's area: the panel takes it while it is up; else the
// buttons under it.
Hit panel_hit(float x, float y) {
    const Embedded* e = shown_embedded();
    const bool setup_failed = g_view == kStoreView && !store_app() &&
                              g_store_setup.step() == winapk::launcher::StoreSetup::Step::kFailed;
    const bool ended = e && (e->state == Embedded::kClosed || e->state == Embedded::kFailed) &&
                       !(g_view == kStoreView && !store_app());
    if ((setup_failed || ended) && contains(g_l.panel_open, x, y)) return {setup_failed ? kHitRetry : kHitPanelOpen, -1};
    if (panel_shows_log() && (setup_failed || ended) && contains(g_l.panel_log, x, y)) return {kHitPanelLog, -1};
    return {};
}

// The view under the Store's panel: while the store app is got or updated before it shows, how far that is; if
// that failed, why and Try again; once it ran and ended, Open.
void paint_store(Graphics& g) {
    using Step = winapk::launcher::StoreSetup::Step;
    using Failure = winapk::launcher::StoreSetup::Failure;
    const App* app = store_app();
    const Step step = g_store_setup.step();
    const bool before = !app || (step == Step::kInstalling && !g_store_app.up());  // nothing to show yet
    std::wstring doing;
    if (before && step == Step::kChecking) doing = tr(L"Getting the Store…");
    else if (before && step == Step::kDownloading)
        doing = fmt(L"Downloading the Store… {0}%", {std::to_wstring((std::max)(0, g_store_setup.percent()))});
    else if (before && (step == Step::kWaiting || step == Step::kInstalling)) doing = tr(L"Installing the Store…");
    paint_embedded_title(g, g_store_app, before ? nullptr : app, doing);
    if (before && !doing.empty()) {
        const RectF& gr = g_l.grid;
        const float cy = gr.Y + (std::min)(gr.Height / 2, px(110));
        spinner(g, RectF(gr.X + gr.Width / 2 - px(20), cy - px(58), px(40), px(40)), g_t.accent, px(3.5f));
        paint_empty(g, nullptr, doing, tr(L"Win.apk's own store, from its page on GitHub. This happens once."));
        return;
    }
    if (!app && step == Step::kFailed) {
        const Failure f = g_store_setup.failure();
        paint_empty(g, kGlyphError, tr(L"The Store couldn't be set up"),
                    tr(f == Failure::kUnreached    ? L"GitHub could not be reached, or did not answer. Check the internet "
                                                     L"connection, then try again later."
                       : f == Failure::kNoRelease  ? L"GitHub lists no release of the Store that Win.apk can check."
                       : f == Failure::kUnverified ? L"The downloaded file is not the one GitHub lists. It was deleted "
                                                     L"and not installed."
                                                   : L"Installing it failed. Its log says why."));
        draw_button(g, g_l.panel_open, tr(L"Try again"), g_hot.kind == kHitRetry, true, false);
        if (f == Failure::kInstall) draw_button(g, g_l.panel_log, tr(L"Show log"), g_hot.kind == kHitPanelLog, false, false);
        return;
    }
    paint_embedded_end(g, g_store_app, kGlyphStore, L"The Store closed", L"The Store didn't start", L"Open the Store");
}

// The F-Droid catalog earlier versions kept in store\ (catalog.tsv, its icons, downloads): gone with the F-Droid
// store, removed once, in the background.
void remove_old_store_data() {
    std::error_code ec;
    if (!fs::exists(data_dir() / L"store", ec)) return;
    if (HANDLE t = CreateThread(nullptr, 0, [](void*) -> DWORD {
            std::error_code e;
            fs::remove_all(data_dir() / L"store", e);
            return 0;
        }, nullptr, 0, nullptr)) {
        CloseHandle(t);
    }
}

// ---- Software updates ----------------------------------------------------------
//
// The check and the download run on threads (update_check_thread, update_download_thread) and report to the
// library's window. The Updates section says where things stand, and the InfoBar when a newer version is out.
// Update downloads the release's installer, then runs it and closes the library, so that the installer can
// replace Win.apk.exe and the runtime; with an app running or an install under way it does not start.

// Where the check and the download stand (updates.h: their states and steps). The library's thread only.
using UpdateTracker = winapk::launcher::UpdateTracker<Release>;
UpdateTracker g_update;
size_t g_updates_section = 0;  // the Updates section's index in Settings, for the InfoBar's "What's new"
size_t g_account_section = 0;  // the Google account section's index in Settings, for the pane's account item

void show_settings();
void save_window_size();
bool installs_active();

DWORD unix_now() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    const unsigned long long t = (static_cast<unsigned long long>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return static_cast<DWORD>((t - kUnixEpochFiletime) / 10000000);
}

FILETIME unix_filetime(DWORD seconds) {
    const unsigned long long t = kUnixEpochFiletime + static_cast<unsigned long long>(seconds) * 10000000;
    return FILETIME{static_cast<DWORD>(t), static_cast<DWORD>(t >> 32)};
}

bool update_available() { return g_update.available(); }

// Installers that are not the newest release's: one that ran, or one left behind; and an installer's download
// that was cut off (`<setup>.exe.part`: the process ended under it), which http_get only renames when whole. Not
// while a download runs, whose `.part` is being written; releases.json's own `.part` is the check's.
void remove_old_setups() {
    if (g_update.downloading()) return;
    std::error_code ec;
    const fs::path keep = update_available() ? setup_file(g_update.latest().version) : fs::path();
    for (fs::directory_iterator it(updates_dir(), ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path p = it->path();
        const bool cut_off = p.extension() == L".part" && p.stem().extension() == L".exe";
        if ((p.extension() == L".exe" && p != keep) || cut_off) fs::remove(p, ec);
    }
}

// `fetch` asks GitHub; else only the answer kept from the last check is read (at start, within the day).
void start_update_check(bool fetch, bool manual) {
    if (!g_update.begin_check(fetch)) return;  // one runs already
    auto* c = new UpdateCheck;
    c->fetch = fetch;
    c->manual = manual;
    if (HANDLE t = CreateThread(nullptr, 0, update_check_thread, c, 0, nullptr)) {
        CloseHandle(t);
    } else {
        delete c;
        g_update.abort_check();
    }
}

bool update_check_due() {
    const DWORD last = setting(L"LastUpdateCheck", 0), now = unix_now();
    return now < last || now - last >= kUpdateInterval;
}

// When the library is shown and hourly while it is: the daily check if it is on and due, else (once) the last
// answer, so that Settings and the InfoBar know of a newer version found earlier.
void auto_update_check() {
    static bool read_kept = false;
    if (!g_main || !IsWindowVisible(g_main)) return;
    if (setting(L"AutoUpdate", 1) && update_check_due()) {
        start_update_check(true, false);
        if (store_app()) store_app_check();  // the Store's app is updated with Win.apk's own check (store_app.h)
    } else if (!read_kept) {
        start_update_check(false, false);
    }
    read_kept = true;
}

// The pane's account item: Settings, scrolled to the Google account (its sign-in row), as the
// Microsoft Store's profile button opens the account's own place.
void show_account() {
    show_settings();
    g_page.reveal = static_cast<int>(g_account_section);
}

// The InfoBar's "What's new": Settings, scrolled to Updates.
void show_update() {
    show_settings();
    g_page.reveal = static_cast<int>(g_updates_section);
}

void update_checked(UpdateCheck& c) {
    const bool newer = c.found && compare_versions(c.latest.version, version()) > 0;
    const UpdateTracker::Say say = g_update.checked(c, std::move(c.latest), newer, setting(L"AutoUpdate", 1) != 0);
    if (c.fetch && !g_update.fetch_failed()) set_setting(L"LastUpdateCheck", unix_now());
    if (c.ok && !g_update.downloading()) remove_old_setups();
    switch (say) {
        case UpdateTracker::Say::kNothing: break;
        case UpdateTracker::Say::kCouldNotCheck:
            info(tr(L"Couldn't check for updates. Check the internet connection."), kError);
            break;
        case UpdateTracker::Say::kUpToDate:
            info(fmt(L"Win.apk is up to date (version {0}).", {version()}), kSuccess);
            break;
        case UpdateTracker::Say::kNewVersion:
            info(fmt(L"A new version of Win.apk is available: {0}.", {g_update.latest().version}), kInfo,
                 tr(L"What's new"), show_update);
            break;
    }
    if (g_view == kSettingsView) rebuild_page();
}

// Whether the installer may run now, else says why in the InfoBar: it replaces the runtime that running apps
// use and the launcher that installs go through.
bool update_can_start() {
    if (installs_active() || any_pending()) {
        info(tr(L"Wait for the installs to finish, then update Win.apk."), kError);
        return false;
    }
    std::vector<std::wstring> running;
    for (const App& a : g_apps) {
        if (app_is_running(a.name)) running.push_back(a.label);
    }
    if (running.size() == 1) {
        info(fmt(L"Close {0} first: the update replaces files it uses.", {running[0]}), kError);
        return false;
    }
    if (!running.empty() || any_runs()) {
        info(tr(L"Close the running apps first: the update replaces files they use."), kError);
        return false;
    }
    return true;
}

// Runs the downloaded installer and closes the library, so that the installer can replace Win.apk.exe; false
// when it did not start.
bool run_installer() {
    if (!update_can_start()) return false;
    const fs::path setup = setup_file(g_update.latest().version);
    const auto r = reinterpret_cast<INT_PTR>(ShellExecuteW(g_main, L"open", setup.c_str(), nullptr, nullptr,
                                                           SW_SHOWNORMAL));
    if (r <= 32) {
        info(tr(L"The installer could not be started."), kError);
        return false;
    }
    save_window_size();
    g_page.build = nullptr;  // the Update button's page_click must not rebuild a page of a window that is gone
    g_quitting = true;       // the windows' WM_DESTROY below must not start a second end (maybe_quit)
    if (g_log_wnd) DestroyWindow(g_log_wnd);
    if (g_debug_wnd) DestroyWindow(g_debug_wnd);
    if (g_install_wnd) DestroyWindow(g_install_wnd);
    DestroyWindow(g_main);  // its WM_DESTROY ends the program
    return true;
}

void start_update() {
    if (g_update.downloading() || !update_available()) return;
    const Release& r = g_update.latest();
    if (r.url.empty()) {  // a release without the installer: its page has what it has
        ShellExecuteW(nullptr, L"open", (r.page.empty() ? kReleasesPage : r.page.c_str()), nullptr, nullptr,
                      SW_SHOWNORMAL);
        return;
    }
    if (!update_can_start()) return;
    std::error_code ec;
    // A copy downloaded before is checked again on the thread, as a new download is, before it runs.
    const bool kept = fs::exists(setup_file(r.version), ec);
    remove_old_setups();
    fs::create_directories(updates_dir(), ec);
    auto* job = new Release(r);
    if (HANDLE t = CreateThread(nullptr, 0, update_download_thread, job, 0, nullptr)) {
        CloseHandle(t);
        g_update.begin_download();
        info(kept ? fmt(L"Checking the Win.apk {0} installer…", {r.version})
                  : fmt(L"Downloading Win.apk {0}…", {r.version}),
             kInfo);
    } else {
        delete job;
    }
}

// The download thread is back: `result` is 0 when the download broke off, else updates.h's SetupVerdict + 1.
void update_downloaded(int result) {
    using winapk::launcher::SetupVerdict;
    g_update.downloaded();
    const auto verdict = static_cast<SetupVerdict>(result - 1);
    if (result > 0 && winapk::launcher::setup_runs(verdict) && run_installer()) return;  // the library is gone
    if (result == 0 || verdict == SetupVerdict::kSize) {
        info(tr(L"Couldn't download the update. Check the internet connection."), kError);
    } else if (!winapk::launcher::setup_runs(verdict)) {
        // Not the release's installer: it was deleted, and nothing ran.
        const wchar_t* why = verdict == SetupVerdict::kDigest
                                 ? L"The downloaded installer is not the one GitHub lists (its SHA-256 differs). It was "
                                   L"deleted and not run."
                             : verdict == SetupVerdict::kVersion
                                 ? L"The downloaded installer is not of the version offered. It was deleted and not "
                                   L"run."
                                 : L"The downloaded installer's signature does not hold. It was deleted and not run.";
        info(tr(why), kError);
    }
    if (g_view == kSettingsView) rebuild_page();
}

// The Update button shows how far the download is; changed in place, since a rebuild asks the runtime for the
// Google account.
void update_progress(int percent) {
    g_update.progress(percent);
    if (g_view != kSettingsView) return;
    for (Section& sec : g_page.sections) {
        for (Row& row : sec.rows) {
            if (row.key && strcmp(row.key, "update") == 0) {
                row.value = fmt(L"Downloading… {0}%", {std::to_wstring(percent)});
            }
        }
    }
    InvalidateRect(g_main, nullptr, FALSE);
}

// Settings > Updates, and when a newer version is out, what changed in it.
std::vector<Section> update_sections() {
    std::vector<Section> out;
    const bool newer = update_available();
    const Release& r = g_update.latest();
    std::wstring status;
    if (g_update.asking()) status = tr(L"Checking for updates…");
    else if (newer) status = fmt(L"Version {0} is available.", {r.version});
    else if (g_update.fetch_failed()) status = tr(L"Couldn't check for updates. Check the internet connection.");
    else if (const DWORD last = setting(L"LastUpdateCheck", 0)) {
        status = g_update.known() != UpdateTracker::Known::kNothing ? fmt(L"Win.apk is up to date. Last checked {0}.", {long_date(unix_filetime(last))})
                               : fmt(L"Last checked {0}.", {long_date(unix_filetime(last))});
    } else {
        status = tr(L"Not checked yet.");
    }
    Section sec{tr(L"Updates"), {}, L""};
    sec.rows.push_back(button_row(fmt(L"Version {0}", {version()}), status,
                                  g_update.asking() ? tr(L"Checking…") : tr(L"Check"),
                                  [] {
                                      start_update_check(true, true);
                                      if (store_app()) store_app_check();  // the Store's app too, as the daily check
                                  }));
    Row autoc;
    autoc.kind = Row::kToggle;
    autoc.title = tr(L"Check for updates automatically");
    autoc.detail = tr(L"Once a day, while the library is open.");
    autoc.on = setting(L"AutoUpdate", 1) != 0;
    autoc.act = [](int on) {
        set_setting(L"AutoUpdate", static_cast<DWORD>(on));
        if (on && update_check_due()) start_update_check(true, false);
    };
    sec.rows.push_back(autoc);
    if (newer) {
        const std::wstring button = r.url.empty()           ? tr(L"Open")
                                    : g_update.downloading() ? fmt(L"Downloading… {0}%",
                                                                   {std::to_wstring(g_update.percent())})
                                                             : tr(L"Update");
        Row up = button_row(fmt(L"Win.apk {0}", {r.version}),
                            r.unchecked     ? tr(L"GitHub gives no SHA-256 checksum of this release's installer, so "
                                                 L"Win.apk does not download it; its page on GitHub has its files.")
                            : r.url.empty() ? tr(L"This release has no installer; its page on GitHub has its files.")
                                            : tr(L"Win.apk closes and the installer replaces it. Your apps and their "
                                                 L"data are kept."),
                            button, [] { start_update(); });
        up.primary = true;
        up.key = "update";
        sec.rows.push_back(up);
    }
    sec.footer = tr(L"The check reads the list of releases on GitHub. Nothing about you or your apps is sent.");
    // Said as it is: no Win.apk build is code-signed yet, so the installer is held to GitHub's checksum.
    if (own_signer().empty()) {
        sec.footer += std::wstring(L" ") + tr(L"Win.apk's installers are not code-signed: an update runs only when its size, its "
                                L"SHA-256 checksum and its version are the ones GitHub lists for the release.");
    }
    out.push_back(std::move(sec));
    if (newer) {
        const bool dated = r.published.dwHighDateTime || r.published.dwLowDateTime;
        const std::wstring page = r.page.empty() ? kReleasesPage : r.page;
        out.push_back({fmt(L"What's new in {0}", {r.version}),
                       {text_row(dated ? fmt(L"Released {0}", {long_date(r.published)}) : tr(L"Release notes"), L"",
                                 r.notes.empty() ? tr(L"This release has no notes.") : r.notes),
                        button_row(tr(L"Release page"), L"github.com/israel2176/win.apk/releases", tr(L"Open"),
                                   [page] {
                                       ShellExecuteW(nullptr, L"open", page.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                                   })},
                       L""});
    }
    return out;
}

std::vector<Section> settings_sections() {
    std::vector<Section> out;

    Row theme;
    theme.kind = Row::kChoice;
    theme.title = tr(L"Theme");
    theme.choices = {tr(L"Follow Windows"), tr(L"Light"), tr(L"Dark")};  // the setting's 0, 1, 2
    theme.choice = static_cast<int>(winapk::launcher::settings::theme(setting(L"Theme", 0)));
    theme.act = [](int i) {
        set_setting(L"Theme", static_cast<DWORD>(i));
        apply_theme();
    };
    Row lang;
    lang.kind = Row::kChoice;
    lang.title = tr(L"Language");
    // Listed Hebrew first; the setting keeps its values (0 as Windows, 1 English, 2 Hebrew).
    lang.choices = {tr(L"Follow Windows"), L"עברית", L"English"};
    using winapk::launcher::settings::Language;
    const Language language = winapk::launcher::settings::language(setting(L"Language", 0));
    lang.choice = language == Language::kHebrew ? 1 : language == Language::kEnglish ? 2 : 0;
    lang.act = [](int i) {
        set_setting(L"Language", static_cast<DWORD>(i == 1 ? 2 : i == 2 ? 1 : 0));
        apply_language();
    };
    out.push_back({tr(L"Appearance"), {theme, lang}, L""});

    Row size;
    size.kind = Row::kChoice;
    size.title = tr(L"App window size");
    size.detail = tr(L"The size apps open at. An app whose window you resized opens at its saved size.");
    for (const auto& p : kPhonePresets) size.choices.push_back(tr(p.label));
    size.choice =
        static_cast<int>(winapk::launcher::settings::choice(setting(L"PhoneSize", 0), std::size(kPhonePresets)));
    size.act = [](int i) { set_setting(L"PhoneSize", static_cast<DWORD>(i)); };
    Row orient;
    orient.kind = Row::kChoice;
    orient.title = tr(L"Default orientation");
    orient.detail = tr(L"Apps that require an orientation always open in it. You can change it per app on its "
                       L"details page.");
    orient.choices = {tr(L"Portrait"), tr(L"Landscape")};
    orient.choice = landscape_default() ? 1 : 0;
    orient.act = [](int i) { set_setting(L"Landscape", static_cast<DWORD>(i)); };
    // Read by the runtime when an app opens (runtime_main.cpp, map_shared_folders).
    Row folders;
    folders.kind = Row::kToggle;
    folders.title = tr(L"Use my Windows folders");
    folders.detail = tr(L"Apps save to and open from your Downloads, Pictures, Documents, Music and Videos.");
    folders.on = setting(L"UserFolders", 1) != 0;
    folders.act = [](int on) { set_setting(L"UserFolders", static_cast<DWORD>(on)); };
    // Read by the runtime and its background translation (runtime_main.cpp, aot_background_on).
    Row aot;
    aot.kind = Row::kToggle;
    aot.title = tr(L"Speed up ARM apps");
    aot.detail = tr(L"Translates ARM code to your PC's code in the background so apps run faster. Runs at low "
                    L"priority.");
    aot.on = setting(L"AotBackground", 1) != 0;
    aot.act = [](int on) { set_setting(L"AotBackground", static_cast<DWORD>(on)); };
    // Read by the runtime when an app opens (runtime_main.cpp, play_installer_setting -> WINAPK_PLAY_INSTALLER,
    // InstallerOfRecord.reported): apps such as bit refuse to run unless a store installed them.
    Row play;
    play.kind = Row::kToggle;
    play.title = tr(L"Report Google Play as the installer");
    play.detail = tr(L"Apps you install appear to come from Google Play, as some banking apps require.");
    play.on = setting(L"PlayInstaller", 1) != 0;
    play.act = [](int on) { set_setting(L"PlayInstaller", static_cast<DWORD>(on)); };
    Row desktop;
    desktop.kind = Row::kToggle;
    desktop.title = tr(L"Desktop shortcut for new apps");
    desktop.detail = tr(L"Every app also gets one in Start > Win.apk Apps.");
    desktop.on = setting(L"DesktopShortcut", 1) != 0;
    desktop.act = [](int on) { set_setting(L"DesktopShortcut", static_cast<DWORD>(on)); };
    Row types;
    if (is_default_handler()) {
        types = text_row(tr(L"Open .apk, .apkm, .xapk and .apks files with Win.apk"), tr(L"Default"));
        types.check = true;
    } else {
        types = button_row(tr(L"Open .apk, .apkm, .xapk and .apks files with Win.apk"), L"", tr(L"Make default"),
                           [] { register_file_types(true); });
    }
    out.push_back({tr(L"Apps"), {size, orient, folders, aot, play, desktop, types}, L""});

    g_account_section = out.size();
    out.push_back(google_account_section());

    std::vector<Row> trouble = {
        button_row(tr(L"Logs"),
                   tr(L"Every app's install and run logs, to attach to a bug report. Each app's own is on its "
                      L"details page."),
                   tr(L"Open folder"), [] {
                       std::error_code ec;
                       fs::create_directories(logs_dir(), ec);
                       open_path(logs_dir());
                   }),
        button_row(tr(L"App data"), tr(L"The files and settings each app has saved."), tr(L"Open folder"),
                   [] { open_path(data_dir()); })};
    // The library's own apps; Google's system components stay with Google services.
    std::vector<App> own;
    for (const App& a : g_apps)
        if (!system_app(a.name)) own.push_back(a);
    if (!own.empty()) {
        trouble.push_back(button_row(tr(L"Clear library"),
                                     tr(L"Uninstalls every app and removes its shortcuts. App data is kept."),
                                     tr(L"Clear…"), [own] {
                                         if (!confirm(tr(L"Clear the library?"),
                                                      fmt(L"All {0} apps and their shortcuts will be removed. App "
                                                          L"data is kept.",
                                                          {std::to_wstring(own.size())}),
                                                      tr(L"Clear library"))) {
                                             return;
                                         }
                                         std::vector<std::wstring> kept;  // still running, or a file in use
                                         for (const App& a : own) {
                                             if (!remove_app(a, false)) kept.push_back(a.label);
                                         }
                                         refresh();
                                         if (kept.empty()) {
                                             info(tr(L"The library was cleared."), kInfo);
                                         } else if (kept.size() == 1) {
                                             info(fmt(L"The library was cleared, except {0}. Close it, then try "
                                                      L"again.",
                                                      {kept[0]}),
                                                  kError);
                                         } else {
                                             std::wstring names;
                                             for (const std::wstring& k : kept)
                                                 names += (names.empty() ? L"" : L", ") + k;
                                             info(fmt(L"The library was cleared, except {0} apps: {1}. Close them, "
                                                      L"then try again.",
                                                      {std::to_wstring(kept.size()), names}),
                                                  kError);
                                         }
                                     },
                                     true));
    }
    // Read by the runtime when an app opens (host/debug_agent.cpp, debug_mode_on).
    Row debug;
    debug.kind = Row::kToggle;
    debug.title = tr(L"Debug mode");
    debug.detail = tr(L"For developers. Apps opened from now on report every thread they have, with its Java and "
                      L"native stack.");
    debug.on = debug_mode();
    debug.act = [](int on) {
        set_setting(L"DebugMode", static_cast<DWORD>(on));
        if (!on && g_debug_wnd) DestroyWindow(g_debug_wnd);
    };  // the page is built again after it: the Threads row comes and goes with the switch
    trouble.push_back(debug);
    if (debug_mode()) {
        trouble.push_back(button_row(tr(L"Threads"),
                                     tr(L"Every thread of the running apps, live. Each app's own is on its details "
                                        L"page."),
                                     tr(L"Open"), [] { show_debugger({}); }));
    }
    out.push_back({tr(L"Troubleshooting"), std::move(trouble), L""});

    g_updates_section = out.size();
    for (Section& sec : update_sections()) out.push_back(std::move(sec));

    auto open_url = [](const wchar_t* url) {
        return [url] { ShellExecuteW(nullptr, L"open", url, nullptr, nullptr, SW_SHOWNORMAL); };
    };
    // Credits and licenses: THIRD_PARTY_NOTICES.txt, packaged next to README.txt and LICENSE.txt, which credits
    // Mac.apk (the AGPL's attribution) and every project the package ships; the repository's copy when this is
    // a build without one beside it.
    out.push_back({tr(L"About"),
                   {text_row(L"Win.apk", fmt(L"Version {0}", {version()}), tr(L"Android apps on Windows.")),
                    button_row(tr(L"Project page"), L"github.com/israel2176/win.apk", tr(L"Open"),
                               open_url(L"https://github.com/israel2176/win.apk")),
                    button_row(tr(L"Credits and licenses"),
                               tr(L"Mac.apk, which Win.apk is built on, the other projects it uses, and their "
                                  L"licenses."),
                               tr(L"View"), [] {
                                   std::error_code ec;
                                   const fs::path notices = root() / L"THIRD_PARTY_NOTICES.txt";
                                   if (fs::exists(notices, ec)) open_path(notices);
                                   else ShellExecuteW(nullptr, L"open",
                                                      L"https://github.com/israel2176/win.apk/blob/main/"
                                                      L"THIRD_PARTY_NOTICES.txt",
                                                      nullptr, nullptr, SW_SHOWNORMAL);
                               })},
                   L""});
    return out;
}

void show_settings() {
    Page p;
    p.title = tr(L"Settings");
    p.build = settings_sections;
    open_page(kSettingsView, std::move(p));
}

std::vector<Row> permission_rows(const App& a);

fs::path desktop_shortcut(const App& a) {
    return known_folder(FOLDERID_Desktop) / (file_name_safe(a.label) + L".lnk");
}

// ---- The app's arm64 translation (runtime_main.cpp, "arm64 translation, in the background") ----
//
// The installed app's arm64 libraries: lib\arm64 next to the base.apk that the install's record
// (android\data\app\.<name>.installed, its path in UTF-8) names. Empty when it has none.
fs::path arm64_libs_of(const std::wstring& name) {
    const fs::path copy = installed_copy(name);
    if (copy.empty()) return {};
    const fs::path libs = copy / L"lib" / L"arm64";
    std::error_code ec;
    return fs::is_directory(libs, ec) ? libs : fs::path();
}

struct AotState {
    bool running = false;            // a worker holds the app's mutex
    size_t done = 0, parts = 0;      // its pass: parts compiled, of those winapk-aot has written
    bool large_arm_only = false;     // only arm64 code, and a library too large to translate whole
};

AotState aot_state(const std::wstring& name) {
    AotState st;
    const fs::path libs = arm64_libs_of(name);
    std::error_code ec;
    if (libs.empty()) return st;
    // The worker's mutex, named as runtime_main.cpp's aot_mutex_name does: keep the two the same.
    std::wstring key = libs.lexically_normal().wstring();
    for (wchar_t& ch : key) ch = static_cast<wchar_t>(towlower(ch == L'/' ? L'\\' : ch));
    uint64_t h = 1469598103934665603ull;
    for (const wchar_t ch : key) h = (h ^ static_cast<uint16_t>(ch)) * 1099511628211ull;
    wchar_t mutex[48];
    swprintf(mutex, 48, L"Local\\WinApk-aot-%016llx", static_cast<unsigned long long>(h));
    if (HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, mutex)) {
        st.running = true;
        CloseHandle(m);
        // .aot-tmp\<lib>\: the C parts (<lib>.c, <lib>.c.<n>.c) and the objects compiled from them (p-<hash>.o).
        for (const auto& dir : fs::directory_iterator(libs / L".aot-tmp", ec)) {
            for (const auto& e : fs::directory_iterator(dir.path(), ec)) {
                const std::wstring ext = e.path().extension().wstring();
                if (ext == L".c") ++st.parts;
                else if (ext == L".o") ++st.done;
            }
        }
    }
    // A stamp that says "by its profile" (aot_stamp.h: kProfile, or kWait not after a failure) is a library over
    // the runtime's kAotFullBelow, translated only where the app's code ran.
    if (!fs::exists(libs.parent_path() / L"x86_64", ec)) {
        for (const auto& e : fs::directory_iterator(libs, ec)) {
            if (e.path().extension() != L".stamp") continue;
            // read_tail shares the file for delete: the worker replaces a stamp by renaming a new one over it.
            if (winapk::arm64::aot_stamp_by_profile(winapk::arm64::aot_parse_stamp(read_tail(e.path(), 256)))) {
                st.large_arm_only = true;
            }
        }
    }
    return st;
}

// An app's details: how it opens, its permissions, its shortcut and files, and Stop, Update, Remove.
void show_info(const App& app) {
    const App a = app;
    std::error_code ec;
    // A bundle's app is its .apk and the splits beside it (library_files.h): hard links to the installed APKs,
    // so this is what the app takes on disk, once.
    const fs::path splits = library_dir() / (a.name + L".splits");
    const bool has_splits = fs::is_directory(splits, ec);
    const std::wstring apk_size = format_bytes(fs::file_size(a.apk, ec) + (has_splits ? folder_size(splits) : 0));
    const std::wstring files_size = format_bytes(folder_size(app_dir(a.name)));
    const std::vector<std::string> pkg = package_of(a.name);
    Page p;
    p.app = a.name;
    p.title = a.label;
    if (pkg.size() > 2 && !pkg[2].empty()) p.sub = fmt(L"Version {0}", {utf8_to_wide(pkg[2])}) + L"  ·  ";
    p.sub += pkg.empty() ? a.name : utf8_to_wide(pkg[0]);
    p.back = true;
    p.from = kLibraryView;
    p.build = [a, apk_size, has_splits, files_size] {
        std::vector<Section> out;
        // The orientation the app opens in: what its manifest fixes, or the user's choice for it over the settings'.
        std::vector<Row> window;
        if (const int fixed = manifest_orientation(a.name); fixed != kOrientUnset) {
            window.push_back(text_row(tr(L"Orientation"), tr(fixed == kLandscape ? L"Landscape – required by the app"
                                                                                 : L"Portrait – required by the app")));
        } else {
            Row orient;
            orient.kind = Row::kChoice;
            orient.title = tr(L"Orientation");
            orient.detail = tr(landscape_default() ? L"The settings' default is landscape."
                                                   : L"The settings' default is portrait.");
            orient.choices = {tr(L"Settings' default"), tr(L"Portrait"), tr(L"Landscape")};
            orient.choice = app_orientation(a.name) == kOrientUnset ? 0 : app_orientation(a.name) == kLandscape ? 2 : 1;
            orient.act = [a](int i) { set_orientation(a, i == 0 ? kOrientUnset : i == 2 ? kLandscape : kPortrait); };
            window.push_back(orient);
        }
        // The size the user last gave the app's window (the runtime keeps it as library\<name>.window, in dp).
        if (std::ifstream f(library_dir() / (a.name + L".window")); f) {
            int w = 0, h = 0;
            char x = 0;
            if (f >> w >> x >> h && w > 0 && h > 0) {
                window.push_back(button_row(fmt(L"Window size  ·  {0} × {1} dp", {std::to_wstring(w), std::to_wstring(h)}),
                                            tr(L"The size you last gave the app's window. After a reset, it opens at the "
                                               L"app window size in Settings again."),
                                            tr(L"Reset"), [a] {
                                                std::error_code ec;
                                                fs::remove(library_dir() / (a.name + L".window"), ec);
                                                info(fmt(L"{0} opens at the app window size in Settings again.", {a.label}),
                                                     kSuccess);
                                            }));
            }
        }
        // An app with no launcher activity: the activity it opens (choose_activity).
        if (const std::vector<std::string> lines = package_of(a.name); has_no_launcher(lines)) {
            const std::vector<StartableActivity> list = startable_activities(lines);
            const std::wstring cls = chosen_activity(a.name, list);
            std::wstring shown = tr(L"Asked when it opens");
            for (const StartableActivity& s : list) {
                if (s.cls == cls) shown = activity_title(s);
            }
            if (!list.empty()) {
                window.insert(window.begin(),
                              button_row(std::wstring(tr(L"Screen it opens")) + L"  ·  " + shown,
                                         tr(L"The app has no screen to open from a launcher, so it opens this one of the "
                                            L"screens other apps can open."),
                                         tr(L"Change…"), [a, list] {
                                             std::wstring chosen;
                                             if (!choose_activity(a, list, &chosen)) return;
                                             for (const StartableActivity& s : list) {
                                                 if (s.cls != chosen) continue;
                                                 info(fmt(L"{0} opens {1} from now on.", {a.label, activity_title(s)}),
                                                      kSuccess);
                                             }
                                             rebuild_page();
                                         }));
            }
        }
        out.push_back({tr(L"Window"), std::move(window), L""});
        if (std::vector<Row> perms = permission_rows(a); !perms.empty())
            out.push_back({tr(L"Permissions"), std::move(perms), L""});
        Row desktop;
        desktop.kind = Row::kToggle;
        desktop.title = tr(L"Desktop shortcut");
        desktop.detail = tr(L"The app is in Start > Win.apk Apps either way.");
        std::error_code ec;
        desktop.on = fs::exists(desktop_shortcut(a), ec);
        desktop.act = [a](int on) {
            if (on) {
                make_app_shortcuts(a, true);
            } else {
                std::error_code ec;
                fs::remove(desktop_shortcut(a), ec);
            }
        };
        out.push_back({tr(L"Shortcut"), {desktop}, L""});
        // Its arm64 code's translation, while a worker makes it, and what an x86_64 build would spare.
        {
            const AotState st = aot_state(a.name);
            std::vector<Row> perf;
            if (st.running) {
                perf.push_back(text_row(tr(L"Optimizing for this PC…"),
                                        st.parts > 0 ? fmt(L"{0} of {1} parts", {std::to_wstring((std::min)(st.done,
                                                                                    st.parts)),
                                                                                std::to_wstring(st.parts)})
                                                     : L"",
                                        tr(L"The app's ARM code is being translated for this processor, at the "
                                           L"lowest priority. Until then the app runs it more slowly.")));
            }
            if (st.large_arm_only) {
                perf.push_back(text_row(tr(L"This app has only ARM code"), L"",
                                        tr(L"It runs translated, which uses more CPU. An x86_64 version of the app, "
                                           L"if one exists, runs faster.")));
            }
            if (!perf.empty()) out.push_back({tr(L"Performance"), std::move(perf), L""});
        }
        // The app's own log: its last run, else its install (the log window's picker has the rest); and a
        // report for a problem met inside the app, which the splash's and the install's errors cannot see.
        {
            std::error_code ec2;
            const fs::path log = fs::exists(run_log(a.name), ec2) ? run_log(a.name) : install_log(a.name);
            std::vector<Row> trouble = {
                button_row(tr(L"Log"), tr(L"View the app's recent logs"), tr(L"Show log"), [log] { show_logs(log); }),
                button_row(tr(L"Report a problem"), tr(L"Opens a GitHub issue with the app's details and log"),
                           tr(L"Report"), [a, log] { report_problem(g_main, a.name, a.label, log, {}); })};
            if (debug_mode()) {
                trouble.push_back(button_row(tr(L"Threads"),
                                             tr(L"Every thread of the app's processes, live, with its stack"),
                                             tr(L"Open"), [a] { show_debugger(a.name); }));
            }
            out.push_back({tr(L"Troubleshooting"), std::move(trouble), L""});
        }
        out.push_back({tr(L"Storage"),
                       {button_row(fmt(has_splits ? L"The app's files  ·  {0}" : L"The app's file  ·  {0}",
                                       {apk_size}),
                                   a.apk.wstring(), tr(L"Show"),
                                   [a] { show_in_explorer(a.apk); }),
                        button_row(fmt(L"Run files  ·  {0}", {files_size}),
                                   fmt(L"What Win.apk prepared to run the app: {0}",  // a path reads left to right
                                       {g_rtl ? L"\x200E" + app_dir(a.name).wstring() + L"\x200E"
                                              : app_dir(a.name).wstring()}),
                                   tr(L"Open"), [a] { open_path(app_dir(a.name)); })},
                       L""});
        std::vector<Row> manage;
        if (app_is_running(a.name)) {  // the page is built again when that changes (refresh_running)
            manage.push_back(button_row(tr(L"Stop"), tr(L"Closes the app at once, as Force stop does on a phone."),
                                        tr(L"Stop"), [a] { force_stop(a); }));
        }
        // No "Update from a file": opening or dropping a newer file of the app already installs over it.
        manage.push_back(button_row(tr(L"Remove"), tr(L"Removes the app and its shortcuts. You can also remove what "
                                                     L"it saved."),
                                    tr(L"Remove…"),
                                    [a] { remove_app(a, true); }, true));
        out.push_back({tr(L"Manage"), std::move(manage), L""});
        return out;
    };
    open_page(kAppView, std::move(p));
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


// A window of its own: over the library when it is shown, and owned by it (no taskbar button of
// its own); else in the middle of the screen with the mouse, with a taskbar button.
HWND open_tool_window(const wchar_t* cls, const wchar_t* title, int w, int h,
                      DWORD style = WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX) {
    const bool over_library = g_main && IsWindowVisible(g_main) && !IsIconic(g_main);
    RECT mr;
    if (over_library) {
        GetWindowRect(g_main, &mr);
    } else {
        POINT cursor;
        GetCursorPos(&cursor);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY), &mi);
        mr = mi.rcWork;
    }
    const int x = mr.left + (mr.right - mr.left - w) / 2, y = mr.top + (std::max)(0L, (mr.bottom - mr.top - h) / 2);
    HWND hwnd = CreateWindowExW(layout_style(), cls, title, style, x, y, w, h, over_library ? g_main : nullptr,
                                nullptr, g_inst, nullptr);
    if (hwnd) {
        theme_window(hwnd);
        ShowWindow(hwnd, SW_SHOW);
        SetForegroundWindow(hwnd);
    }
    return hwnd;
}

// ---------------------------------------------------------------------------
// The install window: a small card with the app's icon and name, a moving bar
// and one line of plain status; the queue under it when several files came;
// the log behind "Show details". At the end: Open, Close, Show in library; on
// a failure the reason, Show log and Copy details. Opening an APK from
// Explorer shows only this window (wWinMain).

struct InstallView {
    Targets targets;
    int hot = 0;
    bool tracking = false;
    bool details = false;          // the log is shown: Show details, for this window only
    std::wstring log_name;         // whose log the details show; empty: the one being installed
    std::vector<std::wstring> log;
    ULONGLONG log_read = 0;
    float need_h = 0;              // the height the last paint wanted, for fit_install_window
} g_iv;

// This process is the install helper, installing for an app that asked (a store): the window is Android's
// package installer after its confirmation (InstallInstalling, then InstallSuccess or InstallFailed). The
// same card, in that installer's words: "App installed." with Open and Done, or "App not installed." with
// the reason; no batch, no library (helper_install_ui).
bool g_helper_ui = false;
void helper_install_ended(const std::wstring& name);
void helper_progress(double fraction, const char* stage);

enum { kActClose = 1, kActCancel, kActOpen, kActDetails, kActCopy, kActShowLog, kActLibrary, kActReport,
       kActRowOpen = 1000, kActRowLog = 2000 };
constexpr UINT kFitInstall = WM_APP + 20;

// How far an install got, from the lines the runtime writes, in the order it works (runtime_main.cpp):
// 0 starting, 1 installing, 2 reading the name and icon, 3 converting the code, the long part (which starts
// beside the name and icon, so 2 shows only until the conversion has said so).
int install_phase(const fs::path& log) {
    const std::string tail = read_tail(log, 128 * 1024);
    if (tail.find("converting the app's code") != std::string::npos) return 3;
    if (tail.find("host natives:") != std::string::npos) return 2;
    if (tail.find("installing the app (once per install)") != std::string::npos) return 1;
    return 0;
}

const wchar_t* phase_text(int phase) {
    switch (phase) {
        case 1: return L"Installing…";
        case 2: return L"Getting the app's name and icon…";
        case 3: return L"Converting the app's code. This happens once.";
        default: return L"Preparing…";
    }
}

std::wstring install_failure_reason(const fs::path& log) {
    bool no_native_code = false;
    log_summary(log, &no_native_code);
    if (no_native_code) {
        return tr(L"This file has no code for PCs. Install the full bundle (.apkm, .xapk or .apks) that includes "
                  L"the arm64-v8a part.");
    }
    // AppInstall's INSTALL_FAILED_NO_MATCHING_ABIS: code only for processors this PC is not, as a device refuses it.
    const std::string abi_tail = read_tail(log, 16384);
    if (const size_t at = abi_tail.find("INSTALL_FAILED_NO_MATCHING_ABIS"); at != std::string::npos) {
        const size_t only = abi_tail.find("only for ", at), end = abi_tail.find(';', only);
        const std::string list = only == std::string::npos || end == std::string::npos
                                     ? std::string() : abi_tail.substr(only + 9, end - only - 9);
        if (list.find("x86") == std::string::npos && list.find("arm64") == std::string::npos &&
            list.find("armeabi") != std::string::npos)
            return tr(L"This app is built only for 32-bit ARM, which Win.apk cannot run. Download its x86_64 or "
                      L"arm64-v8a build.");
        return fmt(L"This app has code only for {0}, which Win.apk cannot run. Download its x86_64 or arm64-v8a build.",
                   {utf8_to_wide(list)});
    }
    std::error_code ec;
    if (!fs::exists(runtime_exe(), ec)) return tr(L"Win.apk's runtime is missing. Install Win.apk again.");
    if (std::wstring why = runtime_failure_reason(log); !why.empty()) return why;
    // AppInstall's refusals, Android's own (PackageInstallerSession): the file is a part of an app, not all of it.
    const std::string refused = read_tail(log, 16 * 1024);
    if (refused.find("Full install must include a base package") != std::string::npos)
        return tr(L"This file is only a part of an app. Install the app's whole bundle (.apkm, .xapk or .apks), or "
                  L"all of its .apk files together.");
    if (refused.find("INSTALL_FAILED_MISSING_SPLIT") != std::string::npos)
        return tr(L"This app comes in parts, and some are missing. Install the app's whole bundle (.apkm, .xapk or "
                  L".apks), or all of its .apk files together.");
    if (refused.find("[install] INSTALL_FAILED_INVALID_APK") != std::string::npos ||
        refused.find("[install] INSTALL_PARSE_FAILED") != std::string::npos)
        return tr(L"These files are not the parts of one version of one app.");
    // The launcher's own lines (launcher_error_line): the file did not reach the library.
    const std::string tail = read_tail(log, 4096);
    if (tail.find("[E] launcher: could not ") != std::string::npos)
        return tr(L"Win.apk could not copy this file into its library. Check that the disk has free space, then try "
                  L"again.");
    return tr(L"Win.apk could not convert this app. See the log for details.");
}

// The name to show for an install: the app's own once known, else the file's.
std::wstring job_title(const Job& j) {
    if (find_app(j.name) || g_job_labels.count(j.name)) return job_label(j.name);
    if (!j.label.empty()) return j.label;
    return j.file.empty() ? j.name : j.file;
}

Gdiplus::Bitmap* job_icon(Job& j) {
    if (j.icon) return j.icon.get();
    if (const App* a = find_app(j.name)) return icon_for(*a);
    return nullptr;
}

// Whether the installed app has an Open: not one of Google's (a part of Google services), and for an app's
// install not one with no launcher activity, as InstallSuccess offers Open only for an app with a launch
// intent.
bool job_opens(const Job& j) {
    if (system_app(j.name)) return false;
    return !g_helper_ui || !has_no_launcher(package_of(j.name));
}

bool installs_active() {
    for (const Job& j : g_jobs)
        if (j.state == Job::kWaiting || j.state == Job::kInstalling) return true;
    return false;
}

// The job the card is about: the one being installed, else the only one, else none (a summary).
Job* hero_job() {
    for (Job& j : g_jobs)
        if (j.state == Job::kInstalling) return &j;
    for (Job& j : g_jobs)
        if (j.state == Job::kWaiting) return &j;
    return g_jobs.size() == 1 ? &g_jobs[0] : nullptr;
}

// An app's icon at `r`; a neutral tile with a package glyph while it has none.
void draw_job_icon(Graphics& g, Job& j, const RectF& r) {
    if (Gdiplus::Bitmap* bmp = job_icon(j)) {
        draw_icon(g, bmp, job_title(j), r);
        return;
    }
    fill_round(g, r, r.Width * 0.22f, g_t.hover);
    draw_text(g, kGlyphPackage, r.Width >= px(56) ? g_f.glyph24.get() : g_f.glyph16.get(), g_t.text3, r, kCenter);
}

void draw_badge(Graphics& g, const RectF& icon, bool ok, float size) {
    const RectF b(icon.GetRight() - size * 0.75f, icon.GetBottom() - size * 0.75f, size, size);
    Gdiplus::SolidBrush ring(g_t.layer), dot(ok ? g_t.success : g_t.danger);
    g.FillEllipse(&ring, inset(b, -px(2), -px(2)));
    g.FillEllipse(&dot, b);
    if (ok) draw_text(g, kGlyphCheck, g_f.glyph12.get(), kWhite, b, kCenter);
    else draw_text(g, L"!", g_f.body_semi.get(), kWhite, b, kCenter);
}

// A text link: accent text with a chevron, for the window's secondary ways on.
float draw_link(Graphics& g, const std::wstring& text, float x, float y, float h, bool hot, int act, Align align = kLeft) {
    const float w = text_width(g, text, g_f.body.get()) + px(4);
    const RectF r(align == kRight ? x - w : x, y, w, h);
    draw_text(g, text, g_f.body.get(), hot ? mix(g_t.accent, g_t.text, 0.3f) : g_t.accent, r);
    g_iv.targets.push_back({r, act});
    return r.Width;
}

void paint_install(Graphics& g, float W, float H) {
    g.Clear(g_t.layer);  // as the dialog: the layer, and a footer band of the window colour
    g_iv.targets.clear();
    const float m = px(24), foot = px(64), cw = W - 2 * m;
    int done = 0, failed = 0, cancelled_n = 0, shown_done = 0;  // shown_done: in the library (not Google's)
    for (const Job& j : g_jobs) {
        done += j.state == Job::kDone;
        shown_done += j.state == Job::kDone && !system_app(j.name);
        failed += j.state == Job::kFailed;
        cancelled_n += j.state == Job::kCancelled;
    }
    const int total = static_cast<int>(g_jobs.size());
    const bool active = installs_active();
    Job* hero = hero_job();

    // The card: the job's icon, name and status, or the summary of a finished batch.
    float y = px(20);
    const RectF ir(m, y, px(64), px(64));
    const float tx = m + px(80), tw = cw - px(80);
    std::wstring title, sub;
    Color sub_ink = g_t.text2;
    if (hero) {
        draw_job_icon(g, *hero, ir);
        const std::wstring name = job_title(*hero);
        switch (hero->state) {
            case Job::kWaiting:
            case Job::kInstalling:
                title = name;
                sub = tr(phase_text(hero->phase));
                break;
            case Job::kDone:
                if (g_helper_ui) {  // InstallSuccess: the app's name over "App installed."
                    title = name;
                    sub = tr(hero->update ? L"App updated." : L"App installed.");
                } else {
                    title = fmt(L"{0} is ready", {name});
                    const std::vector<std::string> pkg = package_of(hero->name);
                    sub = tr(!pkg.empty() && pkg[0] == winapk::launcher::kStorePackage ? L"It's part of Win.apk."
                             : system_app(hero->name)                ? L"It's part of Google services."
                             : setting(L"DesktopShortcut", 1) != 0 ? L"It's in the Start menu and on the desktop."
                                                                   : L"It's in the Start menu.");
                }
                draw_badge(g, ir, true, px(24));
                break;
            case Job::kFailed:
                if (g_helper_ui) {  // InstallFailed: "App not installed." and why
                    title = name;
                    sub = std::wstring(tr(L"App not installed.")) + L" " + hero->reason;
                } else {
                    title = fmt(L"Couldn't install {0}", {name});
                    sub = hero->reason;
                }
                draw_badge(g, ir, false, px(24));
                break;
            case Job::kCancelled:
                title = g_helper_ui ? name : fmt(L"{0} was not installed", {name});
                sub = tr(hero->update ? L"The install was cancelled. The app stays as it was."
                                      : L"The install was cancelled.");
                break;
        }
    } else {
        const bool ok = failed == 0;
        fill_round(g, ir, px(32), with_alpha(ok ? g_t.success : g_t.danger, g_t.dark ? 60 : 30));
        draw_text(g, ok ? kGlyphCheck : L"!", ok ? g_f.glyph24.get() : g_f.title.get(), ok ? g_t.success : g_t.danger,
                  ir, kCenter);
        title = failed == 0 ? fmt(L"{0} apps are ready", {std::to_wstring(done)})
                            : fmt(L"{0} of {1} apps are installed", {std::to_wstring(done), std::to_wstring(total)});
        sub = tr(failed == 0 ? L"They're in the Start menu." : L"The reason for each failure is shown below.");
    }
    draw_text(g, title, g_f.title.get(), g_t.text, RectF(tx, y + px(2), tw, px(28)));
    const float sh = (std::max)(px(18), text_height(g, sub, g_f.caption.get(), tw) + px(2));
    draw_text(g, sub, g_f.caption.get(), sub_ink, RectF(tx, y + px(32), tw, sh), kLeft, true, false);
    float hero_bottom = y + px(32) + sh;
    // A failed install's Report a problem, under its reason: the footer's width is taken by its buttons.
    if (hero && hero->state == Job::kFailed && !active) {
        draw_link(g, tr(L"Report a problem"), tx, hero_bottom + px(2), px(22), g_iv.hot == kActReport, kActReport);
        hero_bottom += px(24);
    }
    if (hero && (hero->state == Job::kWaiting || hero->state == Job::kInstalling)) {
        draw_progress_line(g, RectF(tx, hero_bottom + px(8), tw, px(4)));
        hero_bottom += px(12);
    }
    y = (std::max)(ir.GetBottom(), hero_bottom) + px(16);

    // The others in this batch, the newest last; at most five.
    std::vector<int> rows;
    for (int i = 0; i < total; ++i)
        if (&g_jobs[static_cast<size_t>(i)] != hero) rows.push_back(i);
    if (rows.size() > 5) rows.erase(rows.begin(), rows.end() - 5);
    if (!rows.empty() && total > 1) {
        const float rh = px(48);
        const RectF card(m, y, cw, rh * rows.size());
        fill_round(g, card, px(kRadiusCard), g_t.card);
        for (size_t k = 0; k < rows.size(); ++k) {
            Job& j = g_jobs[static_cast<size_t>(rows[k])];
            const RectF r(m, y + k * rh, cw, rh);
            if (k > 0) {
                Gdiplus::Pen line(g_t.divider, 1);
                g.DrawLine(&line, r.X + px(56), r.Y, r.GetRight() - px(12), r.Y);
            }
            draw_job_icon(g, j, RectF(r.X + px(12), r.Y + px(8), px(32), px(32)));
            std::wstring state;
            Color ink = g_t.text2;
            switch (j.state) {
                case Job::kWaiting: state = tr(L"Waiting"); break;
                case Job::kInstalling: state = tr(phase_text(j.phase)); break;
                case Job::kDone: state = tr(L"Ready"); break;
                case Job::kFailed: state = tr(L"Couldn't install"); ink = g_t.danger; break;
                case Job::kCancelled: state = tr(L"Cancelled"); break;
            }
            const float rx = r.X + px(56), rw = r.Width - px(56) - px(110);
            draw_text(g, job_title(j), g_f.body_semi.get(), g_t.text, RectF(rx, r.Y + px(6), rw, px(20)));
            draw_text(g, state, g_f.caption.get(), ink, RectF(rx, r.Y + px(25), rw, px(17)));
            const float right = r.GetRight() - px(14);
            if (j.state == Job::kDone && !system_app(j.name)) {
                draw_link(g, tr(L"Open"), right, r.Y, rh, g_iv.hot == kActRowOpen + rows[k], kActRowOpen + rows[k], kRight);
            } else if (j.state == Job::kFailed) {
                draw_link(g, tr(L"Show log"), right, r.Y, rh, g_iv.hot == kActRowLog + rows[k], kActRowLog + rows[k],
                          kRight);
            } else if (j.state == Job::kWaiting) {
                draw_text(g, kGlyphClock, g_f.glyph16.get(), g_t.text3, RectF(right - px(20), r.Y, px(20), rh), kCenter);
            } else if (j.state == Job::kInstalling) {
                spinner(g, RectF(right - px(18), r.Y + (rh - px(16)) / 2, px(16), px(16)), g_t.accent, px(2));
            }
        }
        stroke_round(g, card, px(kRadiusCard), g_t.border);
        y += card.Height + px(12);
    }

    // Show details: the log of the install the card is about, or the one asked for.
    const bool any_failed = failed > 0;
    const std::wstring details_text = tr(g_iv.details ? L"Hide details" : L"Show details");
    {
        const float lw = text_width(g, details_text, g_f.body.get()) + px(20);
        const RectF r(m, y, lw, px(24));
        draw_text(g, g_iv.details ? kGlyphChevronUp : kGlyphChevron, g_f.glyph12.get(),
                  g_iv.hot == kActDetails ? g_t.text : g_t.text2, RectF(r.X, r.Y, px(14), r.Height), kCenter);
        draw_text(g, details_text, g_f.body.get(), g_iv.hot == kActDetails ? g_t.text : g_t.text2,
                  RectF(r.X + px(18), r.Y, lw, r.Height));
        g_iv.targets.push_back({r, kActDetails});
        y += px(30);
    }
    if (g_iv.details) {
        const float lh = px(16);
        const RectF box(m, y, cw, lh * 10 + px(16));
        fill_round(g, box, px(kRadiusControl), code_bg());
        stroke_round(g, box, px(kRadiusControl), g_t.border);
        if (g_iv.log.empty()) {
            draw_text(g, tr(L"Starting…"), g_f.caption.get(), g_t.text2, box, kCenter);
        } else {
            const size_t from = g_iv.log.size() > 10 ? g_iv.log.size() - 10 : 0;
            float ly = box.Y + px(8);
            for (size_t i = from; i < g_iv.log.size(); ++i, ly += lh) {
                const std::wstring& l = g_iv.log[i];
                const bool bad = l.find(L"rror") != std::wstring::npos || l.find(L"xception") != std::wstring::npos;
                draw_text(g, l, g_f.mono.get(), bad ? g_t.danger : g_t.text2,
                          RectF(box.X + px(10), ly, box.Width - px(20), lh), kLeft, false, true, true);
            }
        }
        y += box.Height + px(12);
    }
    g_iv.need_h = y + px(4) + foot;

    // The footer's buttons, right-aligned; the default one first, as Windows' dialogs order them.
    const float fy = H - foot;
    Gdiplus::SolidBrush bar(g_t.bg);
    g.FillRectangle(&bar, RectF(0, fy, W, foot));
    Gdiplus::Pen line(g_t.border, 1);
    g.DrawLine(&line, 0.0f, fy, W, fy);
    float right = W - m;
    const float by = fy + (foot - px(32)) / 2;
    auto button = [&](const std::wstring& text, int act, bool primary) {
        const float bw = button_width(g, text, primary);
        const RectF r(right - bw, by, bw, px(32));
        draw_button(g, r, text, g_iv.hot == act, primary, false);
        g_iv.targets.push_back({r, act});
        right -= bw + px(8);
    };
    if (active) {
        button(tr(L"Cancel"), kActCancel, false);
    } else {
        const bool single_ok = hero && hero->state == Job::kDone && job_opens(*hero);
        if (any_failed && hero && hero->state == Job::kFailed) {
            button(tr(L"Copy details"), kActCopy, false);
            button(tr(L"Show log"), kActShowLog, false);
        }
        // The helper's ends are InstallSuccess's and InstallFailed's: Done, and Open beside it.
        button(tr(g_helper_ui ? L"Done" : L"Close"), kActClose, !single_ok);
        if (single_ok) button(tr(L"Open"), kActOpen, true);
        if (shown_done > 0 && !g_helper_ui)
            draw_link(g, tr(L"Show in library"), m, fy, foot, g_iv.hot == kActLibrary, kActLibrary);
    }
}

void fit_install_window() {
    if (g_install_wnd) PostMessageW(g_install_wnd, kFitInstall, 0, 0);
}

// The install window's record of an install that ended `how`: its state, and why when it failed.
void job_ended(const std::wstring& name, InstallEnd::Kind how) {
    for (Job& j : g_jobs) {
        if (j.name != name || (j.state != Job::kInstalling && j.state != Job::kWaiting)) continue;
        j.state = how == InstallEnd::kCancelled ? Job::kCancelled
                  : how == InstallEnd::kDone    ? Job::kDone
                                                : Job::kFailed;
        j.end = GetTickCount64();
        if (j.state == Job::kFailed) {
            j.reason = how == InstallEnd::kAppRunning ? fmt(L"Close {0} first, then install it again.", {job_label(name)})
                                                      : install_failure_reason(install_log(name));
        }
        break;
    }
    if (g_install_wnd) {
        fit_install_window();
        InvalidateRect(g_install_wnd, nullptr, FALSE);
    }
}

// The window's title: what it is doing, for the taskbar.
std::wstring install_title() {
    const Job* hero = hero_job();
    int done = 0;
    for (const Job& j : g_jobs) done += j.state != Job::kWaiting && j.state != Job::kInstalling;
    if (installs_active()) {
        if (g_jobs.size() > 1) {
            return fmt(L"Installing {0} of {1}", {std::to_wstring((std::min)(done + 1, static_cast<int>(g_jobs.size()))),
                                                  std::to_wstring(g_jobs.size())});
        }
        return fmt(L"Installing {0}", {hero ? job_title(*hero) : std::wstring()});
    }
    return hero ? job_title(*hero) : std::wstring(L"Win.apk");
}

// Copy details: what a bug report needs, the reason and the end of the log.
void copy_install_details(HWND hwnd, const Job& j) {
    std::wstring text = L"Win.apk " + version() + L"\r\n" + fmt(L"Couldn't install {0}", {job_title(j)}) + L"\r\n" +
                        j.reason + L"\r\n\r\n";
    for (const std::wstring& l : last_lines(install_log(j.name), 200)) text += l + L"\r\n";
    copy_text(hwnd, text);
}

bool ask_stop_installing(HWND hwnd) {
    Prompt p;
    p.question = tr(L"Stop installing?");
    p.note = tr(L"What was not installed yet is left out. An app that was being updated stays as it was.");
    p.buttons = {{IDOK, tr(L"Stop installing")}, {IDCANCEL, tr(L"Keep installing")}};
    p.default_id = IDCANCEL;
    return run_dialog(p, hwnd) == IDOK;
}

void cancel_all_installs() {
    for (const Job& j : g_jobs) {
        if (j.state == Job::kWaiting || j.state == Job::kInstalling) cancel_install(j.name);
    }
}

void install_action(HWND hwnd, int act) {
    if (act == kActClose) {
        DestroyWindow(hwnd);
    } else if (act == kActCancel) {
        if (ask_stop_installing(hwnd)) cancel_all_installs();
    } else if (act == kActOpen) {
        const Job* hero = hero_job();
        const App* a = hero ? find_app(hero->name) : nullptr;
        if (a && g_helper_ui) {
            // InstallSuccess's Open starts the app's launch intent: here, as its shortcut does, in a
            // Win.apk.exe of its own (this helper ends with its window).
            std::wstring cmd = L"\"" + exe_path().wstring() + L"\" --run \"" + a->apk.wstring() + L"\"";
            STARTUPINFOW si{sizeof(si)};
            PROCESS_INFORMATION pi{};
            if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
                AllowSetForegroundWindow(pi.dwProcessId);
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
            }
            DestroyWindow(hwnd);
        } else if (a) {
            launch(*a);  // first: with the library hidden, closing this window last must not end the process
            DestroyWindow(hwnd);
        }
    } else if (act == kActDetails) {
        g_iv.details = !g_iv.details;
        g_iv.log_read = 0;
        fit_install_window();
    } else if (act == kActShowLog) {
        g_iv.details = true;
        if (const Job* hero = hero_job()) g_iv.log_name = hero->name;
        g_iv.log_read = 0;
        fit_install_window();
    } else if (act == kActCopy) {
        if (const Job* hero = hero_job()) copy_install_details(hwnd, *hero);
    } else if (act == kActReport) {
        if (const Job* hero = hero_job()) {
            const Job j = *hero;  // the dialog is modal: the jobs can change under it
            report_problem(hwnd, j.name, job_title(j), install_log(j.name),
                           fmt(L"Couldn't install {0}", {job_title(j)}) + L". " + j.reason);
        }
    } else if (act == kActLibrary) {
        std::wstring pick;
        for (const Job& j : g_jobs)
            if (j.state == Job::kDone && !system_app(j.name)) pick = j.name;
        show_library(SW_SHOWDEFAULT);
        set_view(kLibraryView);
        if (!pick.empty()) select_app(pick);
        DestroyWindow(hwnd);
    } else if (act >= kActRowLog) {
        const size_t i = static_cast<size_t>(act - kActRowLog);
        if (i < g_jobs.size()) {
            g_iv.details = true;
            g_iv.log_name = g_jobs[i].name;
            g_iv.log_read = 0;
            fit_install_window();
        }
    } else if (act >= kActRowOpen) {
        const size_t i = static_cast<size_t>(act - kActRowOpen);
        if (i < g_jobs.size() && !system_app(g_jobs[i].name)) {
            if (const App* a = find_app(g_jobs[i].name)) launch(*a);
        }
    }
    InvalidateRect(hwnd, nullptr, FALSE);
}

// Every 400 ms: the install's step and icon from what it has written, and the log when it is shown.
void poll_install(HWND hwnd) {
    for (Job& j : g_jobs) {
        if (j.state != Job::kInstalling) continue;
        const int phase = (std::max)(j.phase, install_phase(install_log(j.name)));
        // The asking app's session moves with the install's steps (its progress bar, onProgressChanged).
        static const char* const kStages[] = {"", "installing", "reading", "converting"};
        if (g_helper_ui && phase > j.phase) helper_progress(0.1 + 0.25 * phase, kStages[phase]);
        j.phase = phase;
        if (!find_app(j.name)) {  // a new app: what its install has written of it so far
            const PendingLook& look = pending_look(j.name);
            if (!j.icon) j.icon = look.icon;
            if (j.label.empty()) j.label = look.label;
        }
    }
    if (g_iv.details) {
        std::wstring who = g_iv.log_name;
        if (who.empty() || installs_active()) {
            if (const Job* hero = hero_job()) who = hero->name;
        }
        for (auto it = g_jobs.rbegin(); who.empty() && it != g_jobs.rend(); ++it) {  // a finished batch: the last one's
            if (it->state != Job::kWaiting) who = it->name;
        }
        g_iv.log = who.empty() ? std::vector<std::wstring>{} : last_lines(install_log(who), 40);
    }
    g_iv.log_read = GetTickCount64();
    SetWindowTextW(hwnd, install_title().c_str());
}

LRESULT CALLBACK install_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE:
            SetTimer(hwnd, 1, 33, nullptr);
            return 0;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            RECT rc;
            GetClientRect(hwnd, &rc);
            buffered_paint(hwnd, paint_install);
            if (std::fabs(g_iv.need_h - static_cast<float>(rc.bottom)) > 1) fit_install_window();
            return 0;
        }
        case kFitInstall: {
            RECT wr, rc;
            GetWindowRect(hwnd, &wr);
            GetClientRect(hwnd, &rc);
            const int want = static_cast<int>(std::lround(g_iv.need_h));
            if (want > 0 && want != rc.bottom) {
                SetWindowPos(hwnd, nullptr, 0, 0, wr.right - wr.left, (wr.bottom - wr.top) - rc.bottom + want,
                             SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_TIMER: {
            const bool active = installs_active();
            if (GetTickCount64() - g_iv.log_read > 400) poll_install(hwnd);
            if (active) InvalidateRect(hwnd, nullptr, FALSE);
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
            if (wp == VK_ESCAPE) {
                install_action(hwnd, installs_active() ? kActCancel : kActClose);
            } else if (wp == VK_RETURN && !installs_active()) {
                const Job* hero = hero_job();
                install_action(hwnd, hero && hero->state == Job::kDone && job_opens(*hero) ? kActOpen : kActClose);
            }
            return 0;
        case kInstalled: {  // the install helper's install ended (helper_install_thread)
            std::unique_ptr<std::wstring> name(reinterpret_cast<std::wstring*>(lp));
            if (g_helper_ui) helper_install_ended(*name);
            return 0;
        }
        case WM_CLOSE:
            if (installs_active()) {
                if (ask_stop_installing(hwnd)) cancel_all_installs();
                return 0;  // the window stays to show the result
            }
            break;
        case WM_DESTROY:
            g_install_wnd = nullptr;
            g_iv = InstallView{};
            if (!any_pending()) g_jobs.clear();
            if (g_helper_ui) PostQuitMessage(0);  // the helper's one window: its run ends with it
            maybe_quit();
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void show_install_window() {
    if (g_install_wnd) {
        poll_install(g_install_wnd);
        fit_install_window();
        if (IsIconic(g_install_wnd)) ShowWindow(g_install_wnd, SW_RESTORE);
        SetForegroundWindow(g_install_wnd);
        return;
    }
    g_iv.details = false;  // collapsed each time: the log is for when something needs a look
    g_install_wnd = open_tool_window(L"WinApkInstall", install_title().c_str(), ipx(440), ipx(200),
                                     WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX);
    if (g_install_wnd) {
        poll_install(g_install_wnd);
        fit_install_window();
    }
}

// ---------------------------------------------------------------------------
// The log window: which log, search, Copy and Open folder, and the log itself
// in a read-only RichEdit (Windows' own msftedit.dll) that follows the end as
// the runtime writes it, with error lines and search matches marked.

struct LogView {
    fs::path path;
    uintmax_t read = 0;           // bytes of the file shown so far (whole lines)
    bool missing = false;
    HWND search = nullptr;
    std::wstring query;
    std::vector<std::pair<LONG, LONG>> matches;  // character ranges
    int current = -1;
    Targets targets;
    int hot = 0;
    bool tracking = false;
} g_lv;

enum { kLogPick = 1, kLogCopy, kLogFolder };
constexpr int kIdLogSearch = 101;

// What the picker calls a log: the app and which log, in words.
std::wstring log_title(const fs::path& p) {
    const std::wstring stem = p.stem().wstring();
    auto ends = [&](const std::wstring& suffix) {
        return stem.size() > suffix.size() && stem.compare(stem.size() - suffix.size(), suffix.size(), suffix) == 0;
    };
    if (stem.rfind(L"run-", 0) == 0) {
        if (ends(L".1")) return fmt(L"{0}  ·  run before", {label_of(stem.substr(4, stem.size() - 6))});
        return fmt(L"{0}  ·  last run", {label_of(stem.substr(4))});
    }
    if (stem.rfind(L"install-", 0) == 0 && stem != L"install-helper") {
        return fmt(L"{0}  ·  install", {label_of(stem.substr(8))});
    }
    if (stem == L"gms") return tr(L"Google sign-in");
    if (stem == L"google-setup") return tr(L"Google services setup");
    if (stem == L"install-helper") return tr(L"Installs asked for by apps");
    return stem;
}

// The logs, the newest first; last-run.log is a second name for one of them, so it is left out.
std::vector<fs::path> log_files() {
    std::vector<std::pair<fs::file_time_type, fs::path>> found;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(logs_dir(), ec)) {
        if (e.path().extension() == L".log" && e.path().filename() != L"last-run.log")
            found.push_back({e.last_write_time(ec), e.path()});
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<fs::path> out;
    for (auto& f : found) out.push_back(std::move(f.second));
    return out;
}

bool error_line(const std::wstring& l) {
    return l.find(L"rror") != std::wstring::npos || l.find(L"xception") != std::wstring::npos ||
           l.rfind(L"[E]", 0) == 0 || l.rfind(L"[F]", 0) == 0 || l.find(L"FATAL") != std::wstring::npos;
}

void log_format(LONG from, LONG to, COLORREF color, bool back, COLORREF back_color = 0) {
    CHARRANGE cr{from, to};
    SendMessageW(g_log_edit, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&cr));
    CHARFORMAT2W cf{};
    cf.cbSize = sizeof(cf);
    if (back) {
        cf.dwMask = CFM_BACKCOLOR;
        cf.crBackColor = back_color;
        if (back_color == CLR_DEFAULT) {
            cf.dwEffects = CFE_AUTOBACKCOLOR;
            cf.crBackColor = 0;
        }
    } else {
        cf.dwMask = CFM_COLOR;
        cf.crTextColor = color;
    }
    SendMessageW(g_log_edit, EM_SETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&cf));
}

// Marks every match of the search and selects the current one.
void log_mark_matches(bool keep_current) {
    SendMessageW(g_log_edit, WM_SETREDRAW, FALSE, 0);
    CHARRANGE keep{};
    SendMessageW(g_log_edit, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&keep));
    for (const auto& m : g_lv.matches) log_format(m.first, m.second, 0, true, CLR_DEFAULT);
    g_lv.matches.clear();
    if (!g_lv.query.empty()) {
        FINDTEXTEXW ft{};
        ft.chrg = {0, -1};
        ft.lpstrText = g_lv.query.c_str();
        while (g_lv.matches.size() < 5000) {
            const LRESULT at = SendMessageW(g_log_edit, EM_FINDTEXTEXW, FR_DOWN, reinterpret_cast<LPARAM>(&ft));
            if (at < 0) break;
            g_lv.matches.push_back({ft.chrgText.cpMin, ft.chrgText.cpMax});
            ft.chrg.cpMin = ft.chrgText.cpMax;
        }
        const COLORREF hl = g_t.dark ? RGB(110, 90, 20) : RGB(255, 230, 140);
        for (const auto& m : g_lv.matches) log_format(m.first, m.second, 0, true, hl);
    }
    if (!keep_current || g_lv.current >= static_cast<int>(g_lv.matches.size())) g_lv.current = g_lv.matches.empty() ? -1 : 0;
    SendMessageW(g_log_edit, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&keep));
    SendMessageW(g_log_edit, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_log_edit, nullptr, TRUE);
    InvalidateRect(g_log_wnd, nullptr, FALSE);
}

void log_go_to_match(int step) {
    if (g_lv.matches.empty()) return;
    const int n = static_cast<int>(g_lv.matches.size());
    g_lv.current = ((g_lv.current < 0 ? 0 : g_lv.current + step) % n + n) % n;
    const auto& m = g_lv.matches[static_cast<size_t>(g_lv.current)];
    CHARRANGE cr{m.first, m.second};
    SendMessageW(g_log_edit, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&cr));
    SendMessageW(g_log_edit, EM_SCROLLCARET, 0, 0);
    InvalidateRect(g_log_wnd, nullptr, FALSE);
}

// Reads what the file gained since last time (the whole file when `force`, or when it was started
// again and shrank), appends it and marks its error lines. The view stays at the end if it was there.
void load_log(bool force) {
    std::error_code ec;
    const bool exists = fs::exists(g_lv.path, ec);
    const uintmax_t size = exists ? fs::file_size(g_lv.path, ec) : 0;
    if (!force && exists == !g_lv.missing && size == g_lv.read) return;
    if (force || size < g_lv.read || !exists || g_lv.missing) {
        SetWindowTextW(g_log_edit, L"");
        g_lv.read = 0;
        g_lv.matches.clear();
    }
    g_lv.missing = !exists;
    if (!exists) {
        SetWindowTextW(g_log_edit, tr(L"This log is written the next time the app is opened or installed."));
        InvalidateRect(g_log_wnd, nullptr, FALSE);
        return;
    }
    // At most the last megabyte at first; then only whole new lines.
    const uintmax_t from = g_lv.read == 0 && size > (1u << 20) ? size - (1u << 20) : g_lv.read;
    std::string bytes;
    HANDLE f = CreateFileW(g_lv.path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER at{};
    at.QuadPart = static_cast<LONGLONG>(from);
    SetFilePointerEx(f, at, nullptr, FILE_BEGIN);
    bytes.resize(static_cast<size_t>(size - from));
    DWORD got = 0;
    if (!bytes.empty()) ReadFile(f, &bytes[0], static_cast<DWORD>(bytes.size()), &got, nullptr);
    CloseHandle(f);
    bytes.resize(got);
    const size_t last_nl = bytes.rfind('\n');
    if (last_nl == std::string::npos) return;  // no whole line yet
    bytes.resize(last_nl + 1);
    if (g_lv.read == 0 && from > 0) bytes.erase(0, bytes.find('\n') + 1);  // the first line was cut
    g_lv.read = from + last_nl + 1;
    std::wstring text;
    const std::wstring raw = utf8_to_wide(bytes);
    text.reserve(raw.size());
    for (wchar_t c : raw) {
        if (c != L'\r') text += c == L'\n' ? L'\r' : c;  // RichEdit's line end
    }

    SCROLLINFO si{sizeof(si), SIF_ALL};
    GetScrollInfo(g_log_edit, SB_VERT, &si);
    const bool at_end = force || si.nPos + static_cast<int>(si.nPage) >= si.nMax - 2;
    SendMessageW(g_log_edit, WM_SETREDRAW, FALSE, 0);
    CHARRANGE keep{};
    SendMessageW(g_log_edit, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&keep));
    const LONG start = GetWindowTextLengthW(g_log_edit);
    CHARRANGE end{start, start};
    SendMessageW(g_log_edit, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&end));
    SendMessageW(g_log_edit, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(text.c_str()));
    // The new lines that report errors, in the error colour.
    LONG pos = start;
    size_t line_at = 0;
    while (line_at < text.size()) {
        size_t nl = text.find(L'\r', line_at);
        if (nl == std::wstring::npos) nl = text.size();
        if (error_line(text.substr(line_at, nl - line_at)))
            log_format(pos, pos + static_cast<LONG>(nl - line_at), colorref(g_t.danger), false);
        pos += static_cast<LONG>(nl - line_at + 1);
        line_at = nl + 1;
    }
    SendMessageW(g_log_edit, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&keep));
    SendMessageW(g_log_edit, WM_SETREDRAW, TRUE, 0);
    if (!g_lv.query.empty()) log_mark_matches(true);
    if (at_end) {
        SendMessageW(g_log_edit, WM_VSCROLL, SB_BOTTOM, 0);
    }
    InvalidateRect(g_log_edit, nullptr, TRUE);
    InvalidateRect(g_log_wnd, nullptr, FALSE);
}

constexpr float kLogTools = 56;

void paint_logs(Graphics& g, float W, float H) {
    g.Clear(g_t.layer);
    g_lv.targets.clear();
    const float m = px(16), ch = px(32), cy = (px(kLogTools) - ch) / 2;
    // The picker: which log.
    const RectF pick(m, cy, (std::min)(px(280), W * 0.34f), ch);
    fill_round(g, pick, px(kRadiusControl), g_lv.hot == kLogPick ? g_t.hover : g_t.card);
    stroke_round(g, pick, px(kRadiusControl), g_t.border);
    draw_text(g, log_title(g_lv.path), g_f.body.get(), g_t.text, RectF(pick.X + px(12), pick.Y, pick.Width - px(40), ch));
    draw_text(g, kGlyphChevron, g_f.glyph12.get(), g_t.text2, RectF(pick.GetRight() - px(30), pick.Y, px(22), ch),
              kCenter);
    g_lv.targets.push_back({pick, kLogPick});
    // Copy and Open folder at the end.
    float x = W - m;
    for (auto [label, act] : {std::pair<const wchar_t*, int>{tr(L"Open folder"), kLogFolder}, {tr(L"Copy"), kLogCopy}}) {
        const float w = text_width(g, label, g_f.body.get()) + px(32);
        x -= w;
        const RectF r(x, cy, w, ch);
        draw_button(g, r, label, g_lv.hot == act, false, false);
        g_lv.targets.push_back({r, act});
        x -= px(8);
    }
    // The search box between them (the edit is a child; this is its frame), with the match count.
    const RectF box(pick.GetRight() + px(12), cy, (std::max)(px(80), x - px(4) - pick.GetRight() - px(12)), ch);
    const bool focused = GetFocus() == g_lv.search;
    fill_round(g, box, px(kRadiusControl), g_t.card);
    stroke_round(g, box, px(kRadiusControl), focused ? g_t.accent : g_t.border, focused ? px(1.5f) : 1);
    draw_text(g, kGlyphSearch, g_f.glyph12.get(), g_t.text2, RectF(box.X + px(10), box.Y, px(16), ch), kCenter);
    if (!g_lv.query.empty()) {
        const std::wstring count = g_lv.matches.empty()
                                       ? std::wstring(tr(L"No matches"))
                                       : fmt(L"{0} of {1}", {std::to_wstring(g_lv.current + 1),
                                                             std::to_wstring(g_lv.matches.size())});
        draw_text(g, count, g_f.caption.get(), g_t.text2, RectF(box.GetRight() - px(96), box.Y, px(88), ch), kRight);
    }
    Gdiplus::Pen line(g_t.border, 1);
    g.DrawLine(&line, 0.0f, px(kLogTools), W, px(kLogTools));
    (void)H;
}

void layout_logs(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int top = ipx(kLogTools) + 1;
    MoveWindow(g_log_edit, 0, top, rc.right, (std::max)(0L, rc.bottom - top), TRUE);
    RECT text;  // some room around the text
    GetClientRect(g_log_edit, &text);
    InflateRect(&text, -ipx(12), -ipx(8));
    SendMessageW(g_log_edit, EM_SETRECT, 0, reinterpret_cast<LPARAM>(&text));
    // The search edit sits inside the frame paint_logs draws: after the picker, before the buttons.
    HDC dc = GetDC(hwnd);
    float buttons = 0;
    {
        Graphics g(dc);
        for (const wchar_t* label : {tr(L"Open folder"), tr(L"Copy")}) buttons += text_width(g, label, g_f.body.get()) + px(40);
    }
    ReleaseDC(hwnd, dc);
    const float W = static_cast<float>(rc.right), m = px(16);
    const float pick_r = m + (std::min)(px(280), W * 0.34f);
    const float bx = pick_r + px(12), bw = (std::max)(px(80), W - m - buttons - px(4) - bx);
    const int eh = ipx(20);
    MoveWindow(g_lv.search, static_cast<int>(bx + px(32)), static_cast<int>((px(kLogTools) - eh) / 2),
               static_cast<int>(bw - px(32) - px(100)), eh, TRUE);
}

LRESULT CALLBACK log_search_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_KEYDOWN) {
        const bool shift = GetKeyState(VK_SHIFT) < 0;
        if (wp == VK_RETURN || wp == VK_F3) {
            log_go_to_match(shift ? -1 : 1);
            return 0;
        }
        if (wp == VK_ESCAPE) {
            if (GetWindowTextLengthW(hwnd) > 0) SetWindowTextW(hwnd, L"");
            else DestroyWindow(g_log_wnd);
            return 0;
        }
    }
    if (msg == WM_CHAR && (wp == VK_RETURN || wp == VK_ESCAPE)) return 0;  // no beep
    return DefSubclassProc(hwnd, msg, wp, lp);
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
        std::wstring text(static_cast<size_t>(len) + 1, L'\0');
        GetWindowTextW(g_log_edit, &text[0], len + 1);
        text.resize(static_cast<size_t>(len));
        std::wstring crlf;
        crlf.reserve(text.size() + text.size() / 32);
        for (wchar_t c : text) {
            if (c == L'\r') crlf += L"\r\n";
            else crlf += c;
        }
        copy_text(hwnd, crlf);
    } else if (act == kLogFolder) {
        std::error_code ec;
        if (fs::exists(g_lv.path, ec)) show_in_explorer(g_lv.path);
        else open_path(logs_dir());
    }
}

LRESULT CALLBACK log_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            // A log is English, paths and stack traces: it reads left to right with its scroll bar on the right,
            // also in a Hebrew window. A child takes its parent's mirrored layout when it is made, so the
            // window stops passing it on first.
            SetWindowLongPtrW(hwnd, GWL_EXSTYLE, GetWindowLongPtrW(hwnd, GWL_EXSTYLE) | WS_EX_NOINHERITLAYOUT);
            static HMODULE richedit = LoadLibraryW(L"Msftedit.dll");
            (void)richedit;
            g_log_edit = CreateWindowExW(WS_EX_LTRREADING | WS_EX_LEFT | WS_EX_RIGHTSCROLLBAR, MSFTEDIT_CLASS, L"",
                                         WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_LEFT | ES_MULTILINE |
                                             ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_NOHIDESEL,
                                         0, 0, 0, 0, hwnd, nullptr, g_inst, nullptr);
            SendMessageW(g_log_edit, EM_EXLIMITTEXT, 0, 64 << 20);
            SendMessageW(g_log_edit, EM_SETBKGNDCOLOR, 0, colorref(code_bg()));
            CHARFORMAT2W cf{};
            cf.cbSize = sizeof(cf);
            cf.dwMask = CFM_FACE | CFM_SIZE | CFM_COLOR;
            cf.yHeight = 200;  // 10 pt, in twips
            cf.crTextColor = colorref(g_t.text);
            Gdiplus::FontFamily cascadia(L"Cascadia Mono");
            wcscpy(cf.szFaceName, cascadia.GetLastStatus() == Gdiplus::Ok && cascadia.IsAvailable() ? L"Cascadia Mono"
                                                                                                   : L"Consolas");
            SendMessageW(g_log_edit, EM_SETCHARFORMAT, SCF_ALL, reinterpret_cast<LPARAM>(&cf));
            SendMessageW(g_log_edit, EM_SETCHARFORMAT, SCF_DEFAULT, reinterpret_cast<LPARAM>(&cf));
            SetWindowTheme(g_log_edit, g_t.dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
            g_lv.search = CreateWindowExW(g_rtl ? WS_EX_RTLREADING : 0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                          0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdLogSearch)),
                                          g_inst, nullptr);
            SendMessageW(g_lv.search, WM_SETFONT, reinterpret_cast<WPARAM>(g_edit_font), FALSE);
            SendMessageW(g_lv.search, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(tr(L"Search the log")));
            SetWindowSubclass(g_lv.search, log_search_proc, 1, 0);
            SetTimer(hwnd, 1, 700, nullptr);
            return 0;
        }
        case WM_SIZE: layout_logs(hwnd); InvalidateRect(hwnd, nullptr, FALSE); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: buffered_paint(hwnd, paint_logs); return 0;
        case WM_CTLCOLOREDIT:
            SetTextColor(reinterpret_cast<HDC>(wp), colorref(g_t.text));
            SetBkColor(reinterpret_cast<HDC>(wp), colorref(g_t.card));
            return reinterpret_cast<LRESULT>(g_edit_brush);
        case WM_COMMAND:
            if (LOWORD(wp) == kIdLogSearch) {
                if (HIWORD(wp) == EN_CHANGE) {
                    wchar_t buf[256] = {};
                    GetWindowTextW(g_lv.search, buf, 256);
                    g_lv.query = buf;
                    log_mark_matches(false);
                    if (!g_lv.matches.empty()) log_go_to_match(0);
                } else if (HIWORD(wp) == EN_SETFOCUS || HIWORD(wp) == EN_KILLFOCUS) {
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            return 0;
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
        case WM_ACTIVATE:
            if (LOWORD(wp) != WA_INACTIVE) SetFocus(g_lv.search);
            return 0;
        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {ipx(560), ipx(360)};
            return 0;
        case WM_DESTROY:
            g_log_wnd = g_log_edit = nullptr;
            g_lv = LogView{};
            maybe_quit();
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// The log window on `log`; the newest log when none is named.
void show_logs(const fs::path& log) {
    fs::path path = log;
    if (path.empty()) {
        const auto files = log_files();
        path = files.empty() ? last_run_log() : files.front();
    }
    g_lv.path = path;
    if (!g_log_wnd) {
        g_log_wnd = open_tool_window(L"WinApkLogs", tr(L"Log - Win.apk"), ipx(860), ipx(620));
        if (!g_log_wnd) return;
        layout_logs(g_log_wnd);
    } else {
        ShowWindow(g_log_wnd, SW_RESTORE);
        SetForegroundWindow(g_log_wnd);
    }
    load_log(true);
}

// ---------------------------------------------------------------------------
// The Threads window (debug mode, Settings > Troubleshooting): every thread of an app's processes, live.
// A worker thread asks Windows each second for the processes of every running app (each winapk-runtime
// and whatever it started: WebView2, a translation worker) and all their threads, and each runtime's debug
// agent (host/debug_agent.cpp) for what only it knows: the Java threads, the app's native ones, a thread's
// stack. Windows' list is the ground: a thread is a row whether or not an agent answered (debug_threads.h).
// The window picks the app, searches, sorts and shows the selected thread's stack.

namespace appops {
std::wstring command_line_of(HANDLE process);
}

using winapk::launcher::ThreadColumn;
using winapk::launcher::ThreadKind;
using winapk::launcher::ThreadRow;

bool debug_mode() { return setting(L"DebugMode", 0) != 0; }

constexpr wchar_t kDebugSystemApp[] = L"*system";  // the system broker and what it runs for no one app
constexpr UINT kDebugSnapshot = WM_APP + 1;        // the worker's: lParam a new DebugSnapshot
constexpr int kIdDebugSearch = 101, kIdDebugList = 102, kIdDebugStack = 103;
constexpr float kDebugSummary = 30;
enum { kDebugPick = 1, kDebugPause, kDebugCopy };

struct DebugProcess {
    DWORD pid = 0;
    std::wstring exe;
    std::wstring app;      // the library name it runs for; kDebugSystemApp; empty: none known
    bool runtime = false;  // a winapk-runtime.exe
    bool agent = false;    // its debug agent answered
};

enum class StackState { kNone, kText, kNoAgent, kNotRuntime };

struct DebugSnapshot {
    std::wstring app;  // the app it was taken for; empty: all
    std::vector<DebugProcess> processes;  // of every app
    std::vector<ThreadRow> rows;          // of `app`
    uint64_t time = 0;                    // 100 ns
    DWORD stack_pid = 0, stack_tid = 0;
    long long stack_jid = -1;
    StackState stack_state = StackState::kNone;
    std::string stack;
};

// What the window and its worker share.
struct DebugShared {
    SRWLOCK lock = SRWLOCK_INIT;
    HANDLE wake = nullptr;  // look now
    HWND hwnd = nullptr;
    bool stop = false, paused = false;
    std::wstring app;
    bool selected = false;
    DWORD sel_pid = 0, sel_tid = 0;
    long long sel_jid = -1;
};

struct DebugView {
    HWND list = nullptr, stack = nullptr, search = nullptr;
    Targets targets;
    int hot = 0;
    bool tracking = false;
    std::shared_ptr<DebugShared> shared;
    std::wstring app;
    bool first = true;  // nothing came from the worker yet
    bool paused = false;
    bool syncing = false;  // the list's selection is being set from here
    std::vector<DebugProcess> processes;
    std::vector<ThreadRow> all, rows;  // of the app; those the search leaves, sorted
    winapk::launcher::CpuTracker cpu;
    std::string query;
    ThreadColumn sort = ThreadColumn::kProcess;
    bool descending = false;
    bool selected = false;
    DWORD sel_pid = 0, sel_tid = 0;
    long long sel_jid = -1;
    std::wstring stack_text;
} g_dv;

// One question to a runtime's debug agent; false when it has none or does not answer in time.
bool debug_ask(DWORD pid, const std::string& question, DWORD timeout_ms, std::string* reply) {
    const std::wstring name = winapk::debug::pipe_name(pid);
    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 3; ++attempt) {
        pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED,
                           nullptr);
        if (pipe != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_PIPE_BUSY || !WaitNamedPipeW(name.c_str(), 300)) return false;
    }
    if (pipe == INVALID_HANDLE_VALUE) return false;
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    // One read or write, given up at the deadline: an agent whose process hangs must not hang the window's worker.
    auto io = [&](bool write, void* buf, DWORD n, DWORD* moved) {
        OVERLAPPED ov{};
        ov.hEvent = done;
        ResetEvent(done);
        *moved = 0;
        const BOOL ok = write ? WriteFile(pipe, buf, n, nullptr, &ov) : ReadFile(pipe, buf, n, nullptr, &ov);
        if (!ok) {
            if (GetLastError() != ERROR_IO_PENDING) return false;
            const ULONGLONG now = GetTickCount64();
            if (now >= deadline || WaitForSingleObject(done, static_cast<DWORD>(deadline - now)) != WAIT_OBJECT_0) {
                CancelIo(pipe);
                GetOverlappedResult(pipe, &ov, moved, TRUE);
                return false;
            }
        }
        return GetOverlappedResult(pipe, &ov, moved, FALSE) != 0;
    };
    std::string q = question + "\n";
    DWORD moved = 0;
    bool ok = done && io(true, &q[0], static_cast<DWORD>(q.size()), &moved) && moved == q.size();
    if (ok) {
        reply->clear();
        std::vector<char> buf(64 * 1024);
        for (;;) {
            SetLastError(ERROR_SUCCESS);
            if (!io(false, buf.data(), static_cast<DWORD>(buf.size()), &moved) || !moved) break;
            reply->append(buf.data(), moved);
        }
        // The agent closed its end: the answer is whole. Anything else (the deadline) and it is not.
        const DWORD end = GetLastError();
        ok = end == ERROR_BROKEN_PIPE || end == ERROR_HANDLE_EOF || end == ERROR_PIPE_NOT_CONNECTED ||
             end == ERROR_SUCCESS;
    }
    if (done) CloseHandle(done);
    CloseHandle(pipe);
    return ok;
}

uint64_t filetime_100ns(const FILETIME& t) { return (static_cast<uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime; }

// The app a winapk-runtime.exe runs for, by its command line; empty when it says none.
std::wstring debug_app_of(const std::wstring& cmd) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(cmd.c_str(), &argc);
    std::wstring app;
    for (int i = 1; argv && i < argc && app.empty(); ++i) {
        const std::wstring a = argv[i];
        if (a == L"--apk" && i + 1 < argc) {
            app = fs::path(argv[i + 1]).stem().wstring();
        } else if (a == L"--system" || a == L"--gms") {
            app = kDebugSystemApp;
        } else if (a == L"--aot-translate" && i + 1 < argc) {  // ...\apps\<name>\lib\arm64
            fs::path dir = argv[i + 1];
            for (fs::path p = dir; p.has_parent_path() && p != p.parent_path(); p = p.parent_path()) {
                if (_wcsicmp(p.parent_path().filename().c_str(), L"apps") == 0) app = p.filename().wstring();
            }
        }
    }
    if (argv) LocalFree(argv);
    return app;
}

// Every winapk-runtime.exe and every process one of them started, each with the app it belongs to. A
// process takes its parent's app; a parent is believed only when it started first (a process id is reused).
std::vector<DebugProcess> debug_processes() {
    struct Entry {
        DWORD pid = 0, parent = 0;
        std::wstring exe;
        int state = 0;  // 0 not looked at, 1 being looked at, 2 done
        bool in_tree = false;
        std::wstring app;
    };
    std::map<DWORD, Entry> all;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return {};
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    for (BOOL more = Process32FirstW(snap, &pe); more; more = Process32NextW(snap, &pe)) {
        Entry e;
        e.pid = pe.th32ProcessID;
        e.parent = pe.th32ParentProcessID;
        e.exe = pe.szExeFile;
        all[e.pid] = std::move(e);
    }
    CloseHandle(snap);
    auto created = [](DWORD pid) -> uint64_t {
        HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!p) return 0;
        FILETIME c{}, x1, x2, x3;
        const bool ok = GetProcessTimes(p, &c, &x1, &x2, &x3) != 0;
        CloseHandle(p);
        return ok ? filetime_100ns(c) : 0;
    };
    std::function<void(Entry&)> resolve = [&](Entry& e) {
        if (e.state) return;
        e.state = 1;
        const bool runtime = _wcsicmp(e.exe.c_str(), L"winapk-runtime.exe") == 0;
        if (runtime) {
            e.in_tree = true;
            if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, e.pid)) {
                e.app = debug_app_of(appops::command_line_of(p));
                CloseHandle(p);
            }
        }
        const auto parent = all.find(e.parent);
        if (e.pid && parent != all.end() && parent->second.state != 1 && (!e.in_tree || e.app.empty())) {
            resolve(parent->second);
            if (parent->second.in_tree) {
                const uint64_t mine = created(e.pid), theirs = created(parent->second.pid);
                if (mine && theirs && theirs <= mine) {
                    e.in_tree = true;
                    if (e.app.empty()) e.app = parent->second.app;
                }
            }
        }
        e.state = 2;
    };
    std::vector<DebugProcess> out;
    for (auto& [pid, e] : all) {
        resolve(e);
        if (!e.in_tree) continue;
        DebugProcess p;
        p.pid = pid;
        p.exe = e.exe;
        p.app = e.app;
        p.runtime = _wcsicmp(e.exe.c_str(), L"winapk-runtime.exe") == 0;
        out.push_back(std::move(p));
    }
    return out;
}

// The threads Windows lists for `pids`, by process. `modules` remembers which module a start address is in.
std::map<DWORD, std::vector<winapk::launcher::OsThread>> debug_os_threads(
    const std::set<DWORD>& pids, std::map<std::pair<DWORD, uintptr_t>, std::string>* modules) {
    std::map<DWORD, std::vector<winapk::launcher::OsThread>> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    using GetDescription = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    using MappedName = DWORD(WINAPI*)(HANDLE, LPVOID, LPWSTR, DWORD);
    using QueryThread = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    static const auto get_description =
        reinterpret_cast<GetDescription>(reinterpret_cast<void*>(GetProcAddress(kernel, "GetThreadDescription")));
    static const auto mapped_name =
        reinterpret_cast<MappedName>(reinterpret_cast<void*>(GetProcAddress(kernel, "K32GetMappedFileNameW")));
    static const auto query_thread = reinterpret_cast<QueryThread>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread")));
    std::map<DWORD, HANDLE> processes;
    for (auto it = modules->begin(); it != modules->end();) it = pids.count(it->first.first) ? std::next(it) : modules->erase(it);
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    for (BOOL more = Thread32First(snap, &te); more; more = Thread32Next(snap, &te)) {
        if (!pids.count(te.th32OwnerProcessID)) continue;
        winapk::launcher::OsThread t;
        t.tid = te.th32ThreadID;
        t.priority = static_cast<int>(te.tpBasePri);
        HANDLE h = OpenThread(THREAD_QUERY_INFORMATION, FALSE, t.tid);
        if (!h) h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, t.tid);
        if (h) {
            FILETIME c{}, e{}, k{}, u{};
            if (GetThreadTimes(h, &c, &e, &k, &u)) {
                t.timed = true;
                t.cpu = filetime_100ns(k) + filetime_100ns(u);
            }
            PWSTR desc = nullptr;
            if (get_description && SUCCEEDED(get_description(h, &desc)) && desc) {
                t.name = wide_to_utf8(desc);
                LocalFree(desc);
            }
            void* start = nullptr;
            constexpr ULONG kThreadQuerySetWin32StartAddress = 9;
            if (query_thread && query_thread(h, kThreadQuerySetWin32StartAddress, &start, sizeof(start), nullptr) == 0 &&
                start) {
                const auto key = std::make_pair(te.th32OwnerProcessID, reinterpret_cast<uintptr_t>(start));
                auto known = modules->find(key);
                if (known == modules->end()) {
                    if (!processes.count(key.first)) {
                        processes[key.first] =
                            OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, key.first);
                    }
                    std::string module;
                    wchar_t path[MAX_PATH * 2];
                    if (mapped_name && processes[key.first] &&
                        mapped_name(processes[key.first], start, path, MAX_PATH * 2) > 0)
                        module = wide_to_utf8(fs::path(path).filename().wstring());
                    known = modules->emplace(key, std::move(module)).first;
                }
                t.start = known->second;
            }
            CloseHandle(h);
        }
        out[te.th32OwnerProcessID].push_back(std::move(t));
    }
    CloseHandle(snap);
    for (auto& [pid, p] : processes)
        if (p) CloseHandle(p);
    return out;
}

DWORD WINAPI debug_thread(void* param) {
    const std::shared_ptr<DebugShared> shared = *std::unique_ptr<std::shared_ptr<DebugShared>>(
        static_cast<std::shared_ptr<DebugShared>*>(param));
    std::map<std::pair<DWORD, uintptr_t>, std::string> modules;
    for (;;) {
        AcquireSRWLockShared(&shared->lock);
        const bool stop = shared->stop, paused = shared->paused, selected = shared->selected;
        const std::wstring app = shared->app;
        const DWORD sel_pid = shared->sel_pid, sel_tid = shared->sel_tid;
        const long long sel_jid = shared->sel_jid;
        ReleaseSRWLockShared(&shared->lock);
        if (stop) break;
        if (!paused) {
            auto snap = std::make_unique<DebugSnapshot>();
            snap->app = app;
            snap->processes = debug_processes();
            std::set<DWORD> pids;
            for (const DebugProcess& p : snap->processes)
                if (app.empty() || p.app == app) pids.insert(p.pid);
            FILETIME now;
            GetSystemTimeAsFileTime(&now);
            snap->time = filetime_100ns(now);
            const auto os = debug_os_threads(pids, &modules);
            for (DebugProcess& p : snap->processes) {
                if (!pids.count(p.pid)) continue;
                winapk::debug::AgentReply reply;
                std::string text;
                if (p.runtime && debug_ask(p.pid, "threads", 1500, &text)) reply = winapk::debug::parse_threads(text);
                p.agent = reply.ok;
                const auto threads = os.find(p.pid);
                std::vector<ThreadRow> rows = winapk::launcher::merge_threads(
                    p.pid, wide_to_utf8(p.exe) + " (" + std::to_string(p.pid) + ")",
                    threads == os.end() ? std::vector<winapk::launcher::OsThread>{} : threads->second,
                    reply.ok ? &reply : nullptr);
                snap->rows.insert(snap->rows.end(), std::make_move_iterator(rows.begin()),
                                  std::make_move_iterator(rows.end()));
            }
            if (selected) {
                snap->stack_pid = sel_pid;
                snap->stack_tid = sel_tid;
                snap->stack_jid = sel_jid;
                for (const DebugProcess& p : snap->processes) {
                    if (p.pid != sel_pid) continue;
                    if (!p.runtime) {
                        snap->stack_state = StackState::kNotRuntime;
                    } else if (!p.agent) {
                        snap->stack_state = StackState::kNoAgent;
                    } else if (debug_ask(sel_pid, "stack " + std::to_string(sel_tid) + " " + std::to_string(sel_jid),
                                         2500, &snap->stack)) {
                        snap->stack_state = StackState::kText;
                    }
                }
            }
            DebugSnapshot* raw = snap.release();
            if (!PostMessageW(shared->hwnd, kDebugSnapshot, 0, reinterpret_cast<LPARAM>(raw))) delete raw;
        }
        WaitForSingleObject(shared->wake, 1000);
    }
    CloseHandle(shared->wake);
    return 0;
}

std::wstring debug_app_label(const std::wstring& app) {
    if (app.empty()) return tr(L"All apps");
    if (app == kDebugSystemApp) return tr(L"Win.apk services");
    const App* a = find_app(app);
    return a ? a->label : app;
}

const wchar_t* debug_kind_label(ThreadKind k) {
    switch (k) {
        case ThreadKind::kJava: return L"Java";
        case ThreadKind::kApp: return tr(L"App native");
        case ThreadKind::kJvm: return L"JVM";
        case ThreadKind::kRuntime: return L"Win.apk";
        case ThreadKind::kSystem: return tr(L"System");
        case ThreadKind::kUnknown: break;
    }
    return L"";
}

struct DebugColumn {
    const wchar_t* title;
    int width;  // dp
    bool right;
};
const DebugColumn kDebugColumns[] = {{L"Process", 176, false}, {L"Thread ID", 84, true}, {L"Name", 210, false},
                                     {L"Type", 96, false},     {L"State", 170, false},  {L"CPU %", 64, true},
                                     {L"CPU time", 84, true},  {L"Priority", 64, true}, {L"Started in", 220, false}};

std::wstring debug_cell(const ThreadRow& r, int column) {
    wchar_t buf[64];
    switch (static_cast<ThreadColumn>(column)) {
        case ThreadColumn::kProcess: return utf8_to_wide(r.process);
        case ThreadColumn::kTid: return r.tid ? std::to_wstring(r.tid) : L"–";
        case ThreadColumn::kName: return utf8_to_wide(r.name);
        case ThreadColumn::kKind: return debug_kind_label(r.kind);
        case ThreadColumn::kState: return utf8_to_wide(r.state) + (r.daemon ? L" · daemon" : L"");
        case ThreadColumn::kCpuPct:
            if (r.cpu_pct < 0) return L"";
            swprintf(buf, 64, L"%.1f", r.cpu_pct);
            return buf;
        case ThreadColumn::kCpuTime:
            if (!r.timed) return L"";
            swprintf(buf, 64, L"%.2f s", static_cast<double>(r.cpu) / 1e7);
            return buf;
        case ThreadColumn::kPriority: return r.tid ? std::to_wstring(r.priority) : L"";
        case ThreadColumn::kStart: return utf8_to_wide(r.start);
        case ThreadColumn::kCount: break;
    }
    return L"";
}

// The line under the tools: how many threads, of which kinds; or why there are none.
std::wstring debug_summary() {
    if (g_dv.first) return tr(L"Looking for running apps…");
    int processes = 0, no_agent = 0;
    for (const DebugProcess& p : g_dv.processes) {
        if (!g_dv.app.empty() && p.app != g_dv.app) continue;
        ++processes;
        if (p.runtime && !p.agent) ++no_agent;
    }
    if (!processes) {
        return g_dv.app.empty() ? std::wstring(tr(L"No app is running. Open an app and its threads appear here."))
                                : fmt(L"{0} is not running. Open it and its threads appear here.",
                                      {debug_app_label(g_dv.app)});
    }
    const winapk::launcher::ThreadCounts c = winapk::launcher::count_threads(g_dv.all);
    std::wstring s = fmt(L"Threads: {0}  ·  Processes: {1}", {std::to_wstring(c.total), std::to_wstring(processes)});
    auto add = [&](const wchar_t* label, int n) {
        if (n) s += std::wstring(L"  ·  ") + label + L" " + std::to_wstring(n);
    };
    add(L"Java", c.java);
    add(tr(L"App native"), c.app);
    add(L"JVM", c.jvm);
    add(L"Win.apk", c.runtime);
    add(tr(L"System"), c.system);
    add(tr(L"Not identified"), c.unknown);
    if (g_dv.rows.size() != g_dv.all.size()) s += L"  ·  " + fmt(L"{0} shown", {std::to_wstring(g_dv.rows.size())});
    if (no_agent) s += std::wstring(L"  ·  ") + tr(L"Reopen the app for Java names and stacks");
    if (g_dv.paused) s += std::wstring(L"  ·  ") + tr(L"Paused");
    return s;
}

void debug_set_stack(const std::wstring& text) {
    if (text == g_dv.stack_text) return;
    g_dv.stack_text = text;
    std::wstring crlf;
    crlf.reserve(text.size() + text.size() / 16);
    for (wchar_t c : text) {
        if (c == L'\n') crlf += L"\r\n";
        else if (c != L'\r') crlf += c;
    }
    SetWindowTextW(g_dv.stack, crlf.c_str());
}

int debug_selected_index() {
    if (!g_dv.selected) return -1;
    for (size_t i = 0; i < g_dv.rows.size(); ++i) {
        const ThreadRow& r = g_dv.rows[i];
        if (r.pid == g_dv.sel_pid && r.tid == g_dv.sel_tid && (r.tid || r.jid == g_dv.sel_jid)) return static_cast<int>(i);
    }
    return -1;
}

// The selection, told to the worker: its next look asks for that thread's stack.
void debug_share_selection(bool wake) {
    DebugShared& s = *g_dv.shared;
    AcquireSRWLockExclusive(&s.lock);
    s.selected = g_dv.selected;
    s.sel_pid = g_dv.sel_pid;
    s.sel_tid = g_dv.sel_tid;
    s.sel_jid = g_dv.sel_jid;
    s.app = g_dv.app;
    s.paused = g_dv.paused;
    ReleaseSRWLockExclusive(&s.lock);
    if (wake) SetEvent(s.wake);
}

// The rows the search leaves, sorted; the list shows them, with the same thread selected as before.
void debug_rebuild_rows() {
    g_dv.rows.clear();
    for (const ThreadRow& r : g_dv.all)
        if (winapk::launcher::row_matches(r, g_dv.query)) g_dv.rows.push_back(r);
    winapk::launcher::sort_rows(g_dv.rows, g_dv.sort, g_dv.descending);
    g_dv.syncing = true;
    ListView_SetItemCountEx(g_dv.list, g_dv.rows.size(), LVSICF_NOSCROLL | LVSICF_NOINVALIDATEALL);
    const int now = debug_selected_index();
    const int was = ListView_GetNextItem(g_dv.list, -1, LVNI_SELECTED);
    if (was != now) {
        ListView_SetItemState(g_dv.list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        if (now >= 0) ListView_SetItemState(g_dv.list, now, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }
    g_dv.syncing = false;
    InvalidateRect(g_dv.list, nullptr, FALSE);
    InvalidateRect(g_debug_wnd, nullptr, FALSE);
}

void debug_apply(const DebugSnapshot& snap) {
    if (snap.app != g_dv.app || g_dv.paused) return;  // taken for another app, or before Pause
    g_dv.first = false;
    g_dv.processes = snap.processes;
    g_dv.all = snap.rows;
    g_dv.cpu.apply(g_dv.all, snap.time);
    debug_rebuild_rows();
    if (!g_dv.selected) {
        debug_set_stack(tr(L"Select a thread to see its stack."));
        return;
    }
    if (snap.stack_pid != g_dv.sel_pid || snap.stack_tid != g_dv.sel_tid || snap.stack_jid != g_dv.sel_jid) return;
    switch (snap.stack_state) {
        case StackState::kText: debug_set_stack(utf8_to_wide(snap.stack)); break;
        case StackState::kNoAgent:
            debug_set_stack(tr(L"This process has no debug agent: it was opened before debug mode was turned on, or "
                               L"it does not answer. Stop the app and open it again to see Java names and stacks."));
            break;
        case StackState::kNotRuntime:
            debug_set_stack(tr(L"Stacks are shown for the processes that run the app's code. This one is a helper "
                               L"the app started."));
            break;
        case StackState::kNone: debug_set_stack(tr(L"The thread has ended.")); break;
    }
}

void paint_debug(Graphics& g, float W, float H) {
    g.Clear(g_t.layer);
    g_dv.targets.clear();
    const float m = px(16), ch = px(32), cy = (px(kLogTools) - ch) / 2;
    // The picker: which app.
    const RectF pick(m, cy, (std::min)(px(260), W * 0.3f), ch);
    fill_round(g, pick, px(kRadiusControl), g_dv.hot == kDebugPick ? g_t.hover : g_t.card);
    stroke_round(g, pick, px(kRadiusControl), g_t.border);
    draw_text(g, debug_app_label(g_dv.app), g_f.body.get(), g_t.text, RectF(pick.X + px(12), pick.Y, pick.Width - px(40), ch));
    draw_text(g, kGlyphChevron, g_f.glyph12.get(), g_t.text2, RectF(pick.GetRight() - px(30), pick.Y, px(22), ch),
              kCenter);
    g_dv.targets.push_back({pick, kDebugPick});
    float x = W - m;
    for (auto [label, act] : {std::pair<const wchar_t*, int>{tr(L"Copy"), kDebugCopy},
                              {g_dv.paused ? tr(L"Resume") : tr(L"Pause"), kDebugPause}}) {
        const float w = text_width(g, label, g_f.body.get()) + px(32);
        x -= w;
        const RectF r(x, cy, w, ch);
        draw_button(g, r, label, g_dv.hot == act, false, false);
        g_dv.targets.push_back({r, act});
        x -= px(8);
    }
    // The search box between them (the edit is a child; this is its frame).
    const RectF box(pick.GetRight() + px(12), cy, (std::max)(px(80), x - px(4) - pick.GetRight() - px(12)), ch);
    const bool focused = GetFocus() == g_dv.search;
    fill_round(g, box, px(kRadiusControl), g_t.card);
    stroke_round(g, box, px(kRadiusControl), focused ? g_t.accent : g_t.border, focused ? px(1.5f) : 1);
    draw_text(g, kGlyphSearch, g_f.glyph12.get(), g_t.text2, RectF(box.X + px(10), box.Y, px(16), ch), kCenter);
    draw_text(g, debug_summary(), g_f.caption.get(), g_t.text2,
              RectF(m, px(kLogTools) - px(4), W - 2 * m, px(kDebugSummary)));
    (void)H;
}

void layout_debug(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int top = ipx(kLogTools + kDebugSummary);
    const int rest = (std::max)(0L, rc.bottom - top);
    const int stack_h = (std::min)(rest, (std::max)(ipx(150), rest * 36 / 100));
    MoveWindow(g_dv.list, 0, top, rc.right, rest - stack_h, TRUE);
    MoveWindow(g_dv.stack, 0, top + rest - stack_h + 1, rc.right, (std::max)(0, stack_h - 1), TRUE);
    // The last column takes the width that is left, so no bare header shows beside it.
    int used = 0;
    const int last = static_cast<int>(std::size(kDebugColumns)) - 1;
    for (int c = 0; c < last; ++c) used += ListView_GetColumnWidth(g_dv.list, c);
    ListView_SetColumnWidth(g_dv.list, last,
                            (std::max)(ipx(static_cast<float>(kDebugColumns[last].width)),
                                       static_cast<int>(rc.right) - used - GetSystemMetrics(SM_CXVSCROLL)));
    HDC dc = GetDC(hwnd);
    float buttons = 0;
    {
        Graphics g(dc);
        for (const wchar_t* label : {tr(L"Copy"), g_dv.paused ? tr(L"Resume") : tr(L"Pause")})
            buttons += text_width(g, label, g_f.body.get()) + px(40);
    }
    ReleaseDC(hwnd, dc);
    const float W = static_cast<float>(rc.right), m = px(16);
    const float pick_r = m + (std::min)(px(260), W * 0.3f);
    const float bx = pick_r + px(12), bw = (std::max)(px(80), W - m - buttons - px(4) - bx);
    const int eh = ipx(20);
    MoveWindow(g_dv.search, static_cast<int>(bx + px(32)), static_cast<int>((px(kLogTools) - eh) / 2),
               static_cast<int>(bw - px(32) - px(12)), eh, TRUE);
}

void debug_pick_app(HWND hwnd) {
    std::vector<std::wstring> apps = {L""};
    for (const DebugProcess& p : g_dv.processes)
        if (!p.app.empty() && std::find(apps.begin(), apps.end(), p.app) == apps.end()) apps.push_back(p.app);
    if (std::find(apps.begin(), apps.end(), g_dv.app) == apps.end()) apps.push_back(g_dv.app);
    std::sort(apps.begin() + 1, apps.end(), [](const std::wstring& a, const std::wstring& b) {
        return _wcsicmp(debug_app_label(a).c_str(), debug_app_label(b).c_str()) < 0;
    });
    HMENU menu = CreatePopupMenu();
    for (size_t i = 0; i < apps.size(); ++i)
        AppendMenuW(menu, MF_STRING | (apps[i] == g_dv.app ? MF_CHECKED : 0), i + 1, debug_app_label(apps[i]).c_str());
    const RectF& r = g_dv.targets[0].first;
    POINT pt{static_cast<LONG>(r.X), static_cast<LONG>(r.GetBottom() + px(2))};
    ClientToScreen(hwnd, &pt);
    const int pick = TrackPopupMenu(menu, TPM_RETURNCMD | menu_flags(true), pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
    if (pick <= 0 || apps[static_cast<size_t>(pick - 1)] == g_dv.app) return;
    g_dv.app = apps[static_cast<size_t>(pick - 1)];
    g_dv.all.clear();
    g_dv.selected = false;
    g_dv.first = true;
    g_dv.cpu.reset();
    debug_set_stack(L"");
    debug_rebuild_rows();
    debug_share_selection(true);
}

void debug_action(HWND hwnd, int act) {
    if (act == kDebugPick) {
        debug_pick_app(hwnd);
    } else if (act == kDebugPause) {
        g_dv.paused = !g_dv.paused;
        g_dv.cpu.reset();
        debug_share_selection(true);
        layout_debug(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
    } else if (act == kDebugCopy) {
        std::wstring text = debug_app_label(g_dv.app) + L"\r\n" + debug_summary() + L"\r\n\r\n";
        for (size_t c = 0; c < std::size(kDebugColumns); ++c) text += std::wstring(c ? L"\t" : L"") + kDebugColumns[c].title;
        text += L"\r\n";
        for (const ThreadRow& r : g_dv.rows) {
            for (size_t c = 0; c < std::size(kDebugColumns); ++c)
                text += (c ? L"\t" : L"") + debug_cell(r, static_cast<int>(c));
            text += L"\r\n";
        }
        if (g_dv.selected) {
            const int len = GetWindowTextLengthW(g_dv.stack);
            std::wstring stack(static_cast<size_t>(len) + 1, L'\0');
            GetWindowTextW(g_dv.stack, &stack[0], len + 1);
            stack.resize(static_cast<size_t>(len));
            text += L"\r\n" + stack;
        }
        copy_text(hwnd, text);
    }
}

LRESULT CALLBACK debug_search_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) {
        if (GetWindowTextLengthW(hwnd) > 0) SetWindowTextW(hwnd, L"");
        else DestroyWindow(g_debug_wnd);
        return 0;
    }
    if (msg == WM_KEYDOWN && (wp == VK_DOWN || wp == VK_RETURN)) {
        SetFocus(g_dv.list);
        return 0;
    }
    if (msg == WM_CHAR && (wp == VK_RETURN || wp == VK_ESCAPE)) return 0;  // no beep
    return DefSubclassProc(hwnd, msg, wp, lp);
}

// The list's header in the window's colours (a themed header stays light in a dark window), with the
// sort's direction on its column. The header's notifications go to the list, its parent: caught here.
LRESULT CALLBACK debug_list_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_NOTIFY) {
        auto* cd = reinterpret_cast<NMCUSTOMDRAW*>(lp);
        if (cd->hdr.code == NM_CUSTOMDRAW && cd->hdr.hwndFrom == ListView_GetHeader(hwnd)) {
            if (cd->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW | CDRF_NOTIFYPOSTPAINT;
            if (cd->dwDrawStage == CDDS_POSTPAINT) {  // what is left of the header beside the last column
                RECT all, last{};
                GetClientRect(cd->hdr.hwndFrom, &all);
                const int n = Header_GetItemCount(cd->hdr.hwndFrom);
                if (n > 0) Header_GetItemRect(cd->hdr.hwndFrom, n - 1, &last);
                if (last.right < all.right) {
                    all.left = last.right;
                    HBRUSH back = CreateSolidBrush(colorref(g_t.card));
                    FillRect(cd->hdc, &all, back);
                    DeleteObject(back);
                }
                return CDRF_DODEFAULT;
            }
            if (cd->dwDrawStage == CDDS_ITEMPREPAINT) {
                const size_t col = static_cast<size_t>(cd->dwItemSpec);
                HBRUSH back = CreateSolidBrush(colorref(g_t.card));
                FillRect(cd->hdc, &cd->rc, back);
                DeleteObject(back);
                HBRUSH line = CreateSolidBrush(colorref(g_t.border));
                RECT edge = cd->rc;
                edge.top = edge.bottom - 1;
                FillRect(cd->hdc, &edge, line);
                edge = cd->rc;
                edge.left = edge.right - 1;
                FillRect(cd->hdc, &edge, line);
                DeleteObject(line);
                if (col < std::size(kDebugColumns)) {
                    std::wstring title = tr(kDebugColumns[col].title);
                    if (static_cast<size_t>(g_dv.sort) == col) title += g_dv.descending ? L"  ▼" : L"  ▲";
                    RECT text = cd->rc;
                    InflateRect(&text, -ipx(8), 0);
                    SetBkMode(cd->hdc, TRANSPARENT);
                    SetTextColor(cd->hdc, colorref(g_t.text2));
                    HGDIOBJ old = SelectObject(cd->hdc, g_edit_font);
                    DrawTextW(cd->hdc, title.c_str(), -1, &text,
                              DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX |
                                  (kDebugColumns[col].right ? DT_RIGHT : DT_LEFT));
                    SelectObject(cd->hdc, old);
                }
                return CDRF_SKIPDEFAULT;
            }
        }
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

LRESULT CALLBACK debug_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            // Thread names, modules and stack frames read left to right, as a log does (log_proc).
            SetWindowLongPtrW(hwnd, GWL_EXSTYLE, GetWindowLongPtrW(hwnd, GWL_EXSTYLE) | WS_EX_NOINHERITLAYOUT);
            static bool classes = false;
            if (!classes) {
                INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_LISTVIEW_CLASSES};
                InitCommonControlsEx(&icc);
                classes = true;
            }
            g_dv.list = CreateWindowExW(0, WC_LISTVIEWW, L"",
                                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_OWNERDATA | LVS_SINGLESEL |
                                            LVS_SHOWSELALWAYS,
                                        0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdDebugList)),
                                        g_inst, nullptr);
            ListView_SetExtendedListViewStyle(g_dv.list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
            SetWindowTheme(g_dv.list, g_t.dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
            ListView_SetBkColor(g_dv.list, colorref(g_t.layer));
            ListView_SetTextBkColor(g_dv.list, colorref(g_t.layer));
            ListView_SetTextColor(g_dv.list, colorref(g_t.text));
            SendMessageW(g_dv.list, WM_SETFONT, reinterpret_cast<WPARAM>(g_edit_font), FALSE);
            for (size_t c = 0; c < std::size(kDebugColumns); ++c) {
                LVCOLUMNW col{};
                col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
                col.pszText = const_cast<wchar_t*>(tr(kDebugColumns[c].title));
                col.cx = ipx(static_cast<float>(kDebugColumns[c].width));
                col.fmt = kDebugColumns[c].right ? LVCFMT_RIGHT : LVCFMT_LEFT;
                ListView_InsertColumn(g_dv.list, static_cast<int>(c), &col);
            }
            SetWindowSubclass(g_dv.list, debug_list_proc, 1, 0);
            g_dv.stack = CreateWindowExW(WS_EX_LTRREADING | WS_EX_LEFT, L"EDIT", L"",
                                         WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_LEFT | ES_MULTILINE |
                                             ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_NOHIDESEL,
                                         0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdDebugStack)),
                                         g_inst, nullptr);
            SendMessageW(g_dv.stack, WM_SETFONT, reinterpret_cast<WPARAM>(g_mono_font), FALSE);
            SendMessageW(g_dv.stack, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(ipx(12), ipx(12)));
            SetWindowTheme(g_dv.stack, g_t.dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
            g_dv.search = CreateWindowExW(g_rtl ? WS_EX_RTLREADING : 0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                          0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdDebugSearch)),
                                          g_inst, nullptr);
            SendMessageW(g_dv.search, WM_SETFONT, reinterpret_cast<WPARAM>(g_edit_font), FALSE);
            SendMessageW(g_dv.search, EM_SETCUEBANNER, FALSE,
                         reinterpret_cast<LPARAM>(tr(L"Search threads: a name, a type, a library")));
            SetWindowSubclass(g_dv.search, debug_search_proc, 1, 0);
            g_dv.shared = std::make_shared<DebugShared>();
            g_dv.shared->hwnd = hwnd;
            g_dv.shared->app = g_dv.app;
            g_dv.shared->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            auto* param = new std::shared_ptr<DebugShared>(g_dv.shared);
            if (HANDLE t = CreateThread(nullptr, 0, debug_thread, param, 0, nullptr)) CloseHandle(t);
            else delete param;
            return 0;
        }
        case WM_SIZE: layout_debug(hwnd); InvalidateRect(hwnd, nullptr, FALSE); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: buffered_paint(hwnd, paint_debug); return 0;
        case WM_CTLCOLOREDIT:
            SetTextColor(reinterpret_cast<HDC>(wp), colorref(g_t.text));
            SetBkColor(reinterpret_cast<HDC>(wp), colorref(g_t.card));
            return reinterpret_cast<LRESULT>(g_edit_brush);
        case WM_CTLCOLORSTATIC:  // the read-only stack
            SetTextColor(reinterpret_cast<HDC>(wp), colorref(g_t.text));
            SetBkColor(reinterpret_cast<HDC>(wp), colorref(code_bg()));
            return reinterpret_cast<LRESULT>(g_code_brush);
        case kDebugSnapshot: {
            const std::unique_ptr<DebugSnapshot> snap(reinterpret_cast<DebugSnapshot*>(lp));
            debug_apply(*snap);
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wp) == kIdDebugSearch) {
                if (HIWORD(wp) == EN_CHANGE) {
                    wchar_t buf[256] = {};
                    GetWindowTextW(g_dv.search, buf, 256);
                    g_dv.query = winapk::launcher::lower(wide_to_utf8(buf));
                    debug_rebuild_rows();
                } else if (HIWORD(wp) == EN_SETFOCUS || HIWORD(wp) == EN_KILLFOCUS) {
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            return 0;
        case WM_NOTIFY: {
            auto* hdr = reinterpret_cast<NMHDR*>(lp);
            if (hdr->hwndFrom != g_dv.list) break;
            if (hdr->code == LVN_GETDISPINFOW) {
                auto* info = reinterpret_cast<NMLVDISPINFOW*>(lp);
                const size_t i = static_cast<size_t>(info->item.iItem);
                if ((info->item.mask & LVIF_TEXT) && i < g_dv.rows.size() && info->item.cchTextMax > 0) {
                    const std::wstring cell = debug_cell(g_dv.rows[i], info->item.iSubItem);
                    lstrcpynW(info->item.pszText, cell.c_str(), info->item.cchTextMax);
                }
                return 0;
            }
            if (hdr->code == LVN_COLUMNCLICK) {
                const int col = reinterpret_cast<NMLISTVIEW*>(lp)->iSubItem;
                if (col < 0 || col >= static_cast<int>(ThreadColumn::kCount)) return 0;
                if (g_dv.sort == static_cast<ThreadColumn>(col)) {
                    g_dv.descending = !g_dv.descending;
                } else {
                    g_dv.sort = static_cast<ThreadColumn>(col);
                    // Numbers are looked at from the top down: the busiest thread first.
                    g_dv.descending = g_dv.sort == ThreadColumn::kCpuPct || g_dv.sort == ThreadColumn::kCpuTime ||
                                      g_dv.sort == ThreadColumn::kPriority;
                }
                debug_rebuild_rows();
                InvalidateRect(ListView_GetHeader(g_dv.list), nullptr, TRUE);
                return 0;
            }
            if (hdr->code == LVN_ITEMCHANGED && !g_dv.syncing) {
                const auto* change = reinterpret_cast<NMLISTVIEW*>(lp);
                if (!(change->uChanged & LVIF_STATE) || !((change->uNewState ^ change->uOldState) & LVIS_SELECTED))
                    return 0;
                const int at = ListView_GetNextItem(g_dv.list, -1, LVNI_SELECTED);
                g_dv.selected = at >= 0 && static_cast<size_t>(at) < g_dv.rows.size();
                if (g_dv.selected) {
                    const ThreadRow& r = g_dv.rows[static_cast<size_t>(at)];
                    g_dv.sel_pid = r.pid;
                    g_dv.sel_tid = r.tid;
                    g_dv.sel_jid = r.jid;
                    debug_set_stack(g_dv.paused ? tr(L"Paused. Resume to see this thread's stack.") : L"…");
                } else {
                    debug_set_stack(tr(L"Select a thread to see its stack."));
                }
                debug_share_selection(true);
                return 0;
            }
            if (hdr->code == NM_CUSTOMDRAW) {  // a row in its kind's colour: the app's own threads stand out
                auto* cd = reinterpret_cast<NMLVCUSTOMDRAW*>(lp);
                if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
                if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                    const size_t i = static_cast<size_t>(cd->nmcd.dwItemSpec);
                    if (i < g_dv.rows.size()) {
                        const ThreadKind k = g_dv.rows[i].kind;
                        cd->clrText = colorref(k == ThreadKind::kJava   ? g_t.text
                                               : k == ThreadKind::kApp ? g_t.accent
                                                                       : g_t.text2);
                    }
                    return CDRF_NEWFONT;
                }
            }
            break;
        }
        case WM_MOUSEMOVE: track_hot(hwnd, lp, g_dv.targets, &g_dv.hot, &g_dv.tracking); return 0;
        case WM_MOUSELEAVE:
            g_dv.tracking = false;
            g_dv.hot = 0;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT && reinterpret_cast<HWND>(wp) == hwnd) {
                SetCursor(LoadCursorW(nullptr, g_dv.hot ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_LBUTTONUP: debug_action(hwnd, find_target(g_dv.targets, lp)); return 0;
        case WM_ACTIVATE:
            if (LOWORD(wp) != WA_INACTIVE && GetFocus() != g_dv.list && GetFocus() != g_dv.stack) SetFocus(g_dv.search);
            return 0;
        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {ipx(640), ipx(420)};
            return 0;
        case WM_DESTROY:
            if (g_dv.shared) {
                AcquireSRWLockExclusive(&g_dv.shared->lock);
                g_dv.shared->stop = true;
                ReleaseSRWLockExclusive(&g_dv.shared->lock);
                SetEvent(g_dv.shared->wake);
            }
            g_debug_wnd = nullptr;
            g_dv = DebugView{};
            maybe_quit();
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// The Threads window on `app` (a library name); every running app when none is named.
void show_debugger(const std::wstring& app) {
    if (g_debug_wnd) {
        if (g_dv.app != app) {
            g_dv.app = app;
            g_dv.all.clear();
            g_dv.selected = false;
            g_dv.first = true;
            g_dv.cpu.reset();
            debug_set_stack(L"");
            debug_rebuild_rows();
            debug_share_selection(true);
        }
        ShowWindow(g_debug_wnd, SW_RESTORE);
        SetForegroundWindow(g_debug_wnd);
        return;
    }
    g_dv.app = app;
    g_debug_wnd = open_tool_window(L"WinApkThreads", tr(L"Threads - Win.apk"), ipx(1200), ipx(720), WS_OVERLAPPEDWINDOW);
    if (!g_debug_wnd) return;
    layout_debug(g_debug_wnd);
    debug_set_stack(tr(L"Select a thread to see its stack."));
}

// ---------------------------------------------------------------------------
// The launch splash: shown the moment an app is opened, as a phone shows one,
// until the app's own window is up (the runtime's ready event, kRunReady).
// If the app ends first it turns into a short error with Show log and Report a problem.

struct Splash {
    std::wstring name;
    HWND hwnd = nullptr;
    ULONGLONG start = 0, phase_read = 0;
    const wchar_t* phase = nullptr;  // what the run log says it is doing; null: starting
    bool failed = false;
    std::wstring error_title, error_text;
    fs::path log;  // Show log's
    float need_h = 0;  // the failed state's height, when its sentence is long
    float need_w = 0;  // and its width, when its title is long ("<a long name> stopped with an error")
    std::unique_ptr<Gdiplus::Bitmap> icon;  // the app's, at the starting window's size
    Targets targets;
    int hot = 0;
    bool tracking = false;
    // Input given to the app while it has no window (a key, a click on its starting window): when, and
    // whether its "isn't responding" question is on show. InputDispatcher holds such input for the app
    // that is starting and, five seconds on, reports it not responding ("does not have a focused window").
    ULONGLONG input_at = 0;
    bool asking = false;
};

// InputDispatcher's DEFAULT_DISPATCHING_TIMEOUT: input an app has not taken in this long is an ANR.
constexpr ULONGLONG kInputDispatchingTimeoutMs = 5000;

std::map<std::wstring, std::unique_ptr<Splash>> g_splashes;

bool any_splash() { return !g_splashes.empty(); }
enum { kSplashClose = 1, kSplashLog = 2, kSplashCancel = 3, kSplashReport = 4 };
constexpr UINT kFitSplash = WM_APP + 21;

void maybe_quit();
bool work_keeps_process();
std::set<DWORD> windowed_processes();

Splash* splash_of(HWND hwnd) { return reinterpret_cast<Splash*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA)); }

// The step of a start that takes long, from the lines the runtime writes; null when there is none to tell.
const wchar_t* run_phase(const fs::path& log) {
    const std::string tail = read_tail(log, 64 * 1024);
    if (tail.find("converting the app's code") != std::string::npos &&
        tail.find("host natives:") == std::string::npos) {
        return L"Converting the app's code. This happens once.";
    }
    if (tail.find("installing the app (once per install)") != std::string::npos &&
        tail.find("host natives:") == std::string::npos) {
        return L"Getting the app ready. This happens once.";
    }
    // The first start after a new Win.apk's first install waits for the framework archive that install
    // started (host/cds.cpp, wait_for_running_dump).
    if (tail.find("this start waits for it") != std::string::npos && tail.find("host natives:") == std::string::npos)
        return L"Getting Win.apk ready. This happens once.";
    return nullptr;
}

// Why the app did not come up, in a sentence: the log's own reason when it has a known one.
std::wstring failure_reason(const fs::path& log) {
    // No runtime started at all: "the app stopped with an error" would blame the app (as an install says).
    std::error_code ec;
    if (!fs::exists(runtime_exe(), ec)) return tr(L"Win.apk's runtime is missing. Install Win.apk again.");
    bool no_native_code = false;
    log_summary(log, &no_native_code);
    if (no_native_code) {
        return tr(L"This file has no code for PCs. Install the full bundle (.apkm, .xapk or .apks) that includes "
                  L"the arm64-v8a part.");
    }
    // Not the app's fault: say so, rather than "the app stopped with an error".
    if (std::wstring why = runtime_failure_reason(log); !why.empty()) return why;
    return tr(L"The app stopped with an error.");
}

void paint_splash(Splash& s, Graphics& g, float W, float H) {
    g.Clear(g_t.layer);
    s.targets.clear();
    const App* app = find_app(s.name);
    const std::wstring label = app ? app->label : job_label(s.name);
    Gdiplus::Bitmap* icon = s.icon ? s.icon.get() : app ? icon_for(*app) : nullptr;
    const float m = px(24);
    if (!s.failed) {
        // The app's starting window, as Android draws one: its icon in the middle of its own window, and
        // nothing else unless the start takes long. Three dots that brighten in turn say it is alive.
        const float is = (std::min)(px(96), (std::min)(W, H) * 0.4f);
        const RectF ir((W - is) / 2, (H - is) / 2 - px(16), is, is);
        draw_icon(g, icon, label, ir);
        const float t = static_cast<float>(GetTickCount64() % 1200) / 1200.0f, d = px(7), gap = px(8);
        const float x0 = (W - 3 * d - 2 * gap) / 2, y = ir.GetBottom() + px(28);
        for (int i = 0; i < 3; ++i) {
            float phase = t - i * 0.17f;
            phase -= std::floor(phase);
            const float lit = phase < 0.4f ? std::sin(phase / 0.4f * 3.14159f) : 0.0f;
            Gdiplus::SolidBrush b(with_alpha(g_t.accent, static_cast<BYTE>(70 + 185 * lit)));
            g.FillEllipse(&b, x0 + i * (d + gap), y, d, d);
        }
        std::wstring sub = s.phase ? std::wstring(tr(s.phase)) : std::wstring();
        if (!s.phase && GetTickCount64() - s.start > 20000) sub = tr(L"Still starting. The first start can take a minute.");
        if (!sub.empty()) {
            draw_text(g, sub, g_f.caption.get(), g_t.text2, RectF(m, y + d + px(14), W - 2 * m, px(40)), kCenter, true,
                      false);
        }
        return;
    }
    // The error, a block of the old splash's size in the middle of the window, taller when its sentence
    // is long (kFitSplash also grows a window too small for it).
    const float block = (std::max)(px(300), s.need_h);
    const float top = (std::max)(0.0f, (H - block) / 2);
    const float is = px(88);
    const RectF ir((W - is) / 2, top + px(40), is, is);
    draw_icon(g, icon, label, ir);
    const RectF badge(ir.GetRight() - px(20), ir.GetBottom() - px(20), px(26), px(26));
    Gdiplus::SolidBrush ring(g_t.layer), dot(g_t.danger);
    g.FillEllipse(&ring, inset(badge, -px(2), -px(2)));
    g.FillEllipse(&dot, badge);
    draw_text(g, L"!", g_f.body_semi.get(), kWhite, badge, kCenter);
    draw_text(g, s.error_title, g_f.title.get(), g_t.text, RectF(m, top + px(144), W - 2 * m, px(30)), kCenter);
    draw_text(g, s.error_text, g_f.caption.get(), g_t.text2, RectF(m, top + px(178), W - 2 * m, px(54)), kCenter, true,
              false);
    // Report a problem as a link under the sentence, as under a failed install's reason.
    const float sh = (std::min)(px(54), text_height(g, s.error_text, g_f.caption.get(), W - 2 * m));
    const std::wstring report = tr(L"Report a problem");
    const float rw = text_width(g, report, g_f.body.get()) + px(4);
    const RectF rep((W - rw) / 2, top + px(178) + sh + px(6), rw, px(22));
    draw_text(g, report, g_f.body.get(), s.hot == kSplashReport ? mix(g_t.accent, g_t.text, 0.3f) : g_t.accent, rep,
              kCenter);
    const float need = (std::max)(px(300), rep.GetBottom() - top + px(16) + px(32) + px(24));
    // As wide as the title, up to 560: a long app name is not cut to an ellipsis.
    const float need_w = (std::min)(px(560), (std::max)(px(300), text_width(g, s.error_title, g_f.title.get()) +
                                                                   2 * m + px(8)));
    // Windows' order and the dialog's widths: Close (the accent one), then Show log.
    const float cw = button_width(g, tr(L"Close"), true), lw = button_width(g, tr(L"Show log"), false);
    const float by = top + block - px(24) - px(32);
    const RectF close((W - cw - lw - px(8)) / 2, by, cw, px(32)), log(close.GetRight() + px(8), by, lw, px(32));
    draw_button(g, log, tr(L"Show log"), s.hot == kSplashLog, false, false);
    draw_button(g, close, tr(L"Close"), s.hot == kSplashClose, true, false);
    s.targets.push_back({rep, kSplashReport});
    s.targets.push_back({log, kSplashLog});
    s.targets.push_back({close, kSplashClose});
    if (std::lround(need) != std::lround(s.need_h) || std::lround(need_w) != std::lround(s.need_w)) {
        s.need_h = need;
        s.need_w = need_w;
        PostMessageW(s.hwnd, kFitSplash, 0, 0);
    }
}

void splash_action(Splash& s, int act) {
    if (act == kSplashLog) {
        show_logs(s.log);
        DestroyWindow(s.hwnd);
    } else if (act == kSplashReport) {
        const App* app = find_app(s.name);
        report_problem(s.hwnd, s.name, app ? app->label : job_label(s.name), s.log, s.error_text);
    } else if (act == kSplashClose) {
        DestroyWindow(s.hwnd);
    } else if (act == kSplashCancel) {
        // Whether the runtime is up, still starting or already ending, its exit is now no failure.
        stop_run(s.name);
        expect_run_end(s.name);
        DestroyWindow(s.hwnd);
    }
}

LRESULT CALLBACK splash_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Splash* s = splash_of(hwnd);
    switch (msg) {
        case WM_NCCREATE:
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
            break;
        case WM_CREATE: SetTimer(hwnd, 1, 40, nullptr); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT:
            buffered_paint(hwnd, [&](Graphics& g, float w, float h) { paint_splash(*s, g, w, h); });
            return 0;
        case kFitSplash: {  // the error needs a client area of its title's width (300 dp at least) by its own height
            RECT wr, rc;
            GetWindowRect(hwnd, &wr);
            GetClientRect(hwnd, &rc);
            const int want_h = static_cast<int>(std::lround((std::max)(px(300), s->need_h)));
            const int want_w = static_cast<int>(std::lround((std::max)(px(300), s->need_w)));
            const int grow_w = (std::max)(0, want_w - static_cast<int>(rc.right));
            const int grow_h = (std::max)(0, want_h - static_cast<int>(rc.bottom));
            if (grow_w || grow_h) {
                SetWindowPos(hwnd, nullptr, wr.left - grow_w / 2, wr.top - grow_h / 2, wr.right - wr.left + grow_w,
                             wr.bottom - wr.top + grow_h, SWP_NOZORDER | SWP_NOACTIVATE);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_TIMER:
            if (!s->failed && GetTickCount64() - s->phase_read > 500) {
                s->phase = run_phase(run_log(s->name));
                s->phase_read = GetTickCount64();
                // Converting the app's code or readying Win.apk is an install's work on a phone, before any
                // input reaches the app: no clock runs for it.
                if (s->phase) s->input_at = 0;
            }
            if (!s->failed) InvalidateRect(hwnd, nullptr, FALSE);
            if (!s->failed && s->input_at && !s->asking &&
                GetTickCount64() - s->input_at >= kInputDispatchingTimeoutMs) {
                // The app was given input and has not shown a window to take it in five seconds: "<app>
                // isn't responding", over its starting window. The window coming up meanwhile ends the
                // question (it is owned by the starting window, which then goes).
                s->asking = true;
                const std::wstring name = s->name, label = label_of(name);
                Prompt p = app_end_prompt(name, label, fmt(L"{0} isn't responding", {label}));
                RECT r;
                GetWindowRect(hwnd, &r);
                const int answer = ask_not_responding(p, hwnd, r);
                const auto it = g_splashes.find(name);
                if (it == g_splashes.end() || it->second->hwnd != hwnd) return 0;  // the app came up, or ended
                s->asking = false;
                s->input_at = 0;  // Wait: the input is dropped, as InputDispatcher drops it; new input starts again
                if (answer == kCloseApp) splash_action(*s, kSplashCancel);
            }
            return 0;
        case WM_NCHITTEST: {  // the app's window to be: its frame, but not one to resize
            const LRESULT hit = DefWindowProcW(hwnd, msg, wp, lp);
            return hit >= HTLEFT && hit <= HTBOTTOMRIGHT ? HTBORDER : hit;
        }
        case WM_SYSCOMMAND:  // nor to maximize: it has the size the app's window opens at
            if ((wp & 0xFFF0) == SC_MAXIMIZE || (wp & 0xFFF0) == SC_SIZE) return 0;
            break;
        case WM_MOUSEMOVE: track_hot(hwnd, lp, s->targets, &s->hot, &s->tracking); return 0;
        case WM_MOUSELEAVE:
            s->tracking = false;
            s->hot = 0;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                SetCursor(LoadCursorW(nullptr, s->hot ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_LBUTTONDOWN:  // a click on the app's starting window is input for the app
            if (!s->failed && !s->phase && !s->input_at) s->input_at = GetTickCount64();
            return 0;
        case WM_LBUTTONUP: splash_action(*s, find_target(s->targets, lp)); return 0;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) splash_action(*s, s->failed ? kSplashClose : kSplashCancel);
            else if (wp == VK_RETURN && s->failed) splash_action(*s, kSplashClose);
            else if (!s->failed && !s->phase && !s->input_at) s->input_at = GetTickCount64();  // a key for the app
            return 0;
        case WM_CLOSE:  // its close button, as the app's own: the start is cancelled
            splash_action(*s, s->failed ? kSplashClose : kSplashCancel);
            return 0;
        case WM_DESTROY: {
            const std::wstring name = s->name;
            g_splashes.erase(name);  // frees *s
            maybe_quit();
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// The window's own System.AppUserModel.ID: the taskbar then counts it as the app's (one button with the
// app's window, under the app's shortcut), not as Win.apk's.
void set_window_app_id(HWND hwnd, const std::wstring& aumid) {
    static const PROPERTYKEY kAppUserModelId = {
        {0x9f4c2855, 0x9f79, 0x4b39, {0xa8, 0xd0, 0xe1, 0xd4, 0x2d, 0xe1, 0xd5, 0xf3}}, 5};
    IPropertyStore* props = nullptr;
    if (FAILED(SHGetPropertyStoreForWindow(hwnd, IID_IPropertyStore, reinterpret_cast<void**>(&props))) || !props) {
        return;
    }
    PROPVARIANT v{};
    v.vt = VT_LPWSTR;
    v.pwszVal = const_cast<wchar_t*>(aumid.c_str());  // read, not freed: no PropVariantClear
    if (SUCCEEDED(props->SetValue(kAppUserModelId, v))) props->Commit();
    props->Release();
}

// The outer size of the window the app `name` will open with on a monitor at `dpi` with work area `wa`:
// the runtime's own arithmetic (host/window_fit.h) on the phone size it will be given, so that the
// starting window is the app's window in everything but its content.
SIZE app_window_size(const std::wstring& name, UINT dpi, const RECT& wa) {
    int w = 360, h = 800;
    runtime_phone_size(name, &w, &h);
    // The size the user last gave this app's window comes first, as in the runtime (launch_phone_size).
    if (std::ifstream f(library_dir() / (name + L".window")); f) {
        int sw = 0, sh = 0;
        char x = 0;
        if (f >> sw >> x >> sh && x == 'x' && sw >= 200 && sh >= 200 && sw <= 4000 && sh <= 4000) {
            w = sw;
            h = sh;
        }
    }
    RECT frame{0, 0, 0, 0};
    AdjustWindowRectExForDpi(&frame, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    const int fw = frame.right - frame.left, fh = frame.bottom - frame.top, m = MulDiv(4, static_cast<int>(dpi), 96);
    int cw = 0, ch = 0;
    winapk::host::window_launch_client(w, h, static_cast<int>(dpi), (wa.right - wa.left) - fw - 2 * m,
                                       (wa.bottom - wa.top) - fh - 2 * m, &cw, &ch);
    return {cw + fw, ch + fh};
}

// The starting window for `name`, made now: where the app's window will open and of its size, with the
// app's name and icon on its frame and on the taskbar, so that it is the app's window to the eye from the
// first moment; the runtime opens the real one over it (WINAPK_START_WINDOW) and it goes. `compact`: only
// to say how a run ended, when no start is on show: the old splash's size.
Splash* show_splash(const std::wstring& name, bool compact) {
    if (auto it = g_splashes.find(name); it != g_splashes.end()) return it->second.get();
    auto s = std::make_unique<Splash>();
    s->name = name;
    s->start = GetTickCount64();
    s->log = run_log(name);
    POINT cursor;
    GetCursorPos(&cursor);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY), &mi);
    const RECT& wa = mi.rcWork;
    const App* app = find_app(name);
    // Made on that monitor, so that its DPI is the window's; sized and placed before it is shown.
    HWND hwnd = CreateWindowExW(layout_style(), L"WinApkSplash", (app ? app->label : name).c_str(),
                                WS_OVERLAPPEDWINDOW, wa.left, wa.top, ipx(300), ipx(300), nullptr, nullptr, g_inst,
                                s.get());
    if (!hwnd) return nullptr;
    s->hwnd = hwnd;
    const UINT dpi = GetDpiForWindow(hwnd);
    SIZE size;
    if (compact) {
        RECT r{0, 0, MulDiv(300, static_cast<int>(dpi), 96), MulDiv(300, static_cast<int>(dpi), 96)};
        AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
        size = {r.right - r.left, r.bottom - r.top};
    } else {
        size = app_window_size(name, dpi, wa);
    }
    SetWindowPos(hwnd, nullptr, wa.left + ((wa.right - wa.left) - size.cx) / 2,
                 wa.top + ((wa.bottom - wa.top) - size.cy) / 2, size.cx, size.cy, SWP_NOZORDER | SWP_NOACTIVATE);
    set_window_app_id(hwnd, app_user_model_id(name));
    for (const int which : {ICON_BIG, ICON_SMALL}) {
        const int ip = GetSystemMetrics(which == ICON_BIG ? SM_CXICON : SM_CXSMICON);
        if (auto icon = static_cast<HICON>(LoadImageW(nullptr, shortcut_icon(name).c_str(), IMAGE_ICON, ip, ip,
                                                      LR_LOADFROMFILE))) {
            SendMessageW(hwnd, WM_SETICON, static_cast<WPARAM>(which), reinterpret_cast<LPARAM>(icon));
        }
    }
    s->icon = rounded_icon(app_dir(name) / L"icon.png", ipx(96));
    Splash* raw = s.get();
    g_splashes[name] = std::move(s);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);  // Esc cancels
    return raw;
}

// Shows the app's starting window; null when it could not be made.
HWND start_window(const std::wstring& name) {
    const Splash* s = show_splash(name);
    return s ? s->hwnd : nullptr;
}

// The starting window turned into the error: the one still up, else a small new one (an app that failed
// after its window was up).
void splash_failed(const std::wstring& name, const std::wstring& title, const std::wstring& text) {
    Splash* s = show_splash(name, true);
    if (!s) return;
    s->failed = true;
    s->error_title = title;
    s->error_text = text;
    s->log = run_log(name);
    PostMessageW(s->hwnd, kFitSplash, 0, 0);  // a starting window smaller than the error grows to hold it
    InvalidateRect(s->hwnd, nullptr, FALSE);
    SetForegroundWindow(s->hwnd);
}

void close_splash(const std::wstring& name) {
    const auto it = g_splashes.find(name);
    if (it != g_splashes.end() && !it->second->failed) DestroyWindow(it->second->hwnd);
}

// The starting window of a start still on show (not one turned into an error), else null.
HWND starting_window(const std::wstring& name) {
    const auto it = g_splashes.find(name);
    return it != g_splashes.end() && !it->second->failed ? it->second->hwnd : nullptr;
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

void set_view(View view);

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
        accept_ = !dropped_files(data).empty();
        // The drop overlay is the library's; the Google services setup takes its files itself.
        if (accept_ && g_view != kGmsSetupView) set_view(kLibraryView);
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
        if (accept_ && g_view == kGmsSetupView) {
            for (const fs::path& p : dropped_files(data)) setup_offer(p);
        } else if (accept_) {
            install_files(dropped_files(data));
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
    if (g_log_wnd) {  // the log's colours are set in its text: open it again
        const fs::path path = g_lv.path;
        DestroyWindow(g_log_wnd);
        show_logs(path);
    }
    if (g_debug_wnd) {  // and the Threads window's controls take theirs when they are made
        const std::wstring app = g_dv.app;
        DestroyWindow(g_debug_wnd);
        show_debugger(app);
    }
    InvalidateRect(g_main, nullptr, FALSE);
}

void command(int id);

// The tile's menu, under the app's icon and name: Open, Stop while it runs, App details, Remove.
// Everything else is on the details page.
void show_context_menu(const App& app, POINT pt, bool keyboard = false) {
    const bool running = app_is_running(app.name);
    const bool pending = is_pending(app.name);
    PopupMenu m;
    m.app = &app;
    m.title = app.label;
    const std::vector<std::string> pkg = package_of(app.name);
    if (running) m.sub = tr(L"Running");
    else if (pkg.size() > 2 && !pkg[2].empty()) m.sub = fmt(L"Version {0}", {utf8_to_wide(pkg[2])});
    else if (!pkg.empty()) m.sub = utf8_to_wide(pkg[0]);
    auto item = [&](int id, const wchar_t* text, const wchar_t* glyph, std::wstring key = L"") {
        MenuItem it;
        it.id = id;
        it.text = tr(text);
        it.glyph = glyph;
        it.shortcut = std::move(key);
        it.disabled = pending && (id == kCmdOpen || id == kCmdRemove);
        it.destructive = id == kCmdRemove;
        m.items.push_back(it);
    };
    item(kCmdOpen, L"Open", kGlyphPlay, tr(L"Enter"));
    if (running) item(kCmdStop, L"Stop", kGlyphStop);
    item(kCmdInfo, L"App details", kGlyphInfo, L"Alt+Enter");
    m.items.push_back({});
    item(kCmdRemove, L"Remove…", kGlyphDelete);
    if (const int pick = run_menu(m, pt, LONG_MIN, keyboard)) command(pick);
}

void command(int id) {
    const App* sel = selected();
    if (!sel) return;
    const App a = *sel;
    switch (id) {
        case kCmdOpen: launch(a); break;
        case kCmdStop: force_stop(a); break;
        case kCmdInfo: show_info(a); break;
        case kCmdRemove: remove_app(a, true); break;
    }
}

// Moves the selection by tiles (dx) and rows (dy) from the keyboard; `to` jumps to a tile instead.
void move_selection(int dx, int dy, int to = -1) {
    int i = to >= 0 ? to : tile_of(g_selected);
    if (to < 0 && i < 0) {
        for (size_t t = 0; t < g_tiles.size() && i < 0; ++t) {
            if (g_tiles[t].app >= 0) i = static_cast<int>(t);
        }
    } else if (to < 0) {
        i += dx + dy * g_l.cols;
        if (dy != 0 && i >= static_cast<int>(g_tiles.size())) i = static_cast<int>(g_tiles.size()) - 1;
    }
    if (i < 0 || i >= static_cast<int>(g_tiles.size()) || g_tiles[static_cast<size_t>(i)].app < 0) return;
    g_focus_visible = true;
    select_app(g_apps[static_cast<size_t>(g_tiles[static_cast<size_t>(i)].app)].name);
}

// What a tooltip says for a control that is only a glyph, or a tile whose name may be cut short.
std::wstring tooltip_text(const Hit& h) {
    switch (h.kind) {
        case kHitNav:
            if (!g_l.compact) return L"";
            return tr(h.tile == kNavLibrary ? L"Library"
                      : h.tile == kNavStore ? L"Store"
                      : h.tile == kNavPlay  ? L"Google Play"
                                            : L"Settings");
        case kHitAccount:
            return g_account.empty() ? std::wstring(tr(L"Google account: not signed in"))
                                     : fmt(L"Google account: {0}", {g_account});
        case kHitTile: {
            const Tile& t = g_tiles[static_cast<size_t>(h.tile)];
            if (t.app < 0) return fmt(L"Installing {0}…", {job_label(t.job)});
            const App& a = g_apps[static_cast<size_t>(t.app)];
            return g_running.count(a.name) ? a.label + L"  ·  " + tr(L"Running") : a.label;
        }
        default: return L"";
    }
}

// One tooltip for the window; its text is asked for (TTN_GETDISPINFO) from what the mouse is over.
void create_tooltip() {
    if (g_tip) DestroyWindow(g_tip);
    g_tip = CreateWindowExW(WS_EX_TOPMOST | layout_style() | (g_rtl ? WS_EX_RTLREADING : 0), TOOLTIPS_CLASSW, nullptr,
                            WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                            CW_USEDEFAULT, g_main, nullptr, g_inst, nullptr);
    if (!g_tip) return;
    TOOLINFOW ti{sizeof(ti)};
    ti.uFlags = TTF_SUBCLASS | TTF_IDISHWND;
    ti.hwnd = g_main;
    ti.uId = reinterpret_cast<UINT_PTR>(g_main);
    ti.lpszText = LPSTR_TEXTCALLBACK;
    SendMessageW(g_tip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
    SendMessageW(g_tip, TTM_SETMAXTIPWIDTH, 0, ipx(320));
}

// Shows a view. The library keeps its scroll position while another is shown; a page scrolls on its own, from its
// top.
void set_view(View view) {
    if (view == g_view && view == kLibraryView) return;
    // Leaving the Store or Google Play: its panel hides (compute_layout) and the keyboard comes back to the window.
    for (const Embedded* e : {&g_store_app, &g_play}) {
        const HWND w = embedded_window(*e);
        if (view != e->view && w && GetFocus() == w) SetFocus(g_main);
    }
    g_view = view;
    g_hot = {};
    setup_watch(view == kGmsSetupView);
    if (page_view()) {
        g_page.scroll = 0;
        g_page.hot = g_page.focus = -1;
    }
    rebuild_tiles();
    update_timer();
}

void apply_language() {
    pick_language();
    for (HWND w : {g_main, g_install_wnd, g_store_app.panel, g_play.panel}) {
        if (!w) continue;
        const LONG_PTR ex = GetWindowLongPtrW(w, GWL_EXSTYLE);
        SetWindowLongPtrW(w, GWL_EXSTYLE, g_rtl ? (ex | WS_EX_LAYOUTRTL) : (ex & ~WS_EX_LAYOUTRTL));
        SetWindowPos(w, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        InvalidateRect(w, nullptr, FALSE);
    }
    if (g_install_wnd) SetWindowTextW(g_install_wnd, install_title().c_str());
    if (g_log_wnd) DestroyWindow(g_log_wnd);
    if (g_debug_wnd) DestroyWindow(g_debug_wnd);
    create_tooltip();
    // A page holds its words: Settings (where the language is picked) is made again, a details page goes back.
    if (g_view == kSettingsView) show_settings();
    else if (g_view == kGmsSetupView) show_gms_setup(g_setup_from_settings);
    else if (page_view()) set_view(g_page.from);
    else rebuild_tiles();
}

void set_hot(const Hit& h) {
    if (h == g_hot) return;
    g_hot = h;
    if (g_tip) SendMessageW(g_tip, TTM_POP, 0, 0);  // the next hover asks for the new target's text
    InvalidateRect(g_main, nullptr, FALSE);
}

// The window's size is kept between runs, in DIPs so a DPI change does not grow it.
void save_window_size() {
    WINDOWPLACEMENT wp{sizeof(wp)};
    if (!GetWindowPlacement(g_main, &wp)) return;
    const int dpi = static_cast<int>(g_dpi);
    const RECT& r = wp.rcNormalPosition;
    set_setting(L"WindowWidth", static_cast<DWORD>(MulDiv(r.right - r.left, 96, dpi)));
    set_setting(L"WindowHeight", static_cast<DWORD>(MulDiv(r.bottom - r.top, 96, dpi)));
    set_setting(L"WindowMaximized", wp.showCmd == SW_SHOWMAXIMIZED ? 1 : 0);
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            g_main = hwnd;
            g_dpi = static_cast<float>(GetDpiForWindow(hwnd));
            make_fonts();
            create_tooltip();
            apply_theme();
            g_drop_target = new DropTarget;
            RegisterDragDrop(hwnd, g_drop_target);
            refresh();
            refresh_account();
            remove_old_store_data();
            SetTimer(hwnd, kUpdateTimer, 3600 * 1000, nullptr);
            SetTimer(hwnd, kMaintenanceTimer, mw::kCheckMinutes * 60 * 1000, nullptr);
            SetTimer(hwnd, kRunningTimer, 2000, nullptr);
            return 0;
        }
        case WM_SIZE:
            compute_layout();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_DPICHANGED: {
            g_dpi = HIWORD(wp);
            make_fonts();
            g_icons.clear();
            g_brand.reset();
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
        case WM_CTLCOLOREDIT:
            SetTextColor(reinterpret_cast<HDC>(wp), colorref(g_t.text));
            SetBkColor(reinterpret_cast<HDC>(wp), colorref(g_t.card));
            return reinterpret_cast<LRESULT>(g_edit_brush);
        case WM_NOTIFY:
            if (reinterpret_cast<NMHDR*>(lp)->code == TTN_GETDISPINFOW) {
                static wchar_t text[256];
                const std::wstring tip = tooltip_text(g_hot);
                wcsncpy(text, tip.c_str(), 255);
                text[255] = 0;
                reinterpret_cast<NMTTDISPINFOW*>(lp)->lpszText = text;
                return 0;
            }
            break;
        case WM_COMMAND:
            command(LOWORD(wp));
            return 0;
        case WM_MOUSEMOVE: {
            const float x = static_cast<float>(GET_X_LPARAM(lp)), y = static_cast<float>(GET_Y_LPARAM(lp));
            if (!g_tracking) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
                g_tracking = TrackMouseEvent(&tme);
            }
            if (g_page.thumb_drag && page_view()) {
                const float room = g_page.view.Height - px(8) - g_page.thumb.Height;
                if (room > 0) g_page.scroll = g_page.thumb_scroll0 + (y - g_page.thumb_y0) * page_max_scroll() / room;
                page_scroll(0);
                return 0;
            }
            if (g_thumb_drag) {
                const float room = g_l.grid.Height - thumb_rect().Height;
                if (room > 0) g_scroll = g_thumb_scroll0 + (y - g_thumb_y0) * g_l.max_scroll / room;
                clamp_scroll();
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            const Hit h = hit_test(x, y);
            if (page_view()) {
                const int ph = h.kind == kHitPage ? page_hit(x, y) : -1;
                if (ph != g_page.hot) {
                    g_page.hot = ph;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            set_hot(h);
            return 0;
        }
        case WM_MOUSELEAVE:
            g_tracking = false;
            if (!g_thumb_drag && !g_page.thumb_drag) {
                g_page.hot = -1;
                set_hot({});
            }
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT && reinterpret_cast<HWND>(wp) == hwnd) {
                const bool hand = g_hot.kind == kHitNav || g_hot.kind == kHitAccount || g_hot.kind == kHitAdd ||
                                  g_hot.kind == kHitRetry || g_hot.kind == kHitInfoClose || g_hot.kind == kHitInfoLink ||
                                  g_hot.kind == kHitPanelOpen || g_hot.kind == kHitPanelLog ||
                                  (g_hot.kind == kHitPage && g_page.hot != -1 && g_page.hot != kPageThumb);
                SetCursor(LoadCursorW(nullptr, hand ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_TIMER:
            if (wp == kAnimTimer) {
                InvalidateRect(hwnd, nullptr, FALSE);
                for (const Embedded* e : {&g_store_app, &g_play}) {
                    if (e->panel && e->state == Embedded::kStarting) InvalidateRect(e->panel, nullptr, FALSE);
                }
            }
            if (wp == kInfoBarTimer) info(L"", kInfo);
            if (wp == kUpdateTimer) auto_update_check();
            if (wp == kMaintenanceTimer) maintenance_check();
            if (wp == kQuitTimer) {
                KillTimer(hwnd, kQuitTimer);
                maybe_quit();
            }
            if (wp == kRunningTimer && IsWindowVisible(hwnd)) refresh_running();
            if (wp == kSetupScanTimer) {
                KillTimer(hwnd, kSetupScanTimer);
                if (g_view == kGmsSetupView) {
                    setup_scan(false);
                    setup_next();
                }
            }
            return 0;
        case WM_LBUTTONDOWN: {
            SetFocus(hwnd);
            g_focus_visible = false;
            const float x = static_cast<float>(GET_X_LPARAM(lp)), y = static_cast<float>(GET_Y_LPARAM(lp));
            const Hit h = hit_test(x, y);
            switch (h.kind) {
                case kHitNav:
                    if (h.tile == kNavSettings) show_settings();
                    else if (h.tile == kNavPlay) open_google_play();
                    else if (h.tile == kNavStore) open_store();
                    else set_view(kLibraryView);
                    break;
                case kHitAdd: add_from_dialog(); break;
                case kHitPanelOpen:
                case kHitRetry: g_view == kStoreView ? open_store() : open_google_play(); break;
                case kHitPanelLog: show_panel_log(); break;
                case kHitAccount: show_account(); break;
                case kHitInfoClose: info(L"", kInfo); break;
                case kHitInfoLink: {
                    const std::function<void()> act = g_status_act;
                    info(L"", kInfo);
                    if (act) act();
                    break;
                }
                case kHitPage: {
                    g_page.focus = -1;  // the mouse takes over: no focus ring
                    if (page_hit(x, y) == kPageThumb) {
                        g_page.thumb_drag = true;
                        g_page.thumb_y0 = y;
                        g_page.thumb_scroll0 = g_page.scroll;
                        SetCapture(hwnd);
                    }
                    InvalidateRect(hwnd, nullptr, FALSE);
                    break;
                }
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
                    break;
                }
                case kHitNone:
                case kHitBack:
                    g_selected.clear();
                    InvalidateRect(hwnd, nullptr, FALSE);
                    break;
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            const float x = static_cast<float>(GET_X_LPARAM(lp)), y = static_cast<float>(GET_Y_LPARAM(lp));
            if (g_page.thumb_drag) {
                g_page.thumb_drag = false;
                ReleaseCapture();
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (g_thumb_drag) {
                g_thumb_drag = false;
                ReleaseCapture();
                set_hot(hit_test(x, y));
                return 0;
            }
            if (page_view() && hit_test(x, y).kind == kHitPage) page_click(page_hit(x, y));
            return 0;
        }
        case WM_XBUTTONUP:  // the mouse's Back button
            if (GET_XBUTTON_WPARAM(wp) == XBUTTON1 && page_view()) page_back();
            return TRUE;
        case WM_LBUTTONDBLCLK: {
            const Hit h = hit_test(static_cast<float>(GET_X_LPARAM(lp)), static_cast<float>(GET_Y_LPARAM(lp)));
            if (h.kind == kHitTile && g_tiles[static_cast<size_t>(h.tile)].app >= 0) {
                launch(g_apps[static_cast<size_t>(g_tiles[static_cast<size_t>(h.tile)].app)]);
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
            if (GET_X_LPARAM(lp) == -1 && g_view == kLibraryView && selected()) {  // the keyboard's menu key
                const RectF r = tile_rect(tile_of(g_selected));
                POINT pt{static_cast<LONG>(r.X + r.Width / 2), static_cast<LONG>(r.Y + r.Height / 2)};
                ClientToScreen(hwnd, &pt);
                show_context_menu(*selected(), pt, true);
                return 0;
            }
            break;
        case WM_MOUSEWHEEL:
            if (page_view()) {
                page_scroll(-GET_WHEEL_DELTA_WPARAM(wp) / 120.0f * px(60));
                return 0;
            }
            if (g_view != kLibraryView) return 0;  // the Store and Google Play scroll in their own windows
            g_scroll -= GET_WHEEL_DELTA_WPARAM(wp) / 120.0f * px(60);
            clamp_scroll();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_SYSKEYDOWN:
            if (wp == VK_RETURN && g_view == kLibraryView && selected()) {  // Alt+Enter, as Explorer's properties
                show_info(*selected());
                return 0;
            }
            if (wp == VK_LEFT && page_view()) {  // Alt+Left: back
                page_back();
                return 0;
            }
            break;
        case WM_KEYDOWN: {
            const bool ctrl = GetKeyState(VK_CONTROL) < 0;
            if (ctrl && (wp == '1' || wp == '2')) {
                if (wp == '2') open_store();
                else set_view(kLibraryView);
                return 0;
            }
            if (ctrl && wp == VK_OEM_COMMA) {
                show_settings();
                return 0;
            }
            if (ctrl && wp == 'L') {
                show_logs({});
                return 0;
            }
            if (ctrl && wp == 'T' && debug_mode()) {  // debug mode's Threads window, on every running app
                show_debugger({});
                return 0;
            }
            if (ctrl && wp == 'O') {
                add_from_dialog();
                return 0;
            }
            if (page_view()) {
                switch (wp) {
                    case VK_ESCAPE:
                    case VK_BACK: page_back(); return 0;
                    case VK_TAB: page_tab(GetKeyState(VK_SHIFT) < 0); return 0;
                    case VK_RETURN:
                    case VK_SPACE:
                        if (g_page.focus != -1) page_click(g_page.focus);
                        return 0;
                    case VK_UP: page_scroll(-px(48)); return 0;
                    case VK_DOWN: page_scroll(px(48)); return 0;
                    case VK_PRIOR: page_scroll(-g_page.view.Height * 0.9f); return 0;
                    case VK_NEXT: page_scroll(g_page.view.Height * 0.9f); return 0;
                    case VK_HOME: page_scroll(-g_page.scroll); return 0;
                    case VK_END: page_scroll(page_max_scroll()); return 0;
                }
                return 0;
            }
            if (g_view != kLibraryView) break;  // the Store and Google Play take their keys in their own windows
            switch (wp) {
                case VK_LEFT: move_selection(g_rtl ? 1 : -1, 0); return 0;
                case VK_RIGHT: move_selection(g_rtl ? -1 : 1, 0); return 0;
                case VK_UP: move_selection(0, -1); return 0;
                case VK_DOWN: move_selection(0, 1); return 0;
                case VK_PRIOR:
                case VK_NEXT: {
                    const int rows = (std::max)(1, static_cast<int>(g_l.grid.Height / (g_l.tile_h + g_l.gap)));
                    move_selection(0, wp == VK_PRIOR ? -rows : rows);
                    return 0;
                }
                case VK_HOME: move_selection(0, 0, 0); return 0;
                case VK_END: move_selection(0, 0, static_cast<int>(g_tiles.size()) - 1); return 0;
                case VK_RETURN: if (const App* a = selected()) launch(*a); return 0;
                case VK_ESCAPE:
                    g_selected.clear();
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
            }
            break;
        }
        case WM_CHAR: return 0;  // no beep
        case kInstallProbed: {  // install_probe_thread: a file read
            std::unique_ptr<InstallProbe> p(reinterpret_cast<InstallProbe*>(lp));
            g_probe_running = false;
            install_probed(*p);
            probe_next();
            maybe_quit();  // a downgrade refused, and nothing else under way
            return 0;
        }
        case kInstallSets: {  // install_sets_thread: the batch's files, as installs
            std::unique_ptr<std::vector<winapk::launcher::InstallSet>> sets(
                reinterpret_cast<std::vector<winapk::launcher::InstallSet>*>(lp));
            for (const winapk::launcher::InstallSet& set : *sets) {
                std::vector<SplitFile> splits;
                for (const auto& f : set.splits) splits.emplace_back(fs::path(f.first), f.second);
                probe_install(set.base, std::move(splits));
            }
            return 0;
        }
        case kInstallStarted: {
            std::unique_ptr<std::wstring> name(reinterpret_cast<std::wstring*>(lp));
            for (Job& j : g_jobs) {
                if (j.name == *name && j.state == Job::kWaiting) {
                    j.state = Job::kInstalling;
                    j.start = GetTickCount64();
                    break;
                }
            }
            if (g_install_wnd) InvalidateRect(g_install_wnd, nullptr, FALSE);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case kMaintenanceEnded:
            maintenance_ended();
            return 0;
        case kInstalled: {
            std::unique_ptr<std::wstring> name(reinterpret_cast<std::wstring*>(lp));
            const InstallEnd end = finish_install(*name);
            const InstallEnd::Kind how = end.kind();
            g_queued_packages.erase(*name);  // the library has it now, with its .package, or it is not there
            refresh();
            // The Store's own app: no word in the library, the Store's page says how it went.
            const bool store = store_app_installed(*name, how);
            // The last of Google's three apps installed: background updates can start.
            if (how == InstallEnd::kDone && !store) maintenance_check();
            job_ended(*name, how);
            update_timer();
            if (how == InstallEnd::kDone && !store) {
                select_app(*name);
                // The install window closed meanwhile: the library says it.
                if (!g_install_wnd) info(fmt(L"{0} is installed.", {label_of(*name)}), kSuccess);
            } else if (how != InstallEnd::kCancelled && !g_install_wnd && !store) {
                info(fmt(L"{0} could not be installed.", {job_label(*name)}), kError);
            }
            if (!any_pending()) {
                g_job_labels.clear();
                g_pending_looks.clear();
            }
            if (g_view == kGmsSetupView) rebuild_page();  // the app's row is ticked
            maybe_quit();
            return 0;
        }
        case kRunReady: {
            std::unique_ptr<std::wstring> name(reinterpret_cast<std::wstring*>(lp));
            if (Embedded* e = embedded_of(*name); e && e->state == Embedded::kStarting) {
                embedded_ready(*e);
            } else {
                bring_forward(*name);  // the app's window, over the starting window, before that one goes
                close_splash(*name);
            }
            return 0;
        }
        case kRunExited: {
            std::unique_ptr<std::wstring> name(reinterpret_cast<std::wstring*>(lp));
            const RunEnd end = finish_run(*name);
            update_timer();
            refresh_running();  // its badge goes, and its details page loses Stop
            InvalidateRect(hwnd, nullptr, FALSE);
            const std::wstring label = label_of(*name);
            if (Embedded* e = embedded_of(*name); e && e->up()) {
                embedded_exited(*e, end.kind() == RunEnd::kDidNotStart ||
                                        (end.kind() == RunEnd::kCrashed && !end.was_ready));
            } else {
                switch (end.kind()) {
                    case RunEnd::kQuiet:
                    case RunEnd::kStopped: close_splash(*name); break;
                    case RunEnd::kCrashed: {
                        // Win.apk's own part missing says so on the starting window, as it did.
                        if (std::wstring why = runtime_failure_reason(run_log(*name)); !why.empty()) {
                            splash_failed(*name, fmt(L"{0} didn't start", {label}), why);
                            break;
                        }
                        // Android's crash dialog where the app was: its starting window goes (hidden first,
                        // so that the launcher, which lives while a window of its is up, is still here).
                        RECT near_r = work_area_with_cursor();
                        if (HWND sw = starting_window(*name)) {
                            GetWindowRect(sw, &near_r);
                            ShowWindow(sw, SW_HIDE);
                        }
                        show_crash_dialog(*name, near_r);
                        close_splash(*name);
                        break;
                    }
                    case RunEnd::kDidNotStart:
                        splash_failed(*name, fmt(L"{0} didn't start", {label}),
                                      failure_reason(run_log(*name)));
                        break;
                    case RunEnd::kFailed:
                        splash_failed(*name, fmt(L"{0} stopped with an error", {label}),
                                      failure_reason(run_log(*name)));
                        break;
                }
            }
            maybe_quit();
            return 0;
        }
        case kRefresh:
            refresh();
            if (g_view == kGmsSetupView) rebuild_page();
            return 0;
        case kSetupChanged:
            // A download lands in several steps (a temporary name, then the real one): look once it settles.
            SetTimer(hwnd, kSetupScanTimer, 1000, nullptr);
            return 0;
        case kSetupChecked:
            setup_checked(std::unique_ptr<SetupCheck>(reinterpret_cast<SetupCheck*>(lp)));
            return 0;
        case kSetupFetched:
            setup_fetched(std::unique_ptr<SetupFetch>(reinterpret_cast<SetupFetch*>(lp)));
            return 0;
        case kGmsDone: {
            std::unique_ptr<std::wstring> out(reinterpret_cast<std::wstring*>(lp));
            g_gms_busy = false;
            const int code = static_cast<int>(static_cast<unsigned>(wp));
            if (code == 0) {
                // "SIGNED_IN<tab>email" is the last line that matters.
                std::wstring email;
                if (const size_t at = out->find(L"SIGNED_IN\t"); at != std::wstring::npos) {
                    email = out->substr(at + 10, out->find_first_of(L"\r\n", at) - at - 10);
                }
                info(email.empty() ? tr(L"Signed in to Google.") : fmt(L"Signed in as {0}.", {email}), kSuccess);
            } else if (code == 2) {
                info(tr(L"No account was added."), kInfo);
            }
            refresh_account();
            if (g_view == kSettingsView) rebuild_page();
            if (code != 0 && code != 2) {  // after the page is rebuilt: behind the dialog it no longer says "Signing in…"
                InvalidateRect(hwnd, nullptr, FALSE);
                UpdateWindow(hwnd);
                // The runtime's ERROR line carries the failure's code (a WebView2
                // HRESULT, the page's web error and HTTP status, Google's Error=):
                // shown as is, since a code is what a user can look up or report.
                std::wstring why;
                if (const size_t at = out->find(L"ERROR\t"); at != std::wstring::npos) {
                    why = out->substr(at + 6, out->find_first_of(L"\r\n", at) - at - 6);
                    if (why.size() > 200) why = why.substr(0, 200) + L"…";
                }
                const std::wstring body =
                    why.empty() ? tr(L"Google did not answer. Check the internet connection, then try again.")
                                : fmt(L"Google sign-in failed (error: {0}).", {why});
                error_with_log(tr(L"Could not sign in to Google"), body, gms_log());
            }
            return 0;
        }
        case kStoreAppChecked: {
            std::unique_ptr<StoreAppCheck> c(reinterpret_cast<StoreAppCheck*>(lp));
            store_app_checked(*c);
            maybe_quit();  // no download follows: the hidden process may end
            return 0;
        }
        case kStoreAppProgress:
            g_store_setup.progress(static_cast<int>(wp));
            if (g_view == kStoreView) InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case kStoreAppDownloaded:
            store_app_downloaded(static_cast<StoreAppDownload>(wp));
            maybe_quit();
            return 0;
        case kUpdateChecked: {
            std::unique_ptr<UpdateCheck> c(reinterpret_cast<UpdateCheck*>(lp));
            update_checked(*c);
            return 0;
        }
        case kUpdateProgress: update_progress(static_cast<int>(wp)); return 0;
        case kUpdateDownloaded:
            update_downloaded(static_cast<int>(wp));
            maybe_quit();
            return 0;
        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {ipx(680), ipx(520)};
            return 0;
        case WM_COPYDATA: {
            const auto* cds = reinterpret_cast<const COPYDATASTRUCT*>(lp);
            if (cds->dwData != kCopyOpen || cds->cbData < sizeof(wchar_t)) return FALSE;
            const std::wstring text(static_cast<const wchar_t*>(cds->lpData), cds->cbData / sizeof(wchar_t) - 1);
            size_t at = text.find(L'\n');
            std::vector<fs::path> files;
            while (at != std::wstring::npos) {
                const size_t end = text.find(L'\n', at + 1);
                const fs::path f = text.substr(at + 1, end == std::wstring::npos ? std::wstring::npos : end - at - 1);
                if (is_app_file(f)) files.push_back(f);
                at = end;
            }
            install_files(files);
            if (!text.empty() && text[0] == L'L') show_library(SW_SHOWDEFAULT);
            return TRUE;
        }
        case WM_ACTIVATEAPP:  // another program (an app's store, the install helper) may have changed the library
            if (wp && IsWindowVisible(hwnd) && !any_pending() && library_stamp() != g_library_stamp) refresh();
            // An app may have added the account (its "Sign in with Google") or the user signed out.
            if (wp && google_account_present() != !g_account.empty()) refresh_account();
            break;
        case kGmsAccount: {
            std::unique_ptr<std::wstring> account(reinterpret_cast<std::wstring*>(lp));
            g_account = *account;
            g_account_photo_key.clear();  // a sign-in or sign-out may have changed the picture
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_ACTIVATE:  // back to the window: the keyboard goes to the Store or Google Play when it is the view
            if (LOWORD(wp) != WA_INACTIVE && shown_embedded()) {
                focus_embedded();
                return 0;
            }
            break;
        case WM_CLOSE:
            save_window_size();
            // The user closes Win.apk: what it started in the background ends once nothing of it is on screen,
            // a maintenance window included (g_closed_by_user, maybe_quit).
            g_closed_by_user = true;
            // The Store and Google Play live in this window: they close with it.
            for (const Embedded* e : {&g_store_app, &g_play}) {
                if (!e->name.empty() && e->up()) stop_run(e->name);
            }
            // Work still under way (an install, a splash, an app's window) keeps the process; the library only
            // hides. Otherwise the process ends, and what it started in the background with it (end_background_work).
            if (work_keeps_process() || !windowed_processes().empty()) {
                ShowWindow(hwnd, SW_HIDE);
                maybe_quit();
                return 0;
            }
            g_quitting = true;  // before DestroyWindow ends the owned windows, whose WM_DESTROY asks maybe_quit
            break;
        case kQuitIfIdle:  // posted by maybe_quit: nothing has come up since
            if (!g_quitting && !IsWindowVisible(hwnd) && !work_keeps_process() && windowed_processes().empty()) {
                g_quitting = true;
                DestroyWindow(hwnd);  // WM_DESTROY posts the quit
            }
            return 0;
        case WM_DESTROY:
            g_quitting = true;
            RevokeDragDrop(hwnd);
            if (g_drop_target) g_drop_target->Release();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// The installed-apps registry

std::string json_string(const std::string& s) {
    std::string out = "\"";
    for (const unsigned char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\u%04x", c);
            out += buf;
        } else {
            out += static_cast<char>(c);
        }
    }
    return out + "\"";
}

// The lines of library\<name>.package: package, versionCode, versionName.
std::vector<std::string> package_of(const std::wstring& name) {
    std::ifstream f(package_file(name), std::ios::binary);
    std::vector<std::string> lines;
    for (std::string line; std::getline(f, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    return lines;
}

std::string iso_time(const fs::path& p) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    SYSTEMTIME t{};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a) || !FileTimeToSystemTime(&a.ftLastWriteTime, &t))
        return "";
    char buf[32];
    std::snprintf(buf, sizeof buf, "%04u-%02u-%02uT%02u:%02u:%02uZ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
                  t.wSecond);
    return buf;
}

// The APK the registry names for `pkg`: of the library's copies of it, the first in library() order, as
// write_registry picks it. Empty when the library has none.
fs::path registry_apk(const std::string& pkg) {
    for (const App& app : library()) {
        const std::vector<std::string> p = package_of(app.name);
        if (!p.empty() && p[0] == pkg) return app.apk;
    }
    return {};
}

namespace appops {
std::string installed_at(const std::string& pkg);
}

// <data>\installed_apps.json, upstream's registry (launcher/InstallCore.swift
// RegistryStore, schema 2) as MacApkPackageManager.InstalledRegistry reads
// it: one row per library app whose package is known. Rewritten whole, to a
// temporary file then renamed, so a reader sees an old or a new file.
void write_registry() {
    std::string out = "{\"schema\":2,\"apps\":[";
    bool first = true;
    std::vector<std::string> seen;  // one row per package, as on a device
    for (const App& app : library()) {
        const std::vector<std::string> p = package_of(app.name);
        if (p.empty() || p[0].empty()) continue;
        if (std::find(seen.begin(), seen.end(), p[0]) != seen.end()) continue;
        seen.push_back(p[0]);
        const std::string when = iso_time(package_file(app.name));
        // installedAt is Android's firstInstallTime: an update keeps the row's, so the app keeps what it was
        // granted (the signed grants are keyed by package and installedAt); a removal drops the row, and the
        // next install of the package starts with no grant.
        std::string since = appops::installed_at(p[0]);
        if (since.empty()) since = when;
        out += first ? "\n" : ",\n";
        first = false;
        out += "{\"package\":" + json_string(p[0]) + ",\"label\":" + json_string(wide_to_utf8(app.label)) +
               ",\"versionCode\":" + std::to_string(p.size() > 1 ? std::atoll(p[1].c_str()) : 0) +
               ",\"versionName\":" + json_string(p.size() > 2 ? p[2] : "") +
               ",\"apkPath\":" + json_string(wide_to_utf8(app.apk.wstring())) +
               ",\"bundlePath\":" + json_string(wide_to_utf8(app.apk.wstring())) +
               ",\"dirName\":" + json_string(wide_to_utf8(app.name)) + ",\"state\":\"installed\"" +
               ",\"installedAt\":" + json_string(since) + ",\"updatedAt\":" + json_string(when) + "}";
    }
    out += "\n]}\n";
    std::error_code ec;
    fs::create_directories(data_dir(), ec);
    const fs::path file = data_dir() / L"installed_apps.json";
    // Per process: the library and an install helper (another Win.apk.exe) may write it at once, and one
    // shared temporary name would let them interleave into a file neither wrote.
    const fs::path tmp = data_dir() / (L"installed_apps.json.tmp." + std::to_wstring(GetCurrentProcessId()));
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    f << out;
    f.close();  // a full disk shows here, when the buffer is written
    if (!f || !MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        fs::remove(tmp, ec);  // the old registry stays whole
    }
}

// Apps installed before the library recorded packages get theirs read now,
// in the background, so the registry knows them too. (Only the name: the
// version comes with the app's next install.)
DWORD WINAPI backfill_thread(void*) {
    bool changed = false;
    std::error_code ec;
    std::set<std::string> registered, found;
    for (const App& app : library()) {
        if (fs::exists(package_file(app.name), ec)) {
            const std::vector<std::string> p = package_of(app.name);
            if (!p.empty()) registered.insert(p[0]);
            continue;
        }
        HANDLE p = start_runtime(L"--apk \"" + app.apk.wstring() + L"\" --package-to \"" +
                                     package_file(app.name).wstring() + L"\"",
                                 logs_dir() / L"package-backfill.log");
        if (p && wait_exit(p) == 0) {
            changed = true;
            const std::vector<std::string> pkg = package_of(app.name);
            if (!pkg.empty() && !pkg[0].empty()) found.insert(pkg[0]);
        } else {
            fs::remove(package_file(app.name), ec);
        }
    }
    if (changed) {
        write_registry();
        // A package the registry did not have until now was no app's to see, nor the broker's.
        for (const std::string& pkg : found)
            if (!registered.count(pkg)) tell_broker(winapk::package_events::kAdded, pkg);
        if (g_main) PostMessageW(g_main, kRefresh, 0, 0);  // the store sees these packages as installed
    }
    return 0;
}

// ---------------------------------------------------------------------------
// The install helper

// The helper's output goes where its caller pointed it (a transcript file
// PackageInstallerBackend reads), in UTF-8.
void helper_say(const std::string& line) {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out == nullptr || out == INVALID_HANDLE_VALUE) return;
    const std::string s = line + "\n";
    DWORD n = 0;
    WriteFile(out, s.data(), static_cast<DWORD>(s.size()), &n, nullptr);
}

void helper_progress(double fraction, const char* stage) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "[helper] progress %.2f %s", fraction, stage);
    helper_say(buf);
}

int helper_fail(int code, const char* stage, const std::string& message) {
    helper_say("{\"ok\":false,\"stage\":" + json_string(stage) + ",\"message\":" + json_string(message) + "}");
    return code;
}

// The library app that holds `pkg`, or null.
const App* find_package(const std::vector<App>& apps, const std::string& pkg) {
    for (const App& app : apps) {
        const std::vector<std::string> p = package_of(app.name);
        if (!p.empty() && p[0] == pkg) return &app;
    }
    return nullptr;
}

// Asks the user, as the Mac's helper does before it acts for an app: the launcher's dialog, in front of
// the app, Enter on Cancel. `app` heads it when the question is about one in the library, else `heading`
// when given. A no is exit 3, which the installer reports as STATUS_FAILURE_ABORTED.
bool helper_consent(const App* app, const std::wstring& question, const std::wstring& note, const std::wstring& action,
                    const std::wstring& heading = L"") {
    Prompt p;
    if (app) {
        p.app = app->label;
        p.icon = rounded_icon(app_dir(app->name) / L"icon.png", ipx(40));
    } else {
        p.app = heading;
    }
    p.caption = L"Win.apk";
    p.question = question;
    p.note = note;
    p.buttons = {{IDOK, action}, {IDCANCEL, tr(L"Cancel")}};
    p.default_id = IDCANCEL;
    return run_dialog(p, nullptr) == IDOK;
}

void notify_library() {
    if (HWND w = FindWindowW(L"WinApkLibrary", nullptr)) PostMessageW(w, kRefresh, 0, 0);
}

namespace appops {
bool caller_is_play_store();
bool caller_is_builtin_store();
bool installs_allowed_from_caller();
}

// The helper's end, once known: its one JSON line, then "[helper] exit <code>", which tells the asking app's
// runtime that the install is over (PackageInstallerBackend.runHelper) while the window stays to show it, as
// a device sends the session's status when the install ends, not when the user closes the installer.
int g_helper_code = -1;
std::string g_helper_pkg;
bool g_helper_update = false;

int helper_report(int code) {
    helper_say("[helper] exit " + std::to_string(code));
    g_helper_code = code;
    return code;
}

DWORD WINAPI helper_install_thread(void* param) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::unique_ptr<InstallJob> job(static_cast<InstallJob*>(param));
    bool running = false;
    const bool ok = install_into_library(job->source, job->name, &running);
    AcquireSRWLockExclusive(&g_installs_lock);
    g_installs.ended(job->name, ok, running);
    ReleaseSRWLockExclusive(&g_installs_lock);
    // The window outlives the install (it is closed only from a result), so it is there to tell.
    PostMessageW(g_install_wnd, kInstalled, 0, reinterpret_cast<LPARAM>(new std::wstring(job->name)));
    CoUninitialize();
    return 0;
}

// The install window, on its install's end: the result shown, and told to the app that asked.
void helper_install_ended(const std::wstring& name) {
    const InstallEnd::Kind how = finish_install(name).kind();
    g_apps = library();  // the installed app's own name and icon, and Open
    job_ended(name, how);
    switch (how) {
        case InstallEnd::kDone:
            helper_progress(1.0, "installed");
            notify_library();
            helper_say("{\"ok\":true,\"package\":" + json_string(g_helper_pkg) + ",\"wasUpdate\":\"" +
                       (g_helper_update ? "true" : "false") + "\"}");
            helper_report(0);
            break;
        case InstallEnd::kCancelled:  // InstallInstalling's Cancel abandons the session: STATUS_FAILURE_ABORTED
            helper_report(helper_fail(3, "cancelled", "the user cancelled the install"));
            break;
        case InstallEnd::kAppRunning:
            helper_report(helper_fail(5, "install", "the app is running; close it, then update it"));
            break;
        default:
            helper_report(helper_fail(5, "install", "installing failed; the log is " +
                                                        wide_to_utf8(install_log(name).wstring())));
            break;
    }
}

// After the user agreed: Android's package installer shows the install (InstallInstalling: the app, a moving
// bar, Cancel until it commits) and then its end (InstallSuccess: "App installed." with Done and Open;
// InstallFailed: "App not installed." and why). Here that is the install window, alone in this process,
// installing as the library does (install_into_library, cancelled through the same InstallTable). Returns
// the helper's exit code, told to the app as soon as the install ends (helper_report).
int helper_install_ui(const fs::path& apk, const std::wstring& name, const std::string& pkg, bool update,
                      const std::wstring& shown) {
    g_helper_ui = true;
    g_helper_pkg = pkg;
    g_helper_update = update;
    g_apps = library();
    WNDCLASSEXW sc{sizeof(sc)};
    sc.hInstance = g_inst;
    sc.hIcon = LoadIconW(g_inst, MAKEINTRESOURCEW(1));
    sc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    sc.lpfnWndProc = install_proc;
    sc.lpszClassName = L"WinApkInstall";
    RegisterClassExW(&sc);

    begin_install(name);
    AcquireSRWLockExclusive(&g_installs_lock);
    g_installs.start(name);
    ReleaseSRWLockExclusive(&g_installs_lock);
    Job job{name};
    job.file = shown;
    job.update = update;
    job.state = Job::kInstalling;
    job.start = GetTickCount64();
    g_jobs = {job};
    show_install_window();
    if (!g_install_wnd) {  // no window (no desktop): the install still runs and is told, with nothing shown
        bool running = false;
        const bool ok = install_into_library(apk, name, &running);
        AcquireSRWLockExclusive(&g_installs_lock);
        g_installs.ended(name, ok, running);
        ReleaseSRWLockExclusive(&g_installs_lock);
        helper_install_ended(name);
        return g_helper_code;
    }
    auto* work = new InstallJob{apk, name, false};
    if (HANDLE t = CreateThread(nullptr, 0, helper_install_thread, work, 0, nullptr)) {
        CloseHandle(t);
    } else {
        delete work;
        AcquireSRWLockExclusive(&g_installs_lock);
        g_installs.ended(name, false, false);
        ReleaseSRWLockExclusive(&g_installs_lock);
        helper_install_ended(name);
    }
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    // The window closes only once the install has ended (WM_CLOSE asks to cancel while it runs).
    return g_helper_code < 0 ? helper_report(helper_fail(5, "install", "the install window closed")) : g_helper_code;
}

int helper_main(int argc, wchar_t** argv) {
    const std::wstring verb = argv[1];
    std::wstring apk, package, source, requestor;
    for (int i = 2; i < argc; ++i) {
        const std::wstring a = argv[i];
        auto value = [&](std::wstring* out) {
            if (i + 1 >= argc) return false;
            *out = argv[++i];
            return true;
        };
        bool ok = true;
        if (a == L"--apk") ok = value(&apk);
        else if (a == L"--package") ok = value(&package);
        else if (a == L"--source") ok = value(&source);
        else if (a == L"--requestor-label") ok = value(&requestor);
        else if (a == L"--json") {}
        else ok = false;
        if (!ok) return helper_fail(2, "args", "unknown argument " + wide_to_utf8(a));
    }
    if (source != L"android") return helper_fail(2, "args", "--source android is required");
    const std::wstring who = requestor.empty() ? tr(L"An app") : requestor;
    const std::vector<App> apps = library();

    if (verb == L"uninstall") {
        const App* app = package.empty() ? nullptr : find_package(apps, wide_to_utf8(package));
        if (!app) return helper_fail(5, "lookup", "not installed: " + wide_to_utf8(package));
        if (!helper_consent(app, fmt(L"Remove {0}?", {app->label}), fmt(L"{0} asks to remove it.", {who}),
                            tr(L"Remove")))
            return helper_fail(3, "consent", "declined");
        if (!remove_app(*app, false))
            return helper_fail(5, "uninstall", "the app is running or a file of it is in use");
        notify_library();
        helper_say("{\"ok\":true,\"package\":" + json_string(wide_to_utf8(package)) + ",\"dataRemoved\":\"false\"}");
        return 0;
    }

    std::error_code ec;
    if (apk.empty() || !fs::is_regular_file(apk, ec)) return helper_fail(2, "args", "no --apk file");
    if (!is_app_file(apk)) return helper_fail(6, "invalid-apk", "not an .apk/.apkm/.xapk/.apks");

    // Which app this is decides what it replaces, so it is read first.
    const fs::path pkg_out = data_dir() / L"staging" / (L"package-" + std::to_wstring(GetCurrentProcessId()) + L".txt");
    fs::create_directories(pkg_out.parent_path(), ec);
    HANDLE probe = start_runtime(L"--apk \"" + apk + L"\" --package-to \"" + pkg_out.wstring() + L"\"",
                                 logs_dir() / L"install-helper.log");
    std::string pkg;
    if (probe && wait_exit(probe) == 0) {
        std::ifstream f(pkg_out, std::ios::binary);
        std::getline(f, pkg);
    }
    fs::remove(pkg_out, ec);
    // Not the file's fault when the runtime never ran: STATUS_FAILURE, not STATUS_FAILURE_INVALID.
    if (!probe) return helper_fail(5, "install", "the runtime could not be started; the log is " +
                                                     wide_to_utf8((logs_dir() / L"install-helper.log").wstring()));
    if (pkg.empty()) return helper_fail(6, "invalid-apk", "no package name in the app");
    helper_progress(0.05, "parsed");

    const App* existing = find_package(apps, pkg);
    const bool update = existing != nullptr;
    const std::wstring wpkg = utf8_to_wide(pkg);
    // The Play Store installs as a phone's system installer does, with no question: it holds INSTALL_PACKAGES
    // there, and the user already chose the app in it. Only the real one: the asking runtime runs the
    // library's com.android.vending and Google signed that APK (appops::caller_is_play_store). Win.apk's own
    // Store is a store that came with the PC, as a phone's preinstalled store holds INSTALL_PACKAGES: the user
    // chose the app in it, so it asks no second time (appops::caller_is_builtin_store: the very release asset
    // the launcher installed).
    const bool system_store = appops::caller_is_play_store() || appops::caller_is_builtin_store();
    // Any other app installs only from a source the user allowed ("Install unknown apps"), as Android's
    // installer checks before its question (PackageInstallerActivity: the blocked dialog, Settings, back).
    if (!system_store && !appops::installs_allowed_from_caller())
        return helper_fail(3, "unknown-sources", "installs from this app are not allowed");
    // PackageInstallerActivity's question: the app's name over "Do you want to install this app?" (a new app's
    // name is not known before its install reads it: its package stands in).
    const bool agreed =
        system_store ||
        (update ? helper_consent(existing, tr(L"Do you want to update this app?"),
                                 fmt(L"{0} asks to update it with the copy it has ({1}).", {who, wpkg}), tr(L"Update"))
                : helper_consent(nullptr, tr(L"Do you want to install this app?"),
                                 fmt(L"{0} asks to install it.", {who}), tr(L"Install"), wpkg));
    if (!agreed) return helper_fail(3, "consent", "declined");
    helper_progress(0.1, "consented");

    const std::wstring name = update ? existing->name : file_name_safe(wpkg);
    // Everyone else's install is shown as Android's installer shows it after the question: its progress, then
    // its end with Open and Done (helper_install_ui).
    if (!system_store) return helper_install_ui(apk, name, pkg, update, update ? existing->label : wpkg);
    // A system store's update of an app open in a window: PackageManagerService kills an app it replaces, and
    // the Play Store has already waited for it when it asked to (an auto-update's InstallConstraints, "app not
    // interacting"), or the user tapped Update in it. So the app is stopped, not the update refused. Any other
    // installer's update leaves an open window to the user (stop_app_for_install).
    if (update && !stop_app_for_update(name))
        return helper_fail(5, "install", "the app is running and could not be stopped");
    bool running = false;
    if (!install_into_library(apk, name, &running)) {
        if (running) return helper_fail(5, "install", "the app is running; close it, then update it");
        return helper_fail(5, "install", "installing failed; the log is " +
                                             wide_to_utf8(install_log(name).wstring()));
    }
    helper_progress(1.0, "installed");
    notify_library();
    helper_say("{\"ok\":true,\"package\":" + json_string(pkg) + ",\"wasUpdate\":\"" + (update ? "true" : "false") +
               "\"}");
    return 0;
}

// ---------------------------------------------------------------------------
// Permissions: the app-ops store and the permission dialogs
//
// Android asks the user before an app may read shared storage or the location
// (the runtime permissions), or manage all files or media (the special-access
// switches in Settings). The runtime (SpecialAccessHandler) asks through this
// program, as upstream asks through macapk-installhelper
// (launcher/InstallHelperMain.swift RuntimePermission, SpecialAccess and
// app-details):
//
//   Win.apk.exe runtime-permission --source android --level <api> (--prompt <kind> | --auto)
//               --perm <p>... [--member <p>...] [--partial <p>...] --json
//   Win.apk.exe special-access --op <op> --source android --json
//   Win.apk.exe app-details --source android --level <api> [--group <NAME>=<p>,<p>...] [--op <op>...] --json
//
// The app asking is the parent process (winapk-runtime.exe, whose --apk names
// the library file), never an argument. The answer is the user's, in a task
// dialog over the app's window, and is signed into <data>\appops\app-ops.json
// with an ECDSA P-256 key whose private half is kept beside it under DPAPI;
// SpecialAccessStore believes only the entries its public key verifies. The
// payload, the store's layout and the state tokens are upstream's
// (InstallCore.swift AppOpsStore), keyed by package + installedAt, so a
// reinstall starts with no grant. Exit: 0 granted, 2 bad arguments, 3 denied,
// 4 no answer, 5 no identity or the store failed, 6 an --auto grant refused.
//
// The library's Get Info sheet shows the same grants and can change or forget
// them (by "panel", as upstream's control panel).

namespace appops {

const std::string kReadExternal = "android.permission.READ_EXTERNAL_STORAGE";
const std::string kWriteExternal = "android.permission.WRITE_EXTERNAL_STORAGE";
const std::string kMediaLocation = "android.permission.ACCESS_MEDIA_LOCATION";
const std::string kImages = "android.permission.READ_MEDIA_IMAGES";
const std::string kVideo = "android.permission.READ_MEDIA_VIDEO";
const std::string kAudio = "android.permission.READ_MEDIA_AUDIO";
const std::string kUserSelected = "android.permission.READ_MEDIA_VISUAL_USER_SELECTED";
const std::string kFine = "android.permission.ACCESS_FINE_LOCATION";
const std::string kCoarse = "android.permission.ACCESS_COARSE_LOCATION";
const std::string kBackgroundLocation = "android.permission.ACCESS_BACKGROUND_LOCATION";
const std::string kBodySensors = "android.permission.BODY_SENSORS";
const std::string kBodySensorsBackground = "android.permission.BODY_SENSORS_BACKGROUND";
const std::string kAllFiles = "android:manage_external_storage";
const std::string kManageMedia = "android:manage_media";
const std::string kOverlay = "android:system_alert_window";
const std::string kInstallApps = "android:request_install_packages";
const char* const kPayloadTag = "macapk-appops/3";
const char* const kLegacyTag = "macapk-appops/2";

// Every other dangerous permission, with the group it is asked under (the
// prompt's name, RuntimePermissionGroups.promptName) and the API level that
// defines it; the call-log trio is "phone" below API 28. The two "all the
// time" permissions are groups of their own, asked after their foreground
// group. MUST agree with runtime/java/com/macapk/loader/RuntimePermissionGroups.
struct PermRow {
    const char* perm;
    const char* group;
    int since;
};
const PermRow kGroupRows[] = {
    {"android.permission.READ_CONTACTS", "contacts", 1},
    {"android.permission.WRITE_CONTACTS", "contacts", 1},
    {"android.permission.GET_ACCOUNTS", "contacts", 1},
    {"android.permission.READ_CALENDAR", "calendar", 1},
    {"android.permission.WRITE_CALENDAR", "calendar", 1},
    {"android.permission.SEND_SMS", "sms", 1},
    {"android.permission.RECEIVE_SMS", "sms", 1},
    {"android.permission.READ_SMS", "sms", 1},
    {"android.permission.RECEIVE_WAP_PUSH", "sms", 1},
    {"android.permission.RECEIVE_MMS", "sms", 1},
    {"android.permission.READ_CELL_BROADCASTS", "sms", 1},
    {"android.permission.RECORD_AUDIO", "microphone", 1},
    {"android.permission.ACTIVITY_RECOGNITION", "activity_recognition", 29},
    {"android.permission.CAMERA", "camera", 1},
    {"android.permission.READ_CALL_LOG", "call_log", 16},
    {"android.permission.WRITE_CALL_LOG", "call_log", 16},
    {"android.permission.PROCESS_OUTGOING_CALLS", "call_log", 1},
    {"android.permission.READ_PHONE_STATE", "phone", 1},
    {"android.permission.READ_PHONE_NUMBERS", "phone", 26},
    {"android.permission.CALL_PHONE", "phone", 1},
    {"android.permission.ADD_VOICEMAIL", "phone", 14},
    {"android.permission.USE_SIP", "phone", 9},
    {"android.permission.ANSWER_PHONE_CALLS", "phone", 26},
    {"android.permission.ACCEPT_HANDOVER", "phone", 28},
    {"android.permission.BODY_SENSORS", "sensors", 20},
    {"android.permission.BODY_SENSORS_BACKGROUND", "sensors_background", 33},
    {"android.permission.BLUETOOTH_ADVERTISE", "nearby_devices", 31},
    {"android.permission.BLUETOOTH_CONNECT", "nearby_devices", 31},
    {"android.permission.BLUETOOTH_SCAN", "nearby_devices", 31},
    {"android.permission.UWB_RANGING", "nearby_devices", 31},
    {"android.permission.NEARBY_WIFI_DEVICES", "nearby_devices", 33},
    {"android.permission.POST_NOTIFICATIONS", "notifications", 33},
    {"android.permission.ACCESS_BACKGROUND_LOCATION", "location_background", 29},
};

bool is_storage(const std::string& p) {
    return p == kReadExternal || p == kWriteExternal || p == kMediaLocation || p == kImages || p == kVideo ||
           p == kAudio || p == kUserSelected;
}
bool is_location(const std::string& p) { return p == kFine || p == kCoarse; }
const PermRow* group_row(const std::string& p) {
    for (const PermRow& r : kGroupRows)
        if (p == r.perm) return &r;
    return nullptr;
}
// The group `p` is asked under at `level`, or "" when the level does not define it.
std::string group_name(const std::string& p, int level) {
    const PermRow* r = group_row(p);
    if (!r || level < r->since) return "";
    if (std::string(r->group) == "call_log" && level < 28) return "phone";
    return r->group;
}
bool is_group_perm(const std::string& p) { return group_row(p) != nullptr; }
bool is_group_name(const std::string& g) {
    for (const PermRow& r : kGroupRows)
        if (g == r.group) return true;
    return false;
}
// Every runtime permission a dialog can answer.
bool is_runtime_perm(const std::string& p) { return is_storage(p) || is_location(p) || is_group_perm(p); }
// The foreground permissions an "all the time" one depends on.
std::vector<std::string> foreground_of(const std::string& background) {
    if (background == kBackgroundLocation) return {kFine, kCoarse};
    if (background == kBodySensorsBackground) return {kBodySensors};
    return {};
}
bool is_op(const std::string& op) {
    return op == kAllFiles || op == kManageMedia || op == kOverlay || op == kInstallApps;
}
// Settings screens for access this host cannot grant, or has nothing to
// restrict: the dialog informs and records nothing (SpecialAccessHandler's
// INFO_* ops).
bool is_info_op(const std::string& op) { return op.rfind("winapk:", 0) == 0; }
bool is_state(const std::string& s) {
    return s == "granted" || s == "granted_fixed" || s == "denied" || s == "denied_fixed";
}
bool is_granted(const std::string& s) { return s == "granted" || s == "granted_fixed"; }

fs::path dir() { return data_dir() / L"appops"; }
fs::path store_file() { return dir() / L"app-ops.json"; }
fs::path key_file() { return dir() / L"signing-key.p256"; }
fs::path pub_file() { return dir() / L"signing-key.pub"; }

// ---- bytes -------------------------------------------------------------------

using Bytes = std::vector<unsigned char>;

std::string base64(const Bytes& b) {
    static const char* abc = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < b.size(); i += 3) {
        const unsigned v = (b[i] << 16) | ((i + 1 < b.size() ? b[i + 1] : 0) << 8) | (i + 2 < b.size() ? b[i + 2] : 0);
        out += abc[(v >> 18) & 63];
        out += abc[(v >> 12) & 63];
        out += i + 1 < b.size() ? abc[(v >> 6) & 63] : '=';
        out += i + 2 < b.size() ? abc[v & 63] : '=';
    }
    return out;
}

bool unbase64(const std::string& s, Bytes* out) {
    out->clear();
    unsigned v = 0;
    int bits = 0;
    for (const char c : s) {
        int d;
        if (c >= 'A' && c <= 'Z') d = c - 'A';
        else if (c >= 'a' && c <= 'z') d = c - 'a' + 26;
        else if (c >= '0' && c <= '9') d = c - '0' + 52;
        else if (c == '+') d = 62;
        else if (c == '/') d = 63;
        else if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
        else return false;
        v = (v << 6) | static_cast<unsigned>(d);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out->push_back(static_cast<unsigned char>((v >> bits) & 0xFF));
        }
    }
    return true;
}

bool read_bytes(const fs::path& p, Bytes* out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

// Written to a temporary file and renamed over the old one, so a reader sees
// the old file or the new one.
bool write_bytes(const fs::path& p, const void* data, size_t n) {
    const fs::path tmp = p.parent_path() / (L"." + p.filename().wstring() + L".tmp." +
                                            std::to_wstring(GetCurrentProcessId()));
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    f.write(static_cast<const char*>(data), static_cast<std::streamsize>(n));
    f.close();  // a full disk shows here, when the buffer is written
    if (f && MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
    std::error_code ec;
    fs::remove(tmp, ec);  // no temporary file left behind, and the old file stays
    return false;
}

// ---- the key -------------------------------------------------------------------

// A P-256 key pair from BCrypt: the private half for signing, kept in
// signing-key.p256 as a BCRYPT_ECCPRIVATE_BLOB under DPAPI (this Windows user
// only); the public half beside it as base64 of an X.509 SubjectPublicKeyInfo,
// which is what SpecialAccessStore.loadPublicKey reads.
struct Key {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    Key() { BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0); }
    ~Key() {
        if (key) BCryptDestroyKey(key);
        if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    }
    Key(const Key&) = delete;
    Key& operator=(const Key&) = delete;

    Bytes blob(const wchar_t* type) const {
        ULONG n = 0;
        if (!key || BCryptExportKey(key, nullptr, type, nullptr, 0, &n, 0) != 0) return {};
        Bytes b(n);
        if (BCryptExportKey(key, nullptr, type, b.data(), n, &n, 0) != 0) return {};
        b.resize(n);
        return b;
    }
    // The SubjectPublicKeyInfo: the fixed id-ecPublicKey / prime256v1 header,
    // then the uncompressed point 04 || X || Y.
    Bytes spki() const {
        const Bytes pub = blob(BCRYPT_ECCPUBLIC_BLOB);
        if (pub.size() != sizeof(BCRYPT_ECCKEY_BLOB) + 64) return {};
        Bytes out = {0x30, 0x59, 0x30, 0x13, 0x06, 0x07, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01, 0x06,
                     0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07, 0x03, 0x42, 0x00, 0x04};
        out.insert(out.end(), pub.begin() + sizeof(BCRYPT_ECCKEY_BLOB), pub.end());
        return out;
    }
};

Bytes sha256(const std::string& data) {
    Bytes h(32);
    BCRYPT_ALG_HANDLE a = nullptr;
    if (BCryptOpenAlgorithmProvider(&a, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return {};
    const bool ok = BCryptHash(a, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),
                               static_cast<ULONG>(data.size()), h.data(), 32) == 0;
    BCryptCloseAlgorithmProvider(a, 0);
    return ok ? h : Bytes{};
}

// BCrypt signs as r || s; Java's SHA256withECDSA wants the DER SEQUENCE of two
// INTEGERs, as CryptoKit's derRepresentation gives upstream.
Bytes der_of(const Bytes& rs) {
    auto integer = [](const unsigned char* p) {
        size_t i = 0;
        while (i < 31 && p[i] == 0) ++i;
        Bytes v(p + i, p + 32);
        if (v[0] & 0x80) v.insert(v.begin(), 0);
        v.insert(v.begin(), static_cast<unsigned char>(v.size()));
        v.insert(v.begin(), 0x02);
        return v;
    };
    Bytes r = integer(rs.data()), s = integer(rs.data() + 32);
    Bytes out = {0x30, static_cast<unsigned char>(r.size() + s.size())};
    out.insert(out.end(), r.begin(), r.end());
    out.insert(out.end(), s.begin(), s.end());
    return out;
}

bool rs_of(const Bytes& der, Bytes* rs) {
    rs->assign(64, 0);
    size_t i = 2;
    if (der.size() < 8 || der[0] != 0x30 || der[1] != der.size() - 2) return false;
    for (int part = 0; part < 2; ++part) {
        if (i + 2 > der.size() || der[i] != 0x02) return false;
        size_t n = der[i + 1];
        i += 2;
        if (i + n > der.size() || n == 0) return false;
        const unsigned char* p = der.data() + i;
        while (n > 32 && *p == 0) ++p, --n;
        if (n > 32) return false;
        std::copy(p, p + n, rs->begin() + part * 32 + (32 - n));
        i += der[i - 1];
    }
    return i == der.size();
}

std::string sign(const Key& k, const std::string& payload) {
    const Bytes h = sha256(payload);
    Bytes rs(64);
    ULONG n = 0;
    if (h.empty() || BCryptSignHash(k.key, nullptr, const_cast<PUCHAR>(h.data()), 32, rs.data(), 64, &n, 0) != 0 ||
        n != 64)
        return "";
    return base64(der_of(rs));
}

bool verify(const Key& k, const std::string& payload, const std::string& sig) {
    Bytes der, rs;
    if (!unbase64(sig, &der) || !rs_of(der, &rs)) return false;
    const Bytes h = sha256(payload);
    return !h.empty() && BCryptVerifySignature(k.key, nullptr, const_cast<PUCHAR>(h.data()), 32, rs.data(), 64, 0) == 0;
}

bool load_key(Key* k) {
    Bytes sealed;
    if (!k->alg || !read_bytes(key_file(), &sealed)) return false;
    DATA_BLOB in{static_cast<DWORD>(sealed.size()), sealed.data()}, out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return false;
    const bool ok = BCryptImportKeyPair(k->alg, nullptr, BCRYPT_ECCPRIVATE_BLOB, &k->key, out.pbData, out.cbData, 0) == 0;
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return ok;
}

// The key, made on first use. A new key cannot verify what the old one signed,
// so the store starts over with it, as upstream's loadOrCreateKey does.
bool load_or_create_key(Key* k, std::string* why) {
    std::error_code ec;
    fs::create_directories(dir(), ec);
    if (!load_key(k)) {
        if (!k->alg || BCryptGenerateKeyPair(k->alg, &k->key, 256, 0) != 0 || BCryptFinalizeKeyPair(k->key, 0) != 0) {
            *why = "could not make a signing key";
            return false;
        }
        Bytes priv = k->blob(BCRYPT_ECCPRIVATE_BLOB);
        DATA_BLOB in{static_cast<DWORD>(priv.size()), priv.data()}, out{};
        const bool sealed = CryptProtectData(&in, L"Win.apk app-ops signing key", nullptr, nullptr, nullptr,
                                             CRYPTPROTECT_UI_FORBIDDEN, &out);
        SecureZeroMemory(priv.data(), priv.size());
        if (!sealed) {
            *why = "could not protect the signing key";
            return false;
        }
        const bool wrote = write_bytes(key_file(), out.pbData, out.cbData);
        LocalFree(out.pbData);
        if (!wrote) {
            *why = "could not write " + wide_to_utf8(key_file().wstring());
            return false;
        }
        if (fs::exists(store_file(), ec)) {
            const std::string empty = "{\n  \"apps\" : {\n\n  },\n  \"version\" : 2\n}\n";
            write_bytes(store_file(), empty.data(), empty.size());
        }
    }
    const std::string pub = base64(k->spki()) + "\n";
    Bytes have;
    if (!read_bytes(pub_file(), &have) || std::string(have.begin(), have.end()) != pub) {
        if (pub.size() < 8 || !write_bytes(pub_file(), pub.data(), pub.size())) {
            *why = "could not write " + wide_to_utf8(pub_file().wstring());
            return false;
        }
    }
    return true;
}

// ---- the store -----------------------------------------------------------------

struct Entry {
    std::string package, installed_at, updated_at, by, sig;
    std::map<std::string, std::string> ops, perms;
    bool has_perms = false;  // an S1 (/2) entry has no "perms" object at all
};
using Store = std::map<std::string, Entry>;  // package@installedAt -> entry

std::string key_of(const std::string& pkg, const std::string& installed_at) { return pkg + "@" + installed_at; }

// SpecialAccessStore.canonicalPayload, byte for byte.
bool payload(const char* tag, const Entry& e, std::string* out) {
    auto bad = [](const std::string& s) { return s.find('\n') != std::string::npos; };
    if (bad(e.package) || bad(e.installed_at) || bad(e.updated_at) || bad(e.by)) return false;
    std::string s = std::string(tag) + "\npackage=" + e.package + "\ninstalledAt=" + e.installed_at + "\n";
    for (const auto& [k, v] : e.ops) {
        if (bad(k) || bad(v)) return false;
        s += "op:" + k + "=" + v + "\n";
    }
    for (const auto& [k, v] : e.perms) {
        if (bad(k) || bad(v)) return false;
        s += "perm:" + k + "=" + v + "\n";
    }
    *out = s + "updatedAt=" + e.updated_at + "\nby=" + e.by + "\n";
    return true;
}

bool genuine(const Key& k, const Entry& e) {
    std::string p;
    if (e.sig.empty()) return false;
    if (payload(kPayloadTag, e, &p) && verify(k, p, e.sig)) return true;
    if (e.has_perms) return false;
    return payload(kLegacyTag, e, &p) && verify(k, p, e.sig);
}

Store read_store() {
    Store out;
    Bytes raw;
    if (!read_bytes(store_file(), &raw)) return out;
    const std::string text(raw.begin(), raw.end());
    Json j{text.data(), text.data() + text.size()};
    std::string version;
    Store apps;
    j.object([&](const std::string& k) {
        if (k == "version") return j.scalar(&version);
        if (k != "apps") return j.skip();
        j.object([&](const std::string& id) {
            Entry e;
            j.object([&](const std::string& f) {
                if (f == "package") j.str(&e.package);
                else if (f == "installedAt") j.str(&e.installed_at);
                else if (f == "updatedAt") j.str(&e.updated_at);
                else if (f == "by") j.str(&e.by);
                else if (f == "sig") j.str(&e.sig);
                else if (f == "ops" || f == "perms") {
                    auto& m = f == "ops" ? e.ops : e.perms;
                    if (f == "perms") e.has_perms = true;
                    j.object([&](const std::string& name) { j.str(&m[name]); });
                } else {
                    j.skip();
                }
            });
            apps[id] = e;
        });
    });
    if (j.ok && version == "2") out = std::move(apps);
    return out;
}

bool write_store(const Store& s) {
    auto map_json = [](const std::map<std::string, std::string>& m) {
        std::string o = "{";
        bool first = true;
        for (const auto& [k, v] : m) {
            o += (first ? "\n        " : ",\n        ") + json_string(k) + " : " + json_string(v);
            first = false;
        }
        return o + (first ? "}" : "\n      }");
    };
    std::string o = "{\n  \"apps\" : {";
    bool first = true;
    for (const auto& [id, e] : s) {
        o += (first ? "\n" : ",\n") + std::string("    ") + json_string(id) + " : {\n";
        first = false;
        o += "      \"by\" : " + json_string(e.by) + ",\n";
        o += "      \"installedAt\" : " + json_string(e.installed_at) + ",\n";
        o += "      \"ops\" : " + map_json(e.ops) + ",\n";
        o += "      \"package\" : " + json_string(e.package) + ",\n";
        if (e.has_perms) o += "      \"perms\" : " + map_json(e.perms) + ",\n";
        o += "      \"sig\" : " + json_string(e.sig) + ",\n";
        o += "      \"updatedAt\" : " + json_string(e.updated_at) + "\n    }";
    }
    o += "\n  },\n  \"version\" : 2\n}\n";
    return write_bytes(store_file(), o.data(), o.size());
}

// One writer at a time, across processes: a lock on app-ops.json.lock.
struct Lock {
    HANDLE h = INVALID_HANDLE_VALUE;
    bool held = false;
    explicit Lock(DWORD timeout_ms) {
        std::error_code ec;
        fs::create_directories(dir(), ec);
        h = CreateFileW((dir() / L"app-ops.json.lock").c_str(), GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
        for (DWORD waited = 0;; waited += 50) {
            OVERLAPPED o{};
            if (LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &o)) {
                held = true;
                return;
            }
            if (waited >= timeout_ms) return;
            Sleep(50);
        }
    }
    ~Lock() {
        if (held) {
            OVERLAPPED o{};
            UnlockFileEx(h, 0, 1, 0, &o);
        }
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
};

std::string now_iso() {
    SYSTEMTIME t;
    GetSystemTime(&t);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%04u-%02u-%02uT%02u:%02u:%02uZ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
                  t.wSecond);
    return buf;
}

// Who the grant is for: a row of installed_apps.json.
struct Identity {
    std::string package, installed_at, label, dir_name;
};

// The verified entry for `id`, or an empty one (nothing stored, or nothing genuine).
Entry stored(const Identity& id) {
    Key k;
    if (!load_key(&k)) return {};
    const Store s = read_store();
    const auto it = s.find(key_of(id.package, id.installed_at));
    if (it == s.end() || it->second.package != id.package || it->second.installed_at != id.installed_at ||
        !genuine(k, it->second))
        return {};
    return it->second;
}

std::map<std::string, std::string> permissions(const Identity& id) {
    std::map<std::string, std::string> out;
    for (const auto& [p, st] : stored(id).perms)
        if (is_runtime_perm(p) && is_state(st)) out[p] = st;
    return out;
}

// The stored mode of `op` ("allow" or "deny"), or "" when none is.
std::string mode(const Identity& id, const std::string& op) {
    const Entry e = stored(id);
    const auto it = e.ops.find(op);
    return it == e.ops.end() ? "" : it->second;
}

// Records `perms` and `ops` changes for `id`, keeping what a genuine entry
// already holds, and signs the result (upstream's setPermissions / setMode).
bool record(const Identity& id, const std::map<std::string, std::string>& perms,
            const std::map<std::string, std::string>& ops, const std::string& by, std::string* why) {
    for (const auto& [p, st] : perms) {
        if (!is_runtime_perm(p) || !is_state(st)) {
            *why = "refusing to sign " + p + "=" + st;
            return false;
        }
    }
    for (const auto& [op, m] : ops) {
        if (!is_op(op) || (m != "allow" && m != "deny")) {
            *why = "refusing to sign " + op + "=" + m;
            return false;
        }
    }
    Lock lock(10000);
    if (!lock.held) {
        *why = "could not lock the store";
        return false;
    }
    Key k;
    if (!load_or_create_key(&k, why)) return false;
    Store s = read_store();
    const std::string key = key_of(id.package, id.installed_at);
    Entry e;
    if (const auto it = s.find(key); it != s.end() && it->second.package == id.package &&
                                     it->second.installed_at == id.installed_at && genuine(k, it->second))
        e = it->second;
    e.package = id.package;
    e.installed_at = id.installed_at;
    for (const auto& [p, st] : perms) e.perms[p] = st;
    for (const auto& [op, m] : ops) e.ops[op] = m;
    e.has_perms = true;
    e.updated_at = now_iso();
    e.by = by;
    std::string p;
    if (!payload(kPayloadTag, e, &p) || (e.sig = sign(k, p)).empty()) {
        *why = "could not sign the entry for " + key;
        return false;
    }
    s[key] = e;
    if (!write_store(s)) {
        *why = "could not write " + wide_to_utf8(store_file().wstring());
        return false;
    }
    return true;
}

// Removes every grant of `pkg` (the app left the library: a reinstall starts
// with none, as AOSP clears app-ops on uninstall).
void forget(const std::string& pkg) {
    std::error_code ec;
    if (pkg.empty() || !fs::exists(store_file(), ec)) return;
    Lock lock(2000);
    if (!lock.held) return;
    Store s = read_store();
    bool changed = false;
    for (auto it = s.begin(); it != s.end();) {
        if (it->second.package == pkg) {
            it = s.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }
    if (changed) write_store(s);
}

// The registry row of `pkg` (write_registry keeps one per package).
bool identity_of(const std::string& pkg, Identity* id) {
    Bytes raw;
    if (pkg.empty() || !read_bytes(data_dir() / L"installed_apps.json", &raw)) return false;
    const std::string text(raw.begin(), raw.end());
    Json j{text.data(), text.data() + text.size()};
    int found = 0;
    j.object([&](const std::string& k) {
        if (k != "apps") return j.skip();
        j.array([&] {
            Identity row;
            std::string state;  // Json::str appends; a row without one is installed
            j.object([&](const std::string& f) {
                if (f == "package") j.str(&row.package);
                else if (f == "installedAt") j.str(&row.installed_at);
                else if (f == "label") j.str(&row.label);
                else if (f == "dirName") j.str(&row.dir_name);
                else if (f == "state") j.str(&state);
                else j.skip();
            });
            if (row.package == pkg && (state.empty() || state == "installed") && !row.installed_at.empty()) {
                ++found;
                *id = row;
            }
        });
    });
    return j.ok && found == 1;
}

// The registry's installedAt of `pkg`, empty when it has no row (write_registry keeps it across updates).
std::string installed_at(const std::string& pkg) {
    Identity id;
    return identity_of(pkg, &id) ? id.installed_at : std::string();
}

// ---- the caller ----------------------------------------------------------------

struct Caller {
    Identity id;
    std::wstring name;  // the library name, for the icon
    DWORD pid = 0;
};

std::wstring command_line_of(HANDLE process) {
    using Query = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    const auto query = reinterpret_cast<Query>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess")));
    if (!query) return L"";
    std::vector<unsigned char> buf(64 * 1024);
    ULONG n = 0;
    constexpr ULONG kProcessCommandLineInformation = 60;
    if (query(process, kProcessCommandLineInformation, buf.data(), static_cast<ULONG>(buf.size()), &n) != 0)
        return L"";
    const auto* s = reinterpret_cast<const UNICODE_STRING*>(buf.data());
    return s->Buffer ? std::wstring(s->Buffer, s->Length / sizeof(wchar_t)) : L"";
}

// The app's --apk, from a winapk-runtime.exe's command line; empty when it is
// not one.
fs::path apk_of_command_line(const std::wstring& cmd) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(cmd.c_str(), &argc);
    fs::path apk;
    for (int i = 1; argv && i + 1 < argc; ++i)
        if (wcscmp(argv[i], L"--apk") == 0) apk = argv[i + 1];
    if (argv) LocalFree(argv);
    return apk;
}

// Ends the runtime running `apk`, whoever started it (the library window, a
// shortcut's Win.apk.exe --run): Android kills a process whose runtime
// permission or all-files access was taken away, and the app comes back
// clean when opened again. True when one was found. With `wait_ms`, waits
// that long for each one to have ended (TerminateProcess does not).
bool stop_runtime_of(const fs::path& apk, DWORD wait_ms) {
    bool stopped = false;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{sizeof(pe)};
    std::error_code ec;
    for (BOOL more = Process32FirstW(snap, &pe); more; more = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, L"winapk-runtime.exe") != 0) continue;
        HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE, FALSE,
                               pe.th32ProcessID);
        if (!p) continue;
        const fs::path theirs = apk_of_command_line(command_line_of(p));
        if (!theirs.empty() && fs::equivalent(theirs, apk, ec) && TerminateProcess(p, 1)) {
            stopped = true;
            if (wait_ms) WaitForSingleObject(p, wait_ms);
        }
        CloseHandle(p);
    }
    CloseHandle(snap);
    return stopped;
}

// Ends every runtime running the library app `name` (its APK is library\<name>.<ext>), whoever
// started it, with `exit_code`. True when one was found.
bool stop_library_app(const std::wstring& name, UINT exit_code) {
    bool stopped = false;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{sizeof(pe)};
    std::error_code ec;
    for (BOOL more = Process32FirstW(snap, &pe); more; more = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, L"winapk-runtime.exe") != 0) continue;
        HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
        if (!p) continue;
        const fs::path theirs = apk_of_command_line(command_line_of(p));
        if (!theirs.empty() && _wcsicmp(theirs.stem().c_str(), name.c_str()) == 0 &&
            fs::equivalent(theirs.parent_path(), library_dir(), ec) && TerminateProcess(p, exit_code))
            stopped = true;
        CloseHandle(p);
    }
    CloseHandle(snap);
    return stopped;
}

// The app asking: our parent must be a winapk-runtime.exe that started before
// us (not a reused process id), and its --apk a library app whose package has
// one registry row. Nothing the app passes in its arguments is believed.
bool resolve(Caller* c, std::string* why) {
    const DWORD self = GetCurrentProcessId();
    DWORD parent = 0;
    if (HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0); snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe{sizeof(pe)};
        for (BOOL more = Process32FirstW(snap, &pe); more; more = Process32NextW(snap, &pe))
            if (pe.th32ProcessID == self) parent = pe.th32ParentProcessID;
        CloseHandle(snap);
    }
    HANDLE p = parent ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, parent) : nullptr;
    if (!p) {
        *why = "the parent process cannot be opened";
        return false;
    }
    wchar_t image[MAX_PATH * 2];
    DWORD len = MAX_PATH * 2;
    const bool named = QueryFullProcessImageNameW(p, 0, image, &len) &&
                       _wcsicmp(fs::path(image).filename().c_str(), L"winapk-runtime.exe") == 0;
    FILETIME pc{}, sc{}, x1, x2, x3;
    const bool older = GetProcessTimes(p, &pc, &x1, &x2, &x3) &&
                       GetProcessTimes(GetCurrentProcess(), &sc, &x1, &x2, &x3) && CompareFileTime(&pc, &sc) <= 0;
    const std::wstring cmd = named && older ? command_line_of(p) : L"";
    CloseHandle(p);
    if (!named || !older) {
        *why = "the parent process is not an app run by Win.apk";
        return false;
    }
    const fs::path apk = apk_of_command_line(cmd);
    std::error_code ec;
    for (const App& app : library()) {
        if (apk.empty() || !fs::equivalent(app.apk, apk, ec)) continue;
        const std::vector<std::string> pkg = package_of(app.name);
        if (pkg.empty() || !identity_of(pkg[0], &c->id)) {
            *why = "no single registry row for " + wide_to_utf8(app.name);
            return false;
        }
        if (c->id.label.empty()) c->id.label = wide_to_utf8(app.label);
        c->name = app.name;
        c->pid = parent;
        return true;
    }
    *why = "the parent runs no library app (" + wide_to_utf8(apk.wstring()) + ")";
    return false;
}

// Whether the app asking is the real Google Play Store: the parent runtime runs the library's
// com.android.vending (resolve), and winapk-runtime --verify-apk says Google signed that APK and it verifies.
bool caller_is_play_store() {
    Caller c;
    std::string why;
    if (!resolve(&c, &why) || c.id.package != "com.android.vending") return false;
    const App* app = find_app(c.name);
    if (!app) return false;
    std::wstring out;
    const fs::path log = logs_dir() / L"install-helper.log";
    if (run_runtime_captured(L"--verify-apk " + gms_arg(app->apk.wstring()), &out, log) != 0) return false;
    bool pkg = false, verified = false, google = false;
    for (size_t at = 0; at < out.size();) {
        size_t end = out.find(L'\n', at);
        if (end == std::wstring::npos) end = out.size();
        std::wstring line = out.substr(at, end - at);
        at = end + 1;
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (line == L"PACKAGE\tcom.android.vending") pkg = true;
        else if (line == L"VERIFIED\t1") verified = true;
        else if (line == L"GOOGLE\t1") google = true;
    }
    return pkg && verified && google;
}

// Whether the app asking is Win.apk's own Store as the launcher installed it: the parent runtime runs the
// library's app.apkstore and its APK is the release asset whose SHA-256 the launcher checked against GitHub's and
// noted at its install (store-app\installed.sha256). A copy of the Store from anywhere else is an ordinary store.
bool caller_is_builtin_store() {
    Caller c;
    std::string why;
    if (!resolve(&c, &why) || c.id.package != winapk::launcher::kStorePackage) return false;
    const App* app = find_app(c.name);
    if (!app) return false;
    const std::string want = installed_store_sha();
    return !want.empty() && sha256_of(app->apk) == want;
}

// ---- the dialog ----------------------------------------------------------------
//
// Every question an app asks is the launcher's dialog (Prompt, run_prompt): the
// app's icon and name, one question, at most a line saying what it means here,
// switches when there is something to pick, and the buttons. No answer in five
// minutes (upstream's alert timeout) changes nothing.

constexpr int kAnswerAllow = 100, kAnswerDeny = 101, kAnswerSelect = 102;

// A question with buttons (the first is the accent one), as the app `c`: its name and icon head it.
int ask(const Caller& c, const std::wstring& question, const std::wstring& note,
        const std::vector<std::pair<int, std::wstring>>& choices, int default_id,
        std::vector<Prompt::Switch>* switches = nullptr) {
    Prompt p;
    p.app = utf8_to_wide(c.id.label.empty() ? c.id.package : c.id.label);
    p.icon = rounded_icon(app_dir(c.name) / L"icon.png", ipx(40));
    p.question = question;
    p.note = note;
    p.buttons = choices;
    p.default_id = default_id;
    if (switches) p.switches = *switches;
    const int pressed = run_prompt(p, c.pid);
    if (switches) *switches = p.switches;
    return pressed;
}

std::wstring quoted(const Identity& id) { return utf8_to_wide(id.label.empty() ? id.package : id.label); }

// ---- runtime-permission ----------------------------------------------------------

// The group a permission's dialog belongs to (PermissionMapping.kt :94-150):
// the storage family's three, location, or one of kGroupRows'.
std::string group_of(const std::string& p, int level) {
    if (is_location(p)) return "location";
    if (is_group_perm(p)) return group_name(p, level);
    if (p == kReadExternal || p == kWriteExternal) return "storage";
    if (p == kMediaLocation) return level < 33 ? "storage" : "visual";
    if (p == kAudio) return "aural";
    return "visual";
}

// The permissions whose grant means the group is granted; MUST agree with
// SpecialAccessStore.PermModel.groupFullyGranted (the photo-picker prompt is
// off, as upstream's: USER_SELECTED never counts) and GroupModel's (any
// member of the group).
std::set<std::string> anchors(const std::string& g, int level) {
    if (g == "storage")
        return level < 33 ? std::set<std::string>{kReadExternal, kWriteExternal, kMediaLocation}
                          : std::set<std::string>{kReadExternal, kWriteExternal};
    if (g == "aural") return {kAudio};
    if (g == "location") return {kFine, kCoarse};
    if (is_group_name(g)) {
        std::set<std::string> out;
        for (const PermRow& r : kGroupRows)
            if (group_name(r.perm, level) == g) out.insert(r.perm);
        return out;
    }
    return {kImages, kVideo, kMediaLocation};
}

// GrantPermissionsViewModel's silent grant into a group that is already granted.
bool auto_grant_allowed(const std::vector<std::string>& perms, const std::map<std::string, std::string>& cur,
                        int level) {
    if (perms.empty()) return false;
    for (const std::string& p : perms) {
        bool any = false;
        for (const std::string& a : anchors(group_of(p, level), level)) {
            const auto it = cur.find(a);
            any = any || (it != cur.end() && is_granted(it->second));
        }
        if (!any) return false;
    }
    return true;
}

// What an answer writes, only the tokens that change (onPermissionGrantResult,
// GrantPermissionsViewModel.kt :678-770): Allow grants every permission asked
// for; Don't allow denies them, fixed when the group had been answered before;
// "Only approximate" grants COARSE and denies FINE.
std::map<std::string, std::string> changes(int answer, const std::vector<std::string>& perms,
                                           const std::vector<std::string>& members,
                                           const std::vector<std::string>& partial,
                                           const std::map<std::string, std::string>& cur) {
    bool user_set = false;
    for (const auto* list : {&members, &perms})
        for (const std::string& p : *list) user_set = user_set || cur.count(p);
    const std::string denied = user_set ? "denied_fixed" : "denied";
    std::map<std::string, std::string> want;
    for (const std::string& p : perms) {
        const bool part = std::find(partial.begin(), partial.end(), p) != partial.end();
        if (answer == kAnswerAllow) want[p] = "granted";
        else if (answer == kAnswerSelect) want[p] = part ? "granted" : denied;
        else if (answer == kAnswerDeny) want[p] = denied;
    }
    for (auto it = want.begin(); it != want.end();) {
        const auto c = cur.find(it->first);
        it = c != cur.end() && c->second == it->second ? want.erase(it) : std::next(it);
    }
    return want;
}

// The question, worded from the permissions (PermissionController strings.xml).
std::wstring question(const Identity& id, const std::string& prompt, const std::vector<std::string>& perms,
                      int level) {
    const std::wstring who = quoted(id);
    if (prompt == "location") return fmt(L"Allow {0} to access this PC's location?", {who});
    if (prompt == "location_background")
        return fmt(L"Allow {0} to access this PC's location all the time?", {who});
    if (prompt == "camera") return fmt(L"Allow {0} to take pictures and record video?", {who});
    if (prompt == "microphone") return fmt(L"Allow {0} to record audio?", {who});
    if (prompt == "contacts") return fmt(L"Allow {0} to access your contacts?", {who});
    if (prompt == "calendar") return fmt(L"Allow {0} to access your calendar?", {who});
    if (prompt == "phone") return fmt(L"Allow {0} to make and manage phone calls?", {who});
    if (prompt == "call_log") return fmt(L"Allow {0} to access your phone call logs?", {who});
    if (prompt == "sms") return fmt(L"Allow {0} to send and view SMS messages?", {who});
    if (prompt == "notifications") return fmt(L"Allow {0} to send you notifications?", {who});
    if (prompt == "nearby_devices")
        return fmt(L"Allow {0} to find, connect to, and determine the relative position of nearby devices?", {who});
    if (prompt == "activity_recognition") return fmt(L"Allow {0} to access your physical activity?", {who});
    if (prompt == "sensors") return fmt(L"Allow {0} to access sensor data about your vital signs?", {who});
    if (prompt == "sensors_background")
        return fmt(L"Allow {0} to access sensor data about your vital signs all the time?", {who});
    if (prompt == "supergroup_pre_q")
        return fmt(L"Allow {0} to access photos, videos, music, audio, and other files on this PC?", {who});
    std::set<std::string> groups;
    for (const std::string& p : perms) groups.insert(group_of(p, level));
    if (prompt == "supergroup_q_to_s" || groups.size() > 1)
        return fmt(L"Allow {0} to access photos, videos, music, and audio on this PC?", {who});
    const std::string g = groups.empty() ? "storage" : *groups.begin();
    if (g == "aural") return fmt(L"Allow {0} to access music and audio on this PC?", {who});
    if (g == "visual") return fmt(L"Allow {0} to access photos and videos on this PC?", {who});
    return fmt(L"Allow {0} to access photos, media, and files on this PC?", {who});
}

void emit(std::initializer_list<std::pair<const char*, std::string>> fields) {
    std::string o = "{";
    for (const auto& [k, v] : fields) o += (o.size() > 1 ? "," : "") + json_string(k) + ":" + json_string(v);
    helper_say(o + "}");
}

int usage(const std::string& message) {
    emit({{"status", "error"}, {"message", message}});
    return 2;
}

int refused(const std::string& why) {
    emit({{"status", "identity_refused"}, {"message", why}});
    return 5;
}

int runtime_permission(int level, const std::string& prompt, bool automatic, const std::vector<std::string>& perms,
                       const std::vector<std::string>& members, const std::vector<std::string>& partial) {
    if (level < 23) return usage("--level must be the presented API level");
    const bool location = prompt == "location";
    // An --auto request names no prompt: its family is its permissions'.
    const bool group = is_group_name(prompt) || (automatic && !perms.empty() && is_group_perm(perms[0]));
    auto known = [&](const std::string& p) {
        if (location) return is_location(p);
        if (group) return is_group_perm(p) && (automatic || group_name(p, level) == prompt);
        return is_storage(p);
    };
    if (perms.empty() || !std::all_of(perms.begin(), perms.end(), known) ||
        !std::all_of(members.begin(), members.end(), known))
        return usage(location ? "--prompt location takes only location runtime permissions"
                     : group  ? "--perm/--member must name runtime permissions of the --prompt group at --level"
                              : "--perm/--member must name storage runtime permissions");
    for (const std::string& p : partial)
        if (!location || p != kCoarse || std::find(perms.begin(), perms.end(), p) == perms.end())
            return usage("--partial is only ACCESS_COARSE_LOCATION with --prompt location, and one of the --perm values"
                         " (there is no photo picker: Allow means allow all)");
    static const std::set<std::string> offered = {"basic", "supergroup_pre_q", "supergroup_q_to_s", "location"};
    if (!automatic && !offered.count(prompt) && !is_group_name(prompt))
        return usage("--prompt must be basic, supergroup_pre_q, supergroup_q_to_s, location or a permission group"
                     " (or --auto)");
    Caller c;
    std::string why;
    if (!resolve(&c, &why)) return refused(why);
    const std::map<std::string, std::string> cur = permissions(c.id);
    // An "all the time" permission is never granted without its foreground
    // one (LocationGrantBehavior drops it from the request); the runtime
    // checks the same before asking, this is the helper's own reading.
    for (const std::string& p : perms) {
        const std::vector<std::string> fg = foreground_of(p);
        if (fg.empty()) continue;
        const bool any = std::any_of(fg.begin(), fg.end(), [&](const std::string& f) {
            const auto it = cur.find(f);
            return it != cur.end() && is_granted(it->second);
        });
        if (!any) {
            emit({{"status", "refused"}, {"message", p + " needs a granted foreground permission first"}});
            return 6;
        }
    }
    if (automatic) {
        if (!auto_grant_allowed(perms, cur, level)) {
            emit({{"status", "refused"}, {"message", "--auto needs a granted permission in the same group"}});
            return 6;
        }
        std::map<std::string, std::string> grant;
        for (const std::string& p : perms)
            if (!cur.count(p) || cur.at(p) != "granted") grant[p] = "granted";
        if (!grant.empty() && !record(c.id, grant, {}, "helper", &why)) {
            emit({{"status", "error"}, {"message", "could not record the grant: " + why}});
            return 5;
        }
        emit({{"status", "auto_granted"}, {"dirName", c.id.dir_name}});
        return 0;
    }
    // What stands behind the grant on this PC, where it is not what the user would guess.
    const wchar_t* note = nullptr;
    if (location || prompt == "location_background") note = L"Windows' location settings apply too.";
    else if (prompt == "camera") note = L"Windows' camera settings apply too.";
    else if (prompt == "microphone") note = L"Windows' microphone settings apply too.";
    else if (prompt == "notifications") note = L"They show as Windows notifications.";
    else if (prompt == "contacts" || prompt == "calendar" || prompt == "phone" || prompt == "call_log" ||
             prompt == "sms")
        note = L"This PC has none of these for the app to find.";
    else if (prompt == "activity_recognition" || prompt == "sensors" || prompt == "sensors_background")
        note = L"This PC has no such sensors.";
    else if (prompt == "nearby_devices") note = L"These are the Bluetooth devices near this PC.";
    // Location's "Only approximate" is the Precise switch, as Android 12 asks it.
    std::vector<Prompt::Switch> precise;
    if (!partial.empty()) precise.push_back({tr(L"Precise location"), true});
    int answer = ask(c, question(c.id, prompt, perms, level), note ? tr(note) : L"",
                     {{kAnswerAllow, tr(L"Allow")}, {kAnswerDeny, tr(L"Don't allow")}}, kAnswerDeny, &precise);
    if (answer == kAnswerAllow && !precise.empty() && !precise[0].on) answer = kAnswerSelect;
    if (answer == 0) {
        emit({{"status", "timeout"}, {"message", "the dialog was closed without an answer; nothing changed"}});
        return 4;
    }
    const auto want = changes(answer, perms, members, partial, cur);
    if (!want.empty() && !record(c.id, want, {}, "helper", &why)) {
        emit({{"status", "error"}, {"message", "could not record the answer: " + why}});
        return 5;
    }
    if (answer == kAnswerDeny) {
        emit({{"status", "denied"}, {"dirName", c.id.dir_name}});
        return 3;
    }
    emit({{"status", answer == kAnswerAllow ? "granted" : "partial"}, {"dirName", c.id.dir_name}});
    return 0;
}

// ---- special-access ----------------------------------------------------------------

// A Settings screen for access this host cannot grant (notification access,
// accessibility, usage access, Do Not Disturb) or does not restrict (battery
// optimization, exact alarms, system settings): the dialog says so, with one
// button, and nothing is recorded.
int special_access_info(const Caller& c, const std::string& op) {
    const std::wstring who = quoted(c.id);
    std::wstring main, body;
    if (op == "winapk:notification_listener") {
        main = tr(L"Notification access");
        body = fmt(L"Win.apk can't give {0} notification access. Windows has no feed of other apps' notifications "
                   L"for an Android app to read.",
                   {who});
    } else if (op == "winapk:accessibility") {
        main = tr(L"Accessibility");
        body = fmt(L"Win.apk can't run {0} as an accessibility service. There is no Android accessibility "
                   L"framework on Windows for it to observe or act through.",
                   {who});
    } else if (op == "winapk:usage_access") {
        main = tr(L"Usage access");
        body = fmt(L"Win.apk can't give {0} usage access. Each Android app runs on its own here, so there is no "
                   L"usage history of other apps to read.",
                   {who});
    } else if (op == "winapk:notification_policy") {
        main = tr(L"Do Not Disturb access");
        body = fmt(L"Win.apk has no Do Not Disturb mode for {0} to control.", {who});
    } else if (op == "winapk:battery") {
        main = tr(L"Battery optimization");
        body = fmt(L"Win.apk never limits {0} in the background, so it already runs unrestricted.", {who});
    } else if (op == "winapk:exact_alarm") {
        main = tr(L"Alarms and reminders");
        body = fmt(L"Win.apk lets {0} schedule exact alarms; there is nothing to turn on.", {who});
    } else if (op == "winapk:write_settings") {
        main = tr(L"Modify system settings");
        body = fmt(L"There are no Android system settings for {0} to modify on Windows; it is treated as allowed.",
                   {who});
    } else {
        return usage("unknown --op " + op);
    }
    ask(c, main, body, {{kAnswerDeny, tr(L"OK")}}, kAnswerDeny);
    emit({{"status", "info"}, {"op", op}, {"dirName", c.id.dir_name}});
    return 0;
}

// Settings' "All files access" / "Media management" / "Display over other
// apps" / "Install unknown apps" switch for the app. The default button keeps
// the current state, as upstream's alert's Return does. 0 allowed, 3 not
// allowed, 4 no answer (nothing changed), 5 the answer could not be recorded.
int special_access_switch(const Caller& c, const std::string& op, std::string* why) {
    const std::string current = mode(c.id, op);
    const bool files = op == kAllFiles;
    std::wstring main, body;
    if (files) {
        main = fmt(L"Allow {0} to manage all files?", {quoted(c.id)});
        body = tr(L"It can read, change and delete every file in your Downloads, Pictures, Documents, Music and "
                  L"Videos.");
    } else if (op == kManageMedia) {
        main = fmt(L"Allow {0} to manage media?", {quoted(c.id)});
        body = tr(L"It can change or delete photos, videos and music that other apps made, without asking you.");
    } else if (op == kOverlay) {
        main = fmt(L"Allow {0} to display over other apps?", {quoted(c.id)});
        body = tr(L"On Windows such windows stay inside the app's own window.");
    } else {
        main = fmt(L"Allow {0} to install other apps?", {quoted(c.id)});
        body = tr(L"Win.apk still asks you before each app it installs.");
    }
    // Enter keeps what is set now.
    const int answer = ask(c, main, body, {{kAnswerAllow, tr(L"Allow")}, {kAnswerDeny, tr(L"Don't allow")}},
                           current == "allow" ? kAnswerAllow : kAnswerDeny);
    if (answer == 0) return 4;
    const std::string want = answer == kAnswerAllow ? "allow" : "deny";
    // "Don't allow" writes only to turn an allowed switch off.
    if (want != current && (want == "allow" || current == "allow") && !record(c.id, {}, {{op, want}}, "helper", why))
        return 5;
    return want == "allow" ? 0 : 3;
}

int special_access(const std::string& op) {
    if (!is_op(op) && !is_info_op(op))
        return usage("--op must be " + kAllFiles + ", " + kManageMedia + ", " + kOverlay + " or " + kInstallApps);
    Caller c;
    std::string why;
    if (!resolve(&c, &why)) return refused(why);
    if (is_info_op(op)) return special_access_info(c, op);
    const int code = special_access_switch(c, op, &why);
    if (code == 4) {
        emit({{"status", "timeout"},
              {"op", op},
              {"message", "the dialog was closed without an answer; nothing changed"}});
    } else if (code == 5) {
        emit({{"status", "error"}, {"message", "could not record the answer: " + why}});
    } else {
        emit({{"status", code == 0 ? "allowed" : "denied"}, {"op", op}, {"dirName", c.id.dir_name}});
    }
    return code;
}

// Before an app's install (the install helper): whether the app asking may install apps, as Android's
// installer decides before its question (PackageInstallerActivity.checkIfAllowedAndInitiateInstall). Its
// "Install unknown apps" switch on: yes. Off: the dialog Android shows for a blocked source, whose Settings
// is that switch (special_access_switch, as the app's own MANAGE_UNKNOWN_APP_SOURCES opens it); turned on
// there, the install goes on to its question. With no app identity (not a library app's runtime) there is no
// switch to read: the install's own question still guards it.
bool installs_allowed_from_caller() {
    Caller c;
    std::string why;
    if (!resolve(&c, &why)) return true;
    if (mode(c.id, kInstallApps) == "allow") return true;
    constexpr int kSettings = 103;
    const int answer = ask(c, tr(L"For your security, your PC currently isn't allowed to install unknown apps "
                                 L"from this source."),
                           tr(L"You can change this in Settings."),
                           {{kSettings, tr(L"Settings")}, {kAnswerDeny, tr(L"Cancel")}}, kSettings);
    if (answer != kSettings) return false;
    return special_access_switch(c, kInstallApps, &why) == 0;
}

// Win.apk's own Store (store_app.h) may install apps from its first install, as a device's preinstalled store
// may: its "Install unknown apps" is set to allow while no answer is stored for it. A later "Don't allow" stays
// (an update keeps installedAt, so the entry); a removal forgets it with the app.
void grant_builtin_store() {
    Identity id;
    std::string why;
    if (!identity_of(winapk::launcher::kStorePackage, &id) || !mode(id, kInstallApps).empty()) return;
    record(id, {}, {{kInstallApps, "allow"}}, "system", &why);
}

// ---- app-details --------------------------------------------------------------------

// One line of the App info dialog: a permission group or a special-access op.
struct Item {
    std::wstring title;
    std::vector<std::string> perms;  // a group's permissions
    std::string op;                  // or an op
    bool on = false, was = false;
};

// A group's name in App info and Get Info (Settings' permission-group labels).
std::wstring group_title(const std::string& name, int level) {
    if (name == "READ_MEDIA_VISUAL") return tr(L"Photos and videos");
    if (name == "READ_MEDIA_AURAL") return tr(L"Music and audio");
    if (name == "STORAGE") return level < 33 ? tr(L"Files and media") : tr(L"Files");
    if (name == "storage-supergroup") return tr(L"Photos, videos, music, and audio");
    if (name == "location") return tr(L"Location");
    if (name == "location_background") return tr(L"Location, all the time");
    if (name == "camera") return tr(L"Camera");
    if (name == "microphone") return tr(L"Microphone");
    if (name == "contacts") return tr(L"Contacts");
    if (name == "calendar") return tr(L"Calendar");
    if (name == "phone") return tr(L"Phone");
    if (name == "call_log") return tr(L"Call logs");
    if (name == "sms") return tr(L"SMS");
    if (name == "notifications") return tr(L"Notifications");
    if (name == "nearby_devices") return tr(L"Nearby devices");
    if (name == "activity_recognition") return tr(L"Physical activity");
    if (name == "sensors") return tr(L"Body sensors");
    if (name == "sensors_background") return tr(L"Body sensors, all the time");
    return utf8_to_wide(name);
}

std::wstring op_title(const std::string& op) {
    if (op == kAllFiles) return tr(L"All files access");
    if (op == kManageMedia) return tr(L"Media management");
    if (op == kOverlay) return tr(L"Display over other apps");
    return tr(L"Install unknown apps");
}

int app_details(int level, const std::vector<std::string>& groups, const std::vector<std::string>& ops) {
    if (level < 23) return usage("--level must be the presented API level");
    Caller c;
    std::string why;
    if (!resolve(&c, &why)) return refused(why);
    const auto cur = permissions(c.id);
    std::vector<Item> items;
    static const std::set<std::string> storage_names = {"STORAGE", "READ_MEDIA_VISUAL", "READ_MEDIA_AURAL",
                                                        "storage-supergroup"};
    for (const std::string& spec : groups) {
        const size_t eq = spec.find('=');
        Item it;
        if (eq == std::string::npos) return usage("bad --group " + spec);
        const std::string name = spec.substr(0, eq);
        for (size_t at = eq + 1; at <= spec.size();) {
            const size_t comma = std::min(spec.find(',', at), spec.size());
            if (comma > at) it.perms.push_back(spec.substr(at, comma - at));
            at = comma + 1;
        }
        // A row's permissions are its group's: the storage names take storage
        // permissions, "location" the two foreground ones, a group of
        // kGroupRows its members at this level.
        auto belongs = [&](const std::string& p) {
            if (storage_names.count(name)) return is_storage(p);
            if (name == "location") return is_location(p);
            return is_group_name(name) && group_name(p, level) == name;
        };
        if (it.perms.empty() || !std::all_of(it.perms.begin(), it.perms.end(), belongs))
            return usage("bad --group " + spec);
        std::set<std::string> anchor;
        for (const std::string& p : it.perms)
            for (const std::string& a : anchors(group_of(p, level), level)) anchor.insert(a);
        for (const std::string& p : it.perms)
            it.on = it.on || (anchor.count(p) && cur.count(p) && is_granted(cur.at(p)));
        it.title = group_title(name, level);
        it.was = it.on;
        items.push_back(it);
    }
    for (const std::string& op : ops) {
        if (!is_op(op)) return usage("bad --op " + op);
        Item it;
        it.op = op;
        it.title = op_title(op);
        it.on = it.was = mode(c.id, op) == "allow";
        items.push_back(it);
    }
    // One switch per item, and Done / Cancel: what changed is written on Done.
    Prompt pr;
    pr.app = utf8_to_wide(c.id.label.empty() ? c.id.package : c.id.label);
    pr.icon = rounded_icon(app_dir(c.name) / L"icon.png", ipx(40));
    pr.question = fmt(L"{0}'s permissions", {pr.app});
    if (items.empty()) {
        pr.note = tr(L"This app has no permissions to set.");
        pr.buttons = {{IDOK, tr(L"OK")}};
    } else {
        pr.note = fmt(L"Turning a permission off closes {0}.", {pr.app});
        for (const Item& it : items) pr.switches.push_back({it.title, it.on});
        pr.buttons = {{IDOK, tr(L"Done")}, {IDCANCEL, tr(L"Cancel")}};
        pr.footer = true;
    }
    pr.default_id = IDOK;
    const int pressed = run_prompt(pr, c.pid);
    for (size_t i = 0; i < items.size() && i < pr.switches.size(); ++i) items[i].on = pr.switches[i].on;
    if (pressed != IDOK || items.empty()) {
        emit({{"status", pressed == IDOK ? "done" : "cancelled"}, {"changed", "no"}});
        return 0;
    }
    std::map<std::string, std::string> perms, opm;
    for (const Item& it : items) {
        if (it.on == it.was) continue;
        if (!it.op.empty()) opm[it.op] = it.on ? "allow" : "deny";
        for (const std::string& p : it.perms) {
            const std::string want = it.on ? "granted" : "denied";
            if (!cur.count(p) || cur.at(p) != want) perms[p] = want;
        }
    }
    if ((!perms.empty() || !opm.empty()) && !record(c.id, perms, opm, "helper", &why)) {
        emit({{"status", "error"}, {"message", "could not record the change: " + why}});
        return 5;
    }
    emit({{"status", "done"}, {"changed", perms.empty() && opm.empty() ? "no" : "yes"}});
    return 0;
}

// ---- the library's side --------------------------------------------------------------

// The grants Get Info shows for an app: each stored permission group and op.
struct Grant {
    std::wstring title;
    std::vector<std::string> perms;
    std::string op;
    bool on = false;
};

std::vector<Grant> grants_of(const Identity& id) {
    const Entry e = stored(id);
    std::vector<Grant> out;
    struct G {
        const wchar_t* title;
        std::vector<std::string> perms;
    };
    std::vector<G> table = {{L"Files and media", {kReadExternal, kWriteExternal}},
                            {L"Photos and videos", {kImages, kVideo, kMediaLocation, kUserSelected}},
                            {L"Music and audio", {kAudio}},
                            {L"Location", {kFine, kCoarse}}};
    // Then every group of kGroupRows, in the table's order of first mention.
    for (const PermRow& r : kGroupRows) {
        G* g = nullptr;
        for (G& have : table)
            if (have.perms.size() && group_row(have.perms[0]) && std::string(group_row(have.perms[0])->group) == r.group)
                g = &have;
        if (!g) {
            table.push_back({L"", {}});
            g = &table.back();
        }
        g->perms.push_back(r.perm);
    }
    for (const G& g : table) {
        Grant gr;
        gr.title = g.title[0] ? tr(g.title) : group_title(group_row(g.perms[0])->group, 10000);
        for (const std::string& p : g.perms) {
            const auto it = e.perms.find(p);
            if (it == e.perms.end()) continue;
            gr.perms.push_back(p);
            gr.on = gr.on || is_granted(it->second);
        }
        if (!gr.perms.empty()) out.push_back(gr);
    }
    for (const std::string& op : {kAllFiles, kManageMedia, kOverlay, kInstallApps}) {
        const auto it = e.ops.find(op);
        if (it != e.ops.end()) out.push_back({op_title(op), {}, op, it->second == "allow"});
    }
    return out;
}

// Get Info took a grant away: Android ends a process whose runtime permission
// or all-files access was revoked (killUid, killAppForOpChange), and so does
// this, whoever started the app. Media management, overlays and installing
// have no kill rule on a device.
void revoked(const App& a, const std::string& op) {
    if (op == kManageMedia || op == kOverlay || op == kInstallApps) return;
    if (!stop_runtime_of(a.apk)) return;
    expect_run_end(a.name);  // started from here: the InfoBar says why it ended, no error splash
    info(fmt(L"{0} was stopped, as Android does when a permission is revoked.", {a.label}), kInfo);
}

// ---- app-not-responding -----------------------------------------------------------
//
// The runtime asks it when the app's main thread has not taken the input it was given for five seconds
// (com.winapk.aospviews.InputTimeout, InputDispatcher's timeout): "<app> isn't responding", over the
// app's window. Exit 3 Close app, 0 Wait (also Esc, the caption's close, and no answer in five minutes),
// 5 no identity.

int not_responding() {
    Caller c;
    std::string why;
    if (!resolve(&c, &why)) return refused(why);
    const std::wstring label = quoted(c.id);
    Prompt p = app_end_prompt(c.name, label, fmt(L"{0} isn't responding", {label}));
    not_responding_choices(p);
    const bool close = run_prompt(p, c.pid) == kCloseApp;
    emit({{"status", "ok"}, {"answer", close ? "close" : "wait"}});
    return close ? 3 : 0;
}

}  // namespace appops

void appops_forget(const std::string& pkg) { appops::forget(pkg); }

// The Get Info sheet's permissions: each grant as a switch, and a reset that
// forgets them all, so the app asks again.
std::vector<Row> permission_rows(const App& a) {
    std::vector<Row> rows;
    const std::vector<std::string> pkg = package_of(a.name);
    appops::Identity id;
    if (pkg.empty() || !appops::identity_of(pkg[0], &id)) return rows;
    for (const appops::Grant& g : appops::grants_of(id)) {
        Row r;
        r.kind = Row::kToggle;
        r.title = g.title;
        r.on = g.on;
        r.act = [a, id, g](int on) {
            std::map<std::string, std::string> perms, ops;
            for (const std::string& p : g.perms) perms[p] = on ? "granted" : "denied";
            if (!g.op.empty()) ops[g.op] = on ? "allow" : "deny";
            std::string why;
            if (!appops::record(id, perms, ops, "panel", &why))
                info(fmt(L"The permission could not be changed: {0}", {utf8_to_wide(why)}), kError);
            else if (!on && g.on) appops::revoked(a, g.op);
        };
        rows.push_back(r);
    }
    if (!rows.empty()) {
        bool any_on = false;
        for (const Row& r : rows) any_on = any_on || r.on;
        rows.push_back(button_row(tr(L"Reset permissions"), tr(L"The app asks again the next time it needs them."),
                                  tr(L"Reset"), [a, pkg, any_on] {
                                      appops::forget(pkg[0]);
                                      info(fmt(L"{0} will ask for its permissions again.", {a.label}), kSuccess);
                                      if (any_on) appops::revoked(a, "");
                                  }));
    }
    return rows;
}

int permission_main(int argc, wchar_t** argv) {
    const std::wstring verb = argv[1];
    std::string source, prompt;
    std::vector<std::string> perms, members, partial, groups, ops;
    int level = 0;
    bool automatic = false;
    for (int i = 2; i < argc; ++i) {
        const std::wstring a = argv[i];
        const bool has = i + 1 < argc;
        auto next = [&] { return wide_to_utf8(argv[++i]); };
        if (a == L"--source" && has) source = next();
        else if (a == L"--prompt" && has) prompt = next();
        else if (a == L"--perm" && has) perms.push_back(next());
        else if (a == L"--member" && has) members.push_back(next());
        else if (a == L"--partial" && has) partial.push_back(next());
        else if (a == L"--group" && has) groups.push_back(next());
        else if (a == L"--op" && has) ops.push_back(next());
        else if (a == L"--level" && has) level = std::atoi(next().c_str());
        else if (a == L"--auto") automatic = true;
        else if (a == L"--json") {}
        else return appops::usage("unknown argument " + wide_to_utf8(a));
    }
    // Only an app asks (the runtime passes it as a constant). No flag skips
    // the user's answer.
    if (source != "android")
        return appops::usage(wide_to_utf8(verb) + " is only asked for by an app (--source android)");
    if (verb == L"runtime-permission")
        return appops::runtime_permission(level, prompt, automatic, perms, members, partial);
    if (verb == L"app-not-responding") return appops::not_responding();
    if (verb == L"special-access") {
        if (ops.size() != 1) return appops::usage("--op takes one op");
        return appops::special_access(ops[0]);
    }
    return appops::app_details(level, groups, ops);
}

// One Win.apk.exe serves the library, the install window and the splashes: another start hands
// its files (or its wish to see the library) to it in a WM_COPYDATA, as one string of paths
// separated by '\n', led by "L" for "show the library" or "F" for files only.

bool forward_to_running(bool show_library, const std::vector<fs::path>& files) {
    // Explorer starts one process per selected file, all at once: the first takes the mutex and makes the
    // window, the others wait for that window to hand their file over.
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\WinApk-launcher");
    const bool first = mutex && GetLastError() != ERROR_ALREADY_EXISTS;
    HWND other = FindWindowW(L"WinApkLibrary", nullptr);
    for (int i = 0; !other && !first && i < 200; ++i) {  // up to 10 s
        Sleep(50);
        other = FindWindowW(L"WinApkLibrary", nullptr);
    }
    if (!other) return false;  // keep the mutex (leaked on purpose) for as long as this process serves
    std::wstring text = show_library ? L"L" : L"F";
    for (const fs::path& f : files) text += L"\n" + fs::absolute(f).wstring();
    DWORD pid = 0;
    GetWindowThreadProcessId(other, &pid);
    AllowSetForegroundWindow(pid);
    COPYDATASTRUCT cds{kCopyOpen, static_cast<DWORD>((text.size() + 1) * sizeof(wchar_t)), &text[0]};
    DWORD_PTR result = 0;
    const bool sent = SendMessageTimeoutW(other, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cds), SMTO_ABORTIFHUNG,
                                          10000, &result) && result;
    if (mutex) CloseHandle(mutex);
    return sent;
}

// The library window as it was last left: its size and whether it was maximized.
void show_library(int show) {
    g_closed_by_user = false;  // Win.apk is open again: background updates go on (maintenance_check)
    if (IsWindowVisible(g_main)) {
        if (IsIconic(g_main)) ShowWindow(g_main, SW_RESTORE);
    } else {
        ShowWindow(g_main, show == SW_SHOWDEFAULT && setting(L"WindowMaximized", 0) ? SW_SHOWMAXIMIZED
                           : show == SW_SHOWDEFAULT                               ? SW_SHOWNORMAL
                                                                                  : show);
    }
    SetForegroundWindow(g_main);
    refresh_running();  // what ran or ended while it was hidden
    auto_update_check();
}

// Whether `pid` runs a program of this install (its bin\, jre\, clang\...) other than Win.apk.exe
// itself and the uninstaller: the runtime, and what it started (the framework archive's java, the
// translation's clang).
bool ours_to_end(DWORD pid) {
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return false;
    wchar_t buf[MAX_PATH * 4];
    DWORD n = MAX_PATH * 4;
    const bool named = QueryFullProcessImageNameW(p, 0, buf, &n) != 0;
    CloseHandle(p);
    if (!named) return false;
    const fs::path image(std::wstring(buf, n));
    const std::wstring file = image.filename().wstring();
    if (_wcsicmp(file.c_str(), exe_path().filename().c_str()) == 0 || _wcsnicmp(file.c_str(), L"unins", 5) == 0)
        return false;
    const std::wstring dir = root().wstring() + L"\\";
    return image.wstring().size() > dir.size() && _wcsnicmp(image.c_str(), dir.c_str(), dir.size()) == 0;
}

// The processes of this install, in this Windows session, with a window on screen: an app another app
// started (an app's process starts another's, as a share or a link does), which the launcher has no run
// of.
std::set<DWORD> windowed_processes() {
    std::set<DWORD> pids;
    EnumWindows(
        [](HWND w, LPARAM lp) -> BOOL {
            DWORD pid = 0;
            if (IsWindowVisible(w) && GetWindow(w, GW_OWNER) == nullptr && GetWindowThreadProcessId(w, &pid) &&
                pid != GetCurrentProcessId()) {
                reinterpret_cast<std::set<DWORD>*>(lp)->insert(pid);
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&pids));
    for (auto it = pids.begin(); it != pids.end();) it = ours_to_end(*it) ? std::next(it) : pids.erase(it);
    return pids;
}

// When Win.apk ends, what it started in the background ends with it: the arm64 translation, the
// framework archive's dump, the system broker and the Google account's token service. Nothing of
// Win.apk goes on with nothing on screen, so closing it is how the user stops it all (a phone stops
// such work with the app's own Force stop). Each of these is cut off safely: a translation or a dump
// goes on where it was next time, and the broker writes its files whole (a temporary file, then a
// rename). A process with a window on screen is left alone (windowed_processes).
void end_background_work() {
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    const std::set<DWORD> windowed = windowed_processes();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe{sizeof(pe)};
    for (BOOL more = Process32FirstW(snap, &pe); more; more = Process32NextW(snap, &pe)) {
        DWORD theirs = 0;
        if (pe.th32ProcessID == GetCurrentProcessId() || windowed.count(pe.th32ProcessID) ||
            !ProcessIdToSessionId(pe.th32ProcessID, &theirs) || theirs != session || !ours_to_end(pe.th32ProcessID))
            continue;
        if (HANDLE p = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID)) {
            TerminateProcess(p, 1);
            CloseHandle(p);
        }
    }
    CloseHandle(snap);
}

// What keeps the process when the library is not shown, besides an app's window it did not start
// (windowed_processes): an install window, a splash, a file being read before its install, an app it started (to
// report how it ended), the log or Threads window, a maintenance window it opened (until kMaintenanceEnded, and only
// when the user did not close the library: maintenance.h, wait_for_window; otherwise the process ends now and
// end_background_work ends the window too), and a download the user started: Win.apk's update (whose installer then
// runs, as if the library were still open) and the Store's own app (which then installs), as an install goes on with
// the library closed.
bool work_keeps_process() {
    return g_install_wnd || g_log_wnd || g_debug_wnd || !g_splashes.empty() || any_runs() || any_pending() ||
           probes_pending() || mw::wait_for_window(g_maintenance != nullptr, g_closed_by_user) ||
           g_update.downloading() || g_store_setup.busy();
}

// When the library is not shown, the process lives only for what it is doing (work_keeps_process), and for
// an app's window it did not start (looked at again every few seconds), so that the broker such an app uses is
// not ended under it (end_background_work). It is called from windows' WM_DESTROY, some of them owned by the
// library: destroying the library there would destroy its owned windows again from inside their own
// destruction. So the end is posted, and the library's window ends from its own message, once.
void maybe_quit() {
    if (g_quitting || !g_main || IsWindowVisible(g_main)) return;
    if (work_keeps_process()) return;
    if (!windowed_processes().empty()) {
        SetTimer(g_main, kQuitTimer, 3000, nullptr);
        return;
    }
    PostMessageW(g_main, kQuitIfIdle, 0, 0);
}

// Every GDI+ object the launcher keeps, released while GDI+ still runs: a static's destructor that ran after
// GdiplusShutdown would call into a GDI+ that is gone and crash the exit. Every way out of wWinMain that started
// GDI+ comes through here, so a new kept picture or font is added once, below.
void release_graphics() {
    g_icons.clear();
    g_brand.reset();
    g_account_photo.reset();
    g_pending_looks.clear();
    for (Job& j : g_jobs) j.icon.reset();
    for (auto& [name, splash] : g_splashes) splash->icon.reset();
    g_f = Fonts{};
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    g_inst = inst;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    OleInitialize(nullptr);
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    // Earlier builds' scheduled task for background updates, should it run before the launcher removed it.
    if (argc >= 2 && wcscmp(argv[1], L"maintenance") == 0) {
        const int code = maintenance_main();
        LocalFree(argv);
        OleUninitialize();
        return code;
    }

    // The uninstaller's: the apps' shortcuts go with the program they start.
    if (argc >= 2 && wcscmp(argv[1], L"remove-shortcuts") == 0) {
        const int code = remove_shortcuts_main();
        LocalFree(argv);
        OleUninitialize();
        return code;
    }

    // The install helper, for apps that install apps.
    if (argc >= 2 && (wcscmp(argv[1], L"install") == 0 || wcscmp(argv[1], L"uninstall") == 0)) {
        ULONG_PTR gdiplus = 0;  // its question is the launcher's dialog
        Gdiplus::GdiplusStartupInput gsi;
        Gdiplus::GdiplusStartup(&gdiplus, &gsi, nullptr);
        pick_language();
        pick_theme();
        g_dpi = static_cast<float>(GetDpiForSystem());
        make_fonts();
        const int code = helper_main(argc, argv);
        LocalFree(argv);
        release_graphics();
        Gdiplus::GdiplusShutdown(gdiplus);
        OleUninitialize();
        return code;
    }

    // What this start is for: a shortcut's app (--run), files to install, or the library.
    fs::path run_apk;
    std::vector<fs::path> files;
    const bool permission = argc >= 2 && (wcscmp(argv[1], L"runtime-permission") == 0 ||
                                          wcscmp(argv[1], L"special-access") == 0 ||
                                          wcscmp(argv[1], L"app-details") == 0 ||
                                          wcscmp(argv[1], L"app-not-responding") == 0);
    if (argc >= 3 && wcscmp(argv[1], L"--run") == 0) {
        run_apk = argv[2];
    } else if (!permission) {
        for (int i = 1; i < argc; ++i)
            if (is_app_file(argv[i])) files.emplace_back(argv[i]);
        if (forward_to_running(files.empty(), files)) {
            LocalFree(argv);
            OleUninitialize();
            return 0;
        }
    }

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    ULONG_PTR gdiplus = 0;
    Gdiplus::GdiplusStartupInput gsi;
    Gdiplus::GdiplusStartup(&gdiplus, &gsi, nullptr);
    pick_language();
    pick_theme();

    // The permission dialogs, for apps that ask for permissions.
    if (permission) {
        g_dpi = static_cast<float>(GetDpiForSystem());
        make_fonts();
        const int code = permission_main(argc, argv);
        LocalFree(argv);
        release_graphics();
        Gdiplus::GdiplusShutdown(gdiplus);
        OleUninitialize();
        return code;
    }

    // Before anything reads the library: an update cut off the last time (a crash, the power) is undone.
    recover_interrupted_installs();
    if (run_apk.empty()) {
        integrate_with_windows();
        refresh_app_shortcuts();
        write_registry();  // apps installed before the registry existed, or by an older launcher
        if (HANDLE t = CreateThread(nullptr, 0, backfill_thread, nullptr, 0, nullptr)) CloseHandle(t);
        remove_maintenance_task();  // earlier builds' scheduled task: background updates are the launcher's now
    }
    seal_gms_tokens();  // an older install's plain Google token, sealed before an app opens
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
    sc.hInstance = inst;
    sc.hIcon = wc.hIcon;
    sc.hCursor = wc.hCursor;
    sc.lpfnWndProc = install_proc;
    sc.lpszClassName = L"WinApkInstall";
    RegisterClassExW(&sc);
    sc.lpfnWndProc = log_proc;
    sc.lpszClassName = L"WinApkLogs";
    RegisterClassExW(&sc);
    sc.lpfnWndProc = debug_proc;
    sc.lpszClassName = L"WinApkThreads";
    RegisterClassExW(&sc);
    sc.lpfnWndProc = splash_proc;
    sc.lpszClassName = L"WinApkSplash";
    RegisterClassExW(&sc);
    // The size from last time, within the screen; 980 x 720 the first time. The window is made
    // hidden: an install from Explorer or a shortcut's start shows it only when asked for.
    const int dpi = static_cast<int>(GetDpiForSystem());
    const int w = (std::min)(GetSystemMetrics(SM_CXSCREEN),
                             MulDiv((std::max)(680, static_cast<int>(setting(L"WindowWidth", 980))), dpi, 96));
    const int h = (std::min)(GetSystemMetrics(SM_CYSCREEN),
                             MulDiv((std::max)(520, static_cast<int>(setting(L"WindowHeight", 720))), dpi, 96));
    CreateWindowExW(layout_style(), wc.lpszClassName, L"Win.apk", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                    CW_USEDEFAULT, CW_USEDEFAULT, w, h, nullptr, nullptr, inst, nullptr);

    if (!run_apk.empty()) {
        // A shortcut: the app, with its splash; nothing else unless it fails.
        const std::wstring name = run_apk.stem().wstring();
        const App* app = find_app(name);
        launch(app ? *app : App{name, label_of(name), run_apk});
    } else if (!files.empty()) {
        // Opened from Explorer, or dropped on the program: only the install window.
        install_files(files);
    } else {
        show_library(show);
    }
    // At start, after the first thing asked for is under way: a window, if the last one is kIntervalSeconds old.
    maintenance_check();
    LocalFree(argv);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    end_background_work();
    release_graphics();
    Gdiplus::GdiplusShutdown(gdiplus);
    OleUninitialize();
    return 0;
}
