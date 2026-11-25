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
    # GL_CHROMIUM_*), forwarded like the rest: Skia's GrGLInterface (Flutter) wants
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
    "eglCopyBuffers", "eglSwapInterval",
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


# ---- reading the headers ---------------------------------------------------
#
# The Khronos and ANGLE headers declare each entry point on its own as
#
#     <API macro> <return type> <ENTRY macro> <name> (<parameters>);
#
# e.g. `GL_APICALL const GLubyte *GL_APIENTRY glGetString (GLenum name);`, inside `#ifdef GL_GLEXT_PROTOTYPES`
# blocks and among `typedef ... (GL_APIENTRYP PFN...)` pointer types, which do not start with the API macro. The text
# is read as C tokens with its comments and preprocessor lines taken out, and a declaration is the tokens from the
# API macro to the next `;` that have that shape; the return type and the parameters are kept as the header spells
# them, blanks folded.

C_TOKEN = re.compile(r"[A-Za-z_]\w*|\d\w*|\.\.\.|\S")
TYPE_WORDS = {"void", "char", "short", "int", "long", "float", "double", "signed", "unsigned", "const", "volatile",
              "struct", "enum", "union"}


def code_text(text):
    """The header without its comments and preprocessor lines (each replaced by
    blanks, so offsets keep their meaning for the declarations found)."""
    out = []
    i, n = 0, len(text)
    line_start = True
    while i < n:
        c = text[i]
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", text[i:j]))
            i = j
        elif text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif line_start and c == "#":
            # A directive runs to the end of its line, and on past a backslash at the end of one.
            j = i
            while True:
                k = text.find("\n", j)
                if k < 0:
                    j = n
                    break
                if text[k - 1] == "\\":
                    j = k + 1
                    continue
                j = k
                break
            out.append(re.sub(r"[^\n]", " ", text[i:j]))
            i = j
        else:
            out.append(c)
            if c == "\n":
                line_start = True
            elif not c.isspace():
                line_start = False
            i += 1
    return "".join(out)


def fold(s):
    return " ".join(s.split())


def declarations(text, api, entry):
    """(return type, name, parameter text) of each entry point declared in
    `text`, in order; the parameter text is "" for none, `(void)`."""
    code = code_text(text)
    toks = [(m.group(), m.start(), m.end()) for m in C_TOKEN.finditer(code)]
    i = 0
    while i < len(toks):
        if toks[i][0] != api:
            i += 1
            continue
        # The declaration's tokens, to its `;`.
        j = i + 1
        while j < len(toks) and toks[j][0] != ";":
            j += 1
        decl = toks[i + 1:j]
        i = j + 1
        at = next((k for k, t in enumerate(decl) if t[0] == entry), None)
        # API ret... ENTRY name ( params ) ;
        if at is None or at == 0 or at + 3 > len(decl) or decl[at + 2][0] != "(" or decl[-1][0] != ")":
            continue
        name = decl[at + 1][0]
        if not re.fullmatch(r"[A-Za-z_]\w*", name):
            continue
        ret = fold(code[decl[0][1]:decl[at - 1][2]])
        params = fold(code[decl[at + 2][2]:decl[-1][1]])
        if params == "void":
            params = ""
        yield ret, name, params


def split_params(params):
    """The parameters one by one; commas inside parentheses (a function pointer's
    own parameters) do not split."""
    out, depth, cur = [], 0, ""
    for c in params:
        if c == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
            continue
        depth += c == "("
        depth -= c == ")"
        cur += c
    if cur.strip():
        out.append(cur.strip())
    return out


def param_name(param):
    """The name a parameter declares, or None when it declares only a type
    (`GLenum`, `const void *`, `GLuint *const`)."""
    toks = C_TOKEN.findall(param)
    if toks and toks[-1] == "]":  # an array: the name is before its bound
        toks = toks[:toks.index("[")] if "[" in toks else toks
    if len(toks) < 2:
        return None
    last = toks[-1]
    if not re.fullmatch(r"[A-Za-z_]\w*", last) or last in TYPE_WORDS:
        return None
    return last


def prototypes(path, api, entry):
    """(return type, name, parameters) of each entry point of a header, every
    parameter named: one declared by type only gets a0, a1... by its place, so
    a forwarder can pass it on."""
    text = open(os.path.join(KHR, path)).read()
    for ret, name, params in declarations(text, api, entry):
        parts = split_params(params)
        for k, p in enumerate(parts):
            if param_name(p) is None:
                parts[k] = p + ("" if p.endswith("*") else " ") + f"a{k}"
        yield ret, name, ", ".join(parts)


def arg_names(params):
    return [param_name(p) for p in split_params(params)]


def extensions(path, api, entry, prefix):
    """Each extension the header declares, with the functions it declares: the
    text from one `#ifndef NAME` / `#define NAME 1` to the next (not to its
    `#endif /* NAME */`: gl2ext_angle.h leaves some unnamed or misnamed)."""
    text = open(os.path.join(KHR, path)).read()
    start = re.compile(r"^#ifndef (" + prefix + r"_\w+)[ \t]*\n#define \1 1[ \t]*$", re.M)
    marks = list(start.finditer(text))
    for i, m in enumerate(marks):
        end = marks[i + 1].start() if i + 1 < len(marks) else len(text)
        yield m.group(1), [name for _, name, _ in declarations(text[m.end():end], api, entry)]


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
