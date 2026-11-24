# Stage-independent vendored QuickJS runtime for core tools and examples.
function(lightusd_add_quickjs source_root)
  if(TARGET lightusd_quickjs)
    return()
  endif()
  set(_qjs_root ${source_root})
  add_library(lightusd_quickjs STATIC
    ${_qjs_root}/cutils.c ${_qjs_root}/libregexp.c ${_qjs_root}/libunicode.c
    ${_qjs_root}/quickjs.c ${_qjs_root}/xsum.c)
  if(UNIX)
    target_compile_definitions(lightusd_quickjs PRIVATE _GNU_SOURCE)
    target_link_libraries(lightusd_quickjs PUBLIC m ${CMAKE_DL_LIBS})
  endif()
  if(NOT MSVC)
    target_compile_options(lightusd_quickjs PRIVATE -w)
  endif()
endfunction()
