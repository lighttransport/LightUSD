#pragma once

// usdz-convert pipeline + texture op + fpnge unit tests

void usdz_convert_png_roundtrip_test(void);
void usdz_convert_resize_test(void);
void usdz_convert_pack_channels_test(void);
void usdz_convert_orm_scalar_fallback_test(void);
void usdz_convert_orm_scalar_nonfinite_test(void);
void usdz_convert_archive_collision_name_test(void);
void usdz_convert_fit_budget_test(void);
void usdz_convert_pipeline_test(void);
void usdz_convert_flat_output_size_stats_test(void);
void usdz_convert_usdz_root_layer_format_test(void);
void usdz_convert_arkit_forces_flattened_usdc_root_test(void);
void usdz_convert_repack_files_test(void);
void usdz_convert_jpeg_roundtrip_test(void);
void usdz_convert_remap_asset_paths_test(void);
void usdz_convert_regression_texture_remap_test(void);
void usdz_convert_regression_material_preview_dedupe_test(void);
void usdz_convert_regression_invalid_index_skip_test(void);
void usdz_convert_material_dedupe_test(void);
void usdz_convert_material_preview_atlas_fallback_test(void);
void usdz_convert_material_preview_transitive_key_test(void);
void usdz_convert_geometry_merge_test(void);
void usdz_convert_geometry_subset_merge_test(void);
void usdz_convert_geometry_subset_partition_test(void);
void usdz_convert_geometry_subset_overlap_skip_test(void);
void usdz_convert_geometry_descendant_material_skip_test(void);
void usdz_convert_geometry_authored_semantics_skip_test(void);
void usdz_convert_geometry_normal_inverse_transpose_test(void);
void usdz_convert_geometry_multi_binding_skip_test(void);
void usdz_convert_geometry_display_constant_size_skip_test(void);
void usdz_convert_error_path_test(void);
void usdz_convert_adversarial_image_test(void);
void usdz_convert_pack_channels_error_test(void);
void usdz_convert_fit_budget_error_test(void);
void usdz_convert_missing_texture_reference_test(void);
void usdz_convert_pipeline_jpeg_test(void);
void usdz_convert_cleanup_test(void);

// EXR encode/decode + fp32 resize (HDR texture support)
void usdz_convert_exr_roundtrip_test(void);
void usdz_convert_resize_float_test(void);

void usdz_convert_udim_bake_test(void);
void usdz_convert_udim_layout_test(void);

void usdz_convert_udim_clip_test(void);
