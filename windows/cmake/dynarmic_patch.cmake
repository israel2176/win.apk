# Applies dynarmic_recip_steps.patch to the fetched dynarmic once (FetchContent's
# PATCH_COMMAND may run again on a tree that already has it).
#
# The patch: Win.apk runs dynarmic with Unsafe_InaccurateNaN (arm64_jit.cpp),
# which is meant to change only which NaN a result is. For FRECPS and FRSQRTS
# it also dropped their special case: 0 times infinity is 2.0 (1.5), not NaN,
# which is what keeps a NEON reciprocal or 1/sqrt of 0 at infinity. With the
# patch those two take dynarmic's exact path (an FMA and a check).
set(patch "${CMAKE_CURRENT_LIST_DIR}/dynarmic_recip_steps.patch")
execute_process(COMMAND git apply --reverse --check "${patch}" RESULT_VARIABLE applied OUTPUT_QUIET ERROR_QUIET)
if(NOT applied EQUAL 0)
  execute_process(COMMAND git apply "${patch}" RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "cannot apply ${patch} to dynarmic")
  endif()
endif()
