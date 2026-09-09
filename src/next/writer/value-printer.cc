// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - Value Printer Implementation

#include "value-printer.hh"
#include "../crate/crate-data-source.hh"
#include "../crate/crate-format.hh"  // HalfToFloat
#include "../crate/lazy-array.hh"
#include "../../safe-arithmetic.hh"
#include "stream-writer.hh"
#include "../types/value-view.hh"
#include "../types/type-id.hh"
#include "../strfmt.hh"
#include "dtoa.hh"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace lightusd {
namespace next {

namespace {

// Append helpers: format straight into a reused std::string with no per-element
// heap allocation. Byte-identical to the corresponding `ss << v` (classic
// locale: plain decimal, leading '-' for negatives, no '+'/padding/separators).
inline void AppendFloat(std::string& o, float v) { dtos_append(o, v); }
inline void AppendDouble(std::string& o, double v) { dtos_append(o, v); }

// Integer append helpers live in ../strfmt.hh (AppendInt/AppendUInt), shared with
// the USDA writer.

// Reserve headroom for an array, but cap the up-front growth so a single huge
// array doesn't trigger a giant mmap. Beyond the cap, std::string's geometric
// growth handles it with cheap amortized reallocations — and, crucially, avoids
// the multi-hundred-MB simultaneous allocations that thrash the allocator when
// many subtrees are serialized in parallel.
inline void ReserveArrayHeadroom(std::string& o, size_t want) {
  constexpr size_t kCap = 8u << 20;  // 8 MiB
  o.reserve(o.size() + std::min<size_t>(want, kCap) + 8);
}

// Escape a string for USDA output
std::string EscapeString(const std::string& s) {
  std::string result;
  result.reserve(s.size() + 2);
  result += '"';
  for (char c : s) {
    switch (c) {
      case '"':  result += "\\\""; break;
      case '\\': result += "\\\\"; break;
      case '\n': result += "\\n"; break;
      case '\r': result += "\\r"; break;
      case '\t': result += "\\t"; break;
      default: {
        // Control bytes written raw make the output unreadable by next's own
        // loader (format sniffing rejects them); emit \xNN like pxr does.
        const unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 0x20 || uc == 0x7f) {
          static const char* hexd = "0123456789abcdef";
          result += "\\x";
          result += hexd[uc >> 4];
          result += hexd[uc & 0xf];
        } else {
          result += c;
        }
        break;
      }
    }
  }
  result += '"';
  return result;
}

// Forward decl: recursive dictionary printer (defined after PrintValue).
std::string PrintDictionaryIndented(const Dict& d, const PrintOptions& opts,
                                    int base_depth);

class ChunkedStream {
 public:
  explicit ChunkedStream(StreamWriter& out) : out_(out) {
    buf_.reserve(kFlushAt + 256);
  }

  ~ChunkedStream() { flush(); }

  void append(const char* s) {
    while (*s) append(*s++);
  }
  void append(const char* s, size_t n) {
    if (n >= kFlushAt) {
      flush();
      out_.write(s, n);
      return;
    }
    if (buf_.size() + n > kFlushAt) flush();
    buf_.append(s, n);
  }
  void append(const std::string& s) { append(s.data(), s.size()); }
  void append(char c) {
    if (buf_.size() == kFlushAt) flush();
    buf_.push_back(c);
  }
  // Number formatters write into a small stack buffer and append it to the chunk
  // buffer in a single copy -- no intermediate std::string per scalar (this is
  // the per-element hot path for large numeric arrays). The formatters never emit
  // more than `sizeof(t)` bytes; clamping the length is a no-op at runtime but
  // lets the optimizer prove the append cannot take the oversized path (which
  // would otherwise trip -Warray-bounds on the stack buffer).
  void append_int(int64_t v) {
    char t[32];
    size_t n = IntTo(t, v);
    append(t, n < sizeof(t) ? n : sizeof(t));
  }
  void append_uint(uint64_t v) {
    char t[32];
    size_t n = UIntTo(t, v);
    append(t, n < sizeof(t) ? n : sizeof(t));
  }
  void append_float(float v) {
    char t[32];
    size_t n = dtos_to(t, v);
    append(t, n < sizeof(t) ? n : sizeof(t));
  }
  void append_half(uint16_t bits) {
    char t[48];
    size_t n = htos_to(t, bits);
    append(t, n < sizeof(t) ? n : sizeof(t));
  }
  void append_double(double v) {
    char t[32];
    size_t n = dtos_to(t, v);
    append(t, n < sizeof(t) ? n : sizeof(t));
  }
  void flush() {
    if (!buf_.empty()) {
      out_.write(buf_.data(), buf_.size());
      buf_.clear();
    }
  }

 private:
  static constexpr size_t kFlushAt = 64u << 10;
  StreamWriter& out_;
  std::string buf_;
};

template <typename T, typename Emit>
void EmitScalarArray(ChunkedStream& out, const T* data, size_t n,
                     size_t maxN, Emit emit) {
  out.append('[');
  size_t limit = (maxN > 0) ? std::min(maxN, n) : n;
  for (size_t i = 0; i < limit; ++i) {
    if (i) out.append(", ");
    emit(data[i]);
  }
  if (limit < n) out.append(", ...");
  out.append(']');
}

// Component shape of a vector/matrix/scalar element type: number of scalars per
// USD element, and (for matrices) the square dimension. Shared so the full and
// ranged array printers decompose elements identically.
struct CompShape {
  bool is_matrix;
  size_t mat_dim;
  size_t comp_count;
};
inline CompShape GetCompShape(TypeId type_id) {
  CompShape s;
  s.is_matrix =
      (type_id == TypeId::Matrix2f || type_id == TypeId::Matrix2d ||
       type_id == TypeId::Matrix3f || type_id == TypeId::Matrix3d ||
       type_id == TypeId::Matrix4f || type_id == TypeId::Matrix4d);
  s.mat_dim =
      (type_id == TypeId::Matrix2f || type_id == TypeId::Matrix2d) ? 2 :
      (type_id == TypeId::Matrix3f || type_id == TypeId::Matrix3d) ? 3 : 4;
  s.comp_count = GetComponentCount(type_id);
  if (s.comp_count < 1) s.comp_count = 1;
  return s;
}

// Emit a single composite element `(x, y, z)` / `((m00, ...), ...)` / scalar.
template <typename T, typename Emit>
inline void EmitCompElem(ChunkedStream& out, const T* d, const CompShape& s,
                         Emit emit) {
  if (s.is_matrix) {
    out.append('(');
    for (size_t r = 0; r < s.mat_dim; ++r) {
      if (r) out.append(", ");
      out.append('(');
      for (size_t c = 0; c < s.mat_dim; ++c) {
        if (c) out.append(", ");
        emit(d[r * s.mat_dim + c]);
      }
      out.append(')');
    }
    out.append(')');
  } else if (s.comp_count > 1) {
    out.append('(');
    for (size_t c = 0; c < s.comp_count; ++c) {
      if (c) out.append(", ");
      emit(d[c]);
    }
    out.append(')');
  } else {
    emit(d[0]);
  }
}

template <typename T, typename Emit>
void EmitCompArray(ChunkedStream& out, const T* data, size_t flat_count,
                   TypeId type_id, size_t maxN, Emit emit) {
  const CompShape s = GetCompShape(type_id);
  const size_t n = flat_count / s.comp_count;
  const size_t limit = (maxN > 0) ? std::min(maxN, n) : n;
  out.append('[');
  for (size_t i = 0; i < limit; ++i) {
    if (i) out.append(", ");
    EmitCompElem(out, data + i * s.comp_count, s, emit);
  }
  if (limit < n) out.append(", ...");
  out.append(']');
}

// Range variants: emit USD elements [lo, hi) only, controlling the enclosing
// brackets. The separator before element i uses the GLOBAL index i (so element 0
// has no leading ", " and a chunk starting mid-array prefixes ", " correctly),
// making concatenated ranges byte-identical to the full array print.
template <typename T, typename Emit>
void EmitScalarRange(ChunkedStream& out, const T* data, size_t lo, size_t hi,
                     bool open, bool close, Emit emit) {
  if (open) out.append('[');
  for (size_t i = lo; i < hi; ++i) {
    if (i) out.append(", ");
    emit(data[i]);
  }
  if (close) out.append(']');
}

template <typename T, typename Emit>
void EmitCompRange(ChunkedStream& out, const T* data, TypeId type_id, size_t lo,
                   size_t hi, bool open, bool close, Emit emit) {
  const CompShape s = GetCompShape(type_id);
  if (open) out.append('[');
  for (size_t i = lo; i < hi; ++i) {
    if (i) out.append(", ");
    EmitCompElem(out, data + i * s.comp_count, s, emit);
  }
  if (close) out.append(']');
}

void DiscardLazyArrayRangePages(const Value& value, size_t elem_lo,
                                size_t elem_hi) {
  if (!value.is_lazy() || elem_hi <= elem_lo) return;
  const LazyArrayRef* ref = value.lazy_ref();
  if (!ref || !ref->source || ref->block_len == 0 ||
      ref->crate_type == CrateTypeId::Invalid || ref->src_elem_stride == 0) {
    return;
  }
  const uint64_t header_bytes =
      CrateArrayCountHeaderBytes(ref->source->version());
  const uint64_t data_begin = ref->block_offset + header_bytes;
  if (data_begin < ref->block_offset) return;
  const uint64_t stride = uint64_t(ref->src_elem_stride);
  if (uint64_t(elem_lo) > ((std::numeric_limits<uint64_t>::max)() - data_begin) /
                              stride ||
      uint64_t(elem_hi) > ((std::numeric_limits<uint64_t>::max)() - data_begin) /
                              stride) {
    return;
  }
  const uint64_t byte_begin =
      data_begin + uint64_t(elem_lo) * stride;
  const uint64_t byte_end =
      data_begin + uint64_t(elem_hi) * stride;
  if (byte_begin < data_begin || byte_end < byte_begin) return;
  ref->source->DiscardRange(byte_begin, byte_end - byte_begin);
}

bool PrintCompressedIntLazyArrayToStream(StreamWriter& os, const Value& value,
                                         const PrintOptions& opts) {
  if (!value.is_lazy() || value.type_id() != TypeId::Int) return false;
  const LazyArrayRef* ref = value.lazy_ref();
  if (!ref || !ref->source || !ref->is_compressed ||
      ref->crate_type != CrateTypeId::Int) {
    return false;
  }

  const uint64_t header_bytes =
      CrateArrayCountHeaderBytes(ref->source->version());
  const uint64_t metadata_bytes = header_bytes + sizeof(uint64_t);
  if (ref->block_len < metadata_bytes) return false;

  const uint64_t count = ref->element_count;
  if (count > uint64_t((std::numeric_limits<size_t>::max)())) return false;
  if (opts.max_array_elements > 0 && count > opts.max_array_elements) {
    return false;  // preview truncation is handled by the generic path.
  }

  const uint64_t block_begin = ref->block_offset;
  const uint64_t block_end = block_begin + ref->block_len;
  if (block_end < block_begin || block_end > ref->source->size()) return false;
  const uint8_t* base = ref->source->base();
  if (!base) return false;

  uint64_t stored_count = 0;
  if (header_bytes == sizeof(uint32_t)) {
    uint32_t stored_count32 = 0;
    std::memcpy(&stored_count32, base + block_begin, sizeof(stored_count32));
    stored_count = stored_count32;
  } else {
    std::memcpy(&stored_count, base + block_begin, sizeof(stored_count));
  }
  if (stored_count != count) return false;

  uint64_t comp_size = 0;
  std::memcpy(&comp_size, base + block_begin + header_bytes,
              sizeof(comp_size));
  if (comp_size == 0 || comp_size > ref->block_len - metadata_bytes ||
      comp_size > uint64_t((std::numeric_limits<size_t>::max)())) {
    return false;
  }

  size_t code_bits = 0;
  size_t code_bytes = 0;
  size_t value_bytes = 0;
  size_t hint_value_bytes = 0;
  size_t initial_delta_size = 0;
  size_t max_delta_size = 0;
  if (!safe::mul(static_cast<size_t>(count), size_t(2), &code_bits) ||
      !safe::add(code_bits, size_t(7), &code_bits) ||
      !safe::mul(static_cast<size_t>(count), sizeof(int32_t), &value_bytes) ||
      !safe::mul(static_cast<size_t>(count), size_t(2), &hint_value_bytes) ||
      !safe::add(sizeof(int32_t), code_bits / 8, &code_bytes) ||
      !safe::add(code_bytes, hint_value_bytes, &initial_delta_size) ||
      !safe::add(code_bytes, value_bytes, &max_delta_size)) {
    return false;
  }
  initial_delta_size = std::min(initial_delta_size, max_delta_size);

  DecompressResult dr = DecompressCrateBlobWithCapacityHint(
      base + block_begin + metadata_bytes, static_cast<size_t>(comp_size),
      max_delta_size, initial_delta_size);
  if (!dr.success) return false;
  const uint8_t* buffer = dr.data.data();
  const size_t buffer_size = dr.data.size();
  if (count == 0) {
    os.write("[]", 2);
    return true;
  }
  if (!buffer || buffer_size < sizeof(int32_t)) return false;

  int32_t common_delta = 0;
  std::memcpy(&common_delta, buffer, sizeof(int32_t));
  const size_t codes_start = sizeof(int32_t);
  const size_t vints_start = code_bytes;
  if (buffer_size < vints_start) return false;

  size_t check_vints_pos = vints_start;
  for (size_t i = 0; i < static_cast<size_t>(count); ++i) {
    const uint8_t code_byte = buffer[codes_start + i / 4];
    const uint8_t code = (code_byte >> ((i % 4) * 2)) & 3;
    size_t step = 0;
    switch (code) {
      case 0:
        step = 0;
        break;
      case 1:
        step = sizeof(int8_t);
        break;
      case 2:
        step = sizeof(int16_t);
        break;
      case 3:
        step = sizeof(int32_t);
        break;
    }
    if (step > buffer_size - check_vints_pos) return false;
    check_vints_pos += step;
  }

  ChunkedStream out(os);
  out.append('[');
  int32_t prev = 0;
  size_t vints_pos = vints_start;
  for (size_t i = 0; i < static_cast<size_t>(count); ++i) {
    const uint8_t code_byte = buffer[codes_start + i / 4];
    const uint8_t code = (code_byte >> ((i % 4) * 2)) & 3;
    int32_t delta = 0;
    switch (code) {
      case 0:
        delta = common_delta;
        break;
      case 1: {
        if (vints_pos + sizeof(int8_t) > buffer_size) return false;
        int8_t v = 0;
        std::memcpy(&v, buffer + vints_pos, sizeof(int8_t));
        delta = static_cast<int32_t>(v);
        vints_pos += sizeof(int8_t);
        break;
      }
      case 2: {
        if (vints_pos + sizeof(int16_t) > buffer_size) return false;
        int16_t v = 0;
        std::memcpy(&v, buffer + vints_pos, sizeof(int16_t));
        delta = static_cast<int32_t>(v);
        vints_pos += sizeof(int16_t);
        break;
      }
      case 3: {
        if (vints_pos + sizeof(int32_t) > buffer_size) return false;
        std::memcpy(&delta, buffer + vints_pos, sizeof(int32_t));
        vints_pos += sizeof(int32_t);
        break;
      }
    }

    const uint32_t value_u =
        static_cast<uint32_t>(prev) + static_cast<uint32_t>(delta);
    const int32_t value = static_cast<int32_t>(value_u);
    if (i) out.append(", ");
    out.append_int(value);
    prev = value;
  }
  out.append(']');
  return true;
}

bool PrintArrayToStream(StreamWriter& os, const Value& value,
                        const PrintOptions& opts) {
  if (!value.is_array()) return false;
  if (PrintCompressedIntLazyArrayToStream(os, value, opts)) return true;
  constexpr size_t kSerialRangeChunkElems = 64u * 1024u;
  if (value.is_lazy() && value.array_size() >= kSerialRangeChunkElems &&
      IsChunkableArray(value, opts)) {
    const size_t n = value.array_size();
    for (size_t lo = 0; lo < n; lo += kSerialRangeChunkElems) {
      const size_t hi = (lo + kSerialRangeChunkElems < n)
                            ? lo + kSerialRangeChunkElems
                            : n;
      if (!PrintArrayRangeToStream(os, value, opts, lo, hi,
                                   /*open=*/lo == 0,
                                   /*close=*/hi == n)) {
        return false;
      }
    }
    return true;
  }
  ChunkedStream out(os);
  const size_t maxN = opts.max_array_elements;
  const TypeId type_id = value.type_id();

  switch (type_id) {
    case TypeId::Int: {
      ArrayScratch<int32_t> scratch;
      ArrayView<int32_t> view;
      if (!GetIntArrayView(value, &scratch, &view)) return false;
      EmitScalarArray(out, view.data, view.size, maxN,
                      [&](int32_t v) { out.append_int(v); });
      return true;
    }
    case TypeId::UInt:
    case TypeId::UChar: {
      ArrayScratch<uint32_t> scratch;
      ArrayView<uint32_t> view;
      if (!GetUIntArrayView(value, &scratch, &view)) return false;
      EmitScalarArray(out, view.data, view.size, maxN,
                      [&](uint32_t v) { out.append_uint(v); });
      return true;
    }
    case TypeId::Int64: {
      ArrayScratch<int64_t> scratch;
      ArrayView<int64_t> view;
      if (!GetInt64ArrayView(value, &scratch, &view)) return false;
      EmitScalarArray(out, view.data, view.size, maxN,
                      [&](int64_t v) { out.append_int(v); });
      return true;
    }
    case TypeId::UInt64: {
      ArrayScratch<uint64_t> scratch;
      ArrayView<uint64_t> view;
      if (!GetUInt64ArrayView(value, &scratch, &view)) return false;
      EmitScalarArray(out, view.data, view.size, maxN,
                      [&](uint64_t v) { out.append_uint(v); });
      return true;
    }
    case TypeId::Bool: {
      Value tmp;
      const Value* src = &value;
      if (value.is_lazy()) {
        tmp = value.materialized_copy();
        src = &tmp;
      }
      const std::vector<uint8_t>* a = src->as_bool_array();
      if (!a) return false;
      out.append('[');
      size_t limit = (maxN > 0) ? std::min(maxN, a->size()) : a->size();
      for (size_t i = 0; i < limit; ++i) {
        if (i) out.append(", ");
        out.append((*a)[i] ? "1" : "0");  // pxr spells VALUE bools 1/0
      }
      if (limit < a->size()) out.append(", ...");
      out.append(']');
      return true;
    }
    case TypeId::Token:
    case TypeId::String:
    case TypeId::AssetPath: {
      Value tmp;
      const Value* src = &value;
      if (value.is_lazy()) {
        tmp = value.materialized_copy();
        src = &tmp;
      }
      const std::vector<std::string>* a = src->as_token_array();
      if (!a) return false;
      out.append('[');
      size_t limit = (maxN > 0) ? std::min(maxN, a->size()) : a->size();
      for (size_t i = 0; i < limit; ++i) {
        if (i) out.append(", ");
        if (type_id == TypeId::AssetPath) {
          out.append(FormatAssetPathForUsda((*a)[i]));
        } else {
          out.append(EscapeString((*a)[i]));
        }
      }
      if (limit < a->size()) out.append(", ...");
      out.append(']');
      return true;
    }
    default: {
      const TypeId component = GetComponentType(type_id);
      const bool dbl = (component == TypeId::Double) ||
                       type_id == TypeId::Double ||
                       type_id == TypeId::TimeCode;
      const bool half = (component == TypeId::Half) || type_id == TypeId::Half;
      const bool flt = (component == TypeId::Float) ||
                       type_id == TypeId::Float || half;
      if (dbl) {
        ArrayScratch<double> scratch;
        ArrayView<double> view;
        if (!GetDoubleArrayView(value, &scratch, &view)) return false;
        EmitCompArray(out, view.data, view.size, type_id, maxN,
                      [&](double v) { out.append_double(v); });
        return true;
      }
      if (flt) {
        ArrayScratch<float> scratch;
        ArrayView<float> view;
        if (!GetFloatArrayView(value, &scratch, &view)) return false;
        EmitCompArray(out, view.data, view.size, type_id, maxN,
                      [&](float v) {
                        if (half) {
                          out.append_half(FloatToHalf(v));
                        } else {
                          out.append_float(v);
                        }
                      });
        return true;
      }
      if (component == TypeId::Int) {  // Int2/Int3/Int4 arrays
        ArrayScratch<int32_t> scratch;
        ArrayView<int32_t> view;
        if (!GetIntArrayView(value, &scratch, &view)) return false;
        EmitCompArray(out, view.data, view.size, type_id, maxN,
                      [&](int32_t v) { out.append_int(v); });
        return true;
      }
      if (component == TypeId::UInt) {  // UInt2/UInt3/UInt4 arrays
        ArrayScratch<uint32_t> scratch;
        ArrayView<uint32_t> view;
        if (!GetUIntArrayView(value, &scratch, &view)) return false;
        EmitCompArray(out, view.data, view.size, type_id, maxN,
                      [&](uint32_t v) { out.append_uint(v); });
        return true;
      }
      return false;
    }
  }
}

void DiscardLazyArraySourcePages(const Value& value) {
  if (!value.is_lazy()) return;
  const LazyArrayRef* ref = value.lazy_ref();
  if (!ref || !ref->source || ref->block_len == 0) return;
  constexpr uint64_t kMinDiscard = 1ull << 20;
  if (ref->block_len < kMinDiscard) return;
  ref->source->DiscardRange(ref->block_offset, ref->block_len);
}

}  // anonymous namespace

void PrintValueInto(std::string& out, const Value& value,
                    const PrintOptions& opts) {
  if (value.is_empty()) {
    out += "None";
    return;
  }

  TypeId type_id = value.type_id();

  // Handle arrays
  if (value.is_array()) {
    const size_t maxN = opts.max_array_elements;

    const bool is_matrix =
        (type_id == TypeId::Matrix2f || type_id == TypeId::Matrix2d ||
         type_id == TypeId::Matrix3f || type_id == TypeId::Matrix3d ||
         type_id == TypeId::Matrix4f || type_id == TypeId::Matrix4d);
    const size_t mat_dim =
        (type_id == TypeId::Matrix2f || type_id == TypeId::Matrix2d) ? 2 :
        (type_id == TypeId::Matrix3f || type_id == TypeId::Matrix3d) ? 3 : 4;
    size_t comp_count = GetComponentCount(type_id);
    if (comp_count < 1) comp_count = 1;
    const bool is_half = (GetComponentType(type_id) == TypeId::Half) ||
                         type_id == TypeId::Half;

    // Reserve a generous estimate (over-reserve is the accepted marginal memory)
    // so large numeric arrays append without reallocation churn.
    out += "[";

    // Emit a single element (scalar, vector tuple, or nested matrix rows).
    auto emit_float = [&](float v) {
      if (is_half) {
        htos_append(out, FloatToHalf(v));
      } else {
        AppendFloat(out, v);
      }
    };
    auto emit_elem_float = [&](const float* d) {
      if (is_matrix) {
        out += "(";
        for (size_t r = 0; r < mat_dim; ++r) {
          if (r) out += ", ";
          out += "(";
          for (size_t c = 0; c < mat_dim; ++c) {
            if (c) out += ", ";
            emit_float(d[r * mat_dim + c]);
          }
          out += ")";
        }
        out += ")";
      } else if (comp_count > 1) {
        out += "(";
        for (size_t c = 0; c < comp_count; ++c) {
          if (c) out += ", ";
          emit_float(d[c]);
        }
        out += ")";
      } else {
        emit_float(d[0]);
      }
    };
    auto emit_elem_double = [&](const double* d) {
      if (is_matrix) {
        out += "(";
        for (size_t r = 0; r < mat_dim; ++r) {
          if (r) out += ", ";
          out += "(";
          for (size_t c = 0; c < mat_dim; ++c) {
            if (c) out += ", ";
            AppendDouble(out, d[r * mat_dim + c]);
          }
          out += ")";
        }
        out += ")";
      } else if (comp_count > 1) {
        out += "(";
        for (size_t c = 0; c < comp_count; ++c) {
          if (c) out += ", ";
          AppendDouble(out, d[c]);
        }
        out += ")";
      } else {
        AppendDouble(out, d[0]);
      }
    };

    switch (type_id) {
      case TypeId::Int: {
        if (const auto* a = value.as_int_array()) {
          size_t limit = (maxN > 0) ? std::min(maxN, a->size()) : a->size();
          ReserveArrayHeadroom(out, limit * 8);
          for (size_t i = 0; i < limit; ++i) { if (i) out += ", "; AppendInt(out, (*a)[i]); }
          if (limit < a->size()) out += ", ...";
        }
        break;
      }
      case TypeId::UInt:
      case TypeId::UChar: {
        if (const auto* a = value.as_uint_array()) {
          size_t limit = (maxN > 0) ? std::min(maxN, a->size()) : a->size();
          ReserveArrayHeadroom(out, limit * 8);
          for (size_t i = 0; i < limit; ++i) { if (i) out += ", "; AppendUInt(out, (*a)[i]); }
          if (limit < a->size()) out += ", ...";
        }
        break;
      }
      case TypeId::Int64: {
        if (const auto* a = value.as_int64_array()) {
          size_t limit = (maxN > 0) ? std::min(maxN, a->size()) : a->size();
          ReserveArrayHeadroom(out, limit * 10);
          for (size_t i = 0; i < limit; ++i) { if (i) out += ", "; AppendInt(out, (*a)[i]); }
          if (limit < a->size()) out += ", ...";
        }
        break;
      }
      case TypeId::UInt64: {
        if (const auto* a = value.as_uint64_array()) {
          size_t limit = (maxN > 0) ? std::min(maxN, a->size()) : a->size();
          ReserveArrayHeadroom(out, limit * 10);
          for (size_t i = 0; i < limit; ++i) { if (i) out += ", "; AppendUInt(out, (*a)[i]); }
          if (limit < a->size()) out += ", ...";
        }
        break;
      }
      case TypeId::Bool: {
        if (const auto* a = value.as_bool_array()) {
          size_t limit = (maxN > 0) ? std::min(maxN, a->size()) : a->size();
          for (size_t i = 0; i < limit; ++i) { if (i) out += ", "; out += ((*a)[i] ? "1" : "0"); }  // pxr: 1/0
          if (limit < a->size()) out += ", ...";
        }
        break;
      }
      case TypeId::Token:
      case TypeId::String:
      case TypeId::AssetPath: {
        if (const auto* a = value.as_token_array()) {
          size_t limit = (maxN > 0) ? std::min(maxN, a->size()) : a->size();
          for (size_t i = 0; i < limit; ++i) {
            if (i) out += ", ";
            if (type_id == TypeId::AssetPath) out += FormatAssetPathForUsda((*a)[i]);
            else out += EscapeString((*a)[i]);
          }
          if (limit < a->size()) out += ", ...";
        }
        break;
      }
      default: {
        // float- or double-backed scalar / vector / matrix arrays
        const bool dbl = (GetComponentType(type_id) == TypeId::Double) ||
                         type_id == TypeId::Double ||
                         type_id == TypeId::TimeCode;
        if (dbl) {
          if (const auto* a = value.as_double_array()) {
            size_t n = a->size() / comp_count;
            size_t limit = (maxN > 0) ? std::min(maxN, n) : n;
            ReserveArrayHeadroom(out, limit * comp_count * 12);
            for (size_t i = 0; i < limit; ++i) { if (i) out += ", "; emit_elem_double(a->data() + i * comp_count); }
            if (limit < n) out += ", ...";
          }
        } else if (GetComponentType(type_id) == TypeId::UInt) {
          // UInt2/UInt3/UInt4 arrays (flat uint32 buffer)
          if (const auto* a = value.as_uint_array()) {
            size_t n = a->size() / comp_count;
            size_t limit = (maxN > 0) ? std::min(maxN, n) : n;
            ReserveArrayHeadroom(out, limit * comp_count * 12);
            for (size_t i = 0; i < limit; ++i) {
              if (i) out += ", ";
              const uint32_t* d = a->data() + i * comp_count;
              out += "(";
              for (size_t c = 0; c < comp_count; ++c) {
                if (c) out += ", ";
                AppendUInt(out, d[c]);
              }
              out += ")";
            }
            if (limit < n) out += ", ...";
          }
        } else if (GetComponentType(type_id) == TypeId::Int) {
          // Int2/Int3/Int4 arrays (flat int32 buffer)
          if (const auto* a = value.as_int_array()) {
            size_t n = a->size() / comp_count;
            size_t limit = (maxN > 0) ? std::min(maxN, n) : n;
            ReserveArrayHeadroom(out, limit * comp_count * 12);
            for (size_t i = 0; i < limit; ++i) {
              if (i) out += ", ";
              const int32_t* d = a->data() + i * comp_count;
              out += "(";
              for (size_t c = 0; c < comp_count; ++c) {
                if (c) out += ", ";
                AppendInt(out, d[c]);
              }
              out += ")";
            }
            if (limit < n) out += ", ...";
          }
        } else {
          if (const auto* a = value.as_float_array()) {
            size_t n = a->size() / comp_count;
            size_t limit = (maxN > 0) ? std::min(maxN, n) : n;
            ReserveArrayHeadroom(out, limit * comp_count * 12);
            for (size_t i = 0; i < limit; ++i) { if (i) out += ", "; emit_elem_float(a->data() + i * comp_count); }
            if (limit < n) out += ", ...";
          }
        }
        break;
      }
    }

    out += "]";
    return;
  }

  // Handle scalars and vectors
  switch (type_id) {
    case TypeId::Bool: {
      const bool* v = value.as_bool();
      out += v ? (*v ? "1" : "0") : "None";  // pxr spells VALUE bools 1/0
      return;
    }

    case TypeId::Int: {
      const int32_t* v = value.as_int();
      if (v) AppendInt(out, *v); else out += "None";
      return;
    }

    case TypeId::UInt: {
      const uint32_t* v = value.as_uint();
      if (v) AppendUInt(out, *v); else out += "None";
      return;
    }

    case TypeId::UChar: {
      const uint8_t* v = value.as_uchar();
      if (v) AppendUInt(out, *v); else out += "None";
      return;
    }

    case TypeId::Int64: {
      const int64_t* v = value.as_int64();
      if (v) AppendInt(out, *v); else out += "None";
      return;
    }

    case TypeId::UInt64: {
      const uint64_t* v = value.as_uint64();
      if (v) AppendUInt(out, *v); else out += "None";
      return;
    }

    case TypeId::Float: {
      const float* v = value.as_float();
      if (v) AppendFloat(out, *v); else out += "None";
      return;
    }

    case TypeId::Double:
    case TypeId::TimeCode: {  // scalar timecode: same 8-byte double storage
      const double* v = value.as_double();
      if (v) AppendDouble(out, *v); else out += "None";
      return;
    }

    case TypeId::String:
    case TypeId::PathExpression: {  // SdfPathExpression prints as a string
      const std::string* v = value.as_string();
      out += v ? EscapeString(*v) : "None";
      return;
    }

    case TypeId::Token: {
      const std::string* v = value.as_token();
      out += v ? EscapeString(*v) : "None";
      return;
    }

    case TypeId::AssetPath: {
      const std::string* v = value.as_asset_path();
      if (v) out += FormatAssetPathForUsda(*v); else out += "None";
      return;
    }

    case TypeId::UInt2:
    case TypeId::UInt3:
    case TypeId::UInt4: {
      // Raw uint32 SBO lanes (no dedicated accessors).
      size_t nbytes = 0;
      const uint8_t* b = value.raw_bytes(&nbytes);
      const size_t comps = nbytes / 4;
      if (!b || comps < 2 || comps > 4) { out += "None"; return; }
      uint32_t lanes[4];
      std::memcpy(lanes, b, comps * 4);
      out += '(';
      for (size_t c = 0; c < comps; ++c) {
        if (c) out += ", ";
        AppendUInt(out, lanes[c]);
      }
      out += ')';
      return;
    }

    case TypeId::Int2: {
      const int32_t* v = value.as_int2();
      if (!v) { out += "None"; return; }
      out += '('; AppendInt(out, v[0]); out += ", "; AppendInt(out, v[1]); out += ')';
      return;
    }

    case TypeId::Int3: {
      const int32_t* v = value.as_int3();
      if (!v) { out += "None"; return; }
      out += '('; AppendInt(out, v[0]); out += ", "; AppendInt(out, v[1]);
      out += ", "; AppendInt(out, v[2]); out += ')';
      return;
    }

    case TypeId::Int4: {
      const int32_t* v = value.as_int4();
      if (!v) { out += "None"; return; }
      out += '('; AppendInt(out, v[0]); out += ", "; AppendInt(out, v[1]);
      out += ", "; AppendInt(out, v[2]); out += ", "; AppendInt(out, v[3]); out += ')';
      return;
    }

    case TypeId::Float2:
    case TypeId::Texcoord2f: {
      const float* v = value.as_float2();
      if (!v) { out += "None"; return; }
      out += '('; AppendFloat(out, v[0]); out += ", "; AppendFloat(out, v[1]); out += ')';
      return;
    }

    case TypeId::Float3:
    case TypeId::Point3f:
    case TypeId::Vector3f:
    case TypeId::Normal3f:
    case TypeId::Color3f:
    case TypeId::Texcoord3f: {
      const float* v = value.as_float3();
      if (!v) { out += "None"; return; }
      out += '('; AppendFloat(out, v[0]); out += ", "; AppendFloat(out, v[1]);
      out += ", "; AppendFloat(out, v[2]); out += ')';
      return;
    }

    case TypeId::Float4:
    case TypeId::Color4f: {
      const float* v = value.as_float4();
      if (!v) { out += "None"; return; }
      out += '('; AppendFloat(out, v[0]); out += ", "; AppendFloat(out, v[1]);
      out += ", "; AppendFloat(out, v[2]); out += ", "; AppendFloat(out, v[3]); out += ')';
      return;
    }

    case TypeId::Double2:
    case TypeId::Texcoord2d: {
      const double* v = value.as_double2();
      if (!v) { out += "None"; return; }
      out += '('; AppendDouble(out, v[0]); out += ", "; AppendDouble(out, v[1]); out += ')';
      return;
    }

    case TypeId::Double3:
    case TypeId::Point3d:
    case TypeId::Vector3d:
    case TypeId::Normal3d:
    case TypeId::Color3d:
    case TypeId::Texcoord3d: {
      const double* v = value.as_double3();
      if (!v) { out += "None"; return; }
      out += '('; AppendDouble(out, v[0]); out += ", "; AppendDouble(out, v[1]);
      out += ", "; AppendDouble(out, v[2]); out += ')';
      return;
    }

    case TypeId::Double4:
    case TypeId::Color4d: {
      const double* v = value.as_double4();
      if (!v) { out += "None"; return; }
      out += '('; AppendDouble(out, v[0]); out += ", "; AppendDouble(out, v[1]);
      out += ", "; AppendDouble(out, v[2]); out += ", "; AppendDouble(out, v[3]); out += ')';
      return;
    }

    case TypeId::Quatf: {
      const float* v = value.as_float4();  // Quat stored as 4 floats
      if (!v) { out += "None"; return; }
      out += '('; AppendFloat(out, v[0]); out += ", "; AppendFloat(out, v[1]);
      out += ", "; AppendFloat(out, v[2]); out += ", "; AppendFloat(out, v[3]); out += ')';
      return;
    }

    case TypeId::Quatd: {
      const double* v = value.as_double4();
      if (!v) { out += "None"; return; }
      out += '('; AppendDouble(out, v[0]); out += ", "; AppendDouble(out, v[1]);
      out += ", "; AppendDouble(out, v[2]); out += ", "; AppendDouble(out, v[3]); out += ')';
      return;
    }

    case TypeId::Matrix2f: {
      const float* v = value.as_matrix2f();
      if (!v) { out += "None"; return; }
      out += "(("; AppendFloat(out, v[0]); out += ", "; AppendFloat(out, v[1]);
      out += "), ("; AppendFloat(out, v[2]); out += ", "; AppendFloat(out, v[3]); out += "))";
      return;
    }

    case TypeId::Matrix3f: {
      const float* v = value.as_matrix3f();
      if (!v) { out += "None"; return; }
      out += "((";
      for (int row = 0; row < 3; ++row) {
        if (row > 0) out += "), (";
        for (int col = 0; col < 3; ++col) {
          if (col > 0) out += ", ";
          AppendFloat(out, v[row * 3 + col]);
        }
      }
      out += "))";
      return;
    }

    case TypeId::Matrix4f: {
      const float* v = value.as_matrix4f();
      if (!v) { out += "None"; return; }
      out += "((";
      for (int row = 0; row < 4; ++row) {
        if (row > 0) out += "), (";
        for (int col = 0; col < 4; ++col) {
          if (col > 0) out += ", ";
          AppendFloat(out, v[row * 4 + col]);
        }
      }
      out += "))";
      return;
    }

    case TypeId::Matrix2d: {
      const double* v = value.as_matrix2d();
      if (!v) { out += "None"; return; }
      out += "(("; AppendDouble(out, v[0]); out += ", "; AppendDouble(out, v[1]);
      out += "), ("; AppendDouble(out, v[2]); out += ", "; AppendDouble(out, v[3]); out += "))";
      return;
    }

    case TypeId::Matrix3d: {
      const double* v = value.as_matrix3d();
      if (!v) { out += "None"; return; }
      out += "((";
      for (int row = 0; row < 3; ++row) {
        if (row > 0) out += "), (";
        for (int col = 0; col < 3; ++col) {
          if (col > 0) out += ", ";
          AppendDouble(out, v[row * 3 + col]);
        }
      }
      out += "))";
      return;
    }

    case TypeId::Matrix4d:
    case TypeId::Frame4d: {  // matrix4d role
      const double* v = value.as_matrix4d();
      if (!v) { out += "None"; return; }
      out += "((";
      for (int row = 0; row < 4; ++row) {
        if (row > 0) out += "), (";
        for (int col = 0; col < 4; ++col) {
          if (col > 0) out += ", ";
          AppendDouble(out, v[row * 4 + col]);
        }
      }
      out += "))";
      return;
    }

    // Raw-half SBO scalars (half, half2/3/4, quath, half role types): the
    // parser stores raw half-bit lanes; widen per lane for printing.
    case TypeId::Half:
    case TypeId::Half2:
    case TypeId::Half3:
    case TypeId::Half4:
    case TypeId::Quath:
    case TypeId::Point3h:
    case TypeId::Vector3h:
    case TypeId::Normal3h:
    case TypeId::Color3h:
    case TypeId::Color4h:
    case TypeId::Texcoord2h:
    case TypeId::Texcoord3h: {
      size_t nbytes = 0;
      const uint8_t* b = value.raw_bytes(&nbytes);
      const size_t comps = nbytes / 2;
      if (!b || comps == 0 || comps > 4) { out += "None"; return; }
      uint16_t lanes[4];
      std::memcpy(lanes, b, comps * 2);
      if (comps == 1) { htos_append(out, lanes[0]); return; }
      out += '(';
      for (size_t c = 0; c < comps; ++c) {
        if (c) out += ", ";
        htos_append(out, lanes[c]);
      }
      out += ')';
      return;
    }

    case TypeId::Dictionary: {
      const Dict* d = value.as_dictionary();
      if (d) out += PrintDictionaryIndented(*d, opts, 0); else out += "{\n}";
      return;
    }

    default:
      out += "<unsupported type ";
      AppendInt(out, static_cast<int>(type_id));
      out += ">";
      return;
  }
}

std::string FormatAssetPathForUsda(const std::string& path) {
  if (path.find('@') == std::string::npos) {
    return "@" + path + "@";
  }
  // pxr triple-delimiter form for paths containing '@'; a literal "@@@" inside
  // is escaped as "\\@@@".
  std::string out = "@@@";
  for (size_t i = 0; i < path.size(); ++i) {
    if (path.compare(i, 3, "@@@") == 0) {
      out += "\\@@@";
      i += 2;
    } else {
      out += path[i];
    }
  }
  out += "@@@";
  return out;
}

std::string PrintValue(const Value& value, const PrintOptions& opts) {
  std::string out;
  PrintValueInto(out, value, opts);
  return out;
}

void PrintValue(StreamWriter& out, const Value& value, const PrintOptions& opts) {
  if (value.is_array() && PrintArrayToStream(out, value, opts)) {
    DiscardLazyArraySourcePages(value);
    return;
  }
  std::string tmp;
  PrintValueInto(tmp, value, opts);
  out.write(tmp);
}

size_t ArrayElementCount(const Value& value) {
  return value.is_array() ? value.array_size() : 0;
}

bool IsChunkableType(const Value& value, const PrintOptions& opts) {
  if (!value.is_array()) return false;
  const size_t n = value.array_size();
  if (n == 0) return false;
  if (opts.max_array_elements > 0 && n > opts.max_array_elements) return false;
  const TypeId type_id = value.type_id();
  switch (type_id) {
    case TypeId::Int:
    case TypeId::UInt:
    case TypeId::UChar:
    case TypeId::Int64:
    case TypeId::UInt64:
      return true;
    default: {
      const TypeId comp = GetComponentType(type_id);
      const bool dbl = (comp == TypeId::Double) || type_id == TypeId::Double ||
                       type_id == TypeId::TimeCode;
      // Half-backed types (half / half3 / quath / ...) are widened to float by
      // as_float_array, so the float range printer handles them identically.
      const bool flt = (comp == TypeId::Float) || type_id == TypeId::Float ||
                       (comp == TypeId::Half) || type_id == TypeId::Half;
      return dbl || flt;
    }
  }
}

bool IsChunkableArray(const Value& value, const PrintOptions& opts) {
  if (!value.is_array()) return false;
  // Lazy arrays are chunkable only when their bytes can be aliased zero-copy
  // (uncompressed/aligned): then each chunk borrows the same source mapping with
  // no decode, so concurrent range formatting is safe and allocation-free. A
  // compressed/unborrowable lazy array would re-decode per chunk, so exclude it.
  if (value.is_lazy() && !CanBorrowLazyFlat(value)) return false;
  const size_t n = value.array_size();
  if (n == 0) return false;
  // A truncated array prints only a few elements + ", ..." -> never worth
  // chunking, and the range printer does not reproduce the truncation marker.
  if (opts.max_array_elements > 0 && n > opts.max_array_elements) return false;
  const TypeId type_id = value.type_id();
  switch (type_id) {
    case TypeId::Int:
    case TypeId::UInt:
    case TypeId::UChar:
    case TypeId::Int64:
    case TypeId::UInt64:
      return true;
    default: {
      const TypeId comp = GetComponentType(type_id);
      const bool dbl = (comp == TypeId::Double) || type_id == TypeId::Double ||
                       type_id == TypeId::TimeCode;
      const bool flt = (comp == TypeId::Float) || type_id == TypeId::Float ||
                       (comp == TypeId::Half) || type_id == TypeId::Half;
      return dbl || flt;
    }
  }
}

bool PrintArrayRangeToStream(StreamWriter& os, const Value& value,
                             const PrintOptions& opts, size_t elem_lo,
                             size_t elem_hi, bool open, bool close) {
  // Array element formatting uses shortest round-trip (dtoa), matching the full
  // array printer; `opts` precision applies only to scalar values, so it is
  // unused here but kept for signature symmetry with the full printer.
  (void)opts;
  if (!value.is_array()) return false;
  const size_t element_count = value.array_size();
  if (elem_lo > elem_hi || elem_hi > element_count) return false;
  ChunkedStream out(os);
  auto done = [&]() {
    DiscardLazyArrayRangePages(value, elem_lo, elem_hi);
    return true;
  };
  const TypeId type_id = value.type_id();
  switch (type_id) {
    case TypeId::Int: {
      ArrayScratch<int32_t> scratch;
      ArrayView<int32_t> view;
      if (!GetIntArrayView(value, &scratch, &view)) return false;
      EmitScalarRange(out, view.data, elem_lo, elem_hi, open, close,
                      [&](int32_t v) { out.append_int(v); });
      return done();
    }
    case TypeId::UInt: {
      ArrayScratch<uint32_t> scratch;
      ArrayView<uint32_t> view;
      if (!GetUIntArrayView(value, &scratch, &view)) return false;
      EmitScalarRange(out, view.data, elem_lo, elem_hi, open, close,
                      [&](uint32_t v) { out.append_uint(v); });
      return done();
    }
    case TypeId::Int64: {
      ArrayScratch<int64_t> scratch;
      ArrayView<int64_t> view;
      if (!GetInt64ArrayView(value, &scratch, &view)) return false;
      EmitScalarRange(out, view.data, elem_lo, elem_hi, open, close,
                      [&](int64_t v) { out.append_int(v); });
      return done();
    }
    case TypeId::UInt64: {
      ArrayScratch<uint64_t> scratch;
      ArrayView<uint64_t> view;
      if (!GetUInt64ArrayView(value, &scratch, &view)) return false;
      EmitScalarRange(out, view.data, elem_lo, elem_hi, open, close,
                      [&](uint64_t v) { out.append_uint(v); });
      return done();
    }
    default: {
      const TypeId component = GetComponentType(type_id);
      const bool dbl = (component == TypeId::Double) || type_id == TypeId::Double;
      // Half-backed types widen to float via as_float_array (see IsChunkableType).
      const bool half = (component == TypeId::Half) || type_id == TypeId::Half;
      const bool flt = (component == TypeId::Float) || type_id == TypeId::Float ||
                       half;
      if (dbl) {
        ArrayScratch<double> scratch;
        ArrayView<double> view;
        if (!GetDoubleArrayView(value, &scratch, &view)) return false;
        EmitCompRange(out, view.data, type_id, elem_lo, elem_hi, open, close,
                      [&](double v) { out.append_double(v); });
        return done();
      }
      if (flt) {
        ArrayScratch<float> scratch;
        ArrayView<float> view;
        if (!GetFloatArrayView(value, &scratch, &view)) return false;
        EmitCompRange(out, view.data, type_id, elem_lo, elem_hi, open, close,
                      [&](float v) {
                        if (half) {
                          out.append_half(FloatToHalf(v));
                        } else {
                          out.append_float(v);
                        }
                      });
        return done();
      }
      return false;
    }
  }
}

namespace {

std::string PrintDictionaryIndented(const Dict& d, const PrintOptions& opts,
                                    int base_depth) {
  std::string s;
  s += "{\n";

  // Keys that aren't valid identifiers (spaces, punctuation, leading digit,
  // keyword-free requirement does not apply) must be quoted or the output is
  // unparseable.
  auto key_text = [](const std::string& k) -> std::string {
    auto ident = [](const std::string& v) {
      if (v.empty()) return false;
      auto head = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
      };
      if (!head(v[0])) return false;
      for (char c : v) {
        if (!head(c) && !(c >= '0' && c <= '9') && c != ':') return false;
      }
      return true;
    };
    return ident(k) ? k : EscapeString(k);
  };

  struct Frame {
    const Dict* dict{nullptr};
    int depth{0};
    size_t next_entry{0};
  };
  std::vector<Frame> stack;
  stack.push_back(Frame{&d, base_depth, 0});

  while (!stack.empty()) {
    Frame& frame = stack.back();
    if (frame.next_entry < frame.dict->entries().size()) {
      const auto& kv = frame.dict->entries()[frame.next_entry++];
      const std::string& key = kv.first;
      const Value& val = kv.second;
      for (int i = 0; i <= frame.depth; ++i) s += opts.indent;
      if (val.is_dictionary()) {
        s += "dictionary ";
        s += key_text(key);
        s += " = ";
        const Dict* nested = val.as_dictionary();
        if (!nested) {
          s += "{}\n";
          continue;
        }
        s += "{\n";
        stack.push_back(Frame{nested, frame.depth + 1, 0});
        continue;
      }

      s += PrintTypeName(val.type_id(), val.is_array());
      s += " ";
      s += key_text(key);
      s += " = ";
      PrintValueInto(s, val, opts);
      s += "\n";
      continue;
    }

    for (int i = 0; i < frame.depth; ++i) s += opts.indent;
    s += "}";
    stack.pop_back();
    if (!stack.empty()) s += "\n";
  }
  return s;
}

}  // anonymous namespace

std::string PrintTypeName(TypeId type_id, bool is_array) {
  const char* type_name = GetTypeName(type_id);
  std::string name = (type_name && type_name[0] != '\0') ? type_name : "unknown";
  if (is_array) {
    name += "[]";
  }
  return name;
}

std::string PrintAttributeValue(const std::string& type_name, const std::string& attr_name,
                                 const Value& value, const PrintOptions& opts) {
  std::string s;
  s += type_name;
  if (value.is_array()) {
    s += "[]";
  }
  s += " ";
  s += attr_name;
  s += " = ";
  PrintValueInto(s, value, opts);
  return s;
}

}  // namespace next
}  // namespace lightusd
