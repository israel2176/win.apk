# ANGLE's gl2ext_angle.h

`GLES2/gl2ext_angle.h` and `LICENSE` (BSD-3-Clause) are unmodified copies from
ANGLE at commit `ab6a82e0ed1425316da0ba9a0b1c715a58a0b725` (2025-09-12), the
revision AOSP's `platform/external/angle` rolled to for tag
`android-16.0.0_r4` (commit `ff25cbdfe963e149e1a1ef4689e502270dfe6214`, whose
`include/GLES2/gl2ext_angle.h` is the same file). Fetched from the two
repositories directly (gitiles' `?format=TEXT`) and checked to agree:

- https://chromium.googlesource.com/angle/angle/+/ab6a82e0ed1425316da0ba9a0b1c715a58a0b725/include/GLES2/gl2ext_angle.h
  and https://android.googlesource.com/platform/external/angle/+/refs/tags/android-16.0.0_r4/include/GLES2/gl2ext_angle.h
  (SHA-256 `2c49625b9dfd9d72714315247d6002e45759dc8423d3d366c17d5a41fd5d1380`)
- https://chromium.googlesource.com/angle/angle/+/ab6a82e0ed1425316da0ba9a0b1c715a58a0b725/LICENSE
  (SHA-256 `bf4da21bd20bcfb5b60b7ecc67fa864a79be049e21d6178076887f178dd6c71a`; AOSP's
  `LICENSE` at the tag is this text followed by the Apache-2.0 terms of its
  `android/cts/` files, none of which is taken here)

Only `windows/tools/gen_gl_stubs.py` reads it, at build time (nothing of it is
shipped): it declares the entry points of the extensions ANGLE adds to
`GL_EXTENSIONS` beyond the Khronos headers (`GL_ANGLE_*`, `GL_CHROMIUM_*`), so
the generator writes their forwarders and guest wrappers. The ANGLE Win.apk
ships (`fetch_angle.sh`, Electron's) is an older commit than this header; that
is safe because a guest is told of an extension only when ANGLE reports it and
every one of its functions resolves in that ANGLE at run time
(`host/gl/guest_gl.cpp`). To update it, fetch both files at a newer ANGLE
commit (or the one a newer AOSP tag rolls to), record it here, and run
`windows/tools/gen_gl_stubs.py`.
