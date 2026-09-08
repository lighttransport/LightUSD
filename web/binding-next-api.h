/* SPDX-License-Identifier: Apache-2.0 */
#ifndef LIGHTUSD_WEB_NEXT_API_H_
#define LIGHTUSD_WEB_NEXT_API_H_
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Internal browser bridge. All IDs are uint32, including on memory64.
 * call returns an owned emval ID, or zero for invalid handles/methods.
 * args is a borrowed emval ID. Release successful results with val_release.
 * Object handles validate their index, generation and method's class.
 */
uint32_t lightusd_next_create(uint32_t kind);
void lightusd_next_destroy(uint32_t handle);
uint32_t lightusd_next_call(uint32_t handle, uint32_t method, uint32_t args);
void lightusd_next_val_release(uint32_t value);
#ifdef __cplusplus
}
namespace lightusd {
namespace web_next {
void* NextCreateObject(uint32_t kind);
void NextDestroyObject(uint32_t kind, void* object);
uint32_t NextInvokeObject(uint32_t kind, void* object, uint32_t method, uint32_t args);
}
}
#endif
#endif
