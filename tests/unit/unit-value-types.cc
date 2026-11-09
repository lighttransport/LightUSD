#ifdef _MSC_VER
#define NOMINMAX
#endif

#define TEST_NO_MAIN
#include "acutest.h"

#include "unit-value-types.h"
#include "chunked-typed-array.hh"
#include "value-types.hh"
#include "math-util.inc"

#include <cstring>

using namespace lightusd;

using namespace lightusd::legacy;  // NOLINT

void value_types_test(void) {

  lightusd::legacy::value::token tok1("bora");
  lightusd::legacy::value::token tok2("muda");
  lightusd::legacy::value::token tok3("bora");
  TEST_CHECK(tok1 == tok1);
  TEST_CHECK(tok1 != tok2);
  TEST_CHECK(tok1 == tok3);

  TEST_CHECK(lightusd::legacy::value::GetTypeName(lightusd::legacy::value::TYPE_ID_TOKEN) == "token");
  TEST_CHECK(lightusd::legacy::value::GetTypeName(lightusd::legacy::value::TYPE_ID_TOKEN|lightusd::legacy::value::TYPE_ID_1D_ARRAY_BIT) == "token[]");

  TEST_CHECK(lightusd::legacy::value::GetTypeId("token") == lightusd::legacy::value::TYPE_ID_TOKEN);
  TEST_CHECK(lightusd::legacy::value::GetTypeId("token[]") == (lightusd::legacy::value::TYPE_ID_TOKEN|lightusd::legacy::value::TYPE_ID_1D_ARRAY_BIT));

  TEST_CHECK(!lightusd::legacy::value::TryGetTypeName(lightusd::legacy::value::TYPE_ID_ALL));

  // texCoord2f <-> float2 cast
  lightusd::legacy::value::float2 uv{1.0f, 2.0f};
  lightusd::legacy::value::Value value(uv);

  lightusd::legacy::value::texcoord2f *tex2f = value.as<lightusd::legacy::value::texcoord2f>();
  TEST_CHECK(tex2f != nullptr);
  if (tex2f) {
    TEST_CHECK(math::is_close(tex2f->s, 1.0f));
    TEST_CHECK(math::is_close(tex2f->t, 2.0f));
  }

}

// Test zero-copy RoleTypeCast functionality
void role_type_cast_test(void) {

  // Test 1: Scalar float2 -> texcoord2f
  {
    lightusd::legacy::value::float2 uv{1.5f, 2.5f};
    lightusd::legacy::value::Value val(uv);

    // Get pointer to underlying data before cast
    const void* ptr_before = val.get_raw().cast<lightusd::legacy::value::float2>();

    // Perform zero-copy cast
    bool result = RoleTypeCast(lightusd::legacy::value::TypeTraits<lightusd::legacy::value::texcoord2f>::type_id(), val);
    TEST_CHECK(result == true);
    TEST_CHECK(val.type_id() == lightusd::legacy::value::TypeTraits<lightusd::legacy::value::texcoord2f>::type_id());

    // Verify data is unchanged (zero-copy)
    const void* ptr_after = val.get_raw().cast<lightusd::legacy::value::texcoord2f>();
    TEST_CHECK(ptr_before == ptr_after);

    // Verify values are preserved
    auto* tex = val.as<lightusd::legacy::value::texcoord2f>();
    TEST_CHECK(tex != nullptr);
    if (tex) {
      TEST_CHECK(math::is_close(tex->s, 1.5f));
      TEST_CHECK(math::is_close(tex->t, 2.5f));
    }
  }

  // Test 2: Scalar float3 -> normal3f
  {
    lightusd::legacy::value::float3 n{0.0f, 1.0f, 0.0f};
    lightusd::legacy::value::Value val(n);

    const void* ptr_before = val.get_raw().cast<lightusd::legacy::value::float3>();

    bool result = RoleTypeCast(lightusd::legacy::value::TypeTraits<lightusd::legacy::value::normal3f>::type_id(), val);
    TEST_CHECK(result == true);
    TEST_CHECK(val.type_id() == lightusd::legacy::value::TypeTraits<lightusd::legacy::value::normal3f>::type_id());

    const void* ptr_after = val.get_raw().cast<lightusd::legacy::value::normal3f>();
    TEST_CHECK(ptr_before == ptr_after);

    auto* norm = val.as<lightusd::legacy::value::normal3f>();
    TEST_CHECK(norm != nullptr);
    if (norm) {
      TEST_CHECK(math::is_close(norm->x, 0.0f));
      TEST_CHECK(math::is_close(norm->y, 1.0f));
      TEST_CHECK(math::is_close(norm->z, 0.0f));
    }
  }

  // Test 3: Scalar float3 -> point3f
  {
    lightusd::legacy::value::float3 p{1.0f, 2.0f, 3.0f};
    lightusd::legacy::value::Value val(p);

    bool result = RoleTypeCast(lightusd::legacy::value::TypeTraits<lightusd::legacy::value::point3f>::type_id(), val);
    TEST_CHECK(result == true);
    TEST_CHECK(val.type_id() == lightusd::legacy::value::TypeTraits<lightusd::legacy::value::point3f>::type_id());

    auto* pt = val.as<lightusd::legacy::value::point3f>();
    TEST_CHECK(pt != nullptr);
    if (pt) {
      TEST_CHECK(math::is_close(pt->x, 1.0f));
      TEST_CHECK(math::is_close(pt->y, 2.0f));
      TEST_CHECK(math::is_close(pt->z, 3.0f));
    }
  }

  // Test 4: Scalar float3 -> vector3f
  {
    lightusd::legacy::value::float3 v{4.0f, 5.0f, 6.0f};
    lightusd::legacy::value::Value val(v);

    bool result = RoleTypeCast(lightusd::legacy::value::TypeTraits<lightusd::legacy::value::vector3f>::type_id(), val);
    TEST_CHECK(result == true);
    TEST_CHECK(val.type_id() == lightusd::legacy::value::TypeTraits<lightusd::legacy::value::vector3f>::type_id());

    auto* vec = val.as<lightusd::legacy::value::vector3f>();
    TEST_CHECK(vec != nullptr);
    if (vec) {
      TEST_CHECK(math::is_close(vec->x, 4.0f));
      TEST_CHECK(math::is_close(vec->y, 5.0f));
      TEST_CHECK(math::is_close(vec->z, 6.0f));
    }
  }

  // Test 5: Scalar float3 -> color3f
  {
    lightusd::legacy::value::float3 c{0.5f, 0.6f, 0.7f};
    lightusd::legacy::value::Value val(c);

    bool result = RoleTypeCast(lightusd::legacy::value::TypeTraits<lightusd::legacy::value::color3f>::type_id(), val);
    TEST_CHECK(result == true);
    TEST_CHECK(val.type_id() == lightusd::legacy::value::TypeTraits<lightusd::legacy::value::color3f>::type_id());

    auto* col = val.as<lightusd::legacy::value::color3f>();
    TEST_CHECK(col != nullptr);
    if (col) {
      TEST_CHECK(math::is_close(col->r, 0.5f));
      TEST_CHECK(math::is_close(col->g, 0.6f));
      TEST_CHECK(math::is_close(col->b, 0.7f));
    }
  }

  // Test 6: Scalar float4 -> color4f
  {
    lightusd::legacy::value::float4 c{0.1f, 0.2f, 0.3f, 1.0f};
    lightusd::legacy::value::Value val(c);

    bool result = RoleTypeCast(lightusd::legacy::value::TypeTraits<lightusd::legacy::value::color4f>::type_id(), val);
    TEST_CHECK(result == true);
    TEST_CHECK(val.type_id() == lightusd::legacy::value::TypeTraits<lightusd::legacy::value::color4f>::type_id());

    auto* col = val.as<lightusd::legacy::value::color4f>();
    TEST_CHECK(col != nullptr);
    if (col) {
      TEST_CHECK(math::is_close(col->r, 0.1f));
      TEST_CHECK(math::is_close(col->g, 0.2f));
      TEST_CHECK(math::is_close(col->b, 0.3f));
      TEST_CHECK(math::is_close(col->a, 1.0f));
    }
  }

  // Test 7: Array float2[] -> texcoord2f[]
  {
    std::vector<lightusd::legacy::value::float2> uvs = {{1.0f, 2.0f}, {3.0f, 4.0f}, {5.0f, 6.0f}};
    lightusd::legacy::value::Value val(uvs);

    const void* ptr_before = val.get_raw().cast<std::vector<lightusd::legacy::value::float2>>();

    bool result = RoleTypeCast(lightusd::legacy::value::TypeTraits<std::vector<lightusd::legacy::value::texcoord2f>>::type_id(), val);
    TEST_CHECK(result == true);
    TEST_CHECK(val.type_id() == lightusd::legacy::value::TypeTraits<std::vector<lightusd::legacy::value::texcoord2f>>::type_id());

    // Verify zero-copy: same vector object
    const void* ptr_after = val.get_raw().cast<std::vector<lightusd::legacy::value::texcoord2f>>();
    TEST_CHECK(ptr_before == ptr_after);

    auto* texs = val.as<std::vector<lightusd::legacy::value::texcoord2f>>();
    TEST_CHECK(texs != nullptr);
    if (texs) {
      TEST_CHECK(texs->size() == 3);
      TEST_CHECK(math::is_close((*texs)[0].s, 1.0f));
      TEST_CHECK(math::is_close((*texs)[0].t, 2.0f));
      TEST_CHECK(math::is_close((*texs)[1].s, 3.0f));
      TEST_CHECK(math::is_close((*texs)[1].t, 4.0f));
      TEST_CHECK(math::is_close((*texs)[2].s, 5.0f));
      TEST_CHECK(math::is_close((*texs)[2].t, 6.0f));
    }
  }

  // Test 8: Array float3[] -> normal3f[]
  {
    std::vector<lightusd::legacy::value::float3> normals = {{0.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};
    lightusd::legacy::value::Value val(normals);

    const void* ptr_before = val.get_raw().cast<std::vector<lightusd::legacy::value::float3>>();

    bool result = RoleTypeCast(lightusd::legacy::value::TypeTraits<std::vector<lightusd::legacy::value::normal3f>>::type_id(), val);
    TEST_CHECK(result == true);

    const void* ptr_after = val.get_raw().cast<std::vector<lightusd::legacy::value::normal3f>>();
    TEST_CHECK(ptr_before == ptr_after);

    auto* norms = val.as<std::vector<lightusd::legacy::value::normal3f>>();
    TEST_CHECK(norms != nullptr);
    if (norms) {
      TEST_CHECK(norms->size() == 3);
      TEST_CHECK(math::is_close((*norms)[0].y, 1.0f));
      TEST_CHECK(math::is_close((*norms)[1].x, 1.0f));
      TEST_CHECK(math::is_close((*norms)[2].z, 1.0f));
    }
  }

  // Test 9: Double precision - double3 -> normal3d
  {
    lightusd::legacy::value::double3 n{0.0, 1.0, 0.0};
    lightusd::legacy::value::Value val(n);

    bool result = RoleTypeCast(lightusd::legacy::value::TypeTraits<lightusd::legacy::value::normal3d>::type_id(), val);
    TEST_CHECK(result == true);
    TEST_CHECK(val.type_id() == lightusd::legacy::value::TypeTraits<lightusd::legacy::value::normal3d>::type_id());

    auto* norm = val.as<lightusd::legacy::value::normal3d>();
    TEST_CHECK(norm != nullptr);
    if (norm) {
      TEST_CHECK(math::is_close(norm->y, 1.0));
    }
  }

  // Test 10: Half precision - half3 -> normal3h
  {
    lightusd::legacy::value::half3 n{lightusd::legacy::value::float_to_half_full(0.0f), lightusd::legacy::value::float_to_half_full(1.0f), lightusd::legacy::value::float_to_half_full(0.0f)};
    lightusd::legacy::value::Value val(n);

    bool result = RoleTypeCast(lightusd::legacy::value::TypeTraits<lightusd::legacy::value::normal3h>::type_id(), val);
    TEST_CHECK(result == true);
    TEST_CHECK(val.type_id() == lightusd::legacy::value::TypeTraits<lightusd::legacy::value::normal3h>::type_id());
  }

  // Test 11: Invalid cast - type mismatch should fail
  {
    lightusd::legacy::value::float2 uv{1.0f, 2.0f};
    lightusd::legacy::value::Value val(uv);

    // Try to cast float2 to normal3f (incompatible)
    bool result = RoleTypeCast(lightusd::legacy::value::TypeTraits<lightusd::legacy::value::normal3f>::type_id(), val);
    TEST_CHECK(result == false);

    // Type should remain unchanged
    TEST_CHECK(val.type_id() == lightusd::legacy::value::TypeTraits<lightusd::legacy::value::float2>::type_id());
  }

  // Test 12: matrix4d -> frame4d
  {
    lightusd::legacy::value::matrix4d m = lightusd::legacy::value::matrix4d::identity();
    lightusd::legacy::value::Value val(m);

    bool result = RoleTypeCast(lightusd::legacy::value::TypeTraits<lightusd::legacy::value::frame4d>::type_id(), val);
    TEST_CHECK(result == true);
    TEST_CHECK(val.type_id() == lightusd::legacy::value::TypeTraits<lightusd::legacy::value::frame4d>::type_id());
  }

}

void value_types_typed_array_memory_test(void) {
  // lightusd::legacy::value::Value(lvalue) copies (it does not steal the buffer), so to make the
  // Value own the array we move it in. estimate_memory_usage() is a lower-bound
  // estimate derived from the element count; it does not account for reserved
  // (capacity > size) or per-chunk over-allocation, so we only require a sane
  // non-zero estimate for a populated array. (The previous form compared against
  // arr.memory_usage() but only passed because the implicit move had emptied
  // arr to ~0 bytes -- it never actually exercised the bound.)
  {
    TypedArray<float> arr;
    arr.reserve(4096);
    arr.resize(16);
    lightusd::legacy::value::Value val(std::move(arr));
    TEST_CHECK(val.estimate_memory_usage() > 0);
  }

  {
    ChunkedTypedArray<float> arr(4096);
    lightusd::legacy::value::Value val(std::move(arr));
    TEST_CHECK(val.estimate_memory_usage() > 0);
  }
}

// Regression: lightusd::legacy::value::Value's perfect-forwarding constructor must NOT move-from a
// non-const lvalue (std::forward, not std::move). A bug here silently empties the
// caller's object, e.g. `std::vector<uint8_t> a{...}; lightusd::legacy::value::Value(a);`.
void value_construct_from_lvalue_copies_test(void) {
  // lvalue -> copy: source is preserved, Value holds an independent copy.
  std::vector<int32_t> a{1, 2, 3, 4};
  lightusd::legacy::value::Value v(a);
  TEST_CHECK(a.size() == 4);
  if (const auto* p = v.as<std::vector<int32_t>>()) {
    TEST_CHECK(p->size() == 4);
    if (p->size() == 4) TEST_CHECK((*p)[3] == 4);
  } else {
    TEST_CHECK(false);
  }

  // const lvalue -> copy.
  const std::vector<int32_t> c{5, 6};
  lightusd::legacy::value::Value vc(c);
  TEST_CHECK(c.size() == 2);

  // rvalue -> move (still works): source is left empty.
  std::vector<int32_t> b{9, 8, 7};
  lightusd::legacy::value::Value w(std::move(b));
  TEST_CHECK(b.empty());
  if (const auto* p = w.as<std::vector<int32_t>>()) TEST_CHECK(p->size() == 3);
  else TEST_CHECK(false);
}
