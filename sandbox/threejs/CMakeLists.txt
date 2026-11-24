# Assume this project is invoked by emcmake.
cmake_minimum_required(VERSION 3.5.1)

set(BUILD_TARGET "lightusd")

if (NOT EMSCRIPTEN)
  message(FATAL "Must be compiled with emscripten")
endif()

project(${BUILD_TARGET} CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# cmake modules
list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/../../cmake)
#list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/../../cmake/sanitizers)
#find_package(Sanitizers) # Address sanitizer (-DSANITIZE_ADDRESS=ON)

# [lightusd]
add_subdirectory(../../  lightusd)

list(
  APPEND
  SOURCES
  binding.cc
)


add_executable(${BUILD_TARGET} ${SOURCES})
add_sanitizers(${BUILD_TARGET})

target_compile_options(${BUILD_TARGET} PRIVATE ${EXT_COMPILE_OPTIONS})

# lightusd dir
target_include_directories(${BUILD_TARGET}
                           PRIVATE "${PROJECT_SOURCE_DIR}/../../src/")


target_link_libraries(${BUILD_TARGET} PRIVATE lightusd::legacy_static
                                              ${EXT_LIBRARIES} PUBLIC ${CMAKE_DL_LIBS})

source_group("Source Files" FILES ${SOURCES})

if (EMSCRIPTEN)
    set_target_properties(
        ${BUILD_TARGET}
        PROPERTIES OUTPUT_NAME lightusd
                   SUFFIX ".js"
                   RUNTIME_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/simple)
    #set(CMAKE_EXECUTABLE_SUFFIX ".html")
endif()

set_target_properties(${BUILD_TARGET} PROPERTIES LINK_FLAGS "-s INITIAL_MEMORY=128MB -s TOTAL_MEMORY=512MB -sTOTAL_STACK=128MB -sASSERTIONS -s ALLOW_MEMORY_GROWTH=1 -s WASM=1 -sMODULARIZE -sEXPORT_ES6=1 --bind")

# ENVIRONMENT=web
# SINGLE_FILE=1
