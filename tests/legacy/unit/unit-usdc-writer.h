// SPDX-License-Identifier: Apache 2.0
// USDC writer unit tests — usdGeom and usdSkel roundtrip coverage

#pragma once

// Geometry
void usdc_writer_mesh_basic_test(void);
void usdc_writer_mesh_subdiv_test(void);
void usdc_writer_mesh_velocities_test(void);
void usdc_writer_points_test(void);
void usdc_writer_basiscurves_test(void);
void usdc_writer_geomsubset_test(void);
void usdc_writer_camera_test(void);
void usdc_writer_primitives_test(void);

// Skeleton
void usdc_writer_skeleton_test(void);
void usdc_writer_skelanimation_test(void);
void usdc_writer_blendshape_test(void);
void usdc_writer_skelroot_test(void);

// Shader / Material
void usdc_writer_shader_terminal_test(void);
void usdc_writer_layer_empty_shader_outputs_test(void);
void usdc_writer_material_outputs_test(void);
void usdc_writer_uvtexture_test(void);
void usdc_writer_inmemory_shader_inputs_test(void);
void usdc_writer_primvarreader_test(void);
void usdc_writer_transform2d_test(void);
void usdc_writer_previewsurface_full_test(void);

// Curves and Points
void usdc_writer_nurbscurves_test(void);
void usdc_writer_pointinstancer_test(void);
void usdc_writer_pointinstancer_prototypes_test(void);

// Instancing
void usdc_writer_instanceable_test(void);

// Lights
void usdc_writer_sphere_light_test(void);
void usdc_writer_distant_light_test(void);
void usdc_writer_dome_light_test(void);
void usdc_writer_light_shadow_shaping_test(void);

// apiSchemas
void usdc_writer_apischemas_test(void);

// MaterialX
void usdc_writer_materialx_config_test(void);

// Path utilities
void path_lessthan_basic_test(void);
void path_lessthan_variant_test(void);
void path_has_prefix_basic_test(void);
void path_has_prefix_variant_test(void);
void path_get_parent_basic_test(void);
void path_get_parent_variant_test(void);

// Path tree roundtrip (USDC)
void path_tree_flat_siblings_test(void);
void path_tree_deep_hierarchy_test(void);
void path_tree_mixed_props_test(void);
void path_tree_variant_basic_test(void);

// Variant roundtrip tests
void usdc_writer_variant_with_props_test(void);
void usdc_writer_variant_multi_selection_test(void);
void usdc_writer_variant_nested_test(void);
void usdc_writer_variant_with_selection_test(void);
void usdc_writer_variant_with_children_test(void);
void usdc_writer_variant_empty_test(void);
void usdc_writer_variant_multiple_sets_test(void);
void usdc_writer_variant_props_and_children_roundtrip_test(void);
void usdc_writer_variant_3level_nested_test(void);
void usdc_writer_variant_nested_with_props_test(void);

// Reproducers for known issues
void usdc_writer_props_asset_roundtrip_test(void);
void usdc_writer_timesamples_double3_test(void);
void usdc_writer_timesamples_scalar_double_test(void);

// Deeper coverage tests
void usdc_writer_asset_array_test(void);
void usdc_writer_timesamples_token_test(void);
void usdc_writer_timesamples_string_test(void);
void usdc_writer_timesamples_int_array_test(void);
void usdc_writer_timesamples_float3_test(void);
void usdc_writer_timesamples_single_sample_test(void);
void usdc_writer_timesamples_with_default_test(void);
void usdc_writer_asset_typed_uvtexture_test(void);

// Edge-case coverage
void usdc_writer_half_scalar_test(void);
void usdc_writer_half_array_test(void);
void usdc_writer_blocked_value_test(void);
void usdc_writer_timesamples_matrix4d_test(void);
void usdc_writer_timesamples_quatf_test(void);
void usdc_writer_uniform_token_array_test(void);
void usdc_writer_customdata_dict_test(void);
void usdc_writer_empty_string_array_test(void);
void usdc_writer_int64_scalar_test(void);
void usdc_writer_color3f_array_test(void);
void usdc_writer_relationship_targets_test(void);
void usdc_writer_attribute_connection_test(void);
void usdc_writer_displayname_test(void);
void usdc_writer_doc_metadata_test(void);
void usdc_writer_nested_prim_paths_test(void);

// Hardening — additional shader connections, timesamples edges, big arrays
void usdc_writer_uvtexture_st_connection_test(void);
void usdc_writer_uvtexture_file_connection_test(void);
void usdc_writer_preview_metallic_connection_test(void);
void usdc_writer_preview_roughness_connection_test(void);
void usdc_writer_timesamples_half_test(void);
void usdc_writer_timesamples_color3f_test(void);
void usdc_writer_timesamples_negative_time_test(void);
void usdc_writer_timesamples_blocked_sample_test(void);
void usdc_writer_timesamples_blocked_array_sample_test(void);
void usdc_writer_large_int_array_test(void);
void usdc_writer_large_float_array_test(void);
void usdc_writer_variant_with_connection_test(void);
void usdc_writer_assetinfo_dict_test(void);
void usdc_writer_attr_displaygroup_test(void);
void usdc_writer_kind_metadata_test(void);
void usdc_writer_specifier_class_test(void);
void usdc_writer_specifier_over_test(void);

// Composition arcs + further hardening
void usdc_writer_inherits_test(void);
void usdc_writer_specializes_test(void);
void usdc_writer_references_test(void);
void usdc_writer_payload_test(void);
void usdc_writer_stage_metadata_test(void);
void usdc_writer_timesamples_half3_test(void);
void usdc_writer_attr_customdata_test(void);
void usdc_writer_attr_hidden_test(void);
void usdc_writer_skeleton_joints_test(void);
void usdc_writer_apischemas_multi_apply_test(void);
void usdc_writer_active_metadata_test(void);
void usdc_writer_hidden_metadata_test(void);
void usdc_writer_instanceable_metadata_test(void);
void usdc_writer_purpose_attribute_test(void);
void usdc_writer_visibility_attribute_test(void);
void usdc_writer_normal_array_test(void);
void usdc_writer_uint_array_test(void);

// More hardening
void usdc_writer_pointinstancer_prototypes_rel_test(void);
void usdc_writer_camera_full_roundtrip_test(void);
void usdc_writer_skelanimation_translations_ts_test(void);
void usdc_writer_scope_nested_test(void);
void usdc_writer_inherits_strict_test(void);
void usdc_writer_specializes_strict_test(void);
void usdc_writer_payload_strict_test(void);

// Reference path-content checks (probes the path-tree-resort bug)
void usdc_writer_references_primpath_test(void);
void usdc_writer_multiple_references_primpath_test(void);
void usdc_writer_payload_primpath_test(void);

// More writer tests
void usdc_writer_comment_metadata_test(void);
void usdc_writer_scenename_metadata_test(void);
void usdc_writer_empty_stage_test(void);
void usdc_writer_unknown_prim_type_test(void);
void usdc_writer_point3d_array_test(void);
void usdc_writer_dome_light_texture_test(void);
void usdc_writer_disk_light_shaping_test(void);
void usdc_writer_mesh_subdiv_creases_test(void);
void usdc_writer_geomsubset_inline_test(void);
void usdc_writer_blendshape_offsets_test(void);
void usdc_writer_basis_curves_full_test(void);
void usdc_writer_multi_shader_material_test(void);
void usdc_writer_variant_with_timesamples_test(void);
void usdc_writer_mesh_primvar_indices_test(void);
void usdc_writer_geom_subdiv_full_test(void);

// Recently fixed regressions
void usdc_writer_shader_generic_inputs_test(void);
void usdc_writer_clips_metadata_test(void);
void usdc_writer_attr_doc_alias_test(void);
void usdc_writer_attr_documentation_test(void);
void usdc_writer_layer_offset_parser_test(void);
void usdc_writer_basiscurves_widths_interpolation_test(void);
void usdc_writer_int64_large_test(void);
void usdc_writer_uint64_large_test(void);
void usdc_writer_quatf_roundtrip_test(void);
void usdc_writer_quatd_roundtrip_test(void);
void usdc_writer_quath_roundtrip_test(void);

// Phase B coverage additions
void usdc_writer_int2_array_test(void);
void usdc_writer_int3_array_test(void);
void usdc_writer_int4_array_test(void);
void usdc_writer_timesamples_int2_test(void);
void usdc_writer_timesamples_int3_test(void);
void usdc_writer_timesamples_int4_test(void);
void usdc_writer_frame4d_test(void);
void usdc_writer_frame4d_array_test(void);
void usdc_writer_attr_metadata_passthrough_test(void);
void usdc_writer_uint64_array_test(void);
void usdc_writer_layer_offset_payload_test(void);
void usdc_writer_scene_name_test(void);
void usdc_writer_customdata_array_types_test(void);
void usdc_writer_uint_tuple_unregistered_test(void);
void usdc_writer_half_role_values_test(void);
void usdc_writer_timecode_value_test(void);
void usdc_writer_matrixf_unregistered_test(void);
void usdc_writer_small_integer_unregistered_test(void);

// Quaternion wire-format coverage (pxr-compat regression fence)
void usdc_writer_quatf_array_roundtrip_test(void);
void usdc_writer_quatd_array_roundtrip_test(void);
void usdc_writer_quath_array_roundtrip_test(void);
void usdc_writer_quat_wire_byteorder_test(void);

// Reference customData out-of-line packing
void usdc_writer_reference_customdata_double_test(void);
void usdc_writer_reference_customdata_large_int64_test(void);
void usdc_writer_reference_customdata_mixed_test(void);

// Attribute meta `allowedTokens` USDC roundtrip
void usdc_writer_allowed_tokens_test(void);

// Shader info:id uniform + UsdPrimvarReader varname-connection (USDC write regression)
void usdc_writer_primvar_reader_varname_connection_test(void);
void usdc_writer_shader_info_id_uniform_test(void);

// UsdTransform2d connected inputs:in (USDC write regression)
void usdc_writer_transform2d_in_connection_test(void);
