// SPDX-License-Identifier: Apache 2.0
// Copyright 2026 - Present, Light Transport Entertainment Inc.
//
// pprint-buf.hh - reusable "format into a string" streams for the printer.
//
// The printer used to build a std::stringstream in (almost) every printing
// function -- 155 of them. Constructing one costs ~4k instructions of pure
// setup (`ios_base::_M_init`, `std::locale` copy, `basic_ios::init`,
// stringbuf allocation) before it formats a single character. On a 4000-mesh
// layer, where a prim's print fans out through a dozen such functions, that
// setup was ~15% of the whole print pass -- and the same applies to the
// per-element ostringstream the array printer used to build.
//
// A `Scratch` owns one stream that formats straight into its std::string, so the
// same `operator<<` calls only pay for formatting. `ScratchRef` borrows one from
// a per-thread pool for the duration of a function:
//
//     std::string print_thing(...) {
//       pprint::ScratchRef buf;
//       std::ostream &ss = buf.os();
//       ss << ...;
//       return buf.str();       // copy out; the buffer is reused next time
//     }
//
// The pool is what makes this safe for the printer's mutually recursive calls:
// a nested call borrows a *different* buffer instead of clobbering the caller's
// half-written one. Depth is bounded by the printing recursion, and each buffer
// keeps its capacity, so steady-state printing does not allocate.
//
// Scope: this formats text. It never touches a file descriptor, and its format
// state is the default one a fresh ostringstream would have (the printer uses no
// width/fill/base manipulators). The buffer is reused -- copy out what you need
// before the next call.
//
#pragma once

#include <cstddef>
#include <memory>
#include <ostream>
#include <streambuf>
#include <string>
#include <vector>

namespace lightusd {
namespace pprint {

/// A std::streambuf that appends everything written to it to a std::string.
/// xsputn/overflow are the only two entry points the stream machinery uses, so
/// no put-area bookkeeping is needed: every write lands in the string directly.
class StringSink : public std::streambuf {
 public:
  std::string &str() { return s_; }

  void clear() { s_.clear(); }

 protected:
  std::streamsize xsputn(const char *p, std::streamsize n) override {
    s_.append(p, static_cast<size_t>(n));
    return n;
  }

  int_type overflow(int_type c) override {
    if (!traits_type::eq_int_type(c, traits_type::eof())) {
      s_.push_back(static_cast<char>(c));
    }
    return traits_type::not_eof(c);
  }

 private:
  std::string s_;
};

/// One reusable text buffer + stream. Not thread-safe by itself: each thread
/// owns a pool of them and borrows through `ScratchRef`.
class Scratch {
 public:
  Scratch() : os_(&sink_) {}

  Scratch(const Scratch &) = delete;
  Scratch &operator=(const Scratch &) = delete;

  std::ostream &os() { return os_; }
  std::string &str() { return sink_.str(); }

  /// Empty the buffer, keeping its capacity. Also clears sticky stream state so
  /// one failed insertion cannot silently poison every later use (a fresh
  /// ostringstream would start clean each time).
  void reset() {
    sink_.clear();
    os_.clear();
  }

 private:
  StringSink sink_;
  std::ostream os_;
};

namespace detail {

/// Per-thread scratch pool: created on first use, intentionally leaked so it
/// stays out of static-destruction order (and keeps its buffers for the life of
/// the thread).
struct ScratchPool {
  std::vector<std::unique_ptr<Scratch>> owned;
  std::vector<Scratch *> free_list;

  Scratch *acquire() {
    if (!free_list.empty()) {
      Scratch *s = free_list.back();
      free_list.pop_back();
      s->reset();
      return s;
    }
    owned.push_back(std::make_unique<Scratch>());
    Scratch *s = owned.back().get();
    s->reset();
    return s;
  }

  void release(Scratch *s) { free_list.push_back(s); }
};

inline ScratchPool &scratch_pool() {
  static thread_local ScratchPool *pool = new ScratchPool();
  return *pool;
}

}  // namespace detail

/// Borrowed scratch buffer. Releases it back to the pool on scope exit, so early
/// returns are safe.
class ScratchRef {
 public:
  ScratchRef() : scratch_(detail::scratch_pool().acquire()) {}

  ~ScratchRef() { detail::scratch_pool().release(scratch_); }

  ScratchRef(const ScratchRef &) = delete;
  ScratchRef &operator=(const ScratchRef &) = delete;

  std::ostream &os() { return scratch_->os(); }

  /// The formatted text. Returns a copy, so the buffer can be reused as soon as
  /// the caller is done with it.
  std::string str() const { return scratch_->str(); }

  /// Direct access to the buffer, for appending separators/brackets without
  /// paying for a stream insertion.
  std::string &buffer() { return scratch_->str(); }

 private:
  Scratch *scratch_;
};

}  // namespace pprint
}  // namespace lightusd
