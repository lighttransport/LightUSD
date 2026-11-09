// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// spline-binary.hh
//
// Encoder/decoder for the OpenUSD `TsSpline` binary blob -- the
// `vector<uint8_t>` payload of Crate value type 59. Byte-compatible with
// pxr/base/ts/binary.cpp (little-endian platforms, as the rest of the Crate
// reader/writer assumes).
//
// This module is self-contained: it depends only on the type-erased
// primvar::PrimVar::SplineData (+ value-types) and the standard library. The
// surrounding Crate framing (`uint64 blobSize` + blob + `uint64 customDataCount`
// + entries) is handled by the Crate reader/writer.
//
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "primvar.hh"  // primvar::PrimVar::SplineData

namespace lightusd {

// Encode SplineData into the OpenUSD ts-binary blob. The value type
// (double/float/half) is inferred from the knots' stored value type; an empty
// spline encodes with the "unspecified" (double) descriptor.
bool EncodeSplineToBinary(const primvar::PrimVar::SplineData &sd,
                          std::vector<uint8_t> *out, std::string *err = nullptr);

// Decode an OpenUSD ts-binary blob (without the surrounding Crate framing) into
// SplineData.
bool DecodeSplineFromBinary(const uint8_t *data, size_t size,
                            primvar::PrimVar::SplineData *out,
                            std::string *err = nullptr);

// The ts-binary format version required to encode `sd`:
//   1 : initial spline format (no tangent algorithms).
//   2 : at least one knot has a non-None tangent algorithm (AutoEase/Custom).
// Version 2 adds a per-knot algorithmByte and requires Crate version 0.13.0.
// Mirrors pxr Ts_BinaryDataAccess::GetBinaryFormatVersion.
uint8_t SplineBinaryFormatVersion(const primvar::PrimVar::SplineData &sd);

}  // namespace lightusd
