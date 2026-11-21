#pragma once

void layer_create_empty_test(void);
void layer_add_primspec_test(void);
void layer_emplace_primspec_test(void);
void layer_replace_primspec_test(void);
void layer_find_primspec_at_test(void);
void layer_copy_resets_lookup_cache_test(void);
void layer_check_unresolved_refs_test(void);
void layer_check_unresolved_payload_test(void);
void layer_check_unresolved_inherits_test(void);
void layer_check_unresolved_specializes_test(void);
void layer_check_unresolved_variant_test(void);
void layer_check_over_primspec_test(void);
void layer_metas_test(void);
void layer_asset_resolution_state_test(void);
void layer_memory_estimation_test(void);
void layer_moved_from_is_valid_test(void);

void layer_find_primspec_at_same_leaf_name_test(void);
void layer_find_primspec_at_cache_full_path_test(void);

void layer_deep_namespace_depth_test(void);
void layer_children_by_parent_test(void);
void layer_max_prim_path_length_test(void);
