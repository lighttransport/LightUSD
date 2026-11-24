#include "static_any.hh"
#include <iostream>
#include <string>
#include <vector>
#include <cassert>

namespace lightusd {

struct Point {
    float x, y, z;
    Point(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
};

struct LargeStruct {
    char data[64];
    int value;
    LargeStruct(int v = 0) : value(v) {
        for (int i = 0; i < 64; ++i) data[i] = 'A' + (i % 26);
    }
};

template <>
struct TypeTraits<int> {
    static constexpr uint8_t type_id() { return 1; }
};

template <>
struct TypeTraits<float> {
    static constexpr uint8_t type_id() { return 2; }
};

template <>
struct TypeTraits<double> {
    static constexpr uint8_t type_id() { return 3; }
};

template <>
struct TypeTraits<std::string> {
    static constexpr uint8_t type_id() { return 4; }
};

template <>
struct TypeTraits<Point> {
    static constexpr uint8_t type_id() { return 5; }
};

template <>
struct TypeTraits<LargeStruct> {
    static constexpr uint8_t type_id() { return 6; }
};

template <>
struct TypeTraits<std::vector<int>> {
    static constexpr uint8_t type_id() { return 7; }
};

}

using namespace lightusd;

using namespace lightusd::legacy;  // NOLINT

void test_basic_types() {
    std::cout << "Testing basic types..." << std::endl;

    static_any a(42);
    assert(a.has_value());
    assert(a.is<int>());
    assert(a.type_id() == 1);
    assert(*a.cast<int>() == 42);

    a = 3.14f;
    assert(a.is<float>());
    assert(a.type_id() == 2);
    assert(*a.cast<float>() == 3.14f);

    a = 2.718;
    assert(a.is<double>());
    assert(a.type_id() == 3);

    std::cout << "  Basic types: PASSED" << std::endl;
}

void test_string() {
    std::cout << "Testing strings..." << std::endl;

    static_any a(std::string("Hello, World!"));
    assert(a.is<std::string>());
    assert(a.type_id() == 4);
    assert(*a.cast<std::string>() == "Hello, World!");

    a = std::string("Updated");
    assert(*a.cast<std::string>() == "Updated");

    std::cout << "  Strings: PASSED" << std::endl;
}

void test_custom_types() {
    std::cout << "Testing custom types..." << std::endl;

    static_any a(Point(1.0f, 2.0f, 3.0f));
    assert(a.is<Point>());
    assert(a.type_id() == 5);

    Point* p = a.cast<Point>();
    assert(p != nullptr);
    assert(p->x == 1.0f && p->y == 2.0f && p->z == 3.0f);

    std::cout << "  Custom types: PASSED" << std::endl;
}

void test_large_types() {
    std::cout << "Testing large types (heap allocation)..." << std::endl;

    static_any a(LargeStruct(999));
    assert(a.is<LargeStruct>());
    assert(a.type_id() == 6);
    assert(a.cast<LargeStruct>()->value == 999);

    static_any b = a;
    assert(b.cast<LargeStruct>()->value == 999);

    std::cout << "  Large types: PASSED" << std::endl;
}

void test_copy_move() {
    std::cout << "Testing copy and move semantics..." << std::endl;

    static_any a(42);
    static_any b(a);
    assert(b.is<int>() && *b.cast<int>() == 42);

    static_any c(std::move(a));
    assert(!a.has_value());
    assert(c.is<int>() && *c.cast<int>() == 42);

    static_any d;
    d = b;
    assert(d.is<int>() && *d.cast<int>() == 42);

    static_any e;
    e = std::move(b);
    assert(!b.has_value());
    assert(e.is<int>() && *e.cast<int>() == 42);

    std::cout << "  Copy/Move: PASSED" << std::endl;
}

void test_reset() {
    std::cout << "Testing reset..." << std::endl;

    static_any a(42);
    assert(a.has_value());

    a.reset();
    assert(!a.has_value());
    assert(a.type_id() == 0);
    assert(a.cast<int>() == nullptr);

    std::cout << "  Reset: PASSED" << std::endl;
}

void test_swap() {
    std::cout << "Testing swap..." << std::endl;

    static_any a(42);
    static_any b(3.14f);

    a.swap(b);
    assert(a.is<float>() && *a.cast<float>() == 3.14f);
    assert(b.is<int>() && *b.cast<int>() == 42);

    swap(a, b);
    assert(a.is<int>() && *a.cast<int>() == 42);
    assert(b.is<float>() && *b.cast<float>() == 3.14f);

    std::cout << "  Swap: PASSED" << std::endl;
}

void test_vector() {
    std::cout << "Testing vector..." << std::endl;

    std::vector<int> vec = {1, 2, 3, 4, 5};
    static_any a(vec);
    assert(a.is<std::vector<int>>());
    assert(a.type_id() == 7);

    auto* stored_vec = a.cast<std::vector<int>>();
    assert(stored_vec != nullptr);
    assert(stored_vec->size() == 5);
    assert((*stored_vec)[0] == 1);
    assert((*stored_vec)[4] == 5);

    std::cout << "  Vector: PASSED" << std::endl;
}

void test_emplace() {
    std::cout << "Testing emplace..." << std::endl;

    static_any a;
    a.emplace(42);
    assert(a.is<int>() && *a.cast<int>() == 42);

    a.emplace(std::string("emplaced"));
    assert(a.is<std::string>() && *a.cast<std::string>() == "emplaced");

    std::cout << "  Emplace: PASSED" << std::endl;
}

void test_as_method() {
    std::cout << "Testing as() method (unsafe cast)..." << std::endl;

    static_any a(42);
    int& value = a.as<int>();
    assert(value == 42);

    value = 100;
    assert(a.as<int>() == 100);

    const static_any b(3.14f);
    const float& f = b.as<float>();
    assert(f == 3.14f);

    std::cout << "  as() method: PASSED" << std::endl;
}

int main() {
    std::cout << "=== Testing static_any ===" << std::endl;
    std::cout << "Small buffer size: 24 bytes" << std::endl;
    std::cout << "Supports up to 256 types (type_id 0-255)" << std::endl;
    std::cout << std::endl;

    test_basic_types();
    test_string();
    test_custom_types();
    test_large_types();
    test_copy_move();
    test_reset();
    test_swap();
    test_vector();
    test_emplace();
    test_as_method();

    std::cout << std::endl;
    std::cout << "=== All tests PASSED! ===" << std::endl;

    return 0;
}