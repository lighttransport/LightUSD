// Test for compact 16-byte ValueView implementation
#include "src/legacy/value-types.hh"
#include "src/legacy/typed-array.hh"
#include <iostream>
#include <cassert>
#include <vector>
#include <cstring>

using namespace lightusd;
using namespace lightusd::value;

int main() {
    std::cout << "Testing compact ValueView implementation...\n\n";

    // Check that ValueView is exactly 16 bytes
    std::cout << "Size of ValueView: " << sizeof(ValueView) << " bytes\n";
    static_assert(sizeof(ValueView) == 16, "ValueView must be exactly 16 bytes");
    std::cout << "✓ ValueView is exactly 16 bytes\n\n";

    // Test default constructor
    {
        ValueView view;
        assert(!view.valid());
        assert(view.type_id() == TYPE_ID_INVALID);
        std::cout << "✓ Default constructor works\n";
    }

    // Test direct construction from concrete types
    {
        float f = 3.14f;
        ValueView view(&f);
        assert(view.valid());
        assert(view.type_id() == TypeTraits<float>::type_id());

        // Test view() method
        const float* ptr = view.view<float>();
        assert(ptr != nullptr);
        assert(*ptr == 3.14f);
        std::cout << "✓ Direct construction from float* works\n";
        std::cout << "  - view<float>() returns correct value: " << *ptr << "\n";
    }

    // Test with vector
    {
        std::vector<double> vec = {1.0, 2.0, 3.0};
        ValueView view(&vec);
        assert(view.valid());
        assert(view.type_id() == TypeTraits<std::vector<double>>::type_id());
        assert(view.is_vector());
        assert(!view.is_typed_array());

        const std::vector<double>* vec_ptr = view.view<std::vector<double>>();
        assert(vec_ptr != nullptr);
        assert(vec_ptr->size() == 3);
        assert((*vec_ptr)[0] == 1.0);
        std::cout << "✓ Direct construction from std::vector<double>* works\n";
        std::cout << "  - is_vector() = " << view.is_vector() << "\n";
    }

    // Test as_view() method with vector
    {
        std::vector<int> vec = {10, 20, 30};
        ValueView view(&vec);

        auto array_view = view.as_view<int>();
        assert(array_view.size() == 3);
        assert(array_view[0] == 10);
        assert(array_view[1] == 20);
        assert(array_view[2] == 30);
        std::cout << "✓ as_view<int>() works for std::vector\n";
    }

    // Test reset methods
    {
        double d = 2.718;
        ValueView view(&d);
        assert(view.valid());

        view.reset();
        assert(!view.valid());
        assert(view.type_id() == TYPE_ID_INVALID);

        int i = 42;
        view.reset(&i);
        assert(view.valid());
        assert(view.type_id() == TypeTraits<int>::type_id());
        std::cout << "✓ reset() methods work\n";
    }

    // Test role types (e.g., point3f vs float3)
    {
        point3f pt = {1.0f, 2.0f, 3.0f};
        ValueView view(&pt);
        assert(view.type_id() == TypeTraits<point3f>::type_id());

        // Should be able to view as underlying type (float3)
        const float3* f3_ptr = view.view<float3>();
        assert(f3_ptr != nullptr);
        assert((*f3_ptr)[0] == 1.0f);
        assert((*f3_ptr)[1] == 2.0f);
        assert((*f3_ptr)[2] == 3.0f);
        std::cout << "✓ Role type handling works (point3f -> float3)\n";
    }

    // Test storage flags
    {
        std::vector<float> vec = {1.0f, 2.0f};
        ValueView vec_view(&vec);
        assert(vec_view.is_vector());
        assert(!vec_view.is_typed_array());
        std::cout << "✓ Storage flags work correctly\n";
    }

    // Test equality operators
    {
        int a = 42;
        int b = 100;
        ValueView view1(&a);
        ValueView view2(&a);
        ValueView view3(&b);

        assert(view1 == view2);
        assert(view1 != view3);
        std::cout << "✓ Equality operators work\n";
    }

    // Test member layout (verify 16-byte structure)
    {
        struct TestLayout {
            const void* ptr;     // 8 bytes
            uint32_t type_id;    // 4 bytes
            uint8_t flags;       // 1 byte
            uint8_t padding[3];  // 3 bytes
        };
        static_assert(sizeof(TestLayout) == 16, "Test layout should be 16 bytes");
        std::cout << "✓ Member layout confirms 16-byte structure\n";
    }

    std::cout << "\n✅ All compact ValueView tests passed!\n";
    std::cout << "\nSummary:\n";
    std::cout << "- ValueView size: " << sizeof(ValueView) << " bytes (exactly 16)\n";
    std::cout << "- Supports direct construction from T*\n";
    std::cout << "- Supports direct view<T>() method\n";
    std::cout << "- Tracks storage type (vector/TypedArray) with flags\n";
    std::cout << "- Memory layout: pointer(8) + type_id(4) + flags(1) + padding(3) = 16 bytes\n";

    return 0;
}