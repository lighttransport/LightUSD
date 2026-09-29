// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - USDC Crate Reader Implementation

#include "crate-reader-internal.hh"
#include "../layer/array-edit.hh"
#include "../writer/value-printer.hh"
#include "safe-arithmetic.hh"

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace lightusd {
namespace next {

#if defined(LIGHTUSD_ENABLE_THREAD)
thread_local CrateReader::Impl::ThreadDecodeCtx*
    CrateReader::Impl::tls_decode_ctx_ = nullptr;
#endif

// ============================================================
// Main value unpacker using switch statement
// ============================================================

// VtArrayEdit (crate >= 0.14.0). Layout at the rep's payload offset:
//   ValueRep valuesRep   (8 bytes)  -- packed VtArray<T> of literal elements
//   ValueRep indexesRep  (8 bytes)  -- packed VtInt64Array (op stream)
//   bool     isDense     (1 byte)   -- legacy, discarded
// payload == 0 is the identity (empty) edit. The op stream uses OpenUSD's
// Vt_ArrayEditOps encoding: a packed [count:56 | op:8] word followed by
// `arity(op) * count` int64 operands.
bool CrateReader::Impl::UnpackArrayEditData(ValueRep rep, ArrayEditData* out) {
  if (!out) return false;
  out->ops.clear();
  if (rep.payload() == 0) {
    return true;  // identity edit
  }
  if (!reader()->seek(static_cast<size_t>(rep.payload()))) {
    AddWarning("Invalid offset for array-edit value");
    return false;
  }
  uint64_t values_raw = 0, indexes_raw = 0;
  uint8_t is_dense = 0;
  if (!reader()->read_u64(values_raw) || !reader()->read_u64(indexes_raw) ||
      !reader()->read_u8(is_dense)) {
    AddWarning("Truncated array-edit tuple");
    return false;
  }
  (void)is_dense;  // legacy field

  const ValueRep values_rep(values_raw);
  const ValueRep indexes_rep(indexes_raw);
  if (values_rep.is_array_edit() || indexes_rep.is_array_edit()) {
    AddWarning("Nested array-edit reps are not supported");
    return false;
  }

  // Literal elements -> canonical usda element texts.
  std::vector<std::string> literal_texts;
  {
    Value literals;
    if (!UnpackValue(values_rep, literals)) {
      AddWarning("Failed to decode array-edit literals");
      return false;
    }
    if (literals.is_array() && literals.array_size() > 0) {
      PrintOptions popts;
      popts.compact = true;
      if (!SplitPrintedArrayElements(PrintValue(literals, popts),
                                     &literal_texts)) {
        AddWarning("Failed to decompose array-edit literals");
        return false;
      }
    }
  }

  // Op instruction stream (int64[]).
  std::vector<int64_t> ins;
  {
    Value idx_val;
    if (!UnpackValue(indexes_rep, idx_val)) {
      AddWarning("Failed to decode array-edit op stream");
      return false;
    }
    if (const std::vector<int64_t>* v = idx_val.as_int64_array()) {
      ins = *v;
    }
  }

  auto lit = [&](int64_t i, std::string* txt) -> bool {
    if (i < 0 || static_cast<size_t>(i) >= literal_texts.size()) return false;
    *txt = literal_texts[static_cast<size_t>(i)];
    return true;
  };

  size_t pos = 0;
  while (pos < ins.size()) {
    const uint64_t word = static_cast<uint64_t>(ins[pos++]);
    const uint8_t opcode = static_cast<uint8_t>((word >> 56) & 0xffu);
    int64_t count =
        static_cast<int64_t>(word & 0x00FFFFFFFFFFFFFFull);
    // pxr op codes (Vt_ArrayEditOps::Op): 0 WriteLiteral, 1 WriteRef,
    // 2 InsertLiteral, 3 InsertRef, 4 EraseRef, 5 MinSize, 6 MinSizeFill,
    // 7 SetSize, 8 SetSizeFill, 9 MaxSize.
    const bool two_args = (opcode == 0 || opcode == 1 || opcode == 2 ||
                           opcode == 3 || opcode == 6 || opcode == 8);
    if (opcode > 9) {
      AddWarning("Unknown array-edit op code " + std::to_string(opcode));
      return false;
    }
    for (; count > 0; --count) {
      const size_t arity = two_args ? 2u : 1u;
      if (pos + arity > ins.size()) {
        AddWarning("Truncated array-edit op stream");
        return false;
      }
      const int64_t a1 = ins[pos];
      const int64_t a2 = two_args ? ins[pos + 1] : 0;
      pos += arity;
      ArrayEditOpRec rec;
      bool ok = true;
      switch (opcode) {
        case 0:  // WriteLiteral: a1 literal idx, a2 dst
          rec.kind = ArrayEditOpRec::WriteLiteral;
          rec.a2 = a2;
          ok = lit(a1, &rec.literal);
          break;
        case 1:  // WriteRef
          rec.kind = ArrayEditOpRec::WriteRef;
          rec.a1 = a1;
          rec.a2 = a2;
          break;
        case 2:  // InsertLiteral: a1 literal idx, a2 dst
          rec.kind = ArrayEditOpRec::InsertLiteral;
          rec.a2 = a2;
          ok = lit(a1, &rec.literal);
          break;
        case 3:  // InsertRef
          rec.kind = ArrayEditOpRec::InsertRef;
          rec.a1 = a1;
          rec.a2 = a2;
          break;
        case 4:  // EraseRef
          rec.kind = ArrayEditOpRec::Erase;
          rec.a1 = a1;
          break;
        case 5:  // MinSize
          rec.kind = ArrayEditOpRec::MinSize;
          rec.a1 = a1;
          break;
        case 6:  // MinSizeFill: a1 size, a2 literal idx
          rec.kind = ArrayEditOpRec::MinSize;
          rec.a1 = a1;
          rec.has_fill = true;
          ok = lit(a2, &rec.literal);
          break;
        case 7:  // SetSize
          rec.kind = ArrayEditOpRec::SetSize;
          rec.a1 = a1;
          break;
        case 8:  // SetSizeFill
          rec.kind = ArrayEditOpRec::SetSize;
          rec.a1 = a1;
          rec.has_fill = true;
          ok = lit(a2, &rec.literal);
          break;
        default:  // 9 MaxSize
          rec.kind = ArrayEditOpRec::MaxSize;
          rec.a1 = a1;
          break;
      }
      if (!ok) {
        AddWarning("Array-edit literal index out of range");
        return false;
      }
      out->ops.push_back(std::move(rec));
    }
  }
  return true;
}

bool CrateReader::Impl::UnpackValue(ValueRep rep, Value& out, int depth) {
  // Shared ceiling with DecodeDictionary — see the header comment.
  if (depth > kMaxValueNestDepth) {
    AddWarning("Value nesting too deep; value dropped");
    return false;
  }

  // VtArrayEdit reps (crate 0.14) are not VALUES: the attribute-spec decode
  // intercepts them (UnpackArrayEditData) and stores the structured edit on
  // the PrimSpec. Reaching here means an edit rep appeared in a context that
  // has no edit storage (timeSamples, dict entry, ...) — drop it rather than
  // let it masquerade as a scalar whose payload is the edit tuple header.
  if (rep.is_array_edit()) {
    AddWarning("VtArrayEdit value in an unsupported context; value dropped");
    return false;
  }

  CrateTypeId type_id = rep.type_id();

  if (rep.is_array()) {
    return UnpackArray(rep, out);
  }

  switch (type_id) {
    case CrateTypeId::Bool: return UnpackBool(rep, out);
    case CrateTypeId::Int: return UnpackInt(rep, out);
    case CrateTypeId::UInt: return UnpackUInt(rep, out);
    case CrateTypeId::UChar: {
      // Keep the uchar type identity (was mutated to uint).
      if (!UnpackUInt(rep, out)) return false;
      if (const uint32_t* u = out.as_uint()) {
        const uint8_t b = static_cast<uint8_t>(*u & 0xFFu);
        out = Value::MakeFromRaw(TypeId::UChar, &b);
      }
      return true;
    }
    case CrateTypeId::Int64: return UnpackInt64(rep, out);
    case CrateTypeId::UInt64: return UnpackUInt64(rep, out);
    case CrateTypeId::Float: return UnpackFloat(rep, out);
    case CrateTypeId::Double: return UnpackDouble(rep, out);
    case CrateTypeId::Token: return UnpackToken(rep, out);
    case CrateTypeId::String: return UnpackString(rep, out);
    case CrateTypeId::AssetPath: return UnpackAssetPath(rep, out);
    case CrateTypeId::Vec2f: return UnpackVec2f(rep, out);
    case CrateTypeId::Vec3f: return UnpackVec3f(rep, out);
    case CrateTypeId::Vec4f: return UnpackVec4f(rep, out);
    case CrateTypeId::Vec2d: return UnpackVec2d(rep, out);
    case CrateTypeId::Vec3d: return UnpackVec3d(rep, out);
    case CrateTypeId::Vec4d: return UnpackVec4d(rep, out);
    case CrateTypeId::Quatf: return UnpackQuatf(rep, out);
    case CrateTypeId::Quatd: return UnpackQuatd(rep, out);
    case CrateTypeId::Matrix2d: return UnpackMatrix2d(rep, out);
    case CrateTypeId::Matrix3d: return UnpackMatrix3d(rep, out);
    case CrateTypeId::Matrix4d: return UnpackMatrix4d(rep, out);
    case CrateTypeId::Specifier: return UnpackSpecifier(rep, out);
    case CrateTypeId::Permission: return UnpackPermission(rep, out);
    case CrateTypeId::Variability: return UnpackVariability(rep, out);
    case CrateTypeId::TimeCode: {
      // Decode like a double but preserve the TimeCode type identity
      // (a crate rewrite must not silently mutate timecode -> double).
      if (!UnpackDouble(rep, out)) return false;
      if (const double* d = out.as_double()) {
        out = Value::MakeFromRaw(TypeId::TimeCode, d);
      }
      return true;
    }
    case CrateTypeId::TimeSamples: return UnpackTimeSamples(rep, out);
    case CrateTypeId::UnregisteredValue: {
      if (rep.is_inlined() || rep.payload() == 0) return false;
      const uint64_t wrapper = rep.payload_as_offset();
      if (!reader()->seek(static_cast<size_t>(wrapper))) return false;
      int64_t relative = 0;
      if (!reader()->read_i64(relative) || relative < 0) return false;
      uint64_t nested_pos = 0;
      if (static_cast<uint64_t>(relative) >
              (std::numeric_limits<uint64_t>::max)() - wrapper) {
        return false;
      }
      nested_pos = wrapper + static_cast<uint64_t>(relative);
      if (!reader()->seek(static_cast<size_t>(nested_pos))) {
        return false;
      }
      uint64_t nested_raw = 0;
      if (!reader()->read_u64(nested_raw)) return false;
      const ValueRep nested(nested_raw);
      // OpenUSD stores the authored source spelling as a nested String (and
      // may use Dictionary for registered dictionary-shaped extensions).
      if (nested.type_id() != CrateTypeId::String &&
          nested.type_id() != CrateTypeId::Dictionary) {
        return false;
      }
      // Count the wrapper hop: this re-entry is exactly what let a cycle of
      // UnregisteredValue-wrapped dictionaries recurse without bound.
      return UnpackValue(nested, out, depth + 1);
    }
    case CrateTypeId::Half: return UnpackHalf(rep, out);
    case CrateTypeId::Vec2i: return UnpackVec2i(rep, out);
    case CrateTypeId::Vec3i: return UnpackVec3i(rep, out);
    case CrateTypeId::Vec4i: return UnpackVec4i(rep, out);
    case CrateTypeId::Vec2h: return UnpackVec2h(rep, out);
    case CrateTypeId::Vec3h: return UnpackVec3h(rep, out);
    case CrateTypeId::Vec4h: return UnpackVec4h(rep, out);
    case CrateTypeId::Quath: return UnpackQuath(rep, out);

    case CrateTypeId::ReferenceListOp:
    case CrateTypeId::PayloadListOp: {
      std::vector<std::string> arcs;
      if (!DecodeReferenceListOp(rep, type_id == CrateTypeId::PayloadListOp,
                                 arcs)) {
        return false;
      }
      out = Value::MakeTokenArray(std::move(arcs));
      return true;
    }

    case CrateTypeId::Invalid:     // an empty VtValue (e.g. an empty dict entry)
      out = Value();
      return true;

    case CrateTypeId::ValueBlock:  // a blocked (`= None`) value: a real opinion
      out = Value::MakeBlock();    // that suppresses weaker values and re-emits
      return true;                 // `= None` (not a declared-only attribute).

    // TokenVector / StringVector / DoubleVector: std::vector<T> stored as a
    // non-array value ([u64 count][elements]); an empty vector inlines as
    // payload 0.
    case CrateTypeId::TokenVector:
    case CrateTypeId::StringVector:
      return UnpackTokenOrStringVector(rep, type_id, out);

    case CrateTypeId::DoubleVector:
      return UnpackDoubleVector(rep, out);

    // std::vector<SdfLayerOffset>: [u64 count][f64 offset, f64 scale]*count.
    // Surfaced as a flat double array (pairs); BuildStage pairs it with
    // subLayers into LayerMeta::subLayerOffsets.
    case CrateTypeId::LayerOffsetVector: {
      if (rep.payload() == 0) {
        out = Value::MakeDoubleArray(std::vector<double>());
        return true;
      }
      if (!SeekToPayload(reader(), rep)) return false;
      uint64_t n = 0;
      if (!reader()->read_u64(n)) return false;
      if (n > options_.max_array_elements) return false;
      if (!reader()->has_elements(static_cast<size_t>(n), 16)) return false;
      if (n > static_cast<uint64_t>((std::numeric_limits<size_t>::max)())) {
        return false;
      }
      size_t scalar_count = 0;
      size_t byte_count = 0;
      if (!safe::mul(static_cast<size_t>(n), size_t{2}, &scalar_count) ||
          !safe::mul(scalar_count, sizeof(double), &byte_count)) {
        return false;
      }
      std::vector<double> vals(scalar_count);
      if (n && !reader()->read(vals.data(), byte_count)) return false;
      out = Value::MakeDoubleArray(std::move(vals));
      return true;
    }

    // Path-valued fields: inheritPaths / specializes arcs (PathListOp) and
    // raw path vectors. Flattened to a token array of path strings; BuildStage
    // routes them into PrimSpecMeta.
    case CrateTypeId::PathVector:
    case CrateTypeId::PathListOp: {
      std::vector<std::string> paths;
      if (!DecodePathTargets(rep, paths, /*with_markers=*/true)) return false;
      // Arc-string form ("</Base>"), matching what the usda parser stores in
      // PrimSpecMeta::inherits/specializes. Sublist markers ("\x01?") pass
      // through unbracketed for BuildStage's list-op reconstruction.
      for (std::string& p : paths) {
        if (!p.empty() && p[0] != '\x01') p = "<" + p + ">";
      }
      out = Value::MakeTokenArray(std::move(paths));
      return true;
    }

    // Single (non-listOp) SdfPayload item: [u32 assetIdx][u32 pathIdx]
    // (+ [f64 offset][f64 scale] from crate 0.8). Common in pxr-written 0.8.x
    // crates for `payload = @asset@</P>`.
    case CrateTypeId::Payload: {
      if (rep.payload() == 0) {
        out = Value::MakeTokenArray(std::vector<std::string>());
        return true;
      }
      if (!SeekToPayload(reader(), rep)) return false;
      uint32_t asset_idx = 0, path_idx = 0;
      if (!reader()->read_u32(asset_idx) || !reader()->read_u32(path_idx)) {
        return false;
      }
      double offset = 0.0, scale = 1.0;
      if (version_.minor >= 8) {
        if (!reader()->read_f64(offset) || !reader()->read_f64(scale)) {
          return false;
        }
        if (!std::isfinite(offset) || !std::isfinite(scale)) return false;
      }
      std::string asset;
      if (!GetString(asset_idx, asset) || path_idx >= paths_.size()) {
        return false;
      }
      const std::string prim = paths_.str(path_idx);
      // Internal arcs (no asset) render as "</Prim>", matching the usda parser.
      std::string arc;
      if (!asset.empty()) arc = "@" + asset + "@";
      if (!prim.empty() && prim != "/") arc += "<" + prim + ">";
      if (offset != 0.0 || scale != 1.0) {
        arc += "?layerOffset=" + std::to_string(offset) + ":" +
               std::to_string(scale);
      }
      out = Value::MakeTokenArray({std::move(arc)});
      return true;
    }

    case CrateTypeId::TokenListOp:
    case CrateTypeId::StringListOp: {
      std::vector<std::string> toks;
      if (!DecodeTokenListOp(rep, toks)) return false;
      out = Value::MakeTokenArray(std::move(toks));
      return true;
    }

    case CrateTypeId::PathExpression: {
      // SdfPathExpression (crate >= 0.10): the expression text as a u32
      // string index in the value stream.
      if (!SeekToPayload(reader(), rep)) {
        return false;
      }
      uint32_t sidx = 0;
      if (!reader()->read_u32(sidx)) return false;
      std::string text;
      if (!GetString(sidx, text)) return false;
      out = Value::MakeStringLike(text, TypeId::PathExpression);
      return true;
    }

    case CrateTypeId::Dictionary:
      return DecodeDictionary(rep, out, depth);

    case CrateTypeId::Relocates: {
      // SdfRelocates (crate >= 0.11): [u64 count][(u32 src, u32 dst)*].
      // Surface as a flat token array of [src, dst, ...] pairs; the stage
      // builder folds them into PrimSpecMeta::relocates.
      if (rep.payload() == 0) { out = Value::MakeTokenArray({}); return true; }
      if (!SeekToPayload(reader(), rep)) {
        return false;
      }
      uint64_t n = 0;
      if (!reader()->read_u64(n)) return false;
      if (n > options_.max_array_elements) return false;
      if (n > static_cast<uint64_t>((std::numeric_limits<size_t>::max)())) {
        return false;
      }
      size_t pair_count = 0;
      if (!safe::mul(static_cast<size_t>(n), size_t{2}, &pair_count) ||
          !reader()->has_elements(static_cast<size_t>(n), size_t{8})) {
        return false;
      }
      std::vector<std::string> pairs;
      pairs.reserve(pair_count);
      for (uint64_t i = 0; i < n; ++i) {
        uint32_t src = 0, dst = 0;
        if (!reader()->read_u32(src) || !reader()->read_u32(dst)) return false;
        if (src >= paths_.size() || dst >= paths_.size()) return false;
        pairs.push_back(paths_.str(src));
        pairs.push_back(paths_.str(dst));
      }
      out = Value::MakeTokenArray(std::move(pairs));
      return true;
    }

    default:
      AddWarning(std::string("Unsupported value type: ") + CrateTypeIdName(type_id));
      return false;
  }
}

}  // namespace next
}  // namespace lightusd
