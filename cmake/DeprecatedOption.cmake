# SPDX-License-Identifier: Apache-2.0
# Copyright 2024-Present Light Transport Entertainment Inc.
#
# lightusd_deprecated_option(<old> <new>)
#
# Carries a value given under a renamed CMake option/cache variable over to its
# new name, with a deprecation message. Call it before the new option() is
# declared. The old cache entry is removed so a later change of the new name
# is not overwritten by the stale old value.
#
# Used for the 1.1 rename of the `next` options (LIGHTUSD_NEXT_* ->
# LIGHTUSD_*, TYDRA_NEXT_* -> LIGHTUSD_TYDRA_*). Remove after one release.
function(lightusd_deprecated_option old new)
  if(DEFINED ${old})
    message(DEPRECATION "${old} is deprecated; use ${new} (value '${${old}}' carried over).")
    get_property(_lusd_type CACHE ${old} PROPERTY TYPE)
    if(NOT _lusd_type OR _lusd_type STREQUAL "UNINITIALIZED")
      set(_lusd_type STRING)
    endif()
    get_property(_lusd_doc CACHE ${old} PROPERTY HELPSTRING)
    set(${new} "${${old}}" CACHE ${_lusd_type} "${_lusd_doc}" FORCE)
    unset(${old} CACHE)
    unset(${old} PARENT_SCOPE)
  endif()
endfunction()

# The full rename table of the `next` -> core options.
macro(lightusd_apply_deprecated_core_options)
  lightusd_deprecated_option(LIGHTUSD_NEXT_BUILD_TESTS LIGHTUSD_BUILD_TESTS)
  lightusd_deprecated_option(LIGHTUSD_NEXT_BUILD_EXAMPLES LIGHTUSD_BUILD_EXAMPLES)
  lightusd_deprecated_option(LIGHTUSD_NEXT_BUILD_TOOLS LIGHTUSD_BUILD_LUSDCAT)
  lightusd_deprecated_option(LIGHTUSD_NEXT_ENABLE_THREAD LIGHTUSD_ENABLE_THREAD)
  lightusd_deprecated_option(LIGHTUSD_NEXT_USE_PCH LIGHTUSD_USE_PCH)
  lightusd_deprecated_option(LIGHTUSD_NEXT_BUILD_C_API_EXAMPLE LIGHTUSD_BUILD_C_API_EXAMPLE)
  lightusd_deprecated_option(LIGHTUSD_NEXT_DEFERRED_FILL LIGHTUSD_DEFERRED_FILL)
  lightusd_deprecated_option(LIGHTUSD_NEXT_FINE_LOCKS LIGHTUSD_FINE_LOCKS)
  lightusd_deprecated_option(LIGHTUSD_NEXT_WITH_ALLOCATOR LIGHTUSD_WITH_ALLOCATOR)
  lightusd_deprecated_option(LIGHTUSD_NEXT_ALLOCATOR LIGHTUSD_ALLOCATOR)
  lightusd_deprecated_option(LIGHTUSD_NEXT_JEMALLOC_CONF LIGHTUSD_JEMALLOC_CONF)
  lightusd_deprecated_option(LIGHTUSD_NEXT_BUILD_TSAN_TESTS LIGHTUSD_BUILD_TSAN_TESTS)
  lightusd_deprecated_option(LIGHTUSD_NEXT_BUILD_FUZZERS LIGHTUSD_BUILD_FUZZERS)
  lightusd_deprecated_option(LIGHTUSD_NEXT_BUILD_SHARED_C_API LIGHTUSD_BUILD_SHARED_C_API)
  lightusd_deprecated_option(LIGHTUSD_NEXT_BUILD_LMETALVIEW LIGHTUSD_BUILD_LMETALVIEW)
  lightusd_deprecated_option(LIGHTUSD_NEXT_BUILD_LD3D12VIEW LIGHTUSD_BUILD_LD3D12VIEW)
  lightusd_deprecated_option(LIGHTUSD_NEXT_DTOA_EXHAUSTIVE_TEST LIGHTUSD_DTOA_EXHAUSTIVE_TEST)
  lightusd_deprecated_option(LIGHTUSD_NEXT_PXR_PYTHON LIGHTUSD_PXR_PYTHON)
  lightusd_deprecated_option(TYDRA_NEXT_USE_PCH LIGHTUSD_TYDRA_USE_PCH)
  lightusd_deprecated_option(TYDRA_NEXT_BUILD_TESTS LIGHTUSD_TYDRA_BUILD_TESTS)
  lightusd_deprecated_option(LIGHTUSD_USE_NEXT_PCP_LARGE_SCENE LIGHTUSD_LEGACY_USE_CORE_PCP)
endmacro()
