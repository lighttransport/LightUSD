# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.16)
project(lightusd_meson_bridge LANGUAGES C CXX)
# Reuse the canonical target/source definitions instead of maintaining a
# second source list that can drift back to the legacy implementation.
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../.." lightusd)

# File API link probes capture CMake's resolved transitive libraries and public
# include/definition requirements. They are inspected but never built.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/link-probe.cc" "int main() { return 0; }\n")
foreach(product IN ITEMS core tydra legacy shared legacy_shared mcp)
  if(product STREQUAL "core")
    set(library lightusd_core)
  elseif(product STREQUAL "tydra")
    set(library tydra)
  elseif(product STREQUAL "legacy")
    set(library lightusd_legacy_static)
  elseif(product STREQUAL "shared")
    set(library lightusd_c_shared)
  elseif(product STREQUAL "legacy_shared")
    set(library lightusd_legacy)
  else()
    set(library lightusd_mcp)
  endif()
  if(TARGET ${library})
    add_executable(lightusd_meson_${product}_probe EXCLUDE_FROM_ALL
      "${CMAKE_CURRENT_BINARY_DIR}/link-probe.cc")
    target_link_libraries(lightusd_meson_${product}_probe PRIVATE ${library})
  endif()
endforeach()
