#pragma once

void minijson_parse_basic_test(void);
void minijson_shortest_double_roundtrip_test(void);
void minijson_unicode_escape_test(void);
void minijson_reject_invalid_utf8_test(void);
void minijson_reject_duplicate_key_test(void);
void minijson_reject_invalid_number_test(void);
void minijson_reject_depth_limit_test(void);
void minijson_reject_input_size_limit_test(void);
void minijson_reject_string_size_limit_test(void);
void minijson_reject_array_size_limit_test(void);
void minijson_reject_object_size_limit_test(void);
void minijson_reject_nonfinite_serialize_test(void);
void minijson_reject_serialize_depth_limit_test(void);
void minijson_serialize_escapes_control_chars_test(void);
void minijson_tydra_value_bridge_test(void);
