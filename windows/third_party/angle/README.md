# ANGLE's gl2ext_angle.h

`GLES2/gl2ext_angle.h` and `LICENSE` (BSD-3-Clause) are unmodified copies from
ANGLE at commit `ab6a82e0ed1425316da0ba9a0b1c715a58a0b725` (AOSP
android-16.0.0_r4's external/angle), the copy the other runtime keeps
(https://example.invalid/other-runtime, `compat/bionic/third_party/angle/`):

- https://chromium.googlesource.com/angle/angle/+/ab6a82e0ed1425316da0ba9a0b1c715a58a0b725/include/GLES2/gl2ext_angle.h
  (SHA-256 `2c49625b9dfd9d72714315247d6002e45759dc8423d3d366c17d5a41fd5d1380`)
- https://chromium.googlesource.com/angle/angle/+/ab6a82e0ed1425316da0ba9a0b1c715a58a0b725/LICENSE
  (SHA-256 `bf4da21bd20bcfb5b60b7ecc67fa864a79be049e21d6178076887f178dd6c71a`)

Only `windows/tools/gen_gl_stubs.py` reads it, at build time (nothing of it is
shipped): it declares the entry points of the extensions ANGLE adds to
`GL_EXTENSIONS` beyond the Khronos headers (`GL_ANGLE_*`, `GL_CHROMIUM_*`), so
the generator writes their forwarders and guest wrappers. The ANGLE Win.apk
ships (`fetch_angle.sh`, Electron's) is an older commit than this header; that
is safe because a guest is told of an extension only when ANGLE reports it and
every one of its functions resolves in that ANGLE at run time
(`host/gl/guest_gl.cpp`). After updating it, run `windows/tools/gen_gl_stubs.py`.
