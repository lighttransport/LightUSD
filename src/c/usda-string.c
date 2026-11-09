/* SPDX-License-Identifier: Apache-2.0 */
#include "usda-string.h"
#include <stdint.h>

size_t lu_usda_quote(const char* text, size_t size, char* out, size_t capacity) {
  static const char hex[] = "0123456789abcdef";
  size_t required = 2;
  if (!text && size) return SIZE_MAX;
  for (size_t i = 0; i < size; ++i) {
    unsigned char c = (unsigned char)text[i];
    size_t n = c == '"' || c == '\\' || c == '\n' || c == '\r' || c == '\t'
        ? 2 : (c < 0x20 || c == 0x7f ? 4 : 1);
    if (n > SIZE_MAX - required) return SIZE_MAX;
    required += n;
  }
  if (!out || capacity < required) return required;
  size_t j = 0;
  out[j++] = '"';
  for (size_t i = 0; i < size; ++i) {
    unsigned char c = (unsigned char)text[i];
    if (c == '"' || c == '\\' || c == '\n' || c == '\r' || c == '\t') {
      out[j++] = '\\';
      out[j++] = c == '\n' ? 'n' : c == '\r' ? 'r' : c == '\t' ? 't' : (char)c;
    } else if (c < 0x20 || c == 0x7f) {
      out[j++] = '\\'; out[j++] = 'x';
      out[j++] = hex[c >> 4]; out[j++] = hex[c & 15];
    } else out[j++] = (char)c;
  }
  out[j] = '"';
  return required;
}
