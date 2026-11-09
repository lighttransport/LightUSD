/* SPDX-License-Identifier: MIT
 * Copyright 2024-Present Light Transport Entertainment Inc.
 *
 * unicode-xid-c.h -- Unicode identifier character classes (UAX #31).
 */
#ifndef UNICODE_XID_C_H_
#define UNICODE_XID_C_H_

#ifdef __cplusplus
extern "C" {
#endif

/* 1 when codepoint is XID_Start / XID_Continue, 0 otherwise. */
int unicode_xid_is_start(unsigned int codepoint);
int unicode_xid_is_continue(unsigned int codepoint);

#ifdef __cplusplus
}
#endif

#endif /* UNICODE_XID_C_H_ */
