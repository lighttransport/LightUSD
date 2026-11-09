// SPDX-License-Identifier: Apache-2.0
// Common string-intern table core shared by TypeNameTable and PropNameTable.
// Both intern names into a deque-backed StringIndex under a rwlock; this holds
// that machinery so each table only adds its ID type, capacity, and (for
// PropNameTable) the lock-free freeze/snapshot layer. IdT must expose a `id`
// integer field and `is_valid()`; MaxN is the capacity / invalid sentinel.
#pragma once
#include <deque>
#include <string>
#include <string_view>
#if defined(LIGHTUSD_ENABLE_THREAD)
#include <mutex>
#include <shared_mutex>
#endif

#include "../core/string-index.hh"

namespace lightusd {
namespace next {
namespace detail {

template <typename IdT, size_t MaxN>
class StringInternTable {
 public:
  size_t size() const { return names_.size(); }

 protected:
  // Key extractor for StringIndex: yields names[index] as (data, length).
  static const char* KeyFn(const void* context, size_t index, size_t* length) {
    const auto& names = *static_cast<const std::deque<std::string>*>(context);
    if (index >= names.size()) return nullptr;
    *length = names[index].size();
    return names[index].data();
  }

  static IdT make_id(size_t id) {
    IdT r{};
    r.id = static_cast<decltype(r.id)>(id);
    return r;
  }

  // Lookup without inserting. Uses the optional index, falling back to a linear
  // scan when the index lagged an allocation failure (an unindexed name must not
  // get a second ID on a later lookup).
  IdT find_live(std::string_view name) const {
    const size_t id = name_to_id_.find(name.empty() ? "" : name.data(),
                                       name.size(), KeyFn, &names_);
    if (id < MaxN) return make_id(id);
    if (name_to_id_.size() != names_.size()) {
      for (size_t i = 0; i < names_.size(); ++i) {
        if (names_[i] == name) return make_id(i);
      }
    }
    return IdT{};
  }

  // Intern under the rwlock (shared fast-path, exclusive insert). Callers with
  // a lock-free snapshot fast-path (PropNameTable) call this after it misses.
  // on_inserted(ctx) runs under the exclusive lock right after a fresh insert
  // (used by PropNameTable to refresh its lock-free snapshot).
  IdT intern_locked(const std::string& name,
                    void (*on_inserted)(void*) = nullptr, void* ctx = nullptr) {
#if defined(LIGHTUSD_ENABLE_THREAD)
    {
      std::shared_lock<std::shared_mutex> rlk(mu_);
      const IdT hit = find_live(name);
      if (hit.is_valid()) return hit;
    }
    std::unique_lock<std::shared_mutex> wlk(mu_);
#endif
    const IdT hit = find_live(name);
    if (hit.is_valid()) return hit;
    if (names_.size() >= MaxN) return IdT{};  // table full
    const size_t id = names_.size();
    names_.push_back(name);
    if (name_to_id_.size() + 1 == names_.size()) {
      name_to_id_.insert(id, KeyFn, &names_);
    } else {
      name_to_id_.rebuild(names_.size(), KeyFn, &names_);
    }
    if (on_inserted) on_inserted(ctx);
    return make_id(id);
  }

  // deque, not vector: get() may return a reference held after the lock is
  // released; deque never relocates on push_back.
  std::deque<std::string> names_;
  detail::StringIndex name_to_id_;
#if defined(LIGHTUSD_ENABLE_THREAD)
  mutable std::shared_mutex mu_;
#endif
};

}  // namespace detail
}  // namespace next
}  // namespace lightusd
