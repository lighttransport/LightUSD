// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "layer-registry.hh"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__) && !defined(__wasi__)
#define LIGHTUSD_NEXT_SOURCE_SNAPSHOT_POSIX 1
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace lightusd {
namespace next {
namespace pcp {

// Exact first-read bytes, independent of the original path or reader. The
// private file remains named for portable file-reader/mmap reuse. Every failure
// removes it; live mapped array owners survive its eventual unlink on POSIX.
class LayerSourceSnapshot {
 public:
  ~LayerSourceSnapshot() {
#if defined(LIGHTUSD_NEXT_SOURCE_SNAPSHOT_POSIX)
    if (fd_ >= 0) ::close(fd_);
#endif
    if (!path_.empty()) std::remove(path_.c_str());
  }
  LayerSourceSnapshot(const LayerSourceSnapshot&) = delete;
  LayerSourceSnapshot& operator=(const LayerSourceSnapshot&) = delete;
  const std::string& path() const { return path_; }
  bool content_dispatch() const { return content_dispatch_; }
  void set_options(const LayerLoadOptions& options, bool content_dispatch) {
    options_ = options;
    content_dispatch_ = content_dispatch;
    // Retain semantic limits/modes, never callback captures owned by a past
    // composition cache. A replay receives the current caller's diagnostics.
    options_.progress_callback = {};
    options_.usda_parse_options.progress_callback = {};
    options_.asset_read = {};
    options_.asset_open = {};
  }
  LayerLoadOptions replay_options(const LayerLoadOptions& current) const {
    LayerLoadOptions result = options_;
    result.progress_callback = current.progress_callback;
    result.usda_parse_options.progress_callback = current.usda_parse_options.progress_callback;
    result.enable_timing = current.enable_timing;
    result.parse_num_threads = current.parse_num_threads;
    result.min_parallel_parse_bytes = current.min_parallel_parse_bytes;
    return result;
  }

  static bool supported() {
#if defined(LIGHTUSD_NEXT_SOURCE_SNAPSHOT_POSIX)
    return true;
#else
    return false;
#endif
  }

  static std::shared_ptr<LayerSourceSnapshot> FromMemory(
      const uint8_t* bytes, size_t size, const std::string& directory,
      const std::shared_ptr<AssetPayloadBudget>& budget, std::string* error) {
#if defined(LIGHTUSD_NEXT_SOURCE_SNAPSHOT_POSIX)
    auto result = Create(size, directory, budget, error);
    if (!result) return {};
    const int fd = result->fd_;
    if (!Write(fd, bytes, size) || !result->Seal()) {
      Fail(error, "cannot write source snapshot");
      return {};
    }
    return result;
#else
    (void)bytes; (void)size; (void)directory; (void)budget;
    Fail(error, "source snapshots are unavailable on this platform");
    return {};
#endif
  }

  static std::shared_ptr<LayerSourceSnapshot> FromFile(
      const ResolvedAsset& asset, size_t max_bytes, const std::string& directory,
      const std::shared_ptr<AssetPayloadBudget>& budget, std::string* error) {
#if defined(LIGHTUSD_NEXT_SOURCE_SNAPSHOT_POSIX)
    const int input = ::open(asset.resolved_path.c_str(),
                             O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (input < 0) { Fail(error, "cannot open snapshot source"); return {}; }
    struct Close { int fd; ~Close() { ::close(fd); } } close{input};
    struct stat before;
    if (::fstat(input, &before) != 0 || !S_ISREG(before.st_mode) ||
        before.st_size < 0 ||
        static_cast<uint64_t>(before.st_size) > SIZE_MAX ||
        (max_bytes && static_cast<uint64_t>(before.st_size) > max_bytes) ||
        (asset.has_file_identity &&
         (static_cast<uint64_t>(before.st_dev) != asset.file_device ||
          static_cast<uint64_t>(before.st_ino) != asset.file_inode))) {
      Fail(error, "invalid or changed snapshot source"); return {};
    }
    size_t remaining = static_cast<size_t>(before.st_size);
    auto result = Create(remaining, directory, budget, error);
    if (!result) return {};
    std::array<uint8_t, 64 * 1024> block;
    while (remaining) {
      const size_t count = std::min(remaining, block.size());
      const ssize_t got = ::read(input, block.data(), count);
      if (got < 0 && errno == EINTR) continue;
      if (got <= 0 || !Write(result->fd_, block.data(), static_cast<size_t>(got))) {
        Fail(error, "cannot copy snapshot source"); return {};
      }
      remaining -= static_cast<size_t>(got);
    }
    struct stat after;
    if (::fstat(input, &after) != 0 || before.st_size != after.st_size ||
        before.st_mtime != after.st_mtime || before.st_ctime != after.st_ctime ||
#if defined(__APPLE__)
        before.st_mtimespec.tv_nsec != after.st_mtimespec.tv_nsec ||
        before.st_ctimespec.tv_nsec != after.st_ctimespec.tv_nsec ||
#else
        before.st_mtim.tv_nsec != after.st_mtim.tv_nsec ||
        before.st_ctim.tv_nsec != after.st_ctim.tv_nsec ||
#endif
        !result->Seal()) {
      Fail(error, "snapshot source changed during copy"); return {};
    }
    return result;
#else
    (void)asset; (void)max_bytes; (void)directory; (void)budget;
    Fail(error, "source snapshots are unavailable on this platform");
    return {};
#endif
  }

 private:
  LayerSourceSnapshot() = default;
  static void Fail(std::string* error, const char* message) {
    if (error) *error += std::string(message) + "\n";
  }
  std::string path_;
  LayerLoadOptions options_;
  bool content_dispatch_ = true;
  std::shared_ptr<void> reservation_;
#if defined(LIGHTUSD_NEXT_SOURCE_SNAPSHOT_POSIX)
  int fd_ = -1;

  static std::shared_ptr<LayerSourceSnapshot> Create(
      size_t size, const std::string& directory,
      const std::shared_ptr<AssetPayloadBudget>& budget, std::string* error) {
    auto result = std::shared_ptr<LayerSourceSnapshot>(new LayerSourceSnapshot());
    result->reservation_ = budget->Reserve(size);
    if (size && !result->reservation_) {
      Fail(error, "source snapshot byte limit exceeded"); return {};
    }
    std::string pattern = directory + "/lightusd-source-XXXXXX";
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back('\0');
    result->fd_ = ::mkstemp(name.data());
    if (result->fd_ < 0) { Fail(error, "cannot create source snapshot"); return {}; }
    result->path_ = name.data();
    if (::fcntl(result->fd_, F_SETFD, FD_CLOEXEC) < 0) {
      Fail(error, "cannot protect source snapshot descriptor"); return {};
    }
    return result;
  }
  static bool Write(int fd, const uint8_t* bytes, size_t size) {
    while (size) {
      const size_t chunk = std::min(size, size_t(64 * 1024));
      const ssize_t wrote = ::write(fd, bytes, chunk);
      if (wrote < 0 && errno == EINTR) continue;
      if (wrote <= 0) return false;
      bytes += static_cast<size_t>(wrote);
      size -= static_cast<size_t>(wrote);
    }
    return true;
  }
  bool Seal() {
    // Readers only need the filename now. No persistent writable descriptor.
    const int fd = fd_;
    fd_ = -1;
    return ::close(fd) == 0;
  }
#endif
};

}  // namespace pcp
}  // namespace next
}  // namespace lightusd
