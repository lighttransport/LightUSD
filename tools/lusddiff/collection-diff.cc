// SPDX-License-Identifier: Apache-2.0
#include "collection-diff.hh"
#include "next/pcp/layer-registry.hh"
#include "next/reader/usdz-reader.hh"
#include "minijson.hh"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>

namespace {
namespace fs = std::filesystem;
namespace n = lightusd::next;
using Json = lightusd::minijson::Value;
constexpr size_t kByteLimit = size_t(1) << 30;
constexpr size_t kEntryLimit = 100000;
struct Entry {
  std::string file;
  std::vector<uint8_t> bytes;
  bool usd = false;
};
std::string Extension(const std::string& s) {
  std::string e = fs::path(s).extension().string();
  std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return e;
}
bool IsUSD(const std::string& s) {
  const auto e = Extension(s);
  return e == ".usd" || e == ".usda" || e == ".usdc";
}
bool Read(const std::string& path, std::vector<uint8_t>* out, std::string* error) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  const auto size = in.tellg();
  if (!in || size < 0 || uint64_t(size) > kByteLimit) { *error = "unreadable/oversized file: " + path; return false; }
  out->resize(size_t(size)); in.seekg(0);
  if (size && !in.read(reinterpret_cast<char*>(out->data()), size)) { *error = "read failed: " + path; return false; }
  return true;
}
class Inventory {
 public:
  std::map<std::string, Entry> entries;
  std::string error;
  size_t bytes = 0;
  bool Add(const std::string& name, Entry e) {
    if (entries.size() >= kEntryLimit || entries.count(name)) { error = "entry limit or ambiguous path: " + name; return false; }
    entries.emplace(name, std::move(e)); return true;
  }
  bool Package(const std::string& name, const uint8_t* data, size_t size, size_t depth) {
    if (depth > 16 || size > kByteLimit - bytes) { error = "archive nesting/byte limit exceeded"; return false; }
    bytes += size;
    n::USDZReader reader;
    if (!reader.Open(data, size)) { error = reader.Error(); return false; }
    const int root = reader.FindRootLayer();
    if (root < 0) { error = "archive has no valid root layer"; return false; }
    const auto root_name = reader.EntryName(size_t(root));
    Entry marker;
    marker.bytes.assign(root_name.begin(), root_name.end());
    if (!Add(name + "::<root>", std::move(marker))) return false;
    for (size_t i = 0; i < reader.NumEntries(); ++i) {
      const auto& entry_name = reader.EntryName(i);
      const auto key = name + "[" + entry_name + "]";
      const auto* p = reader.EntryData(i);
      const auto count = reader.EntrySize(i);
      if (Extension(entry_name) == ".usdz") {
        if (!Package(key, p, count, depth + 1)) return false;
      } else {
        Entry e; e.usd = IsUSD(entry_name);
        if (count) e.bytes.assign(p, p + count);
        if (!Add(key, std::move(e))) return false;
      }
    }
    return true;
  }
  bool File(const std::string& name, const std::string& path) {
    if (Extension(path) == ".usdz") {
      std::vector<uint8_t> data;
      return Read(path, &data, &error) && Package(name, data.data(), data.size(), 0);
    }
    Entry e; e.file = path; e.usd = IsUSD(path);
    return Add(name, std::move(e));
  }
  bool Load(const std::string& path) {
    std::error_code ec;
    const bool directory = fs::is_directory(path, ec);
    if (ec) { error = ec.message(); return false; }
    if (!directory) return File("", path);
    fs::recursive_directory_iterator it(path, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
      const auto status = it->symlink_status(ec);
      if (ec) break;
      if (fs::is_symlink(status)) { error = "symlink entries are not followed: " + it->path().string(); return false; }
      if (fs::is_regular_file(status)) {
        const auto name = it->path().lexically_relative(path).generic_string();
        if (!File(name, it->path().string())) return false;
      }
    }
    if (ec) { error = ec.message(); return false; }
    return true;
  }
};
}  // namespace
bool IsDiffDirectory(const std::string& path) {
  std::error_code ec;
  return fs::is_directory(path, ec);
}
int DiffCollections(const std::string& left, const std::string& right,
                    const n::DiffOptions& options, bool json, bool quiet) {
  Inventory a, b;
  if (!a.Load(left) || !b.Load(right)) {
    std::cerr << "lusddiff: " << a.error << b.error << '\n'; return 2;
  }
  std::set<std::string> names;
  for (const auto& e : a.entries) names.insert(e.first);
  for (const auto& e : b.entries) names.insert(e.first);
  Json changes = Json::array();
  bool failed = false;
  for (const auto& name : names) {
    const auto ai = a.entries.find(name), bi = b.entries.find(name);
    Json item{{"path", name}};
    if (ai == a.entries.end()) item["status"] = "added";
    else if (bi == b.entries.end()) item["status"] = "removed";
    else {
      std::vector<uint8_t> av, bv;
      const auto& ae = ai->second; const auto& be = bi->second;
      std::string error;
      if ((!ae.file.empty() && !Read(ae.file, &av, &error)) ||
          (!be.file.empty() && !Read(be.file, &bv, &error))) {
        item["status"] = "error"; item["message"] = error; failed = true;
      } else {
        const auto& x = ae.file.empty() ? ae.bytes : av;
        const auto& y = be.file.empty() ? be.bytes : bv;
        if (ae.usd && be.usd) {
          std::string warn;
          auto l = n::pcp::LoadLayerFromMemory(name, x.data(), x.size(), &warn, &error);
          auto r = n::pcp::LoadLayerFromMemory(name, y.data(), y.size(), &warn, &error);
          if (!l || !r) { item["status"] = "error"; item["message"] = error; failed = true; }
          else {
            const auto text = n::DiffToText(*l, *r, left, right, options);
            if (text == "No differences found.\n") continue;
            item["status"] = "modified"; item["comparison"] = "semantic";
            Json detail;
            if (!lightusd::minijson::Parse(n::DiffToJSON(*l, *r, left, right, options), &detail)) {
              item["status"] = "error"; item["message"] = "cannot serialize semantic diff"; failed = true;
            } else item["diff"] = std::move(detail);
          }
        } else {
          if (x == y) continue;
          item["status"] = "modified"; item["comparison"] = "bytes";
        }
      }
    }
    changes.push_back(std::move(item));
  }
  if (!quiet) {
    if (json) std::cout << Json{{"schemaVersion", 1}, {"left", left}, {"right", right},
        {"complete", !failed}, {"entries", changes}}.dump(2) << '\n';
    else for (const auto& item : changes)
      std::cout << item["status"].get_string() << ' ' << item["path"].get_string()
                << (item.contains("message") ? ": " + item["message"].get_string() : "") << '\n';
  }
  return failed || !std::cout ? 2 : changes.empty() ? 0 : 1;
}
