// SPDX-License-Identifier: Apache-2.0
// Resolver inventory and validation behind the portable C boundary.
#include "c-internal.hh"

#include <atomic>
#include <cctype>
#include <map>
#include <limits>
#include <mutex>
#include <new>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>
#include <utility>

#include "next/resolver/dependency-report.hh"

namespace n = lightusd::next;
using lightusd_internal::Fail;

namespace {

struct CallbackUserdata {
  std::atomic<size_t> refs{1};
  void* data = nullptr;
  lightusd_userdata_destroy_fn destroy = nullptr;
};

// Intrusive ownership for callback userdata. Borrowed refs carry no control
// block; owned refs release userdata after the final in-flight callback copy.
class UserdataRef {
 public:
  UserdataRef() = default;
  UserdataRef(const UserdataRef& other) { CopyFrom(other); }
  UserdataRef(UserdataRef&& other) noexcept : owned_(other.owned_),
                                               raw_(other.raw_) {
    other.owned_ = nullptr;
    other.raw_ = nullptr;
  }
  UserdataRef& operator=(const UserdataRef& other) {
    if (this != &other) {
      UserdataRef copy(other);
      Swap(copy);
    }
    return *this;
  }
  UserdataRef& operator=(UserdataRef&& other) noexcept {
    if (this != &other) {
      Release();
      owned_ = other.owned_;
      raw_ = other.raw_;
      other.owned_ = nullptr;
      other.raw_ = nullptr;
    }
    return *this;
  }
  ~UserdataRef() { Release(); }

  static UserdataRef Borrowed(void* data) {
    UserdataRef ref;
    ref.raw_ = data;
    return ref;
  }

  static bool MakeOwned(void* data, lightusd_userdata_destroy_fn destroy,
                        UserdataRef* out) {
    if (!out) return false;
    if (!destroy) {
      *out = Borrowed(data);
      return true;
    }
    CallbackUserdata* holder = new (std::nothrow) CallbackUserdata();
    if (!holder) return false;
    holder->data = data;
    holder->destroy = destroy;
    out->Release();
    out->owned_ = holder;
    out->raw_ = nullptr;
    return true;
  }

  void* get() const { return owned_ ? owned_->data : raw_; }

 private:
  void CopyFrom(const UserdataRef& other) {
    owned_ = other.owned_;
    raw_ = other.raw_;
    if (owned_) owned_->refs.fetch_add(1, std::memory_order_relaxed);
  }
  void Release() {
    if (owned_ &&
        owned_->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      if (owned_->destroy) owned_->destroy(owned_->data);
      delete owned_;
    }
    owned_ = nullptr;
    raw_ = nullptr;
  }
  void Swap(UserdataRef& other) noexcept {
    std::swap(owned_, other.owned_);
    std::swap(raw_, other.raw_);
  }

  CallbackUserdata* owned_ = nullptr;
  void* raw_ = nullptr;
};

struct PackageCallbackEntry {
  lightusd_package_resolver_callback callback = nullptr;
  UserdataRef userdata;
};

lightusd_string* MakeOwnedString(const std::string& value) {
  lightusd_string* out = new (std::nothrow) lightusd_string();
  if (!out) return nullptr;
  out->s = value;
  return out;
}

}  // namespace

struct lightusd_asset_resolver {
  n::AssetResolver resolver;
  std::map<std::string, std::string> aliases;
  std::map<std::string, size_t> memory_asset_sizes;
  std::map<std::string, PackageCallbackEntry> package_callbacks;
  size_t memory_asset_bytes{0};
  size_t memory_asset_limit{0};
  mutable std::mutex memory_mutex;
  mutable std::mutex package_mutex;
  mutable std::mutex alias_mutex;

  lightusd_asset_resolver() {
    resolver.SetCustomResolver(
        [this](const std::string& authored, const std::string&) {
          std::lock_guard<std::mutex> lock(alias_mutex);
          const auto found = aliases.find(authored);
          return found == aliases.end() ? std::string() : found->second;
        });
  }
};

struct lightusd_resolver_context {
  n::ResolverContext context;
};

struct lightusd_resolver_cache_scope {
  lightusd_asset_resolver* resolver = nullptr;
  void* data = nullptr;
};

namespace lightusd_internal {
n::AssetResolver* AssetResolverForLoad(lightusd_asset_resolver* resolver) {
  return resolver ? &resolver->resolver : nullptr;
}
}  // namespace lightusd_internal

extern "C" {

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

lightusd_status lightusd_asset_resolver_set_filesystem_access(
    lightusd_asset_resolver* resolver, uint8_t enabled) {
  if (!resolver || enabled > 1)
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "invalid filesystem access setting");
#ifdef LIGHTUSD_NO_NATIVE_RESOLVER_IO
  if (enabled) return Fail(LIGHTUSD_ERR_UNSUPPORTED, "native filesystem resolver was not built");
#endif
  auto config = resolver->resolver.GetConfig();
  config.allow_filesystem = enabled != 0;
  resolver->resolver.SetConfig(config);
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

lightusd_status lightusd_asset_resolver_register_scheme_owned(
    lightusd_asset_resolver* resolver, const char* scheme,
    lightusd_scheme_resolver_callback resolver_callback,
    lightusd_scheme_reader_callback reader_callback, void* userdata,
    lightusd_userdata_destroy_fn destroy) {
  if (!resolver || !scheme || !scheme[0] ||
      (!resolver_callback && !reader_callback)) {
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "invalid scheme handler");
  }
  const std::string normalized = n::AssetResolver::NormalizeSchemeName(scheme);
  if (normalized.empty() ||
      n::AssetResolver::GetIdentifierScheme(normalized + ":asset") !=
          normalized) {
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "invalid scheme");
  }
  UserdataRef holder;
  if (!UserdataRef::MakeOwned(userdata, destroy, &holder)) {
    return Fail(LIGHTUSD_ERR_OUT_OF_MEMORY, "callback owner alloc failed");
  }
  n::ContextResolverCallback context_resolver;
  if (resolver_callback) {
    context_resolver = [resolver_callback, holder](
                           const std::string& identifier,
                           const std::string& anchor_path,
                           const std::string& scheme_context) {
      const char* resolved = nullptr;
      if (!resolver_callback(holder.get(), identifier.c_str(),
                             anchor_path.c_str(), scheme_context.c_str(),
                             &resolved) ||
          !resolved) {
        return std::string();
      }
      return std::string(resolved);
    };
  }
  n::AssetReadCallback reader;
  if (reader_callback) {
    reader = [reader_callback, holder](
                 const std::string& identifier, std::vector<uint8_t>* out,
                 std::string* err) {
      uint8_t* data = nullptr;
      size_t size = 0;
      if (!reader_callback(holder.get(), identifier.c_str(), &data, &size) ||
          (!data && size)) {
        if (err) *err += "scheme reader failed: " + identifier + "\n";
        std::free(data);
        return false;
      }
      if (size) {
        out->assign(data, data + size);
      } else {
        out->clear();
      }
      std::free(data);
      return true;
    };
  }
  resolver->resolver.RegisterScheme(scheme, std::move(context_resolver),
                                    std::move(reader));
  return LIGHTUSD_OK;
}

lightusd_status lightusd_asset_resolver_register_scheme(
    lightusd_asset_resolver* resolver, const char* scheme,
    lightusd_scheme_resolver_callback resolver_callback,
    lightusd_scheme_reader_callback reader_callback, void* userdata) {
  return lightusd_asset_resolver_register_scheme_owned(
      resolver, scheme, resolver_callback, reader_callback, userdata, nullptr);
}

lightusd_status lightusd_asset_resolver_unregister_scheme(
    lightusd_asset_resolver* resolver, const char* scheme) {
  if (!resolver || !scheme || !scheme[0]) {
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "invalid scheme");
  }
  if (!resolver->resolver.UnregisterScheme(scheme)) {
    return Fail(LIGHTUSD_ERR_NOT_FOUND, "scheme is not registered");
  }
  return LIGHTUSD_OK;
}

lightusd_status lightusd_asset_resolver_create_identifier(
    const lightusd_asset_resolver* resolver, const char* asset_path,
    const char* anchor_path, lightusd_string** out) {
  if (!resolver || !asset_path || !asset_path[0] || !out) {
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "invalid identifier arguments");
  }
  *out = nullptr;
  const std::string identifier = resolver->resolver.CreateIdentifier(
      asset_path, anchor_path ? anchor_path : "");
  lightusd_string* result = new (std::nothrow) lightusd_string();
  if (!result) {
    return Fail(LIGHTUSD_ERR_OUT_OF_MEMORY, "identifier alloc failed");
  }
  result->s = identifier;
  *out = result;
  return LIGHTUSD_OK;
}

int lightusd_asset_resolver_has_scheme(
    const lightusd_asset_resolver* resolver, const char* scheme) {
  if (!resolver || !scheme || !scheme[0]) return 0;
  return resolver->resolver.HasScheme(scheme) ? 1 : 0;
}

lightusd_status lightusd_asset_resolver_set_alias(
    lightusd_asset_resolver* resolver, const char* authored_path,
    const char* resolved_identifier) {
  if (!resolver || !authored_path || !authored_path[0] ||
      !resolved_identifier || !resolved_identifier[0]) {
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "invalid asset alias");
  }
  {
    std::lock_guard<std::mutex> lock(resolver->alias_mutex);
    resolver->aliases[authored_path] = resolved_identifier;
  }
  return LIGHTUSD_OK;
}

lightusd_status lightusd_asset_resolver_unset_alias(
    lightusd_asset_resolver* resolver, const char* authored_path) {
  if (!resolver || !authored_path || !authored_path[0]) {
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "invalid asset alias");
  }
  bool removed = false;
  {
    std::lock_guard<std::mutex> lock(resolver->alias_mutex);
    removed = resolver->aliases.erase(authored_path) != 0;
  }
  if (!removed) {
    return Fail(LIGHTUSD_ERR_NOT_FOUND, "alias was not registered");
  }
  return LIGHTUSD_OK;
}

lightusd_status lightusd_asset_resolver_get_asset_info(
    const lightusd_asset_resolver* resolver, const char* asset_path,
    const char* anchor_path, lightusd_asset_info* out) {
  if (!resolver || !asset_path || !asset_path[0] || !out) {
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "invalid asset info arguments");
  }
  out->resolved_path = nullptr;
  out->asset_name = nullptr;
  out->repository_path = nullptr;
  out->exists = 0;
  const n::ResolvedAsset resolved =
      resolver->resolver.Resolve(asset_path, anchor_path ? anchor_path : "");
  const n::AssetInfo info =
      resolver->resolver.GetAssetInfo(asset_path, resolved);
  out->resolved_path = MakeOwnedString(info.resolved_path);
  out->asset_name = MakeOwnedString(info.asset_name);
  out->repository_path = MakeOwnedString(info.repository_path);
  if (!out->resolved_path || !out->asset_name || !out->repository_path) {
    lightusd_asset_info_destroy(out);
    return Fail(LIGHTUSD_ERR_OUT_OF_MEMORY, "asset info alloc failed");
  }
  out->exists = info.exists ? 1 : 0;
  return LIGHTUSD_OK;
}

void lightusd_asset_info_destroy(lightusd_asset_info* info) {
  if (!info) return;
  lightusd_string_destroy(info->resolved_path);
  lightusd_string_destroy(info->asset_name);
  lightusd_string_destroy(info->repository_path);
  info->resolved_path = nullptr;
  info->asset_name = nullptr;
  info->repository_path = nullptr;
  info->exists = 0;
}

lightusd_status lightusd_asset_resolver_get_modification_timestamp(
    const lightusd_asset_resolver* resolver, const char* asset_path,
    const char* anchor_path, uint64_t* seconds, uint32_t* nanoseconds,
    uint8_t* valid) {
  if (!resolver || !asset_path || !asset_path[0] || !seconds || !nanoseconds ||
      !valid) {
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "invalid timestamp arguments");
  }
  *seconds = 0;
  *nanoseconds = 0;
  *valid = 0;
  const n::ResolvedAsset resolved =
      resolver->resolver.Resolve(asset_path, anchor_path ? anchor_path : "");
  const n::AssetTimestamp timestamp =
      resolver->resolver.GetModificationTimestamp(asset_path, resolved);
  *seconds = timestamp.seconds;
  *nanoseconds = static_cast<uint32_t>(timestamp.nanoseconds);
  *valid = timestamp.valid ? 1 : 0;
  return LIGHTUSD_OK;
}

lightusd_status lightusd_asset_resolver_read(
    const lightusd_asset_resolver* resolver, const char* resolved_identifier,
    lightusd_string** out) {
  if (!resolver || !resolved_identifier || !out) {
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "invalid asset read arguments");
  }
  *out = nullptr;
  std::string error;
  // The identifier is already resolved; only a package entry still needs to be
  // split so the package file's bytes are returned.
  std::string read_path = resolved_identifier;
  std::string package_file;
  std::string package_entry;
  if (n::AssetResolver::IsPackagePath(read_path) &&
      n::AssetResolver::ParsePackagePath(read_path, &package_file,
                                          &package_entry)) {
    read_path = package_file;
  }
  // Read straight into the string: a large asset is not copied twice.
  std::string bytes;
  if (!resolver->resolver.ReadAssetToString(read_path, &bytes, &error)) {
    return Fail(LIGHTUSD_ERR_IO, error);
  }
  lightusd_string* result = new (std::nothrow) lightusd_string();
  if (!result) return Fail(LIGHTUSD_ERR_OUT_OF_MEMORY, "asset alloc failed");
  result->s = std::move(bytes);
  *out = result;
  return LIGHTUSD_OK;
}

lightusd_status lightusd_asset_resolver_resolve(
    const lightusd_asset_resolver* resolver, const char* asset_path,
    const char* anchor_path, lightusd_string** resolved_path, uint8_t* exists,
    uint8_t* is_package) {
  if (!resolver || !asset_path || !resolved_path) {
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "invalid resolution arguments");
  }
  *resolved_path = nullptr;
  const n::ResolvedAsset resolved =
      resolver->resolver.Resolve(asset_path, anchor_path ? anchor_path : "");
  lightusd_string* result = new (std::nothrow) lightusd_string();
  if (!result) return Fail(LIGHTUSD_ERR_OUT_OF_MEMORY, "resolution alloc failed");
  result->s = resolved.resolved_path;
  *resolved_path = result;
  if (exists) *exists = resolved.exists ? 1 : 0;
  if (is_package) *is_package = resolved.is_package ? 1 : 0;
  return LIGHTUSD_OK;
}

lightusd_status lightusd_asset_resolver_resolve_for_new_asset(
    const lightusd_asset_resolver* resolver, const char* asset_path,
    const char* anchor_path, lightusd_string** resolved_path, uint8_t* exists,
    uint8_t* is_package) {
  if (!resolver || !asset_path || !resolved_path) {
    return Fail(LIGHTUSD_ERR_INVALID_ARG,
                "invalid new-asset resolution arguments");
  }
  *resolved_path = nullptr;
  const n::ResolvedAsset resolved = resolver->resolver.ResolveForNewAsset(
      asset_path, anchor_path ? anchor_path : "");
  lightusd_string* result = new (std::nothrow) lightusd_string();
  if (!result) return Fail(LIGHTUSD_ERR_OUT_OF_MEMORY, "resolution alloc failed");
  result->s = resolved.resolved_path;
  *resolved_path = result;
  if (exists) *exists = resolved.exists ? 1 : 0;
  if (is_package) *is_package = resolved.is_package ? 1 : 0;
  return LIGHTUSD_OK;
}

lightusd_status lightusd_asset_resolver_create_context(
    const lightusd_asset_resolver* resolver, const char* context_string,
    lightusd_resolver_context** out) {
  if (!resolver || !out) {
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "invalid context arguments");
  }
  *out = nullptr;
  auto* context = new (std::nothrow) lightusd_resolver_context();
  if (!context) return Fail(LIGHTUSD_ERR_OUT_OF_MEMORY, "context alloc failed");
  context->context =
      resolver->resolver.CreateContextFromString(context_string
                                                     ? context_string
                                                     : "");
  *out = context;
  return LIGHTUSD_OK;
}

void lightusd_resolver_context_destroy(lightusd_resolver_context* context) {
  delete context;
}

lightusd_sv lightusd_resolver_context_string(
    const lightusd_resolver_context* context) {
  if (!context) return lightusd_internal::EmptySV();
  return lightusd_internal::SV(context->context.context_string);
}

lightusd_status lightusd_resolver_context_set_scheme(
    lightusd_resolver_context* context, const char* scheme,
    const char* scheme_context) {
  if (!context || !scheme || !scheme[0]) {
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "invalid scheme context");
  }
  context->context.SetSchemeContext(scheme,
                                    scheme_context ? scheme_context : "");
  return LIGHTUSD_OK;
}

lightusd_sv lightusd_resolver_context_get_scheme(
    const lightusd_resolver_context* context, const char* scheme) {
  if (!context || !scheme) return lightusd_internal::EmptySV();
  const std::string* value = context->context.FindSchemeContext(scheme);
  if (!value || value->empty()) return lightusd_internal::EmptySV();
  return lightusd_internal::SV(*value);
}

lightusd_status lightusd_asset_resolver_bind_context(
    lightusd_asset_resolver* resolver, const lightusd_resolver_context* context,
    void** binding_data) {
  if (!resolver) return Fail(LIGHTUSD_ERR_INVALID_ARG, "resolver is null");
  const n::ResolverContext default_context;
  resolver->resolver.BindContext(context ? context->context : default_context,
                                 binding_data);
  return LIGHTUSD_OK;
}

lightusd_status lightusd_asset_resolver_unbind_context(
    lightusd_asset_resolver* resolver, const lightusd_resolver_context* context,
    void** binding_data) {
  if (!resolver) return Fail(LIGHTUSD_ERR_INVALID_ARG, "resolver is null");
  const n::ResolverContext default_context;
  if (!resolver->resolver.UnbindContext(
          context ? context->context : default_context, binding_data)) {
    return Fail(LIGHTUSD_ERR_NOT_FOUND,
                "no matching context binding on this thread");
  }
  return LIGHTUSD_OK;
}

lightusd_status lightusd_asset_resolver_get_context(
    const lightusd_asset_resolver* resolver, lightusd_resolver_context** out) {
  if (!resolver || !out) {
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "invalid context arguments");
  }
  *out = nullptr;
  auto* context = new (std::nothrow) lightusd_resolver_context();
  if (!context) return Fail(LIGHTUSD_ERR_OUT_OF_MEMORY, "context alloc failed");
  context->context = resolver->resolver.GetCurrentContext();
  *out = context;
  return LIGHTUSD_OK;
}

int lightusd_asset_resolver_is_context_dependent(
    const lightusd_asset_resolver* resolver, const char* asset_path) {
  if (!resolver || !asset_path) return 0;
  return resolver->resolver.IsContextDependentPath(asset_path) ? 1 : 0;
}

lightusd_status lightusd_asset_resolver_create_cache_scope(
    lightusd_asset_resolver* resolver, lightusd_resolver_cache_scope** out) {
  if (!resolver || !out) {
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "invalid cache scope arguments");
  }
  *out = nullptr;
  auto* scope = new (std::nothrow) lightusd_resolver_cache_scope();
  if (!scope) return Fail(LIGHTUSD_ERR_OUT_OF_MEMORY, "cache alloc failed");
  scope->resolver = resolver;
  resolver->resolver.BeginCacheScope(&scope->data);
  *out = scope;
  return LIGHTUSD_OK;
}

void lightusd_resolver_cache_scope_destroy(
    lightusd_resolver_cache_scope* scope) {
  if (!scope) return;
  if (scope->resolver && scope->data) {
    scope->resolver->resolver.EndCacheScope(&scope->data);
  }
  delete scope;
}

lightusd_status lightusd_asset_resolver_register_package_owned(
    lightusd_asset_resolver* resolver, const char* extension,
    lightusd_package_resolver_callback callback, void* userdata,
    lightusd_userdata_destroy_fn destroy) {
  if (!resolver || !callback) {
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "invalid package resolver");
  }
  const std::string normalized =
      n::AssetResolver::NormalizePackageExtension(extension ? extension : "");
  if (normalized.empty()) {
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "invalid package extension");
  }
  UserdataRef holder;
  if (!UserdataRef::MakeOwned(userdata, destroy, &holder)) {
    return Fail(LIGHTUSD_ERR_OUT_OF_MEMORY, "callback owner alloc failed");
  }
  PackageCallbackEntry entry;
  entry.callback = callback;
  entry.userdata = std::move(holder);
  PackageCallbackEntry retired;
  {
    std::lock_guard<std::mutex> lock(resolver->package_mutex);
    auto& slot = resolver->package_callbacks[normalized];
    retired = std::move(slot);
    slot = std::move(entry);
  }
  (void)retired;
  lightusd_asset_resolver* self = resolver;
  resolver->resolver.RegisterPackageResolver(
      normalized,
      [self, normalized](const std::string& package_path,
                         const std::string& asset_path,
                         std::string* resolved) {
        PackageCallbackEntry entry;
        {
          std::lock_guard<std::mutex> lock(self->package_mutex);
          const auto found = self->package_callbacks.find(normalized);
          if (found == self->package_callbacks.end()) return false;
          entry = found->second;
        }
        if (!entry.callback) return false;
        const char* text = nullptr;
        if (!entry.callback(entry.userdata.get(), package_path.c_str(),
                            asset_path.c_str(), &text) ||
            !text) {
          return false;
        }
        *resolved = text;
        return true;
      });
  return LIGHTUSD_OK;
}

lightusd_status lightusd_asset_resolver_register_package(
    lightusd_asset_resolver* resolver, const char* extension,
    lightusd_package_resolver_callback callback, void* userdata) {
  return lightusd_asset_resolver_register_package_owned(
      resolver, extension, callback, userdata, nullptr);
}

lightusd_status lightusd_asset_resolver_unregister_package(
    lightusd_asset_resolver* resolver, const char* extension) {
  if (!resolver) return Fail(LIGHTUSD_ERR_INVALID_ARG, "resolver is null");
  const std::string normalized =
      n::AssetResolver::NormalizePackageExtension(extension ? extension : "");
  if (normalized.empty()) {
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "invalid package extension");
  }
  PackageCallbackEntry retired;
  {
    std::lock_guard<std::mutex> lock(resolver->package_mutex);
    const auto it = resolver->package_callbacks.find(normalized);
    if (it != resolver->package_callbacks.end()) {
      retired = it->second;
      resolver->package_callbacks.erase(it);
    }
  }
  (void)retired;
  if (!resolver->resolver.UnregisterPackageResolver(normalized)) {
    return Fail(LIGHTUSD_ERR_NOT_FOUND, "package resolver was not registered");
  }
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
