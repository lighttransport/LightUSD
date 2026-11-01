# ld3d12view - native Windows (Direct3D 12) minimal USD viewer example.
#
# Added from src/next/CMakeLists.txt after examples/lmetalview, whose portable
# C++ core (lmetalview_core: USD load -> flat preview scene behind core/lmv.h,
# plus the shared OrbitCamera wrapper) it reuses. The window and input come from
# the vendored lightui (examples/common/lightui); D3D12 renders onto its HWND.
#
# Shaders are compiled at startup with D3DCompile, so no offline shader
# toolchain (dxc/fxc) is needed.

if(NOT WIN32)
  message(STATUS "ld3d12view: skipped (Windows only)")
  return()
endif()
if(NOT TARGET lmetalview_core)
  message(STATUS "ld3d12view: skipped (lmetalview_core target not available)")
  return()
endif()

get_filename_component(LD3D_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(LMV_EXAMPLE_DIR ${LD3D_REPO_ROOT}/examples/lmetalview)

if(NOT TARGET lightui)
  add_subdirectory(${LD3D_REPO_ROOT}/examples/common/lightui lightui_ld3d12view)
endif()

add_executable(ld3d12view
    main.cc
    d3d12/device.cc
    d3d12/grid.cc
    d3d12/lighting.cc
    d3d12/scene_gpu.cc
    d3d12/mesh_renderer.cc
    d3d12/points_lines.cc
    d3d12/hud_renderer.cc
    d3d12/background.cc
    hud.cc
    loader.cc)
target_compile_features(ld3d12view PRIVATE cxx_std_17)
target_include_directories(ld3d12view PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${LD3D_REPO_ROOT}/src/external)
target_compile_definitions(ld3d12view PRIVATE
    NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
target_link_libraries(ld3d12view PRIVATE
    lmetalview_core lightui d3d12 dxgi d3dcompiler user32 gdi32)
if(MSVC)
  target_compile_options(ld3d12view PRIVATE /W4 /utf-8)
  # Console subsystem on purpose: diagnostics go to stderr; main() is the entry.
endif()

if(LIGHTUSD_BUILD_TESTS OR LIGHTUSD_NEXT_BUILD_TESTS)
  # Hidden window on the WARP software adapter: runs on machines without a GPU.
  add_test(NAME ld3d12view-smoke-warp
      COMMAND ld3d12view --hidden --warp --size 320x200 --frames 5
              --screenshot ${CMAKE_CURRENT_BINARY_DIR}/ld3d12view-smoke.png)
  set_tests_properties(ld3d12view-smoke-warp PROPERTIES
      LABELS "ld3d12view;gpu;d3d12" TIMEOUT 120)

  add_test(NAME ld3d12view-mesh-warp
      COMMAND ld3d12view --hidden --warp --size 320x240 --frames 3
              --screenshot ${CMAKE_CURRENT_BINARY_DIR}/ld3d12view-mesh.png
              ${LD3D_REPO_ROOT}/models/geomsubset-material-001.usda)
  add_test(NAME ld3d12view-instanced-warp
      COMMAND ld3d12view --hidden --warp --size 320x240 --frames 3 --wireframe
              --screenshot ${CMAKE_CURRENT_BINARY_DIR}/ld3d12view-instanced.png
              ${LD3D_REPO_ROOT}/tests/usda/pointinstancer-full-001.usda)
  # Texture paths: 8-bit sRGB PNG/JPG, float EXR, and BC1 from KTX2.
  foreach(tex IN ITEMS
      "textured|${LD3D_REPO_ROOT}/models/cube-mtlx-texture.usda"
      "exr|${LMV_EXAMPLE_DIR}/tests/exr-texture.usda"
      "bc1|${LMV_EXAMPLE_DIR}/tests/ktx2-bc1-texture.usda")
    string(REPLACE "|" ";" tex_parts "${tex}")
    list(GET tex_parts 0 tex_name)
    list(GET tex_parts 1 tex_file)
    add_test(NAME ld3d12view-${tex_name}-warp
        COMMAND ld3d12view --hidden --warp --size 320x240 --frames 3
                --screenshot ${CMAKE_CURRENT_BINARY_DIR}/ld3d12view-${tex_name}.png
                ${tex_file})
    set_tests_properties(ld3d12view-${tex_name}-warp PROPERTIES
        LABELS "ld3d12view;gpu;d3d12" TIMEOUT 120)
  endforeach()
  # UDIM (texture array + tile LUT) and Ptex (atlas + per-face rects, per-vertex
  # face ids): both used to fall back to the constant colour.
  add_test(NAME ld3d12view-udim-warp
      COMMAND ld3d12view --hidden --warp --size 320x240 --frames 3 --no-hud
              --screenshot ${CMAKE_CURRENT_BINARY_DIR}/ld3d12view-udim.png
              ${LMV_EXAMPLE_DIR}/tests/udim-texture.usda)
  add_test(NAME ld3d12view-ptex-warp
      COMMAND ld3d12view --hidden --warp --size 320x240 --frames 3 --no-hud
              --screenshot ${CMAKE_CURRENT_BINARY_DIR}/ld3d12view-ptex.png
              ${LMV_EXAMPLE_DIR}/tests/ptex-texture.usda)
  set_tests_properties(ld3d12view-udim-warp ld3d12view-ptex-warp PROPERTIES
      PASS_REGULAR_EXPRESSION "uploaded, 0 unsupported"
      LABELS "ld3d12view;gpu;d3d12" TIMEOUT 120)
  # Alpha: blend-mode materials sorted back to front over the opaque image, and
  # alpha-tested (cutout) materials in the main pass and in the shadow map.
  add_test(NAME ld3d12view-alpha-modes-warp
      COMMAND ld3d12view --hidden --warp --size 320x240 --frames 3 --no-hud
              --preset 1
              --screenshot ${CMAKE_CURRENT_BINARY_DIR}/ld3d12view-alpha-modes.png
              ${LD3D_REPO_ROOT}/models/lusdquicklook-alpha-modes.usda)
  add_test(NAME ld3d12view-transparency-warp
      COMMAND ld3d12view --hidden --warp --size 320x240 --frames 3 --no-hud
              --preset 1
              --screenshot ${CMAKE_CURRENT_BINARY_DIR}/ld3d12view-transparency.png
              ${LD3D_REPO_ROOT}/models/lusdview-transparency.usda)
  set_tests_properties(ld3d12view-alpha-modes-warp ld3d12view-transparency-warp
      PROPERTIES LABELS "ld3d12view;gpu;d3d12" TIMEOUT 120)
  # OpenPBR extras (coat, sheen, subsurface, transmission, thin film,
  # anisotropy) and the background (dome environment or gradient; --no-background
  # falls back to the clear colour).
  foreach(pbr IN ITEMS coated-cube glass-sphere subsurface-sphere brick-sphere
                       metallic-cube emissive-plane multi-object)
    add_test(NAME ld3d12view-openpbr-${pbr}-warp
        COMMAND ld3d12view --hidden --warp --size 320x240 --frames 3 --no-hud
                --screenshot ${CMAKE_CURRENT_BINARY_DIR}/ld3d12view-openpbr-${pbr}.png
                ${LD3D_REPO_ROOT}/models/openpbr-${pbr}.usda)
    set_tests_properties(ld3d12view-openpbr-${pbr}-warp PROPERTIES
        LABELS "ld3d12view;gpu;d3d12" TIMEOUT 120)
  endforeach()
  add_test(NAME ld3d12view-background-env-warp
      COMMAND ld3d12view --hidden --warp --size 320x240 --frames 3 --no-hud
              --screenshot ${CMAKE_CURRENT_BINARY_DIR}/ld3d12view-background-env.png
              ${LD3D_REPO_ROOT}/models/mtlx-normalmap-sphere.usda)
  add_test(NAME ld3d12view-background-off-warp
      COMMAND ld3d12view --hidden --warp --size 320x240 --frames 3 --no-hud
              --no-background ${LD3D_REPO_ROOT}/models/suzanne-pbr.usda)
  set_tests_properties(ld3d12view-background-env-warp
      PROPERTIES PASS_REGULAR_EXPRESSION "dome environment")
  set_tests_properties(ld3d12view-background-env-warp ld3d12view-background-off-warp
      PROPERTIES LABELS "ld3d12view;gpu;d3d12" TIMEOUT 120)
  # Lighting: authored directional light + shadow map, and a dome environment
  # (SH9 irradiance + reflection).
  add_test(NAME ld3d12view-shadow-warp
      COMMAND ld3d12view --hidden --warp --size 320x240 --frames 3 --preset 0
              --screenshot ${CMAKE_CURRENT_BINARY_DIR}/ld3d12view-shadow.png
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/shadow-box.usda)
  add_test(NAME ld3d12view-env-warp
      COMMAND ld3d12view --hidden --warp --size 320x240 --frames 3
              --screenshot ${CMAKE_CURRENT_BINARY_DIR}/ld3d12view-env.png
              ${LD3D_REPO_ROOT}/models/mtlx-normalmap-sphere.usda)
  set_tests_properties(ld3d12view-shadow-warp ld3d12view-env-warp
      PROPERTIES LABELS "ld3d12view;gpu;d3d12" TIMEOUT 120)
  # Animation: in-place updates (transforms, lights, camera, visibility,
  # instancer) and the re-conversion fallback (animated materials). Same
  # fixtures and times as the lmetalview headless tests.
  foreach(anim IN ITEMS
      "xform|${LMV_EXAMPLE_DIR}/tests/animated-xform.usda|5"
      "light|${LMV_EXAMPLE_DIR}/tests/animated-light.usda|12"
      "camera|${LMV_EXAMPLE_DIR}/tests/animated-camera.usda|12"
      "visibility|${LMV_EXAMPLE_DIR}/tests/animated-visibility.usda|12"
      "material|${LMV_EXAMPLE_DIR}/tests/animated-material.usda|12"
      "pointinstancer|${LD3D_REPO_ROOT}/tests/usda/pointinstancer-full-001.usda|12"
      "skinned|${LD3D_REPO_ROOT}/tests/usda/ue-skinned-blendshape.usda|2"
      "skeletal|${LD3D_REPO_ROOT}/models/skintest-animated.usda|12")
    string(REPLACE "|" ";" anim_parts "${anim}")
    list(GET anim_parts 0 anim_name)
    list(GET anim_parts 1 anim_file)
    list(GET anim_parts 2 anim_time)
    add_test(NAME ld3d12view-animated-${anim_name}-warp
        COMMAND ld3d12view --hidden --warp --size 320x240 --frames 3
                --authored-camera --animate-from 0 --time ${anim_time}
                --screenshot ${CMAKE_CURRENT_BINARY_DIR}/ld3d12view-animated-${anim_name}.png
                ${anim_file})
    set_tests_properties(ld3d12view-animated-${anim_name}-warp PROPERTIES
        LABELS "ld3d12view;gpu;d3d12" TIMEOUT 120)
  endforeach()
  # Points (splats) and curves (ribbons).
  foreach(geo IN ITEMS
      "points|${LD3D_REPO_ROOT}/tests/usda/lusdrender-points.usda"
      "gaussians|${LD3D_REPO_ROOT}/tests/usda/lusdview-gaussian-half.usda"
      "curves|${LD3D_REPO_ROOT}/models/lusdview-curves-ribbons.usda")
    string(REPLACE "|" ";" geo_parts "${geo}")
    list(GET geo_parts 0 geo_name)
    list(GET geo_parts 1 geo_file)
    add_test(NAME ld3d12view-${geo_name}-warp
        COMMAND ld3d12view --hidden --warp --size 320x240 --frames 3
                --screenshot ${CMAKE_CURRENT_BINARY_DIR}/ld3d12view-${geo_name}.png
                ${geo_file})
    set_tests_properties(ld3d12view-${geo_name}-warp PROPERTIES
        LABELS "ld3d12view;gpu;d3d12" TIMEOUT 120)
  endforeach()
  # Space toggles playback (a synthetic WM_KEYDOWN through the window
  # procedure); animated scenes autoplay from the start of the clip. The pause
  # test only checks the toggle: playback is wall-clock driven, so the time it
  # pauses at depends on how slow the first frames were. The start of the clip
  # is checked deterministically by space-play (starts paused, so t is exactly 1).
  add_test(NAME ld3d12view-space-pause-warp
      COMMAND ld3d12view --hidden --warp --size 320x240 --frames 12
              --simulate-space ${LD3D_REPO_ROOT}/models/skintest-animated.usda)
  add_test(NAME ld3d12view-space-play-warp
      COMMAND ld3d12view --hidden --warp --size 320x240 --frames 12 --paused
              --simulate-space ${LD3D_REPO_ROOT}/models/skintest-animated.usda)
  add_test(NAME ld3d12view-space-static-warp
      COMMAND ld3d12view --hidden --warp --size 320x240 --frames 12
              --simulate-space ${LD3D_REPO_ROOT}/models/suzanne-pbr.usda)
  set_tests_properties(ld3d12view-space-pause-warp
      PROPERTIES PASS_REGULAR_EXPRESSION "paused at t=")
  set_tests_properties(ld3d12view-space-play-warp
      PROPERTIES PASS_REGULAR_EXPRESSION "playing at t=1[.]00")
  set_tests_properties(ld3d12view-space-static-warp
      PROPERTIES PASS_REGULAR_EXPRESSION "no animation in this scene")
  set_tests_properties(ld3d12view-space-pause-warp ld3d12view-space-play-warp
      ld3d12view-space-static-warp PROPERTIES
      LABELS "ld3d12view;gpu;d3d12" TIMEOUT 120)

  # HUD (lightui bar composited over the 3D view): synthetic mouse clicks on
  # the play button and the timeline go through the real window procedure; a
  # synthetic resize re-creates the swapchain, depth buffer and HUD texture.
  set(hud_scene ${LD3D_REPO_ROOT}/models/skintest-animated.usda)
  add_test(NAME ld3d12view-hud-play-click-warp
      COMMAND ld3d12view --hidden --warp --size 640x400 --frames 14
              --simulate-click play ${hud_scene})
  add_test(NAME ld3d12view-hud-timeline-click-warp
      COMMAND ld3d12view --hidden --warp --size 640x400 --frames 14
              --simulate-click timeline=0.5 ${hud_scene})
  add_test(NAME ld3d12view-hud-resize-warp
      COMMAND ld3d12view --hidden --warp --size 640x400 --frames 14
              --simulate-resize 900x600
              --screenshot ${CMAKE_CURRENT_BINARY_DIR}/ld3d12view-hud-resize.png
              ${hud_scene})
  add_test(NAME ld3d12view-hud-hidden-warp
      COMMAND ld3d12view --hidden --warp --size 640x400 --frames 6 --no-hud
              ${hud_scene})
  set_tests_properties(ld3d12view-hud-play-click-warp
      PROPERTIES PASS_REGULAR_EXPRESSION "paused at t=")
  set_tests_properties(ld3d12view-hud-timeline-click-warp
      PROPERTIES PASS_REGULAR_EXPRESSION "timeline seek to t=24[.]50")
  set_tests_properties(ld3d12view-hud-resize-warp
      PROPERTIES PASS_REGULAR_EXPRESSION "resized to ")
  set_tests_properties(ld3d12view-hud-play-click-warp
      ld3d12view-hud-timeline-click-warp ld3d12view-hud-resize-warp
      ld3d12view-hud-hidden-warp PROPERTIES
      LABELS "ld3d12view;gpu;d3d12" TIMEOUT 120)

  # Asynchronous loading: progress phases, cancellation with Esc (a synthetic
  # key press while the load is held in flight), and a drop that supersedes a
  # load that is still running. The test delay keeps a load observable.
  add_test(NAME ld3d12view-load-async-warp
      COMMAND ld3d12view --hidden --warp --size 320x240 --frames 4
              ${LD3D_REPO_ROOT}/models/suzanne-pbr.usda)
  add_test(NAME ld3d12view-load-progress-warp
      COMMAND ld3d12view --hidden --warp --size 640x400 --load-delay-ms 600
              --capture-loading --frames 99
              --screenshot ${CMAKE_CURRENT_BINARY_DIR}/ld3d12view-loading.png
              ${LD3D_REPO_ROOT}/models/suzanne-pbr.usda)
  add_test(NAME ld3d12view-load-cancel-warp
      COMMAND ld3d12view --hidden --warp --size 320x240 --frames 4
              --load-delay-ms 3000 --simulate-cancel
              ${LD3D_REPO_ROOT}/models/suzanne-pbr.usda)
  add_test(NAME ld3d12view-load-supersede-warp
      COMMAND ld3d12view --hidden --warp --size 320x240 --frames 4
              --load-delay-ms 1500
              --simulate-drop ${LD3D_REPO_ROOT}/models/geomsubset-material-001.usda
              ${LD3D_REPO_ROOT}/models/suzanne-pbr.usda)
  add_test(NAME ld3d12view-load-missing-warp
      COMMAND ld3d12view --hidden --warp --size 320x240 --frames 4
              ${CMAKE_CURRENT_BINARY_DIR}/does-not-exist.usda)
  set_tests_properties(ld3d12view-load-async-warp
      PROPERTIES PASS_REGULAR_EXPRESSION "opened .*suzanne-pbr")
  set_tests_properties(ld3d12view-load-progress-warp
      PROPERTIES PASS_REGULAR_EXPRESSION "loading suzanne-pbr.usda: Opening")
  set_tests_properties(ld3d12view-load-cancel-warp
      PROPERTIES PASS_REGULAR_EXPRESSION "load cancelled"
                 FAIL_REGULAR_EXPRESSION "opened ")
  set_tests_properties(ld3d12view-load-supersede-warp
      PROPERTIES PASS_REGULAR_EXPRESSION "load cancelled.*opened .*geomsubset-material-001"
                 FAIL_REGULAR_EXPRESSION "opened .*suzanne-pbr")
  # A load that fails for the command-line scene is an error exit.
  set_tests_properties(ld3d12view-load-missing-warp
      PROPERTIES WILL_FAIL TRUE)
  set_tests_properties(ld3d12view-load-async-warp ld3d12view-load-progress-warp
      ld3d12view-load-cancel-warp ld3d12view-load-supersede-warp
      ld3d12view-load-missing-warp PROPERTIES
      LABELS "ld3d12view;gpu;d3d12" TIMEOUT 120)

  # Drag and drop: a synthetic WM_DROPFILES (as Explorer sends) opens a file in
  # an empty viewer and replaces the open scene; a bad path keeps the old one.
  add_test(NAME ld3d12view-drop-open-warp
      COMMAND ld3d12view --hidden --warp --size 320x240 --frames 8
              --simulate-drop ${LD3D_REPO_ROOT}/models/suzanne-pbr.usda)
  add_test(NAME ld3d12view-drop-replace-warp
      COMMAND ld3d12view --hidden --warp --size 320x240 --frames 8
              --simulate-drop ${LD3D_REPO_ROOT}/models/geomsubset-material-001.usda
              ${LD3D_REPO_ROOT}/models/suzanne-pbr.usda)
  add_test(NAME ld3d12view-drop-bad-warp
      COMMAND ld3d12view --hidden --warp --size 320x240 --frames 8
              --simulate-drop ${CMAKE_CURRENT_BINARY_DIR}/does-not-exist.usda
              ${LD3D_REPO_ROOT}/models/suzanne-pbr.usda)
  set_tests_properties(ld3d12view-drop-open-warp ld3d12view-drop-replace-warp
      PROPERTIES PASS_REGULAR_EXPRESSION "opened ")
  set_tests_properties(ld3d12view-drop-bad-warp
      PROPERTIES PASS_REGULAR_EXPRESSION "cannot load"
                 FAIL_REGULAR_EXPRESSION "opened ")
  set_tests_properties(ld3d12view-drop-open-warp ld3d12view-drop-replace-warp
      ld3d12view-drop-bad-warp PROPERTIES LABELS "ld3d12view;gpu;d3d12"
      TIMEOUT 120)
  # Wall-clock playback pump (non-looping run past the end of the clip).
  add_test(NAME ld3d12view-playback-warp
      COMMAND ld3d12view --hidden --warp --size 320x240 --frames 30 --play
              --speed 40 --no-loop
              ${LMV_EXAMPLE_DIR}/tests/animated-xform.usda)
  set_tests_properties(ld3d12view-playback-warp PROPERTIES
      LABELS "ld3d12view;gpu;d3d12" TIMEOUT 120)
  set_tests_properties(ld3d12view-mesh-warp ld3d12view-instanced-warp
      PROPERTIES LABELS "ld3d12view;gpu;d3d12" TIMEOUT 120)
endif()
