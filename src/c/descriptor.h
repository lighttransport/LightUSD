/* SPDX-License-Identifier: Apache-2.0 */
#ifndef LU_DESCRIPTOR_H
#define LU_DESCRIPTOR_H
#include "value.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Compare this revision and LU_CORE_C_VERSION_MAJOR with lu_abi_get() before
 * passing by-value storage across a shared-library boundary. Revision 2 uses
 * uint64_t/double alignment for lu_value.store; revision 1 used max_align_t
 * (64 rather than 48 bytes on 64-bit ABIs with 16-byte max_align_t).
 * Also compare the reported sizes, alignment and offsets for the target ABI.
 * This layout revision is independent of the package release version. */
#define LU_CORE_C_LAYOUT_REVISION 2u

typedef enum lu_component {
  LU_COMP_NONE, LU_COMP_UINT8, LU_COMP_INT32, LU_COMP_UINT32,
  LU_COMP_INT64, LU_COMP_UINT64, LU_COMP_FLOAT16, LU_COMP_FLOAT32,
  LU_COMP_FLOAT64, LU_COMP_UINT16, LU_COMP_INT16
} lu_component;
typedef struct lu_component_info {
  const char* name;
  char format;
  uint8_t size, alignment;
} lu_component_info;
typedef struct lu_type_descriptor {
  uint16_t type;
  uint8_t storage, components;
  uint32_t stride, alignment;
  uint8_t is_array, is_string;
} lu_type_descriptor;
typedef struct lu_abi_info {
  uint32_t major, pointer_bits, layout_revision;
  uint32_t value_size, value_alignment, value_store_offset;
  uint32_t descriptor_size, chunks_size;
} lu_abi_info;

LU_CORE_API const lu_component_info* lu_component_get(uint8_t component);
LU_CORE_API int lu_type_describe(uint16_t type, int array, lu_type_descriptor* out);
LU_CORE_API lu_abi_info lu_abi_get(void);

typedef enum lu_number_kind { LU_NUMBER_SIGNED, LU_NUMBER_UNSIGNED, LU_NUMBER_FLOAT } lu_number_kind;
typedef struct lu_number {
  lu_number_kind kind;
  union { int64_t signed_value; uint64_t unsigned_value; double float_value; } value;
} lu_number;
/* Read flat scalar i. Return zero to abort without changing the destination. */
typedef int (*lu_number_at)(void* context, size_t i, lu_number* out);
LU_CORE_API int lu_value_set_numbers(lu_value*, uint16_t type, int array,
                                    size_t count, lu_number_at, void*, lu_budget*);
#ifdef __cplusplus
}
#endif
#endif
