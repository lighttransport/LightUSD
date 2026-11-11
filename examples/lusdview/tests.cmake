# lusdview integration tests (screenshots, backends, deformation). Included
# from examples/lusdview/CMakeLists.txt when LIGHTUSD_BUILD_TESTS is ON. GUI
# runs must go through xvfb (scripts/xctest.sh); GPU-only checks skip with 77.
if(LIGHTUSD_PYTHON_EXECUTABLE)
  set(LUSDR_PYTHON "${LIGHTUSD_PYTHON_EXECUTABLE}")
else()
  find_program(LUSDR_PYTHON NAMES python3 python)
endif()


# Windowless Vulkan render regression. This works on both hardware and
# software Vulkan devices and self-skips when no Vulkan device is available.
if(UNIX AND NOT APPLE AND TARGET lusdview)
  # Opt-in external Island production smoke. With no ISLAND_USD environment
  # variable this returns 77 immediately, proving the corpus remains optional
  # in ordinary CI while making the real-data gate discoverable via CTest.
  add_test(
    NAME lusdview-island-production-smoke
    COMMAND ${CMAKE_COMMAND} -E env
            "LUSDVIEW=$<TARGET_FILE:lusdview>"
            bash
            "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-island-production-smoke.sh"
    )
  set_tests_properties(lusdview-island-production-smoke PROPERTIES
                       SKIP_RETURN_CODE 77 TIMEOUT 5400
                       LABELS "lusdview;external;island")

  add_test(
    NAME lusdview-progressive-checkpoints
    COMMAND ${CMAKE_COMMAND} -E env
            "LUSDVIEW=$<TARGET_FILE:lusdview>"
            "LUSDVIEW_ROOT=${PROJECT_SOURCE_DIR}"
            bash
            "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-progressive-checkpoints.sh"
    )
  set_tests_properties(lusdview-progressive-checkpoints PROPERTIES
                       SKIP_RETURN_CODE 77 TIMEOUT 150 LABELS "lusdview")

  add_test(
    NAME lusdview-vk-multitlas
    COMMAND ${CMAKE_COMMAND} -E env
            "LUSDVIEW=$<TARGET_FILE:lusdview>"
            "LUSDVIEW_ROOT=${PROJECT_SOURCE_DIR}"
            bash
            "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-vk-multitlas.sh"
    )
  set_tests_properties(lusdview-vk-multitlas PROPERTIES
                       SKIP_RETURN_CODE 77 TIMEOUT 600 LABELS "lusdview")

  add_test(
    NAME lusdview-ptex-camera-demand
    COMMAND ${CMAKE_COMMAND} -E env
            "LUSDVIEW=$<TARGET_FILE:lusdview>"
            "LUSDVIEW_PTEX_GENERATOR=$<TARGET_FILE:lusdview_ptex_atlas_test>"
            bash
            "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-ptex-camera-demand.sh"
    )
  set_tests_properties(lusdview-ptex-camera-demand PROPERTIES
                       SKIP_RETURN_CODE 77 TIMEOUT 90 LABELS "lusdview")

  add_test(
    NAME lusdview-vk-render
    COMMAND ${CMAKE_COMMAND} -E env
            "LUSDVIEW=$<TARGET_FILE:lusdview>"
            bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-vk-render.sh"
    )
  set_tests_properties(lusdview-vk-render PROPERTIES
                       SKIP_RETURN_CODE 77 TIMEOUT 300)

  if(LUSDR_PYTHON)
    add_test(NAME lusdview-core-mesh-materials
      COMMAND ${LUSDR_PYTHON}
              "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-core-mesh-materials.py"
              "$<TARGET_FILE:lusdview>"
              --output "${CMAKE_BINARY_DIR}/lusdview-core-mesh-materials")
    set_tests_properties(lusdview-core-mesh-materials PROPERTIES
                         SKIP_RETURN_CODE 77 TIMEOUT 600
                         LABELS "lusdview;vulkan;rt")
  endif()

  add_test(
    NAME lusdview-vk-stacked-glass
    COMMAND ${CMAKE_COMMAND} -E env
            "LUSDVIEW=$<TARGET_FILE:lusdview>"
            "ASSET=${PROJECT_SOURCE_DIR}/tests/usda/lusdview-stacked-glass.usda"
            bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-vk-render.sh"
  )
  set_tests_properties(lusdview-vk-stacked-glass PROPERTIES
                       SKIP_RETURN_CODE 77 TIMEOUT 300
                       LABELS "lusdview;vulkan;transparency")

  add_test(
    NAME lusdview-vk-swbvh-render
    COMMAND ${CMAKE_COMMAND} -E env
            "LUSDVIEW=$<TARGET_FILE:lusdview>"
            bash
            "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-vk-swbvh-render.sh"
    )
  set_tests_properties(lusdview-vk-swbvh-render PROPERTIES
                       SKIP_RETURN_CODE 77 TIMEOUT 180
                       LABELS "lusdview;vulkan;rt")

  add_test(
    NAME lusdview-vk-swbvh-stacked-glass
    COMMAND ${CMAKE_COMMAND} -E env
            "LUSDVIEW=$<TARGET_FILE:lusdview>"
            "ASSET=${PROJECT_SOURCE_DIR}/tests/usda/lusdview-stacked-glass.usda"
            bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-vk-swbvh-render.sh"
  )
  set_tests_properties(lusdview-vk-swbvh-stacked-glass PROPERTIES
                       SKIP_RETURN_CODE 77 TIMEOUT 180
                       LABELS "lusdview;vulkan;rt;transparency")

  add_test(
    NAME lusdview-vk-texture-validation
    COMMAND ${CMAKE_COMMAND} -E env
            "LUSDVIEW=$<TARGET_FILE:lusdview>"
            "LUSDVIEW_ROOT=${PROJECT_SOURCE_DIR}"
            bash
            "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-vk-texture-validation.sh"
    )
  set_tests_properties(lusdview-vk-texture-validation PROPERTIES
                       SKIP_RETURN_CODE 77 TIMEOUT 150
                       LABELS "lusdview;vulkan;validation")

  add_test(
    NAME lusdview-vk-mtlx-cold-fallback
    COMMAND ${CMAKE_COMMAND} -E env
            "LUSDVIEW=$<TARGET_FILE:lusdview>"
            bash
            "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-vk-mtlx-cold-fallback.sh"
  )
  set_tests_properties(lusdview-vk-mtlx-cold-fallback PROPERTIES
                       SKIP_RETURN_CODE 77 TIMEOUT 420
                       LABELS "lusdview;vulkan;rt;materialx")

  add_test(
    NAME lusdview-vk-authored-materials
    COMMAND ${CMAKE_COMMAND} -E env
            "LUSDVIEW=$<TARGET_FILE:lusdview>"
            bash
            "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-vk-authored-materials.sh"
    )
  set_tests_properties(lusdview-vk-authored-materials PROPERTIES
                       SKIP_RETURN_CODE 77 TIMEOUT 180
                       LABELS "lusdview;vulkan;rt;materialx;authored-assets")

  add_test(
    NAME lusdview-raster-multilight
    COMMAND bash
            "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-raster-multilight.sh"
            "$<TARGET_FILE:lusdview>" "${PROJECT_SOURCE_DIR}"
    )
  set_tests_properties(lusdview-raster-multilight PROPERTIES
                       SKIP_RETURN_CODE 77 TIMEOUT 180 LABELS "lusdview"
                       ENVIRONMENT "LUSDVIEW_GL_TIMEOUT=120s"
                       RESOURCE_LOCK lusdview_gl_gpu)

  add_test(
    NAME lusdview-raster-geometry-light
    COMMAND bash
            "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-raster-geometry-light.sh"
            "$<TARGET_FILE:lusdview>" "${PROJECT_SOURCE_DIR}"
    )
  set_tests_properties(lusdview-raster-geometry-light PROPERTIES
                       SKIP_RETURN_CODE 77 TIMEOUT 120
                       LABELS "lusdview;lighting"
                       RESOURCE_LOCK lusdview_gl_gpu)

  add_test(
    NAME lusdview-rt-light-links
    COMMAND bash
            "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-rt-light-links.sh"
            "$<TARGET_FILE:lusdview>" "${PROJECT_SOURCE_DIR}"
    )
  set_tests_properties(lusdview-rt-light-links PROPERTIES
                       SKIP_RETURN_CODE 77 TIMEOUT 150 LABELS "lusdview")

  add_test(
    NAME lusdview-raster-shadow-map
    COMMAND bash
            "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-raster-shadow-map.sh"
            "$<TARGET_FILE:lusdview>" "${PROJECT_SOURCE_DIR}"
    )
  set_tests_properties(lusdview-raster-shadow-map PROPERTIES
                       SKIP_RETURN_CODE 77 TIMEOUT 180 LABELS "lusdview")

  add_test(
    NAME lusdview-raster-carrier-shadow
    COMMAND bash
            "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-raster-carrier-shadow.sh"
            "$<TARGET_FILE:lusdview>" "${PROJECT_SOURCE_DIR}"
    )
  set_tests_properties(lusdview-raster-carrier-shadow PROPERTIES
                       SKIP_RETURN_CODE 77 TIMEOUT 180 LABELS "lusdview")

  add_test(
    NAME lusdview-shadow-alpha-inst
    COMMAND ${CMAKE_COMMAND} -E env
            "LUSDVIEW=$<TARGET_FILE:lusdview>"
            bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-shadow-alpha-inst.sh"
    )
  set_tests_properties(lusdview-shadow-alpha-inst PROPERTIES
                       SKIP_RETURN_CODE 77 TIMEOUT 60 LABELS "lusdview")

  add_test(
    NAME lusdview-aousd-conformance
    COMMAND ${CMAKE_COMMAND} -E env
            "LUSDVIEW=$<TARGET_FILE:lusdview>"
            bash
            "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-aousd-conformance.sh"
    )
  set_tests_properties(lusdview-aousd-conformance PROPERTIES
                       SKIP_RETURN_CODE 77 TIMEOUT 300 LABELS "lusdview")
  # Each fixture starts both viewers. ASan/LSan startup and shutdown costs
  # accumulate across the external suite; retain the normal-build deadline.
  if(SANITIZE_ADDRESS OR SANITIZE_UNDEFINED)
    set_tests_properties(lusdview-aousd-conformance PROPERTIES TIMEOUT 2700)
  endif()

  add_test(
    NAME lusdview-gl-vk-parity
    COMMAND ${CMAKE_COMMAND} -E env
            "LUSDVIEW=$<TARGET_FILE:lusdview>"
            # NVIDIA can need one additional cold shader-program compile for
            # a fixture variant not exercised by the global GL warm-up.
            "LUSDVIEW_GL_VK_TIMEOUT=120s"
            bash
            "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-gl-vk-parity.sh"
    )
  set_tests_properties(lusdview-gl-vk-parity PROPERTIES
                       SKIP_RETURN_CODE 77 TIMEOUT 120 LABELS "lusdview")

  add_test(
    NAME lusdview-backend-parity
    COMMAND ${CMAKE_COMMAND} -E env
            "LUSDVIEW=$<TARGET_FILE:lusdview>"
            "LUSDVIEW_PARITY_FRAMES=1" "LUSDVIEW_PARITY_SIZE=96x96"
            bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-backend-parity.sh"
    )
  set_tests_properties(lusdview-backend-parity PROPERTIES
                       SKIP_RETURN_CODE 77 TIMEOUT 180 LABELS "lusdview")

  add_test(
    NAME lusdview-unsupported-realtime-lobes
    COMMAND bash
            "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-unsupported-lobes.sh"
            "$<TARGET_FILE:lusdview>" "${PROJECT_SOURCE_DIR}"
    )
  set_tests_properties(lusdview-unsupported-realtime-lobes PROPERTIES
                       SKIP_RETURN_CODE 77 TIMEOUT 90 LABELS "lusdview")
endif()

#
# lusdview CPU/GPU skinning screenshot parity. This is intentionally
# registered only for Linux GUI builds with xvfb-run available: OpenGL needs a
# window/context, and xvfb supplies a headless X server.
#
# It needs a HARDWARE GL device to mean anything. Xvfb (and a forwarded
# DISPLAY) has no DRI, so Mesa falls back to llvmpipe, which fetches no vertex
# attribute but aPosition -- the skin joints/weights read as zero and the
# GPU-skinned render is stuck in the rest pose regardless of the code under
# test. The script detects that device and exits 77 (skip); see
# tests/lusdview/gpu_backend.py.
#
if(UNIX AND NOT APPLE AND TARGET lusdview)
  # HIP is loaded dynamically by lusdview.  CTest often runs with a minimal
  # loader environment, so discover the ROCm runtime directory at configure
  # time and propagate it to lusdview tests.  Preserve any caller-supplied
  # LD_LIBRARY_PATH after the discovered directory; this also supports
  # versioned installations such as /opt/rocm/core-7.14/lib.
  set(_lightusd_hip_library_dirs)
  foreach(_lightusd_rocm_root IN ITEMS
          "/opt/rocm" "$ENV{ROCM_PATH}" "$ENV{ROCM_HOME}")
    if(_lightusd_rocm_root AND IS_DIRECTORY "${_lightusd_rocm_root}")
      list(APPEND _lightusd_hip_library_dirs
           "${_lightusd_rocm_root}/lib")
      file(GLOB _lightusd_rocm_versioned_dirs LIST_DIRECTORIES true
           "${_lightusd_rocm_root}/core-*/lib")
      list(APPEND _lightusd_hip_library_dirs
           ${_lightusd_rocm_versioned_dirs})
    endif()
  endforeach()
  find_library(_lightusd_hip_runtime
               NAMES amdhip64 libamdhip64.so
               PATHS ${_lightusd_hip_library_dirs}
               NO_DEFAULT_PATH)
  if(_lightusd_hip_runtime)
    get_filename_component(_lightusd_hip_runtime_dir
                           "${_lightusd_hip_runtime}" DIRECTORY)
    if(DEFINED ENV{LD_LIBRARY_PATH} AND
       NOT "$ENV{LD_LIBRARY_PATH}" STREQUAL "")
      set(_lightusd_hip_ld_library_path
          "${_lightusd_hip_runtime_dir}:$ENV{LD_LIBRARY_PATH}")
    else()
      set(_lightusd_hip_ld_library_path
          "${_lightusd_hip_runtime_dir}")
    endif()
    message(STATUS "lightusd: HIP CTest runtime path: ${_lightusd_hip_runtime_dir}")
  else()
    set(_lightusd_hip_ld_library_path)
    message(STATUS "lightusd: HIP runtime not found; HIP CTest cases will self-skip")
  endif()

  find_program(LIGHTUSD_XVFB_RUN xvfb-run)
  if(LIGHTUSD_XVFB_RUN)
    add_test(
      NAME lusdview-skinning-screenshot-diff
      COMMAND ${LUSDR_PYTHON} ${PROJECT_SOURCE_DIR}/tests/lusdview/compare-skinning-screenshots.py
              --app "$<TARGET_FILE:lusdview>"
              --model "${PROJECT_SOURCE_DIR}/models/skintest-animated.usda"
              --out-dir "${CMAKE_BINARY_DIR}/Testing/lusdview-skinning"
              --backend gl
              --frames 4
              --time 12
              --xvfb-run "${LIGHTUSD_XVFB_RUN}"
      )
    set_tests_properties(lusdview-skinning-screenshot-diff
                         PROPERTIES SKIP_RETURN_CODE 77)

    # The HIP interactive tracer refits its BVH per pose instead of paying a
    # full scene rebuild. Refit must trace byte-identically to the rebuild
    # path (LUSDVIEW_NO_BVH_REFIT=1) over the same --play run. Self-skips
    # without a HIP device.
    add_test(
      NAME lusdview-hip-bvh-refit
      COMMAND ${LUSDR_PYTHON}
              ${PROJECT_SOURCE_DIR}/tests/lusdview/check-hip-bvh-refit.py
              "${LIGHTUSD_XVFB_RUN}"
              "$<TARGET_FILE:lusdview>"
              "${PROJECT_SOURCE_DIR}/models/skintest-animated.usda"
              "${CMAKE_BINARY_DIR}/Testing/lusdview-hip-bvh-refit"
      )
    set_tests_properties(lusdview-hip-bvh-refit
                         PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 1800)
    # Skinning must also reach INSTANCED prototypes (a shared prototype behind
    # `instanceable = true`), which the core loader used to convert but never
    # skin -- every instance rendered the rest pose. Asserts the rig actually
    # animates inside the instance; CPU/GPU are not pixel-compared here (a
    # GPU-skinned prototype is de-instanced onto the material shader).
    add_test(
      NAME lusdview-instanced-prototype-skinning
      COMMAND ${LUSDR_PYTHON}
              ${PROJECT_SOURCE_DIR}/tests/lusdview/check-instanced-prototype-skinning.py
              "$<TARGET_FILE:lusdview>"
              "${PROJECT_SOURCE_DIR}/models/skintest-instanced.usda"
              "${CMAKE_BINARY_DIR}/Testing/lusdview-instanced-skinning"
      )
    # It exits 77 when no usable GL display is present (it renders WINDOWED, to
    # exercise the real context). Without this the skip is reported as a failure.
    set_tests_properties(lusdview-instanced-prototype-skinning
                         PROPERTIES SKIP_RETURN_CODE 77)

    # The large-scene budget tree must descend from the real device, not the
    # GiB(32)/GiB(16) literals it used to be fed, and --vram-budget must
    # override the probe. Headless + tiny scene; skips without Vulkan.
    add_test(
      NAME lusdview-vram-budget
      COMMAND ${LUSDR_PYTHON}
              ${PROJECT_SOURCE_DIR}/tests/lusdview/check-vram-budget.py
              "$<TARGET_FILE:lusdview>"
              "${CMAKE_BINARY_DIR}/Testing/lusdview-vram-budget"
      )
    set_tests_properties(lusdview-vram-budget PROPERTIES SKIP_RETURN_CODE 77)

    # --texture-fit must actually decide whether textures are shrunk and
    # block-compressed. That decision previously had no coverage at all: a
    # regression either way showed up only as a slow or needlessly degraded
    # large-scene load. Headless + tiny scene; skips without Vulkan.
    add_test(
      NAME lusdview-texture-fit
      COMMAND ${LUSDR_PYTHON}
              ${PROJECT_SOURCE_DIR}/tests/lusdview/check-texture-fit.py
              "$<TARGET_FILE:lusdview>"
              "${CMAKE_BINARY_DIR}/Testing/lusdview-texture-fit"
      )
    set_tests_properties(lusdview-texture-fit PROPERTIES SKIP_RETURN_CODE 77)

    # A non-instanced blendshape mesh must animate on the default --core loader.
    # Morph used to be built only for instanced prototypes, so a plain
    # SkelRoot -> Mesh blendshape rendered frozen at its rest pose. Headless
    # (Vulkan); skips without a device.
    add_test(
      NAME lusdview-noninstanced-blendshape
      COMMAND ${LUSDR_PYTHON}
              ${PROJECT_SOURCE_DIR}/tests/lusdview/check-noninstanced-blendshape.py
              "$<TARGET_FILE:lusdview>"
              "${CMAKE_BINARY_DIR}/Testing/lusdview-noninstanced-blendshape"
      )
    set_tests_properties(lusdview-noninstanced-blendshape
                         PROPERTIES SKIP_RETURN_CODE 77)

    # Per-vertex displayColor must render on the Vulkan raster backend (it used
    # to reach only the RT path; VK raster drew the flat material color).
    add_test(
      NAME lusdview-vertex-color
      COMMAND ${LUSDR_PYTHON}
              ${PROJECT_SOURCE_DIR}/tests/lusdview/check-vertex-color.py
              "$<TARGET_FILE:lusdview>"
              "${CMAKE_BINARY_DIR}/Testing/lusdview-vertex-color"
      )
    set_tests_properties(lusdview-vertex-color PROPERTIES SKIP_RETURN_CODE 77)

    # Vulkan RT BLAS compaction: must shrink the acceleration structures, must
    # build ONE BLAS per prototype (not per instance), and must be image-neutral.
    add_test(
      NAME lusdview-blas-compaction
      COMMAND ${LUSDR_PYTHON}
              ${PROJECT_SOURCE_DIR}/tests/lusdview/check-blas-compaction.py
              "$<TARGET_FILE:lusdview>"
              "${PROJECT_SOURCE_DIR}/models/blastest-instanced-static.usda"
              "${CMAKE_BINARY_DIR}/Testing/lusdview-blas-compaction"
      )
    set_tests_properties(lusdview-blas-compaction PROPERTIES
                         SKIP_RETURN_CODE 77 TIMEOUT 300)

    # A texture must sample the UV set its UsdPrimvarReader names. The secondary
    # set used to be hard-coded as <primary>+"1" (so `uvSet1` was never
    # extracted), and nothing routed it even when it existed.
    add_test(
      NAME lusdview-uv-set-routing
      COMMAND ${LUSDR_PYTHON}
              ${PROJECT_SOURCE_DIR}/tests/lusdview/check-uv-set-routing.py
              "$<TARGET_FILE:lusdview>"
              "${PROJECT_SOURCE_DIR}/models/multi-uv-quad.usda"
              "${CMAKE_BINARY_DIR}/Testing/lusdview-uv-set-routing"
      )
    set_tests_properties(lusdview-uv-set-routing PROPERTIES SKIP_RETURN_CODE 77)

    # Cross-tool parity oracle: ONE scene through BOTH binaries. lusdview and
    # lusdrender consume the same Tydra RenderScene, so they must read
    # GeomSubset material bindings and the secondary UV set the same way.
    # Nothing else in the suite compares the two -- which is precisely how a run
    # of lusdview-only material/UV regressions stayed invisible while every
    # per-tool test kept passing. Needs a real Vulkan device (lusdview
    # --headless); skips without one.
    if(TARGET lusdrender)
      add_test(
        NAME lusdview-lusdrender-parity
        COMMAND bash
                ${PROJECT_SOURCE_DIR}/tools/lusdrender/tests/run-cross-tool-parity.sh
        )
      set_tests_properties(lusdview-lusdrender-parity PROPERTIES
        SKIP_RETURN_CODE 77
        ENVIRONMENT
          "LUSDVIEW=$<TARGET_FILE:lusdview>;LUSDRENDER=$<TARGET_FILE:lusdrender>")
    endif()

    # Raster LOD must reach NON-instanced meshes, which also requires each one
    # to carry its own AABB (the core loader used to hand every static mesh the
    # scene-spanning box, disabling the per-mesh frustum cull along with it).
    add_test(
      NAME lusdview-noninstanced-lod
      COMMAND ${LUSDR_PYTHON}
              ${PROJECT_SOURCE_DIR}/tests/lusdview/check-noninst-lod.py
              "$<TARGET_FILE:lusdview>"
              "${CMAKE_BINARY_DIR}/Testing/lusdview-noninstanced-lod"
      )
    set_tests_properties(lusdview-noninstanced-lod PROPERTIES SKIP_RETURN_CODE 77)
    add_test(
      NAME lusdview-wire-depth-fill
      COMMAND ${LUSDR_PYTHON}
              ${PROJECT_SOURCE_DIR}/tests/lusdview/check-wire-depth-fill.py
              "$<TARGET_FILE:lusdview>"
              "${CMAKE_BINARY_DIR}/Testing/lusdview-wire-depth-fill")
    set_tests_properties(lusdview-wire-depth-fill PROPERTIES
      SKIP_RETURN_CODE 77 TIMEOUT 200 LABELS "lusdview")


    # The ray tracer traces vertex buffers, so the raster vertex shader's
    # deform never reaches it. --core --rt used to give up and re-run the whole
    # converter per time code; it now re-poses the retained rest vertices.
    add_test(
      NAME lusdview-rt-skinning
      COMMAND ${LUSDR_PYTHON}
              ${PROJECT_SOURCE_DIR}/tests/lusdview/check-rt-skinning.py
              "$<TARGET_FILE:lusdview>"
              "${PROJECT_SOURCE_DIR}/models/skintest-animated.usda"
              "${CMAKE_BINARY_DIR}/Testing/lusdview-rt-skinning"
      )
    set_tests_properties(lusdview-rt-skinning PROPERTIES
                         SKIP_RETURN_CODE 77 TIMEOUT 600)

    # A skinned mesh's BLAS is refit in place per pose (MODE_UPDATE) instead
    # of destroyed + fully rebuilt. Refit must trace byte-identically to the
    # rebuild path (LUSDVIEW_NO_BLAS_REFIT=1) over the same --play run.
    add_test(
      NAME lusdview-rt-blas-refit
      COMMAND ${LUSDR_PYTHON}
              ${PROJECT_SOURCE_DIR}/tests/lusdview/check-rt-blas-refit.py
              "$<TARGET_FILE:lusdview>"
              "${PROJECT_SOURCE_DIR}/models/skintest-animated.usda"
              "${CMAKE_BINARY_DIR}/Testing/lusdview-rt-blas-refit"
      )
    set_tests_properties(lusdview-rt-blas-refit PROPERTIES
                         SKIP_RETURN_CODE 77 TIMEOUT 420)

    # A non-instanced blendshaped mesh must morph: the static batch path used to
    # build no morph channels at all, so it rendered its rest shape forever.
    add_test(
      NAME lusdview-blendshape-morph
      COMMAND ${LUSDR_PYTHON}
              ${PROJECT_SOURCE_DIR}/tests/lusdview/check-blendshape-morph.py
              "$<TARGET_FILE:lusdview>"
              "${PROJECT_SOURCE_DIR}/models/blendshape-and-animation-test-001.usda"
              "${CMAKE_BINARY_DIR}/Testing/lusdview-blendshape-morph"
      )
    set_tests_properties(lusdview-blendshape-morph PROPERTIES
                         SKIP_RETURN_CODE 77 TIMEOUT 600)

    # GPU deform vs CPU bake, in depth through a fixed camera, over the three
    # deform paths that had no fixture at all. Each of these caught a live bug:
    #   skin-xform     -- skinning under a rotated/scaled parent (the bone rows
    #                     must undo the static path's world bake and re-apply it);
    #   morph-subsets  -- a morphed mesh torn into two batches by material-bind
    #                     GeomSubsets, which re-index the sparse delta lists;
    #   morph-skin     -- both deforms live at once, which pins the ORDER (morph
    #                     the bind pose, then skin the morphed points);
    #   instanced-proto -- the same deform reached through NATIVE INSTANCING,
    #                     which is a different emitter (EmitInstancedProto) whose
    #                     vertices stay prototype-LOCAL. It was called exempt from
    #                     the world-bake bugs by construction, and it was -- but
    #                     the scene box then grew from those local vertices without
    #                     the instance placement, so the grid and the depth ramp
    #                     landed in the wrong place.
    #   pointinstancer -- the same prototype placed by a PointInstancer, whose
    #                     placements come from positions/orientations/scales arrays
    #                     rather than from prims referencing it. It caught the
    #                     orientation quaternion being read imaginary-first when the
    #                     core stage stores it real-first: a 30-degree Z rotation
    #                     decoded as a 150-degree X rotation, i.e. upside-down.
    foreach(deform_case skin-xform skin-animation morph-subsets morph-skin instanced-proto
                        pointinstancer)
      add_test(
        NAME lusdview-deform-${deform_case}
        COMMAND ${LUSDR_PYTHON}
                ${PROJECT_SOURCE_DIR}/tests/lusdview/check-deform-parity.py
                "$<TARGET_FILE:lusdview>"
                "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/deform-${deform_case}.usda"
                "${CMAKE_BINARY_DIR}/Testing/lusdview-deform-${deform_case}"
        )
      set_tests_properties(lusdview-deform-${deform_case}
                           PROPERTIES SKIP_RETURN_CODE 77)
    endforeach()
    set_tests_properties(lusdview-deform-instanced-proto
                         PROPERTIES LABELS "lusdview")

    # An instanced prototype under the RAY TRACER: RT cannot use the raster vertex
    # shader's deform (the BLAS is built from vertex buffers, so the geometry itself
    # has to move), and for a prototype those re-posed vertices are prototype-LOCAL
    # -- one BLAS, re-posed once, referenced by every placement's TLAS instance.
    # Raster and RT therefore reach the same pixels by genuinely different routes.
    add_test(
      NAME lusdview-deform-instanced-proto-rt
      COMMAND ${LUSDR_PYTHON}
              ${PROJECT_SOURCE_DIR}/tests/lusdview/check-deform-parity.py
              "$<TARGET_FILE:lusdview>"
              "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/deform-instanced-proto.usda"
              "${CMAKE_BINARY_DIR}/Testing/lusdview-deform-instanced-proto-rt"
              rt
      )
    set_tests_properties(lusdview-deform-instanced-proto-rt
                         PROPERTIES SKIP_RETURN_CODE 77 LABELS "lusdview")

    # The CUDA tracer builds its BVH from draw_ geometry, so it takes the deform
    # through poseCoreDrawForTracer instead of the vertex shader. It used to be
    # pinned to the load-time CPU bake -- a second deform implementation, free to
    # disagree with the shader's, and it did. Skips (77) where CUDA is absent.
    add_test(
      NAME lusdview-deform-cuda
      COMMAND ${LUSDR_PYTHON}
              ${PROJECT_SOURCE_DIR}/tests/lusdview/check-deform-parity.py
              "$<TARGET_FILE:lusdview>"
              "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/deform-morph-skin.usda"
              "${CMAKE_BINARY_DIR}/Testing/lusdview-deform-cuda"
              cuda
      )
    set_tests_properties(lusdview-deform-cuda PROPERTIES SKIP_RETURN_CODE 77)

    add_test(
      NAME lusdview-cuda-kernel-cache
      COMMAND ${CMAKE_COMMAND} -E env
              "LUSDVIEW=$<TARGET_FILE:lusdview>"
              bash
              "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-cuda-kernel-cache.sh"
      )
    set_tests_properties(lusdview-cuda-kernel-cache
                         PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 210)

    add_test(
      NAME lusdview-cuda-cache-cli
      COMMAND ${CMAKE_COMMAND} -E env
              "LUSDVIEW=$<TARGET_FILE:lusdview>"
              bash
              "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-cuda-cache-cli.sh"
      )
  else()
    message(STATUS "lightusd: skipping lusdview screenshot diff test; xvfb-run not found")
  endif()
endif()

#
# UsdShade material-binding resolution: purpose-scoped (material:binding:preview
# / :full) bindings authored on an ANCESTOR, as production scenes (Scene A) bind.
# Reading only the Mesh's own `material:binding` drops the material and every
# texture with it. Renders headless (Vulkan offscreen); SKIPs without a GPU.
#
if(UNIX AND NOT APPLE AND TARGET lusdview)
  add_test(
    NAME lusdview-material-binding-inheritance
    COMMAND ${CMAKE_COMMAND} -E env
            "LUSDVIEW=$<TARGET_FILE:lusdview>"
            bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-material-binding-inheritance.sh"
    )
  set_tests_properties(lusdview-material-binding-inheritance
                       PROPERTIES SKIP_RETURN_CODE 77)

  # Unsupported bound surface shaders retain familiar PBR constants in a
  # per-material degraded PreviewSurface. Geometry must remain renderable and
  # the structured load summary must report the degradation.
  add_test(
    NAME lusdview-degraded-material
    COMMAND ${CMAKE_COMMAND} -E env
            "LUSDVIEW=$<TARGET_FILE:lusdview>"
            bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-degraded-material.sh"
    )
  set_tests_properties(lusdview-degraded-material
                       PROPERTIES SKIP_RETURN_CODE 77)

  # Deterministic screenshot controls must reject malformed/ambiguous camera
  # input before renderer initialization, so this remains a headless test.
  add_test(
    NAME lusdview-cli-camera-options
    COMMAND bash
            "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-cli-camera-options.sh"
            "$<TARGET_FILE:lusdview>"
    )

  add_test(
    NAME lusdview-cli-path-trace-options
    COMMAND bash
            "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-cli-path-trace-options.sh"
            "$<TARGET_FILE:lusdview>"
    )
  set_tests_properties(lusdview-cli-path-trace-options PROPERTIES
                       LABELS "lusdview;rt;path-trace")

  add_test(
    NAME lusdview-cli-window-size
    COMMAND bash
            "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-cli-window-size.sh"
            "$<TARGET_FILE:lusdview>"
    )
  set_tests_properties(lusdview-cli-window-size
                       PROPERTIES SKIP_RETURN_CODE 77 LABELS "lusdview")

  add_test(
    NAME lusdview-camera-record-equivalence
    COMMAND bash
            "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-camera-record-equivalence.sh"
            "$<TARGET_FILE:lusdview>" "${PROJECT_SOURCE_DIR}"
  )
    set_tests_properties(lusdview-camera-record-equivalence
                       PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300
                                  LABELS "lusdview")

  add_test(
    NAME lusdview-camera-clipping-planes
    COMMAND bash
            "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-camera-clipping-planes.sh"
            "$<TARGET_FILE:lusdview>" "${PROJECT_SOURCE_DIR}"
  )
  set_tests_properties(lusdview-camera-clipping-planes
                       PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180
                                  LABELS "lusdview;raster")

  add_test(
    NAME lusdview-value-clip-carrier
    COMMAND bash
            "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-value-clip-carrier.sh"
            "$<TARGET_FILE:lusdview>" "${PROJECT_SOURCE_DIR}"
  )
  set_tests_properties(lusdview-value-clip-carrier
                       PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120
                                  LABELS "lusdview;animation")

  add_test(
    NAME lusdview-camera-dof-raster
    COMMAND bash
            "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-camera-dof-raster.sh"
            "$<TARGET_FILE:lusdview>" "${PROJECT_SOURCE_DIR}"
  )
  set_tests_properties(lusdview-camera-dof-raster
                       PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180
                                  LABELS "lusdview;raster")

  add_test(
    NAME lusdview-camera-shutter-contract
    COMMAND bash
            "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-camera-shutter-contract.sh"
            "$<TARGET_FILE:lusdview>" "${PROJECT_SOURCE_DIR}"
  )
  set_tests_properties(lusdview-camera-shutter-contract
                       PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120
                                  LABELS "lusdview;camera")

  add_test(
    NAME lusdview-camera-motion-cpu
    COMMAND bash
            "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-camera-motion-cpu.sh"
            "$<TARGET_FILE:lusdview>" "${PROJECT_SOURCE_DIR}"
  )
  set_tests_properties(lusdview-camera-motion-cpu
                       PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180
                                  LABELS "lusdview;camera;cpu-rt")

  add_test(
    NAME lusdview-camera-motion-cuda
    COMMAND bash
            "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-camera-motion-cuda.sh"
            "$<TARGET_FILE:lusdview>" "${PROJECT_SOURCE_DIR}"
  )
  set_tests_properties(lusdview-camera-motion-cuda
                       PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300
                                  LABELS "lusdview;camera;cuda")

  add_test(
    NAME lusdview-camera-motion-hip
    COMMAND bash
            "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-camera-motion-hip.sh"
            "$<TARGET_FILE:lusdview>" "${PROJECT_SOURCE_DIR}"
  )
  set_tests_properties(lusdview-camera-motion-hip
                       PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300
                                  LABELS "lusdview;camera;hip")

  add_test(
    NAME lusdview-camera-stereo-contract
    COMMAND bash
            "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-camera-stereo-contract.sh"
            "$<TARGET_FILE:lusdview>" "${PROJECT_SOURCE_DIR}"
  )
  set_tests_properties(lusdview-camera-stereo-contract
                       PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180
                                  LABELS "lusdview;camera;stereo")

  add_test(
    NAME lusdview-camera-motion-raster-export
    COMMAND bash
            "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-camera-motion-raster-export.sh"
            "$<TARGET_FILE:lusdview>" "${PROJECT_SOURCE_DIR}"
  )
  set_tests_properties(lusdview-camera-motion-raster-export
                       PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 240
                                  LABELS "lusdview;camera;raster")

  add_test(
    NAME lusdview-camera-motion-raster-static
    COMMAND bash
            "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-camera-motion-raster-export.sh"
            "$<TARGET_FILE:lusdview>" "${PROJECT_SOURCE_DIR}" vk static
  )
  set_tests_properties(lusdview-camera-motion-raster-static
                       PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 240
                                  LABELS "lusdview;camera;raster;static")

  add_test(
    NAME lusdview-camera-motion-vulkan
    COMMAND bash
            "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-camera-motion-vulkan.sh"
            "$<TARGET_FILE:lusdview>" "${PROJECT_SOURCE_DIR}"
  )
  set_tests_properties(lusdview-camera-motion-vulkan
                       PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300
                                  LABELS "lusdview;camera;vulkan;rt")

  if(LIGHTUSD_XVFB_RUN)
    add_test(
      NAME lusdview-camera-motion-raster-gl
      COMMAND "${LIGHTUSD_XVFB_RUN}" -a bash
              "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-camera-motion-raster-export.sh"
              "$<TARGET_FILE:lusdview>" "${PROJECT_SOURCE_DIR}" gl
    )
    set_tests_properties(lusdview-camera-motion-raster-gl
                         PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 240
                                    LABELS "lusdview;camera;raster;gl")
  endif()

  add_test(
    NAME lusdview-camera-dof-cuda
    COMMAND bash
            "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-camera-dof-cuda.sh"
            "$<TARGET_FILE:lusdview>" "${PROJECT_SOURCE_DIR}"
  )
  set_tests_properties(lusdview-camera-dof-cuda
                       PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180
                                  LABELS "lusdview")

  add_test(
    NAME lusdview-camera-dof-vulkan
    COMMAND bash
            "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-camera-dof-vulkan.sh"
            "$<TARGET_FILE:lusdview>" "${PROJECT_SOURCE_DIR}"
  )
  set_tests_properties(lusdview-camera-dof-vulkan
                       PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180
                                  LABELS "lusdview")

  add_test(
    NAME lusdview-area-light-cuda
    COMMAND bash
            "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-area-light-cuda.sh"
            "$<TARGET_FILE:lusdview>" "${PROJECT_SOURCE_DIR}"
  )
  set_tests_properties(lusdview-area-light-cuda
                       PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180
                                  LABELS "lusdview")

  add_test(
    NAME lusdview-area-light-vulkan
    COMMAND bash
            "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-area-light-vulkan.sh"
            "$<TARGET_FILE:lusdview>" "${PROJECT_SOURCE_DIR}"
  )
  set_tests_properties(lusdview-area-light-vulkan
                       PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180
                                  LABELS "lusdview")

  add_test(
    NAME lusdview-core-nonmesh-extraction
    COMMAND bash
            "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-core-nonmesh-extraction.sh"
            "$<TARGET_FILE:lusdview>" "${PROJECT_SOURCE_DIR}"
    )
  set_tests_properties(lusdview-core-nonmesh-extraction
                       PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 75
                                  LABELS "lusdview")

  if(UNIX AND NOT APPLE)
    add_test(
      NAME lusdview-gl-nonmesh-render
      COMMAND bash
              "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-gl-nonmesh-render.sh"
              "$<TARGET_FILE:lusdview>" "${PROJECT_SOURCE_DIR}"
    )
    set_tests_properties(lusdview-gl-nonmesh-render
                         PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 75
                                    LABELS "lusdview")

    add_test(
      NAME lusdview-vk-nonmesh-render
      COMMAND bash
              "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-vk-nonmesh-render.sh"
              "$<TARGET_FILE:lusdview>" "${PROJECT_SOURCE_DIR}"
    )
    set_tests_properties(lusdview-vk-nonmesh-render
                         PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 90
                                    LABELS "lusdview")

    add_test(
      NAME lusdview-vk-gaussian-chunks
      COMMAND bash
              "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-vk-gaussian-chunks.sh"
              "$<TARGET_FILE:lusdview>" "${PROJECT_SOURCE_DIR}"
    )
    set_tests_properties(lusdview-vk-gaussian-chunks
                         PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 105
                                    LABELS "lusdview")

    add_test(
      NAME lusdview-nonmesh-backend-parity
      COMMAND bash
              "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-nonmesh-backend-parity.sh"
              "$<TARGET_FILE:lusdview>" "${PROJECT_SOURCE_DIR}"
    )
    set_tests_properties(lusdview-nonmesh-backend-parity
                         PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 150
                                    LABELS "lusdview")
  endif()

  if(LUSDR_PYTHON)
    # Start Vulkan once, then replace the USD scene and capture three cases
    # through MCP. Process-wide backend/loader variants remain separate tests.
    add_test(
      NAME lusdview-mcp-render-batch
      COMMAND ${LUSDR_PYTHON}
              "${PROJECT_SOURCE_DIR}/tests/lusdview/mcp_render_batch.py"
              "$<TARGET_FILE:lusdview>"
              "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/mcp-render-batch.json"
              "${CMAKE_BINARY_DIR}/Testing/lusdview-mcp-render-batch"
      )
    set_tests_properties(lusdview-mcp-render-batch
                         PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
    add_test(
      NAME lusdview-mcp-material-batch
      COMMAND ${LUSDR_PYTHON}
              "${PROJECT_SOURCE_DIR}/tests/lusdview/mcp_render_batch.py"
              "$<TARGET_FILE:lusdview>"
              "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/mcp-material-batch.json"
              "${CMAKE_BINARY_DIR}/Testing/lusdview-mcp-material-batch"
      )
    set_tests_properties(lusdview-mcp-material-batch
                         PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
    # The grid/no-grid image assertion also pins the RT compute-to-overlay
    # composition path without restarting Vulkan or rebuilding the TLAS.
    add_test(
      NAME lusdview-mcp-render-batch-vk-rt
      COMMAND ${LUSDR_PYTHON}
              "${PROJECT_SOURCE_DIR}/tests/lusdview/mcp_render_batch.py"
              "$<TARGET_FILE:lusdview>"
              "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/mcp-vk-rt-overlay.json"
              "${CMAKE_BINARY_DIR}/Testing/lusdview-mcp-render-batch-vk-rt"
              --rt
      )
    set_tests_properties(lusdview-mcp-render-batch-vk-rt
                         PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180)
    add_test(
      NAME lusdview-mcp-live-reload-vulkan
      COMMAND ${LUSDR_PYTHON}
              "${PROJECT_SOURCE_DIR}/tests/lusdview/mcp_shader_reload.py"
              "$<TARGET_FILE:lusdview>" vulkan
              "${PROJECT_SOURCE_DIR}/examples/lusdview/vk/shaders/raytrace.comp"
              "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/area-light-shadow.usda"
              --materialx-vk-shader-max-kib 256
      )
    set_tests_properties(lusdview-mcp-live-reload-vulkan
                         PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 360
                                    LABELS "lusdview;rt;mcp;live-reload;vulkan")
    add_test(
      NAME lusdview-mcp-live-reload-vulkan-swbvh
      COMMAND ${CMAKE_COMMAND} -E env LUSDVIEW_RT_FORCE_SW=1
              ${LUSDR_PYTHON}
              "${PROJECT_SOURCE_DIR}/tests/lusdview/mcp_shader_reload.py"
              "$<TARGET_FILE:lusdview>" vulkan
              "${PROJECT_SOURCE_DIR}/examples/lusdview/vk/shaders/raytrace_swbvh.comp"
              "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/area-light-shadow.usda"
              --async-smoke
      )
    set_tests_properties(lusdview-mcp-live-reload-vulkan-swbvh
                         PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 240
                                    LABELS "lusdview;rt;mcp;live-reload;vulkan;swbvh")
    add_test(
      NAME lusdview-mcp-live-reload-cuda
      COMMAND ${LUSDR_PYTHON}
              "${PROJECT_SOURCE_DIR}/tests/lusdview/mcp_shader_reload.py"
              "$<TARGET_FILE:lusdview>" cuda
              "${PROJECT_SOURCE_DIR}/examples/lusdview/raytracer_kernel_src.txt"
              "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/area-light-shadow.usda"
      )
    set_tests_properties(lusdview-mcp-live-reload-cuda
                         PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 480
                                    LABELS "lusdview;rt;mcp;live-reload;cuda")
    add_test(
      NAME lusdview-mcp-live-reload-hip
      COMMAND ${LUSDR_PYTHON}
              "${PROJECT_SOURCE_DIR}/tests/lusdview/mcp_shader_reload.py"
              "$<TARGET_FILE:lusdview>" hip
              "${PROJECT_SOURCE_DIR}/examples/lusdview/raytracer_kernel_src.txt"
              "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/area-light-shadow.usda"
      )
    set_tests_properties(lusdview-mcp-live-reload-hip
                         PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 360
                                    LABELS "lusdview;rt;mcp;live-reload;hip")
    find_program(LUSDVIEW_XVFB_RUN NAMES xvfb-run)
    if(LUSDVIEW_XVFB_RUN)
      # Same manifest through one real, persistent GLFW/OpenGL window. The
      # trailing backend overrides the manifest's Vulkan default.
      add_test(
        NAME lusdview-mcp-render-batch-gl-window
        COMMAND "${LUSDVIEW_XVFB_RUN}" -a
                ${LUSDR_PYTHON}
                "${PROJECT_SOURCE_DIR}/tests/lusdview/mcp_render_batch.py"
                "$<TARGET_FILE:lusdview>"
                "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/mcp-render-batch.json"
                "${CMAKE_BINARY_DIR}/Testing/lusdview-mcp-render-batch-gl"
                --windowed --backend gl --size 1024x768
        )
      set_tests_properties(lusdview-mcp-render-batch-gl-window
                           PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
      add_test(
        NAME lusdview-mcp-material-batch-gl-window
        COMMAND "${LUSDVIEW_XVFB_RUN}" -a
                ${LUSDR_PYTHON}
                "${PROJECT_SOURCE_DIR}/tests/lusdview/mcp_render_batch.py"
                "$<TARGET_FILE:lusdview>"
                "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/mcp-material-batch.json"
                "${CMAKE_BINARY_DIR}/Testing/lusdview-mcp-material-batch-gl"
                --windowed --backend gl --size 1024x768
        )
      set_tests_properties(lusdview-mcp-material-batch-gl-window
                           PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
    endif()
  endif()

  # Exact material:binding:back resolution and draw-side selection, including
  # a GeomSubset-specific degraded back material and whole-mesh fallback.
  add_test(
    NAME lusdview-backface-material
    COMMAND ${CMAKE_COMMAND} -E env
            "LUSDVIEW=$<TARGET_FILE:lusdview>"
            bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-backface-material.sh"
    )
  set_tests_properties(lusdview-backface-material
                       PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300
                                  ENVIRONMENT "LUSDVIEW_XVFB=1")

  # doubleSided (audit T8): a single-sided mesh back-face culls, a doubleSided
  # one does not -- identically on GL and Vulkan. The --core loader used to
  # drop the authored flag entirely, and Vulkan never culled anything.
  add_test(
    NAME lusdview-double-sided
    COMMAND ${CMAKE_COMMAND} -E env
            "LUSDVIEW=$<TARGET_FILE:lusdview>"
            bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-double-sided.sh"
    )
  set_tests_properties(lusdview-double-sided PROPERTIES SKIP_RETURN_CODE 77)

  # UsdPreviewSurface specular workflow + IOR on --core (audit T12): a colored
  # specularColor must tint the highlight (not fall back to white dielectric),
  # identically on GL and VK.
  add_test(
    NAME lusdview-specular-workflow
    COMMAND ${CMAKE_COMMAND} -E env
            "LUSDVIEW=$<TARGET_FILE:lusdview>"
            bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-specular-workflow.sh"
    )
  set_tests_properties(lusdview-specular-workflow PROPERTIES SKIP_RETURN_CODE 77)

  # T12 opacity completion: separate scalar opacity textures (ordinary/UDIM)
  # on both raster backends, and varying displayOpacity on raster + RT.
  add_test(
    NAME lusdview-opacity-material
    COMMAND ${CMAKE_COMMAND} -E env
            "LUSDVIEW=$<TARGET_FILE:lusdview>"
            bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-opacity-material.sh"
    )
  set_tests_properties(lusdview-opacity-material PROPERTIES
                       SKIP_RETURN_CODE 77 TIMEOUT 900)

  # Texture-driven material factors must survive the complete raster / RT
  # packing chain. Split the backend matrix so each CTest case reports useful
  # progress and a slow backend cannot hide the others behind one silent
  # aggregate test.
  set(_lightusd_semantic_backends gl vk-raster vk-rt cuda hip)
  foreach(_backend IN LISTS _lightusd_semantic_backends)
    set(_test_name "lusdview-texture-semantic-aov-${_backend}")
    set(_semantic_timeout_env)
    if(_backend STREQUAL "gl")
      # A fixture can select an additional NVIDIA shader variant that was
      # not covered by the global warm-up. Under concurrent suite load its
      # first compile can exceed the normal 60 s launch guard.
      set(_semantic_timeout_env "LUSDVIEW_RENDER_TIMEOUT=180s")
    elseif(_backend STREQUAL "vk-rt")
      # A new compact RT SPIR-V can take roughly 150 s for NVIDIA's first
      # driver JIT.  Let that first process populate lusdview's persistent
      # VkPipelineCache; warm launches are typically below 200 ms.
      set(_semantic_timeout_env "LUSDVIEW_RENDER_TIMEOUT=240s")
    endif()
    add_test(
      NAME "${_test_name}"
      COMMAND ${CMAKE_COMMAND} -E env
              "LUSDVIEW=$<TARGET_FILE:lusdview>"
              "LUSDVIEW_SEMANTIC_BACKENDS=${_backend}"
              # The package-parity shards below own UDIM/USDZ and archive
              # coverage. Keep the main shards focused on every OpenPBR AOV;
              # this removes duplicate process/device startup and bounds each
              # hardware backend to roughly 13 viewer launches.
              "LUSDVIEW_SEMANTIC_FAMILIES=openpbr"
              "LUSDVIEW_SEMANTIC_UDIM=0"
              "LUSDVIEW_SEMANTIC_PACKAGE_PARITY=0"
              ${_semantic_timeout_env}
              bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-texture-semantic-aov.sh"
      )
    # 300 s was enough only while a failed backend probe let most cases be
    # skipped; a full GL run over every mode and family measures ~436 s.
    set_tests_properties("${_test_name}" PROPERTIES
                         SKIP_RETURN_CODE 77 TIMEOUT 1800 LABELS "lusdview"
                         RESOURCE_LOCK lusdview_semantic_gpu)
  endforeach()

  # Explicitly exercise the GL normal-map TBN fallback with constant UVs.
  # Keep this as a small GL-only shard so the full CUDA/HIP semantic matrices
  # do not repeat a regression case that targets the raster shader branch.
  add_test(
    NAME lusdview-texture-semantic-aov-gl-degenerate
    COMMAND ${CMAKE_COMMAND} -E env
            "LUSDVIEW=$<TARGET_FILE:lusdview>"
            "LUSDVIEW_SEMANTIC_BACKENDS=gl"
            "LUSDVIEW_SEMANTIC_MODES=degenerate-normal degenerate-coat-normal"
            bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-texture-semantic-aov.sh"
  )
  set_tests_properties(lusdview-texture-semantic-aov-gl-degenerate PROPERTIES
                       SKIP_RETURN_CODE 77 TIMEOUT 300 LABELS "lusdview"
                       RESOURCE_LOCK lusdview_semantic_gpu)

  # Check the RT texture-table archive path without repeating the full
  # semantic matrix above. One textured OpenPBR albedo case still exercises
  # external, UDIM, and USDZ resolution through the core loader and compares the
  # resulting images, while reducing this shard from ~162 viewer launches to
  # three.
  set(_lightusd_semantic_rt_backends vk-rt cuda hip)
  foreach(_backend IN LISTS _lightusd_semantic_rt_backends)
    set(_test_name "lusdview-texture-semantic-rt-package-parity-${_backend}")
    set(_semantic_rt_timeout_env)
    if(_backend STREQUAL "vk-rt")
      set(_semantic_rt_timeout_env "LUSDVIEW_RENDER_TIMEOUT=240s")
    elseif(_backend STREQUAL "cuda")
      # CUDA initialization can stall a child viewer after a preceding CUDA
      # shard has released its context. Keep one stalled render from
      # consuming the entire aggregate parity test timeout; the matrix has
      # passed with this guard and reports a normal backend failure/skip.
      set(_semantic_rt_timeout_env "LUSDVIEW_RENDER_TIMEOUT=20s")
    endif()
    add_test(
      NAME "${_test_name}"
      COMMAND ${CMAKE_COMMAND} -E env
            "LUSDVIEW=$<TARGET_FILE:lusdview>"
            "LUSDVIEW_SEMANTIC_BACKENDS=${_backend}"
            "LUSDVIEW_SEMANTIC_LOADERS=core"
            "LUSDVIEW_SEMANTIC_FAMILIES=openpbr"
            "LUSDVIEW_SEMANTIC_MODES=albedo"
            ${_semantic_rt_timeout_env}
            bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-texture-semantic-aov.sh"
    )
    set_tests_properties("${_test_name}" PROPERTIES
                         SKIP_RETURN_CODE 77 TIMEOUT 600 LABELS "lusdview"
                         RESOURCE_LOCK lusdview_semantic_gpu)
  endforeach()

  add_test(
    NAME lusdview-light-record-equivalence
    COMMAND ${CMAKE_COMMAND} -E env
            "LUSDVIEW=$<TARGET_FILE:lusdview>"
            bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-light-record-equivalence.sh"
    )
  set_tests_properties(lusdview-light-record-equivalence PROPERTIES
                       SKIP_RETURN_CODE 77 TIMEOUT 120)

  add_test(
    NAME lusdview-instancer-invisible-ids
    COMMAND ${CMAKE_COMMAND} -E env
            "LUSDVIEW=$<TARGET_FILE:lusdview>"
            bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-instancer-invisible-ids.sh"
    )
  set_tests_properties(lusdview-instancer-invisible-ids PROPERTIES
                       SKIP_RETURN_CODE 77 TIMEOUT 120)

  add_test(
    NAME lusdview-tydra-pointinstancer
    COMMAND ${CMAKE_COMMAND} -E env
            "LUSDVIEW=$<TARGET_FILE:lusdview>"
            bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-tydra-pointinstancer.sh"
    )
  set_tests_properties(lusdview-tydra-pointinstancer PROPERTIES
                       SKIP_RETURN_CODE 77 TIMEOUT 120)

  add_test(
    NAME lusdview-inst-morph-cull
    COMMAND ${CMAKE_COMMAND} -E env
            "LUSDVIEW=$<TARGET_FILE:lusdview>"
            bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-inst-morph-cull.sh"
    )
  set_tests_properties(lusdview-inst-morph-cull PROPERTIES
                       SKIP_RETURN_CODE 77 TIMEOUT 120)

  # DomeLight IBL baking is provided by the optional vendored textools
  # dependency. Do not register a pixel-regression that can never exercise
  # its feature when that dependency is intentionally disabled.
  if(LIGHTUSD_WITH_TEXTOOLS)
    add_test(
      NAME lusdview-dome-orientation
      COMMAND ${CMAKE_COMMAND} -E env
              "LUSDVIEW=$<TARGET_FILE:lusdview>"
              bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-dome-orientation.sh"
      )
    set_tests_properties(lusdview-dome-orientation PROPERTIES
                         SKIP_RETURN_CODE 77 TIMEOUT 240)
  endif()

  add_test(
    NAME lusdview-displacement-udim
    COMMAND ${CMAKE_COMMAND} -E env
            "LUSDVIEW=$<TARGET_FILE:lusdview>"
            bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-displacement-udim.sh"
    )
  set_tests_properties(lusdview-displacement-udim PROPERTIES
                       SKIP_RETURN_CODE 77 TIMEOUT 600)

  add_test(
    NAME lusdview-vk-displacement-isolation
    COMMAND ${CMAKE_COMMAND} -E env
            "LUSDVIEW=$<TARGET_FILE:lusdview>"
            bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-vk-displacement-isolation.sh"
    )
  set_tests_properties(lusdview-vk-displacement-isolation PROPERTIES
                       SKIP_RETURN_CODE 77 TIMEOUT 120)

  # A single mesh may bind different materials through materialBind
  # GeomSubsets. All RT backends must resolve the committed triangle's
  # submesh material, not the mesh's first material.
  add_test(
    NAME lusdview-rt-geomsubset-material
    COMMAND ${CMAKE_COMMAND} -E env
            "LUSDVIEW=$<TARGET_FILE:lusdview>"
            bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-rt-geomsubset-material.sh"
    )
  set_tests_properties(lusdview-rt-geomsubset-material
                       PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
  # Instrumented RT renders exceed the 60 s per-render default.
  if(SANITIZE_ADDRESS OR SANITIZE_UNDEFINED)
    set_tests_properties(lusdview-rt-geomsubset-material PROPERTIES
                         TIMEOUT 1500 ENVIRONMENT "LUSDVIEW_RENDER_TIMEOUT=240s")
  endif()

  # Deferred package payloads must survive the async recomposition handoff in
  # the core loader. Drive the actual MCP action so the test covers the same
  # lifetime boundary as the interactive "Load payload" UI.
  add_test(
    NAME lusdview-usdz-deferred-payload
    COMMAND ${CMAKE_COMMAND} -E env
            "LUSDVIEW=$<TARGET_FILE:lusdview>"
            bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-usdz-deferred-payload.sh"
    )
  set_tests_properties(lusdview-usdz-deferred-payload
                       PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 90)
  # Instrumented recomposition is slower; widen only the harness poll
  # deadline (the completion assertion is unchanged).
  if(SANITIZE_ADDRESS OR SANITIZE_UNDEFINED)
    set_tests_properties(lusdview-usdz-deferred-payload PROPERTIES
                         TIMEOUT 300
                         ENVIRONMENT "LUSDVIEW_DEFERRED_PAYLOAD_TIMEOUT_MS=120000")
  endif()

  add_test(
    NAME lusdview-incremental-topology
    COMMAND ${CMAKE_COMMAND} -E env
            "LUSDVIEW=$<TARGET_FILE:lusdview>"
            bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-incremental-topology.sh"
    )
  set_tests_properties(lusdview-incremental-topology
                       PROPERTIES LABELS "lusdview" SKIP_RETURN_CODE 77 TIMEOUT 90)

  add_test(
    NAME lusdview-incremental-layer-reload
    COMMAND ${CMAKE_COMMAND} -E env
            "LUSDVIEW=$<TARGET_FILE:lusdview>"
            bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-incremental-layer-reload.sh"
    )
  set_tests_properties(lusdview-incremental-layer-reload
                       PROPERTIES LABELS "lusdview" SKIP_RETURN_CODE 77 TIMEOUT 90)

  if(LUSDVIEW_XVFB_RUN)
    add_test(
      NAME lusdview-incremental-layer-reload-gl
      COMMAND "${LUSDVIEW_XVFB_RUN}" -a
              ${CMAKE_COMMAND} -E env
              "LUSDVIEW=$<TARGET_FILE:lusdview>"
              "BACKEND=gl"
              bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-incremental-layer-reload.sh"
      )
    set_tests_properties(lusdview-incremental-layer-reload-gl
                         PROPERTIES LABELS "lusdview" SKIP_RETURN_CODE 77 TIMEOUT 90)
  endif()

  add_test(
    NAME lusdview-incremental-texture
    COMMAND ${CMAKE_COMMAND} -E env
            "LUSDVIEW=$<TARGET_FILE:lusdview>"
            bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-incremental-texture.sh"
    )
  set_tests_properties(lusdview-incremental-texture
                       PROPERTIES LABELS "lusdview" SKIP_RETURN_CODE 77 TIMEOUT 90)

  if(LUSDVIEW_XVFB_RUN)
    add_test(
      NAME lusdview-incremental-texture-gl
      COMMAND "${LUSDVIEW_XVFB_RUN}" -a
              ${CMAKE_COMMAND} -E env
              "LUSDVIEW=$<TARGET_FILE:lusdview>"
              "BACKEND=gl"
              bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-incremental-texture.sh"
      )
    set_tests_properties(lusdview-incremental-texture-gl
                         PROPERTIES LABELS "lusdview" SKIP_RETURN_CODE 77 TIMEOUT 90)
  endif()

  # Deterministic back-to-front alpha blending on both raster backends.
  # Keep this registered separately from scalar opacity/cutout coverage: it
  # catches draw-order regressions with three overlapping translucent layers.
  add_test(
    NAME lusdview-transparency
    COMMAND ${CMAKE_COMMAND} -E env
            "LUSDVIEW=$<TARGET_FILE:lusdview>"
            bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-transparency.sh"
    )
  set_tests_properties(lusdview-transparency PROPERTIES SKIP_RETURN_CODE 77)

  # A fresh cache must not make the default auto transparency mode pay for
  # weighted OIT. The script reports elapsed time but intentionally has no
  # timing threshold, so it is safe across drivers and software Vulkan.
  add_test(
    NAME lusdview-vulkan-cold-start-auto
    COMMAND ${CMAKE_COMMAND} -E env
            "LUSDVIEW=$<TARGET_FILE:lusdview>"
            bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-vk-cold-start-auto.sh"
    )
  set_tests_properties(lusdview-vulkan-cold-start-auto
                       PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120
                                  LABELS "lusdview;vulkan;startup;transparency")

  add_test(NAME lusdview-vulkan-oit-promotion
    COMMAND ${LUSDR_PYTHON} "${PROJECT_SOURCE_DIR}/tests/lusdview/mcp_oit_promotion.py"
            "$<TARGET_FILE:lusdview>"
            "${PROJECT_SOURCE_DIR}/models/lusdview-transparency.usda" --scene-changes)
  set_tests_properties(lusdview-vulkan-oit-promotion PROPERTIES
    SKIP_RETURN_CODE 77 TIMEOUT 300 LABELS "lusdview;vulkan;transparency")
  if(LUSDVIEW_ENABLE_GL_THREAD)
    add_test(NAME lusdview-vulkan-oit-promotion-threaded
      COMMAND ${LUSDR_PYTHON} "${PROJECT_SOURCE_DIR}/tests/lusdview/mcp_oit_promotion.py"
              "$<TARGET_FILE:lusdview>"
              "${PROJECT_SOURCE_DIR}/models/lusdview-transparency.usda" --threaded)
    set_tests_properties(lusdview-vulkan-oit-promotion-threaded PROPERTIES
      SKIP_RETURN_CODE 77 TIMEOUT 300 LABELS "lusdview;vulkan;transparency")
  endif()

  # Optional Khronos ValidationLayers smoke for the weighted-OIT command path.
  # Local developers can build the layer with
  # scripts/setup-vulkan-validation-linux.sh. CI may provide a distro package
  # or prebuilt VK_LAYER_PATH; otherwise this test deliberately skips (77)
  # rather than downloading/building ValidationLayers during the test job.
  add_test(
    NAME lusdview-vk-oit-validation
    COMMAND ${CMAKE_COMMAND} -E env
            "LUSDVIEW=$<TARGET_FILE:lusdview>"
            "LUSDVIEW_ROOT=${PROJECT_SOURCE_DIR}"
            bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-vk-oit-validation.sh"
    )
  set_tests_properties(lusdview-vk-oit-validation PROPERTIES
                       SKIP_RETURN_CODE 77 TIMEOUT 150)

  # The CPU tracer must own headless screenshots, including its bounded
  # stacked-alpha compositor, rather than silently capturing Vulkan raster.
  add_test(
    NAME lusdview-cpu-rt-headless
    COMMAND ${CMAKE_COMMAND} -E env
            "LUSDVIEW=$<TARGET_FILE:lusdview>"
            bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-cpu-rt-headless.sh"
    )
  set_tests_properties(lusdview-cpu-rt-headless PROPERTIES
                       SKIP_RETURN_CODE 77 TIMEOUT 180)

  # A tangent-space normal map must shade exactly like the same world normal
  # authored as geometry normals, and a flat map must be neutral. Uses the
  # CPU tracer so no GPU ray-query support is required beyond the headless
  # presentation context.
  add_test(
    NAME lusdview-cpu-normal-map
    COMMAND ${CMAKE_COMMAND} -E env
            "LUSDVIEW=$<TARGET_FILE:lusdview>"
            bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-cpu-normal-map.sh"
    )
  set_tests_properties(lusdview-cpu-normal-map PROPERTIES
                       SKIP_RETURN_CODE 77 TIMEOUT 300
                       LABELS "lusdview;rt;cpu-rt")

  # MaterialX `tangent`/`bitangent` graph nodes must report the hit
  # triangle's frame, not fixed constants; the CPU graph evaluator now takes
  # both from the same frame the normal map uses.
  add_test(
    NAME lusdview-cpu-graph-tangent
    COMMAND ${CMAKE_COMMAND} -E env
            "LUSDVIEW=$<TARGET_FILE:lusdview>"
            bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-cpu-graph-tangent.sh"
    )
  set_tests_properties(lusdview-cpu-graph-tangent PROPERTIES
                       SKIP_RETURN_CODE 77 TIMEOUT 300
                       LABELS "lusdview;rt;cpu-rt")

  # Per-slot texture UV transforms and UV-set routing on the CPU path. A
  # transformed slot must shade exactly like the same texture sampled with
  # pre-transformed `st`, including the normal-map tangent frame.
  add_test(
    NAME lusdview-cpu-uv-transform
    COMMAND ${CMAKE_COMMAND} -E env
            "LUSDVIEW=$<TARGET_FILE:lusdview>"
            bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-cpu-uv-transform.sh"
    )
  set_tests_properties(lusdview-cpu-uv-transform PROPERTIES
                       SKIP_RETURN_CODE 77 TIMEOUT 300
                       LABELS "lusdview;rt;cpu-rt")

  # The same per-slot UV transform contract on the non-base semantic lanes
  # (metallic, occlusion, clearcoat, emissive): each lane must match the
  # pre-transformed-UV reference and must not be a no-op.
  add_test(
    NAME lusdview-cpu-lane-uv-transform
    COMMAND ${CMAKE_COMMAND} -E env
            "LUSDVIEW=$<TARGET_FILE:lusdview>"
            bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-cpu-lane-uv-transform.sh"
    )
  set_tests_properties(lusdview-cpu-lane-uv-transform PROPERTIES
                       SKIP_RETURN_CODE 77 TIMEOUT 300
                       LABELS "lusdview;rt;cpu-rt")

  # Second UV set (multi-UV): a texture whose UsdPrimvarReader names `st1`
  # must sample the second set, matching the equivalent pre-transformed `st`
  # lookup, with an identity-st control that must differ.
  # The oracle renders the same scenes through the core lusdrender converter
  # as well as the viewer, so it needs both binaries.
  if(TARGET lusdrender)
    add_test(
      NAME lusdview-cpu-multi-uv
      COMMAND ${CMAKE_COMMAND} -E env
              "LUSDVIEW=$<TARGET_FILE:lusdview>"
              "LUSDRENDER=$<TARGET_FILE:lusdrender>"
              bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-cpu-multi-uv.sh"
      )
    set_tests_properties(lusdview-cpu-multi-uv PROPERTIES
                         SKIP_RETURN_CODE 77 TIMEOUT 300
                         LABELS "lusdview;rt;cpu-rt")
  endif()

  # CPU authored tangent basis: a rotated primvars:tangents must shade the
  # same tangent-space normal like the equivalent geometry normal.
  add_test(
    NAME lusdview-cpu-authored-tangent
    COMMAND ${CMAKE_COMMAND} -E env
            "LUSDVIEW=$<TARGET_FILE:lusdview>"
            bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-cpu-authored-tangent.sh"
    )
  set_tests_properties(lusdview-cpu-authored-tangent PROPERTIES
                       SKIP_RETURN_CODE 77 TIMEOUT 300
                       LABELS "lusdview;rt;cpu-rt")

  # CPU OpenPBR coat-normal lane: a tilted coat map must shade exactly like
  # the same world normal authored as geometry normals and must not change
  # the image at coat weight 0; a flat map is neutral.
  add_test(
    NAME lusdview-cpu-coat-normal
    COMMAND ${CMAKE_COMMAND} -E env
            "LUSDVIEW=$<TARGET_FILE:lusdview>"
            bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-cpu-coat-normal.sh"
    )
  set_tests_properties(lusdview-cpu-coat-normal PROPERTIES
                       SKIP_RETURN_CODE 77 TIMEOUT 300
                       LABELS "lusdview;rt;cpu-rt")

  # CPU preview semantic material lanes: a uniform texture lane must shade
  # exactly like the equivalent constant lane, and MaterialX metallic/
  # roughness/emission graph routes must affect the render.
  foreach(cpu_lane_test
      lusdview-cpu-material-lanes
      lusdview-cpu-graph-lanes
      lusdview-cpu-graph-chain)
    string(REPLACE "lusdview-cpu-" "run-cpu-" cpu_lane_script
                   "${cpu_lane_test}")
    add_test(
      NAME ${cpu_lane_test}
      COMMAND ${CMAKE_COMMAND} -E env
              "LUSDVIEW=$<TARGET_FILE:lusdview>"
              bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/${cpu_lane_script}.sh"
      )
    set_tests_properties(${cpu_lane_test} PROPERTIES
                         SKIP_RETURN_CODE 77 TIMEOUT 300
                         LABELS "lusdview;rt;cpu-rt")
  endforeach()

  # Element-scale Island PTex routing: a real quad-topology element must not
  # fall back to the representative face, and every visible PTex sample must
  # match the independent OpenPtex decoder. Skips without the corpus/OpenPtex.
  add_test(
    NAME lusdview-cpu-ptex-element
    COMMAND ${CMAKE_COMMAND} -E env
            "LUSDVIEW=$<TARGET_FILE:lusdview>"
            bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-cpu-ptex-element.sh"
    )
  set_tests_properties(lusdview-cpu-ptex-element PROPERTIES
                       SKIP_RETURN_CODE 77 TIMEOUT 1800
                       LABELS "lusdview;rt;cpu-rt;ptex")

  # Visible Island PTex material routing: the CPU tracer must map intrinsic
  # Ptex face coordinates through the atlas face rectangles and show the
  # decoded face colour. Skips when the pinned Island texture is unavailable.
  add_test(
    NAME lusdview-cpu-ptex-routing
    COMMAND ${CMAKE_COMMAND} -E env
            "LUSDVIEW=$<TARGET_FILE:lusdview>"
            bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-cpu-ptex-routing.sh"
    )
  set_tests_properties(lusdview-cpu-ptex-routing PROPERTIES
                       SKIP_RETURN_CODE 77 TIMEOUT 300
                       LABELS "lusdview;rt;cpu-rt;ptex")

  add_test(
    NAME lusdview-cpu-rt-stacked-glass
    COMMAND ${CMAKE_COMMAND} -E env
            "LUSDVIEW=$<TARGET_FILE:lusdview>"
            "ASSET=${PROJECT_SOURCE_DIR}/tests/usda/lusdview-stacked-glass.usda"
            bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-cpu-rt-headless.sh"
  )
  set_tests_properties(lusdview-cpu-rt-stacked-glass PROPERTIES
                       SKIP_RETURN_CODE 77 TIMEOUT 180
                       LABELS "lusdview;rt;transparency")

  add_test(
    NAME lusdview-cpu-rt-openpbr-lobes
    COMMAND ${CMAKE_COMMAND} -E env
            "LUSDVIEW=$<TARGET_FILE:lusdview>"
            "ASSET=${PROJECT_SOURCE_DIR}/tests/usda/lusdview-unsupported-realtime-lobes.usda"
            bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-cpu-rt-headless.sh"
  )
  set_tests_properties(lusdview-cpu-rt-openpbr-lobes PROPERTIES
                       SKIP_RETURN_CODE 77 TIMEOUT 180
                       LABELS "lusdview;rt;openpbr")

  add_test(
    NAME lusdview-cpu-rt-materialx-swizzle
    COMMAND ${CMAKE_COMMAND} -E env
            "LUSDVIEW=$<TARGET_FILE:lusdview>"
            bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-cpu-rt-materialx-swizzle.sh"
  )
  set_tests_properties(lusdview-cpu-rt-materialx-swizzle PROPERTIES
                       SKIP_RETURN_CODE 77 TIMEOUT 180
                       LABELS "lusdview;rt;materialx;openpbr")

  add_test(
    NAME lusdview-openchess-materials
    COMMAND ${CMAKE_COMMAND} -E env
            "LUSDVIEW=$<TARGET_FILE:lusdview>"
            bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-openchess-materials.sh"
    )
  set_tests_properties(lusdview-openchess-materials PROPERTIES
                       SKIP_RETURN_CODE 77 TIMEOUT 240
                       LABELS "lusdview;rt;materialx;openpbr")
  # The ~7M-triangle CPU RT capture exceeds 180 s under ASan/UBSan.
  if(SANITIZE_ADDRESS OR SANITIZE_UNDEFINED)
    set_tests_properties(lusdview-openchess-materials PROPERTIES
                         TIMEOUT 1200 ENVIRONMENT "LUSDVIEW_OPENCHESS_TIMEOUT=900s")
  endif()

  add_test(
    NAME lusdview-openchess-path-trace
    COMMAND ${CMAKE_COMMAND} -E env
            "LUSDVIEW=$<TARGET_FILE:lusdview>"
            bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-openchess-path-trace.sh"
    )
  set_tests_properties(lusdview-openchess-path-trace PROPERTIES
                       SKIP_RETURN_CODE 77 TIMEOUT 3600
                       LABELS "lusdview;rt;path-trace;materialx;openpbr;gpu")

  # USD `proxy` and `render` are ALTERNATIVES, not two things to draw. Drawing
  # both put the stand-in on top of the geometry it stands in for (intent-vfx's
  # simpleAsset: a proxy Cube of size 2 exactly encloses the render Sphere of
  # radius 1). Both loaders must supersede the proxy -- and still draw it when
  # there is no render geometry to supersede it.
  add_test(
    NAME lusdview-purpose-proxy-supersede
    COMMAND ${CMAKE_COMMAND} -E env
            "LUSDVIEW=$<TARGET_FILE:lusdview>"
            bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-purpose-proxy-supersede.sh"
    )
  set_tests_properties(lusdview-purpose-proxy-supersede
                       PROPERTIES SKIP_RETURN_CODE 77)

  # Layer-relative asset-path anchoring: a look layer nested below the root
  # reaches its texture with `../tex/...`, which only resolves when anchored to
  # the layer that authored it (see src/layer/asset-anchor.hh).
  add_test(
    NAME lusdview-asset-path-anchoring
    COMMAND ${CMAKE_COMMAND} -E env
            "LUSDVIEW=$<TARGET_FILE:lusdview>"
            bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-material-binding-inheritance.sh"
                 "${PROJECT_SOURCE_DIR}/models/nested-look-texture/root.usda"
    )
  set_tests_properties(lusdview-asset-path-anchoring
                       PROPERTIES SKIP_RETURN_CODE 77)

  # usd-wg/assets regression baseline. The batch harness could always RENDER the
  # corpus; what it had no answer for was "did this change what any of it looks
  # like?" -- so a render that silently turned a scene black still passed. The
  # checked-in baseline (examples/lusdview/tests/usd-assets-goldens.tsv) records a
  # COVERAGE fingerprint per (mode, asset): a silhouette bitfield, chosen over a
  # pixel hash because it survives the GPU/driver lighting differences that would
  # otherwise make the file useless anywhere but the machine that wrote it. A
  # geometry, composition, or material-binding regression moves the silhouette; a
  # shading nudge does not.
  #
  # Still a per-machine artifact in the end, so it is OPT-IN twice over: it needs
  # the corpus (USD_ASSETS_ROOT, else SKIP) and LUSDVIEW_RUN_GOLDEN=1 (else SKIP).
  # Refresh it deliberately, and read the diff before committing it:
  #
  #   LUSDVIEW_RUN_GOLDEN=1 tests/lusdview/run-usd-assets-batch.sh \
  #     --golden-kind coverage \
  #     --update-golden examples/lusdview/tests/usd-assets-goldens.tsv
  add_test(
    NAME lusdview-usd-assets-golden
    COMMAND ${CMAKE_COMMAND} -E env
            "LUSDVIEW=$<TARGET_FILE:lusdview>"
            "LUSDVIEW_USD_ASSETS_GATE=LUSDVIEW_RUN_GOLDEN"
            "LUSDVIEW_USD_ASSETS_FAIL_ON=timeout,backend_error,golden_mismatch,unexpected_degradation,expectation_mismatch"
            bash "${PROJECT_SOURCE_DIR}/tests/lusdview/run-usd-assets-batch.sh"
                 --root "${LIGHTUSD_USD_ASSETS_ROOT}"
                 --golden-kind coverage
                 --golden "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/usd-assets-goldens.tsv"
    )
  set_tests_properties(lusdview-usd-assets-golden
                       PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 5400)

  # Self-contained classification/expectation test for the external-corpus
  # harness. Uses a generated empty layer and lusdrender CPU, so it verifies
  # no_renderable plus hard expectation mismatches without usd-assets data.
  if(TARGET lusdrender)
    add_test(
      NAME lusdview-usd-assets-classification
      COMMAND ${CMAKE_COMMAND} -E env
              "LUSDVIEW=$<TARGET_FILE:lusdview>"
              "LUSDRENDER=$<TARGET_FILE:lusdrender>"
              bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-no-renderable-tracking.sh"
      )
    set_tests_properties(lusdview-usd-assets-classification
                         PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)

    # Bounded but broad cross-renderer coverage for the external usd-wg/assets
    # checkout. This is separately opt-in because it exercises 40 scenes over
    # four backends and is intentionally much heavier than the self-contained
    # profile/classification regressions below.
    add_test(
      NAME lusdview-usd-assets-broad-smoke
      COMMAND ${CMAKE_COMMAND} -E env
              "LUSDVIEW=$<TARGET_FILE:lusdview>"
              "LUSDRENDER=$<TARGET_FILE:lusdrender>"
              "LUSDVIEW_USD_ASSETS_GATE=LUSDVIEW_RUN_USD_ASSETS_BROAD"
              "LUSDVIEW_USD_ASSETS_ALLOW_PARENT=1"
              bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-usd-assets-render-smoke.sh"
                   --root "${LIGHTUSD_USD_ASSETS_ROOT}"
                   --profile usd-assets-broad
                   --require-profile-complete
                   --modes vk-raster,vk-rt,lusdr-cpu,lusdr-vk
                   --time 1
      )
    set_tests_properties(lusdview-usd-assets-broad-smoke PROPERTIES
                         SKIP_RETURN_CODE 77 TIMEOUT 7200
                         LABELS "lusdview;external;usd-assets")

    add_test(
      NAME lusdview-usd-assets-animation-smoke
      COMMAND ${CMAKE_COMMAND} -E env
              "LUSDVIEW=$<TARGET_FILE:lusdview>"
              "LUSDRENDER=$<TARGET_FILE:lusdrender>"
              "LUSDVIEW_USD_ASSETS_GATE=LUSDVIEW_RUN_USD_ASSETS_ANIMATION"
              "LUSDVIEW_USD_ASSETS_ALLOW_PARENT=1"
              "USD_ASSETS_ROOT=${LIGHTUSD_USD_ASSETS_ROOT}"
              bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-usd-assets-animation-smoke.sh"
      )
    set_tests_properties(lusdview-usd-assets-animation-smoke PROPERTIES
                         SKIP_RETURN_CODE 77 TIMEOUT 3600
                         LABELS "lusdview;external;usd-assets;animation")
  endif()

  add_test(
    NAME lusdview-usd-assets-broad-profile
    COMMAND bash
            "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-usd-assets-broad-profile.sh"
    )
  set_tests_properties(lusdview-usd-assets-broad-profile PROPERTIES TIMEOUT 120)
endif()

# Xvfb normally gives GLFW a Mesa software context. On machines where the
# NVIDIA device is visible, PRIME/GLVND can route that context to the real
# GPU; apply the same selection to every lusdview CTest case so nested
# xvfb-run invocations inherit it. Keep this opt-in because forcing the
# NVIDIA ICD on a host without /dev/nvidia0 turns a useful capability skip
# into a hard initialization failure.
#
# Kernel/GLVND presence alone is not enough for Vulkan: a container can
# expose libGLX_nvidia and the NVIDIA layers while the Vulkan ICD is absent,
# leaving only llvmpipe. Probe the physical-device list before exporting
# LUSDVIEW_VK_DEVICE=nvidia; otherwise leave Vulkan on its normal automatic
# hardware/software selection path.
if(LIGHTUSD_LUSDVIEW_NVIDIA_OFFLOAD AND UNIX AND NOT APPLE AND TARGET lusdview)
  if(EXISTS "/proc/driver/nvidia/version" AND
     EXISTS "/dev/nvidia0" AND
     EXISTS "/usr/share/glvnd/egl_vendor.d/10_nvidia.json")
    set(_lightusd_nvidia_vulkan_visible FALSE)
    find_program(_lightusd_vulkaninfo NAMES vulkaninfo)
    if(_lightusd_vulkaninfo)
      execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
                "__NV_PRIME_RENDER_OFFLOAD=1"
                "__GLX_VENDOR_LIBRARY_NAME=nvidia"
                "__EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/10_nvidia.json"
                "${_lightusd_vulkaninfo}" --summary
        RESULT_VARIABLE _lightusd_vulkaninfo_rc
        OUTPUT_VARIABLE _lightusd_vulkaninfo_out
        ERROR_VARIABLE _lightusd_vulkaninfo_err
        TIMEOUT 10)
      if(_lightusd_vulkaninfo_rc EQUAL 0)
        string(CONCAT _lightusd_vulkaninfo_text
               "${_lightusd_vulkaninfo_out}\n${_lightusd_vulkaninfo_err}")
        string(REGEX MATCH
               "deviceName[ \\t]*=[ \\t]*[^\\n]*(NVIDIA|GeForce|RTX|Quadro|Tesla)"
               _lightusd_nvidia_device_match
               "${_lightusd_vulkaninfo_text}")
        if(_lightusd_nvidia_device_match)
          set(_lightusd_nvidia_vulkan_visible TRUE)
        endif()
      endif()
    endif()
    get_property(_lightusd_lusdview_tests DIRECTORY PROPERTY TESTS)
    set(_lightusd_nvidia_gl_cache
        "${CMAKE_BINARY_DIR}/lusdview-nvidia-gl-cache")
    file(MAKE_DIRECTORY "${_lightusd_nvidia_gl_cache}")
    add_test(
      NAME lusdview-nvidia-gl-warmup
      COMMAND ${CMAKE_COMMAND} -E env
              "LUSDVIEW=$<TARGET_FILE:lusdview>"
              bash
              "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-gl-warmup.sh")
    set_tests_properties(
      lusdview-nvidia-gl-warmup
      PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180 LABELS "lusdview;opengl"
                 FIXTURES_SETUP lusdview_nvidia_gl_cache
                 RESOURCE_LOCK lusdview_gl_gpu)
    list(APPEND _lightusd_lusdview_tests lusdview-nvidia-gl-warmup)
    foreach(_lightusd_test IN LISTS _lightusd_lusdview_tests)
      # CPU RT uses Vulkan only to provide a windowless presentation shell;
      # it neither needs GL shader warmup nor an NVIDIA device. Keeping these
      # tests automatic lets them run on RADV/llvmpipe when PRIME state or a
      # configure-time Xvfb display later disappears.
      if(_lightusd_test MATCHES "^lusdview-" AND
         NOT _lightusd_test MATCHES "^lusdview-cpu-rt-")
        get_property(_lightusd_test_env TEST "${_lightusd_test}"
                     PROPERTY ENVIRONMENT)
        if(NOT _lightusd_test_env)
          set(_lightusd_test_env)
        endif()
        list(APPEND _lightusd_test_env
             "__NV_PRIME_RENDER_OFFLOAD=1"
             "__GLX_VENDOR_LIBRARY_NAME=nvidia"
             "__EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/10_nvidia.json"
             "__GL_SHADER_DISK_CACHE=1"
             "__GL_SHADER_DISK_CACHE_PATH=${_lightusd_nvidia_gl_cache}"
             "LUSDVIEW_NVIDIA_OFFLOAD=1"
             "LUSDVIEW_XVFB=1")
        if(_lightusd_nvidia_vulkan_visible)
          list(APPEND _lightusd_test_env "LUSDVIEW_VK_DEVICE=nvidia")
        endif()
        set_tests_properties("${_lightusd_test}"
                             PROPERTIES ENVIRONMENT "${_lightusd_test_env}")
        if(NOT _lightusd_test STREQUAL "lusdview-nvidia-gl-warmup")
          set_property(TEST "${_lightusd_test}" APPEND PROPERTY
                       FIXTURES_REQUIRED lusdview_nvidia_gl_cache)
        endif()
      endif()
    endforeach()
    if(_lightusd_nvidia_vulkan_visible)
      message(STATUS "lightusd: NVIDIA offload environment enabled for lusdview CTest cases (Vulkan NVIDIA device confirmed)")
    else()
      message(STATUS "lightusd: NVIDIA GL offload environment enabled; Vulkan NVIDIA device not visible, leaving Vulkan device selection automatic")
    endif()
  else()
    message(STATUS "lightusd: NVIDIA offload requested but NVIDIA device/GLVND is not visible; lusdview tests keep their normal environment")
  endif()
endif()

if(_lightusd_hip_ld_library_path)
  get_property(_lightusd_lusdview_tests DIRECTORY PROPERTY TESTS)
  foreach(_lightusd_test IN LISTS _lightusd_lusdview_tests)
    if(_lightusd_test MATCHES "^lusdview-")
      get_property(_lightusd_test_env TEST "${_lightusd_test}"
                   PROPERTY ENVIRONMENT)
      if(NOT _lightusd_test_env)
        set(_lightusd_test_env)
      endif()
      list(APPEND _lightusd_test_env
           "LD_LIBRARY_PATH=${_lightusd_hip_ld_library_path}")
      set_tests_properties("${_lightusd_test}"
                           PROPERTIES ENVIRONMENT "${_lightusd_test_env}")
    endif()
  endforeach()
endif()

if(LUSDVIEW_ENABLE_MCP)
  add_test(NAME lusdview-core-physics-preview COMMAND ${CMAKE_COMMAND} -E env
    "LUSDVIEW=$<TARGET_FILE:lusdview>"
    bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-physics-preview.sh")
  set_tests_properties(lusdview-core-physics-preview PROPERTIES
    SKIP_RETURN_CODE 77 TIMEOUT 120 LABELS "lusdview;core;physics")
endif()
