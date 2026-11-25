#pragma once
#include <windows.h>
#define RTLD_NOW 2
#define RTLD_LOCAL 0
#define RTLD_NODELETE 0x1000
#define RTLD_DEFAULT ((void*)0)
static inline void* dlopen(const char* name, int) { return (void*)LoadLibraryA(name); }
static inline void* dlsym(void* h, const char* sym) {
    return (void*)GetProcAddress(h ? (HMODULE)h : GetModuleHandleA(NULL), sym);
}
static inline int dlclose(void* h) { return FreeLibrary((HMODULE)h) ? 0 : -1; }
static inline const char* dlerror(void) { return "dlopen failed"; }
