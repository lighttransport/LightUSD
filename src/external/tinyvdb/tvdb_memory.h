#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h> // For size_t

typedef struct {
    void* buffer;
    size_t buffer_size;
    size_t current_offset;
} tvdb_arena_allocator_t;

// Initialize an arena allocator with a pre-allocated buffer.
// `buffer` should point to a memory block of `buffer_size` bytes.
// The allocator assumes ownership of the buffer pointer and will free it on destroy.
void tvdb_arena_init(tvdb_arena_allocator_t* arena, void* buffer, size_t buffer_size);

// Destroy the arena allocator and free its buffer.
void tvdb_arena_destroy(tvdb_arena_allocator_t* arena);

// Allocate memory from the arena. Returns NULL if allocation fails.
// Basic alignment is handled to align to 8-byte boundaries.
void* tvdb_arena_alloc(tvdb_arena_allocator_t* arena, size_t size);

// Allocate without zero-filling, with a caller-chosen alignment (8,16,32,64).
// Use for large dense buffers that the caller fully overwrites; avoids a
// redundant memset and gives SIMD kernels aligned rows. The arena's base
// buffer must itself be aligned to `alignment` for the guarantee to hold.
void* tvdb_arena_alloc_uninit(tvdb_arena_allocator_t* arena, size_t size,
                              size_t alignment);

// Reset the arena allocator, making all previously allocated memory available again.
// This is useful for reusing the same arena for multiple operations within a scope.
void tvdb_arena_reset(tvdb_arena_allocator_t* arena);
