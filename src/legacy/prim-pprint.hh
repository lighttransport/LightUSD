// SPDX-License-Identifier: Apache 2.0
// Copyright 2022 - 2023, Syoyo Fujita.
// Copyright 2023 - Present, Light Transport Entertainment Inc.

#pragma once

#include <string>
#include <cstdint>

#include "core/prim.hh"       // Prim class (transitively: prim-enums, prim-metas, path)
#include "core/prim-spec.hh"  // PrimSpec, ReferenceList, PayloadList, LayerOffset

namespace lightusd {

// Forward declarations
template <size_t ChunkSize, size_t Alignment> class ChunkedStreamWriter;
class StreamWriter;

namespace prim {

//
// Impelemnted in pprinter.cc at the moment.
//
std::string print_references(const ReferenceList &references, const uint32_t indent);
std::string print_payload(const PayloadList &payload, const uint32_t indent);
std::string print_layeroffset(const LayerOffset &layeroffset, const uint32_t indent);

std::string print_prim(const Prim &prim, const uint32_t indent=0);
std::string print_primspec(const PrimSpec &primspec, const uint32_t indent=0);

///
/// ChunkedStreamWriter versions for efficient printing (large outputs)
///
template <size_t ChunkSize = 4096, size_t Alignment = 16>
void print_prim(ChunkedStreamWriter<ChunkSize, Alignment>& writer, const Prim &prim, const uint32_t indent=0);

template <size_t ChunkSize = 4096, size_t Alignment = 16>
void print_primspec(ChunkedStreamWriter<ChunkSize, Alignment>& writer, const PrimSpec &primspec, const uint32_t indent=0);

// The default specializations are defined explicitly in pprinter.cc.
extern template void print_prim<4096, 16>(
    ChunkedStreamWriter<4096, 16>& writer, const Prim& prim,
    const uint32_t indent);
extern template void print_primspec<4096, 16>(
    ChunkedStreamWriter<4096, 16>& writer, const PrimSpec& primspec,
    const uint32_t indent);

///
/// StreamWriter versions for efficient printing
///
void print_prim(StreamWriter& writer, const Prim &prim, const uint32_t indent=0);
void print_primspec(StreamWriter& writer, const PrimSpec &primspec, const uint32_t indent=0);

} // namespace prim

inline std::string to_string(const Prim &prim) {
  return prim::print_prim(prim);
}

inline std::string to_string(const PrimSpec &primspec) {
  return prim::print_primspec(primspec);
}

} // namespace lightusd
