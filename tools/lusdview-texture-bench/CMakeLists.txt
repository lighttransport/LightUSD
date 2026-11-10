# Standalone headless GPU texture processing benchmark. It intentionally does
# not link or include lusdview's renderer.
# TIFF/TEX input is part of the benchmark's corpus path. The top-level project
# enables LIGHTUSD_WITH_TIFF by default; keep that option visible to callers so
# a deliberately disabled core decoder produces a clear configuration choice.
if(NOT LIGHTUSD_WITH_TIFF)
  message(WARNING "texture GPU benchmark: TIFF/TEX input disabled by LIGHTUSD_WITH_TIFF=OFF")
endif()
include(CTest)
if(BUILD_TESTING)
  enable_testing()
endif()
find_program(LUSDVIEW_TEXTURE_GLSLANG glslangValidator)
if(NOT LUSDVIEW_TEXTURE_GLSLANG)
  message(WARNING "texture GPU benchmark disabled: glslangValidator not found")
  return()
endif()

include(CheckLanguage)
check_language(HIP)
if(CMAKE_HIP_COMPILER)
  enable_language(HIP)
  set(LUSDVIEW_TEXTURE_HAVE_HIP TRUE)
  set(LUSDVIEW_TEXTURE_HIP_ARCHITECTURES "native" CACHE STRING
      "HIP architecture(s) for the texture benchmark")
else()
  message(STATUS "texture GPU benchmark: HIP compiler not found; HIP backend disabled")
endif()
check_language(CUDA)
if(CMAKE_CUDA_COMPILER)
  enable_language(CUDA)
  set(LUSDVIEW_TEXTURE_HAVE_CUDA TRUE)
  set(LUSDVIEW_TEXTURE_CUDA_ARCHITECTURES "120" CACHE STRING
      "CUDA architecture(s) for the texture benchmark")
else()
  message(STATUS "texture GPU benchmark: CUDA compiler not found; CUDA backend disabled")
endif()

set(TEXBENCH_GEN ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${TEXBENCH_GEN})
add_custom_command(
  OUTPUT ${TEXBENCH_GEN}/resize_comp.spv.h ${TEXBENCH_GEN}/resize_hdr_comp.spv.h ${TEXBENCH_GEN}/compress_comp.spv.h ${TEXBENCH_GEN}/bc6h_comp.spv.h
  COMMAND ${LUSDVIEW_TEXTURE_GLSLANG} -V --target-env vulkan1.2
          --vn resize_comp_spv -o ${TEXBENCH_GEN}/resize_comp.spv.h
          ${CMAKE_CURRENT_SOURCE_DIR}/vk/resize.comp
  COMMAND ${LUSDVIEW_TEXTURE_GLSLANG} -V --target-env vulkan1.2
          --vn resize_hdr_comp_spv -o ${TEXBENCH_GEN}/resize_hdr_comp.spv.h
          ${CMAKE_CURRENT_SOURCE_DIR}/vk/resize_hdr.comp
  COMMAND ${LUSDVIEW_TEXTURE_GLSLANG} -V --target-env vulkan1.2
          --vn compress_comp_spv -o ${TEXBENCH_GEN}/compress_comp.spv.h
          ${CMAKE_CURRENT_SOURCE_DIR}/vk/compress.comp
  COMMAND ${LUSDVIEW_TEXTURE_GLSLANG} -V --target-env vulkan1.2
          --vn bc6h_comp_spv -o ${TEXBENCH_GEN}/bc6h_comp.spv.h
          ${CMAKE_CURRENT_SOURCE_DIR}/vk/bc6h.comp
  DEPENDS vk/resize.comp vk/resize_hdr.comp vk/compress.comp vk/bc6h.comp
  VERBATIM)
add_custom_target(lusdview_texture_gpu_shaders
  DEPENDS ${TEXBENCH_GEN}/resize_comp.spv.h ${TEXBENCH_GEN}/resize_hdr_comp.spv.h ${TEXBENCH_GEN}/compress_comp.spv.h ${TEXBENCH_GEN}/bc6h_comp.spv.h)

add_executable(lusdview_texture_gpu_bench
  main.cc texture_gpu.cc vulkan_processor.cc
  ${PROJECT_SOURCE_DIR}/src/external/hipew/hipew.c
  ${PROJECT_SOURCE_DIR}/examples/common/volk/volk.c
  )
if(LUSDVIEW_TEXTURE_HAVE_HIP)
  if(NOT TARGET lusdview_texture_hip_plugin)
    add_library(lusdview_texture_hip_plugin MODULE hip_processor.hip)
    target_include_directories(lusdview_texture_hip_plugin PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR})
    set_target_properties(lusdview_texture_hip_plugin PROPERTIES
      HIP_ARCHITECTURES "${LUSDVIEW_TEXTURE_HIP_ARCHITECTURES}"
      PREFIX "")
  endif()
  add_dependencies(lusdview_texture_gpu_bench lusdview_texture_hip_plugin)
  target_compile_definitions(lusdview_texture_gpu_bench PRIVATE
    LUSDVIEW_TEXTURE_HAVE_HIP=1 LUSDVIEW_TEXTURE_HIP_DYNAMIC=1
    LUSDVIEW_TEXTURE_HIP_PLUGIN_PATH="$<TARGET_FILE:lusdview_texture_hip_plugin>")
endif()
if(LUSDVIEW_TEXTURE_HAVE_CUDA)
  target_sources(lusdview_texture_gpu_bench PRIVATE cuda_processor.cu)
  set_target_properties(lusdview_texture_gpu_bench PROPERTIES
    CUDA_ARCHITECTURES "${LUSDVIEW_TEXTURE_CUDA_ARCHITECTURES}")
  target_compile_definitions(lusdview_texture_gpu_bench PRIVATE LUSDVIEW_TEXTURE_HAVE_CUDA=1)
  set_source_files_properties(cuda_processor.cu PROPERTIES LANGUAGE CUDA)
endif()

# HDR/EXR real-data checks. Set LUSDVIEW_TEXTURE_GPU_ROOT to an EXR (or use
# LUSDVIEW_TEXTURE_GPU_HDR_ROOT for a separate HDR asset) to enable these.
set(_texture_hdr_test_env
    "LUSDVIEW_TEXTURE_BENCH=$<TARGET_FILE:lusdview_texture_gpu_bench>"
    "LUSDVIEW_TEXTURE_GPU_FORMATS=bc6h,bc7,astc"
    "LUSDVIEW_TEXTURE_GPU_MIPS=3"
    "LUSDVIEW_TEXTURE_GPU_MAX_DIMENSION=2048"
    "LUSDVIEW_TEXTURE_GPU_MIN_PSNR=5")
add_test(NAME lusdview-texture-vulkan-nvidia-hdr-real
  COMMAND ${CMAKE_COMMAND} -E env
          ${_texture_hdr_test_env}
          "LUSDVIEW_TEXTURE_GPU_BACKEND=vulkan"
          "LUSDVIEW_TEXTURE_GPU_DEVICE=NVIDIA"
          bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-texture-gpu-real.sh")
set_tests_properties(lusdview-texture-vulkan-nvidia-hdr-real PROPERTIES
  SKIP_RETURN_CODE 77 LABELS "lusdview;gpu;real;hdr;nvidia" TIMEOUT 900)

add_test(NAME lusdview-texture-vulkan-amd-hdr-real
  COMMAND ${CMAKE_COMMAND} -E env
          ${_texture_hdr_test_env}
          "LUSDVIEW_TEXTURE_GPU_BACKEND=vulkan"
          "LUSDVIEW_TEXTURE_GPU_DEVICE=AMD"
          bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-texture-gpu-real.sh")
set_tests_properties(lusdview-texture-vulkan-amd-hdr-real PROPERTIES
  SKIP_RETURN_CODE 77 LABELS "lusdview;gpu;real;hdr;amd" TIMEOUT 900)

set(_texture_bc6h_full_env
    "LUSDVIEW_TEXTURE_BENCH=$<TARGET_FILE:lusdview_texture_gpu_bench>"
    "LUSDVIEW_TEXTURE_GPU_FORMATS=bc6h"
    "LUSDVIEW_TEXTURE_GPU_MIPS=3"
    "LUSDVIEW_TEXTURE_GPU_MIN_PSNR=5"
    "LUSDVIEW_TEXTURE_GPU_MAX_GPU_MS=60000")
add_test(NAME lusdview-texture-vulkan-nvidia-bc6h-full-real
  COMMAND ${CMAKE_COMMAND} -E env
          ${_texture_bc6h_full_env}
          "LUSDVIEW_TEXTURE_GPU_BACKEND=vulkan"
          "LUSDVIEW_TEXTURE_GPU_DEVICE=NVIDIA"
          bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-texture-gpu-real.sh")
set_tests_properties(lusdview-texture-vulkan-nvidia-bc6h-full-real PROPERTIES
  SKIP_RETURN_CODE 77 LABELS "lusdview;gpu;real;hdr;full;nvidia" TIMEOUT 1800)

add_test(NAME lusdview-texture-vulkan-amd-bc6h-full-real
  COMMAND ${CMAKE_COMMAND} -E env
          ${_texture_bc6h_full_env}
          "LUSDVIEW_TEXTURE_GPU_BACKEND=vulkan"
          "LUSDVIEW_TEXTURE_GPU_DEVICE=AMD"
          bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-texture-gpu-real.sh")
set_tests_properties(lusdview-texture-vulkan-amd-bc6h-full-real PROPERTIES
  SKIP_RETURN_CODE 77 LABELS "lusdview;gpu;real;hdr;full;amd" TIMEOUT 1800)
if(LUSDVIEW_TEXTURE_HAVE_HIP)
  add_test(NAME lusdview-texture-hip-hdr-real
    COMMAND ${CMAKE_COMMAND} -E env
            ${_texture_hdr_test_env}
            "LUSDVIEW_TEXTURE_GPU_MAX_DIMENSION=2048"
            "LUSDVIEW_TEXTURE_GPU_BACKEND=hip"
            bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-texture-gpu-real.sh")
  set_tests_properties(lusdview-texture-hip-hdr-real PROPERTIES
    SKIP_RETURN_CODE 77 LABELS "lusdview;gpu;real;hdr;hip" TIMEOUT 900)
endif()

if(LUSDVIEW_TEXTURE_HAVE_CUDA)
  add_test(NAME lusdview-texture-cuda-hdr-real
    COMMAND ${CMAKE_COMMAND} -E env
            ${_texture_hdr_test_env}
            "LUSDVIEW_TEXTURE_GPU_MAX_DIMENSION=2048"
            "LUSDVIEW_TEXTURE_GPU_BACKEND=cuda"
            bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-texture-gpu-real.sh")
  set_tests_properties(lusdview-texture-cuda-hdr-real PROPERTIES
    SKIP_RETURN_CODE 77 LABELS "lusdview;gpu;real;hdr;cuda" TIMEOUT 900)
endif()
set_target_properties(lusdview_texture_gpu_bench PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_dependencies(lusdview_texture_gpu_bench lusdview_texture_gpu_shaders)
target_compile_features(lusdview_texture_gpu_bench PRIVATE cxx_std_17)
target_compile_definitions(lusdview_texture_gpu_bench PRIVATE VK_NO_PROTOTYPES)
target_include_directories(lusdview_texture_gpu_bench PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR} ${TEXBENCH_GEN}
  ${PROJECT_SOURCE_DIR}/examples/common/volk
  ${PROJECT_SOURCE_DIR}/examples/common/vulkan-headers/include
  ${PROJECT_SOURCE_DIR}/src
  ${PROJECT_SOURCE_DIR}/src/external
  ${PROJECT_SOURCE_DIR}/src/external/hipew
  ${PROJECT_SOURCE_DIR}/src/external/textools/texcomp/include
  ${PROJECT_SOURCE_DIR}/src/external/textools/texcomp/src)
target_link_libraries(lusdview_texture_gpu_bench PRIVATE
  lightusd_preview_images tydra lightusd_core lightusd_textools ${CMAKE_DL_LIBS})
if(UNIX)
  find_package(Threads REQUIRED)
  target_link_libraries(lusdview_texture_gpu_bench PRIVATE Threads::Threads m)
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(lusdview_texture_gpu_bench PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Wno-unused-parameter;-Wno-misleading-indentation>
    $<$<COMPILE_LANGUAGE:CUDA>:-Wno-unused-result>
    $<$<COMPILE_LANGUAGE:HIP>:-Wno-unused-result;-Wno-unused-value>
    $<$<COMPILE_LANGUAGE:C>:-w>)
endif()

add_test(NAME lusdview-texture-gpu-synthetic
  COMMAND lusdview_texture_gpu_bench --backend vulkan --synthetic
          --iterations 2 --warmup 1)
set_tests_properties(lusdview-texture-gpu-synthetic PROPERTIES
  SKIP_RETURN_CODE 77 LABELS "lusdview;gpu" TIMEOUT 120)

add_test(NAME lusdview-texture-gpu-mips
  COMMAND lusdview_texture_gpu_bench --backend vulkan --synthetic
          --format astc --mips 3 --iterations 1 --warmup 0)
set_tests_properties(lusdview-texture-gpu-mips PROPERTIES
  SKIP_RETURN_CODE 77 LABELS "lusdview;gpu;mips" TIMEOUT 120)

add_test(NAME lusdview-texture-gpu-bc6h-mips
  COMMAND lusdview_texture_gpu_bench --backend vulkan --synthetic
          --format bc6h --mips 3 --iterations 1 --warmup 0)
set_tests_properties(lusdview-texture-gpu-bc6h-mips PROPERTIES
  SKIP_RETURN_CODE 77 LABELS "lusdview;gpu;mips;hdr" TIMEOUT 120)

add_test(NAME lusdview-texture-vulkan-nvidia-mips
  COMMAND lusdview_texture_gpu_bench --backend vulkan --device NVIDIA
          --synthetic --format astc --mips 3 --iterations 1 --warmup 0)
set_tests_properties(lusdview-texture-vulkan-nvidia-mips PROPERTIES
  SKIP_RETURN_CODE 77 LABELS "lusdview;gpu;mips;nvidia" TIMEOUT 120)
add_test(NAME lusdview-texture-vulkan-nvidia-bc6h-mips
  COMMAND lusdview_texture_gpu_bench --backend vulkan --device NVIDIA
          --synthetic --format bc6h --mips 3 --iterations 1 --warmup 0)
set_tests_properties(lusdview-texture-vulkan-nvidia-bc6h-mips PROPERTIES
  SKIP_RETURN_CODE 77 LABELS "lusdview;gpu;mips;nvidia;hdr" TIMEOUT 120)

add_test(NAME lusdview-texture-vulkan-formats
  COMMAND lusdview_texture_gpu_bench --backend vulkan --synthetic
          --format bc1,bc3,bc5,bc6h,bc7,astc --mips 0
          --filter bilinear --linear --iterations 1 --warmup 0)
set_tests_properties(lusdview-texture-vulkan-formats PROPERTIES
  SKIP_RETURN_CODE 77 LABELS "lusdview;gpu;formats;hdr;mips" TIMEOUT 180)

add_test(NAME lusdview-texture-vulkan-nvidia-formats
  COMMAND lusdview_texture_gpu_bench --backend vulkan --device NVIDIA --synthetic
          --format bc1,bc3,bc5,bc6h,bc7,astc --mips 0
          --filter bilinear --linear --iterations 1 --warmup 0)
set_tests_properties(lusdview-texture-vulkan-nvidia-formats PROPERTIES
  SKIP_RETURN_CODE 77 LABELS "lusdview;gpu;formats;hdr;mips;nvidia" TIMEOUT 180)

# Opt-in real-corpus checks. These remain skipped unless the caller supplies
# LUSDVIEW_TEXTURE_GPU_ROOT, so ordinary builds never depend on private assets.
add_test(NAME lusdview-texture-vulkan-nvidia-real
  COMMAND ${CMAKE_COMMAND} -E env
          "LUSDVIEW_TEXTURE_BENCH=$<TARGET_FILE:lusdview_texture_gpu_bench>"
          "LUSDVIEW_TEXTURE_GPU_BACKEND=vulkan"
          "LUSDVIEW_TEXTURE_GPU_DEVICE=NVIDIA"
          "LUSDVIEW_TEXTURE_GPU_PREFER_TEXTURES=1"
          bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-texture-gpu-real.sh")
set_tests_properties(lusdview-texture-vulkan-nvidia-real PROPERTIES
  SKIP_RETURN_CODE 77 LABELS "lusdview;gpu;real;nvidia" TIMEOUT 900)

add_test(NAME lusdview-texture-vulkan-nvidia-formats-real
  COMMAND ${CMAKE_COMMAND} -E env
          "LUSDVIEW_TEXTURE_BENCH=$<TARGET_FILE:lusdview_texture_gpu_bench>"
          "LUSDVIEW_TEXTURE_GPU_BACKEND=vulkan"
          "LUSDVIEW_TEXTURE_GPU_DEVICE=NVIDIA"
          "LUSDVIEW_TEXTURE_GPU_FORMATS=bc1,bc3,bc5,bc7,astc"
          "LUSDVIEW_TEXTURE_GPU_MIPS=3"
          "LUSDVIEW_TEXTURE_GPU_MAX_IMAGES=1"
          "LUSDVIEW_TEXTURE_GPU_PREFER_TEXTURES=1"
          "LUSDVIEW_TEXTURE_GPU_MIN_PSNR=1"
          bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-texture-gpu-real.sh")
set_tests_properties(lusdview-texture-vulkan-nvidia-formats-real PROPERTIES
  SKIP_RETURN_CODE 77 LABELS "lusdview;gpu;real;formats;nvidia" TIMEOUT 900)

add_test(NAME lusdview-texture-vulkan-amd-real
  COMMAND ${CMAKE_COMMAND} -E env
          "LUSDVIEW_TEXTURE_BENCH=$<TARGET_FILE:lusdview_texture_gpu_bench>"
          "LUSDVIEW_TEXTURE_GPU_BACKEND=vulkan"
          "LUSDVIEW_TEXTURE_GPU_DEVICE=AMD"
          "LUSDVIEW_TEXTURE_GPU_PREFER_TEXTURES=1"
          bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-texture-gpu-real.sh")
set_tests_properties(lusdview-texture-vulkan-amd-real PROPERTIES
  SKIP_RETURN_CODE 77 LABELS "lusdview;gpu;real;amd" TIMEOUT 900)

add_test(NAME lusdview-texture-vulkan-amd-formats-real
  COMMAND ${CMAKE_COMMAND} -E env
          "LUSDVIEW_TEXTURE_BENCH=$<TARGET_FILE:lusdview_texture_gpu_bench>"
          "LUSDVIEW_TEXTURE_GPU_BACKEND=vulkan"
          "LUSDVIEW_TEXTURE_GPU_DEVICE=AMD"
          "LUSDVIEW_TEXTURE_GPU_FORMATS=bc1,bc3,bc5,bc7,astc"
          "LUSDVIEW_TEXTURE_GPU_MIPS=3"
          "LUSDVIEW_TEXTURE_GPU_MAX_IMAGES=1"
          "LUSDVIEW_TEXTURE_GPU_PREFER_TEXTURES=1"
          "LUSDVIEW_TEXTURE_GPU_MIN_PSNR=1"
          bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-texture-gpu-real.sh")
set_tests_properties(lusdview-texture-vulkan-amd-formats-real PROPERTIES
  SKIP_RETURN_CODE 77 LABELS "lusdview;gpu;real;formats;amd" TIMEOUT 900)

add_test(NAME lusdview-texture-tiff-metadata-real
  COMMAND ${CMAKE_COMMAND} -E env
          "LUSDVIEW_TEXTURE_BENCH=$<TARGET_FILE:lusdview_texture_gpu_bench>"
          "LUSDVIEW_TEXTURE_GPU_METADATA_ONLY=1"
          "LUSDVIEW_TEXTURE_GPU_MAX_IMAGES=8"
          "LUSDVIEW_TEXTURE_GPU_PREFER_TEXTURES=1"
          bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-texture-gpu-real.sh")
set_tests_properties(lusdview-texture-tiff-metadata-real PROPERTIES
  SKIP_RETURN_CODE 77 LABELS "lusdview;gpu;real;tiff;metadata" TIMEOUT 300)

add_test(NAME lusdview-texture-tiff-decode-real
  COMMAND ${CMAKE_COMMAND} -E env
          "LUSDVIEW_TEXTURE_BENCH=$<TARGET_FILE:lusdview_texture_gpu_bench>"
          "LUSDVIEW_TEXTURE_GPU_BACKEND=vulkan"
          "LUSDVIEW_TEXTURE_GPU_FORMATS=bc7"
          "LUSDVIEW_TEXTURE_GPU_MIPS=3"
          "LUSDVIEW_TEXTURE_GPU_MAX_IMAGES=1"
          "LUSDVIEW_TEXTURE_GPU_MAX_GPU_MS=5000"
          bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-texture-gpu-real.sh")
set_tests_properties(lusdview-texture-tiff-decode-real PROPERTIES
  SKIP_RETURN_CODE 77 LABELS "lusdview;gpu;real;tiff;decode" TIMEOUT 900)

add_test(NAME lusdview-texture-vulkan-validation
  COMMAND ${CMAKE_COMMAND} -E env
          "LUSDVIEW_TEXTURE_BENCH=$<TARGET_FILE:lusdview_texture_gpu_bench>"
          bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-texture-vulkan-validation.sh")
set_tests_properties(lusdview-texture-vulkan-validation PROPERTIES
  SKIP_RETURN_CODE 77 LABELS "lusdview;gpu;vulkan;validation" TIMEOUT 180)

if(LUSDVIEW_TEXTURE_HAVE_HIP)
  add_test(NAME lusdview-texture-hip-real
    COMMAND ${CMAKE_COMMAND} -E env
            "LUSDVIEW_TEXTURE_BENCH=$<TARGET_FILE:lusdview_texture_gpu_bench>"
            "LUSDVIEW_TEXTURE_GPU_BACKEND=hip"
            "LUSDVIEW_TEXTURE_GPU_PREFER_TEXTURES=1"
            bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-texture-gpu-real.sh")
  set_tests_properties(lusdview-texture-hip-real PROPERTIES
    SKIP_RETURN_CODE 77 LABELS "lusdview;gpu;real;hip" TIMEOUT 900)
endif()

if(LUSDVIEW_TEXTURE_HAVE_CUDA)
  add_test(NAME lusdview-texture-cuda-real
    COMMAND ${CMAKE_COMMAND} -E env
            "LUSDVIEW_TEXTURE_BENCH=$<TARGET_FILE:lusdview_texture_gpu_bench>"
            "LUSDVIEW_TEXTURE_GPU_BACKEND=cuda"
            "LUSDVIEW_TEXTURE_GPU_PREFER_TEXTURES=1"
            bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-texture-gpu-real.sh")
  set_tests_properties(lusdview-texture-cuda-real PROPERTIES
    SKIP_RETURN_CODE 77 LABELS "lusdview;gpu;real;cuda" TIMEOUT 900)
endif()

if(LUSDVIEW_TEXTURE_HAVE_HIP)
  get_filename_component(_texture_hip_bin "${CMAKE_HIP_COMPILER}" DIRECTORY)
  get_filename_component(_texture_hip_root "${_texture_hip_bin}/../../.." REALPATH)
  set_tests_properties(lusdview-texture-gpu-synthetic lusdview-texture-gpu-mips lusdview-texture-gpu-bc6h-mips lusdview-texture-vulkan-formats
    lusdview-texture-vulkan-nvidia-real lusdview-texture-vulkan-nvidia-formats-real
    lusdview-texture-vulkan-amd-real
    lusdview-texture-vulkan-amd-formats-real
    lusdview-texture-tiff-metadata-real lusdview-texture-vulkan-nvidia-hdr-real
    lusdview-texture-vulkan-amd-hdr-real lusdview-texture-tiff-decode-real
    lusdview-texture-vulkan-validation lusdview-texture-vulkan-nvidia-bc6h-full-real
    lusdview-texture-vulkan-amd-bc6h-full-real PROPERTIES
    ENVIRONMENT "LD_LIBRARY_PATH=${_texture_hip_root}/lib:$ENV{LD_LIBRARY_PATH}")
  add_test(NAME lusdview-texture-hip-mips
    COMMAND ${CMAKE_COMMAND} -E env
            "LD_LIBRARY_PATH=${_texture_hip_root}/lib:$ENV{LD_LIBRARY_PATH}"
            $<TARGET_FILE:lusdview_texture_gpu_bench> --backend hip --synthetic
            --format astc --mips 3 --iterations 1 --warmup 0)
  set_tests_properties(lusdview-texture-hip-mips PROPERTIES
    SKIP_RETURN_CODE 77 LABELS "lusdview;gpu;mips;hip" TIMEOUT 120)
  add_test(NAME lusdview-texture-hip-bc6h-mips
    COMMAND ${CMAKE_COMMAND} -E env
            "LD_LIBRARY_PATH=${_texture_hip_root}/lib:$ENV{LD_LIBRARY_PATH}"
            $<TARGET_FILE:lusdview_texture_gpu_bench> --backend hip --synthetic
            --format bc6h --mips 3 --iterations 1 --warmup 0)
  set_tests_properties(lusdview-texture-hip-bc6h-mips PROPERTIES
    SKIP_RETURN_CODE 77 LABELS "lusdview;gpu;mips;hip;hdr" TIMEOUT 120)
  add_test(NAME lusdview-texture-hip-formats
    COMMAND ${CMAKE_COMMAND} -E env
            "LD_LIBRARY_PATH=${_texture_hip_root}/lib:$ENV{LD_LIBRARY_PATH}"
            $<TARGET_FILE:lusdview_texture_gpu_bench> --backend hip --synthetic
            --format bc1,bc3,bc5,bc6h,bc7,astc --mips 0
            --filter bilinear --linear --iterations 1 --warmup 0)
  set_tests_properties(lusdview-texture-hip-formats PROPERTIES
    SKIP_RETURN_CODE 77 LABELS "lusdview;gpu;formats;hdr;mips;hip" TIMEOUT 180)
endif()

if(LUSDVIEW_TEXTURE_HAVE_CUDA)
  get_filename_component(_texture_cuda_bin "${CMAKE_CUDA_COMPILER}" DIRECTORY)
  get_filename_component(_texture_cuda_root "${_texture_cuda_bin}/.." REALPATH)
  add_test(NAME lusdview-texture-cuda-mips
    COMMAND lusdview_texture_gpu_bench --backend cuda --synthetic
            --format astc --mips 3 --iterations 1 --warmup 0)
  set_tests_properties(lusdview-texture-cuda-mips PROPERTIES
    SKIP_RETURN_CODE 77 LABELS "lusdview;gpu;mips;cuda" TIMEOUT 120)
  add_test(NAME lusdview-texture-cuda-bc6h-mips
    COMMAND lusdview_texture_gpu_bench --backend cuda --synthetic
            --format bc6h --mips 3 --iterations 1 --warmup 0)
  set_tests_properties(lusdview-texture-cuda-bc6h-mips PROPERTIES
    SKIP_RETURN_CODE 77 LABELS "lusdview;gpu;mips;cuda;hdr" TIMEOUT 120)
  add_test(NAME lusdview-texture-cuda-formats
    COMMAND ${CMAKE_COMMAND} -E env
            "LD_LIBRARY_PATH=${_texture_cuda_root}/lib64:${_texture_hip_root}/lib:$ENV{LD_LIBRARY_PATH}"
            $<TARGET_FILE:lusdview_texture_gpu_bench> --backend cuda --synthetic
            --format bc1,bc3,bc5,bc6h,bc7,astc --mips 0
            --filter bilinear --linear --iterations 1 --warmup 0)
  set_tests_properties(lusdview-texture-cuda-formats PROPERTIES
    SKIP_RETURN_CODE 77 LABELS "lusdview;gpu;formats;hdr;mips;cuda" TIMEOUT 180)
  set_tests_properties(lusdview-texture-cuda-mips lusdview-texture-cuda-bc6h-mips
    lusdview-texture-cuda-formats lusdview-texture-vulkan-nvidia-mips
    lusdview-texture-vulkan-nvidia-bc6h-mips lusdview-texture-vulkan-nvidia-formats
    lusdview-texture-vulkan-nvidia-real lusdview-texture-vulkan-nvidia-formats-real
    lusdview-texture-vulkan-amd-real
    lusdview-texture-vulkan-amd-formats-real
    lusdview-texture-tiff-metadata-real lusdview-texture-vulkan-nvidia-hdr-real
    lusdview-texture-vulkan-amd-hdr-real lusdview-texture-hip-real
    lusdview-texture-hip-hdr-real lusdview-texture-cuda-real
    lusdview-texture-cuda-hdr-real lusdview-texture-tiff-decode-real
    lusdview-texture-vulkan-validation lusdview-texture-vulkan-nvidia-bc6h-full-real
    lusdview-texture-vulkan-amd-bc6h-full-real PROPERTIES
    ENVIRONMENT "LD_LIBRARY_PATH=${_texture_cuda_root}/lib64:${_texture_hip_root}/lib:$ENV{LD_LIBRARY_PATH}")
endif()
