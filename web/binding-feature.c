/* SPDX-License-Identifier: Apache-2.0 */
#include "../src/c-api/lightusd-c.h"
#include "../src/next/c/chunks.h"
#include <emscripten/emscripten.h>
EMSCRIPTEN_KEEPALIVE uint32_t lu_feature_info(uint32_t field) {
  switch (field) {
    case 0: return LIGHTUSD_API_VERSION_MAJOR;
    case 1: return LU_CORE_C_VERSION_MAJOR;
    case 2: return sizeof(void*) * 8;
    case 3: return 1;
    case 4: return sizeof(lu_value);
    case 5: return _Alignof(lu_value);
    case 6: return offsetof(lu_value, store);
    case 7: return sizeof(lu_type_descriptor);
    case 8: return sizeof(lu_chunks);
    case 9: return sizeof(lightusd_value_view);
    case 10: return offsetof(lightusd_value_view, data);
    case 11: return offsetof(lightusd_value_view, _owner);
    default: return 0;
  }
}
