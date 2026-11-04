/* SPDX-License-Identifier: Apache-2.0 */
/* Private hooks shared by lptex.c and lptex-file.c. Not a public API. */
#ifndef LPTEX_INTERNAL_H_
#define LPTEX_INTERNAL_H_

#include "lptex.h"

/* Make the reader own its source bytes: `release` runs in lptex_close(). */
void lptex_internal_set_release(lptex_reader* reader,
                                void (*release)(void* ctx, const void* data,
                                                size_t size),
                                void* ctx);

#endif /* LPTEX_INTERNAL_H_ */
