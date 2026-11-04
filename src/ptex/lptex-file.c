/* SPDX-License-Identifier: Apache-2.0 */
/* lptex file opener. Built only when file IO is enabled; memory-only builds
 * (wasm, freestanding) compile lptex.c alone and define LPTEX_NO_FILEIO. */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "lptex.h"
#include "lptex-internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32) && (defined(__unix__) || defined(__APPLE__))
#define LPTEX_POSIX_IO 1
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

/* No real Ptex file approaches 16 GiB; also rejects directories that report
 * a huge size. */
#define LPTEX_MAX_FILE_BYTES ((uint64_t)16 << 30)

typedef struct lptex_file_owner {
  lptex_allocator alloc;
  int mapped;
} lptex_file_owner;

static void* file_default_alloc(void* user, size_t size) {
  (void)user;
  return malloc(size ? size : 1u);
}
static void file_default_free(void* user, void* ptr) {
  (void)user;
  free(ptr);
}

static void release_bytes(void* ctx, const void* data, size_t size) {
  lptex_file_owner* owner = (lptex_file_owner*)ctx;
  lptex_allocator a;
  if (!owner) return;
  a = owner->alloc;
#ifdef LPTEX_POSIX_IO
  if (owner->mapped) {
    munmap((void*)(uintptr_t)data, size);
  } else
#endif
  {
    (void)size;
    a.free(a.user, (void*)(uintptr_t)data);
  }
  a.free(a.user, owner);
}

static lptex_status adopt(void* bytes, size_t size, int mapped,
                          const lptex_options* options, lptex_allocator a,
                          lptex_reader** out) {
  lptex_file_owner* owner =
      (lptex_file_owner*)a.alloc(a.user, sizeof(lptex_file_owner));
  lptex_status st;
  if (!owner) {
#ifdef LPTEX_POSIX_IO
    if (mapped) munmap(bytes, size);
    else
#endif
      a.free(a.user, bytes);
    return LPTEX_E_OUT_OF_MEMORY;
  }
  owner->alloc = a;
  owner->mapped = mapped;
  st = lptex_open_memory(bytes, size, options, out);
  if (st != LPTEX_OK) {
    release_bytes(owner, bytes, size);
    return st;
  }
  lptex_internal_set_release(*out, release_bytes, owner);
  return LPTEX_OK;
}

lptex_status lptex_open_file(const char* path, const lptex_options* options,
                             lptex_reader** out) {
  lptex_allocator a;
  uint8_t* buf;
  size_t size;
  if (!out) return LPTEX_E_INVALID_ARGUMENT;
  *out = NULL;
  if (!path) return LPTEX_E_INVALID_ARGUMENT;
  a.alloc = file_default_alloc;
  a.free = file_default_free;
  a.user = NULL;
  if (options && options->allocator.alloc && options->allocator.free)
    a = options->allocator;

#ifdef LPTEX_POSIX_IO
  {
    struct stat stbuf;
    size_t got = 0;
    void* map;
    const int fd = open(path, O_RDONLY);
    if (fd < 0) return LPTEX_E_OPEN_FAILED;
    if (fstat(fd, &stbuf) != 0) {
      close(fd);
      return LPTEX_E_OPEN_FAILED;
    }
    if (!S_ISREG(stbuf.st_mode) ||
        (uint64_t)stbuf.st_size > LPTEX_MAX_FILE_BYTES
#if SIZE_MAX < UINT64_MAX
        || (uint64_t)stbuf.st_size > (uint64_t)SIZE_MAX
#endif
    ) {
      close(fd);
      return LPTEX_E_FILE_TOO_LARGE;
    }
    if (stbuf.st_size <= 0) {
      close(fd);
      return LPTEX_E_EMPTY_FILE;
    }
    size = (size_t)stbuf.st_size;
    /* Mapping keeps multi-GiB containers out of the heap; only touched
     * metadata and face blocks fault in. */
    map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map != MAP_FAILED) {
      close(fd);
      return adopt(map, size, 1, options, a, out);
    }
    /* Fall back to reading (filesystems that refuse mmap, WASI). */
    buf = (uint8_t*)a.alloc(a.user, size);
    if (!buf) {
      close(fd);
      return LPTEX_E_OUT_OF_MEMORY;
    }
    while (got < size) {
      const size_t want = size - got > ((size_t)1 << 30) ? ((size_t)1 << 30)
                                                          : size - got;
      const ssize_t n = read(fd, buf + got, want);
      if (n <= 0) {
        close(fd);
        a.free(a.user, buf);
        return LPTEX_E_IO;
      }
      got += (size_t)n;
    }
    close(fd);
    return adopt(buf, size, 0, options, a, out);
  }
#else
  {
    FILE* f = fopen(path, "rb");
    size_t got = 0;
#if defined(_MSC_VER)
    __int64 n;
#else
    long n;
#endif
    if (!f) return LPTEX_E_OPEN_FAILED;
#if defined(_MSC_VER)
    if (_fseeki64(f, 0, SEEK_END) != 0) n = -1;
    else n = _ftelli64(f);
#else
    if (fseek(f, 0, SEEK_END) != 0) n = -1;
    else n = ftell(f);
#endif
    if (n < 0) {
      fclose(f);
      return LPTEX_E_IO;
    }
    if (n == 0) {
      fclose(f);
      return LPTEX_E_EMPTY_FILE;
    }
    if ((uint64_t)n > LPTEX_MAX_FILE_BYTES
#if SIZE_MAX < UINT64_MAX
        || (uint64_t)n > (uint64_t)SIZE_MAX
#endif
    ) {
      fclose(f);
      return LPTEX_E_FILE_TOO_LARGE;
    }
    size = (size_t)n;
    if (fseek(f, 0, SEEK_SET) != 0) {
      fclose(f);
      return LPTEX_E_IO;
    }
    buf = (uint8_t*)a.alloc(a.user, size);
    if (!buf) {
      fclose(f);
      return LPTEX_E_OUT_OF_MEMORY;
    }
    while (got < size) {
      const size_t k = fread(buf + got, 1, size - got, f);
      if (k == 0) break;
      got += k;
    }
    fclose(f);
    if (got != size) {
      a.free(a.user, buf);
      return LPTEX_E_IO;
    }
    return adopt(buf, size, 0, options, a, out);
  }
#endif
}
