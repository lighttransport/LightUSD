cmake_minimum_required(VERSION 3.16)

# lusdchecker uses only the dependency-free next core. Build that target on
# demand when the top-level project has not enabled another next consumer.
if(NOT TARGET lightusd_next)
  add_subdirectory(${PROJECT_SOURCE_DIR}/src/next
                   ${CMAKE_BINARY_DIR}/lusdchecker_next_build)
endif()

add_executable(lusdchecker lusdchecker.cc report.cc)
target_link_libraries(lusdchecker PRIVATE lightusd_next)
target_compile_features(lusdchecker PRIVATE cxx_std_17)
target_include_directories(lusdchecker PRIVATE ${PROJECT_SOURCE_DIR}/src)

if(COMMAND add_sanitizers)
  add_sanitizers(lusdchecker)
endif()

set_target_properties(lusdchecker PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

if(LIGHTUSD_BUILD_TESTS)
  add_test(NAME lusdchecker_valid
    COMMAND lusdchecker --groups core
            ${PROJECT_SOURCE_DIR}/tests/usda/cube.usda)
  set_tests_properties(lusdchecker_valid PROPERTIES
    PASS_REGULAR_EXPRESSION "Result: PASSED")

  add_test(NAME lusdchecker_invalid
    COMMAND lusdchecker --groups core
            ${CMAKE_CURRENT_SOURCE_DIR}/testdata/invalid-layer.usda)
  set_tests_properties(lusdchecker_invalid PROPERTIES
    WILL_FAIL TRUE)

  add_test(NAME lusdchecker_json
    COMMAND lusdchecker --json --groups core
            ${PROJECT_SOURCE_DIR}/tests/usda/cube.usda)
  set_tests_properties(lusdchecker_json PROPERTIES
    PASS_REGULAR_EXPRESSION "\"valid\":true")

  add_test(NAME lusdchecker_usdc
    COMMAND lusdchecker --core-only
            ${PROJECT_SOURCE_DIR}/tests/usdc/cone-000.usdc)

  add_test(NAME lusdchecker_usdz
    COMMAND lusdchecker --core-only
            ${PROJECT_SOURCE_DIR}/models/cube.usdz)

  add_test(NAME lusdchecker_strict_warning
    COMMAND lusdchecker --strict --core-only
            ${CMAKE_CURRENT_SOURCE_DIR}/testdata/warning-only.usda)
  set_tests_properties(lusdchecker_strict_warning PROPERTIES
    WILL_FAIL TRUE)

  add_test(NAME lusdchecker_materialx
    COMMAND lusdchecker
            ${PROJECT_SOURCE_DIR}/tests/feat/mtlx/data/basic_openpbr.mtlx)

  add_test(NAME lusdchecker_materialx_invalid
    COMMAND lusdchecker
            ${CMAKE_CURRENT_SOURCE_DIR}/testdata/missing-materialx-node.mtlx)
  set_tests_properties(lusdchecker_materialx_invalid PROPERTIES
    WILL_FAIL TRUE)

  add_test(NAME lusdchecker_composed
    COMMAND lusdchecker --composed --groups core
            ${PROJECT_SOURCE_DIR}/tests/usda/aousd-defaultprim-reference.usda)

  add_test(NAME lusdchecker_composed_missing_reference
    COMMAND lusdchecker --composed --groups core
            ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/feat/lusdchecker/missing-reference.usda)
  set_tests_properties(lusdchecker_composed_missing_reference PROPERTIES
    WILL_FAIL TRUE)

  add_test(NAME lusdchecker_schema_placement
    COMMAND lusdchecker --groups geom,physics
            ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/feat/lusdchecker/invalid-schema-placement.usda)
  set_tests_properties(lusdchecker_schema_placement PROPERTIES
    WILL_FAIL TRUE)

  add_test(NAME lusdchecker_composed_type_mismatch
    COMMAND lusdchecker --composed --groups core
            ${CMAKE_CURRENT_SOURCE_DIR}/testdata/type-mismatch-root.usda)
  set_tests_properties(lusdchecker_composed_type_mismatch PROPERTIES
    WILL_FAIL TRUE)

  add_test(NAME lusdchecker_package_regressions
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/test-package.py
            $<TARGET_FILE:lusdchecker>)

  add_test(NAME lusdchecker_crate_structure
    COMMAND lusdchecker --groups crate
            ${PROJECT_SOURCE_DIR}/tests/usdc/cone-000.usdc)
  set_tests_properties(lusdchecker_crate_structure PROPERTIES
    PASS_REGULAR_EXPRESSION "Checked rule groups: crate")

  add_test(NAME lusdchecker_require_all_groups
    COMMAND lusdchecker --require-all-groups --all
            ${PROJECT_SOURCE_DIR}/tests/usda/cube.usda)
  set_tests_properties(lusdchecker_require_all_groups PROPERTIES
    WILL_FAIL TRUE)

  add_test(NAME lusdchecker_unselected_variant
    COMMAND lusdchecker --groups geom
            ${CMAKE_CURRENT_SOURCE_DIR}/testdata/invalid-variant-body.usda)
  set_tests_properties(lusdchecker_unselected_variant PROPERTIES
    WILL_FAIL TRUE)

  add_test(NAME lusdchecker_unknown_api_schema
    COMMAND lusdchecker --strict --groups core
            ${CMAKE_CURRENT_SOURCE_DIR}/testdata/unknown-api-schema.usda)
  set_tests_properties(lusdchecker_unknown_api_schema PROPERTIES
    WILL_FAIL TRUE)

  add_test(NAME lusdchecker_relationship_variability
    COMMAND lusdchecker --strict --groups core
            ${CMAKE_CURRENT_SOURCE_DIR}/testdata/varying-relationship.usda)
  set_tests_properties(lusdchecker_relationship_variability PROPERTIES
    WILL_FAIL TRUE)

  # --- ARKit / RealityKit profile -----------------------------------------
  add_test(NAME lusdchecker_arkit_clean
    COMMAND lusdchecker --arkit
            ${CMAKE_CURRENT_SOURCE_DIR}/testdata/arkit-clean.usda)
  set_tests_properties(lusdchecker_arkit_clean PROPERTIES
    PASS_REGULAR_EXPRESSION "Result: PASSED - no issues found")

  add_test(NAME lusdchecker_arkit_violations
    COMMAND lusdchecker --arkit
            ${CMAKE_CURRENT_SOURCE_DIR}/testdata/arkit-violations.usda)
  set_tests_properties(lusdchecker_arkit_violations PROPERTIES
    WILL_FAIL TRUE)

  # PASS_REGULAR_EXPRESSION ignores the exit status, so this asserts the rule
  # ids reported for the same (failing) asset.
  add_test(NAME lusdchecker_arkit_violation_rules
    COMMAND lusdchecker --arkit
            ${CMAKE_CURRENT_SOURCE_DIR}/testdata/arkit-violations.usda)
  set_tests_properties(lusdchecker_arkit_violation_rules PROPERTIES
    PASS_REGULAR_EXPRESSION "arkit.normalMap.scaleBias")

  # The ARKit profile is opt-in: --all must not report arkit.* rules. (The
  # asset still draws always-on usdchecker-parity warnings — missing stage
  # metadata, unresolvable textures, shade.normalMap — so assert the absence
  # of arkit.* ids, not a clean pass.)
  add_test(NAME lusdchecker_arkit_opt_in
    COMMAND lusdchecker --all
            ${CMAKE_CURRENT_SOURCE_DIR}/testdata/arkit-violations.usda)
  set_tests_properties(lusdchecker_arkit_opt_in PROPERTIES
    PASS_REGULAR_EXPRESSION "Result: PASSED"
    FAIL_REGULAR_EXPRESSION "\\[arkit\\.")

  add_test(NAME lusdchecker_arkit_json
    COMMAND lusdchecker --arkit --json
            ${CMAKE_CURRENT_SOURCE_DIR}/testdata/arkit-clean.usda)
  set_tests_properties(lusdchecker_arkit_json PROPERTIES
    PASS_REGULAR_EXPRESSION "\"checkedGroups\":\\[.*\"arkit\"\\]")

  add_test(NAME lusdchecker_arkit_package
    COMMAND lusdchecker --arkit
            ${CMAKE_CURRENT_SOURCE_DIR}/testdata/arkit-bad-package.usdz)
  set_tests_properties(lusdchecker_arkit_package PROPERTIES
    WILL_FAIL TRUE)

  add_test(NAME lusdchecker_arkit_package_rules
    COMMAND lusdchecker --arkit
            ${CMAKE_CURRENT_SOURCE_DIR}/testdata/arkit-bad-package.usdz)
  set_tests_properties(lusdchecker_arkit_package_rules PROPERTIES
    PASS_REGULAR_EXPRESSION "arkit.package.rootLayer")

  # The same package is only a portability *warning* without --arkit.
  add_test(NAME lusdchecker_arkit_package_opt_in
    COMMAND lusdchecker --groups package
            ${CMAKE_CURRENT_SOURCE_DIR}/testdata/arkit-bad-package.usdz)
  set_tests_properties(lusdchecker_arkit_package_opt_in PROPERTIES
    PASS_REGULAR_EXPRESSION "package.entry.extension")

  # --- usdchecker parity rules ----------------------------------------------
  # Each fixture violates exactly one parity rule family; the test asserts the
  # rule id is reported (exit status ignored by PASS_REGULAR_EXPRESSION).
  function(lusdchecker_parity_rule_test name fixture rule_regex)
    add_test(NAME ${name}
      COMMAND lusdchecker ${CMAKE_CURRENT_SOURCE_DIR}/testdata/${fixture})
    set_tests_properties(${name} PROPERTIES
      PASS_REGULAR_EXPRESSION "${rule_regex}")
  endfunction()

  lusdchecker_parity_rule_test(lusdchecker_parity_stage_upaxis
    parity-stage-metadata.usda "geom.stage.upAxis")
  lusdchecker_parity_rule_test(lusdchecker_parity_stage_metersperunit
    parity-stage-metadata.usda "geom.stage.metersPerUnit")
  lusdchecker_parity_rule_test(lusdchecker_parity_defaultprim_missing
    parity-stage-metadata.usda "core.layer.defaultPrim.missing")
  lusdchecker_parity_rule_test(lusdchecker_parity_imageable_in_material
    parity-imageable-in-material.usda "shade.encapsulation.imageableInMaterial")
  lusdchecker_parity_rule_test(lusdchecker_parity_shader_type_mismatch
    parity-shader-type-mismatch.usda "shade.shader.typeMismatch")
  lusdchecker_parity_rule_test(lusdchecker_parity_unknown_shader_id
    parity-unknown-shader-id.usda "shade.shader.id")
  lusdchecker_parity_rule_test(lusdchecker_parity_subset_parent
    parity-geomsubset.usda "geom.subset.parent")
  lusdchecker_parity_rule_test(lusdchecker_parity_subset_familytype
    parity-geomsubset.usda "geom.subset.familyType")
  lusdchecker_parity_rule_test(lusdchecker_parity_subset_bind_family
    parity-geomsubset.usda "shade.subset.materialBindFamily")
  lusdchecker_parity_rule_test(lusdchecker_parity_unresolvable_dep
    parity-unresolvable-dep.usda "core.dependency.unresolvable")
  lusdchecker_parity_rule_test(lusdchecker_parity_skel_binding_rel
    parity-skel-binding.usda "geom.skel.binding.api")

  # shade.normalMap.* is always-on (not just --arkit), matching pxr's
  # NormalMapTextureValidator.
  lusdchecker_parity_rule_test(lusdchecker_parity_normalmap_always_on
    arkit-violations.usda "shade.normalMap.scaleBias")

  # --no-asset-checks gates only the defaultPrim presence rule (pxr
  # --noAssetChecks); stage metadata rules stay on.
  add_test(NAME lusdchecker_parity_no_asset_checks
    COMMAND lusdchecker --no-asset-checks
            ${CMAKE_CURRENT_SOURCE_DIR}/testdata/parity-stage-metadata.usda)
  set_tests_properties(lusdchecker_parity_no_asset_checks PROPERTIES
    PASS_REGULAR_EXPRESSION "geom.stage.upAxis"
    FAIL_REGULAR_EXPRESSION "core.layer.defaultPrim.missing")

  # --usdchecker-compat raises mapped rules to pxr severities: the same asset
  # flips from warnings-only PASS to FAIL.
  add_test(NAME lusdchecker_parity_compat_pass_without
    COMMAND lusdchecker
            ${CMAKE_CURRENT_SOURCE_DIR}/testdata/parity-stage-metadata.usda)
  add_test(NAME lusdchecker_parity_compat_fails_with
    COMMAND lusdchecker --usdchecker-compat
            ${CMAKE_CURRENT_SOURCE_DIR}/testdata/parity-stage-metadata.usda)
  set_tests_properties(lusdchecker_parity_compat_fails_with PROPERTIES
    WILL_FAIL TRUE)

  # Variant sweep: the defect hides in a NON-selected variant. --composed
  # enumerates all combinations and finds it; --skip-variants must not.
  add_test(NAME lusdchecker_parity_variant_sweep
    COMMAND lusdchecker --composed
            ${CMAKE_CURRENT_SOURCE_DIR}/testdata/parity-variant-error.usda)
  set_tests_properties(lusdchecker_parity_variant_sweep PROPERTIES
    PASS_REGULAR_EXPRESSION "shade.shader.typeMismatch")
  add_test(NAME lusdchecker_parity_variant_skip
    COMMAND lusdchecker --composed --skip-variants
            ${CMAKE_CURRENT_SOURCE_DIR}/testdata/parity-variant-error.usda)
  set_tests_properties(lusdchecker_parity_variant_skip PROPERTIES
    PASS_REGULAR_EXPRESSION "Result: PASSED"
    FAIL_REGULAR_EXPRESSION "shade.shader.typeMismatch")
  add_test(NAME lusdchecker_parity_variant_explicit
    COMMAND lusdchecker --variants quality:bad
            ${CMAKE_CURRENT_SOURCE_DIR}/testdata/parity-variant-error.usda)
  set_tests_properties(lusdchecker_parity_variant_explicit PROPERTIES
    PASS_REGULAR_EXPRESSION "shade.shader.typeMismatch")

  add_test(NAME lusdchecker_parity_dump_rules
    COMMAND lusdchecker --dump-rules)
  set_tests_properties(lusdchecker_parity_dump_rules PROPERTIES
    PASS_REGULAR_EXPRESSION "\\[core:core.layer.defaultPrim.missing\\]")

  # --- sparse array edits (VtArrayEdit, usda 1.2 / crate 0.14) --------------
  # Composition EVALUATES edits like pxr's flatten: the fixture's edit
  # resolves to faceVertexIndices = [0, 1, 99], and index 99 is only
  # detectable if the edit was actually applied.
  add_test(NAME lusdchecker_array_edit_resolved
    COMMAND lusdchecker --composed
            ${CMAKE_CURRENT_SOURCE_DIR}/testdata/array-edit-resolve-invalid.usda)
  set_tests_properties(lusdchecker_array_edit_resolved PROPERTIES
    PASS_REGULAR_EXPRESSION "geom.mesh.topology.index")
  # Without --composed the layer keeps the authored edit unevaluated.
  add_test(NAME lusdchecker_array_edit_authored
    COMMAND lusdchecker
            ${CMAKE_CURRENT_SOURCE_DIR}/testdata/array-edit-resolve-invalid.usda)
  set_tests_properties(lusdchecker_array_edit_authored PROPERTIES
    PASS_REGULAR_EXPRESSION "Result: PASSED"
    FAIL_REGULAR_EXPRESSION "geom.mesh.topology.index")
  add_test(NAME lusdchecker_array_edit_resolved_valid
    COMMAND lusdchecker --composed
            ${CMAKE_CURRENT_SOURCE_DIR}/testdata/array-edit-resolve-valid.usda)
  set_tests_properties(lusdchecker_array_edit_resolved_valid PROPERTIES
    PASS_REGULAR_EXPRESSION "Result: PASSED - no issues found")
  # Crate rep round-trip: the same edit read from a next-written .usdc
  # (pxr-readable), decoded and resolved during composition.
  add_test(NAME lusdchecker_array_edit_usdc
    COMMAND lusdchecker --composed
            ${CMAKE_CURRENT_SOURCE_DIR}/testdata/array-edit-resolve-invalid.usdc)
  set_tests_properties(lusdchecker_array_edit_usdc PROPERTIES
    PASS_REGULAR_EXPRESSION "geom.mesh.topology.index")
  # Edits across a reference arc: WriteRef/insert/maxsize over the referenced
  # base, edit-over-edit stacking, from-end negative indices.
  add_test(NAME lusdchecker_array_edit_compose_arc
    COMMAND lusdchecker --composed
            ${PROJECT_SOURCE_DIR}/tests/usda/array-edit-compose-001.usda)
  set_tests_properties(lusdchecker_array_edit_compose_arc PROPERTIES
    PASS_REGULAR_EXPRESSION "Result: PASSED")
  # next-parser leniency parity with pxr's usda parser: sparse array edits
  # (preserved as raw text), float->int literal coercion, quoted-string asset
  # paths. Each fixture previously failed to parse (exit 2).
  add_test(NAME lusdchecker_parse_array_edit
    COMMAND lusdchecker --groups core
            ${PROJECT_SOURCE_DIR}/tests/usda/array_edit_basic.usda)
  set_tests_properties(lusdchecker_parse_array_edit PROPERTIES
    PASS_REGULAR_EXPRESSION "Result: PASSED")
  add_test(NAME lusdchecker_parse_int_coercion
    COMMAND lusdchecker --groups core
            ${PROJECT_SOURCE_DIR}/tests/usda/int-assign-float-001.usda)
  set_tests_properties(lusdchecker_parse_int_coercion PROPERTIES
    PASS_REGULAR_EXPRESSION "Result: PASSED")
  add_test(NAME lusdchecker_parse_quoted_asset
    COMMAND lusdchecker --groups core
            ${PROJECT_SOURCE_DIR}/tests/usda/assetpath-array-001.usda)
  set_tests_properties(lusdchecker_parse_quoted_asset PROPERTIES
    PASS_REGULAR_EXPRESSION "Result: PASSED")

  # Differential parity harness vs pxr usdchecker over tests/usda.
  # Self-skips when pxr is not installed.
  add_test(NAME lusdchecker_vs_usdchecker
    COMMAND bash ${PROJECT_SOURCE_DIR}/tests/run-lusdchecker-vs-usdchecker.sh)
  set_tests_properties(lusdchecker_vs_usdchecker PROPERTIES
    SKIP_REGULAR_EXPRESSION "SKIP:"
    ENVIRONMENT "LUSDCHECKER_PATH=$<TARGET_FILE:lusdchecker>")
endif()
