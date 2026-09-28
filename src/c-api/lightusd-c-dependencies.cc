// SPDX-License-Identifier: Apache-2.0
// Resolver inventory and validation behind the portable C boundary.
#include "c-internal.hh"

#include <map>
#include <limits>
#include <mutex>
#include <new>
#include <cstring>
#include <iterator>
#include <utility>

#include "next/resolver/dependency-report.hh"
#include "next/validation/usd-validation.hh"

namespace n = lightusd::next;
using lightusd_internal::Fail;

struct lightusd_asset_resolver {
  n::AssetResolver resolver;
  std::map<std::string, std::string> aliases;
  std::map<std::string, size_t> memory_asset_sizes;
  size_t memory_asset_bytes{0};
  size_t memory_asset_limit{0};
  mutable std::mutex memory_mutex;

  lightusd_asset_resolver() {
    resolver.SetCustomResolver(
        [this](const std::string& authored, const std::string&) {
          const auto found = aliases.find(authored);
          return found == aliases.end() ? std::string() : found->second;
        });
  }
};

extern "C" {

lightusd_status lightusd_stage_validate_core(const lightusd_stage* stage,
                                              size_t* errors,
                                              size_t* warnings) {
  if (!stage || !errors || !warnings) {
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "stage/errors/warnings is null");
  }
  *errors = 0;
  *warnings = 0;
  const n::Layer* root = stage->ReadStage().GetRootLayer();
  if (!root) return Fail(LIGHTUSD_ERR_NOT_FOUND, "stage has no root layer");
  const n::USDValidationResult result = n::ValidateLayerAgainstAOUSDCore(*root);
  *errors = result.error_count();
  *warnings = result.warning_count();
  return LIGHTUSD_OK;
}

lightusd_status lightusd_asset_resolver_create(lightusd_asset_resolver** out) {
  if (!out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "out is null");
  *out = new (std::nothrow) lightusd_asset_resolver();
  if (!*out) return Fail(LIGHTUSD_ERR_OUT_OF_MEMORY, "resolver alloc failed");
  return LIGHTUSD_OK;
}

void lightusd_asset_resolver_destroy(lightusd_asset_resolver* resolver) {
  delete resolver;
}

lightusd_status lightusd_asset_resolver_register_memory(
    lightusd_asset_resolver* resolver, const char* identifier,
    const uint8_t* data, size_t size) {
  if (!resolver || !identifier || !identifier[0] || (!data && size)) {
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "invalid memory asset");
  }
  std::lock_guard<std::mutex> lock(resolver->memory_mutex);
  const auto previous = resolver->memory_asset_sizes.find(identifier);
  const size_t previous_size = previous == resolver->memory_asset_sizes.end()
                                   ? 0 : previous->second;
  const size_t base_bytes = resolver->memory_asset_bytes - previous_size;
  if (size > (std::numeric_limits<size_t>::max)() - base_bytes) {
    return Fail(LIGHTUSD_ERR_OVERFLOW, "memory asset byte count overflow");
  }
  const size_t next_bytes = base_bytes + size;
  if (resolver->memory_asset_limit && next_bytes > resolver->memory_asset_limit) {
    return Fail(LIGHTUSD_ERR_RESOURCE_LIMIT, "memory asset limit exceeded");
  }
  std::vector<uint8_t> bytes;
  if (size) bytes.assign(data, data + size);
  resolver->resolver.RegisterMemoryAsset(identifier, std::move(bytes));
  resolver->memory_asset_sizes[identifier] = size;
  resolver->memory_asset_bytes = next_bytes;
  return LIGHTUSD_OK;
}

lightusd_status lightusd_asset_resolver_unregister_memory(
    lightusd_asset_resolver* resolver, const char* identifier) {
  if (!resolver || !identifier || !identifier[0]) {
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "invalid memory asset identifier");
  }
  std::lock_guard<std::mutex> lock(resolver->memory_mutex);
  if (!resolver->resolver.UnregisterMemoryAsset(identifier)) {
    return Fail(LIGHTUSD_ERR_NOT_FOUND, "memory asset was not registered");
  }
  const auto found = resolver->memory_asset_sizes.find(identifier);
  if (found != resolver->memory_asset_sizes.end()) {
    resolver->memory_asset_bytes -= found->second;
    resolver->memory_asset_sizes.erase(found);
  }
  return LIGHTUSD_OK;
}

lightusd_status lightusd_asset_resolver_set_memory_limit(
    lightusd_asset_resolver* resolver, size_t limit_bytes) {
  if (!resolver) return Fail(LIGHTUSD_ERR_INVALID_ARG, "resolver is null");
  std::lock_guard<std::mutex> lock(resolver->memory_mutex);
  if (limit_bytes && resolver->memory_asset_bytes > limit_bytes) {
    return Fail(LIGHTUSD_ERR_RESOURCE_LIMIT,
                "memory asset limit is below current usage");
  }
  resolver->memory_asset_limit = limit_bytes;
  return LIGHTUSD_OK;
}

lightusd_status lightusd_asset_resolver_get_memory_stats(
    const lightusd_asset_resolver* resolver, size_t* asset_count,
    size_t* bytes_used, size_t* limit_bytes) {
  if (!resolver || !asset_count || !bytes_used || !limit_bytes) {
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "invalid resolver stats arguments");
  }
  std::lock_guard<std::mutex> lock(resolver->memory_mutex);
  *asset_count = resolver->memory_asset_sizes.size();
  *bytes_used = resolver->memory_asset_bytes;
  *limit_bytes = resolver->memory_asset_limit;
  return LIGHTUSD_OK;
}

lightusd_status lightusd_asset_resolver_memory_identifier(
    const lightusd_asset_resolver* resolver, size_t index, char* out,
    size_t capacity, size_t* required_size) {
  if (!resolver || !required_size || (!out && capacity != 0)) {
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "invalid memory identifier arguments");
  }
  std::lock_guard<std::mutex> lock(resolver->memory_mutex);
  if (index >= resolver->memory_asset_sizes.size()) {
    *required_size = 0;
    return Fail(LIGHTUSD_ERR_NOT_FOUND, "memory asset index is out of range");
  }
  auto it = resolver->memory_asset_sizes.begin();
  std::advance(it, static_cast<std::ptrdiff_t>(index));
  *required_size = it->first.size();
  if (!out) return LIGHTUSD_OK;
  if (capacity < *required_size) {
    return Fail(LIGHTUSD_ERR_RESOURCE_LIMIT,
                "memory asset identifier buffer is too small");
  }
  if (*required_size) std::memcpy(out, it->first.data(), *required_size);
  return LIGHTUSD_OK;
}

lightusd_status lightusd_asset_resolver_set_alias(
    lightusd_asset_resolver* resolver, const char* authored_path,
    const char* resolved_identifier) {
  if (!resolver || !authored_path || !authored_path[0] ||
      !resolved_identifier || !resolved_identifier[0]) {
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "invalid asset alias");
  }
  resolver->aliases[authored_path] = resolved_identifier;
  return LIGHTUSD_OK;
}

lightusd_status lightusd_asset_resolver_read(
    const lightusd_asset_resolver* resolver, const char* resolved_identifier,
    lightusd_string** out) {
  if (!resolver || !resolved_identifier || !out) {
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "invalid asset read arguments");
  }
  *out = nullptr;
  std::vector<uint8_t> bytes;
  std::string error;
  if (!resolver->resolver.ReadAsset(resolved_identifier, &bytes, &error)) {
    return Fail(LIGHTUSD_ERR_IO, error);
  }
  lightusd_string* result = new (std::nothrow) lightusd_string();
  if (!result) return Fail(LIGHTUSD_ERR_OUT_OF_MEMORY, "asset alloc failed");
  if (!bytes.empty()) {
    result->s.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  }
  *out = result;
  return LIGHTUSD_OK;
}

lightusd_status lightusd_dependency_report_json(
    lightusd_asset_resolver* resolver, const char* root,
    lightusd_string** out, uint8_t* complete) {
  if (!resolver || !root || !root[0] || !out || !complete) {
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "invalid dependency report arguments");
  }
  *out = nullptr;
  *complete = 0;
  const n::DependencyReport report =
      n::CollectDependencies(root, resolver->resolver);
  lightusd_string* result = new (std::nothrow) lightusd_string();
  if (!result) return Fail(LIGHTUSD_ERR_OUT_OF_MEMORY, "report alloc failed");
  result->s = n::DependencyReportToJSON(report);
  *complete = report.complete ? 1 : 0;
  *out = result;
  return LIGHTUSD_OK;
}

}  // extern "C"
