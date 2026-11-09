# SPDX-License-Identifier: Apache-2.0
# Compile all first-party core modules as strict C11, including generated data.
file(GLOB core_sources "${SOURCE_ROOT}/src/next/c/*.c")
list(APPEND core_sources "${SOURCE_ROOT}/src/unicode-xid-c.c"
                         "${SOURCE_ROOT}/tests/next/test_array_parser_c.c"
                         "${SOURCE_ROOT}/src/imageio/exr-codec.c"
                         "${SOURCE_ROOT}/tests/next/test_c11_leaves.c"
                         "${SOURCE_ROOT}/tests/next/test_exr_codec.c")
foreach(source IN LISTS core_sources)
  execute_process(
    COMMAND "${C_COMPILER}" -std=c11 -Wall -Wextra -Wpedantic -Werror
            "-I${SOURCE_ROOT}/src" -fsyntax-only "${source}"
    RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
  if(NOT status EQUAL 0)
    message(FATAL_ERROR "Strict C11 check failed for ${source}:\n${output}${error}")
  endif()
endforeach()
