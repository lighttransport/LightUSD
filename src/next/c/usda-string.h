/* SPDX-License-Identifier: Apache-2.0 */
#ifndef LU_USDA_STRING_H
#define LU_USDA_STRING_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Includes quotes, excludes a terminating NUL. SIZE_MAX means overflow or
 * invalid input. NULL output queries size. A short output is left untouched. */
size_t lu_usda_quote(const char* text, size_t size, char* out, size_t capacity);
#ifdef __cplusplus
}
#endif
#endif
