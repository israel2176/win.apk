// debug_agent.h -- debug mode's thread agent: the runtime's side of the launcher's Threads window.
//
// With debug mode on (Settings > Troubleshooting > Debug mode, HKCU\Software\Win.apk DebugMode;
// WINAPK_DEBUG=1 or 0 wins over it), a process that runs an app, or the system broker, answers on
// \\.\pipe\winapk-debug-<pid> what the launcher cannot see from outside: which of its Windows threads
// are Java threads and their names and states, which the app's native code started and in what
// function, and one thread's Java frames, the monitor it waits for and what its native stack shows
// (include/winapk/debug_protocol.h). The launcher lists the threads themselves from Windows, so a
// process without an agent, or one that no longer answers, still shows every thread it has.
//
// Off, nothing is started: no pipe, no JVMTI environment, no thread.

#pragma once

#include <jni.h>

namespace winapk::host {

// Whether this process runs in debug mode.
bool debug_mode_on();

// Right after JNI_CreateJavaVM, on the thread that created the VM, when debug_mode_on(): the Java
// threads that start from here on are known by their Windows thread; the few the VM already has
// (Finalizer, Reference Handler, ...) are matched by name.
void start_debug_agent(JavaVM* vm);

}  // namespace winapk::host
