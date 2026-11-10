# SPDX-License-Identifier: Apache-2.0
# USD-independent collision/dynamics implementation shared by both products.
if(NOT TARGET lightusd_rb_solver)
  get_filename_component(_rb_root "${CMAKE_CURRENT_LIST_DIR}/../src/tydra" ABSOLUTE)
  add_library(lightusd_rb_solver STATIC ${_rb_root}/rb-solver.cc ${_rb_root}/rb-collision.cc)
  target_include_directories(lightusd_rb_solver PUBLIC $<BUILD_INTERFACE:${_rb_root}/..> $<INSTALL_INTERFACE:include>)
  target_compile_features(lightusd_rb_solver PUBLIC cxx_std_17)
  set_target_properties(lightusd_rb_solver PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()
