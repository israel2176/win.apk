#!/usr/bin/env python3
"""gen_gl_stubs.py -- writes host/gl/gl_stubs.inc, host/gl/guest_gl.inc and
host/gl/guest_gl_extensions.inc from the Khronos headers and ANGLE's
gl2ext_angle.h.

The runtime calls GL and EGL by name (upstream's GLES20 bindings call gl*
directly), but must start on machines without ANGLE. So instead of linking
ANGLE's import libraries, every GLES 1.1-3.2, GLES 2 extension and EGL 1.5
entry point, and every function of ANGLE's and Chromium's own extensions
(third_party/angle/GLES2/gl2ext_angle.h), is defined in gl_stubs.inc as a
forwarder that looks the real function up in libGLESv2.dll or libEGL.dll on
its first call (gl_loader.h), and returns zero when ANGLE is not there.

An app's own native code calls the same functions with the System V calling
convention. guest_gl.inc wraps each forwarder in a WINAPK_GUEST_ABI function
and lists them by name for the guest linker (guest_gl.cpp). The functions in
GUEST_OVERRIDES are written by hand there instead. guest_gl_extensions.inc is
the table of extensions a guest may be told of (EXTENSION_SOURCES).

    windows/tools/gen_gl_stubs.py   (rewrites the three files)
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
KHR = os.path.join(HERE, "..", "third_party", "khronos")
# ANGLE's header, by an absolute path (os.path.join(KHR, ...) keeps it as it is).
ANGLE_EXT = os.path.join(HERE, "..", "third_party", "angle", "GLES2", "gl2ext_angle.h")
OUT = os.path.join(HERE, "..", "host", "gl", "gl_stubs.inc")
GUEST_OUT = os.path.join(HERE, "..", "host", "gl", "guest_gl.inc")
EXT_OUT = os.path.join(HERE, "..", "host", "gl", "guest_gl_extensions.inc")

# Earlier headers win a name both declare.
SOURCES = [
    ("GLES2/gl2.h", "GL_APICALL", "GL_APIENTRY", "gl"),
    ("GLES3/gl3.h", "GL_APICALL", "GL_APIENTRY", "gl"),
    ("GLES3/gl31.h", "GL_APICALL", "GL_APIENTRY", "gl"),
    ("GLES3/gl32.h", "GL_APICALL", "GL_APIENTRY", "gl"),
    ("GLES2/gl2ext.h", "GL_APICALL", "GL_APIENTRY", "gl"),
    ("GLES/gl.h", "GL_API", "GL_APIENTRY", "gl"),
    ("EGL/egl.h", "EGLAPI", "EGLAPIENTRY", "egl"),
    # The extensions ANGLE adds beyond the Khronos headers (GL_ANGLE_*,
    # GL_CHROMIUM_*), forwarded as the other runtime forwards them
    # (compat/bionic/gen_gl_thunks.py): Skia's GrGLInterface (Flutter) wants
    # glBindUniformLocationCHROMIUM for GL_CHROMIUM_bind_uniform_location,
    # WebRender glProvokingVertexANGLE for GL_ANGLE_provoking_vertex.
    (ANGLE_EXT, "GL_APICALL", "GL_APIENTRY", "gl"),
]

# Types of a function pointer one side would call. A guest's callback is
# System V code that ANGLE would call with the Microsoft convention, and a
# pointer ANGLE hands back is Microsoft code a guest would call as System V,
# so a function taking or returning one is not passed through as it is: it is
# written by hand (GUEST_OVERRIDES) or a guest does not get it.
CALLBACK_TYPES = ("GLDEBUGPROC", "GLDEBUGPROCKHR", "GLVULKANPROCNV", "GLSETBLOBPROCANGLE", "GLGETBLOBPROCANGLE")

# Guest entry points with their own code in guest_gl.cpp: the display the
# runtime shares, window surfaces (pbuffers presented on swap), and the calls
# that hand ANGLE a guest function pointer or hand the guest a host one.
GUEST_OVERRIDES = {
    "eglGetDisplay", "eglGetPlatformDisplay", "eglInitialize", "eglTerminate", "eglChooseConfig",
    "eglCreateWindowSurface", "eglCreatePlatformWindowSurface", "eglDestroySurface", "eglSwapBuffers", "eglQuerySurface",
    "eglMakeCurrent", "eglGetCurrentSurface", "eglSurfaceAttrib", "eglBindTexImage", "eglReleaseTexImage",
    "eglCopyBuffers",
    "eglGetProcAddress", "glDebugMessageCallback", "glDebugMessageCallbackKHR",
    # The extension lists, which name only what the guest can call (EXTENSION_SOURCES).
    "glGetString", "glGetStringi", "glGetIntegerv", "eglQueryString",
}

# Extension entry points guest_gl.cpp writes by hand beyond GUEST_OVERRIDES (its
# symbols() table), which a guest can get as well. Those that are Win.apk's own
# code, with nothing of ANGLE's behind them:
GUEST_OWN = {
    "eglGetPlatformDisplayEXT", "eglCreatePlatformWindowSurfaceEXT", "eglSwapBuffersWithDamageKHR",
    "eglSwapBuffersWithDamageEXT", "eglPresentationTimeANDROID", "eglSetDamageRegionKHR",
    "eglDupNativeFenceFDANDROID",
}
# ... and those that forward to ANGLE's function of the same name.
GUEST_FORWARDED = {
    "eglCreateImageKHR", "eglDestroyImageKHR", "eglCreateSyncKHR",
    "eglDestroySyncKHR", "eglClientWaitSyncKHR", "eglWaitSyncKHR", "eglGetSyncAttribKHR",
}
GUEST_EXTRAS = GUEST_OWN | GUEST_FORWARDED

# Direct-state-access forms an extension's block declares but its spec adds
# only "if EXT_direct_state_access is supported" (EXT_texture_storage's
# glTextureStorage*DEXT, OVR_multiview's glNamedFramebufferTextureMultiviewOVR,
# the memory-object ones). A GLES driver without that extension (ANGLE) has the
# extension and not these, so they do not count against it; an app asks for
# them only after finding GL_EXT_direct_state_access, which is checked itself.
# Extensions ANGLE reports that none of the headers declares, and that add no
# function (shader built-ins, formats, query targets; their specs, or ANGLE's
# extension docs for its own names): a guest can use all of each.
FUNCTIONLESS = {
    "GL_EXT_frag_depth", "GL_EXT_EGL_image_external_wrap_modes", "GL_NV_EGL_stream_consumer_external",
    "GL_ANGLE_base_vertex_base_instance_shader_builtin", "GL_ANGLE_memory_size",
    "GL_ANGLE_program_binary_readiness_query", "GL_CHROMIUM_sync_query",
    "GL_OES_compressed_EAC_R11_signed_texture", "GL_OES_compressed_EAC_R11_unsigned_texture",
    "GL_OES_compressed_EAC_RG11_signed_texture", "GL_OES_compressed_EAC_RG11_unsigned_texture",
    "GL_OES_compressed_ETC2_RGB8_texture", "GL_OES_compressed_ETC2_RGBA8_texture",
    "GL_OES_compressed_ETC2_punchthroughA_RGBA8_texture", "GL_OES_compressed_ETC2_punchthroughA_sRGB8_alpha_texture",
    "GL_OES_compressed_ETC2_sRGB8_alpha8_texture", "GL_OES_compressed_ETC2_sRGB8_texture",
}

DSA_ONLY = re.compile(r"gl(Named\w+|TextureStorage\w*EXT|TextureAttachMemoryNV|TexturePageCommitmentMemNV)")

# Where an extension's name and its entry points are declared. On Android a
# driver lists only extensions it implements, and an app (Skia's GrGLInterface,
# so Flutter) that finds one listed requires its functions, from
# eglGetProcAddress, and gives up on GL when one is missing. So an extension
# the driver reports is passed on to the guest only when it is declared here,
# the guest has a wrapper for every one of its functions (they are in SOURCES
# and take no callback), and, at run time, ANGLE resolves every one of them
# (guest_gl_extensions.cpp): ANGLE may report an extension whose
# functions its build lacks, and gl2ext_angle.h is newer than the shipped DLLs.
EXTENSION_SOURCES = [
    ("GLES2/gl2ext.h", "GL_APICALL", "GL_APIENTRY", "GL"),
    ("GLES/glext.h", "GL_API", "GL_APIENTRY", "GL"),
    (ANGLE_EXT, "GL_APICALL", "GL_APIENTRY", "GL"),
    ("EGL/eglext.h", "EGLAPI", "EGLAPIENTRY", "EGL"),
]


def prototypes(path, api, entry):
    text = open(os.path.join(KHR, path)).read()
    pat = re.compile(api + r"\s+(.+?)\s*" + entry + r"\s+(\w+)\s*\(([^;]*?)\)\s*;", re.S)
    for ret, name, params in pat.findall(text):
        yield " ".join(ret.split()), name, named(" ".join(params.split()))


# A parameter given only by its type (gl2ext_angle.h's `glLogicOpANGLE (GLenum)`):
# one type name, maybe const, maybe pointers. (The pattern is the other runtime's, from
# compat/bionic/gen_gl_thunks.py, Copyright (C) 2026 its authors, Apache-2.0.)
UNNAMED = re.compile(r"(?:const\s+)?\w+(?:\s*\*\s*(?:const\b)?)*\s*")


def named(params):
    """`params` with a name for each parameter the header leaves unnamed."""
    if params in ("", "void"):
        return params
    out = []
    for i, p in enumerate(params.split(",")):
        p = p.strip()
        out.append(f"{p} a{i}" if UNNAMED.fullmatch(p) else p)
    return ", ".join(out)


def arg_names(params):
    if params in ("", "void"):
        return []
    names = []
    for p in params.split(","):
        m = re.search(r"(\w+)\s*(\[\s*\w*\s*\])?\s*$", p.strip())
        names.append(m.group(1))
    return names


def extensions(path, api, entry, prefix):
    """Each extension the header declares, with the functions it declares: the
    text from one `#ifndef NAME` / `#define NAME 1` to the next (not to its
    `#endif /* NAME */`: gl2ext_angle.h leaves some unnamed or misnamed)."""
    text = open(os.path.join(KHR, path)).read()
    start = re.compile(r"^#ifndef (" + prefix + r"_\w+)[ \t]*\n#define \1 1[ \t]*$", re.M)
    proto = re.compile(api + r"\s+.+?\s*" + entry + r"\s+(\w+)\s*\(", re.S)
    marks = list(start.finditer(text))
    for i, m in enumerate(marks):
        end = marks[i + 1].start() if i + 1 < len(marks) else len(text)
        yield m.group(1), proto.findall(text[m.end():end])


def takes_callback(ret, params):
    return any(re.search(r"\b" + t + r"\b", ret + " " + params) for t in CALLBACK_TYPES)


def main():
    seen = {}
    for path, api, entry, lib in SOURCES:
        for ret, name, params in prototypes(path, api, entry):
            # ANGLE's callback-taking functions use types only its header
            # defines, and no guest could use them (CALLBACK_TYPES).
            if path == ANGLE_EXT and takes_callback(ret, params):
                continue
            seen.setdefault(name, (ret, params, entry, lib, path))
    out = [
        "// GENERATED by windows/tools/gen_gl_stubs.py from third_party/khronos and third_party/angle. Do not edit.",
        "// Every GLES 2.0-3.2 and EGL 1.5 function and ANGLE's extensions', forwarded to ANGLE (gl_loader.h).",
        "",
    ]
    for name in sorted(seen):
        ret, params, entry, lib, _ = seen[name]
        args = ", ".join(arg_names(params))
        sig = params if params else "void"
        fail = "" if ret == "void" else " {}"
        call = ("" if ret == "void" else "return ") + "fn(" + args + ");"
        out += [
            f'extern "C" {ret} {entry} {name}({sig}) {{',
            f"    using Fn = {ret}({entry}*)({sig});",
            f'    static const Fn fn = reinterpret_cast<Fn>(winapk::host::gl_proc("{name}", winapk::host::GlLib::{"Egl" if lib == "egl" else "Gles"}));',
            f"    if (!fn) return{fail};",
            f"    {call}",
            "}",
            "",
        ]
    with open(OUT, "w", newline="\n") as f:
        f.write("\n".join(out))
    print(f"[gen_gl_stubs] {len(seen)} functions -> {os.path.relpath(OUT)}")

    guest = [
        "// GENERATED by windows/tools/gen_gl_stubs.py from third_party/khronos and third_party/angle. Do not edit.",
        "// The GL and EGL functions an app's native code calls, System V side (guest_gl.cpp).",
        "",
    ]
    names = [n for n in sorted(seen) if n not in GUEST_OVERRIDES and not takes_callback(seen[n][0], seen[n][1])]
    # GLES/gl.h cannot be included next to GLES 3's headers, and ANGLE's header
    # is not included at all, so their own functions are declared here.
    for name in names:
        ret, params, entry, lib, path = seen[name]
        if path in ("GLES/gl.h", ANGLE_EXT):
            guest += [f'extern "C" {ret} {entry} {name}({params if params else "void"});']
    guest += [""]
    for name in names:
        ret, params, entry, lib, _ = seen[name]
        args = ", ".join(arg_names(params))
        sig = params if params else "void"
        call = ("" if ret == "void" else "return ") + name + "(" + args + ");"
        guest += [f"WINAPK_GUEST_ABI {ret} guest_{name}({sig}) {{ {call} }}"]
    callable_names = set(names) | GUEST_OVERRIDES | GUEST_EXTRAS
    # What ANGLE must resolve for a guest's function to work: all but Win.apk's
    # own code (the overrides that only filter what ANGLE answers do need it).
    own = GUEST_OWN | (GUEST_OVERRIDES - {"glGetString", "glGetStringi", "glGetIntegerv", "eglQueryString"})
    ext = [
        "// GENERATED by windows/tools/gen_gl_stubs.py from third_party/khronos and third_party/angle. Do not edit.",
        "// The extensions a guest may be told of (EXTENSION_SOURCES), each with the functions",
        "// ANGLE must resolve for it (guest_gl_extensions.cpp).",
    ]
    for prefix in ("GL", "EGL"):
        ok = {}
        for path, api, entry, p in EXTENSION_SOURCES:
            if p != prefix:
                continue
            for name, funcs in extensions(path, api, entry, p):
                # The first header to declare an extension wins, as for functions.
                if name in ok or not all(f in callable_names for f in funcs):
                    continue
                ok[name] = [f for f in funcs if f not in own and not DSA_ONLY.fullmatch(f)]
        for name in FUNCTIONLESS:
            if name.startswith(prefix + "_"):
                ok.setdefault(name, [])
        ext +=["", f"#define WINAPK_GUEST_{prefix}_EXTENSIONS \\"]
        ext += [f'    {{"{n}", "{" ".join(ok[n])}"}}, \\' for n in sorted(ok)]
        ext += [""]
        print(f"[gen_gl_stubs] {len(ok)} {prefix} extensions a guest may see")
    ext += [""]
    with open(EXT_OUT, "w", newline="\n") as f:
        f.write("\n".join(ext))
    guest += ["", "#define WINAPK_GUEST_GL_SYMBOLS \\"]
    guest += [f"    {{\"{n}\", reinterpret_cast<void*>(&guest_{n})}}, \\" for n in names]
    guest += [""]
    with open(GUEST_OUT, "w", newline="\n") as f:
        f.write("\n".join(guest))
    print(f"[gen_gl_stubs] {len(names)} guest wrappers -> {os.path.relpath(GUEST_OUT)}")


if __name__ == "__main__":
    sys.exit(main())
