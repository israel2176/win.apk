// guest_gl_extensions.cpp -- see guest_gl_extensions.h.

#include "gl/guest_gl_extensions.h"

#include <cstdio>
#include <iterator>
#include <mutex>
#include <unordered_map>

#include "gl/gl_loader.h"

namespace winapk::host {

namespace {

#include "gl/guest_gl_extensions.inc"

struct Extension {
    const char* name;
    const char* functions;
};
// A plain array, made into a map at first use: an initializer list of ~500
// strings would compile to that much construction code.
constexpr Extension kExtensions[] = {WINAPK_GUEST_GL_EXTENSIONS WINAPK_GUEST_EGL_EXTENSIONS};

// Each extension a guest may be told of, with the functions ANGLE must resolve for it.
const std::unordered_map<std::string, const char*>& known_extensions() {
    static const std::unordered_map<std::string, const char*> m = [] {
        std::unordered_map<std::string, const char*> out;
        out.reserve(std::size(kExtensions));
        for (const Extension& e : kExtensions) out.emplace(e.name, e.functions);
        return out;
    }();
    return m;
}

// True when every function of `functions` resolves in the ANGLE loaded.
bool all_resolve(const std::string& extension, const char* functions) {
    const char* p = functions;
    while (*p) {
        while (*p == ' ') ++p;
        const char* e = p;
        while (*e && *e != ' ') ++e;
        if (e > p) {
            const std::string fn(p, e);
            const GlLib lib = fn.compare(0, 3, "egl") == 0 ? GlLib::Egl : GlLib::Gles;
            if (!gl_proc(fn.c_str(), lib)) {
                std::fprintf(stderr, "[win.apk] GL: ANGLE reports %s but has no %s; a guest is not told of it\n",
                             extension.c_str(), fn.c_str());
                return false;
            }
        }
        p = e;
    }
    return true;
}

std::string filtered_list(const char* list) {
    std::string out;
    const char* p = list;
    while (p && *p) {
        while (*p == ' ') ++p;
        const char* e = p;
        while (*e && *e != ' ') ++e;
        if (e > p) {
            const std::string name(p, e);
            if (guest_extension_allowed(name)) {
                if (!out.empty()) out += ' ';
                out += name;
            }
        }
        p = e;
    }
    return out;
}

}  // namespace

const char* guest_extension_functions(const std::string& name) {
    const auto it = known_extensions().find(name);
    return it == known_extensions().end() ? nullptr : it->second;
}

bool guest_extension_allowed(const std::string& name) {
    static std::mutex lock;
    static std::unordered_map<std::string, bool> answers;
    std::lock_guard<std::mutex> hold(lock);
    const auto it = answers.find(name);
    if (it != answers.end()) return it->second;
    const char* functions = guest_extension_functions(name);
    const bool ok = functions && all_resolve(name, functions);
    answers.emplace(name, ok);
    return ok;
}

const char* guest_extensions_filtered(const char* list) {
    static std::mutex lock;
    static std::unordered_map<std::string, std::string> cache;
    if (!list) return nullptr;
    std::lock_guard<std::mutex> hold(lock);
    auto it = cache.find(list);
    if (it == cache.end()) it = cache.emplace(list, filtered_list(list)).first;
    return it->second.c_str();
}

}  // namespace winapk::host
