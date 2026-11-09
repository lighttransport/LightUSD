# Checks the output of `ld3d12view --turntable N --output-dir DIR`:
#   cmake -DDIR=<dir> -DEXPECT=<N> -P check_turntable.cmake
# Exactly N frame-*.png files must exist in one turntable-* folder, and the first
# and the middle frame must differ (the camera really rotated).
file(GLOB frames "${DIR}/turntable-*/frame-*.png")
list(LENGTH frames count)
if(NOT count EQUAL EXPECT)
  message(FATAL_ERROR "expected ${EXPECT} turntable frames in ${DIR}, found ${count}")
endif()
list(SORT frames)
list(GET frames 0 first)
math(EXPR mid_index "${EXPECT} / 2")
list(GET frames ${mid_index} middle)
foreach(f IN LISTS frames)
  file(SIZE "${f}" bytes)
  if(bytes EQUAL 0)
    message(FATAL_ERROR "empty frame ${f}")
  endif()
endforeach()
execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files "${first}" "${middle}"
                RESULT_VARIABLE same)
if(same EQUAL 0)
  message(FATAL_ERROR "first and middle frames are identical: the camera did not turn")
endif()
message(STATUS "turntable ok: ${count} frames")
