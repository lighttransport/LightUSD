// SPDX-License-Identifier: MIT
// Copyright 2026 - Present : Syoyo Fujita
//
// In-tree open-addressing robin-hood hash map.
//
// Goals: header-only, exceptions-free, RTTI-free, drop-in for the lookup-only
// `std::unordered_map<K,V>` sites in lightusd. Iteration order is
// unspecified.
//
// Algorithm: open addressing with linear probing and robin-hood reordering
// (on insert, swap with the encountered slot if its probe distance is lower).
// Erase uses backward-shift to keep the table tombstone-free, so iteration
// after a sequence of erases stays dense.
//
// Hash mixing: we run the user-supplied hash through a splitmix64-style mixer
// before masking down to the bucket count. This protects the linear-probe
// table from weak hashes (e.g. identity hash for small ints), which would
// otherwise pile up at low buckets and inflate probe distances.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <utility>
#include <vector>
#include <type_traits>

#include "compiler-features.hh"

namespace lightusd {

template <typename Key, typename Value, typename Hash = std::hash<Key>,
          typename KeyEqual = std::equal_to<Key>>
class HashMap {
 public:
  using key_type = Key;
  using mapped_type = Value;
  using value_type = std::pair<Key, Value>;
  using size_type = std::size_t;
  using hasher = Hash;
  using key_equal = KeyEqual;

 private:
  // dist == 0 => empty; dist >= 1 => occupied with probe distance (dist - 1).
  // Note: kv stores Key (not const Key) so that probing can move/swap entries
  // during robin-hood reordering. Iterators expose value_type as
  // `std::pair<Key, Value>` rather than the standard `pair<const Key, Value>`;
  // callers must not mutate `it->first`.
  struct Bucket {
    uint32_t dist;
    std::pair<Key, Value> kv;

    Bucket() : dist(0), kv() {}
  };

  struct InsertResult {
    size_type index;
    bool inserted;
    bool ok;
  };

  static uint64_t mix64(uint64_t x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
  }

  size_type _size;
  size_type _mask;       // bucket_count - 1, or 0 when no buckets allocated
  float _max_load;
  std::vector<Bucket> _buckets;
  Hash _hash;
  KeyEqual _eq;

  size_type bucket_count_internal() const {
    return _buckets.size();
  }

  size_type ideal_index(const Key &k) const {
    uint64_t h = static_cast<uint64_t>(_hash(k));
    return static_cast<size_type>(mix64(h)) & _mask;
  }

  static size_type next_pow2(size_type n) {
    if (n < 2) return 2;
    size_type p = 1;
    while (p < n) p <<= 1;
    return p;
  }

  void grow_if_needed() {
    if (_buckets.empty()) {
      rehash_to(8);
      return;
    }
    // load factor check: size+1 > cap * max_load
    if (static_cast<float>(_size + 1) >
        static_cast<float>(_buckets.size()) * _max_load) {
      rehash_to(_buckets.size() * 2);
    }
  }

  void rehash_to(size_type new_cap) {
    new_cap = next_pow2(new_cap);
    std::vector<Bucket> old_buckets;
    old_buckets.swap(_buckets);
    _buckets = std::vector<Bucket>(new_cap);
    _mask = new_cap - 1;
    _size = 0;
    for (auto &b : old_buckets) {
      if (b.dist != 0) {
        insert_into_table(std::move(b.kv.first), std::move(b.kv.second));
      }
    }
  }

  // Core insertion using robin-hood displacement. Returns the bucket index
  // where the (eventual) entry with `key` resides, plus whether a fresh
  // insertion happened.
  // Pass by value so callers may pass either lvalues, rvalues, or
  // function-style cast prvalues like `Key(k)` — MSVC has trouble binding
  // those to `Key&&` parameters, so by-value sidesteps the issue.
  InsertResult insert_into_table(Key k, Value v) {
    if (_buckets.empty() || _size >= _buckets.size()) {
      return {0, false, false};
    }
    size_type idx = ideal_index(k);
    uint32_t dist = 1;
    Key cur_key = std::move(k);
    Value cur_val = std::move(v);
    size_type first_seen = static_cast<size_type>(-1);
    bool inserted_new = true;
    while (true) {
      Bucket &slot = _buckets[idx];
      if (slot.dist == 0) {
        slot.kv.first = std::move(cur_key);
        slot.kv.second = std::move(cur_val);
        slot.dist = dist;
        ++_size;
        if (first_seen == static_cast<size_type>(-1)) first_seen = idx;
        return {first_seen, inserted_new, true};
      }
      if (slot.dist == dist && _eq(slot.kv.first, cur_key)) {
        // Key already present — do not overwrite (std::unordered_map::insert
        // semantics). Caller can mutate via the returned reference if needed.
        return {idx, false, true};
      }
      if (slot.dist < dist) {
        // Robin-hood: rob from the rich.
        if (first_seen == static_cast<size_type>(-1)) first_seen = idx;
        std::swap(slot.kv.first, cur_key);
        std::swap(slot.kv.second, cur_val);
        std::swap(slot.dist, dist);
      }
      idx = (idx + 1) & _mask;
      ++dist;
      // Probe-distance bound: cannot exceed table capacity.
      if (dist > _buckets.size()) {
        // Table is full — should not happen if callers resize properly.
        // Report failure explicitly so callers never index a sentinel bucket.
        return {0, false, false};
      }
    }
  }

  size_type find_index(const Key &k) const {
    if (_buckets.empty() || _size == 0) {
      return static_cast<size_type>(-1);
    }
    size_type idx = ideal_index(k);
    uint32_t dist = 1;
    while (true) {
      const Bucket &slot = _buckets[idx];
      if (slot.dist == 0) return static_cast<size_type>(-1);
      if (slot.dist < dist) return static_cast<size_type>(-1);
      if (_eq(slot.kv.first, k)) return idx;
      idx = (idx + 1) & _mask;
      ++dist;
      if (dist > _buckets.size()) return static_cast<size_type>(-1);
    }
  }

 public:
  // Forward iterator over occupied buckets.
  class iterator {
    friend class HashMap;
    HashMap *_m;
    size_type _i;
    void advance_to_occupied() {
      while (_m && _i < _m->_buckets.size() && _m->_buckets[_i].dist == 0) {
        ++_i;
      }
    }

   public:
    using difference_type = std::ptrdiff_t;
    using value_type = std::pair<Key, Value>;
    using reference = value_type &;
    using pointer = value_type *;
    using iterator_category = std::forward_iterator_tag;

    iterator() : _m(nullptr), _i(0) {}
    iterator(HashMap *m, size_type i) : _m(m), _i(i) { advance_to_occupied(); }
    reference operator*() const LIGHTUSD_LIFETIMEBOUND {
      return _m->_buckets[_i].kv;
    }
    pointer operator->() const LIGHTUSD_LIFETIMEBOUND {
      return &_m->_buckets[_i].kv;
    }
    iterator &operator++() LIGHTUSD_LIFETIMEBOUND {
      ++_i;
      advance_to_occupied();
      return *this;
    }
    iterator operator++(int) {
      iterator t = *this;
      ++(*this);
      return t;
    }
    bool operator==(const iterator &o) const {
      return _m == o._m && _i == o._i;
    }
    bool operator!=(const iterator &o) const { return !(*this == o); }

    const Key &key() const LIGHTUSD_LIFETIMEBOUND {
      return _m->_buckets[_i].kv.first;
    }
    Value &mapped() const LIGHTUSD_LIFETIMEBOUND {
      return _m->_buckets[_i].kv.second;
    }
  };

  class const_iterator {
    friend class HashMap;
    const HashMap *_m;
    size_type _i;
    void advance_to_occupied() {
      while (_m && _i < _m->_buckets.size() && _m->_buckets[_i].dist == 0) {
        ++_i;
      }
    }

   public:
    using difference_type = std::ptrdiff_t;
    using value_type = std::pair<Key, Value>;
    using reference = const value_type &;
    using pointer = const value_type *;
    using iterator_category = std::forward_iterator_tag;

    const_iterator() : _m(nullptr), _i(0) {}
    const_iterator(const HashMap *m, size_type i) : _m(m), _i(i) {
      advance_to_occupied();
    }
    reference operator*() const LIGHTUSD_LIFETIMEBOUND {
      return _m->_buckets[_i].kv;
    }
    pointer operator->() const LIGHTUSD_LIFETIMEBOUND {
      return &_m->_buckets[_i].kv;
    }
    const_iterator &operator++() LIGHTUSD_LIFETIMEBOUND {
      ++_i;
      advance_to_occupied();
      return *this;
    }
    const_iterator operator++(int) {
      const_iterator t = *this;
      ++(*this);
      return t;
    }
    bool operator==(const const_iterator &o) const {
      return _m == o._m && _i == o._i;
    }
    bool operator!=(const const_iterator &o) const { return !(*this == o); }

    const Key &key() const LIGHTUSD_LIFETIMEBOUND {
      return _m->_buckets[_i].kv.first;
    }
    const Value &mapped() const LIGHTUSD_LIFETIMEBOUND {
      return _m->_buckets[_i].kv.second;
    }
  };

  HashMap() : _size(0), _mask(0), _max_load(0.5f), _buckets(), _hash(), _eq() {}

  explicit HashMap(size_type initial_bucket_count)
      : _size(0), _mask(0), _max_load(0.5f), _buckets(), _hash(), _eq() {
    if (initial_bucket_count > 0) {
      rehash_to(initial_bucket_count);
    }
  }

  HashMap(std::initializer_list<value_type> init)
      : _size(0), _mask(0), _max_load(0.5f), _buckets(), _hash(), _eq() {
    reserve(init.size());
    for (const auto &kv : init) {
      emplace(kv.first, kv.second);
    }
  }

  template <typename InputIt>
  HashMap(InputIt first, InputIt last)
      : _size(0), _mask(0), _max_load(0.5f), _buckets(), _hash(), _eq() {
    for (; first != last; ++first) {
      emplace(first->first, first->second);
    }
  }

  HashMap(const HashMap &) = default;
  HashMap(HashMap &&) noexcept = default;
  HashMap &operator=(const HashMap &) = default;
  HashMap &operator=(HashMap &&) noexcept = default;
  ~HashMap() = default;

  size_type size() const { return _size; }
  bool empty() const { return _size == 0; }
  size_type bucket_count() const { return _buckets.size(); }
  float load_factor() const {
    return _buckets.empty()
               ? 0.0f
               : static_cast<float>(_size) / static_cast<float>(_buckets.size());
  }
  float max_load_factor() const { return _max_load; }
  void max_load_factor(float f) {
    if (f <= 0.0f) f = 0.5f;
    if (f >= 1.0f) f = 0.95f;
    _max_load = f;
    if (!_buckets.empty() &&
        static_cast<float>(_size) >
            static_cast<float>(_buckets.size()) * _max_load) {
      rehash_to(_buckets.size() * 2);
    }
  }

  void clear() {
    for (auto &b : _buckets) {
      if (b.dist != 0) {
        b.kv.first = Key();
        b.kv.second = Value();
        b.dist = 0;
      }
    }
    _size = 0;
  }

  void rehash(size_type new_bucket_count) {
    size_type need = static_cast<size_type>(
        static_cast<float>(_size) / _max_load + 1.0f);
    if (new_bucket_count < need) new_bucket_count = need;
    rehash_to(new_bucket_count);
  }

  void reserve(size_type n) {
    size_type need = static_cast<size_type>(
        static_cast<float>(n) / _max_load + 1.0f);
    if (need > _buckets.size()) {
      rehash_to(need);
    }
  }

  std::pair<iterator, bool> insert(const value_type &kv) {
    grow_if_needed();
    Key k = kv.first;
    Value v = kv.second;
    auto r = insert_into_table(std::move(k), std::move(v));
    if (!r.ok) return {end(), false};
    return {iterator(this, r.index), r.inserted};
  }

  std::pair<iterator, bool> insert(value_type &&kv) {
    grow_if_needed();
    auto r = insert_into_table(std::move(kv.first), std::move(kv.second));
    if (!r.ok) return {end(), false};
    return {iterator(this, r.index), r.inserted};
  }

  template <typename K2, typename V2>
  std::pair<iterator, bool> emplace(K2 &&k, V2 &&v) {
    grow_if_needed();
    auto r = insert_into_table(Key(std::forward<K2>(k)),
                               Value(std::forward<V2>(v)));
    if (!r.ok) return {end(), false};
    return {iterator(this, r.index), r.inserted};
  }

  // C++17 insert_or_assign: overwrite the existing value on duplicate key.
  template <typename K2, typename V2>
  std::pair<iterator, bool> insert_or_assign(K2 &&k, V2 &&v) {
    grow_if_needed();
    auto r = insert_into_table(Key(std::forward<K2>(k)), Value());
    if (!r.ok) return {end(), false};
    _buckets[r.index].kv.second = Value(std::forward<V2>(v));
    return {iterator(this, r.index), r.inserted};
  }

  Value &operator[](const Key &k) LIGHTUSD_LIFETIMEBOUND {
    grow_if_needed();
    auto r = insert_into_table(Key(k), Value());
    if (!r.ok) {
      static thread_local Value sink{};
      sink = Value();
      return sink;
    }
    return _buckets[r.index].kv.second;
  }
  Value &operator[](Key &&k) LIGHTUSD_LIFETIMEBOUND {
    grow_if_needed();
    auto r = insert_into_table(std::move(k), Value());
    if (!r.ok) {
      static thread_local Value sink{};
      sink = Value();
      return sink;
    }
    return _buckets[r.index].kv.second;
  }

  // Hardened: on miss, return a static fallback without aborting.
  // Callers are expected to check `find`/`contains` first; this is a safety
  // net for unexpected miss cases.
  Value &at(const Key &k) LIGHTUSD_LIFETIMEBOUND {
    size_type i = find_index(k);
    if (i == static_cast<size_type>(-1)) {
      static thread_local Value sink{};
      sink = Value();
      return sink;
    }
    return _buckets[i].kv.second;
  }
  const Value &at(const Key &k) const LIGHTUSD_LIFETIMEBOUND {
    size_type i = find_index(k);
    if (i == static_cast<size_type>(-1)) {
      static const Value sink{};
      return sink;
    }
    return _buckets[i].kv.second;
  }

  Value *find_value_ptr(const Key &k) LIGHTUSD_LIFETIMEBOUND {
    size_type i = find_index(k);
    if (i == static_cast<size_type>(-1)) return nullptr;
    return &_buckets[i].kv.second;
  }

  const Value *find_value_ptr(const Key &k) const LIGHTUSD_LIFETIMEBOUND {
    size_type i = find_index(k);
    if (i == static_cast<size_type>(-1)) return nullptr;
    return &_buckets[i].kv.second;
  }

  template <typename Predicate>
  const Value *find_value_if(Predicate &&pred) const LIGHTUSD_LIFETIMEBOUND {
    for (const Bucket &bucket : _buckets) {
      if (bucket.dist != 0 && pred(bucket.kv.first, bucket.kv.second)) {
        return &bucket.kv.second;
      }
    }
    return nullptr;
  }

  iterator find(const Key &k) {
    size_type i = find_index(k);
    if (i == static_cast<size_type>(-1)) return end();
    iterator it;
    it._m = this;
    it._i = i;
    return it;
  }
  const_iterator find(const Key &k) const {
    size_type i = find_index(k);
    if (i == static_cast<size_type>(-1)) return end();
    const_iterator it;
    it._m = this;
    it._i = i;
    return it;
  }

  size_type count(const Key &k) const {
    return find_index(k) == static_cast<size_type>(-1) ? 0 : 1;
  }
  bool contains(const Key &k) const {
    return find_index(k) != static_cast<size_type>(-1);
  }

  size_type erase(const Key &k) {
    size_type i = find_index(k);
    if (i == static_cast<size_type>(-1)) return 0;
    erase_at(i);
    return 1;
  }

  iterator erase(iterator it) {
    if (it._m != this || it._i >= _buckets.size()) return end();
    size_type i = it._i;
    erase_at(i);
    return iterator(this, i);
  }

  void swap(HashMap &o) noexcept {
    using std::swap;
    swap(_size, o._size);
    swap(_mask, o._mask);
    swap(_max_load, o._max_load);
    _buckets.swap(o._buckets);
    swap(_hash, o._hash);
    swap(_eq, o._eq);
  }

  iterator begin() { return iterator(this, 0); }
  iterator end() {
    iterator it;
    it._m = this;
    it._i = _buckets.size();
    return it;
  }
  const_iterator begin() const { return const_iterator(this, 0); }
  const_iterator end() const {
    const_iterator it;
    it._m = this;
    it._i = _buckets.size();
    return it;
  }
  const_iterator cbegin() const { return begin(); }
  const_iterator cend() const { return end(); }

 private:
  void erase_at(size_type i) {
    // Backward-shift deletion: walk forward, shifting any neighbour whose
    // probe distance > 1 back one slot, decreasing its dist by 1.
    _buckets[i].dist = 0;
    _buckets[i].kv.first = Key();
    _buckets[i].kv.second = Value();
    --_size;
    size_type prev = i;
    size_type next = (i + 1) & _mask;
    while (_buckets[next].dist > 1) {
      _buckets[prev].kv.first = std::move(_buckets[next].kv.first);
      _buckets[prev].kv.second = std::move(_buckets[next].kv.second);
      _buckets[prev].dist = _buckets[next].dist - 1;
      _buckets[next].dist = 0;
      _buckets[next].kv.first = Key();
      _buckets[next].kv.second = Value();
      prev = next;
      next = (next + 1) & _mask;
    }
  }
};

template <typename K, typename V, typename H, typename E>
inline void swap(HashMap<K, V, H, E> &a, HashMap<K, V, H, E> &b) noexcept {
  a.swap(b);
}

}  // namespace lightusd
