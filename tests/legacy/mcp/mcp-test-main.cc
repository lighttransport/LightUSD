#include "acutest.h"

#include "mcp-test.h"

TEST_LIST = {
  // Stage lifecycle
  { "mcp_stage_new_default", mcp_stage_new_default_test },
  { "mcp_stage_new_with_metadata", mcp_stage_new_with_metadata_test },
  { "mcp_stage_info", mcp_stage_info_test },
  { "mcp_stage_load_usda_string", mcp_stage_load_usda_string_test },
  { "mcp_stage_to_string", mcp_stage_to_string_test },

  // Scene graph
  { "mcp_prim_create_root", mcp_prim_create_root_test },
  { "mcp_prim_list_root", mcp_prim_list_root_test },
  { "mcp_prim_get", mcp_prim_get_test },
  { "mcp_prim_rename", mcp_prim_rename_test },
  { "mcp_prim_remove", mcp_prim_remove_test },
  { "mcp_prim_nested", mcp_prim_nested_test },
  { "mcp_attr_list", mcp_attr_list_test },

  // Query
  { "mcp_query_prims_by_type", mcp_query_prims_by_type_test },
  { "mcp_schema_list_types", mcp_schema_list_types_test },
  { "mcp_schema_get_type", mcp_schema_get_type_test },
  { "mcp_search", mcp_search_test },
  { "mcp_tool_list_registration", mcp_tool_list_registration_test },

  // Validation
  { "mcp_validate_data", mcp_validate_data_test },
  { "mcp_validate_groups", mcp_validate_groups_test },
  { "mcp_validate_session_stage", mcp_validate_session_stage_test },
  { "mcp_validate_no_input", mcp_validate_no_input_test },

  { NULL, NULL }
};
