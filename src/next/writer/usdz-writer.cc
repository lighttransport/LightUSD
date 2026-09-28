#include "usdz-writer.hh"
#include "../writer/usdc-writer.hh"

#include <array>
#include <cstring>
#include <fstream>
#include <limits>

namespace lightusd {
namespace next {

namespace {

// ============================================================
// CRC32 implementation (no external deps)
// ============================================================

// Thread-safe one-time init via a function-local static (C++11 magic statics),
// so concurrent USDZ writes don't race on a hand-rolled init flag.
static const std::array<uint32_t, 256>& CRC32Table() {
  static const std::array<uint32_t, 256> table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t c = i;
      for (int j = 0; j < 8; j++) {
        if (c & 1)
          c = 0xEDB88320u ^ (c >> 1);
        else
          c = c >> 1;
      }
      t[i] = c;
    }
    return t;
  }();
  return table;
}

static uint32_t ComputeCRC32(const uint8_t* data, size_t len) {
  const std::array<uint32_t, 256>& crc32_table = CRC32Table();
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; i++) {
    crc = crc32_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFu;
}

static size_t FilePositionOrZero(std::ofstream& ofs) {
  const std::streampos pos = ofs.tellp();
  return pos < std::streampos(0) ? 0 : static_cast<size_t>(pos);
}

// ============================================================
// ZIP constants
// ============================================================

constexpr size_t kUSDZAlignment = 64;
constexpr size_t kZipLocalHeaderSize = 30;

// ============================================================
// ZIP write helpers
// ============================================================

struct CentralDirEntry {
  std::string name;
  uint32_t crc;
  uint32_t size;
  size_t local_header_offset;
};

static bool WriteLocalFileHeader(std::vector<uint8_t>& buf,
                                  const std::string& name,
                                  const uint8_t* data, size_t data_size,
                                  CentralDirEntry* out_entry,
                                  std::string* err,
                                  uint64_t max_file_size_bytes) {
  size_t header_size = kZipLocalHeaderSize + name.size();
  size_t padding = 0;
  size_t remainder = (buf.size() + header_size) % kUSDZAlignment;
  if (remainder != 0) {
    padding = kUSDZAlignment - remainder;
  }

  if (header_size > (std::numeric_limits<size_t>::max)() - padding ||
      header_size + padding >
          (std::numeric_limits<size_t>::max)() - data_size) {
    if (err) *err = "USDZ output size overflow";
    return false;
  }
  const size_t append_size = header_size + padding + data_size;
  if (max_file_size_bytes &&
      (static_cast<uint64_t>(buf.size()) > max_file_size_bytes ||
       static_cast<uint64_t>(append_size) >
           max_file_size_bytes - static_cast<uint64_t>(buf.size()))) {
    if (err) *err = "USDZ output exceeds configured file-size limit";
    return false;
  }

  size_t local_header_offset = buf.size();

  // ZIP32 limits: entry size and local-header offset are 4-byte fields.
  // Exceeding them silently truncated (corrupt archive); fail loudly until
  // ZIP64 records are implemented.
  if (data_size > 0xFFFFFFFFull || local_header_offset > 0xFFFFFFFFull) {
    if (err) {
      *err = "USDZ entry '" + name +
             "' exceeds the 4 GiB ZIP32 limit (ZIP64 is not supported)";
    }
    return false;
  }

  uint32_t crc = 0;
  if (data && data_size > 0) {
    crc = ComputeCRC32(data, data_size);
  }

  // Local file header
  uint8_t header[kZipLocalHeaderSize];
  std::memset(header, 0, sizeof(header));

  // Signature: PK\003\004
  header[0] = 0x50; header[1] = 0x4b; header[2] = 0x03; header[3] = 0x04;
  // Version needed: 2.0
  header[4] = 20; header[5] = 0;
  // Compression: 0 (stored/uncompressed)
  // CRC-32
  std::memcpy(&header[14], &crc, 4);
  // Compressed size == uncompressed size (stored)
  uint32_t sz32 = static_cast<uint32_t>(data_size);
  std::memcpy(&header[18], &sz32, 4);
  std::memcpy(&header[22], &sz32, 4);
  // Filename length
  uint16_t name_len = static_cast<uint16_t>(name.size());
  std::memcpy(&header[26], &name_len, 2);
  // Extra field length (for alignment padding)
  uint16_t extra_len = static_cast<uint16_t>(padding);
  std::memcpy(&header[28], &extra_len, 2);

  buf.insert(buf.end(), header, header + kZipLocalHeaderSize);
  buf.insert(buf.end(), name.begin(), name.end());
  if (padding > 0) {
    buf.insert(buf.end(), padding, 0);
  }

  // Verify alignment
  if ((buf.size() % kUSDZAlignment) != 0) {
    if (err) *err += "Internal error: USDZ alignment failed\n";
    return false;
  }

  // Write file data
  if (data && data_size > 0) {
    buf.insert(buf.end(), data, data + data_size);
  }

  if (out_entry) {
    out_entry->name = name;
    out_entry->crc = crc;
    out_entry->size = sz32;
    out_entry->local_header_offset = local_header_offset;
  }

  return true;
}

static void WriteCentralDirectory(
    std::vector<uint8_t>& buf,
    const std::vector<CentralDirEntry>& entries) {
  size_t cd_offset = buf.size();

  for (const auto& entry : entries) {
    uint32_t local_offset = static_cast<uint32_t>(entry.local_header_offset);

    uint8_t cdr[46];
    std::memset(cdr, 0, sizeof(cdr));

    // Signature: PK\001\002
    cdr[0] = 0x50; cdr[1] = 0x4b; cdr[2] = 0x01; cdr[3] = 0x02;
    // Version made by: 2.0
    cdr[4] = 20; cdr[5] = 0;
    // Version needed: 2.0
    cdr[6] = 20; cdr[7] = 0;
    // CRC-32
    std::memcpy(&cdr[16], &entry.crc, 4);
    // Compressed size
    std::memcpy(&cdr[20], &entry.size, 4);
    // Uncompressed size
    std::memcpy(&cdr[24], &entry.size, 4);
    // Filename length
    uint16_t name_len = static_cast<uint16_t>(entry.name.size());
    std::memcpy(&cdr[28], &name_len, 2);
    // Relative offset of local header
    std::memcpy(&cdr[42], &local_offset, 4);

    buf.insert(buf.end(), cdr, cdr + 46);
    buf.insert(buf.end(), entry.name.begin(), entry.name.end());
  }

  size_t cd_size = buf.size() - cd_offset;
  // entries.size()/cd offsets were bounds-checked by the caller before the
  // local headers were laid down; offsets past 4 GiB cannot reach here.

  // End of central directory record
  uint8_t eocd[22];
  std::memset(eocd, 0, sizeof(eocd));
  // Signature: PK\005\006
  eocd[0] = 0x50; eocd[1] = 0x4b; eocd[2] = 0x05; eocd[3] = 0x06;
  // Number of entries on this disk
  uint16_t num_entries = static_cast<uint16_t>(entries.size());
  std::memcpy(&eocd[8], &num_entries, 2);
  // Total number of entries
  std::memcpy(&eocd[10], &num_entries, 2);
  // Size of central directory
  uint32_t cd_size32 = static_cast<uint32_t>(cd_size);
  std::memcpy(&eocd[12], &cd_size32, 4);
  // Offset of central directory
  uint32_t cd_offset32 = static_cast<uint32_t>(cd_offset);
  std::memcpy(&eocd[16], &cd_offset32, 4);

  buf.insert(buf.end(), eocd, eocd + 22);
}

static void Store16(uint8_t* out, uint16_t value) {
  out[0] = static_cast<uint8_t>(value);
  out[1] = static_cast<uint8_t>(value >> 8);
}

static void Store32(uint8_t* out, uint32_t value) {
  Store16(out, static_cast<uint16_t>(value));
  Store16(out + 2, static_cast<uint16_t>(value >> 16));
}

static bool WriteSinkSpan(const USDZWriteSink& sink, const uint8_t* data,
                          size_t size, uint64_t max_size, size_t* position,
                          std::string* error) {
  if (size > (std::numeric_limits<size_t>::max)() - *position ||
      (max_size && (static_cast<uint64_t>(*position) > max_size ||
                    static_cast<uint64_t>(size) > max_size - *position))) {
    *error = "USDZ output exceeds configured file-size limit";
    return false;
  }
  if (size && !sink(data, size)) {
    *error = "USDZ output sink rejected data";
    return false;
  }
  *position += size;
  return true;
}

static bool WriteSinkEntry(const USDZWriteSink& sink, const std::string& name,
                          const uint8_t* data, size_t size,
                          uint64_t max_size, size_t* position,
                          CentralDirEntry* entry, std::string* error) {
  if (name.size() > UINT16_MAX || size > UINT32_MAX ||
      *position > UINT32_MAX) {
    *error = "USDZ entry exceeds ZIP32 limits";
    return false;
  }
  const size_t offset = *position;
  const size_t base = kZipLocalHeaderSize + name.size();
  const size_t padding = (kUSDZAlignment - ((base + offset) % kUSDZAlignment)) % kUSDZAlignment;
  if (padding > UINT16_MAX) {
    *error = "USDZ alignment field exceeds ZIP32 limit";
    return false;
  }
  const uint32_t crc = data && size ? ComputeCRC32(data, size) : 0;
  uint8_t header[kZipLocalHeaderSize]{};
  Store32(header, 0x04034b50u);
  Store16(header + 4, 20);
  Store32(header + 14, crc);
  Store32(header + 18, static_cast<uint32_t>(size));
  Store32(header + 22, static_cast<uint32_t>(size));
  Store16(header + 26, static_cast<uint16_t>(name.size()));
  Store16(header + 28, static_cast<uint16_t>(padding));
  if (!WriteSinkSpan(sink, header, sizeof(header), max_size, position, error) ||
      !WriteSinkSpan(sink, reinterpret_cast<const uint8_t*>(name.data()), name.size(), max_size, position, error)) return false;
  uint8_t zeros[kUSDZAlignment]{};
  if (!WriteSinkSpan(sink, zeros, padding, max_size, position, error) ||
      !WriteSinkSpan(sink, data, size, max_size, position, error)) return false;
  entry->name = name;
  entry->crc = crc;
  entry->size = static_cast<uint32_t>(size);
  entry->local_header_offset = offset;
  return true;
}

static bool WriteSinkDirectory(const USDZWriteSink& sink,
                               const std::vector<CentralDirEntry>& entries,
                               uint64_t max_size, size_t* position,
                               std::string* error) {
  const size_t cd_offset = *position;
  if (entries.size() > UINT16_MAX || cd_offset > UINT32_MAX) {
    *error = "USDZ archive exceeds ZIP32 limits (ZIP64 not supported)";
    return false;
  }
  for (const auto& e : entries) {
    uint8_t record[46]{};
    Store32(record, 0x02014b50u);
    Store16(record + 4, 20); Store16(record + 6, 20);
    Store32(record + 16, e.crc); Store32(record + 20, e.size);
    Store32(record + 24, e.size);
    Store16(record + 28, static_cast<uint16_t>(e.name.size()));
    Store32(record + 42, static_cast<uint32_t>(e.local_header_offset));
    if (!WriteSinkSpan(sink, record, sizeof(record), max_size, position, error) ||
        !WriteSinkSpan(sink, reinterpret_cast<const uint8_t*>(e.name.data()), e.name.size(), max_size, position, error)) return false;
  }
  const size_t cd_size = *position - cd_offset;
  if (cd_size > UINT32_MAX) {
    *error = "USDZ central directory exceeds ZIP32 limit";
    return false;
  }
  uint8_t end[22]{};
  Store32(end, 0x06054b50u);
  Store16(end + 8, static_cast<uint16_t>(entries.size()));
  Store16(end + 10, static_cast<uint16_t>(entries.size()));
  Store32(end + 12, static_cast<uint32_t>(cd_size));
  Store32(end + 16, static_cast<uint32_t>(cd_offset));
  return WriteSinkSpan(sink, end, sizeof(end), max_size, position, error);
}

} // namespace

// ============================================================
// Public API
// ============================================================

static USDZWriteResult WriteUSDZFromRootAndAssetsToMemory(
    std::vector<uint8_t>& buffer, const uint8_t* root_data, size_t root_size,
    const std::map<std::string, std::vector<uint8_t>>& assets,
    const std::string& root_name, const USDZWriteOptions& options);

USDZWriteResult WriteUSDZToMemory(std::vector<uint8_t>& buffer, const Stage& stage,
                                  const USDZWriteOptions& options) {
  USDZWriteResult result;

  // First serialize stage to USDC
  std::vector<uint8_t> usdc_buffer;
  USDCWriteOptions write_options;
  write_options.crate_options.max_file_size_bytes = options.max_file_size_bytes;
  write_options.crate_options.max_memory_bytes = options.max_memory_bytes;
  auto usdc_result = WriteUSDCToMemory(usdc_buffer, stage, write_options);
  if (!usdc_result.success) {
    result.error = usdc_result.error;
    return result;
  }

  // Then wrap in ZIP
  return WriteUSDZFromUSDCToMemory(buffer, usdc_buffer.data(),
                                   usdc_buffer.size(), options);
}

USDZWriteResult WriteUSDZToFile(const std::string& filename, const Stage& stage,
                                const USDZWriteOptions& options) {
  USDZWriteResult result;
  std::vector<uint8_t> usdc;
  USDCWriteOptions write_options;
  write_options.crate_options.max_file_size_bytes = options.max_file_size_bytes;
  write_options.crate_options.max_memory_bytes = options.max_memory_bytes;
  auto usdc_result = WriteUSDCToMemory(usdc, stage, write_options);
  if (!usdc_result.success) { result.error = usdc_result.error; return result; }
  std::ofstream ofs(filename, std::ios::binary);
  if (!ofs) {
    result.success = false;
    result.error = "Failed to open file for writing: " + filename;
    return result;
  }
  result = WriteUSDZFromUSDCAndAssetsToSink(
      usdc.data(), usdc.size(), {},
      [&ofs](const uint8_t* bytes, size_t count) {
        ofs.write(reinterpret_cast<const char*>(bytes), static_cast<std::streamsize>(count));
        return ofs.good();
      }, options);
  ofs.flush();
  if (!ofs.good() && result.success) {
    result.success = false;
    result.error = "Write failed (disk full?) for file: " + filename;
  }
  result.bytes_written = FilePositionOrZero(ofs);
  return result;
}


USDZWriteResult WriteUSDZFromUSDCToMemory(std::vector<uint8_t>& buffer,
                                           const uint8_t* usdc_data,
                                           size_t usdc_size,
                                           const USDZWriteOptions& options) {
  const std::map<std::string, std::vector<uint8_t>> no_assets;
  return WriteUSDZFromUSDCAndAssetsToMemory(buffer, usdc_data, usdc_size,
                                            no_assets, options);
}

USDZWriteResult WriteUSDZFromUSDCAndAssetsToMemory(
    std::vector<uint8_t>& buffer, const uint8_t* usdc_data, size_t usdc_size,
    const std::map<std::string, std::vector<uint8_t>>& assets,
    const USDZWriteOptions& options) {
  return WriteUSDZFromRootAndAssetsToMemory(
      buffer, usdc_data, usdc_size, assets, "root.usdc", options);
}

USDZWriteResult WriteUSDZFromUSDCAndAssetsToSink(
    const uint8_t* usdc_data, size_t usdc_size,
    const std::map<std::string, std::vector<uint8_t>>& assets,
    const USDZWriteSink& sink, const USDZWriteOptions& options) {
  USDZWriteResult result;
  if (!sink || !usdc_data || usdc_size == 0) {
    result.error = "USDZ output requires a sink and non-empty root layer";
    return result;
  }
  if (options.max_memory_bytes) {
    size_t retained = usdc_size;
    size_t metadata_estimate = 0;
    const auto add = [](size_t* total, size_t amount) {
      if (amount > (std::numeric_limits<size_t>::max)() - *total) return false;
      *total += amount;
      return true;
    };
    bool ok = add(&metadata_estimate, 9);
    for (const auto& asset : assets) {
      ok = ok && add(&retained, asset.second.size()) &&
           add(&retained, asset.first.capacity()) &&
           add(&metadata_estimate, asset.first.capacity());
    }
    size_t working = retained;
    ok = ok && add(&working, metadata_estimate) &&
         assets.size() < (std::numeric_limits<size_t>::max)() &&
         assets.size() + 1 <= (std::numeric_limits<size_t>::max)() /
                                  sizeof(CentralDirEntry) &&
         add(&working, (assets.size() + 1) * sizeof(CentralDirEntry));
    if (!ok || working > options.max_memory_bytes) {
      result.error = "USDZ estimated working set exceeds configured memory limit";
      return result;
    }
  }
  size_t position = 0;
  std::vector<CentralDirEntry> entries;
  entries.reserve(assets.size() + 1);
  std::string error;
  CentralDirEntry root;
  if (!WriteSinkEntry(sink, "root.usdc", usdc_data, usdc_size,
                      options.max_file_size_bytes, &position, &root, &error)) {
    result.error = error;
    result.bytes_written = position;
    return result;
  }
  entries.push_back(std::move(root));
  for (const auto& asset : assets) {
    const std::string& name = asset.first;
    if (name.empty() || name == "root.usdc" || name == "root.usda" ||
        name[0] == '/' || name.find("..") != std::string::npos ||
        name.find('\\') != std::string::npos) {
      result.error = "Invalid USDZ asset path: " + name;
      result.bytes_written = position;
      return result;
    }
    CentralDirEntry entry;
    const uint8_t* data = asset.second.empty() ? nullptr : asset.second.data();
    if (!WriteSinkEntry(sink, name, data, asset.second.size(),
                        options.max_file_size_bytes, &position, &entry, &error)) {
      result.error = error;
      result.bytes_written = position;
      return result;
    }
    entries.push_back(std::move(entry));
  }
  if (!WriteSinkDirectory(sink, entries, options.max_file_size_bytes,
                          &position, &error)) {
    result.error = error;
    result.bytes_written = position;
    return result;
  }
  result.success = true;
  result.bytes_written = position;
  return result;
}

USDZWriteResult WriteUSDZFromUSDAAndAssetsToMemory(
    std::vector<uint8_t>& buffer, const uint8_t* usda_data, size_t usda_size,
    const std::map<std::string, std::vector<uint8_t>>& assets,
    const USDZWriteOptions& options) {
  return WriteUSDZFromRootAndAssetsToMemory(
      buffer, usda_data, usda_size, assets, "root.usda", options);
}

static USDZWriteResult WriteUSDZFromRootAndAssetsToMemory(
    std::vector<uint8_t>& buffer, const uint8_t* usdc_data, size_t usdc_size,
    const std::map<std::string, std::vector<uint8_t>>& assets,
    const std::string& root_name, const USDZWriteOptions& options) {
  USDZWriteResult result;

  if (!usdc_data || usdc_size == 0) {
    result.error = "Empty USD root-layer data";
    return result;
  }

  if (options.max_memory_bytes) {
    size_t retained_bytes = usdc_size;
    size_t archive_bytes = 0;
    size_t directory_bytes = 22;
    size_t entry_count = 1;
    const auto add_checked = [](size_t* total, size_t amount) {
      if (amount > (std::numeric_limits<size_t>::max)() - *total) return false;
      *total += amount;
      return true;
    };
    const auto estimate_entry = [&](size_t name_size, size_t payload_size) {
      // ZIP local header, path, worst-case USDZ alignment padding, payload;
      // central-directory header and path are charged separately.
      return add_checked(&archive_bytes, kZipLocalHeaderSize) &&
             add_checked(&archive_bytes, name_size) &&
             add_checked(&archive_bytes, kUSDZAlignment - 1) &&
             add_checked(&archive_bytes, payload_size) &&
             add_checked(&directory_bytes, 46) &&
             add_checked(&directory_bytes, name_size);
    };
    bool estimate_ok = estimate_entry(root_name.size(), usdc_size);
    for (const auto& asset : assets) {
      ++entry_count;
      estimate_ok = estimate_ok &&
          add_checked(&retained_bytes, asset.second.size()) &&
          add_checked(&retained_bytes, asset.first.capacity()) &&
          estimate_entry(asset.first.size(), asset.second.size());
    }
    size_t working_bytes = retained_bytes;
    estimate_ok = estimate_ok && add_checked(&working_bytes, archive_bytes) &&
        add_checked(&working_bytes, directory_bytes) &&
        entry_count <= (std::numeric_limits<size_t>::max)() /
                           sizeof(CentralDirEntry) &&
        add_checked(&working_bytes, entry_count * sizeof(CentralDirEntry));
    if (!estimate_ok || working_bytes > options.max_memory_bytes) {
      result.error = "USDZ estimated working set exceeds configured memory limit";
      return result;
    }
  }

  buffer.clear();

  // Write root layer
  CentralDirEntry entry;
  std::string err;
  if (!WriteLocalFileHeader(buffer, root_name, usdc_data, usdc_size, &entry,
                            &err, options.max_file_size_bytes)) {
    result.error = err.empty() ? "Failed to write ZIP entry" : err;
    return result;
  }

  std::vector<CentralDirEntry> entries = {entry};
  for (const auto& asset : assets) {
    const std::string& name = asset.first;
    if (name.empty() || name == root_name || name[0] == '/' ||
        name.find("..") != std::string::npos || name.find('\\') != std::string::npos) {
      result.error = "Invalid USDZ asset path: " + name;
      buffer.clear();
      return result;
    }
    if (asset.second.size() > static_cast<size_t>(UINT32_MAX)) {
      result.error = "USDZ asset exceeds ZIP32 size limit: " + name;
      buffer.clear();
      return result;
    }
    CentralDirEntry asset_entry;
    const uint8_t* data = asset.second.empty() ? nullptr : asset.second.data();
    if (!WriteLocalFileHeader(buffer, name, data, asset.second.size(),
                              &asset_entry, &err,
                              options.max_file_size_bytes)) {
      result.error = err.empty() ? "Failed to write USDZ asset" : err;
      buffer.clear();
      return result;
    }
    entries.push_back(std::move(asset_entry));
  }

  // ZIP32 EOCD limits: 2-byte entry count, 4-byte central-dir offset.
  if (entries.size() > 0xFFFFu || buffer.size() > 0xFFFFFFFFull) {
    result.error = "USDZ archive exceeds ZIP32 limits (ZIP64 not supported)";
    buffer.clear();
    return result;
  }

  // Write central directory
  size_t central_size = 22;
  for (const auto& central_entry : entries) {
    const size_t entry_size = 46 + central_entry.name.size();
    if (entry_size > (std::numeric_limits<size_t>::max)() - central_size) {
      result.error = "USDZ central directory size overflow";
      buffer.clear();
      return result;
    }
    central_size += entry_size;
  }
  if (options.max_file_size_bytes &&
      (static_cast<uint64_t>(buffer.size()) > options.max_file_size_bytes ||
       static_cast<uint64_t>(central_size) >
           options.max_file_size_bytes - static_cast<uint64_t>(buffer.size()))) {
    result.error = "USDZ output exceeds configured file-size limit";
    buffer.clear();
    return result;
  }
  WriteCentralDirectory(buffer, entries);

  result.bytes_written = buffer.size();
  result.success = true;
  return result;
}

USDZWriteResult WriteUSDZFromUSDCToFile(const std::string& filename,
                                         const uint8_t* usdc_data,
                                         size_t usdc_size,
                                         const USDZWriteOptions& options) {
  USDZWriteResult result;
  std::ofstream ofs(filename, std::ios::binary);
  if (!ofs) {
    result.success = false;
    result.error = "Failed to open file for writing: " + filename;
    return result;
  }
  result = WriteUSDZFromUSDCAndAssetsToSink(
      usdc_data, usdc_size, {},
      [&ofs](const uint8_t* bytes, size_t count) {
        ofs.write(reinterpret_cast<const char*>(bytes), static_cast<std::streamsize>(count));
        return ofs.good();
      }, options);
  ofs.flush();
  if (!ofs.good() && result.success) {
    result.success = false;
    result.error = "Write failed (disk full?) for file: " + filename;
  }
  result.bytes_written = FilePositionOrZero(ofs);
  return result;
}

} // namespace next
} // namespace lightusd
