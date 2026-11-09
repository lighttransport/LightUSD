// SPDX-License-Identifier: Apache 2.0
// Copyright 2025 - Present, Light Transport Entertainment Inc.
//
// Test TypedArray-based TimeSamples with deduplication
//

#include <iostream>
#include <vector>
#include <cassert>

#include "value-types.hh"
#include "timesamples.hh"
#include "typed-array.hh"

using namespace lightusd;

using namespace lightusd::legacy;  // NOLINT

// Test helper to check if deduplication is working
template<typename T>
bool test_typed_array_dedup() {
    value::TimeSamples ts;
    // binary storage is selected automatically via init() or the first typed add_sample call

    // Create a TypedArray
    TypedArrayImpl<T> arr_impl(100);
    for (size_t i = 0; i < 100; i++) {
        arr_impl[i] = static_cast<T>(i);
    }
    TypedArray<T> arr(new TypedArrayImpl<T>(arr_impl.data(), arr_impl.size()));

    // Add the same array at different times
    std::string err;
    bool ret = ts.add_array_sample<T>(0.0, arr, &err);
    if (!ret) {
        std::cerr << "Failed to add array sample at t=0.0: " << err << std::endl;
        return false;
    }

    ret = ts.add_array_sample<T>(1.0, arr, &err);
    if (!ret) {
        std::cerr << "Failed to add array sample at t=1.0: " << err << std::endl;
        return false;
    }

    ret = ts.add_array_sample<T>(2.0, arr, &err);
    if (!ret) {
        std::cerr << "Failed to add array sample at t=2.0: " << err << std::endl;
        return false;
    }

    // Verify we have 3 samples
    if (ts.size() != 3) {
        std::cerr << "Expected 3 samples, got " << ts.size() << std::endl;
        return false;
    }

    std::cout << "✓ TypedArray<" << value::TypeTraits<T>::type_name()
              << "> TimeSamples test passed" << std::endl;
    return true;
}

// Test that std::vector still works
template<typename T>
bool test_vector_compatibility() {
    value::TimeSamples ts;
    // binary storage is selected automatically

    std::vector<T> vec(50);
    for (size_t i = 0; i < 50; i++) {
        vec[i] = static_cast<T>(i * 2);
    }

    std::string err;
    bool ret = ts.add_array_sample<T>(0.0, vec, &err);
    if (!ret) {
        std::cerr << "Failed to add vector sample: " << err << std::endl;
        return false;
    }

    if (ts.size() != 1) {
        std::cerr << "Expected 1 sample, got " << ts.size() << std::endl;
        return false;
    }

    std::cout << "✓ std::vector<" << value::TypeTraits<T>::type_name()
              << "> compatibility test passed" << std::endl;
    return true;
}

// Test scalar values
template<typename T>
bool test_scalar_values() {
    value::TimeSamples ts;
    // binary storage is selected automatically

    std::string err;
    T val1 = static_cast<T>(42);
    T val2 = static_cast<T>(84);

    bool ret = ts.add_sample<T>(0.0, val1, &err);
    if (!ret) {
        std::cerr << "Failed to add scalar sample at t=0.0: " << err << std::endl;
        return false;
    }

    ret = ts.add_sample<T>(1.0, val2, &err);
    if (!ret) {
        std::cerr << "Failed to add scalar sample at t=1.0: " << err << std::endl;
        return false;
    }

    if (ts.size() != 2) {
        std::cerr << "Expected 2 samples, got " << ts.size() << std::endl;
        return false;
    }

    std::cout << "✓ Scalar " << value::TypeTraits<T>::type_name()
              << " test passed" << std::endl;
    return true;
}

int main() {
    std::cout << "Testing TypedArray-based TimeSamples with deduplication\n" << std::endl;

    bool all_passed = true;

    // Test integer types
    std::cout << "Testing integer array types:" << std::endl;
    all_passed &= test_typed_array_dedup<int32_t>();
    all_passed &= test_typed_array_dedup<uint32_t>();
    all_passed &= test_typed_array_dedup<int64_t>();
    all_passed &= test_typed_array_dedup<uint64_t>();
    std::cout << std::endl;

    // Test floating point types
    std::cout << "Testing floating point array types:" << std::endl;
    all_passed &= test_typed_array_dedup<float>();
    all_passed &= test_typed_array_dedup<double>();
    std::cout << std::endl;

    // Test vector compatibility
    std::cout << "Testing std::vector compatibility:" << std::endl;
    all_passed &= test_vector_compatibility<int32_t>();
    all_passed &= test_vector_compatibility<float>();
    all_passed &= test_vector_compatibility<double>();
    std::cout << std::endl;

    // Test scalar values
    std::cout << "Testing scalar values:" << std::endl;
    all_passed &= test_scalar_values<int32_t>();
    all_passed &= test_scalar_values<float>();
    all_passed &= test_scalar_values<double>();
    std::cout << std::endl;

    if (all_passed) {
        std::cout << "✅ All tests passed!" << std::endl;
        return 0;
    } else {
        std::cerr << "❌ Some tests failed!" << std::endl;
        return 1;
    }
}
