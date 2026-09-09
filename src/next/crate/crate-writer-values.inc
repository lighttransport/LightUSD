// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - CrateWriter::Impl VALUE encoding helpers.
// Included inside CrateWriter::Impl private section.

  // ============================================================
  // Value encoding
  // ============================================================

  /// Store data in VALUE section and return its offset.
  /// Returns the offset from the START of the VALUE section.
  uint64_t StoreValueData(const void* data, size_t size, TypeId type) {
    DataBlock block;
    block.type = type;
    block.data.resize(size);
    std::memcpy(block.data.data(), data, size);
    return InternBlock(std::move(block));
  }

  // Encode a Value into a ValueRep.
  // For inlinable scalars, payload contains the value directly.
  // For larger values, payload contains an index into value_data_.
  // Returns the ValueRep and records the field's VALUE section offset.
  // Encode a VtDictionary in pxr binary form (OpenUSD WriteMap + _RecursiveWrite
  // of each Write(VtValue)):
  //   [u64 count] then per entry [u32 keyStrIdx][i64 recOffset=8][u64 ValueRep]
  // The recursive forward offset is a constant 8: the ValueRep immediately
  // follows it and the rep's payload is an absolute offset to the value's data
  // block (pxr's reader follows that payload, so no inline data is required).
  // Embedded non-inline reps are relocated by _PatchDictBlock once block offsets
  // are known. The block is patched in place, so it must NOT be deduped/shared
  // (push directly, do not InternBlock).
  ValueRep EncodeDictionary(const Dict& d) {
    std::vector<uint8_t> blk;
    auto put_u64 = [&](uint64_t v) {
      for (int i = 0; i < 8; ++i) blk.push_back(static_cast<uint8_t>(v >> (i * 8)));
    };
    auto put_u32 = [&](uint32_t v) {
      for (int i = 0; i < 4; ++i) blk.push_back(static_cast<uint8_t>(v >> (i * 8)));
    };
    put_u64(static_cast<uint64_t>(d.entries().size()));
    for (const auto& kv : d.entries()) {
      uint32_t kidx = InternString(kv.first);
      ValueRep vr = EncodeValue(kv.second);  // recursive (nested dicts/arrays)
      put_u32(kidx);
      put_u64(static_cast<uint64_t>(static_cast<int64_t>(8)));  // recOffset = 8
      put_u64(vr.raw());
    }
    uint64_t idx = value_data_.size();
    value_data_.push_back({TypeId::Dictionary, std::move(blk)});
    return ValueRep::Make(CrateTypeId::Dictionary, idx, false, false);
  }

  ValueRep EncodeUnregisteredValue(const std::string& source) {
    const ValueRep nested = EncodeValue(Value(source));
    std::vector<uint8_t> block(16);
    const int64_t recursive_offset = 8;
    const uint64_t nested_raw = nested.raw();
    std::memcpy(block.data(), &recursive_offset, 8);
    std::memcpy(block.data() + 8, &nested_raw, 8);
    const uint64_t index = InternBlock({TypeId::Invalid, std::move(block)});
    return ValueRep::Make(CrateTypeId::UnregisteredValue, index, false, false);
  }

  ValueRep EncodeExtensionField(const TypedExtensionField& field) {
    if (!field.unregistered) return EncodeValue(field.value);
    return EncodeUnregisteredValue(
        field.unregistered_source.empty()
            ? PrintValue(field.value, PrintOptions{})
            : field.unregistered_source);
  }

  // Encode a sparse array edit (VtArrayEdit, crate >= 0.14) as a ValueRep
  // with the array-edit bit, the ELEMENT crate type in the type byte, and a
  // payload block holding (valuesRep, indexesRep, isDense). Literal element
  // texts are parsed back into one typed literals array; the op stream uses
  // OpenUSD's Vt_ArrayEditOps encoding.
  ValueRep EncodeArrayEdit(const ArrayEditData& edit, TypeId elem_type) {
    const CrateTypeId elem_crate = ToCrateTypeId(elem_type);
    if (elem_crate == CrateTypeId::Invalid) {
      return ValueRep::Make(CrateTypeId::Invalid, 0, false, false);
    }
    if (required_version_minor_ < 14) required_version_minor_ = 14;
    if (edit.ops.empty()) {
      // Identity edit: no out-of-line data.
      return ValueRep::Make(elem_crate, 0, false, false, false,
                            /*is_array_edit=*/true);
    }

    // Collect literal texts (deduplicated, first-appearance order) and assign
    // indices. lit_texts keeps first-appearance order (the literals array
    // below is built from it in that order); the map makes the per-op lookup
    // O(1) instead of a linear scan over the growing vector.
    std::vector<std::string> lit_texts;
    std::unordered_map<std::string, int64_t> lit_index_by_text;
    auto lit_index = [&](const std::string& t) -> int64_t {
      auto it = lit_index_by_text.find(t);
      if (it != lit_index_by_text.end()) return it->second;
      const int64_t idx = static_cast<int64_t>(lit_texts.size());
      lit_index_by_text.emplace(t, idx);
      lit_texts.push_back(t);
      return idx;
    };

    std::vector<int64_t> ins;
    auto word = [](uint8_t op) {
      return static_cast<int64_t>((static_cast<uint64_t>(op) << 56) | 1u);
    };
    for (const ArrayEditOpRec& op : edit.ops) {
      switch (op.kind) {
        case ArrayEditOpRec::WriteLiteral:
          ins.push_back(word(0));
          ins.push_back(lit_index(op.literal));
          ins.push_back(op.a2);
          break;
        case ArrayEditOpRec::WriteRef:
          ins.push_back(word(1));
          ins.push_back(op.a1);
          ins.push_back(op.a2);
          break;
        case ArrayEditOpRec::InsertLiteral:
          ins.push_back(word(2));
          ins.push_back(lit_index(op.literal));
          ins.push_back(op.a2);
          break;
        case ArrayEditOpRec::InsertRef:
          ins.push_back(word(3));
          ins.push_back(op.a1);
          ins.push_back(op.a2);
          break;
        case ArrayEditOpRec::Erase:
          ins.push_back(word(4));
          ins.push_back(op.a1);
          break;
        case ArrayEditOpRec::MinSize:
          if (op.has_fill) {
            ins.push_back(word(6));
            ins.push_back(op.a1);
            ins.push_back(lit_index(op.literal));
          } else {
            ins.push_back(word(5));
            ins.push_back(op.a1);
          }
          break;
        case ArrayEditOpRec::SetSize:
          if (op.has_fill) {
            ins.push_back(word(8));
            ins.push_back(op.a1);
            ins.push_back(lit_index(op.literal));
          } else {
            ins.push_back(word(7));
            ins.push_back(op.a1);
          }
          break;
        case ArrayEditOpRec::MaxSize:
          ins.push_back(word(9));
          ins.push_back(op.a1);
          break;
      }
    }

    // Literal texts -> one typed array Value via the usda codec.
    Value literals;
    {
      std::string text = "[";
      for (size_t i = 0; i < lit_texts.size(); ++i) {
        if (i) text += ", ";
        text += lit_texts[i];
      }
      text += "]";
      Lexer lexer(text.data(), text.size());
      ParseArrayContext ctx;
      ParseResult r = ParseArrayValue(lexer, elem_type, ctx);
      if (!r.success) {
        return ValueRep::Make(CrateTypeId::Invalid, 0, false, false);
      }
      literals = std::move(r.value);
    }
    const ValueRep values_rep = EncodeValue(literals);
    const ValueRep indexes_rep = EncodeValue(Value::MakeInt64Array(ins));
    if (values_rep.type_id() == CrateTypeId::Invalid ||
        indexes_rep.type_id() == CrateTypeId::Invalid) {
      return ValueRep::Make(CrateTypeId::Invalid, 0, false, false);
    }

    // (valuesRep, indexesRep, isDense) tuple. The embedded reps carry
    // value_data_ indices until ComputeValueLayoutAndPatch relocates them
    // (embedded_value_rep_offsets); push directly like dictionaries so the
    // in-place patch never applies to a shared block twice.
    DataBlock block;
    block.type = TypeId::Invalid;
    block.data.resize(17, 0);
    const uint64_t vraw = values_rep.raw();
    const uint64_t iraw = indexes_rep.raw();
    std::memcpy(block.data.data(), &vraw, 8);
    std::memcpy(block.data.data() + 8, &iraw, 8);
    block.data[16] = 0;  // isDense: legacy, always false
    block.embedded_value_rep_offsets.push_back(0);
    block.embedded_value_rep_offsets.push_back(8);
    const uint64_t idx = value_data_.size();
    value_data_.push_back(std::move(block));
    return ValueRep::Make(elem_crate, idx, false, false, false,
                          /*is_array_edit=*/true);
  }

  ValueRep EncodeValue(const Value& val) {
    if (val.is_dictionary()) {
      const Dict* d = val.as_dictionary();
      return d ? EncodeDictionary(*d)
               : ValueRep::Make(CrateTypeId::Dictionary, 0, false, false);
    }

    // Authored value block (`= None`): a real opinion with no data. Must map
    // to the crate's ValueBlock type (an Invalid rep would make pxr reject the
    // whole layer with "unsupported type enum value 0").
    if (val.is_block()) {
      return ValueRep::Make(CrateTypeId::ValueBlock, 0, false, true);
    }

    TypeId type_id = val.type_id();
    CrateTypeId crate_type = ToCrateTypeId(type_id);
    bool is_array = val.is_array();

    // This writer emits the uint64 count-header array layout introduced in
    // Crate 0.7. Upgrade an older requested version rather than stamping a
    // pre-0.7 bootstrap onto incompatible array bytes.
    if (is_array && required_version_minor_ < 7) {
      required_version_minor_ = 7;
    }

    // pxr-style write-version upgrade: timecode values require crate 0.9.
    if (type_id == TypeId::TimeCode && required_version_minor_ < 9) {
      required_version_minor_ = 9;
    }

    if (crate_type == CrateTypeId::Invalid) {
      return ValueRep::Make(CrateTypeId::Invalid, 0, is_array, false);
    }

    // Fast path: copy an unmodified, crate-backed array straight through from
    // its source buffer, skipping decode -> re-encode of large payloads. This
    // must run before any accessor below (which would materialize the array).
    if (is_array && val.is_lazy() && !val.is_dirty()) {
      ValueRep pt;
      if (TryPassThrough(val, &pt)) {
        ++arrays_passed_through_;
        return pt;
      }
      ++arrays_reencoded_;  // fell back to full decode + re-encode below
    }

    // Handle inline scalars
    if (!is_array && options_.inline_small_values) {
      uint64_t payload = 0;
      bool can_inline = false;

      switch (type_id) {
        case TypeId::Bool: {
          const bool* v = val.as_bool();
          if (v) { payload = *v ? 1 : 0; can_inline = true; }
          break;
        }
        case TypeId::Int: {
          const int32_t* v = val.as_int();
          if (v) { payload = static_cast<uint64_t>(*v); can_inline = true; }
          break;
        }
        case TypeId::UInt: {
          const uint32_t* v = val.as_uint();
          if (v) { payload = *v; can_inline = true; }
          break;
        }
        case TypeId::UChar: {
          const uint8_t* v = val.as_uchar();
          if (v) { payload = *v; can_inline = true; }
          break;
        }
        case TypeId::Float: {
          const float* v = val.as_float();
          if (v) { uint32_t bits; std::memcpy(&bits, v, 4); payload = bits; can_inline = true; }
          break;
        }
        case TypeId::Token: {
          const std::string* s = val.as_token();
          if (s) { payload = InternToken(*s); can_inline = true; }
          break;
        }
        default:
          break;
      }

      if (can_inline) {
        return ValueRep::Make(crate_type, payload, is_array, true);
      }
    }

    // Half-family scalars that fit 4 bytes (half, half2/texcoord2h). pxr
    // treats these as ALWAYS-inlined (sizeof <= 4): its reader unpacks the
    // payload directly and never consults the offset path, so they must be
    // inlined regardless of options_.inline_small_values. Payload = raw
    // little-endian half bit lanes (matches UnpackHalf / UnpackVec2h).
    if (!is_array &&
        (type_id == TypeId::Half || type_id == TypeId::Half2 ||
         type_id == TypeId::Texcoord2h)) {
      size_t nbytes = 0;
      const uint8_t* bytes = val.raw_bytes(&nbytes);
      if (bytes && nbytes <= 4) {
        uint32_t payload32 = 0;
        std::memcpy(&payload32, bytes, nbytes);
        return ValueRep::Make(crate_type, payload32, false, true);
      }
      return ValueRep::Make(CrateTypeId::Invalid, 0, false, false);
    }

    // String / AssetPath via string index (not inline; scalars only --
    // string-family ARRAYS are handled in the array section below).
    if (!is_array && type_id == TypeId::String) {
      const std::string* str = val.as_string();
      if (str) {
        uint32_t str_idx = InternString(*str);
        return ValueRep::Make(crate_type, str_idx, false, true);
      }
    }
    if (!is_array && type_id == TypeId::AssetPath) {
      const std::string* str = val.as_asset_path();
      if (str) {
        // AssetPath: type stays AssetPath; payload is a token index (the path
        // string interned as a token), matching the reader's UnpackAssetPath.
        uint32_t tok_idx = InternToken(*str);
        return ValueRep::Make(CrateTypeId::AssetPath, tok_idx, false, true);
      }
    }
    if (!is_array && type_id == TypeId::Token) {
      const std::string* str = val.as_token();
      if (str) {
        uint32_t tok_idx = InternToken(*str);
        return ValueRep::Make(CrateTypeId::Token, tok_idx, false, true);
      }
    }

    // Non-inline scalars: store in the VALUE data section.
    if (!is_array) {
      // pathExpression: pxr writes the expression TEXT as a u32 string
      // index (crate >= 0.10).
      if (type_id == TypeId::PathExpression) {
        const std::string* str = val.as_string();
        if (str) {
          uint32_t sidx = InternString(*str);
          uint64_t data_idx = StoreValueData(&sidx, 4, type_id);
          if (required_version_minor_ < 10) required_version_minor_ = 10;
          return ValueRep::Make(CrateTypeId::PathExpression, data_idx, false,
                                false);
        }
        return ValueRep::Make(CrateTypeId::Invalid, 0, false, false);
      }
      // Quaternions: crate / GfQuat memory layout is imaginary-first
      // (x, y, z, w); the internal layout (usda text order) is real-first
      // (w, x, y, z). Swizzle on write; the reader swizzles back.
      if (type_id == TypeId::Quatf) {
        const float* v = val.as_float4();
        if (v) {
          const float d[4] = {v[1], v[2], v[3], v[0]};
          uint64_t data_idx = StoreValueData(d, 16, type_id);
          return ValueRep::Make(crate_type, data_idx, false, false);
        }
        return ValueRep::Make(CrateTypeId::Invalid, 0, false, false);
      }
      if (type_id == TypeId::Quatd) {
        const double* v = val.as_double4();
        if (v) {
          const double d[4] = {v[1], v[2], v[3], v[0]};
          uint64_t data_idx = StoreValueData(d, 32, type_id);
          return ValueRep::Make(crate_type, data_idx, false, false);
        }
        return ValueRep::Make(CrateTypeId::Invalid, 0, false, false);
      }
      if (type_id == TypeId::Quath) {
        // SBO holds 4 half-bit lanes, real-first; swizzle in the half domain.
        size_t nbytes = 0;
        const uint8_t* b = val.raw_bytes(&nbytes);
        if (b && nbytes == 8) {
          uint16_t h[4];
          std::memcpy(h, b, 8);
          const uint16_t d[4] = {h[1], h[2], h[3], h[0]};
          uint64_t data_idx = StoreValueData(d, 8, type_id);
          return ValueRep::Make(crate_type, data_idx, false, false);
        }
        return ValueRep::Make(CrateTypeId::Invalid, 0, false, false);
      }
      // Float matrices widen to the crate's double matrix types.
      if (type_id == TypeId::Matrix2f || type_id == TypeId::Matrix3f ||
          type_id == TypeId::Matrix4f) {
        const float* v = (type_id == TypeId::Matrix2f)   ? val.as_matrix2f()
                         : (type_id == TypeId::Matrix3f) ? val.as_matrix3f()
                                                         : val.as_matrix4f();
        const size_t comps = (type_id == TypeId::Matrix2f)   ? 4
                             : (type_id == TypeId::Matrix3f) ? 9
                                                             : 16;
        if (v) {
          double d[16];
          for (size_t k = 0; k < comps; ++k) d[k] = static_cast<double>(v[k]);
          uint64_t data_idx = StoreValueData(d, comps * sizeof(double), type_id);
          return ValueRep::Make(crate_type, data_idx, false, false);
        }
        return ValueRep::Make(CrateTypeId::Invalid, 0, false, false);
      }
      // Generic POD scalar: for every remaining type (bool/ints/uints,
      // float/double/half and their vector, matrix and role forms, timecode)
      // the SBO layout matches the crate layout byte-for-byte.
      {
        const size_t elem_size = CrateValueSize(crate_type, false);
        size_t nbytes = 0;
        const uint8_t* bytes = val.raw_bytes(&nbytes);
        if (bytes && elem_size > 0 && nbytes == elem_size) {
          uint64_t data_idx = StoreValueData(bytes, elem_size, type_id);
          return ValueRep::Make(crate_type, data_idx, false, false);
        }
      }
      // Unencodable scalar: return Invalid so callers skip the field instead
      // of emitting a rep whose payload points at unrelated bytes.
      return ValueRep::Make(CrateTypeId::Invalid, 0, false, false);
    }

    // Array types: store count + elements in data section
    if (is_array) {
      uint64_t count = 0;
      bool data_compressed = false; // whether the array data was compressed
      size_t elem_size = CrateValueSize(crate_type, false);
      (void)elem_size;

      // Collect array data
      std::vector<uint8_t> arr_data;
      auto resize_counted_payload = [&](uint64_t item_count,
                                        size_t item_size) -> bool {
        size_t bytes = 0;
        size_t total = 0;
        if (!safe::mul(item_count, item_size, &bytes) ||
            !safe::add(size_t(8), bytes, &total)) {
          result_.error = "Array payload size overflow";
          return false;
        }
        arr_data.resize(total);
        std::memcpy(arr_data.data(), &item_count, 8);
        return true;
      };
      auto copy_counted_payload = [&](uint64_t item_count, const void* data,
                                      size_t item_size) -> bool {
        if (!resize_counted_payload(item_count, item_size)) {
          return false;
        }
        size_t bytes = 0;
        if (!safe::mul(item_count, item_size, &bytes)) {
          result_.error = "Array payload size overflow";
          return false;
        }
        if (bytes > 0) {
          std::memcpy(arr_data.data() + 8, data, bytes);
        }
        return true;
      };

      switch (type_id) {
        case TypeId::Float: {
          const std::vector<float>* arr = val.as_float_array();
          if (arr) {
            count = arr->size();
            if (!copy_counted_payload(count, arr->data(), sizeof(float))) {
              return ValueRep::Make(CrateTypeId::Invalid, 0, false, false);
            }
          }
          break;
        }
        case TypeId::Int: {
          const std::vector<int32_t>* arr = val.as_int_array();
          if (arr) {
            count = arr->size();
            if (options_.compress_arrays && count >= 16) {
              // Compressed integer array: [u64 count][u64 comp_size][LZ4+delta data]
              std::vector<uint32_t> tmp(count);
              for (size_t k = 0; k < count; ++k) tmp[k] = static_cast<uint32_t>((*arr)[k]);
              std::vector<uint8_t> delta = EncodeDeltaU32(tmp.data(), count);
              CompressResult cr = CompressCrateBlob(delta.data(), delta.size());
              if (cr.success) {
                size_t payload_size = 0;
                size_t total_size = 0;
                if (!safe::add(size_t(8), cr.data.size(), &payload_size) ||
                    !safe::add(size_t(8), payload_size, &total_size)) {
                  result_.error = "Compressed integer array payload size overflow";
                  return ValueRep::Make(CrateTypeId::Invalid, 0, false, false);
                }
                arr_data.resize(total_size);
                std::memcpy(arr_data.data(), &count, 8);
                uint64_t csz = static_cast<uint64_t>(cr.data.size());
                std::memcpy(arr_data.data() + 8, &csz, 8);
                std::memcpy(arr_data.data() + 16, cr.data.data(), cr.data.size());
                data_compressed = true;
              } else {
                // Fall back to uncompressed
                if (!copy_counted_payload(count, arr->data(),
                                          sizeof(int32_t))) {
                  return ValueRep::Make(CrateTypeId::Invalid, 0, false, false);
                }
              }
            } else {
              if (!copy_counted_payload(count, arr->data(), sizeof(int32_t))) {
                return ValueRep::Make(CrateTypeId::Invalid, 0, false, false);
              }
            }
          }
          break;
        }
        case TypeId::Float3:
        case TypeId::Point3f:
        case TypeId::Vector3f:
        case TypeId::Normal3f:
        case TypeId::Color3f:
        case TypeId::Texcoord3f: {
          const std::vector<float>* arr = val.as_float_array();
          if (arr && arr->size() % 3 == 0) {
            count = arr->size() / 3;
            if (!copy_counted_payload(count * uint64_t(3), arr->data(),
                                      sizeof(float))) {
              return ValueRep::Make(CrateTypeId::Invalid, 0, false, false);
            }
            std::memcpy(arr_data.data(), &count, 8);
          }
          break;
        }
        case TypeId::Double: {
          const std::vector<double>* arr = val.as_double_array();
          if (arr) {
            count = arr->size();
            if (!copy_counted_payload(count, arr->data(), sizeof(double))) {
              return ValueRep::Make(CrateTypeId::Invalid, 0, false, false);
            }
          } else {
            // Fallback: try reading as float array and convert
            const std::vector<float>* farr = val.as_float_array();
            if (farr) {
              count = farr->size();
              if (!resize_counted_payload(count, sizeof(double))) {
                return ValueRep::Make(CrateTypeId::Invalid, 0, false, false);
              }
              double* dptr = reinterpret_cast<double*>(arr_data.data() + 8);
              for (size_t i = 0; i < count; ++i) dptr[i] = static_cast<double>((*farr)[i]);
            }
          }
          break;
        }
        case TypeId::Int64: {
          const std::vector<int64_t>* arr = val.as_int64_array();
          if (arr) {
            count = arr->size();
            if (!copy_counted_payload(count, arr->data(), sizeof(int64_t))) {
              return ValueRep::Make(CrateTypeId::Invalid, 0, false, false);
            }
          }
          break;
        }
        case TypeId::UInt: {
          const std::vector<uint32_t>* arr = val.as_uint_array();
          if (arr) {
            count = arr->size();
            if (!copy_counted_payload(count, arr->data(), sizeof(uint32_t))) {
              return ValueRep::Make(CrateTypeId::Invalid, 0, false, false);
            }
          }
          break;
        }
        case TypeId::UChar: {
          // uchar[]: narrow the widened uint32 storage back to the tightly
          // packed uint8 wire form pxr writes/reads.
          const std::vector<uint32_t>* arr = val.as_uint_array();
          if (arr) {
            std::vector<uint8_t> raw8(arr->size());
            for (size_t k = 0; k < arr->size(); ++k) {
              raw8[k] = static_cast<uint8_t>((*arr)[k] & 0xFFu);
            }
            count = raw8.size();
            if (!copy_counted_payload(count, raw8.data(), sizeof(uint8_t))) {
              return ValueRep::Make(CrateTypeId::Invalid, 0, false, false);
            }
          }
          break;
        }
        case TypeId::UInt64: {
          const std::vector<uint64_t>* arr = val.as_uint64_array();
          if (arr) {
            count = arr->size();
            if (!copy_counted_payload(count, arr->data(), sizeof(uint64_t))) {
              return ValueRep::Make(CrateTypeId::Invalid, 0, false, false);
            }
          }
          break;
        }
        case TypeId::Bool: {
          const std::vector<uint8_t>* arr = val.as_bool_array();
          if (arr) {
            count = arr->size();
            if (!copy_counted_payload(count, arr->data(), sizeof(uint8_t))) {
              return ValueRep::Make(CrateTypeId::Invalid, 0, false, false);
            }
          }
          break;
        }
        case TypeId::Token: {
          const std::vector<std::string>* arr = val.as_token_array();
          if (arr) {
            count = arr->size();
            // Store token indices
            if (!resize_counted_payload(count, sizeof(uint32_t))) {
              return ValueRep::Make(CrateTypeId::Invalid, 0, false, false);
            }
            for (size_t k = 0; k < count; ++k) {
              uint32_t idx = InternToken((*arr)[k]);
              std::memcpy(arr_data.data() + 8 + k * 4, &idx, 4);
            }
          }
          break;
        }
        case TypeId::String: {
          // String arrays: uint32 indices into the STRINGS section.
          const std::vector<std::string>* arr = val.as_token_array();
          if (arr) {
            count = arr->size();
            if (!resize_counted_payload(count, sizeof(uint32_t))) {
              return ValueRep::Make(CrateTypeId::Invalid, 0, false, false);
            }
            for (size_t k = 0; k < count; ++k) {
              uint32_t idx = InternString((*arr)[k]);
              std::memcpy(arr_data.data() + 8 + k * 4, &idx, 4);
            }
          }
          break;
        }
        case TypeId::AssetPath: {
          // Asset arrays: uint32 STRING indices (unlike the scalar form, which
          // uses a token index) — matches pxr's VtArray<SdfAssetPath> layout.
          const std::vector<std::string>* arr = val.as_token_array();
          if (arr) {
            count = arr->size();
            if (!resize_counted_payload(count, sizeof(uint32_t))) {
              return ValueRep::Make(CrateTypeId::Invalid, 0, false, false);
            }
            for (size_t k = 0; k < count; ++k) {
              uint32_t idx = InternString((*arr)[k]);
              std::memcpy(arr_data.data() + 8 + k * 4, &idx, 4);
            }
          }
          break;
        }
        case TypeId::Int2:
        case TypeId::Int3:
        case TypeId::Int4: {
          // Int-vector arrays: flat int32 buffer, raw (pxr stores Vec*i arrays
          // uncompressed).
          const std::vector<int32_t>* arr = val.as_int_array();
          const uint32_t comps = ArrayComps(type_id);
          if (arr && comps && arr->size() % comps == 0) {
            count = arr->size() / comps;
            if (!copy_counted_payload(arr->size(), arr->data(),
                                      sizeof(int32_t))) {
              return ValueRep::Make(CrateTypeId::Invalid, 0, false, false);
            }
            std::memcpy(arr_data.data(), &count, 8);
          }
          break;
        }
        case TypeId::UInt2:
        case TypeId::UInt3:
        case TypeId::UInt4: {
          // UInt-vector arrays share the Vec*i crate encoding (same 32-bit
          // lanes, bit-exact; declared type name restores uintN). Without
          // this case they fell into `default:` and were written as EMPTY.
          const std::vector<uint32_t>* arr = val.as_uint_array();
          const uint32_t comps = ArrayComps(type_id);
          if (arr && comps && arr->size() % comps == 0) {
            count = arr->size() / comps;
            if (!copy_counted_payload(arr->size(), arr->data(),
                                      sizeof(uint32_t))) {
              return ValueRep::Make(CrateTypeId::Invalid, 0, false, false);
            }
            std::memcpy(arr_data.data(), &count, 8);
          }
          break;
        }
        // Vector / quat / matrix arrays: flat float or double buffer. comps =
        // scalars per element (Vec2f=2, Vec4f/Quatf=4, Matrix4d=16, ...).
        case TypeId::Quatf: {
          // Internal flat buffer is real-first per element; crate stores
          // imaginary-first. Swizzle each element on write.
          const std::vector<float>* arr = val.as_float_array();
          if (arr && arr->size() % 4 == 0) {
            count = arr->size() / 4;
            if (!resize_counted_payload(arr->size(), sizeof(float))) {
              return ValueRep::Make(CrateTypeId::Invalid, 0, false, false);
            }
            std::memcpy(arr_data.data(), &count, 8);
            float* dst = reinterpret_cast<float*>(arr_data.data() + 8);
            for (size_t e = 0; e < count; ++e) {
              const float* q = arr->data() + e * 4;
              dst[e * 4 + 0] = q[1];
              dst[e * 4 + 1] = q[2];
              dst[e * 4 + 2] = q[3];
              dst[e * 4 + 3] = q[0];
            }
          }
          break;
        }
        case TypeId::Float2:
        case TypeId::Float4:
        case TypeId::Color4f:
        case TypeId::Texcoord2f: {
          const std::vector<float>* arr = val.as_float_array();
          const uint32_t comps = ArrayComps(type_id);
          if (arr && comps && arr->size() % comps == 0) {
            count = arr->size() / comps;
            if (!copy_counted_payload(arr->size(), arr->data(),
                                      sizeof(float))) {
              return ValueRep::Make(CrateTypeId::Invalid, 0, false, false);
            }
            std::memcpy(arr_data.data(), &count, 8);
          }
          break;
        }
        case TypeId::Matrix2f:
        case TypeId::Matrix3f:
        case TypeId::Matrix4f: {
          const std::vector<float>* arr = val.as_float_array();
          const uint32_t comps = ArrayComps(type_id);
          if (arr && comps && arr->size() % comps == 0) {
            count = arr->size() / comps;
            if (!resize_counted_payload(arr->size(), sizeof(double))) {
              return ValueRep::Make(CrateTypeId::Invalid, 0, false, false);
            }
            std::memcpy(arr_data.data(), &count, 8);
            for (size_t i = 0; i < arr->size(); ++i) {
              double d = static_cast<double>((*arr)[i]);
              std::memcpy(arr_data.data() + 8 + i * sizeof(double), &d,
                          sizeof(double));
            }
          }
          break;
        }
        case TypeId::Quatd: {
          const std::vector<double>* arr = val.as_double_array();
          if (arr && arr->size() % 4 == 0) {
            count = arr->size() / 4;
            if (!resize_counted_payload(arr->size(), sizeof(double))) {
              return ValueRep::Make(CrateTypeId::Invalid, 0, false, false);
            }
            std::memcpy(arr_data.data(), &count, 8);
            double* dst = reinterpret_cast<double*>(arr_data.data() + 8);
            for (size_t e = 0; e < count; ++e) {
              const double* q = arr->data() + e * 4;
              dst[e * 4 + 0] = q[1];
              dst[e * 4 + 1] = q[2];
              dst[e * 4 + 2] = q[3];
              dst[e * 4 + 3] = q[0];
            }
          }
          break;
        }
        case TypeId::TimeCode:
        case TypeId::Double2:
        case TypeId::Double3:
        case TypeId::Double4:
        case TypeId::Point3d:
        case TypeId::Vector3d:
        case TypeId::Normal3d:
        case TypeId::Color3d:
        case TypeId::Color4d:
        case TypeId::Texcoord2d:
        case TypeId::Texcoord3d:
        case TypeId::Matrix2d:
        case TypeId::Matrix3d:
        case TypeId::Matrix4d: {
          const std::vector<double>* arr = val.as_double_array();
          const uint32_t comps = ArrayComps(type_id);
          if (arr && comps && arr->size() % comps == 0) {
            count = arr->size() / comps;
            if (!copy_counted_payload(arr->size(), arr->data(),
                                      sizeof(double))) {
              return ValueRep::Make(CrateTypeId::Invalid, 0, false, false);
            }
            std::memcpy(arr_data.data(), &count, 8);
          }
          break;
        }
        // Half arrays materialize as a float buffer; narrow back to uint16 half
        // for the crate (only reached when the array is materialized, not on the
        // lazy byte-passthrough path).
        case TypeId::Quath: {
          const std::vector<float>* arr = val.as_float_array();
          if (arr && arr->size() % 4 == 0) {
            count = arr->size() / 4;
            if (!resize_counted_payload(arr->size(), sizeof(uint16_t))) {
              return ValueRep::Make(CrateTypeId::Invalid, 0, false, false);
            }
            std::memcpy(arr_data.data(), &count, 8);
            uint8_t* dst = arr_data.data() + 8;
            for (size_t e = 0; e < count; ++e) {
              const float* q = arr->data() + e * 4;
              const uint16_t h[4] = {FloatToHalf(q[1]), FloatToHalf(q[2]),
                                     FloatToHalf(q[3]), FloatToHalf(q[0])};
              std::memcpy(dst + e * 8, h, 8);
            }
          }
          break;
        }
        case TypeId::Half:
        case TypeId::Half2:
        case TypeId::Half3:
        case TypeId::Half4:
        case TypeId::Point3h:
        case TypeId::Vector3h:
        case TypeId::Normal3h:
        case TypeId::Color3h:
        case TypeId::Color4h:
        case TypeId::Texcoord2h:
        case TypeId::Texcoord3h: {
          const std::vector<float>* arr = val.as_float_array();
          const uint32_t comps = ArrayComps(type_id);
          if (arr && comps && arr->size() % comps == 0) {
            count = arr->size() / comps;
            if (!resize_counted_payload(arr->size(), sizeof(uint16_t))) {
              return ValueRep::Make(CrateTypeId::Invalid, 0, false, false);
            }
            std::memcpy(arr_data.data(), &count, 8);
            for (size_t i = 0; i < arr->size(); ++i) {
              uint16_t h = FloatToHalf((*arr)[i]);
              std::memcpy(arr_data.data() + 8 + i * sizeof(uint16_t), &h,
                          sizeof(uint16_t));
            }
          }
          break;
        }
        default:
          break;
      }

      // Empty array (count == 0): pxrUSD encodes this as payload 0 with NO data
      // block (a count=0 block makes pxr/legacy readers reject it). The next
      // reader also treats payload==0 as an empty array.
      if (count == 0) {
        return ValueRep::Make(crate_type, 0, true, false);
      }

      if (!arr_data.empty()) {
        DataBlock block;
        block.type = type_id;
        block.data = std::move(arr_data);
        uint64_t idx = InternBlock(std::move(block));
        // Return non-inline value rep (index will be resolved to offset later)
        return ValueRep::Make(crate_type, idx, true, false, data_compressed);
      }
    }

    return ValueRep::Make(crate_type, 0, is_array, false);
  }

  // Build fields and specs from layer
