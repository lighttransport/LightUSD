# Python harness tests are independent of the native product and renderer build.
if(LIGHTUSD_BUILD_TESTS OR LIGHTUSD_BUILD_TESTS)
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    execute_process(COMMAND "${Python3_EXECUTABLE}" -c "import PIL"
                    RESULT_VARIABLE OUSDDIFF_PIL_RESULT OUTPUT_QUIET ERROR_QUIET)
    if(OUSDDIFF_PIL_RESULT EQUAL 0)
      enable_testing()
      add_test(NAME tool-ousddiff-harness
               COMMAND "${Python3_EXECUTABLE}" -m unittest discover
                       -s "${PROJECT_SOURCE_DIR}/tools/ousddiff" -v)
      set_tests_properties(tool-ousddiff-harness PROPERTIES
                           LABELS "tools;ousddiff;python;cpu" TIMEOUT 60)
      option(LIGHTUSD_OUSDDIFF_AMD_TESTS "Run tiny offscreen AMD hardware integration" OFF)
      if(LIGHTUSD_OUSDDIFF_AMD_TESTS)
        add_test(NAME tool-ousddiff-amd-hardware
                 COMMAND "${CMAKE_COMMAND}" -E env OUSDDIFF_RUN_AMD_INTEGRATION=1
                         "${Python3_EXECUTABLE}" -m unittest discover
                         -s "${PROJECT_SOURCE_DIR}/tools/ousddiff" -p test_gpu_integration.py -v)
        set_tests_properties(tool-ousddiff-amd-hardware PROPERTIES
                             LABELS "tools;ousddiff;gpu;amd" TIMEOUT 240)
      endif()
      option(LIGHTUSD_OUSDDIFF_EXTERNAL_TESTS "Run external ousddiff 1-percent CPU probe" OFF)
      if(LIGHTUSD_OUSDDIFF_EXTERNAL_TESTS)
        add_test(NAME tool-ousddiff-assets-probe
                 COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/ousddiff/runner.py"
                         run --preset usd-assets --mode probe --sample-percent 1
                         --no-full-scenes --renderers embree,cpu --optional
                         --out "${CMAKE_BINARY_DIR}/ousddiff-assets-probe")
        set_tests_properties(tool-ousddiff-assets-probe PROPERTIES
                             LABELS "tools;ousddiff;external" TIMEOUT 3600 SKIP_RETURN_CODE 77)
      endif()
    else()
      message(STATUS "ousddiff tests need Pillow in ${Python3_EXECUTABLE}; select an interpreter with -DPython3_EXECUTABLE=...")
    endif()
  endif()
endif()
