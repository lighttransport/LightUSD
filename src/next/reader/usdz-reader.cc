#include "core/native-io.hh"
#include "usdz-reader.hh"
#include "../safe-file-size.hh"
#include "../c/lightusd-core-c.h"
#include <cstring>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <limits>
#include <unordered_set>

namespace lightusd {
namespace next {

// ZIP local file header signature
static const uint32_t kLocalFileHeaderSig = 0x04034b50;
static const uint32_t kCentralDirectoryHeaderSig = 0x02014b50;
static const uint32_t kEndOfCentralDirectorySig = 0x06054b50;

#pragma pack(push, 1)
struct LocalFileHeader {
  uint32_t signature;
  uint16_t version_needed;
  uint16_t flags;
  uint16_t compression;
  uint16_t mod_time;
  uint16_t mod_date;
  uint32_t crc32;
  uint32_t compressed_size;
  uint32_t uncompressed_size;
  uint16_t filename_length;
  uint16_t extra_field_length;
};
#pragma pack(pop)

namespace {

bool CheckedAdd(size_t a, size_t b, size_t* out) {
  if (a > (std::numeric_limits<size_t>::max)() - b) return false;
  *out = a + b;
  return true;
}

// URL-decode a percent-encoded string in-place.
// Returns false if the input contains an invalid percent sequence.
static bool URLDecode(std::string *s) {
  std::string out;
  out.reserve(s->size());
  for (size_t i = 0; i < s->size(); i++) {
    if ((*s)[i] == '%') {
      if (i + 2 >= s->size()) return false;
            int hi = lu_lex_hex_val((*s)[i + 1]);
      int lo = lu_lex_hex_val((*s)[i + 2]);
      if (hi < 0 || lo < 0) return false;
      out.push_back(static_cast<char>((hi << 4) | lo));
      i += 2;
    } else {
      out.push_back((*s)[i]);
    }
  }
  *s = std::move(out);
  return true;
}

bool IsSafeEntryName(const std::string& name) {
  if (name.empty()) return false;

  // Normalize: normalize backslashes, then URL-decode, so that
  // percent-encoded path separators / segments cannot bypass the check.
  std::string n = name;
  for (char &c : n) {
    if (c == '\\') c = '/';
  }
  if (!URLDecode(&n)) return false;

  if (n.front() == '/' ||
      n.find('\0') != std::string::npos) {
    return false;
  }
  if (n.size() >= 2 &&
      ((n[0] >= 'A' && n[0] <= 'Z') ||
       (n[0] >= 'a' && n[0] <= 'z')) &&
      n[1] == ':') {
    return false;
  }
  size_t start = 0;
  while (start < n.size()) {
    const size_t slash = n.find('/', start);
    const size_t end = slash == std::string::npos ? n.size() : slash;
    const std::string part = n.substr(start, end - start);
    if (part.empty() || part == "." || part == "..") return false;
    if (slash == std::string::npos) return true;
    start = slash + 1;
  }
  return false;  // trailing slash / directory entry
}

}  // namespace

bool USDZReader::OpenFile(const std::string& filename,
                          const USDZReadOptions& options) {
  if (!::lightusd::NativeIOAvailable(&error_)) return false;

  error_.clear();
  entries_.clear();
  data_ = nullptr;
  data_size_ = 0;
  file_data_.clear();
  std::ifstream ifs(filename, std::ios::binary | std::ios::ate);
  if (!ifs) {
    error_ = "failed to open USDZ file: " + filename;
    return false;
  }
  size_t sz = 0;
  if (!SafeStreamSize(ifs, static_cast<uint64_t>(options.max_archive_size),
                      &sz)) {
    error_ = "invalid or oversized USDZ file size: " + filename;
    return false;
  }
  ifs.seekg(0);
  file_data_.resize(sz);
  if (sz && !ifs.read(reinterpret_cast<char*>(file_data_.data()),
                      static_cast<std::streamsize>(sz))) {
    error_ = "failed to read USDZ file: " + filename;
    file_data_.clear();
    return false;
  }
  ifs.close();
  return Open(file_data_.data(), sz, options);
}

bool USDZReader::Open(const uint8_t* data, size_t size,
                      const USDZReadOptions& options) {
  error_.clear();
  data_ = data;
  data_size_ = size;
  entries_.clear();

  if (!data || size < 4) {
    error_ = "empty or truncated USDZ archive";
    return false;
  }
  if (options.max_archive_size > 0 && size > options.max_archive_size) {
    error_ = "USDZ archive exceeds maximum memory limit";
    return false;
  }

  size_t pos = 0;
  std::unordered_set<std::string> entry_names;
  while (pos + sizeof(LocalFileHeader) <= size) {
    LocalFileHeader hdr;
    std::memcpy(&hdr, data + pos, sizeof(hdr));

    if (hdr.signature != kLocalFileHeaderSig) break;

    size_t name_pos = 0;
    size_t extra_pos = 0;
    size_t data_pos = 0;
    size_t next_pos = 0;
    if (!CheckedAdd(pos, sizeof(hdr), &name_pos) ||
        !CheckedAdd(name_pos, static_cast<size_t>(hdr.filename_length), &extra_pos) ||
        !CheckedAdd(extra_pos, static_cast<size_t>(hdr.extra_field_length), &data_pos) ||
        !CheckedAdd(data_pos, static_cast<size_t>(hdr.compressed_size), &next_pos) ||
        next_pos > size) {
      error_ = "truncated USDZ local file header";
      entries_.clear();
      return false;
    }

    // USDZ requires every entry to be stored. Skipping a compressed entry would
    // also corrupt positional root semantics by making a later stored layer look
    // like entries_[0].
    if (hdr.compression != 0) {
      error_ = "compressed ZIP entry is not allowed in USDZ";
      entries_.clear();
      return false;
    }
    if (hdr.uncompressed_size != hdr.compressed_size) {
      error_ = "invalid stored USDZ entry size";
      entries_.clear();
      return false;
    }
    if (options.max_entry_size > 0 &&
        static_cast<size_t>(hdr.uncompressed_size) > options.max_entry_size) {
      error_ = "USDZ entry exceeds maximum memory limit";
      entries_.clear();
      return false;
    }

    std::string name(reinterpret_cast<const char*>(data + name_pos),
                     hdr.filename_length);
    if (!IsSafeEntryName(name)) {
      error_ = "unsafe USDZ entry path: " + name;
      entries_.clear();
      return false;
    }
    if (!entry_names.insert(name).second) {
      error_ = "duplicate USDZ entry path: " + name;
      entries_.clear();
      return false;
    }

    if (options.max_entries > 0 &&
        entries_.size() >= options.max_entries) {
      error_ = "USDZ archive contains too many entries";
      entries_.clear();
      return false;
    }

    Entry entry;
    entry.name = name;
    entry.offset = data_pos;
    entry.size = static_cast<size_t>(hdr.uncompressed_size);
    entries_.push_back(entry);

    pos = next_pos;
  }

  // Stored-entry-only test fixtures may end exactly after their local records.
  // Real ZIP/USDZ files continue with a central directory. Anything else means
  // a truncated next header or unrecognized trailing payload and must not be
  // hidden merely because an earlier valid entry was already collected.
  if (pos < size) {
    uint32_t trailing_sig = 0;
    if (size - pos >= sizeof(trailing_sig)) {
      std::memcpy(&trailing_sig, data + pos, sizeof(trailing_sig));
    }
    if (trailing_sig != kCentralDirectoryHeaderSig &&
        trailing_sig != kEndOfCentralDirectorySig) {
      error_ = "invalid or truncated data after USDZ entries";
      entries_.clear();
      return false;
    }
  }

  if (entries_.empty()) {
    if (error_.empty()) error_ = "no stored entries found in USDZ archive";
    return false;
  }
  error_.clear();
  return true;
}

const std::string& USDZReader::EntryName(size_t index) const {
  // Bounds-check like EntryData()/EntrySize() below: an out-of-range index
  // otherwise returns a reference into freed/unrelated memory.
  static const std::string kEmpty;
  if (index >= entries_.size()) return kEmpty;
  return entries_[index].name;
}

const uint8_t* USDZReader::EntryData(size_t index) const {
  if (index >= entries_.size()) return nullptr;
  return data_ + entries_[index].offset;
}

size_t USDZReader::EntrySize(size_t index) const {
  if (index >= entries_.size()) return 0;
  return entries_[index].size;
}

int USDZReader::FindRootLayer() const {
  if (entries_.empty()) return -1;
  const Entry& root = entries_[0];
  auto endsWithNoCase = [](const std::string& s, const char* suffix) {
    const size_t n = std::strlen(suffix);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i) {
      const unsigned char a = static_cast<unsigned char>(s[s.size() - n + i]);
      const unsigned char b = static_cast<unsigned char>(suffix[i]);
      if (std::tolower(a) != std::tolower(b)) return false;
    }
    return true;
  };
  const uint8_t* p = EntryData(0);
  const bool usdc = p && root.size >= 8 && std::memcmp(p, "PXR-USDC", 8) == 0;
  const bool usda = p && root.size >= 5 && std::memcmp(p, "#usda", 5) == 0;
  if (endsWithNoCase(root.name, ".usdc")) return usdc ? 0 : -1;
  if (endsWithNoCase(root.name, ".usda")) return usda ? 0 : -1;
  if (endsWithNoCase(root.name, ".usd")) return (usdc || usda) ? 0 : -1;
  return -1;
}

int USDZReader::FindUSDCFile() const {
  for (size_t i = 0; i < entries_.size(); i++) {
    const auto& name = entries_[i].name;
    // USDZ entries often have a leading "/" or not
    size_t pos = name.rfind(".usdc");
    if (pos != std::string::npos && pos == name.size() - 5) return static_cast<int>(i);
  }
  return -1;
}

int USDZReader::FindUSDAFile() const {
  for (size_t i = 0; i < entries_.size(); i++) {
    const auto& name = entries_[i].name;
    size_t pos = name.rfind(".usda");
    if (pos != std::string::npos && pos == name.size() - 5) return static_cast<int>(i);
  }
  return -1;
}

} // namespace next
} // namespace lightusd
