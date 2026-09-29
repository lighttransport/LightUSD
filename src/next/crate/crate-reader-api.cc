// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - USDC CrateReader public API wrappers

#include "crate-reader-internal.hh"
#include "../safe-file-size.hh"

#include "crate-data-source.hh"
#include "crate-timing.hh"

#include <cstring>
#include <fstream>

namespace lightusd {
namespace next {

CrateReadResult CrateReader::Impl::ReadFromString(std::string&& bytes) {
  if (options_.max_memory && bytes.size() > options_.max_memory) {
    result_ = CrateReadResult();
    AddError("Input exceeds max_memory budget");
    return std::move(result_);
  }

  source_ = CrateDataSource::Adopt(std::move(bytes), CrateVersion{});
  source_->set_max_array_elements(options_.max_array_elements);
  return ParseFromSource();
}

CrateReadResult CrateReader::Impl::ParseFromSource() {
  result_ = CrateReadResult();
  result_.source_was_mmap = source_ && source_->is_mmapped();
  tokens_.clear();
  string_indices_.clear();
  fields_.clear();
  fieldset_indices_.clear();
  specs_.clear();
  paths_.clear();
  path_parent_.clear();

  if (!source_ || source_->size() < kCrateBootstrapSize) {
    AddError("Invalid input data");
    return std::move(result_);
  }

  reader_ = std::make_unique<StreamReader>(source_->base(), source_->size());

  constexpr size_t kPhaseTotal = 10;
  if (!ReportProgress("bootstrap", 0, kPhaseTotal)) return std::move(result_);
  CratePhaseTimer timer(options_.enable_timing, "next_crate_read");
  auto lap = [&](const char* phase) { timer.lap(phase); };
  if (!ReadBootstrap()) return std::move(result_);
  source_->set_version(version_);
  if (!ReportProgress("toc", 1, kPhaseTotal)) return std::move(result_);
  if (!ReadTOC()) return std::move(result_);
  lap("ReadTOC");
  if (!ReportProgress("tokens", 2, kPhaseTotal)) return std::move(result_);
  if (!ReadTokens()) return std::move(result_);
  lap("ReadTokens");
  if (!ReportProgress("strings", 3, kPhaseTotal)) return std::move(result_);
  if (!ReadStrings()) return std::move(result_);
  lap("ReadStrings");
  if (!ReportProgress("fields", 4, kPhaseTotal)) return std::move(result_);
  if (!ReadFields()) return std::move(result_);
  lap("ReadFields");
  if (!ReportProgress("fieldsets", 5, kPhaseTotal)) return std::move(result_);
  if (!ReadFieldsets()) return std::move(result_);
  lap("ReadFieldsets");
  if (!ReportProgress("specs", 6, kPhaseTotal)) return std::move(result_);
  if (!ReadSpecs()) return std::move(result_);
  lap("ReadSpecs");
  if (!ReportProgress("paths", 7, kPhaseTotal)) return std::move(result_);
  if (!ReadPaths()) return std::move(result_);
  lap("ReadPaths");
  if (!ReportProgress("stage", 8, kPhaseTotal)) return std::move(result_);
  if (!BuildStage()) return std::move(result_);
  lap("BuildStage");
  if (!ReportProgress("complete", kPhaseTotal, kPhaseTotal)) {
    return std::move(result_);
  }

  // The structural tables are read; if the file shrank underneath the mapping
  // while we were reading it, say so. We survived this time (the truncated
  // region went untouched), but any lazy value still referencing the mapping
  // can hit SIGBUS later -- see CrateDataSource::MappedFileShrank().
  if (source_ && source_->is_mmapped()) {
    size_t now = 0;
    if (source_->MappedFileShrank(&now)) {
      AddWarning("Mapped file shrank while being read (" +
                 std::to_string(source_->size()) + " -> " +
                 std::to_string(now) +
                 " bytes); lazily-read values may fault. Use "
                 "CrateReadOptions::use_mmap = false for files that other "
                 "processes may rewrite.");
    }
  }

  result_.success = result_.errors.empty();
  result_.version = version_;
  return std::move(result_);
}

CrateReadResult CrateReader::Impl::Read(const uint8_t* data, size_t size) {
  if (!data && size != 0) {
    result_ = CrateReadResult();
    AddError("Invalid input data");
    return std::move(result_);
  }
  if (options_.max_memory && size > options_.max_memory) {
    result_ = CrateReadResult();
    AddError("Input exceeds max_memory budget");
    return std::move(result_);
  }
  if (size == 0) return ReadFromString(std::string());
  return ReadFromString(std::string(reinterpret_cast<const char*>(data), size));
}

CrateReadResult CrateReader::Impl::ReadOwned(std::string&& owned) {
  return ReadFromString(std::move(owned));
}

CrateReadResult CrateReader::Impl::ReadFile(const char* filename) {
  if (!filename || !*filename) {
    CrateReadResult result;
    result.errors.push_back({0, "Invalid filename"});
    return result;
  }
  if (options_.max_memory) {
    std::ifstream probe(filename, std::ios::binary | std::ios::ate);
    if (!probe.is_open()) {
      CrateReadResult result;
      result.errors.push_back({0, std::string("Failed to open file: ") + filename});
      return result;
    }
    std::streamsize probed_size = probe.tellg();
    if (probed_size < 0) {
      CrateReadResult result;
      result.errors.push_back({0, "Failed to determine file size"});
      return result;
    }
    if (static_cast<uint64_t>(probed_size) >
        static_cast<uint64_t>(options_.max_memory)) {
      CrateReadResult result;
      result.errors.push_back({0, "File exceeds max_memory budget"});
      return result;
    }
  }

  if (options_.use_mmap) {
    if (auto src = CrateDataSource::MmapFile(filename)) {
      source_ = std::move(src);
      source_->set_max_array_elements(options_.max_array_elements);
      return ParseFromSource();
    }
  }

  std::ifstream file(filename, std::ios::binary | std::ios::ate);
  if (!file.is_open()) {
    CrateReadResult result;
    result.errors.push_back({0, std::string("Failed to open file: ") + filename});
    return result;
  }

  // Unconditional bound: max_memory is optional, and a directory reports
  // LLONG_MAX here (see SafeStreamSize) which reached std::string(n, '\0').
  size_t fsize = 0;
  if (!SafeStreamSize(file, static_cast<uint64_t>(options_.max_memory),
                      &fsize)) {
    CrateReadResult result;
    result.errors.push_back({0, "Invalid or oversized file size"});
    return result;
  }
  const std::streamsize size = static_cast<std::streamsize>(fsize);
  file.seekg(0, std::ios::beg);

  std::string data(fsize, '\0');
  if (size != 0 && !file.read(data.data(), size)) {
    CrateReadResult result;
    result.errors.push_back({0, "Failed to read file contents"});
    return result;
  }

  return ReadOwned(std::move(data));
}

CrateReader::CrateReader(const CrateReadOptions& options)
    : impl_(std::make_unique<Impl>(options)) {}

CrateReader::~CrateReader() = default;

CrateReader::CrateReader(CrateReader&&) noexcept = default;
CrateReader& CrateReader::operator=(CrateReader&&) noexcept = default;

CrateReadResult CrateReader::Read(const uint8_t* data, size_t size) {
  return impl_->Read(data, size);
}

CrateReadResult CrateReader::ReadOwned(std::string&& owned) {
  return impl_->ReadOwned(std::move(owned));
}

CrateReadResult CrateReader::ReadFile(const char* filename) {
  return impl_->ReadFile(filename);
}

std::vector<std::string> CrateReader::tokens() const {
  return impl_->tokens();
}

std::vector<std::string> CrateReader::paths() const {
  return impl_->paths();
}

const std::vector<CrateField>& CrateReader::fields() const {
  return impl_->fields();
}

const std::vector<CrateSpec>& CrateReader::specs() const {
  return impl_->specs();
}

const std::vector<uint32_t>& CrateReader::fieldset_indices() const {
  return impl_->fieldset_indices();
}

bool IsUSDCData(const uint8_t* data, size_t size) {
  if (!data || size < 8) return false;
  return std::memcmp(data, kCrateMagic, 8) == 0;
}

bool IsUSDCFile(const char* filename) {
  std::ifstream file(filename, std::ios::binary);
  if (!file.is_open()) return false;

  char magic[8];
  file.read(magic, 8);
  if (!file) return false;

  return std::memcmp(magic, kCrateMagic, 8) == 0;
}

}  // namespace next
}  // namespace lightusd
