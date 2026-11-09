// Simple test program for TypedArrayView methods in TimeSamples
#include "src/legacy/timesamples.hh"
#include "src/legacy/typed-array.hh"
#include "src/legacy/value-types.hh"
#include <iostream>
#include <cassert>
#include <vector>

using namespace lightusd;
using namespace lightusd::value;

void test_timesamples_binary_view() {
    std::cout << "Testing TimeSamples TypedArrayView for binary float arrays...\n";

    TimeSamples samples;

    // Add some array samples
    float data1[] = {1.0f, 2.0f, 3.0f};
    float data2[] = {4.0f, 5.0f, 6.0f};
    float data3[] = {7.0f, 8.0f, 9.0f};

    samples.add_array_sample(1.0, data1, 3);
    samples.add_array_sample(2.0, data2, 3);
    samples.add_array_sample(4.0, data3, 3);

    // Test get_typed_array_view_at
    {
        auto view = samples.get_typed_array_view_at<float>(0);
        assert(view.size() == 3);
        assert(view[0] == 1.0f);
        assert(view[1] == 2.0f);
        assert(view[2] == 3.0f);
        std::cout << "  ✓ get_typed_array_view_at(0) works\n";
    }

    {
        auto view = samples.get_typed_array_view_at<float>(1);
        assert(view.size() == 3);
        assert(view[0] == 4.0f);
        assert(view[1] == 5.0f);
        assert(view[2] == 6.0f);
        std::cout << "  ✓ get_typed_array_view_at(1) works\n";
    }

    // Test get_typed_array_view_at_time
    {
        auto view = samples.get_typed_array_view_at_time<float>(1.0);
        assert(view.size() == 3);
        assert(view[0] == 1.0f);
        std::cout << "  ✓ get_typed_array_view_at_time(1.0) works\n";
    }

    {
        auto view = samples.get_typed_array_view_at_time<float>(2.0);
        assert(view.size() == 3);
        assert(view[0] == 4.0f);
        std::cout << "  ✓ get_typed_array_view_at_time(2.0) works\n";
    }

    // Test non-existent time
    {
        auto view = samples.get_typed_array_view_at_time<float>(3.0);
        assert(view.empty());
        std::cout << "  ✓ get_typed_array_view_at_time(3.0) returns empty for non-existent\n";
    }

    // Test non-existent time
    {
        auto view = samples.get_typed_array_view_at_time<float>(5.0);
        assert(view.empty());
        std::cout << "  ✓ get_typed_array_view_at_time(5.0) returns empty for non-existent\n";
    }
}

void test_timesamples_vector_view() {
    std::cout << "\nTesting TimeSamples TypedArrayView with std::vector storage...\n";

    TimeSamples samples;

    // Create Value objects containing std::vectors
    std::vector<double> vec1 = {1.0, 2.0, 3.0};
    std::vector<double> vec2 = {4.0, 5.0, 6.0};
    std::vector<double> vec3 = {7.0, 8.0, 9.0};

    Value v1(vec1);
    Value v2(vec2);
    Value v3(vec3);

    // Add samples with std::vector values
    samples.add_sample(1.0, v1);
    samples.add_sample(2.0, v2);
    samples.add_blocked_sample(3.0, v3); // Blocked sample
    samples.add_sample(4.0, v3);

    // Test get_typed_array_view_at
    {
        auto view = samples.get_typed_array_view_at<double>(0);
        assert(view.size() == 3);
        assert(view[0] == 1.0);
        assert(view[1] == 2.0);
        assert(view[2] == 3.0);
        std::cout << "  ✓ get_typed_array_view_at(0) works for std::vector\n";
    }

    // Test blocked sample
    {
        auto view = samples.get_typed_array_view_at<double>(2);
        assert(view.empty());
        std::cout << "  ✓ get_typed_array_view_at(2) returns empty for blocked\n";
    }

    // Test get_typed_array_view_at_time
    {
        auto view = samples.get_typed_array_view_at_time<double>(1.0);
        assert(view.size() == 3);
        assert(view[0] == 1.0);
        std::cout << "  ✓ get_typed_array_view_at_time(1.0) works for std::vector\n";
    }
}

int main() {
    std::cout << "Testing TypedArrayView methods in TimeSamples...\n\n";

    // Test TimeSamples with binary array storage
    test_timesamples_binary_view();

    // Test TimeSamples with std::vector storage
    test_timesamples_vector_view();

    std::cout << "\n✅ All tests passed!\n";
    return 0;
}
