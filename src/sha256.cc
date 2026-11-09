#include <cstdint>
#include <cstring>
#include <limits>

#include "sha256.hh"

namespace lightusd {

namespace {

constexpr uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

inline uint32_t rotr(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32 - n));
}

inline uint32_t ch(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ (~x & z);
}

inline uint32_t maj(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ (x & z) ^ (y & z);
}

inline uint32_t sigma0(uint32_t x) {
    return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22);
}

inline uint32_t sigma1(uint32_t x) {
    return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25);
}

inline uint32_t gamma0(uint32_t x) {
    return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3);
}

inline uint32_t gamma1(uint32_t x) {
    return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10);
}

inline uint32_t pack32(const uint8_t *str) {
    return (static_cast<uint32_t>(str[0]) << 24) |
           (static_cast<uint32_t>(str[1]) << 16) |
           (static_cast<uint32_t>(str[2]) << 8) |
           static_cast<uint32_t>(str[3]);
}

void sha256_transform(uint32_t state[8], const uint8_t block[64]) {
    uint32_t W[64];
    uint32_t a, b, c, d, e, f, g, h;
    uint32_t T1, T2;

    for (int i = 0; i < 16; i++) {
        W[i] = pack32(&block[i * 4]);
    }

    for (int i = 16; i < 64; i++) {
        W[i] = gamma1(W[i - 2]) + W[i - 7] + gamma0(W[i - 15]) + W[i - 16];
    }

    a = state[0];
    b = state[1];
    c = state[2];
    d = state[3];
    e = state[4];
    f = state[5];
    g = state[6];
    h = state[7];

    for (int i = 0; i < 64; i++) {
        T1 = h + sigma1(e) + ch(e, f, g) + K[i] + W[i];
        T2 = sigma0(a) + maj(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + T1;
        d = c;
        c = b;
        b = a;
        a = T1 + T2;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

}  // anonymous namespace

std::string sha256(const char *binary, size_t size) {
    uint32_t state[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };

    // Keep rejecting unrepresentable input sizes, including the historical
    // SIZE_MAX padding-overflow range. SHA-256 encodes a 64-bit bit count.
    if ((!binary && size) ||
        size > (std::numeric_limits<size_t>::max)() - 72) {
        return "";
    }

    const uint64_t bit_len = static_cast<uint64_t>(size) * 8;
    if (bit_len / 8 != size) return "";
    const uint8_t *data = reinterpret_cast<const uint8_t *>(binary);
    size_t remaining = size;
    while (remaining >= 64) {
        sha256_transform(state, data);
        data += 64;
        remaining -= 64;
    }

    // Only the final one or two blocks require padding. Hash full input
    // blocks directly instead of allocating a second copy of the asset.
    uint8_t tail[128] = {};
    if (remaining) std::memcpy(tail, data, remaining);
    tail[remaining] = 0x80;
    const size_t padded_size = remaining < 56 ? 64 : 128;
    for (unsigned i = 0; i < 8; ++i) {
        tail[padded_size - 8 + i] =
            static_cast<uint8_t>((bit_len >> (56 - i * 8)) & 0xff);
    }
    sha256_transform(state, tail);
    if (padded_size == 128) sha256_transform(state, tail + 64);

    constexpr char hex[] = "0123456789abcdef";
    std::string result(64, '0');
    for (size_t i = 0; i < result.size(); ++i) {
        const unsigned shift = 28u - static_cast<unsigned>(i % 8) * 4u;
        result[i] = hex[(state[i / 8] >> shift) & 0xfu];
    }
    return result;
}

}  // namespace lightusd
