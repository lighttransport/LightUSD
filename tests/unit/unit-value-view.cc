#ifdef _MSC_VER
#define NOMINMAX
#endif

#define TEST_NO_MAIN
#include "acutest.h"

#include "unit-value-view.h"
#include "value-types.hh"
#include "typed-array.hh"

#include <vector>
#include <cstring>

using namespace lightusd;

using namespace lightusd::legacy;  // NOLINT
using namespace lightusd::value;
using namespace lightusd::legacy::value;  // NOLINT

void value_view_size_test(void) {
  // ValueView layout: void* ptr_ + uint32_t type_id_ + uint8_t flags_
  // + padding. The header declares 12 bytes on 32-bit, 16 on 64-bit.
  constexpr size_t kExpectedSize = sizeof(void*) == 8 ? 16 : 12;
  static_assert(sizeof(ValueView) == kExpectedSize,
                "ValueView size mismatch: 16 bytes on 64-bit / 12 on 32-bit");
  TEST_CHECK(sizeof(ValueView) == kExpectedSize);
  TEST_MSG("sizeof(ValueView) = %zu, expected %zu", sizeof(ValueView), kExpectedSize);
}

void value_view_construct_test(void) {
  // Default constructor: invalid, TYPE_ID_INVALID
  {
    ValueView view;
    TEST_CHECK(!view.valid());
    TEST_CHECK(view.type_id() == TYPE_ID_INVALID);
  }

  // Float construction: valid, correct type_id, view<float>() works
  {
    float f = 3.14f;
    ValueView view(&f);
    TEST_CHECK(view.valid());
    TEST_CHECK(view.type_id() == TypeTraits<float>::type_id());

    const float* ptr = view.view<float>();
    TEST_CHECK(ptr != nullptr);
    if (ptr) {
      TEST_CHECK(*ptr == 3.14f);
      TEST_MSG("view<float>() returned %f, expected 3.14", static_cast<double>(*ptr));
    }
  }
}

void value_view_vector_test(void) {
  std::vector<double> vec = {1.0, 2.0, 3.0};
  ValueView view(&vec);
  TEST_CHECK(view.valid());
  TEST_CHECK(view.type_id() == TypeTraits<std::vector<double>>::type_id());
  TEST_CHECK(view.is_vector());
  TEST_CHECK(!view.is_typed_array());

  const std::vector<double>* vec_ptr = view.view<std::vector<double>>();
  TEST_CHECK(vec_ptr != nullptr);
  if (vec_ptr) {
    TEST_CHECK(vec_ptr->size() == 3);
    TEST_CHECK((*vec_ptr)[0] == 1.0);
    TEST_MSG("vec[0] = %f, expected 1.0", (*vec_ptr)[0]);
  }
}

void value_view_as_view_test(void) {
  std::vector<int> vec = {10, 20, 30};
  ValueView view(&vec);

  auto array_view = view.as_view<int>();
  TEST_CHECK(array_view.size() == 3);
  TEST_MSG("as_view size = %zu, expected 3", array_view.size());
  TEST_CHECK(array_view[0] == 10);
  TEST_CHECK(array_view[1] == 20);
  TEST_CHECK(array_view[2] == 30);
}

void value_view_reset_test(void) {
  double d = 2.718;
  ValueView view(&d);
  TEST_CHECK(view.valid());

  view.reset();
  TEST_CHECK(!view.valid());
  TEST_CHECK(view.type_id() == TYPE_ID_INVALID);

  int i = 42;
  view.reset(&i);
  TEST_CHECK(view.valid());
  TEST_CHECK(view.type_id() == TypeTraits<int>::type_id());
}

void value_view_role_types_test(void) {
  point3f pt = {1.0f, 2.0f, 3.0f};
  ValueView view(&pt);
  TEST_CHECK(view.type_id() == TypeTraits<point3f>::type_id());

  // Should be able to view as underlying type (float3)
  const float3* f3_ptr = view.view<float3>();
  TEST_CHECK(f3_ptr != nullptr);
  if (f3_ptr) {
    TEST_CHECK((*f3_ptr)[0] == 1.0f);
    TEST_CHECK((*f3_ptr)[1] == 2.0f);
    TEST_CHECK((*f3_ptr)[2] == 3.0f);
  }
}

void value_view_storage_flags_test(void) {
  std::vector<float> vec = {1.0f, 2.0f};
  ValueView vec_view(&vec);
  TEST_CHECK(vec_view.is_vector() == true);
  TEST_CHECK(vec_view.is_typed_array() == false);
}

void value_view_equality_test(void) {
  int a = 42;
  int b = 100;
  ValueView view1(&a);
  ValueView view2(&a);
  ValueView view3(&b);

  // Same pointer -> equal
  TEST_CHECK(view1 == view2);
  // Different pointer -> not equal
  TEST_CHECK(view1 != view3);
}
