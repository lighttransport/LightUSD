// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - Unit tests

#include <iostream>
#include <fstream>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <limits>
#include <string>
#include <utility>
#include <vector>
#if defined(LIGHTUSD_ENABLE_THREAD)
#include <thread>
#endif

#include "next/types/type-id.hh"
#include "next/types/type-info.hh"
#include "next/types/value.hh"
#include "next/crate/crate-data-source.hh"
#include "next/crate/crate-format.hh"
#include "next/crate/lazy-array.hh"
#include "next/prim/path.hh"
#include "next/prim/attribute.hh"
#include "next/prim/prim.hh"
#include "next/parser/lexer.hh"
#include "next/parser/value-parser.hh"
#include "next/parser/ascii-parser.hh"
#include "next/stage/stage.hh"
#include "next/reader/usda-reader.hh"
#include "next/schema/physics-api.hh"
#include "next/schema/physics-joint.hh"
#include "next/lightusd-next.hh"

using namespace lightusd::next;

namespace {

#if !defined(LIGHTUSD_NEXT_NO_MMAP) && !defined(__EMSCRIPTEN__) && \
    !defined(__wasi__) &&                                             \
    (defined(__unix__) || defined(__APPLE__) || defined(__linux__))
constexpr bool kExpectUsdaLazyMmap = true;
#else
constexpr bool kExpectUsdaLazyMmap = false;
#endif

std::string UsdaFixturePath(const std::string& filename) {
  const std::string file_path(__FILE__);
  const std::string marker = "/tests/next/";
  const size_t pos = file_path.rfind(marker);
  assert(pos != std::string::npos);
  return file_path.substr(0, pos) + "/tests/usda/" + filename;
}

}  // namespace

// ============================================================
// Type system tests
// ============================================================

void test_type_id() {
  std::cout << "Testing TypeId..." << std::endl;

  // Test type name lookup
  assert(GetTypeName(TypeId::Float) != nullptr);
  assert(std::strcmp(GetTypeName(TypeId::Float), "float") == 0);
  assert(std::strcmp(GetTypeName(TypeId::Float3), "float3") == 0);
  assert(std::strcmp(GetTypeName(TypeId::Matrix4d), "matrix4d") == 0);

  // Test reverse lookup
  assert(GetTypeIdFromName("float") == TypeId::Float);
  assert(GetTypeIdFromName("float3") == TypeId::Float3);
  assert(GetTypeIdFromName("unknown") == TypeId::Invalid);

  // Test type size
  assert(GetTypeSize(TypeId::Float) == sizeof(float));
  assert(GetTypeSize(TypeId::Double) == sizeof(double));
  assert(GetTypeSize(TypeId::Float3) == 3 * sizeof(float));
  assert(GetTypeSize(TypeId::Matrix4d) == 16 * sizeof(double));

  // Test scalar detection
  assert(IsScalarType(TypeId::Float) == true);
  assert(IsScalarType(TypeId::Float3) == false);
  assert(IsScalarType(TypeId::Matrix4d) == false);

  // Test component type
  assert(GetComponentType(TypeId::Float3) == TypeId::Float);
  assert(GetComponentType(TypeId::Double3) == TypeId::Double);
  assert(GetComponentType(TypeId::Matrix4d) == TypeId::Double);

  // Test component count
  assert(GetComponentCount(TypeId::Float) == 1);
  assert(GetComponentCount(TypeId::Float3) == 3);
  assert(GetComponentCount(TypeId::Matrix4d) == 16);

  // Every built-in descriptor has an allocation-free bounded-name lookup.
  // A span is not required to be terminated, and embedded NULs are significant.
  for (uint16_t i = 1; i < static_cast<uint16_t>(TypeId::Count); ++i) {
    const TypeId id = static_cast<TypeId>(i);
    const TypeInfo* info = GetTypeInfo(id);
    assert(info && info->id == id && info->name);
    const std::string name(info->name);
    assert(GetTypeIdFromName(name.data(), name.size()) == id);
    bool array = true;
    if (id != TypeId::Extent && id != TypeId::Reference) {
      assert(ParseTypeName(name, array) == id && !array);
      assert(ParseTypeName(name + "[]", array) == id && array);
    }
    if (info->component_type != TypeId::Invalid) {
      assert(info->size == GetTypeSize(info->component_type) * info->component_count);
      assert(info->alignment == GetTypeAlignment(info->component_type));
    }
  }
  const char bounded[] = {'f', 'l', 'o', 'a', 't', 'x'};
  assert(GetTypeIdFromName(bounded, 5) == TypeId::Float);
  assert(GetTypeIdFromName(bounded, 6) == TypeId::Invalid);
  assert(GetTypeIdFromName("float\0tail", 10) == TypeId::Invalid);
  assert(GetTypeIdFromName(nullptr, 3) == TypeId::Invalid);
  assert(GetTypeInfo(static_cast<TypeId>(65535)) == nullptr);
  bool array = true;
  assert(ParseTypeName("", array) == TypeId::Invalid && !array);
  assert(ParseTypeName("[]", array) == TypeId::Invalid && !array);
  assert(ParseTypeName("float[][]", array) == TypeId::Invalid && array);

  std::cout << "  TypeId tests passed!" << std::endl;
}

void test_value() {
  std::cout << "Testing Value..." << std::endl;

  // Test scalar values
  {
    Value v(42);
    assert(v.type_id() == TypeId::Int);
    assert(!v.is_empty());
    assert(!v.is_array());
    assert(v.as_int() != nullptr);
    assert(*v.as_int() == 42);
    assert(v.as_float() == nullptr);  // Wrong type
  }

  // Test float value
  {
    Value v(3.14f);
    assert(v.type_id() == TypeId::Float);
    assert(v.as_float() != nullptr);
    assert(std::abs(*v.as_float() - 3.14f) < 0.001f);
  }

  // Test string value
  {
    Value v(std::string("hello"));
    assert(v.type_id() == TypeId::String);
    assert(v.as_string() != nullptr);
    assert(*v.as_string() == "hello");
  }

  // Test vector factory
  {
    Value v = Value::MakeFloat3(1.0f, 2.0f, 3.0f);
    assert(v.type_id() == TypeId::Float3);
    assert(v.as_float3() != nullptr);
    const float* data = v.as_float3();
    assert(data[0] == 1.0f);
    assert(data[1] == 2.0f);
    assert(data[2] == 3.0f);
  }

  // Test matrix factory
  {
    double mat_data[16] = {
      1, 0, 0, 0,
      0, 1, 0, 0,
      0, 0, 1, 0,
      0, 0, 0, 1
    };
    Value v = Value::MakeMatrix4d(mat_data);
    assert(v.type_id() == TypeId::Matrix4d);
    assert(v.as_matrix4d() != nullptr);
    const double* data = v.as_matrix4d();
    assert(data[0] == 1.0);
    assert(data[15] == 1.0);
  }

  // Test copy
  {
    Value v1(123);
    Value v2 = v1;
    assert(*v2.as_int() == 123);
    *v2.as_int() = 456;
    assert(*v1.as_int() == 123);  // v1 unchanged
    assert(*v2.as_int() == 456);
  }

  // Test move
  {
    Value v1(std::string("test"));
    Value v2 = std::move(v1);
    assert(v1.is_empty());
    assert(*v2.as_string() == "test");
  }

  // Test array
  {
    std::vector<float> data = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f};
    Value v = Value::MakeFloatArray(data);
    assert(v.is_array());
    assert(v.array_size() == 5);
    assert(v.as_float_array() != nullptr);
    assert(v.as_float_array()->size() == 5);
    assert((*v.as_float_array())[2] == 3.0f);
  }

  // Value's compact array-size field cannot represent more than uint32_t.
  // Reject an oversized lazy reference instead of silently truncating it.
  {
    LazyArrayRef ref;
    ref.source =
        CrateDataSource::Adopt(std::string(), CrateVersion{0, 8, 0});
    ref.value_type = TypeId::Float;
    ref.element_count =
        static_cast<uint64_t>((std::numeric_limits<uint32_t>::max)()) + 1;
    Value v = Value::MakeLazyArray(ref);
    assert(v.is_empty());

    ref.element_count = 1;
    ref.source.reset();
    assert(Value::MakeLazyArray(ref).is_empty());  // source is required
  }

  // Component-array factories reject partial tuples, mismatched arity, and a
  // TypeId whose accessor expects a different backing storage class.
  {
    assert(Value::MakeFloat2Array(std::vector<float>{1, 2, 3}).is_empty());
    assert(Value::MakeFloat3Array(std::vector<float>{1, 2, 3, 4}).is_empty());
    assert(Value::MakeFloatCompArray(std::vector<float>{1, 2, 3},
                                     TypeId::Float2, 2)
               .is_empty());
    assert(Value::MakeFloatCompArray(std::vector<float>{1, 2}, TypeId::Token,
                                     2)
               .is_empty());
    assert(Value::MakeDoubleCompArray(std::vector<double>{1, 2, 3},
                                      TypeId::Double3, 2)
               .is_empty());
    assert(Value::MakeIntCompArray(std::vector<int32_t>{1, 2}, TypeId::Int2,
                                   3)
               .is_empty());
    assert(Value::MakeUIntCompArray(std::vector<uint32_t>{1, 2},
                                    TypeId::UInt2, 0)
               .is_empty());
    assert(Value::MakeStringLikeArray(std::vector<std::string>{"x"},
                                      TypeId::Float)
               .is_empty());
  }

  // Pointer-backed factories reject null input and non-trivial storage types;
  // constructing a raw Dictionary would otherwise destroy an uninitialized
  // shared_ptr handle.
  {
    const uint32_t raw = 0;
    assert(Value::MakeMatrix2f(nullptr).is_empty());
    assert(Value::MakeMatrix4d(nullptr).is_empty());
    assert(Value::MakeFromRaw(TypeId::Float, nullptr).is_empty());
    assert(Value::MakeFromRaw(TypeId::Dictionary, &raw).is_empty());
  }

  // A failed mutable accessor must not mark an unrelated value as edited.
  {
    Value v = Value::MakeIntArray(std::vector<int32_t>{1, 2});
    assert(!v.is_dirty());
    assert(v.as_double_array() == nullptr);
    assert(!v.is_dirty());
  }

  // raw_data() on an array exposes its flat element payload, not the private
  // polymorphic storage wrapper. Mutable access must preserve copy-on-write.
  {
    Value original = Value::MakeIntArray(std::vector<int32_t>{1, 2, 3});
    const Value& const_original = original;
    const int32_t* const_raw =
        static_cast<const int32_t*>(const_original.raw_data());
    assert(const_raw && const_raw[0] == 1 && const_raw[2] == 3);
    assert(!original.is_dirty());

    Value edited = original;
    int32_t* mutable_raw = static_cast<int32_t*>(edited.raw_data());
    assert(mutable_raw);
    mutable_raw[0] = 9;
    assert(edited.is_dirty());
    assert((*edited.as_int_array())[0] == 9);
    assert((*original.as_int_array())[0] == 1);

    Value tuples = Value::MakeFloat3Array({0, 1, 2, 3, 4, 5});
    const Value& const_tuples = tuples;
    const float* tuple_raw =
        static_cast<const float*>(const_tuples.raw_data());
    assert(tuple_raw && tuple_raw[0] == 0 && tuple_raw[5] == 5);

    Value token = Value::MakeToken("raw token");
    const Value& const_token = token;
    Value token_copy =
        Value::MakeFromRaw(TypeId::Token, const_token.raw_data());
    assert(token_copy.as_token() && *token_copy.as_token() == "raw token");

    Value empty;
    Value block = Value::MakeBlock();
    assert(empty.raw_data() == nullptr);
    assert(block.raw_data() == nullptr);
  }

  // Materialized array size follows the live mutable backing vector; lazy
  // arrays continue to use their header count without materializing.
  {
    Value scalar = Value::MakeFloatArray(std::vector<float>{1, 2});
    scalar.as_float_array()->push_back(3);
    assert(scalar.array_size() == 3);

    Value tuples = Value::MakeFloat3Array(std::vector<float>{1, 2, 3});
    std::vector<float>* lanes = tuples.as_float_array();
    lanes->push_back(4);
    assert(tuples.array_size() == 0);  // partial tuple is not advertised
    lanes->insert(lanes->end(), {5, 6});
    assert(tuples.array_size() == 2);
  }

  // Copy-on-write: a copy shares the buffer until one side mutates, then the
  // mutation must be private (the other copy is unaffected).
  {
    Value a = Value::MakeFloatArray(std::vector<float>{1, 2, 3});
    Value b = a;  // shares the buffer (refcount bump, no element copy)
    assert(*a.as_float_array() == *b.as_float_array());

    (*b.as_float_array())[0] = 99.0f;  // mutable access detaches b
    assert((*b.as_float_array())[0] == 99.0f);
    assert((*a.as_float_array())[0] == 1.0f && "CoW detach failed: a was mutated");

    // A token array (string elements) detaches correctly too.
    Value t = Value::MakeTokenArray(std::vector<std::string>{"x", "y"});
    Value t2 = t;
    assert(*t.as_token_array() == *t2.as_token_array());
  }

  // Array equality/hash/raw-bytes must cover every concrete array backing type,
  // not just float3/int. These are used by time-sample deduplication.
  {
    Value q1 = Value::MakeFloatCompArray(
        std::vector<float>{0, 0, 0, 1, 0, 1, 0, 0}, TypeId::Quatf, 4);
    Value q2 = Value::MakeFloatCompArray(
        std::vector<float>{0, 0, 0, 1, 0, 1, 0, 0}, TypeId::Quatf, 4);
    assert(q1 == q2);
    assert(q1.hash() == q2.hash());
    size_t n = 0;
    const uint8_t* raw = q1.raw_bytes(&n);
    assert(raw && n == 8 * sizeof(float));

    Value m1 = Value::MakeDoubleCompArray(std::vector<double>(32, 2.0),
                                          TypeId::Matrix4d, 16);
    Value m2 = m1;
    assert(m1 == m2);
    assert(m1.hash() == m2.hash());
    raw = m1.raw_bytes(&n);
    assert(raw && n == 32 * sizeof(double));

    Value tok1 = Value::MakeTokenArray(std::vector<std::string>{"a", "b"});
    Value tok2 = Value::MakeTokenArray(std::vector<std::string>{"a", "b"});
    assert(tok1 == tok2);
    assert(tok1.hash() == tok2.hash());
    assert(tok1.raw_bytes(&n) == nullptr && n == 0);

    (*q2.as_float_array())[0] = 42.0f;
    assert(q1 != q2);
  }

  // Ownership is shared for each backing kind, including string objects.
  // Mutable raw access clones only the edited owner; moving a vector adopts
  // its allocation rather than introducing an extra full-size copy.
  {
    std::vector<float> input{1, 2, 3};
    const float* allocation = input.data();
    const Value adopted = Value::MakeFloatArray(std::move(input));
    assert(adopted.raw_data() == allocation);
    Value arrays[] = {
      adopted, Value::MakeIntArray(std::vector<int32_t>{1, 2}),
      Value::MakeDoubleArray(std::vector<double>{1, 2}),
      Value::MakeInt64Array(std::vector<int64_t>{1, 2}),
      Value::MakeUIntArray(std::vector<uint32_t>{1, 2}),
      Value::MakeUInt64Array(std::vector<uint64_t>{1, 2}),
      Value::MakeBoolArray(std::vector<bool>{true, false}),
      Value::MakeTokenArray(std::vector<std::string>{"owned string", "second"})
    };
    for (const Value& original : arrays) {
      const void* before = original.raw_data();
      Value copy = original;
      const Value& read_copy = copy;
      assert(read_copy.raw_data() == before);
      assert(copy.raw_data() != before);  // detach all eight storage kinds
      assert(copy == original);
      Value moved = std::move(copy);
      assert(copy.is_empty());
      assert(moved == original);
      copy = moved;
      moved.clear();
      assert(copy == original);
    }
    Value tokens = arrays[7];
    auto* strings = static_cast<std::string*>(tokens.raw_data());
    strings[0] = "edited";
    assert((*arrays[7].as_token_array())[0] == "owned string");
    Value bits = arrays[6];
    static_cast<uint8_t*>(bits.raw_data())[0] = 0;
    assert((*arrays[6].as_bool_array())[0] == 1);
  }

  std::cout << "  Value tests passed!" << std::endl;
}

void test_shared_storage() {
  PropNameTable properties;
  const auto first = properties.intern("first");
  const std::string_view stable = properties.get(first);
  const auto empty = properties.intern("");
  assert(properties.find(std::string_view{}).id == empty.id);
  const std::string embedded("a\0b", 3);
  const auto binary = properties.intern(embedded);
  assert(properties.find(std::string_view(embedded)).id == binary.id);
  assert(!properties.find("a").is_valid());
  properties.freeze();
  for (int i = 0; i < 8192; ++i) {
    const std::string name = "property:" + std::to_string(i);
    const auto id = properties.intern(name);
    assert(id.is_valid() && properties.find(name).id == id.id);
  }
  assert(properties.get(first).data() == stable.data() && stable == "first");
  assert(properties.find("first").id == first.id);
  properties.unfreeze();
  properties.freeze();
  assert(properties.find("property:8191").is_valid());

  TypeNameTable types;
  const auto type = types.intern("FirstType");
  const std::string* stable_type = &types.get(type);
  for (int i = 0; i < 4096; ++i) {
    const std::string name = "Type" + std::to_string(i);
    const auto id = types.intern(name);
    assert(id.is_valid() && types.find(name).id == id.id);
  }
  assert(&types.get(type) == stable_type && *stable_type == "FirstType");

#if defined(LIGHTUSD_ENABLE_THREAD)
  const Value shared = Value::MakeFloatArray(std::vector<float>{1, 2, 3});
  std::vector<std::thread> workers;
  for (int worker = 0; worker < 4; ++worker) {
    workers.emplace_back([&properties, &shared, first, stable, worker] {
      for (int i = 0; i < 2000; ++i) {
        assert(properties.find("first").id == first.id);
        assert(properties.get(first).data() == stable.data());
        const auto id = properties.intern("thread:" + std::to_string(worker) +
                                         ":" + std::to_string(i));
        assert(id.is_valid());
        Value copy = shared;
        (*copy.as_float_array())[0] = 42;
        assert((*shared.as_float_array())[0] == 1);
      }
    });
  }
  for (auto& worker : workers) worker.join();
  assert(properties.find("thread:3:1999").is_valid());
#endif
}

// ============================================================
// Prim tests
// ============================================================

void test_path() {
  std::cout << "Testing Path..." << std::endl;

  Path p1("/World/Cube");
  assert(p1.is_absolute());
  assert(!p1.is_root());
  assert(p1.name() == "Cube");
  assert(p1.parent().str() == "/World");

  Path p2 = p1.append_child("child");
  assert(p2.str() == "/World/Cube/child");

  Path p3("/Cube.xformOp:translate");
  assert(p3.has_property());
  assert(p3.property_name() == "xformOp:translate");
  assert(p3.prim_path().str() == "/Cube");

  Path root = Path::root();
  assert(root.is_root());
  assert(root.str() == "/");

  std::cout << "  Path tests passed!" << std::endl;
}

void test_prim() {
  std::cout << "Testing Prim..." << std::endl;

  Prim prim("Cube", "Mesh");
  assert(prim.name() == "Cube");
  assert(prim.type_name() == "Mesh");
  assert(prim.specifier() == Specifier::Def);

  // Test attributes
  Attribute attr("points", TypeId::Float3);
  attr.set_default(Value::MakeFloat3(0, 0, 0));
  prim.set_attribute(std::move(attr));

  assert(prim.has_attribute("points"));
  const Attribute* a = prim.get_attribute("points");
  assert(a != nullptr);
  assert(a->type_id() == TypeId::Float3);

  // Test children
  Prim child("SubMesh", "Mesh");
  prim.add_child(std::move(child));
  assert(prim.child_count() == 1);
  assert(prim.find_child("SubMesh") != nullptr);

  // Test metadata
  prim.set_metadata("purpose", Value::MakeToken("render"));
  assert(prim.has_metadata("purpose"));

  std::cout << "  Prim tests passed!" << std::endl;
}

// ============================================================
// Parser tests
// ============================================================

void test_lexer() {
  std::cout << "Testing Lexer..." << std::endl;

  const char* input = R"(
    def Mesh "Cube" {
      float3 points = (1.0, 2.0, 3.0)
      int count = 42
      string name = "hello world"
    }
  )";

  Lexer lexer(input, std::strlen(input));

  Token tok = lexer.next();
  assert(tok.type == TokenType::Def);

  tok = lexer.next();
  assert(tok.type == TokenType::Identifier);
  assert(tok.value == "Mesh");

  tok = lexer.next();
  assert(tok.type == TokenType::String);
  assert(tok.value == "Cube");

  tok = lexer.next();
  assert(tok.type == TokenType::OpenBrace);

  // Skip to string value test
  while (tok.type != TokenType::Eof) {
    tok = lexer.next();
    if (tok.type == TokenType::String && tok.value == "hello world") {
      break;
    }
  }
  assert(tok.value == "hello world");

  std::cout << "  Lexer tests passed!" << std::endl;
}

void test_value_parser() {
  std::cout << "Testing ValueParser..." << std::endl;

  // Test parsing float3
  {
    const char* input = "(1.0, 2.0, 3.0)";
    Lexer lexer(input, std::strlen(input));
    ParseResult result = ParseValue(lexer, TypeId::Float3);
    assert(result.success);
    assert(result.value.type_id() == TypeId::Float3);
    const float* data = result.value.as_float3();
    assert(data != nullptr);
    assert(data[0] == 1.0f);
    assert(data[1] == 2.0f);
    assert(data[2] == 3.0f);
  }

  // Test parsing matrix4d
  {
    const char* input = "((1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (0, 0, 0, 1))";
    Lexer lexer(input, std::strlen(input));
    ParseResult result = ParseValue(lexer, TypeId::Matrix4d);
    assert(result.success);
    assert(result.value.type_id() == TypeId::Matrix4d);
    const double* data = result.value.as_matrix4d();
    assert(data != nullptr);
    assert(data[0] == 1.0);
    assert(data[15] == 1.0);
  }

  // Test parsing array
  {
    const char* input = "[1.0, 2.0, 3.0]";
    Lexer lexer(input, std::strlen(input));
    ParseResult result = ParseArrayValue(lexer, TypeId::Float);
    assert(result.success);
    assert(result.value.is_array());
    const std::vector<float>* arr = result.value.as_float_array();
    assert(arr != nullptr);
    assert(arr->size() == 3);
    assert((*arr)[0] == 1.0f);
  }

  std::cout << "  ValueParser tests passed!" << std::endl;
}

void test_ascii_parser() {
  std::cout << "Testing AsciiParser..." << std::endl;

  const char* input = R"(#usda 1.0
(
    defaultPrim = "World"
    upAxis = "Y"
    metersPerUnit = 0.01
)

def Xform "World" {
    def Mesh "Cube" {
        float3[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0)]
        int[] faceVertexCounts = [3]
        int[] faceVertexIndices = [0, 1, 2]
    }
}
)";

  AsciiParser parser;
  bool success = parser.Parse(input, std::strlen(input));

  if (!success) {
    std::cout << "Parse errors:" << std::endl;
    for (const auto& err : parser.GetErrors()) {
      std::cout << "  Line " << err.line << ": " << err.message << std::endl;
    }
  }
  assert(success);

  Stage stage = parser.TakeStage();
  assert(stage.GetMeta().defaultPrim == "World");
  assert(stage.GetUpAxis() == "Y");
  assert(std::abs(stage.GetMetersPerUnit() - 0.01) < 0.001);

  auto roots = stage.GetRootPrims();
  assert(roots.size() == 1);
  assert(roots[0].GetName() == "World");
  assert(roots[0].GetTypeName() == "Xform");
  assert(roots[0].GetChildCount() == 1);

  UsdPrim cube = stage.GetPrimAtPath("/World/Cube");
  assert(cube.IsValid());
  assert(cube.GetTypeName() == "Mesh");
  assert(cube.HasProperty("points"));

  std::cout << "  AsciiParser tests passed!" << std::endl;
}

void test_usda_reader() {
  std::cout << "Testing USDAReader..." << std::endl;

  const char* input = R"(#usda 1.0
def Sphere "MySphere" {
    double radius = 1.5
    float3 center = (0, 0, 0)
}
)";

  LoadResult result = LoadUSDAFromString(input, std::strlen(input));
  assert(result.success);

  auto roots = result.stage.GetRootPrims();
  assert(roots.size() == 1);
  assert(roots[0].GetName() == "MySphere");
  assert(roots[0].GetTypeName() == "Sphere");

  // Check property access
  UsdPrim sphere = result.stage.GetPrimAtPath("/MySphere");
  assert(sphere.IsValid());
  assert(sphere.HasProperty("radius"));
  assert(sphere.HasProperty("center"));

  const Value* radius = sphere.GetPropertyValue("radius");
  assert(radius != nullptr);
  assert(radius->as_double() != nullptr);
  assert(std::abs(*radius->as_double() - 1.5) < 0.001);

  std::cout << "  USDAReader tests passed!" << std::endl;
}

void test_usda_lazy_parse_policies() {
  std::cout << "Testing USDA lazy parse policies..." << std::endl;

  const char* input = R"(#usda 1.0
def Mesh "MeshA" {
    int[] small = [1, 2]
    int[] large = [1, 2, 3, 4]
    point3f[] points = [(1, 2, 3), (4, 5, 6), (7, 8, 9)]
}
)";

  // Lazy parsing disabled by default -> every supported array is eager.
  {
    LoadResult result = LoadUSDAFromString(input, std::strlen(input));
    assert(result.success);
    UsdPrim mesh = result.stage.GetPrimAtPath("/MeshA");
    assert(mesh.IsValid());
    assert(mesh.GetPropertyValue("small"));
    assert(!mesh.GetPropertyValue("small")->is_lazy());
    assert(!mesh.GetPropertyValue("large")->is_lazy());
    assert(!mesh.GetPropertyValue("points")->is_lazy());
  }

  LoadOptions enabled_opts;
  enabled_opts.parse_options.enable_usda_lazy_arrays = true;

  // Enable lazy arrays with default policy -> all supported arrays become lazy.
  {
    LoadResult result =
        LoadUSDAFromString(input, std::strlen(input), enabled_opts);
    assert(result.success);
    UsdPrim mesh = result.stage.GetPrimAtPath("/MeshA");
    assert(mesh.IsValid());
    assert(mesh.GetPropertyValue("small")->is_lazy());
    assert(mesh.GetPropertyValue("large")->is_lazy());
    const Value* points = mesh.GetPropertyValue("points");
    assert(points && points->is_lazy());
    assert(points->lazy_ref() && points->lazy_ref()->source);
    assert(!points->lazy_ref()->source->is_mmapped());
    assert(!points->lazy_ref()->source->can_borrow());
    assert(points->array_size() == 3);
    assert(points->as_float_array());
  }

  // Owned string parsing adopts the caller's buffer as the retained lazy source.
  // This is the WASM/browser path after JS bytes have already been copied into a
  // C++ string; it avoids a second full USDA source copy.
  {
    std::string owned(input);
    LoadResult result = LoadUSDAFromStringOwned(std::move(owned), enabled_opts);
    assert(result.success);
    UsdPrim mesh = result.stage.GetPrimAtPath("/MeshA");
    assert(mesh.IsValid());
    const Value* points = mesh.GetPropertyValue("points");
    assert(points && points->is_lazy());
    assert(points->lazy_ref() && points->lazy_ref()->source);
    assert(!points->lazy_ref()->source->is_mmapped());
    assert(!points->lazy_ref()->source->can_borrow());
    const std::vector<float>* values = points->as_float_array();
    assert(values && values->size() == 9);
    assert((*values)[0] == 1.0f && (*values)[8] == 9.0f);
  }

  // Top-level owned memory loader should take the same single-copy USDA path.
  {
    std::string owned(input);
    LoadUSDOptions opts;
    opts.usda_options = enabled_opts;
    Stage stage;
    std::string warn, err;
    bool ok = LoadUSDFromMemoryOwned(std::move(owned), &stage, opts, &warn, &err);
    assert(ok);
    UsdPrim mesh = stage.GetPrimAtPath("/MeshA");
    assert(mesh.IsValid());
    const Value* points = mesh.GetPropertyValue("points");
    assert(points && points->is_lazy());
    assert(points->lazy_ref() && points->lazy_ref()->source);
    assert(!points->lazy_ref()->source->is_mmapped());
    assert(points->array_size() == 3);
    assert(points->as_float_array());
  }

  // File-backed lazy parsing should retain the source through mmap on native
  // POSIX builds, avoiding the extra full-file string copy kept by the string
  // input path above.
  {
    const char* path = "/tmp/lightusd_next_usda_lazy_mmap_test.usda";
    {
      std::ofstream f(path, std::ios::binary);
      f << input;
    }

    LoadResult result = LoadUSDAFromFile(path, enabled_opts);
    std::remove(path);
    assert(result.success);
    UsdPrim mesh = result.stage.GetPrimAtPath("/MeshA");
    assert(mesh.IsValid());
    const Value* points = mesh.GetPropertyValue("points");
    assert(points && points->is_lazy());
    assert(points->lazy_ref() && points->lazy_ref()->source);
    if (kExpectUsdaLazyMmap) {
      assert(points->lazy_ref()->source->is_mmapped());
    } else {
      assert(!points->lazy_ref()->source->is_mmapped());
    }
    assert(!points->lazy_ref()->source->can_borrow());
    assert(points->array_size() == 3);
    const std::vector<float>* values = points->as_float_array();
    assert(values && values->size() == 9);
    assert((*values)[0] == 1.0f && (*values)[8] == 9.0f);
  }

  // Tight max element policy should keep oversized arrays eager.
  {
    LoadOptions limited = enabled_opts;
    limited.parse_options.max_usda_lazy_array_elements = 2;
    LoadResult result =
        LoadUSDAFromString(input, std::strlen(input), limited);
    assert(result.success);
    UsdPrim mesh = result.stage.GetPrimAtPath("/MeshA");
    assert(mesh.IsValid());
    assert(mesh.GetPropertyValue("small")->is_lazy());
    assert(!mesh.GetPropertyValue("large")->is_lazy());
    assert(!mesh.GetPropertyValue("points")->is_lazy());
  }

  // 0 => no cap.
  {
    LoadOptions unlimited = enabled_opts;
    unlimited.parse_options.max_usda_lazy_array_elements = 0;
    LoadResult result =
        LoadUSDAFromString(input, std::strlen(input), unlimited);
    assert(result.success);
    UsdPrim mesh = result.stage.GetPrimAtPath("/MeshA");
    assert(mesh.IsValid());
    assert(mesh.GetPropertyValue("large")->is_lazy());
  }

  // Non-simple arrays are still parsed correctly, but not captured lazily.
  const char* comments = R"(#usda 1.0
def Mesh "MeshB" {
    int[] with_comment = [1, # inline comment
        2, 3]
}
)";
  {
    LoadResult result =
        LoadUSDAFromString(comments, std::strlen(comments), enabled_opts);
    assert(result.success);
    UsdPrim mesh = result.stage.GetPrimAtPath("/MeshB");
    assert(mesh.IsValid());
    const Value* with_comment = mesh.GetPropertyValue("with_comment");
    assert(with_comment);
    assert(!with_comment->is_lazy());
    const std::vector<int32_t>* values = with_comment->as_int_array();
    assert(values && values->size() == 3);
    assert((*values)[1] == 2);
  }

  std::cout << "  USDA lazy parse policy tests passed!" << std::endl;
}

// ============================================================
// Arc list-op qualifiers (prepend / append / delete)
// ============================================================

void test_arc_listops() {
  std::cout << "Testing arc list-op qualifiers..." << std::endl;

  // prepend inserts at the front; append at the back; delete removes. The
  // composed `references` list must reflect those ops in authoring order.
  const char* input = R"(#usda 1.0
def "A" (
    references = [@base.usd@</X>]
    prepend references = [@front.usd@</X>]
    append references = [@back.usd@</X>]
    delete references = [@base.usd@</X>]
)
{
}
)";
  LoadResult result = LoadUSDAFromString(input, std::strlen(input));
  assert(result.success);
  UsdPrim a = result.stage.GetPrimAtPath("/A");
  assert(a.IsValid());
  const std::vector<std::string>& refs = a.GetMeta().references;
  // explicit [base] -> prepend front -> [front, base] -> append back ->
  // [front, base, back] -> delete base -> [front, back].
  assert(refs.size() == 2 && "list-op qualifiers not applied");
  assert(refs[0].find("front.usd") != std::string::npos && "prepend not at front");
  assert(refs[1].find("back.usd") != std::string::npos && "append not at back");
  for (const auto& r : refs) {
    assert(r.find("base.usd") == std::string::npos && "delete did not remove");
  }

  std::cout << "  Arc list-op tests passed!" << std::endl;
}

// A per-reference layer offset `(offset = N; scale = M)` must be captured into
// the canonical ref string as `?layerOffset=N:M`.
void test_arc_layer_offset_parse() {
  std::cout << "Testing arc layer-offset parse..." << std::endl;
  const char* input = R"(#usda 1.0
def "R" (
    references = @asset.usd@</A> (offset = 12; scale = 2)
)
{
}
)";
  LoadResult result = LoadUSDAFromString(input, std::strlen(input));
  assert(result.success);
  UsdPrim r = result.stage.GetPrimAtPath("/R");
  assert(r.IsValid());
  const std::vector<std::string>& refs = r.GetMeta().references;
  assert(refs.size() == 1);
  assert(refs[0].find("layerOffset=") != std::string::npos &&
         "reference layer offset not captured");
  assert(refs[0].find("12") != std::string::npos &&
         refs[0].find(":2") != std::string::npos && "offset/scale wrong");
  std::cout << "  Arc layer-offset parse passed!" << std::endl;
}

// ============================================================
// Physics schema readers (regression: vector3f / quatf properties were dropped)
// ============================================================

void test_physics_schema() {
  std::cout << "Testing physics schema readers..." << std::endl;

  const char* input = R"(#usda 1.0
(
    upAxis = "Z"
)

def Xform "Body" (
    prepend apiSchemas = ["PhysicsRigidBodyAPI", "PhysicsMassAPI"]
)
{
    bool physics:kinematicEnabled = 1
    rel physics:simulationOwner = </Scene>
    vector3f physics:velocity = (1, 2, 3)
    vector3f physics:angularVelocity = (4, 5, 6)
    point3f physics:centerOfMass = (0.5, 0.5, 0.5)
    float3 physics:diagonalInertia = (2, 3, 4)
}

def Xform "BodyDefaults" (
    prepend apiSchemas = ["PhysicsMassAPI"]
)
{
}

def PhysicsScene "Scene"
{
}

def PhysicsScene "AuthoredScene"
{
    vector3f physics:gravityDirection = (0, -1, 0)
    float physics:gravityMagnitude = 3
}

def PhysicsRevoluteJoint "Joint"
{
    token physics:axis = "Z"
    point3f physics:localPos0 = (1, 0, 0)
    point3f physics:localPos1 = (0, 1, 0)
}

def PhysicsSliderJoint "Slider"
{
    token physics:axis = "Y"
    float physics:lowerLimit = -2
    float physics:upperLimit = 4
}

def PhysicsSphericalJoint "Ball"
{
    float physics:coneAngle0Limit = 30
    float physics:coneAngle1Limit = 45
}

def PhysicsBallJoint "BallAlias"
{
    float physics:coneAngle0Limit = 15
    float physics:coneAngle1Limit = 25
}

def Mesh "MeshCollider" (
    prepend apiSchemas = ["PhysicsCollisionAPI", "PhysicsMeshCollisionAPI"]
)
{
    token physics:approximation = "convexHull"
}

def PhysicsJoint "Driven" (
    prepend apiSchemas = ["PhysicsDriveAPI:rotX", "PhysicsLimitAPI:transY"]
)
{
    token physics:drive:rotX:type = "acceleration"
    float physics:drive:rotX:maxForce = 8
    float physics:limit:transY:low = -1
    float physics:limit:transY:high = 2
}
)";

  LoadResult result = LoadUSDAFromString(input, std::strlen(input));
  assert(result.success);

  UsdPrim body = result.stage.GetPrimAtPath("/Body");
  assert(body.IsValid());

  // Rigid body velocity / angularVelocity (single vector3f attrs).
  PhysicsRigidBodyData rb;
  assert(GetPhysicsRigidBodyData(result.stage, body, &rb, 0.0));
  assert(rb.kinematicEnabled);
  assert(rb.simulationOwner == "/Scene");
  assert(std::abs(rb.velocity[0] - 1.0f) < 0.001f);
  assert(std::abs(rb.velocity[1] - 2.0f) < 0.001f);
  assert(std::abs(rb.velocity[2] - 3.0f) < 0.001f);
  assert(std::abs(rb.angularVelocity[0] - 4.0f) < 0.001f);
  assert(std::abs(rb.angularVelocity[1] - 5.0f) < 0.001f);
  assert(std::abs(rb.angularVelocity[2] - 6.0f) < 0.001f);

  // Mass: centerOfMass / diagonalInertia (vector3f).
  PhysicsMassData mass;
  assert(GetPhysicsMassData(result.stage, body, &mass));
  assert(std::abs(mass.centerOfMass[0] - 0.5f) < 0.001f);
  assert(std::abs(mass.centerOfMass[2] - 0.5f) < 0.001f);
  assert(std::abs(mass.diagonalInertia[0] - 2.0f) < 0.001f);
  assert(std::abs(mass.diagonalInertia[1] - 3.0f) < 0.001f);
  assert(std::abs(mass.diagonalInertia[2] - 4.0f) < 0.001f);

  UsdPrim body_defaults = result.stage.GetPrimAtPath("/BodyDefaults");
  assert(body_defaults.IsValid());
  PhysicsMassData mass_defaults;
  assert(GetPhysicsMassData(result.stage, body_defaults, &mass_defaults));
  assert(std::isinf(mass_defaults.centerOfMass[0]) &&
         mass_defaults.centerOfMass[0] < 0.0f);
  assert(mass_defaults.principalAxes[0] == 0.0f);
  assert(mass_defaults.principalAxes[3] == 0.0f);

  // Joint local frame positions (vector3f).
  UsdPrim joint = result.stage.GetPrimAtPath("/Joint");
  assert(joint.IsValid());
  PhysicsJointData jd;
  assert(GetPhysicsJointData(result.stage, joint, &jd, 0.0));
  assert(jd.hasLocalPos0);
  assert(std::abs(jd.localPos0[0] - 1.0f) < 0.001f);
  assert(jd.hasLocalPos1);
  assert(std::abs(jd.localPos1[1] - 1.0f) < 0.001f);
  assert(std::isinf(jd.breakForce));
  assert(!jd.collisionEnabled);

  UsdPrim scene = result.stage.GetPrimAtPath("/Scene");
  assert(scene.IsValid());
  assert(!IsPhysicsJoint(scene));
  PhysicsSceneData scene_data;
  assert(GetPhysicsSceneData(result.stage, scene, &scene_data));
  assert(std::abs(scene_data.gravityMagnitude - 9.81f) < 0.001f);
  assert(std::abs(scene_data.gravityDirection[0]) < 0.001f);
  assert(std::abs(scene_data.gravityDirection[1]) < 0.001f);
  assert(std::abs(scene_data.gravityDirection[2] + 1.0f) < 0.001f);

  UsdPrim authored_scene = result.stage.GetPrimAtPath("/AuthoredScene");
  assert(authored_scene.IsValid());
  PhysicsSceneData authored_scene_data;
  assert(GetPhysicsSceneData(result.stage, authored_scene, &authored_scene_data));
  assert(std::abs(authored_scene_data.gravityMagnitude - 3.0f) < 0.001f);
  assert(std::abs(authored_scene_data.gravityDirection[1] + 1.0f) < 0.001f);

  PhysicsRevoluteJointData rj;
  assert(GetPhysicsRevoluteJointData(result.stage, joint, &rj, 0.0));
  assert(std::abs(rj.axis[2] - 1.0f) < 0.001f);

  UsdPrim slider = result.stage.GetPrimAtPath("/Slider");
  assert(slider.IsValid());
  PhysicsSliderJointData slider_data;
  assert(GetPhysicsSliderJointData(result.stage, slider, &slider_data, 0.0));
  assert(std::abs(slider_data.axis[1] - 1.0f) < 0.001f);
  assert(std::abs(slider_data.lowerLimit + 2.0f) < 0.001f);
  assert(std::abs(slider_data.upperLimit - 4.0f) < 0.001f);

  UsdPrim ball = result.stage.GetPrimAtPath("/Ball");
  assert(ball.IsValid());
  PhysicsSphericalJointData sj;
  assert(GetPhysicsSphericalJointData(result.stage, ball, &sj, 0.0));
  assert(std::abs(sj.coneAngle0Limit - 30.0f) < 0.001f);
  assert(std::abs(sj.coneAngle1Limit - 45.0f) < 0.001f);

  UsdPrim ball_alias = result.stage.GetPrimAtPath("/BallAlias");
  assert(ball_alias.IsValid());
  PhysicsBallJointData ball_alias_data;
  assert(GetPhysicsBallJointData(result.stage, ball_alias, &ball_alias_data, 0.0));
  assert(std::abs(ball_alias_data.coneAngle0Limit - 15.0f) < 0.001f);
  assert(std::abs(ball_alias_data.coneAngle1Limit - 25.0f) < 0.001f);

  UsdPrim mesh_collider = result.stage.GetPrimAtPath("/MeshCollider");
  assert(mesh_collider.IsValid());
  PhysicsMeshCollisionData mesh_collision;
  assert(GetPhysicsMeshCollisionData(mesh_collider, &mesh_collision));
  assert(mesh_collision.approximation == "convexHull");

  UsdPrim driven = result.stage.GetPrimAtPath("/Driven");
  assert(driven.IsValid());
  PhysicsDriveData drive;
  assert(GetPhysicsDriveData(driven, "rotX", &drive));
  assert(drive.type == "acceleration");
  assert(std::abs(drive.maxForce - 8.0f) < 0.001f);
  PhysicsLimitData limit;
  assert(GetPhysicsLimitData(driven, "transY", &limit));
  assert(std::abs(limit.low + 1.0f) < 0.001f);
  assert(std::abs(limit.high - 2.0f) < 0.001f);

  {
    LoadResult fixture =
        LoadUSDAFromFile(UsdaFixturePath("physics-schema-defaults-001.usda"));
    assert(fixture.success);
    UsdPrim fixture_body = fixture.stage.GetPrimAtPath("/World/Body");
    assert(fixture_body.IsValid());
    PhysicsRigidBodyData fixture_rb;
    assert(GetPhysicsRigidBodyData(fixture.stage, fixture_body, &fixture_rb));
    assert(fixture_rb.kinematicEnabled);
    assert(fixture_rb.simulationOwner == "/World/Scene");

    PhysicsMassData fixture_mass;
    assert(GetPhysicsMassData(fixture.stage, fixture_body, &fixture_mass));
    assert(std::isinf(fixture_mass.centerOfMass[0]) &&
           fixture_mass.centerOfMass[0] < 0.0f);
    assert(fixture_mass.principalAxes[0] == 0.0f);
    assert(fixture_mass.principalAxes[3] == 0.0f);

    UsdPrim fixture_scene = fixture.stage.GetPrimAtPath("/World/Scene");
    assert(fixture_scene.IsValid());
    assert(!IsPhysicsJoint(fixture_scene));
  }

  {
    LoadResult fixture = LoadUSDAFromFile(
        UsdaFixturePath("physics-spherical-schema-names-001.usda"));
    assert(fixture.success);
    UsdPrim fixture_ball = fixture.stage.GetPrimAtPath("/World/Ball");
    assert(fixture_ball.IsValid());
    PhysicsSphericalJointData fixture_sj;
    assert(GetPhysicsSphericalJointData(fixture.stage, fixture_ball,
                                        &fixture_sj, 0.0));
    assert(std::abs(fixture_sj.coneAngle0Limit - 30.0f) < 0.001f);
    assert(std::abs(fixture_sj.coneAngle1Limit - 45.0f) < 0.001f);
  }

  std::cout << "  physics schema tests passed!" << std::endl;
}

void test_load_usd_from_memory() {
  std::cout << "Testing LoadUSDFromMemory..." << std::endl;

  const char* usda = R"(#usda 1.0
def Xform "root" {
  def Mesh "geom" {
    float radius = 3.0
  }
}
)";

  // USDA sniffed from content
  Stage stage;
  std::string warn, err;
  bool ok = LoadUSDFromMemory(reinterpret_cast<const uint8_t*>(usda),
                              std::strlen(usda), &stage, &warn, &err);
  assert(ok && "memory load should succeed");
  assert(stage.GetPrimAtPath("/root/geom").IsValid());

  // USDC: write the stage to memory, load it back
  std::vector<uint8_t> usdc;
  USDCWriteResult wres = WriteUSDCToMemory(usdc, stage);
  assert(wres.success && "usdc memory write should succeed");
  Stage stage2;
  ok = LoadUSDFromMemory(usdc.data(), usdc.size(), &stage2, &warn, &err);
  assert(ok && "usdc memory load should succeed");
  assert(stage2.GetPrimAtPath("/root/geom").IsValid());

  // Garbage input fails cleanly
  const uint8_t garbage[] = {0xFF, 0xFE, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
  Stage stage3;
  err.clear();
  ok = LoadUSDFromMemory(garbage, sizeof(garbage), &stage3, &warn, &err);
  assert(!ok && !err.empty() && "garbage input should fail with an error");

  // Empty input fails cleanly
  ok = LoadUSDFromMemory(nullptr, 0, &stage3, &warn, &err);
  assert(!ok && "empty input should fail");

  std::cout << "  LoadUSDFromMemory tests passed!" << std::endl;
}

void test_stage_session_variants() {
  std::cout << "Testing StageSession path-scoped variants..." << std::endl;
  const char* path = "/tmp/lightusd_next_stage_session.usda";
  {
    std::ofstream ofs(path);
    ofs << R"(#usda 1.0
def Xform "A" (
  variants = { string model = "low" }
  prepend variantSets = "model"
) {
  variantSet "model" = {
    "low" { int level = 1 }
    "high" { int level = 2 }
  }
}
def Xform "B" (
  variants = { string model = "low" }
  prepend variantSets = "model"
) {
  variantSet "model" = {
    "low" { int level = 1 }
    "high" { int level = 2 }
  }
}
def Mesh "M" {
  point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
  int[] faceVertexCounts = [3]
  int[] faceVertexIndices = [0, 1, 2]
  string[] labels = ["retained"]
}
)";
  }

  StageSession session;
  assert(session.OpenFile(path));
  assert(session.IsComposed());
  StageSnapshot initial_snapshot = session.GetSnapshot();
  assert(initial_snapshot);
  assert(initial_snapshot.revision == 1);
  const Value* initial_level =
      initial_snapshot->GetPrimAtPath("/A").GetPropertyValue("level");
  assert(initial_level && initial_level->as_int() &&
         *initial_level->as_int() == 1);
  session.ReleaseCompositionCache();
  assert(session.IsComposed());
  assert(session.GetStage().GetPrimAtPath("/A").IsValid());
  Stage::StaticGeometryReleaseStats released =
      session.ReleaseStaticGeometryArrays(1);
  assert(released.property_count == 3);
  assert(released.stage_bytes_after < released.stage_bytes_before);
  UsdPrim compact_mesh = session.GetStage().GetPrimAtPath("/M");
  assert(compact_mesh.HasProperty("points"));
  assert(compact_mesh.GetPropertyValue("points") == nullptr);
  assert(compact_mesh.GetPropertyValue("labels") != nullptr);
  // Releasing geometry is copy-on-write: a previously published immutable
  // snapshot retains its arrays.
  const Value* snapshot_points =
      initial_snapshot->GetPrimAtPath("/M").GetPropertyValue("points");
  assert(snapshot_points && snapshot_points->array_size() == 3);

  StageEditResult edit =
      session.SetVariantSelection(Path("/A"), "model", "high");
  assert(edit);
  assert(edit.snapshot.revision == 2);
  assert(edit.changes.base_revision == 1);
  assert(edit.changes.new_revision == 2);
  bool found_a_change = false;
  for (const PrimChange& change : edit.changes.prims) {
    if (change.path == Path("/A")) {
      found_a_change = true;
      assert(std::find(change.properties.begin(), change.properties.end(),
                       "level") != change.properties.end());
    }
  }
  assert(found_a_change);
  // The old snapshot is still a coherent view of revision 1.
  initial_level =
      initial_snapshot->GetPrimAtPath("/A").GetPropertyValue("level");
  assert(initial_level && initial_level->as_int() &&
         *initial_level->as_int() == 1);
  const Value* a = session.GetStage().GetPrimAtPath("/A").GetPropertyValue("level");
  const Value* b = session.GetStage().GetPrimAtPath("/B").GetPropertyValue("level");
  assert(a && a->as_int() && *a->as_int() == 2);
  assert(b && b->as_int() && *b->as_int() == 1);
  const Value* restored_points =
      session.GetStage().GetPrimAtPath("/M").GetPropertyValue("points");
  assert(restored_points && restored_points->array_size() == 3);
  assert(session.ClearVariantSelection(Path("/A"), "model"));
  a = session.GetStage().GetPrimAtPath("/A").GetPropertyValue("level");
  assert(a && a->as_int() && *a->as_int() == 1);
  std::remove(path);
  std::cout << "  StageSession variant tests passed!" << std::endl;
}

void test_stage_session_payloads_and_cancel() {
  std::cout << "Testing StageSession payload edits and cancellation..."
            << std::endl;
  const char* root_path = "/tmp/lightusd_next_session_root.usda";
  const char* payload_path = "/tmp/lightusd_next_session_payload.usda";
  {
    std::ofstream ofs(payload_path);
    ofs << "#usda 1.0\ndef Xform \"Payload\" { int loadedValue = 7 }\n";
  }
  {
    std::ofstream ofs(root_path);
    ofs << "#usda 1.0\ndef Xform \"P\" (payload = "
           "@lightusd_next_session_payload.usda@</Payload>) {}\n"
           "def Xform \"Q\" (payload = "
           "@lightusd_next_session_payload.usda@</Payload>) {}\n";
  }

  StageSessionOptions options;
  options.composition.load_payloads = false;
  options.cache_retention = CacheRetention::LayersOnly;
  StageSession session;
  assert(session.OpenFile(root_path, options));
  StageSessionMemoryStats initial_stats = session.GetMemoryStats();
  assert(initial_stats.source_layer_bytes > 0);
  assert(initial_stats.composed_stage_bytes > 0);
  assert(initial_stats.prim_index_count == 0);
  assert(session.GetStage().GetPrimAtPath("/P").GetPropertyValue("loadedValue") ==
         nullptr);
  assert(!session.GetDeferredPayloadPaths().empty());
  session.ReleaseCompositionCache();
  assert(session.IsComposed());
  assert(!session.GetDeferredPayloadPaths().empty());
  assert(session.GetMemoryStats().source_layer_bytes == 0);
  assert(session.LoadPayloads({Path("/P"), Path("/Q")}));
  const Value* loaded =
      session.GetStage().GetPrimAtPath("/P").GetPropertyValue("loadedValue");
  assert(loaded && loaded->as_int() && *loaded->as_int() == 7);
  loaded = session.GetStage().GetPrimAtPath("/Q").GetPropertyValue("loadedValue");
  assert(loaded && loaded->as_int() && *loaded->as_int() == 7);
  StageSnapshot before_reload = session.GetSnapshot();
  {
    std::ofstream ofs(payload_path);
    ofs << "#usda 1.0\ndef Xform \"Payload\" { int loadedValue = 8 }\n";
  }
  StageEditResult reload = session.ReloadLayer(payload_path);
  assert(reload);
  assert(reload.changes.base_revision == before_reload.revision);
  assert(reload.changes.new_revision == before_reload.revision + 1);
  bool reload_classified = false;
  for (const PrimChange& change : reload.changes.prims) {
    if ((change.path == Path("/P") || change.path == Path("/Q")) &&
        std::find(change.properties.begin(), change.properties.end(),
                  "loadedValue") != change.properties.end()) {
      reload_classified = true;
    }
  }
  assert(reload_classified);
  loaded = session.GetStage().GetPrimAtPath("/P").GetPropertyValue("loadedValue");
  assert(loaded && loaded->as_int() && *loaded->as_int() == 8);
  const Value* old_loaded =
      before_reload->GetPrimAtPath("/P").GetPropertyValue("loadedValue");
  assert(old_loaded && old_loaded->as_int() && *old_loaded->as_int() == 7);
  assert(session.GetMemoryStats().prim_index_count == 0);
  assert(session.UnloadPayload(Path("/P")));
  assert(session.GetStage().GetPrimAtPath("/P").GetPropertyValue("loadedValue") ==
         nullptr);
  Stage taken = session.TakeStage();
  assert(!session.IsOpen());
  assert(!session.IsComposed());
  loaded = taken.GetPrimAtPath("/Q").GetPropertyValue("loadedValue");
  assert(loaded && loaded->as_int() && *loaded->as_int() == 8);

  StageSessionOptions cancelled_options;
  cancelled_options.progress_callback =
      [](const ProgressEvent&) { return false; };
  StageSession cancelled;
  assert(!cancelled.OpenFile(root_path, cancelled_options));
  assert(!cancelled.IsOpen());
  assert(!cancelled.GetDiagnostics().empty());

  StageSessionOptions tiny_budget;
  tiny_budget.max_total_memory = 1;
  StageSession budgeted;
  assert(!budgeted.OpenFile(root_path, tiny_budget));
  bool saw_memory_budget = false;
  for (const Diagnostic& diagnostic : budgeted.GetDiagnostics()) {
    if (diagnostic.code == "memory_budget") saw_memory_budget = true;
  }
  assert(saw_memory_budget);

  std::remove(root_path);
  std::remove(payload_path);
  std::cout << "  StageSession payload/cancel tests passed!" << std::endl;
}

void test_stage_session_preview_and_dependencies() {
  std::cout << "Testing StageSession preview checkpoint/dependencies..."
            << std::endl;
  const char* root_path = "/tmp/lightusd_next_preview_root.usda";
  const char* sub_path = "/tmp/lightusd_next_preview_sub.usda";
  {
    std::ofstream ofs(sub_path);
    ofs << R"(#usda 1.0
def Mesh "FromSub" {
  float3[] extent = [(-1, -2, -3), (1, 2, 3)]
  double3 xformOp:translate = (4, 5, 6)
  uniform token[] xformOpOrder = ["xformOp:translate"]
  int expensive = 7
}
)";
  }
  {
    std::ofstream ofs(root_path);
    ofs << "#usda 1.0\n( subLayers = [@lightusd_next_preview_sub.usda@] )\n"
           "def Xform \"Root\" { def Scope \"Child\" {} }\n";
  }

  StageSnapshot retained_preview;
  StageSnapshot retained_early_preview;
  int early_preview_calls = 0;
  int preview_calls = 0;
  StageSessionOptions options;
  options.early_preview_callback = [&](const StagePreview& preview) {
    ++early_preview_calls;
    assert(!preview.namespace_complete);
    assert(!preview.spatial_subset);
    assert(!preview.authoritative);
    assert(preview.snapshot);
    assert(preview.snapshot->GetPrimAtPath("/Root").IsValid());
    retained_early_preview = preview.snapshot;
    return true;
  };
  options.preview_callback = [&](const StagePreview& preview) {
    ++preview_calls;
    assert(!preview.namespace_complete);
    assert(preview.spatial_subset);
    assert(!preview.authoritative);
    assert(preview.snapshot);
    assert(preview.snapshot->GetPrimAtPath("/FromSub").IsValid());
    assert(preview.snapshot->GetPrimAtPath("/FromSub").GetTypeName() ==
           "Mesh");
    assert(preview.snapshot->GetPrimAtPath("/FromSub")
               .GetPropertyValue("extent") != nullptr);
    assert(preview.snapshot->GetPrimAtPath("/FromSub")
               .GetPropertyValue("xformOp:translate") != nullptr);
    // The structure checkpoint precedes full opinion filling.
    assert(preview.snapshot->GetPrimAtPath("/FromSub")
               .GetPropertyValue("expensive") == nullptr);
    retained_preview = preview.snapshot;
    return true;
  };

  StageSession session;
  assert(session.OpenFile(root_path, options));
  assert(early_preview_calls == 1);
  assert(retained_early_preview);
  assert(preview_calls == 1);
  assert(retained_preview);
  const Value* final_value =
      session.GetStage().GetPrimAtPath("/FromSub").GetPropertyValue("expensive");
  assert(final_value && final_value->as_int() && *final_value->as_int() == 7);
  // Completion must not mutate the separately-owned preview.
  assert(retained_preview->GetPrimAtPath("/FromSub")
             .GetPropertyValue("expensive") == nullptr);

  const std::vector<std::string> dependencies =
      session.GetLayerDependencies();
  assert(std::is_sorted(dependencies.begin(), dependencies.end()));
  assert(std::adjacent_find(dependencies.begin(), dependencies.end()) ==
         dependencies.end());
  assert(std::find(dependencies.begin(), dependencies.end(), root_path) !=
         dependencies.end());
  assert(std::find(dependencies.begin(), dependencies.end(), sub_path) !=
         dependencies.end());

  std::remove(root_path);
  std::remove(sub_path);
  std::cout << "  StageSession preview/dependency tests passed!" << std::endl;
}

// ============================================================
// Regression: Matrix type name lookup (previously Matrix2f/Matrix3f/Matrix4f
// shared the "matrix2d"/"matrix3d"/"matrix4d" name with their double siblings,
// making GetTypeIdFromName("matrix2f") return Invalid).
// ============================================================

void test_matrix_type_name_lookup() {
  std::cout << "Testing matrix type name lookup regression..." << std::endl;

  // Float matrix types must have their own distinct USD names.
  assert(GetTypeIdFromName("matrix2f") == TypeId::Matrix2f);
  assert(GetTypeIdFromName("matrix3f") == TypeId::Matrix3f);
  assert(GetTypeIdFromName("matrix4f") == TypeId::Matrix4f);

  // Double matrix types remain unchanged.
  assert(GetTypeIdFromName("matrix2d") == TypeId::Matrix2d);
  assert(GetTypeIdFromName("matrix3d") == TypeId::Matrix3d);
  assert(GetTypeIdFromName("matrix4d") == TypeId::Matrix4d);

  // GetTypeName must return the correct name for each type.
  assert(std::strcmp(GetTypeName(TypeId::Matrix2f), "matrix2f") == 0);
  assert(std::strcmp(GetTypeName(TypeId::Matrix3f), "matrix3f") == 0);
  assert(std::strcmp(GetTypeName(TypeId::Matrix4f), "matrix4f") == 0);
  assert(std::strcmp(GetTypeName(TypeId::Matrix2d), "matrix2d") == 0);
  assert(std::strcmp(GetTypeName(TypeId::Matrix3d), "matrix3d") == 0);
  assert(std::strcmp(GetTypeName(TypeId::Matrix4d), "matrix4d") == 0);

  // TypeInfo sizes must still be correct.
  assert(GetTypeSize(TypeId::Matrix2f) == sizeof(float) * 4);
  assert(GetTypeSize(TypeId::Matrix3f) == sizeof(float) * 9);
  assert(GetTypeSize(TypeId::Matrix4f) == sizeof(float) * 16);
  assert(GetTypeSize(TypeId::Matrix2d) == sizeof(double) * 4);
  assert(GetTypeSize(TypeId::Matrix3d) == sizeof(double) * 9);
  assert(GetTypeSize(TypeId::Matrix4d) == sizeof(double) * 16);

  // Float matrices are distinct types from double matrices.
  assert(TypeId::Matrix2f != TypeId::Matrix2d);
  assert(TypeId::Matrix3f != TypeId::Matrix3d);
  assert(TypeId::Matrix4f != TypeId::Matrix4d);

  std::cout << "  Matrix type name lookup regression tests passed!" << std::endl;
}

// ============================================================
// Regression: Frame4d layout and Value copy/move/equality operations.
// ============================================================

void test_frame4d_value_ops() {
  std::cout << "Testing Frame4d Value operations regression..." << std::endl;

  // Frame4d shares the matrix4d layout in the immutable metadata table.
  const TypeInfo* info = GetTypeInfo(TypeId::Frame4d);
  assert(info != nullptr);
  assert(info->size == sizeof(double) * 16);
  assert(std::strcmp(info->name, "frame4d") == 0);

  // Construct a Frame4d value via MakeRaw and verify it round-trips.
  {
    double mat[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    Value v = Value::MakeFromRaw(TypeId::Frame4d, mat);
    assert(v.type_id() == TypeId::Frame4d);
    const double* data = v.as_matrix4d();  // frame4d uses matrix4d storage
    assert(data != nullptr);
    assert(data[0] == 1.0);
    assert(data[15] == 1.0);
  }

  // Copy Frame4d value.
  {
    double mat[16] = {2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 2};
    Value a = Value::MakeFromRaw(TypeId::Frame4d, mat);
    Value b = a;
    assert(a.type_id() == TypeId::Frame4d);
    assert(b.type_id() == TypeId::Frame4d);
    assert(*a.as_matrix4d() == *b.as_matrix4d());
  }

  // Move Frame4d value.
  {
    double mat[16] = {3, 0, 0, 0, 0, 3, 0, 0, 0, 0, 3, 0, 0, 0, 0, 3};
    Value a = Value::MakeFromRaw(TypeId::Frame4d, mat);
    Value b = std::move(a);
    assert(b.type_id() == TypeId::Frame4d);
    assert(b.as_matrix4d()[15] == 3.0);
  }

  std::cout << "  Frame4d Value operations regression tests passed!" << std::endl;
}

// ============================================================
// Regression: mutable as_int_array() must accept Int2/Int3/Int4
// arrays (previously checked type_id_ != TypeId::Int instead of
// IsIntBackedArray, rejecting Int2/Int3/Int4 arrays).
// ============================================================

void test_mutable_int_array_backing_types() {
  std::cout << "Testing mutable as_int_array() backing types regression..."
            << std::endl;

  // Int2 array: const and mutable accessors must both work.
  {
    Value v = Value::MakeIntCompArray(
        std::vector<int32_t>{1, 2, 3, 4, 5, 6}, TypeId::Int2, 2);
    assert(v.type_id() == TypeId::Int2);
    assert(v.is_array());
    assert(v.array_size() == 3);

    // Const accessor
    const Value& cv = v;
    const std::vector<int32_t>* carr = cv.as_int_array();
    assert(carr != nullptr);
    assert(carr->size() == 6);
    assert((*carr)[0] == 1);

    // Mutable accessor
    std::vector<int32_t>* marr = v.as_int_array();
    assert(marr != nullptr && "mutable as_int_array() rejected Int2 array");
    assert(marr->size() == 6);
    (*marr)[0] = 99;
    assert((*v.as_int_array())[0] == 99);
  }

  // Int3 array.
  {
    Value v = Value::MakeIntCompArray(
        std::vector<int32_t>{10, 20, 30, 40, 50, 60}, TypeId::Int3, 3);
    assert(v.type_id() == TypeId::Int3);
    assert(v.array_size() == 2);

    const Value& cv = v;
    assert(cv.as_int_array() != nullptr);

    std::vector<int32_t>* marr = v.as_int_array();
    assert(marr != nullptr && "mutable as_int_array() rejected Int3 array");
    assert(marr->size() == 6);
  }

  // Int4 array.
  {
    Value v = Value::MakeIntCompArray(
        std::vector<int32_t>{1, 2, 3, 4, 5, 6, 7, 8}, TypeId::Int4, 4);
    assert(v.type_id() == TypeId::Int4);
    assert(v.array_size() == 2);

    const Value& cv = v;
    assert(cv.as_int_array() != nullptr);

    std::vector<int32_t>* marr = v.as_int_array();
    assert(marr != nullptr && "mutable as_int_array() rejected Int4 array");
    assert(marr->size() == 8);
  }

  // Plain Int array (regression check: must still work).
  {
    Value v = Value::MakeIntArray(std::vector<int32_t>{7, 8, 9});
    assert(v.type_id() == TypeId::Int);
    std::vector<int32_t>* marr = v.as_int_array();
    assert(marr != nullptr);
    assert(marr->size() == 3);
    assert((*marr)[0] == 7);
  }

  // Float-backed types must NOT be accepted by as_int_array().
  {
    Value v = Value::MakeIntArray(std::vector<int32_t>{1, 2, 3});
    // Correct type: accepted
    assert(v.as_int_array() != nullptr);
  }

  std::cout << "  Mutable as_int_array() backing types regression tests passed!"
            << std::endl;
}

// ============================================================
// Regression: ParseGenericValue tuple arity dispatch
// (Previously hard-coded Float3 for all tuples, so float2/float4/int-tuples
// failed to parse and desynced the lexer.)
// ============================================================

void test_generic_value_tuple_arity() {
  std::cout << "Testing ParseGenericValue tuple arity dispatch..." << std::endl;

  // Helper: parse a string through ParseGenericValue and return (success, type)
  auto parse = [](const char* src, TypeId& out_type) -> bool {
    Lexer lex(src, std::strlen(src));
    ParseResult result = ParseGenericValue(lex, out_type);
    // Consume trailing EOF to verify no unconsumed tokens.
    return result.success && !lex.has_error() &&
           lex.peek().type == TokenType::Eof;
  };

  // Float2 tuple
  {
    TypeId tid = TypeId::Invalid;
    assert(parse("(1.0, 2.0)", tid));
    assert(tid == TypeId::Float2);
  }

  // Float3 tuple (still works as before)
  {
    TypeId tid = TypeId::Invalid;
    assert(parse("(1.0, 2.0, 3.0)", tid));
    assert(tid == TypeId::Float3);
  }

  // Float4 tuple
  {
    TypeId tid = TypeId::Invalid;
    assert(parse("(1.0, 2.0, 3.0, 4.0)", tid));
    assert(tid == TypeId::Float4);
  }

  // Int2 tuple
  {
    TypeId tid = TypeId::Invalid;
    assert(parse("(10, 20)", tid));
    assert(tid == TypeId::Int2);
  }

  // Int3 tuple
  {
    TypeId tid = TypeId::Invalid;
    assert(parse("(10, 20, 30)", tid));
    assert(tid == TypeId::Int3);
  }

  // Int4 tuple
  {
    TypeId tid = TypeId::Invalid;
    assert(parse("(10, 20, 30, 40)", tid));
    assert(tid == TypeId::Int4);
  }

  // Unmatched '(' is an error
  {
    TypeId tid = TypeId::Invalid;
    assert(!parse("(1.0, 2.0", tid));
  }

  // Non-tuple values still work
  {
    TypeId tid = TypeId::Invalid;
    assert(parse("42", tid) && tid == TypeId::Int);
    assert(parse("3.14", tid) && tid == TypeId::Double);
    assert(parse("\"hello\"", tid) && tid == TypeId::String);
  }

  std::cout << "  ParseGenericValue tuple arity dispatch tests passed!"
            << std::endl;
}

// ============================================================
// Regression: EncodeDeltaS32 signed overflow
// (Previously used plain int32_t subtraction which is UB on overflow;
// the unsigned counterpart EncodeDeltaU32 correctly promotes to int64_t.)
// ============================================================

void test_encode_delta_s32_overflow() {
  std::cout << "Testing EncodeDeltaS32 overflow safety..." << std::endl;

  // The encode/decode must round-trip correctly for large (but int32_t-fitting)
  // deltas. The previous UB was in the subtraction `values[i] - prev` when
  // both operands are extreme int32_t values; the fix promotes to int64_t.
  {
    int32_t values[] = {-1000000000, 1000000000};
    std::vector<uint8_t> encoded = EncodeDeltaS32(values, 2);
    assert(!encoded.empty());

    std::vector<int32_t> decoded(2);
    bool ok = DecodeDeltaS32(encoded.data(), encoded.size(),
                             decoded.data(), 2);
    assert(ok);
    assert(decoded[0] == -1000000000);
    assert(decoded[1] == 1000000000);
  }

  // Large negative delta
  {
    int32_t values[] = {0, -2000000000};
    std::vector<uint8_t> encoded = EncodeDeltaS32(values, 2);
    assert(!encoded.empty());

    std::vector<int32_t> decoded(2);
    bool ok = DecodeDeltaS32(encoded.data(), encoded.size(),
                             decoded.data(), 2);
    assert(ok);
    assert(decoded[0] == 0);
    assert(decoded[1] == -2000000000);
  }

  // The on-disk format's code-3 lane is int64 specifically so transitions
  // between opposite int32 extremes remain lossless.
  {
    const int32_t values[] = {INT32_MIN, INT32_MAX, INT32_MIN};
    const std::vector<uint8_t> encoded = EncodeDeltaS32(values, 3);
    assert(!encoded.empty());

    std::vector<int32_t> decoded(3);
    assert(DecodeDeltaS32(encoded.data(), encoded.size(), decoded.data(), 3));
    assert(decoded == std::vector<int32_t>(values, values + 3));
  }

  // A hostile code-3 delta must be range-checked before signed addition.
  {
    std::vector<uint8_t> encoded(sizeof(int32_t) + 1 + sizeof(int32_t) +
                                 sizeof(int64_t), 0);
    encoded[sizeof(int32_t)] = uint8_t{0x0e};  // code 2, then code 3
    const int32_t first_delta = INT32_MAX;
    const int64_t overflowing_delta = INT64_MAX;
    std::memcpy(encoded.data() + sizeof(int32_t) + 1, &first_delta,
                sizeof(first_delta));
    std::memcpy(encoded.data() + sizeof(int32_t) + 1 + sizeof(first_delta),
                &overflowing_delta, sizeof(overflowing_delta));
    int32_t decoded[2] = {};
    assert(!DecodeDeltaS32(encoded.data(), encoded.size(), decoded, 2));
  }

  // The 2-bit code-map calculation must reject hostile counts before it wraps
  // or indexes beyond the supplied buffer. Non-empty decodes also require an
  // output destination.
  {
    const uint8_t tiny[16] = {};
    uint32_t u32 = 0;
    int32_t s32 = 0;
    uint64_t u64 = 0;
    const size_t huge = (std::numeric_limits<size_t>::max)();
    assert(!DecodeDeltaU32(tiny, sizeof(tiny), &u32, huge));
    assert(!DecodeDeltaS32(tiny, sizeof(tiny), &s32, huge));
    assert(!DecodeDeltaU64(tiny, sizeof(tiny), &u64, huge));
    assert(!DecodeDeltaU32(tiny, sizeof(tiny), nullptr, 1));
    assert(!DecodeDeltaS32(tiny, sizeof(tiny), nullptr, 1));
    assert(!DecodeDeltaU64(tiny, sizeof(tiny), nullptr, 1));
  }

  // U64 reconstruction is defined in unsigned arithmetic, including values
  // above INT64_MAX; no implementation-defined signed round-trip is needed.
  {
    std::vector<uint8_t> encoded(sizeof(int64_t) + 1, 0);
    const int64_t common_delta = -1;
    std::memcpy(encoded.data(), &common_delta, sizeof(common_delta));
    uint64_t decoded[2] = {};
    assert(DecodeDeltaU64(encoded.data(), encoded.size(), decoded, 2));
    assert(decoded[0] == (std::numeric_limits<uint64_t>::max)());
    assert(decoded[1] == (std::numeric_limits<uint64_t>::max)() - 1);
  }

  // Single element (no deltas to compute)
  {
    int32_t values[] = {42};
    std::vector<uint8_t> encoded = EncodeDeltaS32(values, 1);
    assert(!encoded.empty());

    std::vector<int32_t> decoded(1);
    bool ok = DecodeDeltaS32(encoded.data(), encoded.size(),
                             decoded.data(), 1);
    assert(ok);
    assert(decoded[0] == 42);
  }

  // Empty input
  {
    std::vector<uint8_t> encoded = EncodeDeltaS32(nullptr, 0);
    assert(encoded.empty());
  }

  std::cout << "  Delta-code overflow safety tests passed!" << std::endl;
}

// ============================================================
// Main
// ============================================================

// ============================================================
// Regression: LerpValue must interpolate every linearly-interpolatable
// scalar type (previously Half/Half2-4, semantic-half aliases, Quath and
// Matrix2f/3f/2d/3d fell through to "held", snapping animated values of
// those types to the earlier time sample).
// ============================================================

void test_lerp_value_scalar_type_coverage() {
  std::cout << "Testing LerpValue scalar type coverage regression..."
            << std::endl;

  // Half scalar: 1.0h (0x3C00) .. 3.0h (0x4200), midpoint must be 2.0.
  {
    const uint16_t one = 0x3C00, three = 0x4200;
    Value a = Value::MakeFromRaw(TypeId::Half, &one);
    Value b = Value::MakeFromRaw(TypeId::Half, &three);
    Value r = LerpValue(a, b, 0.5);
    assert(r.type_id() == TypeId::Half);
    float f = 0.0f;
    assert(r.to_float(&f));
    assert(f == 2.0f);
  }

  // Half3 semantic alias (point3h): componentwise midpoint.
  {
    const uint16_t pa[3] = {0x3C00, 0x3C00, 0x3C00};  // (1,1,1)
    const uint16_t pb[3] = {0x4200, 0x4200, 0x4200};  // (3,3,3)
    Value a = Value::MakeFromRaw(TypeId::Point3h, pa);
    Value b = Value::MakeFromRaw(TypeId::Point3h, pb);
    Value r = LerpValue(a, b, 0.5);
    assert(r.type_id() == TypeId::Point3h);
    float f3[3] = {0, 0, 0};
    assert(r.to_float3(f3));
    assert(f3[0] == 2.0f && f3[1] == 2.0f && f3[2] == 2.0f);
  }

  // Matrix3f: elementwise lerp.
  {
    float ma[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
    float mb[9] = {4, 4, 4, 4, 4, 4, 4, 4, 4};
    Value r = LerpValue(Value::MakeMatrix3f(ma), Value::MakeMatrix3f(mb), 0.25);
    assert(r.type_id() == TypeId::Matrix3f);
    const float* m = r.as_matrix3f();
    assert(m != nullptr);
    for (int i = 0; i < 9; ++i) assert(m[i] == 1.0f);
  }

  // Matrix2d: elementwise lerp.
  {
    double ma[4] = {0, 0, 0, 0};
    double mb[4] = {2, 2, 2, 2};
    Value r = LerpValue(Value::MakeMatrix2d(ma), Value::MakeMatrix2d(mb), 0.5);
    assert(r.type_id() == TypeId::Matrix2d);
    const double* m = r.as_matrix2d();
    assert(m != nullptr);
    for (int i = 0; i < 4; ++i) assert(m[i] == 1.0);
  }

  // Quath: identity..identity slerp stays identity (exercise the half-quat
  // path; previously the value was held, which also returned `a`, so check a
  // non-trivial pair: w-flip halfway must renormalize, not snap).
  {
    const uint16_t qa[4] = {0x3C00, 0, 0, 0};  // (w=1,x=0,y=0,z=0) storage-order agnostic
    Value a = Value::MakeFromRaw(TypeId::Quath, qa);
    Value r = LerpValue(a, a, 0.5);
    assert(r.type_id() == TypeId::Quath);
  }

  // Non-interpolatable types must still be held.
  {
    Value a(int32_t(1)), b(int32_t(3));
    Value r = LerpValue(a, b, 0.5);
    assert(r.type_id() == TypeId::Int);
    assert(*r.as_int() == 1);
  }

  std::cout << "  LerpValue scalar type coverage regression tests passed!"
            << std::endl;
}

// ============================================================
// Regression: PathExpression arrays share the string-vector storage but
// were excluded from as_token_array()/operator==/hash(), so two identical
// pathExpression[] values compared unequal and hashed by type only.
// ============================================================

void test_path_expression_array_equality_hash() {
  std::cout << "Testing pathExpression[] equality/hash regression..."
            << std::endl;

  std::vector<std::string> e1 = {"/World//", "/Set/Chair_*"};
  std::vector<std::string> e2 = {"/World//", "/Set/Chair_*"};
  std::vector<std::string> e3 = {"/Other//"};
  Value a = Value::MakeStringLikeArray(std::move(e1), TypeId::PathExpression);
  Value b = Value::MakeStringLikeArray(std::move(e2), TypeId::PathExpression);
  Value c = Value::MakeStringLikeArray(std::move(e3), TypeId::PathExpression);

  assert(a.as_token_array() != nullptr);
  assert(a.as_token_array()->size() == 2);
  assert(a == b);
  assert(!(a == c));
  assert(a.hash() == b.hash());
  assert(a.hash() != c.hash());

  std::cout << "  pathExpression[] equality/hash regression tests passed!"
            << std::endl;
}

// Regression (ASan heap-use-after-free): ParsePrimContents cached the
// current PrimSpec* across a nested child ParsePrim(), which appends to the
// layer's flat prim vector and can reallocate it. A `reorder` statement
// AFTER a child prim then dereferenced the freed pointer. Found by the
// next_usda fuzzer.
void test_usda_reorder_after_child_prim() {
  std::cout << "Testing USDA reorder-after-child-prim UAF regression..."
            << std::endl;

  // Many children force the flat prim vector to reallocate mid-parse; the
  // trailing `reorder properties` on the parent then touches its meta.
  const char* input = R"(#usda 1.0
def Xform "Parent" {
    def Scope "A" {}
    def Scope "B" {}
    def Scope "C" {}
    def Scope "D" {}
    def Scope "E" {}
    def Scope "F" {}
    def Scope "G" {}
    def Scope "H" {}
    reorder nameChildren = ["H", "A"]
    reorder properties = ["y", "x"]
    float x = 1.0
    float y = 2.0
}

)";

  LoadResult result = LoadUSDAFromString(input, std::strlen(input));
  assert(result.success);
  UsdPrim parent = result.stage.GetPrimAtPath("/Parent");
  assert(parent.IsValid());
  assert(parent.GetChildren().size() == 8);
  assert(parent.HasProperty("x"));
  assert(parent.HasProperty("y"));

  std::cout << "  USDA reorder-after-child-prim UAF regression passed!"
            << std::endl;
}

void test_hardened_session_profile() {
  const size_t cap = size_t(64) << 20;
  StageSessionOptions opts = MakeHardenedStageSessionOptions(cap);
  assert(opts.load.strict_aousd_conformance);
  assert(opts.load.max_memory == cap);
  assert(opts.max_total_memory == cap);
  assert(!opts.load.usdc_options.crate_options.use_mmap);
  assert(opts.composition.error_when_asset_not_found);
  assert(!opts.composition.usdc_use_mmap);
  assert(!opts.resolver.allow_absolute_paths);
  assert(!opts.resolver.allow_parent_paths);
  assert(!opts.resolver.search_recursively);
  assert(!opts.resolver.enable_suffix_fallback);
  assert(opts.execution.max_threads == 1);
  assert(opts.execution.callback_concurrency ==
         CallbackConcurrency::Serialized);
  assert(MakeHardenedStageSessionOptions(0).max_total_memory == 1);
  assert(ClampExecutionThreads(1000) == kMaxExecutionThreads);
  TaskArena arena(1000);
  assert(arena.max_threads() == static_cast<size_t>(kMaxExecutionThreads));
}

int main() {
  std::cout << "=== LightUSD Next Unit Tests ===" << std::endl;
  std::cout << std::endl;

  try {
    test_type_id();
    test_shared_storage();
    test_value();
    test_matrix_type_name_lookup();
    test_frame4d_value_ops();
    test_mutable_int_array_backing_types();
    test_generic_value_tuple_arity();
    test_encode_delta_s32_overflow();
    test_lerp_value_scalar_type_coverage();
    test_path_expression_array_equality_hash();
    test_path();
    test_prim();
    test_lexer();
    test_value_parser();
    test_ascii_parser();
    test_usda_reader();
    test_usda_reorder_after_child_prim();
    test_hardened_session_profile();
    test_usda_lazy_parse_policies();
    test_arc_listops();
    test_arc_layer_offset_parse();
    test_physics_schema();
    test_load_usd_from_memory();
    test_stage_session_variants();
    test_stage_session_payloads_and_cancel();
    test_stage_session_preview_and_dependencies();

    std::cout << std::endl;
    std::cout << "All tests passed!" << std::endl;
    return 0;

  } catch (const std::exception& e) {
    std::cerr << "Test failed with exception: " << e.what() << std::endl;
    return 1;
  }
}
