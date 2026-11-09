#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <memory>

#include "core/security-policy.hh"

namespace lightusd {
namespace next {

/// Options for reading USDZ/ZIP archives.
struct USDZReadOptions {
  /// Maximum archive byte size accepted by Open/OpenFile (0 = no limit).
  size_t max_archive_size = security_policy::kDefaultInputLimitBytes;

  /// Maximum uncompressed byte size for any single entry (0 = no limit).
  size_t max_entry_size = security_policy::kDefaultInputLimitBytes;

  /// Maximum number of entries in an archive (0 = no limit).
  size_t max_entries = security_policy::kDefaultArchiveEntryCount;
};

/// Minimal USDZ/ZIP reader for the next library.
/// Handles store-only ZIP archives with 64-byte alignment.
class USDZReader {
public:
  USDZReader() = default;

  /// Open and parse a USDZ file.
  /// Returns false if the file is not a valid USDZ archive.
  bool Open(const uint8_t* data, size_t size, const USDZReadOptions& options = {});
  bool OpenFile(const std::string& filename, const USDZReadOptions& options = {});

  /// Last parse/open error. Empty when the previous Open/OpenFile succeeded.
  const std::string& Error() const { return error_; }

  /// Get the number of entries in the archive.
  size_t NumEntries() const { return entries_.size(); }

  /// Get entry name at index.
  const std::string& EntryName(size_t index) const;

  /// Get entry data at index (returns pointer to data, valid until Open is called again).
  const uint8_t* EntryData(size_t index) const;
  size_t EntrySize(size_t index) const;

  /// Return the USDZ root layer. Per the package specification this must be the
  /// first archive entry and must contain USDA/USDC data (a .usd extension is
  /// accepted and content-sniffed). Returns -1 for an invalid package root.
  int FindRootLayer() const;

  /// Legacy searches retained for callers selecting non-root entries.
  /// Returns -1 if not found.
  int FindUSDCFile() const;
  int FindUSDAFile() const;

private:
  struct Entry {
    std::string name;
    size_t offset;  // offset of file data in the buffer
    size_t size;    // uncompressed size
  };
  std::vector<Entry> entries_;
  std::vector<uint8_t> file_data_;  // if reading from file
  const uint8_t* data_ = nullptr;
  size_t data_size_ = 0;
  std::string error_;
};

} // namespace next
} // namespace lightusd
