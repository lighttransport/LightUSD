#pragma once

// Tests for digit length security guards in ASCII parser
// Note: int32/uint32 guards are partially bypassed by float parsing path
void ascii_parse_int64_valid_test(void);
void ascii_parse_int64_excessive_digits_test(void);
void ascii_parse_uint64_valid_test(void);
void ascii_parse_uint64_excessive_digits_test(void);
void ascii_parse_string_array_test(void);

// Regression tests: '#' comments inside arrays
void ascii_parse_array_comments_int_test(void);
void ascii_parse_array_comments_float_test(void);
void ascii_parse_array_comments_double_test(void);
void ascii_parse_array_comments_matrix4d_test(void);
void ascii_parse_array_comments_token_test(void);
void ascii_parse_array_comments_quatf_test(void);
