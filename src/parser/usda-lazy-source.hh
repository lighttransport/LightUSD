// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - retained USDA text source for lazy array slices.

#pragma once

#include "crate/lazy-array.hh"
#include "value-parser.hh"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#if !defined(LIGHTUSD_NO_NATIVE_IO) && !defined(LIGHTUSD_NEXT_NO_MMAP) && !defined(__EMSCRIPTEN__) && \
    !defined(__wasi__) &&                                             \
    (defined(__unix__) || defined(__APPLE__) || defined(__linux__))
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#define LIGHTUSD_NEXT_USDA_LAZY_MMAP 1
#else
#define LIGHTUSD_NEXT_USDA_LAZY_MMAP 0
#endif

namespace lightusd {
namespace next {

// Implemented by value-parser.cc through value-parser-arrays.inc.
ParseResult ParseArrayValueFromSlice(const char* data, size_t len, bool simple,
                                     int nt_hint, TypeId element_type);

class UsdaLazyArraySource final : public LazyArraySource {
 public:
  static std::shared_ptr<UsdaLazyArraySource> AdoptString(std::string&& source) {
    return std::shared_ptr<UsdaLazyArraySource>(
        new UsdaLazyArraySource(std::move(source)));
  }

  static std::shared_ptr<UsdaLazyArraySource> MmapFile(
      const std::string& filename, std::string* error = nullptr) {
#if LIGHTUSD_NEXT_USDA_LAZY_MMAP
    int fd = ::open(filename.c_str(), O_RDONLY);
    if (fd < 0) {
      if (error) *error = "Failed to open file for mmap";
      return nullptr;
    }

    struct stat st;
    if (::fstat(fd, &st) != 0) {
      if (error) *error = "Failed to stat file for mmap";
      ::close(fd);
      return nullptr;
    }
    if (st.st_size < 0) {
      if (error) *error = "Negative file size";
      ::close(fd);
      return nullptr;
    }
    if (st.st_size == 0) {
      ::close(fd);
      return AdoptString(std::string());
    }

    void* mapped = ::mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ,
                          MAP_PRIVATE, fd, 0);
    ::close(fd);
    if (mapped == MAP_FAILED) {
      if (error) *error = "Failed to mmap file";
      return nullptr;
    }

    return std::shared_ptr<UsdaLazyArraySource>(
        new UsdaLazyArraySource(reinterpret_cast<const uint8_t*>(mapped),
                                static_cast<size_t>(st.st_size)));
#else
    (void)filename;
    if (error) *error = "USDA lazy mmap is not available on this platform";
    return nullptr;
#endif
  }

  ~UsdaLazyArraySource() override {
#if LIGHTUSD_NEXT_USDA_LAZY_MMAP
    if (mapped_) {
      ::munmap(const_cast<uint8_t*>(mapped_), size_);
    }
#endif
  }

  UsdaLazyArraySource(const UsdaLazyArraySource&) = delete;
  UsdaLazyArraySource& operator=(const UsdaLazyArraySource&) = delete;

  bool MaterializeArray(const LazyArrayRef& ref, Value* out) const override {
    if (!out || !base()) return false;
    const uint64_t begin = ref.block_offset;
    const uint64_t len = ref.block_len;

    if (begin > size_ || len > size_ - begin) return false;

    const char* data =
        reinterpret_cast<const char*>(base() + static_cast<size_t>(begin));
    ParseResult result = ParseArrayValueFromSlice(
        data, static_cast<size_t>(len), false, /*nt_hint=*/0, ref.value_type);
    if (!result.success) return false;

    *out = std::move(result.value);
    return true;
  }

  const uint8_t* base() const override {
    if (mapped_) return mapped_;
    return reinterpret_cast<const uint8_t*>(owned_.data());
  }

  size_t size() const override { return size_; }

  CrateVersion version() const override { return CrateVersion{}; }

  bool is_mmapped() const override { return mapped_ != nullptr; }

  bool can_borrow() const override { return false; }

  void DiscardRange(uint64_t offset, uint64_t length) const override {
#if LIGHTUSD_NEXT_USDA_LAZY_MMAP
    if (!mapped_ || length == 0 || offset >= size_) return;
    uint64_t end = offset + length;
    if (end < offset || end > size_) end = size_;
    const long page = ::sysconf(_SC_PAGESIZE);
    if (page <= 0) return;
    const uint64_t page_size = static_cast<uint64_t>(page);
    const uint64_t aligned_begin = offset & ~(page_size - 1u);
    const uint64_t aligned_end = (end + page_size - 1u) & ~(page_size - 1u);
    if (aligned_end <= aligned_begin || aligned_begin >= size_) return;
    const uint64_t clamped_end =
        aligned_end > size_ ? static_cast<uint64_t>(size_) : aligned_end;
    (void)::madvise(const_cast<uint8_t*>(mapped_ + aligned_begin),
                   static_cast<size_t>(clamped_end - aligned_begin),
                   MADV_DONTNEED);
#else
    (void)offset;
    (void)length;
#endif
  }

 private:
  explicit UsdaLazyArraySource(std::string&& source)
      : owned_(std::move(source)), size_(owned_.size()) {}

  UsdaLazyArraySource(const uint8_t* mapped, size_t size)
      : mapped_(mapped), size_(size) {}

  std::string owned_;
  const uint8_t* mapped_ = nullptr;
  size_t size_ = 0;
};

}  // namespace next
}  // namespace lightusd
