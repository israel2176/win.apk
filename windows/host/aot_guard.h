// aot_guard.h -- the crash guard of the arm64 translation: an app whose
// process crashes twice in a row while translated arm64 code is loaded runs
// on the JIT from the next start.
//
// A fault inside a translated DLL already marks that library's translation
// bad (arm64_aot.cpp, report_aot_fault). What that cannot see is a
// mistranslation that corrupts state and crashes elsewhere: in the JIT, the
// guest's own code, a host function, the JVM. Such a crash is never
// attributed to the translation, and the app would crash at every start. So
// the guard counts whole-process crashes instead:
//
//   - A crash is the process ending through the crash path (crash_report.h,
//     set_crash_hook): a fault nobody handled, abort(), HotSpot's fatal error
//     handler. A normal exit, the user closing the window, a kill (the
//     launcher's Stop and Remove, Task Manager) and a Java exception are not.
//   - On: a crash counts when this process loaded at least one translation
//     (arm64_aot.h, aot_set_loaded_hook). A start after such a run that ended
//     without a crash (whatever ended it) sets the count back to 0. At 2 in a
//     row, the next start runs the app with translation off, as WINAPK_AOT=0
//     does, says so in the app's run log, and stays so.
//   - Off: every crash counts (there is no translation in the process), a run
//     without one sets the count back to 0. Two in a row on the JIT mean the
//     app crashes without the translation as well: the next start turns it on
//     again and the guard stands down for that build of the app (an app that
//     is simply broken would otherwise swap between the two for ever).
//   - The state is apps\<name>\aot-guard.txt, written whole or not at all
//     (aot_files.h, aot_write_file). A new kAotVersion or another build of the
//     app or its libraries (the id the caller passes) starts it over; deleting
//     it tries the translation again.
//
// windows/test/aot_guard_test.cpp drives it through the real crash path.

#pragma once

#include <cstdint>
#include <filesystem>

namespace winapk::host {

// At an app's start, before its libraries load: reads the state, turns
// translation off for this process if it says so (winapk::arm64::aot_disable),
// writes the lines of the run log, and installs the crash and load hooks.
// `id` names this build of the app and of the translator (aot_guard_id).
// Returns false when this start runs the app on the JIT.
bool aot_guard_start(const std::filesystem::path& app_dir, uint64_t id);

// The id aot_guard_start compares: kAotVersion and the APK's and libraries'
// sizes and dates (aot_lib_id).
uint64_t aot_guard_id(const std::filesystem::path& apk, const std::filesystem::path& libs);

// The state file, for the log lines and the documents.
std::filesystem::path aot_guard_file(const std::filesystem::path& app_dir);

}  // namespace winapk::host
