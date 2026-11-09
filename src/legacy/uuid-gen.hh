#pragma once

#include <string>
#include <random>
#include <sstream>
#include <iomanip>

namespace lightusd {

///
/// Simple UUID Version 4 generator
///
/// Generates random UUIDs in the format: xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx
/// where x is a random hexadecimal digit and y is one of 8, 9, A, or B
///
class UUIDGenerator {
 public:
  UUIDGenerator();

  ///
  /// Generate a new UUID v4 string
  /// @return UUID string in standard format (e.g., "550e8400-e29b-41d4-a716-446655440000")
  ///
  std::string generate();

  ///
  /// Generate a new UUID v4 string (static version)
  /// @return UUID string in standard format
  ///
  static std::string generateUUID();

 private:
  std::random_device rd_;
  std::mt19937 gen_;
  std::uniform_int_distribution<uint32_t> dis_;
};

///
/// Generate a UUID v4 string (convenience function)
/// @return UUID string in standard format
///
std::string generateUUID();

}  // namespace lightusd

