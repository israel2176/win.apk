# Run in the fetched dynarmic's source folder (FetchContent's PATCH_COMMAND):
# one change to it, made by string replacement so that it applies whatever
# the checkout's line endings, and again on a tree that already has it.
#
# Win.apk runs dynarmic with Unsafe_InaccurateNaN (arm64_jit.cpp), which is
# meant to change only which NaN a result is. For FRECPS and FRSQRTS (scalar
# and vector) it also dropped their special case: 0 times infinity is 2.0
# (1.5), not NaN, which keeps a NEON reciprocal or 1/sqrt estimate of 0 at
# infinity. Those four fast paths are turned off; dynarmic's exact path (an
# FMA and a check) runs instead.
set(fast "&& ctx.HasOptimization(OptimizationFlag::Unsafe_InaccurateNaN)) {")
set(fp "src/dynarmic/backend/x64/emit_x64_floating_point.cpp")
set(vfp "src/dynarmic/backend/x64/emit_x64_vector_floating_point.cpp")
set(off "&& false /* Win.apk: the step has special cases (0 * inf) besides NaNs */) {")
foreach(spec
    "${fp}@if (code.HasHostFeature(HostFeature::FMA) "
    "${fp}@if (code.HasHostFeature(HostFeature::FMA | HostFeature::AVX) "
    "${vfp}@if (code.HasHostFeature(HostFeature::FMA | HostFeature::AVX) ")
  string(REPLACE "@" ";" parts "${spec}")
  list(GET parts 0 file)
  list(GET parts 1 head)
  file(READ "${file}" text)
  string(REPLACE "${head}${fast}" "${head}${off}" text "${text}")
  file(WRITE "${file}" "${text}")
endforeach()
# Two in each file: the scalar and vector RecipStepFused and RSqrtStepFused.
foreach(file "${fp}" "${vfp}")
  file(READ "${file}" text)
  string(REGEX MATCHALL "Win\\.apk: the step has special cases" found "${text}")
  list(LENGTH found n)
  if(NOT n EQUAL 2)
    message(FATAL_ERROR "dynarmic_patch.cmake: ${file} changed upstream (${n} of 2 places found)")
  endif()
endforeach()
