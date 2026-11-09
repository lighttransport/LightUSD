// SPDX-License-Identifier: Apache-2.0
#ifdef NDEBUG
#undef NDEBUG
#endif
// This header must not introduce namespace-local copies of C ABI types.
#include "next/parser/value-parser-numeric.hh"
#include "next/reader/usda-reader.hh"
#include "next/c/lightusd-core-c.h"
#include "next/types/type-info.hh"
#include <cassert>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <string>

using namespace lightusd::next;

namespace lightusd { namespace next {
bool ParseUsdaArrayElementCount(const char*, size_t, size_t, TypeId, size_t*);
}}

int main() {
  size_t count = 1;
  assert(!ParseUsdaArrayElementCount(nullptr, 0, SIZE_MAX, TypeId::Float, &count));
  assert(count == 0);
  assert(!ParseUsdaArrayElementCount("[", 1, SIZE_MAX, TypeId::Float, &count));
  // Eager and retained-source parsing must agree, including the count-only path
  // accepting trailing commas and timecode values outside float32's range.
  const char* source = R"(#usda 1.0
def Xform "Root" {
  float[] f = [0, -0, +1.25, 1e-30,]
  int[] i = [-2147483648, 2147483647, 1.9,]
  uint[] u = [0, 4294967295, -0.5,]
  int64[] i64 = [-9223372036854775808, 9223372036854775807,]
  uint64[] u64 = [18446744073709551615,]
  bool[] b = [true, false, 0, 1,]
  point3f[] p = [(1,2,3), (4,5,6),]
  half3[] h = [(0.1, -0, 65504),]
  matrix2d[] m = [((1,2),(3,4)),]
  timecode[] t = [1e100,]
  double[] empty = []
}
)";
  LoadOptions lazy;
  lazy.parse_options.enable_usda_lazy_arrays = true;
  auto a = LoadUSDAFromString(source, std::strlen(source));
  auto b = LoadUSDAFromString(source, std::strlen(source), lazy);
  if (!a.success) std::fprintf(stderr, "eager: %s\n", a.error_summary.c_str());
  if (!b.success) std::fprintf(stderr, "lazy: %s\n", b.error_summary.c_str());
  assert(a.success && b.success);
  for (const char* name : {"f", "i", "u", "i64", "u64", "b", "p", "h", "m", "t", "empty"}) {
    const Value* va = a.stage.GetPrimAtPath("/Root").GetPropertyValue(name);
    const Value* vb = b.stage.GetPrimAtPath("/Root").GetPropertyValue(name);
    assert(va && vb && vb->is_lazy());
    assert(va->type_id() == vb->type_id());
    assert(va->array_size() == vb->array_size());

    // Regression: a lazy Value's element count lives in the carrier size
    // field and must survive copy/move (array_size() reads it directly).
    // This must run BEFORE `*va == *vb`, which materializes the lazy value
    // in place and would otherwise hide a lazy copy/move defect.
    {
      Value lazy_copy = *vb;
      assert(lazy_copy.is_lazy());
      assert(lazy_copy.array_size() == va->array_size());
      Value lazy_moved = std::move(lazy_copy);
      assert(lazy_moved.is_lazy());
      assert(lazy_moved.array_size() == va->array_size());
      assert(lazy_moved.type_id() == va->type_id());
    }

    assert(*va == *vb);
  }
  for (const char* field : {
      "int[] x = [2147483648]", "uint[] x = [-1]",
      "int64[] x = [9223372036854775808]",
      "uint64[] x = [18446744073709551616]", "bool[] x = [2]",
      "int[] x = [1, 1e100]", "uint[] x = [1, -1.5]",
      "float3[] x = [(1,2)]", "float3[] x = [(1,2,3,)]",
      "matrix2d[] x = [((1,2),(3))]", "float[] x = [1,,2]"}) {
    const std::string input = std::string("#usda 1.0\ndef Xform \"Root\" { ") + field + "\n}\n";
    assert(!LoadUSDAFromString(input).success);
    assert(!LoadUSDAFromString(input, lazy).success);
  }
  // IEEE binary16 oracle using exact power-of-two scaling, independent of the
  // C decoder's exponent normalization loop. Preserve signed zero/NaN payloads.
  for (unsigned h = 0; h < 65536; ++h) {
    const unsigned exponent = (h >> 10) & 31, mantissa = h & 1023;
    float expected;
    uint32_t bits;
    if (exponent == 31) {
      bits = ((h & 32768u) << 16) | 0x7f800000u | (mantissa << 13);
      std::memcpy(&expected, &bits, sizeof(bits));
    } else {
      expected = exponent ? std::ldexp(float(1024 + mantissa), int(exponent) - 25)
                          : std::ldexp(float(mantissa), -24);
      if (h & 32768) expected = -expected;
      std::memcpy(&bits, &expected, sizeof(bits));
    }
    const uint16_t raw = static_cast<uint16_t>(h);
    float converted = 0;
    assert(Value::MakeFromRaw(TypeId::Half, &raw).to_float(&converted));
    uint32_t actual;
    std::memcpy(&actual, &converted, sizeof(actual));
    assert(actual == bits);
  }
  const TypeInfo* first = GetTypeInfo(TypeId::Float);
  assert(first && first->id == TypeId::Float);
  for (uint16_t i = 0; i < LU_TYPE_COUNT; ++i) {
    const auto* info = GetTypeInfo(static_cast<TypeId>(i));
    const auto* row = ::lu_type_info_at(i);
    assert(info && row && static_cast<uint16_t>(info->id) == row->id);
    assert(info->name == row->name && info->cpp_name == row->cpp_name);
    assert(info->size == row->size && info->alignment == row->alignment);
    assert(static_cast<uint16_t>(info->component_type) == row->component_type);
    assert(info->component_count == row->component_count && info->flags == row->flags);
  }
  assert(GetTypeInfo(TypeId::Float) == first && first->id == TypeId::Float);
  assert(GetTypeInfo(static_cast<TypeId>(65535)) == nullptr);
}
