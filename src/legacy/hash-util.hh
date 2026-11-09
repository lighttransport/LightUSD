// SPDX-License-Identifier: MIT
// SpookyHash v2 implementation
// Original by Bob Jenkins (public domain)
#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace lightusd {

//
// SpookyHash: a 128-bit noncryptographic hash function
// By Bob Jenkins, public domain
//
class SpookyHash {
public:
  //
  // SpookyHash: hash a single message in one call, produce 128-bit output
  //
  static void Hash128(
    const void *message,  // message to hash
    size_t length,        // length of message in bytes
    uint64_t *hash1,      // in/out: in seed 1, out hash value 1
    uint64_t *hash2);     // in/out: in seed 2, out hash value 2

  //
  // Hash64: hash a single message in one call, return 64-bit output
  //
  static uint64_t Hash64(
    const void *message,  // message to hash
    size_t length,        // length of message in bytes
    uint64_t seed)        // seed
  {
    uint64_t hash1 = seed;
    Hash128(message, length, &hash1, &seed);
    return hash1;
  }

  //
  // Hash32: hash a single message in one call, return 32-bit output
  //
  static uint32_t Hash32(
    const void *message,  // message to hash
    size_t length,        // length of message in bytes
    uint32_t seed)        // seed
  {
    uint64_t hash1 = seed, hash2 = seed;
    Hash128(message, length, &hash1, &hash2);
    return static_cast<uint32_t>(hash1);
  }

  //
  // Init: initialize the context of a SpookyHash
  //
  void Init(
    uint64_t seed1,       // any 64-bit value will do, including 0
    uint64_t seed2);      // different seeds produce independent hashes

  //
  // Update: add a piece of a message to a SpookyHash state
  //
  void Update(
    const void *message,  // message fragment
    size_t length);       // length of message fragment in bytes

  //
  // Final: compute the hash for the current SpookyHash state
  //
  // This does not modify the state; you can keep updating it afterward
  //
  // The result is the same as if SpookyHash() had been called with
  // all the pieces concatenated into one message.
  //
  void Final(
    uint64_t *hash1,      // out only: first 64 bits of hash value.
    uint64_t *hash2);     // out only: second 64 bits of hash value.

private:
  //
  // number of uint64_t's in internal state
  //
  static constexpr size_t sc_numVars = 12;

  //
  // size of the internal state
  //
  static constexpr size_t sc_blockSize = sc_numVars * 8;

  //
  // size of buffer of unhashed data, in bytes
  //
  static constexpr size_t sc_bufSize = 2 * sc_blockSize;

  //
  // sc_const: a constant which:
  //  * is not zero
  //  * is odd
  //  * is a not-very-regular mix of 1's and 0's
  //  * does not need any other special mathematical properties
  //
  static constexpr uint64_t sc_const = 0xdeadbeefdeadbeefULL;

  uint64_t m_data[2*sc_numVars];   // unhashed data, for partial messages
  uint64_t m_state[sc_numVars];    // internal state of the hash
  size_t m_length;                 // total length of the input so far
  uint8_t m_remainder;             // length of unhashed data stashed in m_data

  //
  // Rot64: rotate a 64-bit value left
  //
  static inline uint64_t Rot64(uint64_t x, int k)
  {
    return (x << k) | (x >> (64 - k));
  }

  //
  // Mix: mix all 12 inputs together so that h0, h1 are a hash of them all.
  //
  // For two inputs differing in just the input bits
  // Where "differ" means xor or subtraction
  // And "differ by one bit" means the difference is a power of 2
  // Then:
  //   * That bit will be correctly reflected in h0, h1, h2, h3, h4, h5, h6, h7, h8, h9, h10, h11
  //   * No two differing bits will be reflected the same way in h0, h1, h2, h3, h4, h5, h6, h7, h8, h9, h10, h11
  //   * Any set of differing bits will be correctly reflected by h0, h1, h2, h3, h4, h5, h6, h7, h8, h9, h10, h11
  //
  static inline void Mix(
    const uint64_t *data,
    uint64_t &s0, uint64_t &s1, uint64_t &s2, uint64_t &s3,
    uint64_t &s4, uint64_t &s5, uint64_t &s6, uint64_t &s7,
    uint64_t &s8, uint64_t &s9, uint64_t &s10,uint64_t &s11)
  {
    s0 += data[0];    s2 ^= s10;  s11 ^= s0;   s0 = Rot64(s0,11);    s11 += s1;
    s1 += data[1];    s3 ^= s11;  s0 ^= s1;    s1 = Rot64(s1,32);    s0 += s2;
    s2 += data[2];    s4 ^= s0;   s1 ^= s2;    s2 = Rot64(s2,43);    s1 += s3;
    s3 += data[3];    s5 ^= s1;   s2 ^= s3;    s3 = Rot64(s3,31);    s2 += s4;
    s4 += data[4];    s6 ^= s2;   s3 ^= s4;    s4 = Rot64(s4,17);    s3 += s5;
    s5 += data[5];    s7 ^= s3;   s4 ^= s5;    s5 = Rot64(s5,28);    s4 += s6;
    s6 += data[6];    s8 ^= s4;   s5 ^= s6;    s6 = Rot64(s6,39);    s5 += s7;
    s7 += data[7];    s9 ^= s5;   s6 ^= s7;    s7 = Rot64(s7,57);    s6 += s8;
    s8 += data[8];    s10 ^= s6;  s7 ^= s8;    s8 = Rot64(s8,55);    s7 += s9;
    s9 += data[9];    s11 ^= s7;  s8 ^= s9;    s9 = Rot64(s9,54);    s8 += s10;
    s10 += data[10];  s0 ^= s8;   s9 ^= s10;   s10 = Rot64(s10,22);  s9 += s11;
    s11 += data[11];  s1 ^= s9;   s10 ^= s11;  s11 = Rot64(s11,46);  s10 += s0;
  }

  //
  // EndPartial: Mix all 12 inputs together so that h0, h1 are a hash of them all.
  //
  // Unlike Mix(), EndPartial() is *not* reversible, and it uses slightly
  // different constants.
  //
  static inline void EndPartial(
    uint64_t &h0, uint64_t &h1, uint64_t &h2, uint64_t &h3,
    uint64_t &h4, uint64_t &h5, uint64_t &h6, uint64_t &h7,
    uint64_t &h8, uint64_t &h9, uint64_t &h10,uint64_t &h11)
  {
    h11+= h1;    h2 ^= h11;   h1 = Rot64(h1,44);
    h0 += h2;    h3 ^= h0;    h2 = Rot64(h2,15);
    h1 += h3;    h4 ^= h1;    h3 = Rot64(h3,34);
    h2 += h4;    h5 ^= h2;    h4 = Rot64(h4,21);
    h3 += h5;    h6 ^= h3;    h5 = Rot64(h5,38);
    h4 += h6;    h7 ^= h4;    h6 = Rot64(h6,33);
    h5 += h7;    h8 ^= h5;    h7 = Rot64(h7,10);
    h6 += h8;    h9 ^= h6;    h8 = Rot64(h8,13);
    h7 += h9;    h10^= h7;    h9 = Rot64(h9,38);
    h8 += h10;   h11^= h8;    h10= Rot64(h10,53);
    h9 += h11;   h0 ^= h9;    h11= Rot64(h11,42);
    h10+= h0;    h1 ^= h10;   h0 = Rot64(h0,54);
  }

  //
  // End: do some final mixing on the last 12 uint64_t's
  //
  static inline void End(
    const uint64_t *data,
    uint64_t &h0, uint64_t &h1, uint64_t &h2, uint64_t &h3,
    uint64_t &h4, uint64_t &h5, uint64_t &h6, uint64_t &h7,
    uint64_t &h8, uint64_t &h9, uint64_t &h10,uint64_t &h11)
  {
    h0 += data[0];   h1 += data[1];   h2 += data[2];   h3 += data[3];
    h4 += data[4];   h5 += data[5];   h6 += data[6];   h7 += data[7];
    h8 += data[8];   h9 += data[9];   h10 += data[10]; h11 += data[11];
    EndPartial(h0,h1,h2,h3,h4,h5,h6,h7,h8,h9,h10,h11);
    EndPartial(h0,h1,h2,h3,h4,h5,h6,h7,h8,h9,h10,h11);
    EndPartial(h0,h1,h2,h3,h4,h5,h6,h7,h8,h9,h10,h11);
  }

  //
  // Short: hash a short message (< sc_bufSize bytes)
  //
  static void Short(
    const void *message,
    size_t length,
    uint64_t *hash1,
    uint64_t *hash2);

  //
  // ShortEnd: do End() for a short message
  //
  static inline void ShortEnd(
    uint64_t &h0, uint64_t &h1, uint64_t &h2, uint64_t &h3)
  {
    uint64_t h4 = 0, h5 = 0, h6 = 0, h7 = 0;
    uint64_t h8 = 0, h9 = 0, h10 = 0, h11 = 0;

    h11+= h1;    h2 ^= h11;   h1 = Rot64(h1,44);
    h0 += h2;    h3 ^= h0;    h2 = Rot64(h2,15);
    h1 += h3;    h4 ^= h1;    h3 = Rot64(h3,34);
    h2 += h4;    h5 ^= h2;    h4 = Rot64(h4,21);
    h3 += h5;    h6 ^= h3;    h5 = Rot64(h5,38);
    h4 += h6;    h7 ^= h4;    h6 = Rot64(h6,33);
    h5 += h7;    h8 ^= h5;    h7 = Rot64(h7,10);
    h6 += h8;    h9 ^= h6;    h8 = Rot64(h8,13);
    h7 += h9;    h10^= h7;    h9 = Rot64(h9,38);
    h8 += h10;   h11^= h8;    h10= Rot64(h10,53);
    h9 += h11;   h0 ^= h9;    h11= Rot64(h11,42);
    h10+= h0;    h1 ^= h10;   h0 = Rot64(h0,54);
  }
};

//
// hash_combine: Combine a hash value with another value
// Uses Boost's hash_combine algorithm
//
// The golden ratio constant helps ensure good distribution of hash values.
// Formula: seed ^= hash + 0x9e3779b9 + (seed << 6) + (seed >> 2)
//
inline void hash_combine(size_t& seed, size_t hash)
{
  // 32-bit golden ratio: 0x9e3779b9
  // 64-bit golden ratio: 0x9e3779b97f4a7c15
  constexpr size_t golden_ratio = (sizeof(size_t) == 8)
    ? 0x9e3779b97f4a7c15ULL
    : 0x9e3779b9UL;

  seed ^= hash + golden_ratio + (seed << 6) + (seed >> 2);
}

//
// HashState: Stateful hash builder for combining multiple values
//
// Usage:
//   HashState h;
//   h.Combine(value1);
//   h.Combine(value2);
//   size_t result = h.Get();
//
class HashState {
public:
  HashState();
  explicit HashState(size_t seed);

  // Combine a size_t hash value
  void Combine(size_t hash);

  // Combine an integer type
  template<typename T>
  typename std::enable_if<std::is_integral<T>::value, void>::type
  Combine(T value) {
    hash_combine(_state, static_cast<size_t>(value));
  }

  // Combine a floating point type
  template<typename T>
  typename std::enable_if<std::is_floating_point<T>::value, void>::type
  Combine(T value) {
    // Hash the bit representation of floating point values
    union {
      T f;
      size_t i;
    } u;
    u.i = 0; // zero-initialize to avoid uninitialized bits
    u.f = value;
    hash_combine(_state, u.i);
  }

  // Combine a pointer
  template<typename T>
  void Combine(T* ptr) {
    hash_combine(_state, reinterpret_cast<size_t>(ptr));
  }

  // Combine a data buffer using SpookyHash
  void CombineBytes(const void* data, size_t length);

  // Get the current hash value
  size_t Get() const;

  // Reset to initial state
  void Reset(size_t seed = 0);

private:
  size_t _state;
};

}  // namespace lightusd
