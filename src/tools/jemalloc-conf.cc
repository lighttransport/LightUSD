// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// Compiled-in jemalloc configuration for the next CLI tools.
//
// jemalloc reads this symbol before /etc/malloc.conf and the MALLOC_CONF
// environment variable, so both still override it. The value comes from the
// LIGHTUSD_NEXT_JEMALLOC_CONF CMake cache variable (default empty, in which
// case this TU is not compiled at all). Example: `thp:always` (transparent
// huge pages) helps mid-size flattens whose working set is far below free
// memory, and hurts very large ones on a THP=madvise host (synchronous
// compaction); see doc/large-scene.md §10. The allocator never changes output
// bytes.
//
// Only linked when the tool links jemalloc; other allocators ignore it.
#ifndef LIGHTUSD_NEXT_JEMALLOC_CONF
#define LIGHTUSD_NEXT_JEMALLOC_CONF ""
#endif

extern "C" {
const char* malloc_conf = LIGHTUSD_NEXT_JEMALLOC_CONF;
}
