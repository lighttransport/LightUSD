#include "tvdb_memory.h"
#include <string.h> // For memset

void tvdb_arena_init(tvdb_arena_allocator_t* arena, void* buffer, size_t buffer_size) {
    arena->buffer = buffer;
    arena->buffer_size = buffer_size;
    arena->current_offset = 0;
}

void tvdb_arena_destroy(tvdb_arena_allocator_t* arena) {
    if (arena->buffer) {
        free(arena->buffer); // Assuming buffer was allocated with malloc
        arena->buffer = NULL;
        arena->buffer_size = 0;
        arena->current_offset = 0;
    }
}

void* tvdb_arena_alloc(tvdb_arena_allocator_t* arena, size_t size) {
    size_t alignment = 8; // Align to 8-byte boundary
    if (!arena || !arena->buffer || arena->current_offset > SIZE_MAX - (alignment - 1)) return NULL;
    size_t aligned_offset = (arena->current_offset + alignment - 1) & ~(alignment - 1);
    size_t required_size = size;

    if (aligned_offset > arena->buffer_size || required_size > arena->buffer_size - aligned_offset) {
        return NULL; // Not enough space
    }

    void* ptr = (char*)arena->buffer + aligned_offset;
    arena->current_offset = aligned_offset + required_size;
    
    // Zero-initialize the allocated memory
    memset(ptr, 0, size);
    
    return ptr;
}

void* tvdb_arena_alloc_aligned(tvdb_arena_allocator_t* arena, size_t size, size_t alignment, int zero) {
    if (!arena || !arena->buffer || alignment < 8 || (alignment & (alignment - 1)) != 0) return NULL;
    if (arena->current_offset > SIZE_MAX - (alignment - 1)) return NULL;
    size_t aligned_offset = (arena->current_offset + alignment - 1) & ~(alignment - 1);
    size_t required_size = size;

    if (aligned_offset > arena->buffer_size || required_size > arena->buffer_size - aligned_offset) {
        return NULL; // Not enough space
    }

    void* ptr = (char*)arena->buffer + aligned_offset;
    arena->current_offset = aligned_offset + required_size;

    if (zero) memset(ptr, 0, size);

    return ptr;
}

void* tvdb_arena_alloc_uninit(tvdb_arena_allocator_t* arena, size_t size, size_t alignment) {
    return tvdb_arena_alloc_aligned(arena, size, alignment, 0);
}

void tvdb_arena_reset(tvdb_arena_allocator_t* arena) {
    arena->current_offset = 0;
}
