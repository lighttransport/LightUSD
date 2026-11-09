// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - Crate data source implementation

#include "crate-data-source.hh"

#include "lazy-array.hh"
#include "crate-allocation-budget.hh"
#include "core/safe-arithmetic.hh"
#include "../safe-file-size.hh"
#include "stream-reader.hh"
#include "../types/value.hh"
#include "../types/array-builder.hh"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <new>
#include <limits>
#include <type_traits>

// Posix mmap is used to back a CrateDataSource directly off the file (Phase
// 8.3). WASM (emscripten/wasi) and non-posix platforms keep the owned-buffer
// path; MmapFile() then returns nullptr and the reader falls back.
#if !defined(LIGHTUSD_NO_NATIVE_IO) && !defined(LIGHTUSD_NEXT_NO_MMAP) && !defined(__EMSCRIPTEN__) && \
    !defined(__wasi__) &&                                         \
    (defined(__unix__) || defined(__APPLE__) || defined(__linux__))
#define LIGHTUSD_NEXT_HAVE_MMAP 1
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#endif

namespace lightusd {
namespace next {

// ============================================================
// Paged secure snapshot
// ============================================================

#if defined(LIGHTUSD_NEXT_HAVE_MMAP)
namespace {

// The parse copy is either new[] or a private anonymous mapping (>= 4 MiB),
// where the mapping is 2 MiB-aligned and THP-advised so a hundreds-of-MiB
// copy does not pay one minor fault per 4 KiB page.
std::unique_ptr<uint8_t[], CrateSnapshotDeleter> AllocateSnapshotBuffer(size_t n) {
  constexpr size_t kHugeBytes = size_t(2) << 20;
  constexpr size_t kHugeMin = size_t(4) << 20;
  if (n >= kHugeMin &&
      n <= (std::numeric_limits<size_t>::max)() - 2 * kHugeBytes) {
    const size_t span = ((n + kHugeBytes - 1) & ~(kHugeBytes - 1)) + kHugeBytes;
    void* raw = ::mmap(nullptr, span, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw != MAP_FAILED) {
      const uintptr_t aligned =
          (reinterpret_cast<uintptr_t>(raw) + kHugeBytes - 1) & ~(kHugeBytes - 1);
#if defined(MADV_HUGEPAGE)
      (void)::madvise(reinterpret_cast<void*>(aligned), span - kHugeBytes,
                      MADV_HUGEPAGE);
#endif
      return std::unique_ptr<uint8_t[], CrateSnapshotDeleter>(
          reinterpret_cast<uint8_t*>(aligned),
          CrateSnapshotDeleter{span, raw});
    }
  }
  return std::unique_ptr<uint8_t[], CrateSnapshotDeleter>(
      new (std::nothrow) uint8_t[n], CrateSnapshotDeleter{0, nullptr});
}

bool ReadWholeFile(int fd, size_t n, uint8_t* dst, std::string* err) {
  size_t done = 0;
  while (done < n) {
    const size_t want = std::min<size_t>(n - done, size_t(1) << 30);
    const ssize_t got =
        ::pread(fd, dst + done, want, static_cast<off_t>(done));
    if (got < 0) {
      if (errno == EINTR) continue;
      if (err) *err = "Failed to read file contents";
      return false;
    }
    if (got == 0) {
      if (err) *err = "File shrank while being read";
      return false;
    }
    done += static_cast<size_t>(got);
  }
  // A consistent snapshot must also not have grown past its fstat size.
  uint8_t extra = 0;
  ssize_t more = 0;
  do {
    more = ::pread(fd, &extra, 1, static_cast<off_t>(n));
  } while (more < 0 && errno == EINTR);
  if (more != 0) {
    if (err) *err = "File changed while being read";
    return false;
  }
  return true;
}

}  // namespace
#endif  // LIGHTUSD_NEXT_HAVE_MMAP

/// Process-wide append-only snapshot arena. One descriptor serves every paged
/// layer: a scene can reference thousands of layers, and one fd per layer would
/// hit the descriptor limit (the mmap path avoids that for the same reason).
#if defined(LIGHTUSD_NEXT_HAVE_MMAP)
class CrateSnapshotArena {
 public:
  ~CrateSnapshotArena() {
    if (fd_ >= 0) ::close(fd_);
    if (!path_.empty()) (void)::unlink(path_.c_str());
  }
  CrateSnapshotArena(const CrateSnapshotArena&) = delete;
  CrateSnapshotArena& operator=(const CrateSnapshotArena&) = delete;

  /// Find or create the arena for `directory` (empty = `$TMPDIR`, then
  /// `/tmp`). Returns nullptr when the directory cannot host a snapshot.
  static std::shared_ptr<CrateSnapshotArena> Acquire(
      const std::string& directory, std::string* err) {
    std::string dir = directory;
    if (dir.empty()) {
      const char* env = std::getenv("TMPDIR");
      dir = (env && *env) ? env : "/tmp";
    }
    while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
    std::lock_guard<std::mutex> lock(RegistryMutex());
    auto& registry = Registry();
    auto it = registry.find(dir);
    if (it != registry.end()) {
      if (auto existing = it->second.lock()) return existing;
      registry.erase(it);
    }
    auto arena = Create(dir, err);
    if (!arena) return nullptr;
    registry.emplace(std::move(dir), arena);
    return arena;
  }

  /// Append one layer's captured bytes; `*offset` receives its arena position.
  /// Committed ranges are immutable, so concurrent readers need no lock.
  bool Append(const uint8_t* bytes, size_t n, uint64_t* offset,
              std::string* err) {
    if (!offset) return false;
    std::lock_guard<std::mutex> lock(mu_);
    if (n > (std::numeric_limits<uint64_t>::max)() - size_ ||
        size_ > static_cast<uint64_t>((std::numeric_limits<off_t>::max)()) - n) {
      if (err) *err = "snapshot arena byte range overflow";
      return false;
    }
    // One arena can cover a whole scene closure; do not start an append that
    // cannot fit and then fail midway.
    struct statvfs vfs;
    if (::statvfs(dir_.c_str(), &vfs) == 0) {
      const uint64_t avail = static_cast<uint64_t>(vfs.f_bavail) *
                             static_cast<uint64_t>(vfs.f_frsize);
      if (avail < static_cast<uint64_t>(n) + (uint64_t(16) << 20)) {
        if (err) *err = "snapshot arena has insufficient free space";
        return false;
      }
    }
    size_t done = 0;
    while (done < n) {
      const size_t want = std::min<size_t>(n - done, size_t(1) << 30);
      const ssize_t put = ::pwrite(fd_, bytes + done, want,
                                   static_cast<off_t>(size_ + done));
      if (put < 0) {
        if (errno == EINTR) continue;
        if (err) *err = "Failed to write snapshot arena";
        return false;
      }
      if (put == 0) {
        if (err) *err = "Failed to write snapshot arena";
        return false;
      }
      done += static_cast<size_t>(put);
    }
    *offset = size_;
    size_ += n;
    return true;
  }

  bool Read(uint64_t offset, size_t length, uint8_t* dst) const {
    if (fd_ < 0) return false;
    size_t done = 0;
    while (done < length) {
      const size_t want = std::min<size_t>(length - done, size_t(1) << 30);
      ssize_t got = 0;
      do {
        got = ::pread(fd_, dst + done, want,
                      static_cast<off_t>(offset + done));
      } while (got < 0 && errno == EINTR);
      if (got <= 0) return false;
      done += static_cast<size_t>(got);
    }
    return true;
  }

 private:
  CrateSnapshotArena(int fd, std::string dir, std::string path)
      : fd_(fd), dir_(std::move(dir)), path_(std::move(path)) {}

  static std::shared_ptr<CrateSnapshotArena> Create(const std::string& dir,
                                                    std::string* err) {
    std::string pattern = dir + "/lightusd-crate-XXXXXX";
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back('\0');
    const int fd = ::mkstemp(name.data());
    if (fd < 0) {
      if (err) *err = "cannot create snapshot arena in " + dir;
      return nullptr;
    }
    (void)::fcntl(fd, F_SETFD, FD_CLOEXEC);
    std::string path;
    // Unlink immediately: the descriptor is the only handle.
    if (::unlink(name.data()) != 0) path = name.data();
    return std::shared_ptr<CrateSnapshotArena>(
        new CrateSnapshotArena(fd, dir, std::move(path)));
  }

  static std::mutex& RegistryMutex() {
    static std::mutex mutex;
    return mutex;
  }
  static std::map<std::string, std::weak_ptr<CrateSnapshotArena>>& Registry() {
    static std::map<std::string, std::weak_ptr<CrateSnapshotArena>> registry;
    return registry;
  }

  int fd_;
  std::string dir_;
  std::string path_;  // non-empty only when unlink() at creation failed
  std::mutex mu_;
  uint64_t size_ = 0;
};
#endif  // LIGHTUSD_NEXT_HAVE_MMAP

/// One layer's slice of the snapshot arena. `pread` is positional and
/// thread-safe, so materializations from parallel readers need no lock.
#if defined(LIGHTUSD_NEXT_HAVE_MMAP)
class CrateSnapshotPager {
 public:
  CrateSnapshotPager(std::shared_ptr<CrateSnapshotArena> arena, uint64_t offset,
                     uint64_t size)
      : arena_(std::move(arena)), offset_(offset), size_(size) {}
  CrateSnapshotPager(const CrateSnapshotPager&) = delete;
  CrateSnapshotPager& operator=(const CrateSnapshotPager&) = delete;

  uint64_t size() const { return size_; }

  bool Read(uint64_t offset, size_t length, uint8_t* dst) const {
    if (!arena_ || offset > size_ ||
        static_cast<uint64_t>(length) > size_ - offset) {
      return false;
    }
    return arena_->Read(offset_ + offset, length, dst);
  }

 private:
  std::shared_ptr<CrateSnapshotArena> arena_;
  uint64_t offset_;
  uint64_t size_;
};
#else
/// Paged snapshots are POSIX-only; the owned path is used everywhere else.
class CrateSnapshotPager {
 public:
  uint64_t size() const { return 0; }
  bool Read(uint64_t, size_t, uint8_t*) const { return false; }
};
#endif  // LIGHTUSD_NEXT_HAVE_MMAP



// ============================================================
// CrateDataSource
// ============================================================

std::shared_ptr<CrateDataSource> CrateDataSource::Adopt(
    std::string&& bytes, CrateVersion version, std::vector<std::string>&& tokens,
    std::vector<uint32_t>&& string_indices) {
  // Can't use make_shared with a private ctor; construct then wrap.
  std::shared_ptr<CrateDataSource> ds(new CrateDataSource());
  ds->bytes_ = std::move(bytes);
  ds->version_ = version;
  ds->tokens_ = std::move(tokens);
  ds->string_indices_ = std::move(string_indices);
  return ds;
}

std::shared_ptr<CrateDataSource> CrateDataSource::Adopt(std::string&& bytes,
                                                        CrateVersion version) {
  std::shared_ptr<CrateDataSource> ds(new CrateDataSource());
  ds->bytes_ = std::move(bytes);
  ds->version_ = version;
  return ds;
}

std::shared_ptr<CrateDataSource> CrateDataSource::MmapFile(
    const std::string& filename) {
#if defined(LIGHTUSD_NEXT_HAVE_MMAP)
  int fd = ::open(filename.c_str(), O_RDONLY);
  if (fd < 0) return nullptr;
  struct stat st;
  if (::fstat(fd, &st) != 0 || st.st_size <= 0) {
    ::close(fd);
    return nullptr;
  }
  const size_t len = static_cast<size_t>(st.st_size);
  void* addr = ::mmap(nullptr, len, PROT_READ, MAP_PRIVATE, fd, 0);
  ::close(fd);  // the mapping survives the descriptor being closed.
  if (addr == MAP_FAILED) return nullptr;
  std::shared_ptr<CrateDataSource> ds(new CrateDataSource());
  ds->mmap_addr_ = addr;
  ds->mmap_size_ = len;
  ds->mmap_base_ = reinterpret_cast<const uint8_t*>(addr);
  ds->mmap_path_ = filename;
  return ds;
#else
  (void)filename;
  return nullptr;
#endif
}

std::shared_ptr<CrateDataSource> CrateDataSource::ReadFileSnapshot(
    const std::string& filename, uint64_t max_bytes, bool* unavailable,
    std::string* err) {
  if (unavailable) *unavailable = false;
#if defined(LIGHTUSD_NEXT_HAVE_MMAP)
  auto fail = [&](const std::string& message) {
    if (err) *err = message;
    return std::shared_ptr<CrateDataSource>();
  };
  int fd = -1;
  do {
    fd = ::open(filename.c_str(), O_RDONLY | O_CLOEXEC);
  } while (fd < 0 && errno == EINTR);
  if (fd < 0) return fail("Failed to open file: " + filename);
  struct CloseFd {
    int fd;
    ~CloseFd() { ::close(fd); }
  } close_fd{fd};
  struct stat st;
  if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
    return fail("Invalid or oversized file size");
  }
  const uint64_t cap = max_bytes ? max_bytes : kMaxReadableFileBytes;
  const uint64_t n64 = static_cast<uint64_t>(st.st_size);
  if (max_bytes && n64 > max_bytes) {
    return fail("File exceeds max_memory budget");
  }
  if (n64 > cap ||
      n64 > static_cast<uint64_t>((std::numeric_limits<size_t>::max)()) ||
      n64 > static_cast<uint64_t>((std::numeric_limits<off_t>::max)())) {
    return fail("Invalid or oversized file size");
  }
  const size_t n = static_cast<size_t>(n64);
  // Every byte is written by the read below before any use, so the buffer is
  // never zero-filled.
  auto buf = AllocateSnapshotBuffer(n);
  if (!buf) return fail("Failed to allocate file snapshot");
  std::string read_err;
  if (!ReadWholeFile(fd, n, buf.get(), &read_err)) {
    return fail(read_err + ": " + filename);
  }
  std::shared_ptr<CrateDataSource> ds(new CrateDataSource());
  ds->snapshot_ = std::move(buf);
  ds->snapshot_size_ = n;
  return ds;
#else
  (void)filename;
  (void)max_bytes;
  (void)err;
  if (unavailable) *unavailable = true;
  return nullptr;
#endif
}

std::shared_ptr<CrateDataSource> CrateDataSource::PagedFileSnapshot(
    const std::string& filename, uint64_t max_bytes,
    const std::string& temp_directory, bool* unavailable, std::string* err) {
  if (unavailable) *unavailable = false;
#if defined(LIGHTUSD_NEXT_HAVE_MMAP)
  auto fail = [&](const std::string& message) {
    if (err) *err = message;
    return std::shared_ptr<CrateDataSource>();
  };
  auto owned_fallback =
      [](std::unique_ptr<uint8_t[], CrateSnapshotDeleter> buf, size_t n) {
        auto ds = std::shared_ptr<CrateDataSource>(new CrateDataSource());
        ds->snapshot_ = std::move(buf);
        ds->snapshot_size_ = n;
        return ds;
      };

  int fd = -1;
  do {
    fd = ::open(filename.c_str(), O_RDONLY | O_CLOEXEC);
  } while (fd < 0 && errno == EINTR);
  if (fd < 0) return fail("Failed to open file: " + filename);
  struct CloseFd {
    int fd;
    ~CloseFd() { ::close(fd); }
  } close_fd{fd};
  struct stat st;
  if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
    return fail("Invalid or oversized file size");
  }
  const uint64_t cap = max_bytes ? max_bytes : kMaxReadableFileBytes;
  const uint64_t n64 = static_cast<uint64_t>(st.st_size);
  if (max_bytes && n64 > max_bytes) {
    return fail("File exceeds max_memory budget");
  }
  if (n64 > cap ||
      n64 > static_cast<uint64_t>((std::numeric_limits<size_t>::max)()) ||
      n64 > static_cast<uint64_t>((std::numeric_limits<off_t>::max)())) {
    return fail("Invalid or oversized file size");
  }
  const size_t n = static_cast<size_t>(n64);
  // Capture the source exactly once. The captured bytes double as the parse
  // buffer; after parsing they are released and only the snapshot descriptor
  // remains for ranged materialization.
  auto buf = AllocateSnapshotBuffer(n);
  if (!buf) return fail("Failed to allocate file snapshot");
  std::string read_err;
  if (!ReadWholeFile(fd, n, buf.get(), &read_err)) {
    return fail(read_err + ": " + filename);
  }

  auto arena = CrateSnapshotArena::Acquire(temp_directory, err);
  if (!arena) {
    // The platform is paging-capable but the destination is not usable: keep
    // the bytes we already captured rather than rereading the source.
    return owned_fallback(std::move(buf), n);
  }
  uint64_t arena_offset = 0;
  if (!arena->Append(buf.get(), n, &arena_offset, err)) {
    return owned_fallback(std::move(buf), n);
  }

  auto ds = std::shared_ptr<CrateDataSource>(new CrateDataSource());
  ds->pager_ = std::make_shared<CrateSnapshotPager>(std::move(arena),
                                                    arena_offset, n64);
  ds->paged_size_ = n;
  ds->paged_parse_ = std::move(buf);
  return ds;
#else
  (void)filename;
  (void)max_bytes;
  (void)temp_directory;
  (void)err;
  if (unavailable) *unavailable = true;
  return nullptr;
#endif
}

void CrateDataSource::ReleaseParseBuffer() {
  if (pager_) paged_parse_.reset();
}

bool CrateDataSource::ReadRange(uint64_t offset, size_t length,
                                std::vector<uint8_t>* out) const {
  if (!out || !pager_) return false;
  if (offset > pager_->size() ||
      static_cast<uint64_t>(length) > pager_->size() - offset) {
    return false;
  }
  out->resize(length);
  if (length == 0) return true;
  return pager_->Read(offset, length, out->data());
}

void CrateSnapshotDeleter::operator()(uint8_t* p) const noexcept {
  if (!p) return;
#if defined(LIGHTUSD_NEXT_HAVE_MMAP)
  if (mapped_bytes) {
    ::munmap(mapped_base, mapped_bytes);
    return;
  }
#endif
  delete[] p;
}

bool CrateDataSource::MappedFileShrank(size_t* current_size) const {
#if defined(LIGHTUSD_NEXT_HAVE_MMAP)
  if (!mmap_base_ || mmap_path_.empty()) return false;
  struct stat st;
  if (::stat(mmap_path_.c_str(), &st) != 0 || st.st_size < 0) return false;
  const size_t now = static_cast<size_t>(st.st_size);
  if (current_size) *current_size = now;
  return now < mmap_size_;
#else
  (void)current_size;
  return false;
#endif
}

CrateDataSource::~CrateDataSource() {
#if defined(LIGHTUSD_NEXT_HAVE_MMAP)
  if (mmap_addr_) {
    ::munmap(mmap_addr_, mmap_size_);
    mmap_addr_ = nullptr;
  }
#endif
}

void CrateDataSource::DiscardRange(uint64_t offset, uint64_t length) const {
#if defined(LIGHTUSD_NEXT_HAVE_MMAP)
  if (!mmap_addr_ || length == 0 || offset >= mmap_size_) return;
  uint64_t end = offset + length;
  if (end < offset || end > mmap_size_) end = mmap_size_;
  const long page = ::sysconf(_SC_PAGESIZE);
  if (page <= 0) return;
  const uint64_t page_size = static_cast<uint64_t>(page);
  const uint64_t aligned_begin = offset & ~(page_size - 1u);
  const uint64_t aligned_end = (end + page_size - 1u) & ~(page_size - 1u);
  if (aligned_end <= aligned_begin || aligned_begin >= mmap_size_) return;
  const uint64_t clamped_end =
      aligned_end > mmap_size_ ? static_cast<uint64_t>(mmap_size_) : aligned_end;
  (void)::madvise(const_cast<uint8_t*>(mmap_base_ + aligned_begin),
                 static_cast<size_t>(clamped_end - aligned_begin),
                 MADV_DONTNEED);
#else
  (void)offset;
  (void)length;
#endif
}

bool CrateDataSource::MaterializeArray(const LazyArrayRef& ref, Value* out) const {
  if (!out) return false;
  const uint64_t size_limit =
      static_cast<uint64_t>((std::numeric_limits<size_t>::max)());
  const size_t ref_limit = ref.max_elements > size_limit
                               ? (std::numeric_limits<size_t>::max)()
                               : static_cast<size_t>(ref.max_elements);
  const size_t max_elements = std::min(ref_limit, max_array_elements_);
  if (const uint8_t* resident = base()) {
    return DecodeCrateArray(resident, size(), ref.rep, version_, tokens_,
                            max_elements, out, allocation_budget_.get());
  }
  if (!pager_) return false;
  // Empty arrays carry no block. Decode them from a zero-length window.
  if (ref.rep.payload() == 0 || ref.block_len == 0) {
    static const uint8_t kEmpty = 0;
    return DecodeCrateArrayAt(&kEmpty, 0, ref.rep, 0, version_, tokens_,
                              max_elements, out, allocation_budget_.get());
  }
  if (ref.block_len > static_cast<uint64_t>(size_limit)) return false;
  std::vector<uint8_t> block;
  if (!ReadRange(ref.block_offset, static_cast<size_t>(ref.block_len), &block)) {
    return false;
  }
  return DecodeCrateArrayAt(block.data(), block.size(), ref.rep,
                            ref.block_offset, version_, tokens_, max_elements,
                            out, allocation_budget_.get());
}

// ============================================================
// Type metadata
// ============================================================

uint32_t CrateArrayElemStride(CrateTypeId id) {
  switch (id) {
    case CrateTypeId::Bool:
    case CrateTypeId::UChar:
      return 1;
    case CrateTypeId::Half:
      return 2;
    case CrateTypeId::Int:
    case CrateTypeId::UInt:
    case CrateTypeId::Float:
    case CrateTypeId::Token:
    case CrateTypeId::String:
    case CrateTypeId::AssetPath:
    case CrateTypeId::PathExpression:
      return 4;
    case CrateTypeId::Int64:
    case CrateTypeId::UInt64:
    case CrateTypeId::Double:
    case CrateTypeId::TimeCode:
    case CrateTypeId::Vec2f:
    case CrateTypeId::Vec2i:
    case CrateTypeId::Vec4h:
    case CrateTypeId::Quath:
      return 8;
    case CrateTypeId::Vec3f:
    case CrateTypeId::Vec3i:
      return 12;
    case CrateTypeId::Vec4f:
    case CrateTypeId::Vec4i:
    case CrateTypeId::Vec2d:
    case CrateTypeId::Quatf:
      return 16;
    case CrateTypeId::Vec2h:
      return 4;
    case CrateTypeId::Vec3h:
      return 6;
    case CrateTypeId::Vec3d:
      return 24;
    case CrateTypeId::Vec4d:
    case CrateTypeId::Quatd:
    case CrateTypeId::Matrix2d:
      return 32;
    case CrateTypeId::Matrix3d:
      return 72;
    case CrateTypeId::Matrix4d:
      return 128;
    default:
      return 0;
  }
}

uint32_t CrateArrayCountHeaderBytes(CrateVersion version) {
  // Before 0.5 the uncompressed block starts with a u32 rank (always 1),
  // followed by a u32 element count. 0.5/0.6 dropped the rank; 0.7 widened
  // the count to u64. Borrowing/printing need the complete header width.
  if (version.major == 0 && version.minor < 5) return 8u;
  // pxr crateFile.cpp _Write/_ReadUncompressedArray (and the compressed-array
  // paths): `(ver < CrateFile::Version(0,7,0)) ? <uint32_t> : <uint64_t>`.
  return version.is_pre_070() ? 4u : 8u;
}

bool ReadCrateArrayCount(StreamReader& r, CrateVersion version,
                         uint64_t* count) {
  if (!count) return false;
  if (version.major == 0 && version.minor < 5) {
    uint32_t rank = 0;
    if (!r.read_u32(rank) || rank != 1) return false;
  }
  if (version.is_pre_070()) {
    uint32_t c32 = 0;
    if (!r.read_u32(c32)) return false;
    *count = c32;
    return true;
  }
  return r.read_u64(*count);
}

bool CrateArrayTypeCanBeLazy(CrateTypeId id, bool compressed) {
  (void)compressed;
  switch (id) {
    case CrateTypeId::UChar:
    case CrateTypeId::Int:
    case CrateTypeId::UInt:
      return true;
    case CrateTypeId::Float:
    case CrateTypeId::Vec2f:
    case CrateTypeId::Vec3f:
    case CrateTypeId::Vec4f:
    case CrateTypeId::Double:
    case CrateTypeId::Vec2d:
    case CrateTypeId::Vec3d:
    case CrateTypeId::Vec4d:
    case CrateTypeId::Matrix2d:
    case CrateTypeId::Matrix3d:
    case CrateTypeId::Matrix4d:
    case CrateTypeId::Half:
    case CrateTypeId::Vec2h:
    case CrateTypeId::Vec3h:
    case CrateTypeId::Vec4h:
    case CrateTypeId::Int64:
    case CrateTypeId::UInt64:
    case CrateTypeId::Bool:
    case CrateTypeId::TimeCode:
      return true;
    case CrateTypeId::Quatf:
    case CrateTypeId::Quatd:
    case CrateTypeId::Quath:
      // These remain deferred, but cannot be exposed as a direct flat view:
      // crate stores imaginary-first while Value uses real-first. Deferred
      // materialization performs the required per-element swizzle.
      return true;
    default:
      return false;
  }
}

TypeId CrateArrayValueType(CrateTypeId id) {
  switch (id) {
    case CrateTypeId::Bool:
      return TypeId::Bool;
    case CrateTypeId::UChar:
      return TypeId::UChar;
    case CrateTypeId::UInt:
      return TypeId::UInt;
    case CrateTypeId::Int:
      return TypeId::Int;
    case CrateTypeId::Int64:
      return TypeId::Int64;
    case CrateTypeId::UInt64:
      return TypeId::UInt64;
    case CrateTypeId::Half:
      return TypeId::Half;
    case CrateTypeId::Float:
      return TypeId::Float;
    case CrateTypeId::Double:
      return TypeId::Double;
    case CrateTypeId::TimeCode:
      return TypeId::TimeCode;
    case CrateTypeId::String:
      return TypeId::String;
    case CrateTypeId::Token:
      return TypeId::Token;
    case CrateTypeId::AssetPath:
      return TypeId::AssetPath;
    case CrateTypeId::PathExpression:
      return TypeId::PathExpression;
    case CrateTypeId::Vec2f:
      return TypeId::Float2;
    case CrateTypeId::Vec3f:
      return TypeId::Float3;
    case CrateTypeId::Vec4f:
      return TypeId::Float4;
    case CrateTypeId::Vec2d:
      return TypeId::Double2;
    case CrateTypeId::Vec3d:
      return TypeId::Double3;
    case CrateTypeId::Vec4d:
      return TypeId::Double4;
    case CrateTypeId::Vec2h:
      return TypeId::Half2;
    case CrateTypeId::Vec3h:
      return TypeId::Half3;
    case CrateTypeId::Vec4h:
      return TypeId::Half4;
    case CrateTypeId::Vec2i:
      return TypeId::Int2;
    case CrateTypeId::Vec3i:
      return TypeId::Int3;
    case CrateTypeId::Vec4i:
      return TypeId::Int4;
    case CrateTypeId::Quatf:
      return TypeId::Quatf;
    case CrateTypeId::Quatd:
      return TypeId::Quatd;
    case CrateTypeId::Quath:
      return TypeId::Quath;
    case CrateTypeId::Matrix2d:
      return TypeId::Matrix2d;
    case CrateTypeId::Matrix3d:
      return TypeId::Matrix3d;
    case CrateTypeId::Matrix4d:
      return TypeId::Matrix4d;
    default:
      return TypeId::Invalid;
  }
}

// ============================================================
// ProbeArrayBlock
// ============================================================

bool ProbeArrayBlock(const std::shared_ptr<CrateDataSource>& source, ValueRep rep,
                     size_t max_elements, LazyArrayRef* out) {
  if (!source || !out) return false;

  out->source = source;
  out->rep = rep;
  out->crate_type = rep.type_id();
  out->value_type = CrateArrayValueType(rep.type_id());
  out->src_elem_stride = CrateArrayElemStride(rep.type_id());
  out->is_compressed = rep.is_compressed();
  out->element_count = 0;
  out->block_offset = 0;
  out->block_len = 0;

  // payload()==0 (non-inlined) denotes an empty array (pxrUSD convention).
  if (rep.payload() == 0) {
    return true;
  }

  // Probing reads the block header straight from the resident parse buffer.
  // Paged sources release that buffer after parsing and never probe again.
  const uint8_t* source_base = source->base();
  if (!source_base) return false;

  StreamReader r(source_base, source->size());
  // payload_as_offset() sign-extends from bit 47; reject a negative offset
  // rather than let the size_t cast truncate it (see SeekToPayload).
  if (!SeekToPayload(&r, rep)) return false;
  const size_t off = static_cast<size_t>(rep.payload_as_offset());
  const CrateVersion version = source->version();
  const uint64_t hdr = CrateArrayCountHeaderBytes(version);
  uint64_t count = 0;
  if (!ReadCrateArrayCount(r, version, &count)) return false;
  if (count > max_elements) return false;

  const CrateTypeId t = rep.type_id();
  const bool compressed = rep.is_compressed();

  out->element_count = count;
  out->block_offset = off;

  // NOTE: element_count is intentionally NOT rejected here when the block runs
  // past EOF -- callers rely on the probe succeeding with block_len = 0 so the
  // decoder can report the error (see test_crate_alloc_guard). What keeps a
  // LAZY value honest is the caller's `block_len > 0` gate plus `max_elements`
  // (crate-reader-arrays.cc), not this function.
  if (compressed && (t == CrateTypeId::Int || t == CrateTypeId::UInt)) {
    // Block layout: [count header][u64 comp_size][comp_size bytes].
    uint64_t comp_size = 0;
    if (!r.read_u64(comp_size)) return false;
    if (comp_size >
        (std::numeric_limits<uint64_t>::max)() - 16ull) {
      out->block_len = 0;
      return true;
    }

    // Bound element_count by what comp_size could possibly decode to.
    //
    // Without this the count is independent of the payload: a handful of
    // compressed bytes could advertise ~1e9 elements (the flat
    // max_array_elements cap), and Value::array_size() reports that verbatim
    // for a lazy value -- so a consumer sizing a buffer from it allocates on
    // the file's number long before materialization refuses the array.
    //
    // The bound is derived from the codec rather than guessed. USD integer
    // compression spends 2 bits of code per element before LZ4
    // (ComputeDeltaCodeBytes: ceil(count*2/8) = ceil(count/4) bytes), and
    // LZ4's compression ratio is asymptotically capped near 255:1 (a long
    // match costs ~1 byte per additional 255). So
    //     ceil(count/4) <= pre_lz4 <= comp_size * 255
    // giving count <= 1020 * comp_size.
    //
    // Measured against real files: the worst ratio in tests/usdc is 12, but a
    // 28 MB MJCF-converted mesh asset reaches 912.6 -- 89% of the theoretical
    // 1020, which both confirms the derivation and shows how little headroom
    // a bound of exactly 1020 would leave. kSafety keeps 4x on top of theory,
    // so the check kills the "a few bytes claim a billion elements" shape
    // without going anywhere near plausible content.
    constexpr uint64_t kElemsPerCompressedByte = 1020;
    constexpr uint64_t kSafety = 4;
    if (comp_size == 0) {
      if (count != 0) return false;
    } else if (comp_size <= (std::numeric_limits<uint64_t>::max)() /
                                (kElemsPerCompressedByte * kSafety)) {
      const uint64_t max_count =
          comp_size * kElemsPerCompressedByte * kSafety;
      if (count > max_count) {
        return false;  // caller falls through to eager decode, which errors
      }
    }

    out->block_len = hdr + 8ull + comp_size;
  } else if (!compressed && out->src_elem_stride > 0) {
    // Block layout: [count header][count*stride bytes].
    const uint64_t stride = uint64_t(out->src_elem_stride);
    if (count > ((std::numeric_limits<uint64_t>::max)() - 8ull) / stride) {
      out->block_len = 0;
      return true;
    }
    out->block_len = hdr + count * stride;
  } else {
    // Unknown layout (e.g. a compressed array of an unsupported type) — leave
    // block_len = 0 so write-time pass-through declines and re-encodes instead.
    out->block_len = 0;
  }

  // Validate that a known block stays in bounds; otherwise mark it unknown.
  if (out->block_len > 0) {
    if (off > source->size() || out->block_len > source->size() - off) {
      out->block_len = 0;
    }
  }

  return true;
}

// ============================================================
// DecodeCrateArray — mirrors CrateReader::Impl::UnpackArray
// ============================================================

bool DecodeCrateArray(const uint8_t* base, size_t size, ValueRep rep,
                      CrateVersion version,
                      const std::vector<std::string>& tokens, size_t max_elements,
                      Value* out) {
  CrateAllocationBudget budget(size, 0);
  return DecodeCrateArray(base, size, rep, version, tokens, max_elements, out,
                          &budget);
}

namespace {

// Shared body. `base_offset` is the absolute file offset of `base`; it is 0
// for a whole-file buffer and non-zero for a paged window that starts exactly
// at the array block header.
bool DecodeCrateArrayImpl(const uint8_t* base, size_t size, ValueRep rep,
                          uint64_t base_offset, CrateVersion version,
                          const std::vector<std::string>& tokens,
                          size_t max_elements, Value* out,
                          CrateAllocationBudget* budget) {
  CrateAllocationBudget fallback(size, 0);
  if (!budget) budget = &fallback;
  auto charge = [&](uint64_t bytes) -> bool {
    return budget->Charge(bytes) == nullptr;
  };
  auto charge_elements = [&](size_t count, size_t stride) -> bool {
    size_t bytes;
    return safe::mul(count, stride, &bytes) && charge(bytes);
  };
  if (!out) return false;
  const CrateTypeId type_id = rep.type_id();

  // The compressed bit determines the payload layout even for a small array.
  // AOUSD's small-array threshold is writer guidance, not a reader exception.

  StreamReader r(base, size);

  uint64_t count = 0;
  if (rep.payload() != 0) {
    const int64_t off = rep.payload_as_offset();
    if (off < 0) return false;
    const uint64_t u = static_cast<uint64_t>(off);
    if (u < base_offset) return false;
    const uint64_t rel = u - base_offset;
    if (rel > static_cast<uint64_t>((std::numeric_limits<size_t>::max)())) {
      return false;
    }
    if (!r.seek(static_cast<size_t>(rel))) return false;
    if (!ReadCrateArrayCount(r, version, &count)) return false;
  }
  if (count > max_elements) return false;

  const bool compressed = rep.is_compressed();

  // Guard the per-element allocation BEFORE allocating any std::vector(count):
  // the cases below allocate count*elem bytes and only then call read_raw()
  // (whose has_elements() check is too late). A malformed count (e.g. 1e9
  // matrix4d = 128 GB) would OOM first. For an uncompressed array the bytes must
  // be present in the file; for any array, cap the decoded size at the input
  // size times the maximum stream compression ratio (mirrors the eager reader's
  // CheckByteAllocation).
  {
    const uint64_t stride = CrateArrayElemStride(type_id);
    uint64_t elem_bytes = stride ? stride : 1;
    if (type_id == CrateTypeId::Half || type_id == CrateTypeId::Vec2h ||
        type_id == CrateTypeId::Vec3h || type_id == CrateTypeId::Vec4h ||
        type_id == CrateTypeId::Quath) elem_bytes *= 2;
    else if (type_id == CrateTypeId::UChar) elem_bytes = sizeof(uint32_t);
    if (!compressed && stride > 0 &&
        !r.has_elements(static_cast<size_t>(count), static_cast<size_t>(stride))) {
      return false;
    }
    constexpr uint64_t kMaxRatio = 256;
    constexpr uint64_t kSlack = 64ull * 1024 * 1024;
    constexpr uint64_t kU64Max = (std::numeric_limits<uint64_t>::max)();
    const uint64_t fsz = static_cast<uint64_t>(size);
    const uint64_t cap = (fsz > (kU64Max - kSlack) / kMaxRatio)
                             ? kU64Max
                             : fsz * kMaxRatio + kSlack;
    if (count > cap / elem_bytes || !charge(count * elem_bytes)) return false;
  }

  auto read_compressed_u32_n = [&](uint32_t* dst, size_t n) -> bool {
    uint64_t comp_size;
    if (!r.read_u64(comp_size)) return false;
    if (comp_size > static_cast<uint64_t>((std::numeric_limits<size_t>::max)())) {
      return false;
    }
    if (static_cast<size_t>(comp_size) > r.remaining()) return false;
    size_t prefixed_size;
    size_t workspace;
    if (!safe::add(size_t(8), static_cast<size_t>(comp_size), &prefixed_size) ||
        !CrateIntegerWorkspaceBytes(n, sizeof(*dst), r.current(),
                                    static_cast<size_t>(comp_size), &workspace) ||
        !charge(prefixed_size) || !charge(workspace)) return false;
    std::vector<uint8_t> with_prefix(prefixed_size);
    std::memcpy(with_prefix.data(), &comp_size, 8);
    if (!r.read(with_prefix.data() + 8, static_cast<size_t>(comp_size))) return false;
    DecompressResult dr = DecompressCompressedU32(
        with_prefix.data(), with_prefix.size(), dst, n);
    return dr.success;
  };
  auto read_compressed_u32 = [&](uint32_t* dst) -> bool {
    return read_compressed_u32_n(dst, static_cast<size_t>(count));
  };

  auto read_compressed_u64_n = [&](uint64_t* dst, size_t n) -> bool {
    uint64_t comp_size;
    if (!r.read_u64(comp_size)) return false;
    if (comp_size > static_cast<uint64_t>((std::numeric_limits<size_t>::max)())) {
      return false;
    }
    if (static_cast<size_t>(comp_size) > r.remaining()) return false;
    size_t prefixed_size;
    size_t workspace;
    if (!safe::add(size_t(8), static_cast<size_t>(comp_size), &prefixed_size) ||
        !CrateIntegerWorkspaceBytes(n, sizeof(*dst), r.current(),
                                    static_cast<size_t>(comp_size), &workspace) ||
        !charge(prefixed_size) || !charge(workspace)) return false;
    std::vector<uint8_t> with_prefix(prefixed_size);
    std::memcpy(with_prefix.data(), &comp_size, 8);
    if (!r.read(with_prefix.data() + 8, static_cast<size_t>(comp_size))) return false;
    DecompressResult dr = DecompressCompressedU64(
        with_prefix.data(), with_prefix.size(), dst, n);
    return dr.success;
  };
  auto read_compressed_u64 = [&](uint64_t* dst) -> bool {
    return read_compressed_u64_n(dst, static_cast<size_t>(count));
  };

  auto read_raw = [&](void* dst, size_t elem_size) -> bool {
    if (count == 0) return true;
    if (!r.has_elements(count, elem_size)) return false;
    size_t byte_count;
    if (!safe::mul(static_cast<size_t>(count), elem_size, &byte_count)) return false;
    return r.read(dst, byte_count);
  };

  auto read_compressed_floating_n = [&](auto* dst, size_t n) -> bool {
    using T = typename std::remove_pointer<decltype(dst)>::type;
    int8_t code = 0;
    if (!r.read_i8(code)) return false;
    if (code == 'i') {
      if (!charge_elements(n, sizeof(uint32_t))) return false;
      std::vector<uint32_t> ints(n);
      if (!read_compressed_u32_n(ints.data(), n)) return false;
      for (size_t i = 0; i < n; ++i) {
        dst[i] = static_cast<T>(static_cast<int32_t>(ints[i]));
      }
      return true;
    }
    if (code == 't') {
      uint32_t lut_size = 0;
      if (!r.read_u32(lut_size)) return false;
      if (lut_size == 0 || lut_size > max_elements) return false;
      if (!r.has_elements(size_t(lut_size), sizeof(T))) return false;
      if (!charge_elements(lut_size, sizeof(T))) return false;
      std::vector<T> lut(lut_size);
      size_t lut_bytes;
      if (!safe::mul(size_t(lut_size), sizeof(T), &lut_bytes)) return false;
      if (!r.read(lut.data(), lut_bytes)) return false;
      if (!charge_elements(n, sizeof(uint32_t))) return false;
      std::vector<uint32_t> idxs(n);
      if (!read_compressed_u32_n(idxs.data(), n)) return false;
      for (size_t i = 0; i < n; ++i) {
        if (idxs[i] >= lut_size) return false;
        dst[i] = lut[idxs[i]];
      }
      return true;
    }
    return false;
  };

  auto read_compressed_half_n = [&](float* dst, size_t n) -> bool {
    int8_t code = 0;
    if (!r.read_i8(code)) return false;
    if (code == 'i') {
      if (!charge_elements(n, sizeof(uint32_t))) return false;
      std::vector<uint32_t> ints(n);
      if (!read_compressed_u32_n(ints.data(), n)) return false;
      for (size_t i = 0; i < n; ++i) {
        dst[i] = HalfToFloat(FloatToHalf(static_cast<float>(static_cast<int32_t>(ints[i]))));
      }
      return true;
    }
    if (code == 't') {
      uint32_t lut_size = 0;
      if (!r.read_u32(lut_size)) return false;
      if (lut_size == 0 || lut_size > max_elements) return false;
      if (!r.has_elements(size_t(lut_size), sizeof(uint16_t))) return false;
      if (!charge_elements(lut_size, sizeof(uint16_t))) return false;
      std::vector<uint16_t> lut(lut_size);
      size_t lut_bytes;
      if (!safe::mul(size_t(lut_size), sizeof(uint16_t), &lut_bytes)) return false;
      if (!r.read(lut.data(), lut_bytes)) return false;
      if (!charge_elements(n, sizeof(uint32_t))) return false;
      std::vector<uint32_t> idxs(n);
      if (!read_compressed_u32_n(idxs.data(), n)) return false;
      for (size_t i = 0; i < n; ++i) {
        if (idxs[i] >= lut_size) return false;
        dst[i] = HalfToFloat(lut[idxs[i]]);
      }
      return true;
    }
    return false;
  };

  switch (type_id) {
    case CrateTypeId::Float: {
      detail::ArrayBuilder<float> data(static_cast<size_t>(count));
      if (data.failed()) return false;
      if (compressed) {
        if (!read_compressed_floating_n(data.data(), static_cast<size_t>(count))) return false;
      } else if (!read_raw(data.data(), sizeof(float))) {
        return false;
      }
      *out = data.take(TypeId::Float);
      return !out->is_empty();
    }
    case CrateTypeId::Int: {
      detail::ArrayBuilder<int32_t> data(static_cast<size_t>(count));
      if (data.failed()) return false;
      if (compressed) {
        if (!read_compressed_u32(reinterpret_cast<uint32_t*>(data.data()))) return false;
      } else if (!read_raw(data.data(), sizeof(int32_t))) {
        return false;
      }
      *out = data.take(TypeId::Int);
      return !out->is_empty();
    }
    case CrateTypeId::Vec2f: {
      size_t scalars;
      if (!safe::mul(static_cast<size_t>(count), size_t(2), &scalars)) return false;
      detail::ArrayBuilder<float> data(scalars);
      if (data.failed()) return false;
      if (compressed) {
        if (!read_compressed_floating_n(data.data(), scalars)) return false;
      } else if (!read_raw(data.data(), 2 * sizeof(float))) {
        return false;
      }
      *out = data.take(TypeId::Float2);
      return !out->is_empty();
    }
    case CrateTypeId::Vec3f: {
      size_t scalars;
      if (!safe::mul(static_cast<size_t>(count), size_t(3), &scalars)) return false;
      detail::ArrayBuilder<float> data(scalars);
      if (data.failed()) return false;
      if (compressed) {
        if (!read_compressed_floating_n(data.data(), scalars)) return false;
      } else if (!read_raw(data.data(), 3 * sizeof(float))) {
        return false;
      }
      *out = data.take(TypeId::Float3);
      return !out->is_empty();
    }
    case CrateTypeId::Double: {
      detail::ArrayBuilder<double> data(static_cast<size_t>(count));
      if (data.failed()) return false;
      if (compressed) {
        if (!read_compressed_floating_n(data.data(), static_cast<size_t>(count))) return false;
      } else if (!read_raw(data.data(), sizeof(double))) {
        return false;
      }
      *out = data.take(TypeId::Double);
      return !out->is_empty();
    }
    case CrateTypeId::Int64: {
      detail::ArrayBuilder<int64_t> data(static_cast<size_t>(count));
      if (data.failed()) return false;
      if (compressed) {
        if (!read_compressed_u64(reinterpret_cast<uint64_t*>(data.data()))) return false;
      } else if (!read_raw(data.data(), sizeof(int64_t))) {
        return false;
      }
      *out = data.take(TypeId::Int64);
      return !out->is_empty();
    }
    case CrateTypeId::UInt: {
      detail::ArrayBuilder<uint32_t> data(static_cast<size_t>(count));
      if (data.failed()) return false;
      if (compressed) {
        if (!read_compressed_u32(data.data())) return false;
      } else if (!read_raw(data.data(), sizeof(uint32_t))) {
        return false;
      }
      *out = data.take(TypeId::UInt);
      return !out->is_empty();
    }
    case CrateTypeId::UChar: {
      // uchar[] is stored tightly packed on disk but uses the shared uint32
      // array storage in Value. Keep the authored UChar identity while
      // widening each lane on materialization.
      if (compressed) return false;
      detail::ArrayBuilder<uint32_t> data(static_cast<size_t>(count));
      if (data.failed()) return false;
      const uint8_t* raw = r.current();
      if (!r.skip(static_cast<size_t>(count))) return false;
      for (size_t i = 0; i < count; ++i) data[i] = raw[i];
      *out = data.take(TypeId::UChar);
      return !out->is_empty();
    }
    case CrateTypeId::UInt64: {
      detail::ArrayBuilder<uint64_t> data(static_cast<size_t>(count));
      if (data.failed()) return false;
      if (compressed) {
        if (!read_compressed_u64(data.data())) return false;
      } else if (!read_raw(data.data(), sizeof(uint64_t))) {
        return false;
      }
      *out = data.take(TypeId::UInt64);
      return !out->is_empty();
    }
    case CrateTypeId::Bool: {
      detail::ArrayBuilder<uint8_t> bytes(static_cast<size_t>(count));
      if (bytes.failed()) return false;
      if (compressed) {
        if (!charge_elements(static_cast<size_t>(count), sizeof(uint32_t))) return false;
        std::vector<uint32_t> lanes(static_cast<size_t>(count));
        if (!read_compressed_u32(lanes.data())) return false;
        for (size_t i = 0; i < lanes.size(); ++i) {
          bytes[i] = lanes[i] != 0 ? uint8_t(1) : uint8_t(0);
        }
      } else if (!read_raw(bytes.data(), sizeof(uint8_t))) {
        return false;
      }
      for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = bytes[i] ? 1 : 0;
      *out = bytes.take(TypeId::Bool);
      return !out->is_empty();
    }
    case CrateTypeId::TimeCode: {
      detail::ArrayBuilder<double> data(static_cast<size_t>(count));
      if (data.failed()) return false;
      if (compressed) {
        if (!read_compressed_floating_n(data.data(),
                                        static_cast<size_t>(count))) {
          return false;
        }
      } else if (!read_raw(data.data(), sizeof(double))) {
        return false;
      }
      *out = data.take(TypeId::TimeCode);
      return !out->is_empty();
    }
    case CrateTypeId::Token: {
      std::vector<uint32_t> idxs(static_cast<size_t>(count));
      if (compressed) {
        if (!read_compressed_u32(idxs.data())) return false;
      } else if (!read_raw(idxs.data(), sizeof(uint32_t))) {
        return false;
      }
      size_t text_bytes = 0;
      for (uint32_t index : idxs) {
        if (index >= tokens.size()) return false;
        size_t length;
        if (!safe::add(tokens[index].size(), uint64_t(1), &length) ||
            !safe::add(text_bytes, length, &text_bytes)) return false;
      }
      if (!charge_elements(static_cast<size_t>(count), sizeof(std::string)) ||
          !charge(text_bytes)) return false;
      struct Context {
        const std::vector<std::string>* tokens;
        const std::vector<uint32_t>* indices;
      };
      const Context context{&tokens, &idxs};
      const auto at = [](const void* p, size_t i, size_t* length) {
        const auto& ctx = *static_cast<const Context*>(p);
        const auto& text = (*ctx.tokens)[(*ctx.indices)[i]];
        *length = text.size();
        return text.data();
      };
      *out = Value::MakeStrings(TypeId::Token, idxs.size(), at, &context);
      return out->is_array();
    }
    case CrateTypeId::Vec4f:
    case CrateTypeId::Quatf: {
      const uint32_t stride_bytes = CrateArrayElemStride(type_id);
      const uint32_t comps = stride_bytes / 4;
      if (comps == 0 ||
          count > (std::numeric_limits<size_t>::max)() / comps) {
        return false;
      }
      detail::ArrayBuilder<float> data(static_cast<size_t>(count) * comps);
      if (data.failed()) return false;
      if (compressed) {
        if (!read_compressed_floating_n(data.data(), data.size())) return false;
      } else if (!read_raw(data.data(), stride_bytes)) {
        return false;
      }
      if (type_id == CrateTypeId::Quatf) {
        // Disk / GfQuat layout is imaginary-first (x,y,z,w); internal is
        // real-first (w,x,y,z). Mirrors UnpackArray's eager Quatf path.
        for (size_t e = 0; e < count; ++e) {
          float* q = data.data() + e * 4;
          const float w = q[3];
          q[3] = q[2]; q[2] = q[1]; q[1] = q[0]; q[0] = w;
        }
      }
      *out = data.take(CrateArrayValueType(type_id));
      return !out->is_empty();
    }
    case CrateTypeId::Vec2d:
    case CrateTypeId::Vec3d:
    case CrateTypeId::Vec4d:
    case CrateTypeId::Quatd:
    case CrateTypeId::Matrix2d:
    case CrateTypeId::Matrix3d:
    case CrateTypeId::Matrix4d: {
      const uint32_t stride_bytes = CrateArrayElemStride(type_id);
      const uint32_t comps = stride_bytes / 8;
      if (comps == 0 ||
          count > (std::numeric_limits<size_t>::max)() / comps) {
        return false;
      }
      detail::ArrayBuilder<double> data(static_cast<size_t>(count) * comps);
      if (data.failed()) return false;
      if (compressed) {
        if (!read_compressed_floating_n(data.data(), data.size())) return false;
      } else if (!read_raw(data.data(), stride_bytes)) {
        return false;
      }
      if (type_id == CrateTypeId::Quatd) {
        // Imaginary-first on disk -> real-first internal (see Quatf above).
        for (size_t e = 0; e < count; ++e) {
          double* q = data.data() + e * 4;
          const double w = q[3];
          q[3] = q[2]; q[2] = q[1]; q[1] = q[0]; q[0] = w;
        }
      }
      *out = data.take(CrateArrayValueType(type_id));
      return !out->is_empty();
    }
    case CrateTypeId::Half:
    case CrateTypeId::Vec2h:
    case CrateTypeId::Vec3h:
    case CrateTypeId::Vec4h:
    case CrateTypeId::Quath: {
      const uint32_t comps = CrateArrayElemStride(type_id) / 2;  // 2 bytes/half
      if (comps == 0 ||
          count > (std::numeric_limits<size_t>::max)() / comps) {
        return false;
      }
      detail::ArrayBuilder<float> data(static_cast<size_t>(count) * comps);
      if (data.failed()) return false;
      if (compressed) {
        if (!read_compressed_half_n(data.data(), data.size())) return false;
      } else {
        size_t bytes;
        if (!safe::mul(data.size(), sizeof(uint16_t), &bytes)) return false;
        const uint8_t* raw = r.current();
        if (!r.skip(bytes)) return false;
        for (size_t i = 0; i < data.size(); ++i) {
          uint16_t half;
          std::memcpy(&half, raw + i * sizeof(half), sizeof(half));
          data[i] = HalfToFloat(half);
        }
      }
      if (type_id == CrateTypeId::Quath) {
        for (size_t e = 0; e < count; ++e) {
          float* q = data.data() + e * 4;
          const float w = q[3];
          q[3] = q[2]; q[2] = q[1]; q[1] = q[0]; q[0] = w;
        }
      }
      *out = data.take(CrateArrayValueType(type_id));
      return !out->is_empty();
    }
    default:
      // No concrete Value storage for this array type yet.
      return false;
  }
}

}  // namespace

bool DecodeCrateArray(const uint8_t* base, size_t size, ValueRep rep,
                      CrateVersion version,
                      const std::vector<std::string>& tokens, size_t max_elements,
                      Value* out, CrateAllocationBudget* budget) {
  return DecodeCrateArrayImpl(base, size, rep, 0, version, tokens,
                              max_elements, out, budget);
}

bool DecodeCrateArrayAt(const uint8_t* base, size_t size, ValueRep rep,
                        uint64_t base_offset, CrateVersion version,
                        const std::vector<std::string>& tokens,
                        size_t max_elements, Value* out,
                        CrateAllocationBudget* budget) {
  return DecodeCrateArrayImpl(base, size, rep, base_offset, version, tokens,
                              max_elements, out, budget);
}

}  // namespace next
}  // namespace lightusd
