#pragma once

// USDA roundtrip tests: parse USDA, export to string, re-parse, compare via JSON
void usda_roundtrip_basic_test(void);
void usda_roundtrip_xform_test(void);
void usda_roundtrip_mesh_test(void);
void usda_roundtrip_material_test(void);
void usda_roundtrip_timesamples_test(void);
void json_layer_primspec_roundtrip_test(void);
void json_writer_facade_test(void);
void json_stage_runtime_roundtrip_test(void);
void usd_dump_json_test(void);
