// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment, Inc.

#ifdef _MSC_VER
#define NOMINMAX
#endif

#define TEST_NO_MAIN
#include "acutest.h"

#include "unit-tiny-container.h"
#include "tiny-container.hh"

#include <string>
#include <utility>

using namespace lightusd;

using namespace lightusd::legacy;  // NOLINT

// Test basic operations: push_back, pop_back, size, empty, back, operator[]
void stack_vector_basic_test(void) {
  // Test with small number of elements (should stay on stack)
  {
    StackVector<int, 4> vec;
    TEST_CHECK(vec.empty() == true);
    TEST_CHECK(vec.size() == 0);

    vec.push_back(10);
    TEST_CHECK(vec.empty() == false);
    TEST_CHECK(vec.size() == 1);
    TEST_CHECK(vec[0] == 10);
    TEST_CHECK(vec.back() == 10);

    vec.push_back(20);
    vec.push_back(30);
    TEST_CHECK(vec.size() == 3);
    TEST_CHECK(vec[0] == 10);
    TEST_CHECK(vec[1] == 20);
    TEST_CHECK(vec[2] == 30);
    TEST_CHECK(vec.back() == 30);

    vec.pop_back();
    TEST_CHECK(vec.size() == 2);
    TEST_CHECK(vec.back() == 20);

    vec.pop_back();
    vec.pop_back();
    TEST_CHECK(vec.empty() == true);
  }

  // Test emplace_back
  {
    StackVector<std::pair<int, int>, 4> vec;
    vec.emplace_back(1, 2);
    vec.emplace_back(3, 4);
    TEST_CHECK(vec.size() == 2);
    TEST_CHECK(vec[0].first == 1);
    TEST_CHECK(vec[0].second == 2);
    TEST_CHECK(vec[1].first == 3);
    TEST_CHECK(vec[1].second == 4);
  }

  // Test clear
  {
    StackVector<int, 4> vec;
    vec.push_back(1);
    vec.push_back(2);
    vec.push_back(3);
    TEST_CHECK(vec.size() == 3);
    vec.clear();
    TEST_CHECK(vec.empty() == true);
    TEST_CHECK(vec.size() == 0);
  }

  // Test reserve (should not affect behavior for small sizes)
  {
    StackVector<int, 4> vec;
    vec.reserve(2);  // Still within stack capacity
    vec.push_back(1);
    vec.push_back(2);
    TEST_CHECK(vec.size() == 2);
    TEST_CHECK(vec[0] == 1);
    TEST_CHECK(vec[1] == 2);
  }
}

// Test overflow to heap when exceeding stack capacity
void stack_vector_overflow_test(void) {
  // Test that overflow to heap works correctly
  {
    StackVector<int, 4> vec;

    // Fill up stack capacity
    for (int i = 0; i < 4; ++i) {
      vec.push_back(i * 10);
    }
    TEST_CHECK(vec.size() == 4);

    // Add more - should trigger heap allocation
    for (int i = 4; i < 10; ++i) {
      vec.push_back(i * 10);
    }
    TEST_CHECK(vec.size() == 10);

    // Verify all values are correct after overflow
    for (int i = 0; i < 10; ++i) {
      TEST_CHECK(vec[i] == i * 10);
    }

    // Test pop_back after overflow
    vec.pop_back();
    TEST_CHECK(vec.size() == 9);
    TEST_CHECK(vec.back() == 80);
  }

  // Test reserve triggering heap allocation
  {
    StackVector<int, 4> vec;
    vec.push_back(1);
    vec.push_back(2);
    vec.reserve(100);  // Should trigger heap allocation

    // Verify existing data is preserved
    TEST_CHECK(vec.size() == 2);
    TEST_CHECK(vec[0] == 1);
    TEST_CHECK(vec[1] == 2);

    // Add more elements
    for (int i = 0; i < 50; ++i) {
      vec.push_back(i);
    }
    TEST_CHECK(vec.size() == 52);
  }

  // Test clear after overflow and reuse
  {
    StackVector<int, 4> vec;
    for (int i = 0; i < 10; ++i) {
      vec.push_back(i);
    }
    TEST_CHECK(vec.size() == 10);

    vec.clear();
    TEST_CHECK(vec.empty() == true);

    // Should still be able to add elements
    vec.push_back(100);
    TEST_CHECK(vec.size() == 1);
    TEST_CHECK(vec[0] == 100);
  }
}

// Test copy constructor and copy assignment
void stack_vector_copy_test(void) {
  // Copy constructor - stack storage
  {
    StackVector<int, 4> vec1;
    vec1.push_back(1);
    vec1.push_back(2);
    vec1.push_back(3);

    StackVector<int, 4> vec2(vec1);
    TEST_CHECK(vec2.size() == 3);
    TEST_CHECK(vec2[0] == 1);
    TEST_CHECK(vec2[1] == 2);
    TEST_CHECK(vec2[2] == 3);

    // Modify original, copy should not change
    vec1[0] = 100;
    TEST_CHECK(vec2[0] == 1);
  }

  // Copy constructor - heap storage
  {
    StackVector<int, 4> vec1;
    for (int i = 0; i < 10; ++i) {
      vec1.push_back(i);
    }

    StackVector<int, 4> vec2(vec1);
    TEST_CHECK(vec2.size() == 10);
    for (int i = 0; i < 10; ++i) {
      TEST_CHECK(vec2[i] == i);
    }

    // Modify original, copy should not change
    vec1[0] = 100;
    TEST_CHECK(vec2[0] == 0);
  }

  // Copy assignment - stack to stack
  {
    StackVector<int, 4> vec1;
    vec1.push_back(1);
    vec1.push_back(2);

    StackVector<int, 4> vec2;
    vec2.push_back(10);

    vec2 = vec1;
    TEST_CHECK(vec2.size() == 2);
    TEST_CHECK(vec2[0] == 1);
    TEST_CHECK(vec2[1] == 2);
  }

  // Self-assignment
  {
    StackVector<int, 4> vec;
    vec.push_back(1);
    vec.push_back(2);

    vec = vec;
    TEST_CHECK(vec.size() == 2);
    TEST_CHECK(vec[0] == 1);
    TEST_CHECK(vec[1] == 2);
  }
}

// Test move constructor and move assignment
void stack_vector_move_test(void) {
  // Move constructor - stack storage
  {
    StackVector<int, 4> vec1;
    vec1.push_back(1);
    vec1.push_back(2);
    vec1.push_back(3);

    StackVector<int, 4> vec2(std::move(vec1));
    TEST_CHECK(vec2.size() == 3);
    TEST_CHECK(vec2[0] == 1);
    TEST_CHECK(vec2[1] == 2);
    TEST_CHECK(vec2[2] == 3);
  }

  // Move constructor - heap storage
  {
    StackVector<int, 4> vec1;
    for (int i = 0; i < 10; ++i) {
      vec1.push_back(i);
    }

    StackVector<int, 4> vec2(std::move(vec1));
    TEST_CHECK(vec2.size() == 10);
    for (int i = 0; i < 10; ++i) {
      TEST_CHECK(vec2[i] == i);
    }
  }

  // Move assignment - stack to stack
  {
    StackVector<int, 4> vec1;
    vec1.push_back(1);
    vec1.push_back(2);

    StackVector<int, 4> vec2;
    vec2.push_back(10);

    vec2 = std::move(vec1);
    TEST_CHECK(vec2.size() == 2);
    TEST_CHECK(vec2[0] == 1);
    TEST_CHECK(vec2[1] == 2);
  }

  // Move assignment - heap to empty
  {
    StackVector<int, 4> vec1;
    for (int i = 0; i < 10; ++i) {
      vec1.push_back(i);
    }

    StackVector<int, 4> vec2;
    vec2 = std::move(vec1);
    TEST_CHECK(vec2.size() == 10);
    for (int i = 0; i < 10; ++i) {
      TEST_CHECK(vec2[i] == i);
    }
  }
}

// Test iterator interface (begin, end, data)
void stack_vector_iterator_test(void) {
  // Test begin/end - stack storage
  {
    StackVector<int, 4> vec;
    vec.push_back(1);
    vec.push_back(2);
    vec.push_back(3);

    int sum = 0;
    for (auto it = vec.begin(); it != vec.end(); ++it) {
      sum += *it;
    }
    TEST_CHECK(sum == 6);

    // Range-based for loop
    sum = 0;
    for (int val : vec) {
      sum += val;
    }
    TEST_CHECK(sum == 6);
  }

  // Test begin/end - heap storage
  {
    StackVector<int, 4> vec;
    for (int i = 1; i <= 10; ++i) {
      vec.push_back(i);
    }

    int sum = 0;
    for (int val : vec) {
      sum += val;
    }
    TEST_CHECK(sum == 55);  // 1+2+...+10 = 55
  }

  // Test data() pointer
  {
    StackVector<int, 4> vec;
    vec.push_back(10);
    vec.push_back(20);
    vec.push_back(30);

    int* ptr = vec.data();
    TEST_CHECK(ptr[0] == 10);
    TEST_CHECK(ptr[1] == 20);
    TEST_CHECK(ptr[2] == 30);

    // Modify through pointer
    ptr[1] = 200;
    TEST_CHECK(vec[1] == 200);
  }

  // Test const iteration
  {
    StackVector<int, 4> vec;
    vec.push_back(1);
    vec.push_back(2);
    vec.push_back(3);

    const StackVector<int, 4>& cvec = vec;
    int sum = 0;
    for (const int& val : cvec) {
      sum += val;
    }
    TEST_CHECK(sum == 6);

    const int* cptr = cvec.data();
    TEST_CHECK(cptr[0] == 1);
  }
}

// Test with complex types (non-trivial constructors/destructors)
void stack_vector_complex_type_test(void) {
  // Test with std::string
  {
    StackVector<std::string, 4> vec;
    vec.push_back("hello");
    vec.push_back("world");
    vec.emplace_back("test");

    TEST_CHECK(vec.size() == 3);
    TEST_CHECK(vec[0] == "hello");
    TEST_CHECK(vec[1] == "world");
    TEST_CHECK(vec[2] == "test");

    vec.pop_back();
    TEST_CHECK(vec.size() == 2);
    TEST_CHECK(vec.back() == "world");
  }

  // Test with std::string overflow to heap
  {
    StackVector<std::string, 2> vec;
    vec.push_back("one");
    vec.push_back("two");
    vec.push_back("three");  // Triggers heap allocation
    vec.push_back("four");

    TEST_CHECK(vec.size() == 4);
    TEST_CHECK(vec[0] == "one");
    TEST_CHECK(vec[1] == "two");
    TEST_CHECK(vec[2] == "three");
    TEST_CHECK(vec[3] == "four");
  }

  // Test with pair of strings
  {
    StackVector<std::pair<std::string, std::string>, 4> vec;
    vec.emplace_back("key1", "value1");
    vec.emplace_back("key2", "value2");

    TEST_CHECK(vec.size() == 2);
    TEST_CHECK(vec[0].first == "key1");
    TEST_CHECK(vec[0].second == "value1");
    TEST_CHECK(vec[1].first == "key2");
    TEST_CHECK(vec[1].second == "value2");
  }

  // Test copy with complex types
  {
    StackVector<std::string, 4> vec1;
    vec1.push_back("alpha");
    vec1.push_back("beta");

    StackVector<std::string, 4> vec2(vec1);
    TEST_CHECK(vec2[0] == "alpha");
    TEST_CHECK(vec2[1] == "beta");

    // Modify original
    vec1[0] = "modified";
    TEST_CHECK(vec2[0] == "alpha");  // Copy should be independent
  }

  // Test move with complex types
  {
    StackVector<std::string, 4> vec1;
    vec1.push_back("hello");
    vec1.push_back("world");

    StackVector<std::string, 4> vec2(std::move(vec1));
    TEST_CHECK(vec2[0] == "hello");
    TEST_CHECK(vec2[1] == "world");
  }

  // Test clear properly destructs elements
  {
    StackVector<std::string, 4> vec;
    vec.push_back("test1");
    vec.push_back("test2");
    vec.push_back("test3");
    vec.clear();
    TEST_CHECK(vec.empty());

    // Should be able to reuse
    vec.push_back("new");
    TEST_CHECK(vec.size() == 1);
    TEST_CHECK(vec[0] == "new");
  }
}
