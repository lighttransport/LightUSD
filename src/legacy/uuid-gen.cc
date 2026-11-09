// SPDX-License-Identifier: Apache 2.0
// Copyright 2025-Present Light Transport Entertainment, Inc.
//
// Simple UUID Version 4 generator implementation
//
#include "uuid-gen.hh"

#include <cstdint>
#include <random>
#include <sstream>
#include <iomanip>

namespace lightusd {

UUIDGenerator::UUIDGenerator()
    : rd_(), gen_(rd_()), dis_(0, 0xFFFFFFFF) {
}

std::string UUIDGenerator::generate() {
    // Generate 4 32-bit random numbers (128 bits total)
    uint32_t data[4];
    for (int i = 0; i < 4; ++i) {
        data[i] = dis_(gen_);
    }

    // Convert to bytes for easier manipulation
    uint8_t bytes[16];
    for (int i = 0; i < 4; ++i) {
        bytes[i * 4 + 0] = (data[i] >> 24) & 0xFF;
        bytes[i * 4 + 1] = (data[i] >> 16) & 0xFF;
        bytes[i * 4 + 2] = (data[i] >> 8) & 0xFF;
        bytes[i * 4 + 3] = data[i] & 0xFF;
    }

    // Set version (4) in the most significant 4 bits of the 7th byte
    bytes[6] = (bytes[6] & 0x0F) | 0x40;

    // Set variant (10) in the most significant 2 bits of the 9th byte
    bytes[8] = (bytes[8] & 0x3F) | 0x80;

    // Format as UUID string: xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');

    // First group: 8 hex digits
    for (int i = 0; i < 4; ++i) {
        oss << std::setw(2) << static_cast<int>(bytes[i]);
    }
    oss << '-';

    // Second group: 4 hex digits
    for (int i = 4; i < 6; ++i) {
        oss << std::setw(2) << static_cast<int>(bytes[i]);
    }
    oss << '-';

    // Third group: 4 hex digits (with version)
    for (int i = 6; i < 8; ++i) {
        oss << std::setw(2) << static_cast<int>(bytes[i]);
    }
    oss << '-';

    // Fourth group: 4 hex digits (with variant)
    for (int i = 8; i < 10; ++i) {
        oss << std::setw(2) << static_cast<int>(bytes[i]);
    }
    oss << '-';

    // Fifth group: 12 hex digits
    for (int i = 10; i < 16; ++i) {
        oss << std::setw(2) << static_cast<int>(bytes[i]);
    }

    return oss.str();
}

std::string UUIDGenerator::generateUUID() {
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wexit-time-destructors"
#endif

    static UUIDGenerator generator;

#ifdef __clang__
#pragma clang diagnostic pop
#endif

    return generator.generate();
}

std::string generateUUID() {
    return UUIDGenerator::generateUUID();
}

}  // namespace lightusd
