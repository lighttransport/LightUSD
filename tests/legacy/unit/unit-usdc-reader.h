// SPDX-License-Identifier: Apache 2.0
// USDC reader unit tests

#pragma once

// Type Roundtrips
void usdc_reader_scalar_types_roundtrip_test(void);
void usdc_reader_string_token_types_roundtrip_test(void);
void usdc_reader_vector_matrix_roundtrip_test(void);
void usdc_reader_array_int_float_roundtrip_test(void);
void usdc_reader_array_string_token_roundtrip_test(void);
void usdc_reader_array_vector_roundtrip_test(void);

// TimeSamples Roundtrips
void usdc_reader_timesamples_scalar_roundtrip_test(void);
void usdc_reader_timesamples_array_roundtrip_test(void);
void usdc_reader_timesamples_blocked_roundtrip_test(void);
void usdc_reader_timesamples_token_roundtrip_test(void);

// Connections & Metadata Roundtrips
void usdc_reader_connection_roundtrip_test(void);
void usdc_reader_relationship_roundtrip_test(void);
void usdc_reader_prim_metadata_roundtrip_test(void);
void usdc_reader_stage_metadata_roundtrip_test(void);

// Hierarchy & Variants
void usdc_reader_nested_hierarchy_roundtrip_test(void);
void usdc_reader_variantset_roundtrip_test(void);

// Binary-Specific
void usdc_reader_large_array_compression_test(void);
void usdc_reader_inlined_scalar_test(void);
void usdc_reader_multiple_prims_roundtrip_test(void);

// Error Handling
void usdc_reader_truncated_input_test(void);
void usdc_reader_layer_memory_limit_test(void);
void usdc_reader_truncated_array_test(void);
void usdc_reader_corrupt_header_test(void);
void usdc_reader_corrupt_body_test(void);

void usdc_reader_collection_binding_roundtrip_test(void);
