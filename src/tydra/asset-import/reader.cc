// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <cctype>
#include <fstream>
#include <limits>

#include "external/filesystem/include/ghc/filesystem.hpp"
#include "import.hh"
namespace fs = ghc::filesystem;
namespace lightusd {
namespace tydra {
namespace import_asset {
namespace {
size_t SaturatingAdd(size_t a, size_t b) {
  return b > SIZE_MAX - a ? SIZE_MAX : a + b;
}
constexpr size_t kReportTextLimit = 4096;
size_t TextBytes(const std::string& value) {
  return std::min(value.size(), kReportTextLimit);
}
}  // namespace
bool ImportReport::Reserve(size_t amount, size_t entries) {
  if (entries >= 1024 || amount > storage_limit_ - std::min(storage_bytes_, storage_limit_)) {
    dropped_records = SaturatingAdd(dropped_records, 1);
    return false;
  }
  storage_bytes_ += amount;
  return true;
}
std::string ImportReport::Text(const std::string& value) {
  if (value.size() > kReportTextLimit) ++truncated_fields;
  const auto bounded = value.substr(0, kReportTextLimit);
  // Parser diagnostics can contain arbitrary file bytes. Keep the report valid
  // UTF-8 even when the source has a malformed filename or a truncated codepoint.
  if (!Json(bounded).dump().empty()) return bounded;
  const char hex[] = "0123456789abcdef";
  std::string safe;
  for (unsigned char c : bounded) {
    const size_t width = c < 128 ? 1 : 4;
    if (safe.size() + width > kReportTextLimit) { ++truncated_fields; break; }
    if (c < 128) safe += static_cast<char>(c);
    else { safe += "\\x"; safe += hex[c >> 4]; safe += hex[c & 15]; }
  }
  return safe;
}
void ImportReport::Diagnostic(const char* severity, const char* code,
    const std::string& message, const std::string& location, size_t line,
    const std::string& prim_path) {
  if (!Reserve(1024 + 6 * (TextBytes(message) + TextBytes(location.empty() ? source : location) + TextBytes(prim_path)),
               diagnostics.size())) return;
  diagnostics.push_back(Json{{"severity", severity}, {"code", code},
      {"message", Text(message)}, {"source", Text(location.empty() ? source : location)},
      {"line", uint64_t(line)}, {"prim_path", Text(prim_path)}});
}
void ImportReport::Dependency(const std::string& name, const std::string& anchor,
    const std::string& identifier, size_t bytes, bool read, bool cached,
    bool success, bool texture) {
  if (read) {
    dependency_reads = SaturatingAdd(dependency_reads, 1);
    dependency_bytes = SaturatingAdd(dependency_bytes, bytes);
  }
  if (cached) texture_cache_hits = SaturatingAdd(texture_cache_hits, 1);
  if (!Reserve(1536 + 6 * (TextBytes(name) + TextBytes(anchor) + TextBytes(identifier)),
               dependencies.size())) return;
  dependencies.push_back(Json{{"name", Text(name)}, {"anchor", Text(anchor)},
      {"identifier", Text(identifier)}, {"bytes", uint64_t(bytes)}, {"read", read},
      {"cached", cached}, {"success", success}, {"kind", texture ? "texture" : "asset"}});
}
void ImportReport::Feature(const char* name, const char* disposition,
    const std::string& location, size_t line) {
  if (!Reserve(768 + 6 * TextBytes(location.empty() ? source : location), features.size())) return;
  features.push_back(Json{{"name", name}, {"disposition", disposition},
      {"source", Text(location.empty() ? source : location)}, {"line", uint64_t(line)}});
}
Json ImportReport::ToJSON() const {
  return Json{{"version", 1}, {"source", source}, {"format", format}, {"status", status}, {"error", error},
      {"diagnostics", diagnostics}, {"dependencies", dependencies}, {"features", features},
      {"counts", counts}, {"elapsed_seconds", elapsed_seconds},
      {"input_bytes", uint64_t(input_bytes)}, {"estimated_peak_memory_bytes", uint64_t(peak_memory)},
      {"retained_texture_bytes", uint64_t(retained_texture_bytes)},
      {"dependency_reads", uint64_t(dependency_reads)}, {"dependency_bytes", uint64_t(dependency_bytes)},
      {"texture_cache_hits", uint64_t(texture_cache_hits)},
      {"telemetry_estimated_bytes", uint64_t(storage_bytes_)},
      {"dropped_records", uint64_t(dropped_records)}, {"truncated_fields", uint64_t(truncated_fields)},
      {"limits", Json{{"max_memory_bytes", uint64_t(max_memory)},
                       {"max_input_bytes", uint64_t(max_input)}, {"max_depth", uint64_t(max_depth)}}}};
}
ImportSession::ImportSession(Options* options, const std::string& source,
    const std::string& format, const std::string* error)
    : error_(error), started_(std::chrono::steady_clock::now()) {
  if (!options->report) options->report = std::make_shared<ImportReport>();
  report_ = options->report;
  if (!report_->sessions_) {
    *report_ = ImportReport{};
    report_->storage_limit_ = std::min(size_t(1024 * 1024), options->max_memory / 16);
    report_->source = report_->Text(source);
    report_->format = report_->Text(format);
    report_->max_memory = options->max_memory;
    report_->max_input = options->max_input;
    report_->max_depth = options->max_depth;
  }
  ++report_->sessions_;
}
ImportSession::~ImportSession() {
  if (--report_->sessions_) return;
  report_->status = success_ ? "success" : "failed";
  report_->elapsed_seconds = std::chrono::duration<double>(
      std::chrono::steady_clock::now() - started_).count();
  if (!success_) {
    report_->error = report_->Text(error_ && !error_->empty() ? *error_ : "Import failed");
    report_->Diagnostic("error", "import.failed", report_->error);
  }
}
bool Reader::Charge(size_t amount, std::string* err) {
  if (amount > options.max_memory - std::min(bytes, options.max_memory)) {
    if (failure.empty() && options.report)
      options.report->Diagnostic("error", "import.memory_limit", "Import memory budget exceeded", root_source);
    failure = "Import memory budget exceeded";
    if (err) *err = failure;
    return false;
  }
  bytes += amount;
  if (options.report) options.report->ObserveMemory(bytes);
  if (options.progress && bytes - reported_bytes >= (size_t(1) << 20)) {
    reported_bytes = bytes;
    if (!options.progress("import", bytes, options.max_memory)) {
      failure = "Import cancelled";
      if (err) *err = failure;
      return false;
    }
  }
  return true;
}
bool Reader::ChargeArray(size_t count, size_t stride, std::string* err) {
  if (stride && count > SIZE_MAX / stride) {
    failure = "Import array size overflow (memory limit)";
    if (err) *err = failure;
    return false;
  }
  return Charge(count * stride, err);
}
void Reader::Warn(const std::string& message, const char* code,
                   const std::string& source, size_t line) {
  if (options.report) options.report->Diagnostic("warning", code, message,
      source.empty() ? root_source : source, line);
  const size_t cap = 1024 * 1024;
  if (warnings.size() >= cap) return;
  warnings.append(message.data(), std::min(message.size(), cap - warnings.size()));
  if (warnings.size() < cap) warnings += '\n';
}
bool Reader::Unsupported(const std::string& message, const char* code,
    const char* feature, const std::string& source, size_t line, std::string data,
    const char* encoding, bool approximated) {
  const auto policy = options.unsupported_policy;
  if (policy == UnsupportedPolicy::Error) {
    failure = message;
    if (options.report) {
      options.report->Diagnostic("error", code, message, source, line);
      options.report->Feature(feature, "rejected", source, line);
    }
    return false;
  }
  Warn(message, code, source, line);
  const char* disposition = approximated ? "approximated" : "skipped";
  if (policy == UnsupportedPolicy::Preserve) {
    if (data.empty()) {
      failure = "Cannot preserve unsupported feature: " + std::string(feature);
      return false;
    }
    if (!ChargeArray(data.size(), 8, &failure) || !Charge(2048, &failure)) return false;
    Json record{{"feature", feature}, {"source", source},
        {"line", uint64_t(line)}, {"encoding", encoding}, {"data", std::move(data)},
        {"executable", false}};
    // Unlike diagnostic text, preserved source records must not be changed or
    // truncated to make them serializable. Validate the complete record.
    std::string checked;
    if (!::lightusd::minijson::Serialize(record, &checked)) {
      failure = "Unsupported source contains invalid UTF-8 and cannot be preserved";
      return false;
    }
    preserved.push_back(std::move(record));
    disposition = "preserved";
  }
  if (options.report) options.report->Feature(feature, disposition, source, line);
  return true;
}
bool Reader::Open(const std::string& name, const std::string& anchor,
                  AssetSource* source, std::string* err) {
  if (!failure.empty()) {
    if (err) *err = failure;
    return false;
  }
  if (!allow_unanchored && !options.read && !options.open) {
    if (err) *err = "External assets require a source anchor or resolver";
    return false;
  }
  if (options.open) return options.open(name, anchor, source, err);
  if (options.read) {
    if (err) *err = "Asset resolver provides combined resolution/read only";
    return false;
  }
  std::string path = name;
  if (path.rfind("package://", 0) == 0) {
    size_t end = path.find('/', 10);
    std::string package =
        path.substr(10, end == std::string::npos ? end : end - 10);
    auto found = options.packages.find(package);
    if (found == options.packages.end()) {
      if (err) *err = "Unmapped URDF package: " + package;
      return false;
    }
    path = (fs::path(found->second) /
            (end == std::string::npos ? "" : path.substr(end + 1)))
               .string();
  } else if (path.rfind("file://", 0) == 0)
    path = path.substr(7);
  std::error_code ec;
  // Anchors such as "mesh.obj" have no directory part. Resolve them against the
  // working directory first: an empty base must never become an empty (and
  // therefore all-accepting) containment root.
  const fs::path cwd = fs::current_path(ec);
  const auto absolute = [&](const fs::path& p) {
    return p.is_absolute() ? p : (cwd / p);
  };
  fs::path base = anchor.empty() ? cwd : absolute(fs::path(anchor)).parent_path();
  const auto locate = [&](const std::string& relative) {
    fs::path c = fs::path(relative).is_absolute() ? fs::path(relative) : base / relative;
    if (fs::exists(c, ec)) return c;
    for (const std::string& dir : options.asset_dirs) {
      if (dir.empty()) continue;
      fs::path p = absolute(fs::path(dir)) / relative;
      if (fs::exists(p, ec)) return p;
    }
    return fs::path();
  };
  fs::path candidate = locate(path);
  // Windows-authored MTL/URDF files commonly use '\' separators. Retry with
  // portable separators only when the literal name does not exist.
  if (candidate.empty() && path.find('\\') != std::string::npos) {
    std::string portable = path;
    std::replace(portable.begin(), portable.end(), '\\', '/');
    candidate = locate(portable);
    if (!candidate.empty()) path = portable;
  }
  if (candidate.empty())
    candidate = fs::path(path).is_absolute() ? fs::path(path) : base / path;
  candidate = fs::weakly_canonical(candidate, ec);
  // A dependency may be anywhere under the root document or explicitly
  // supplied asset/package roots. Canonicalization also rejects symlink
  // escapes. Empty roots are ignored; every root is absolute. An empty anchor
  // is the caller-selected root document itself.
  if (!anchor.empty()) {
    std::vector<fs::path> roots;
    const auto add_root = [&](const std::string& r) {
      if (r.empty()) return;
      fs::path root = fs::weakly_canonical(absolute(fs::path(r)), ec).lexically_normal();
      if (root.empty()) return;
      if (!root.has_filename() && root != root.root_path()) root = root.parent_path();
      roots.push_back(root);
    };
    for (const std::string& r : options.asset_dirs) add_root(r);
    for (const auto& p : options.packages) add_root(p.second);
    add_root(base.string());
    if (!root_source.empty())
      add_root(absolute(fs::path(root_source)).parent_path().string());
    bool allowed = false;
    for (const fs::path& root : roots) {
      auto ci = candidate.begin(), ri = root.begin();
      for (; ri != root.end() && ci != candidate.end() && *ri == *ci;
           ++ri, ++ci) {
      }
      if (ri == root.end()) {
        allowed = true;
        break;
      }
    }
    if (!allowed) {
      if (err) *err = "Asset outside allowed roots: " + candidate.string();
      return false;
    }
  }
  source->identifier = candidate.generic_string();
  source->read = [this, candidate](std::vector<uint8_t>* data, std::string* why) {
    std::error_code error;
    const uintmax_t size = fs::file_size(candidate, error);
    if (error) {
      if (why) *why = "Cannot read asset: " + candidate.string();
      return false;
    }
    if (size > options.max_input ||
        size > size_t(std::numeric_limits<std::streamsize>::max())) {
      failure = "Asset input limit exceeded";
      if (why) *why = failure;
      return false;
    }
    if (size > options.max_memory - std::min(bytes, options.max_memory))
      return Charge(static_cast<size_t>(size), why);
    std::ifstream file(candidate.string(), std::ios::binary);
    if (!file) {
      if (why) *why = "Cannot open asset: " + candidate.string();
      return false;
    }
    data->resize(static_cast<size_t>(size));
    if (size && !file.read(reinterpret_cast<char*>(data->data()),
                           static_cast<std::streamsize>(size))) {
      if (why) *why = "Failed to read asset: " + candidate.string();
      return false;
    }
    return true;
  };
  return true;
}

namespace {
struct DependencyTrace {
  const Options& options;
  const std::string& name;
  const std::string& anchor;
  std::string identifier;
  size_t bytes = 0;
  bool read = false, cached = false, success = false, texture = false;
  ~DependencyTrace() {
    if (options.report) options.report->Dependency(name, anchor, identifier, bytes,
        read, cached, success, texture);
  }
};
bool CheckInput(Reader& reader, size_t size, std::string* err) {
  if (size <= reader.options.max_input) return true;
  if (reader.failure.empty() && reader.options.report)
    reader.options.report->Diagnostic("error", "import.input_limit", "Asset input limit exceeded", reader.root_source);
  reader.failure = "Asset input limit exceeded";
  if (err) *err = reader.failure;
  return false;
}
}

bool Reader::Read(const std::string& name, const std::string& anchor,
                  std::vector<uint8_t>* data, std::string* resolved,
                  std::string* err) {
  DependencyTrace trace{options, name, anchor};
  if (!failure.empty()) {
    if (err) *err = failure;
    return false;
  }
  if (options.read && !options.open) {
    trace.read = true;
    const bool ok = options.read(name, anchor, data, &trace.identifier, err);
    trace.bytes = data->size();
    if (!ok) return false;
  } else {
    AssetSource source;
    if (!Open(name, anchor, &source, err)) return false;
    trace.identifier = std::move(source.identifier);
    if (!source.read) return false;
    trace.read = true;
    const bool ok = source.read(data, err);
    trace.bytes = data->size();
    if (!ok) return false;
  }
  *resolved = trace.identifier;
  trace.success = CheckInput(*this, data->size(), err) && Charge(data->size(), err);
  return trace.success;
}

bool Reader::Account(const std::string& name, const std::string& anchor,
                     const std::string& identifier, size_t size, std::string* err) {
  DependencyTrace trace{options, name, anchor};
  trace.identifier = identifier;
  trace.read = true;
  trace.bytes = size;
  trace.success = failure.empty() && CheckInput(*this, size, err) && Charge(size, err);
  if (!failure.empty() && err && err->empty()) *err = failure;
  return trace.success;
}

bool Reader::ReadTexture(const std::string& name, const std::string& anchor,
                         AssetBytes* data, std::string* resolved,
                         std::string* err) {
  DependencyTrace trace{options, name, anchor};
  trace.texture = true;
  if (!failure.empty()) {
    if (err) *err = failure;
    return false;
  }
  const auto key = std::make_pair(anchor, name);
  auto request = texture_requests.find(key);
  if (request != texture_requests.end()) {
    *resolved = request->second.resolved;
    auto retained = textures.find(request->second.canonical);
    if (retained == textures.end()) {
      if (err) *err = "Missing retained texture cache entry";
      return false;
    }
    *data = retained->second;
    trace.identifier = *resolved;
    trace.bytes = (*data)->size();
    trace.cached = trace.success = true;
    return true;
  }
  const auto remember = [&](const std::string& resolved_identifier,
                            const std::string& cache_key) {
    const size_t key_bytes = cache_key == resolved_identifier
                                 ? resolved_identifier.size()
                                 : resolved_identifier.size() + cache_key.size();
    return Charge(sizeof(decltype(texture_requests)::value_type) + 4 * sizeof(void*), err) &&
           Charge(anchor.size(), err) && Charge(name.size(), err) && Charge(key_bytes, err);
  };
  AssetSource source;
  std::vector<uint8_t> buffer;
  std::string& identifier = trace.identifier;
  const bool separate = options.open || !options.read;
  if (separate) {
    if (!Open(name, anchor, &source, err)) return false;
    identifier = source.identifier;
  } else {
    trace.read = true;
    const bool ok = options.read(name, anchor, &buffer, &identifier, err);
    trace.bytes = buffer.size();
    if (!ok || !CheckInput(*this, buffer.size(), err)) return false;
  }
  if (identifier.empty()) {
    if (err) *err = "Texture resolver returned an empty identifier";
    return false;
  }
  // Alias paths keep their authored identifier for USD output while sharing
  // one retained payload through the canonical key.
  const std::string cache_key =
      source.canonical.empty() ? identifier : source.canonical;
  auto found = textures.find(cache_key);
  if (found == textures.end()) {
    if (separate) {
      if (!source.read) return false;
      trace.read = true;
      const bool ok = source.read(&buffer, err);
      trace.bytes = buffer.size();
      if (!ok) return false;
    }
    if (!CheckInput(*this, buffer.size(), err) || !Charge(buffer.size(), err) ||
        !Charge(sizeof(AssetMap::value_type) + sizeof(std::vector<uint8_t>) +
                6 * sizeof(void*), err) || !Charge(cache_key.size(), err)) return false;
    found = textures.emplace(cache_key,
        std::make_shared<const std::vector<uint8_t>>(std::move(buffer))).first;
    if (options.report) options.report->retained_texture_bytes = SaturatingAdd(
        options.report->retained_texture_bytes, found->second->size());
  } else trace.cached = true;
  if (!remember(identifier, cache_key)) return false;
  texture_requests.emplace(key, TextureRequest{identifier, cache_key});
  *resolved = identifier;
  *data = found->second;
  trace.bytes = (*data)->size();
  trace.success = true;
  return true;
}

bool GatherTextures(Reader& reader, Result* result, std::string* err) {
  Json* mats = result->payload.find("materials");
  if (!mats) return true;
  // Deduplicate by retained payload, not by authored identifier: aliases that
  // resolve to one canonical texture must package one entry.
  std::map<const std::vector<uint8_t>*, std::string> packages;
  for (Json& mat : *mats) {
    auto retain = [&](const std::string& file) -> std::string {
      if (file.empty()) return {};
      AssetBytes data;
      std::string resolved, why;
      // Material maps already carry identifiers resolved by ReadTexture. Do
      // not resolve them again: a custom resolver may reject an identifier as
      // a request name, and a second Open repeats I/O and canonicalization.
      auto retained = reader.textures.find(file);
      if (retained != reader.textures.end()) {
        resolved = retained->first;
        data = retained->second;
      } else if (!reader.ReadTexture(file, reader.root_source, &data, &resolved, &why)) {
        reader.Warn("Texture: " + why, "import.texture_unresolved");
        return {};
      }
      auto previous = packages.find(data.get());
      if (previous != packages.end()) return previous->second;
      std::string name = "textures/" + std::to_string(packages.size()) + "_" +
                         fs::path(resolved).filename().string();
      packages.emplace(data.get(), name);
      result->assets.emplace(name, std::move(data));
      return name;
    };
    if (mat.contains("texture_abs")) {
      std::string file = mat["texture_abs"].get<std::string>();
      std::string package = retain(file);
      if (!package.empty()) {
        mat["texture"] = file;
        mat["texturePackage"] = package;
      }
    }
    if (Json* maps = mat.find("maps"))
      for (Json& map : *maps) {
        std::string package = retain(map["file"].get<std::string>());
        if (!package.empty()) map["package"] = package;
      }
  }
  if (!reader.failure.empty()) {
    if (err) *err = reader.failure;
    return false;
  }
  return true;
}
}  // namespace import_asset
}  // namespace tydra
}  // namespace lightusd
