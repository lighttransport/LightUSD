#pragma once

// JSEngineState must be a complete type for Context unique_ptr destruction
#include "tydra/js-script.hh"

// Stage lifecycle
void mcp_stage_new_default_test(void);
void mcp_stage_new_with_metadata_test(void);
void mcp_stage_info_test(void);
void mcp_stage_load_usda_string_test(void);
void mcp_stage_to_string_test(void);

// Scene graph
void mcp_prim_create_root_test(void);
void mcp_prim_list_root_test(void);
void mcp_prim_get_test(void);
void mcp_prim_rename_test(void);
void mcp_prim_remove_test(void);
void mcp_prim_nested_test(void);
void mcp_attr_list_test(void);

// Query
void mcp_query_prims_by_type_test(void);
void mcp_schema_list_types_test(void);
void mcp_schema_get_type_test(void);
void mcp_search_test(void);
void mcp_tool_list_registration_test(void);

// Validation
void mcp_validate_data_test(void);
void mcp_validate_groups_test(void);
void mcp_validate_session_stage_test(void);
void mcp_validate_no_input_test(void);
