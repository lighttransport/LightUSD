// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - CrateWriter::Impl property/spec emission helpers.
// Included inside CrateWriter::Impl private section.

  // Build an explicit PathListOp value_data_ block for relationship/connection
  // targets: [u8 header=IsExplicit|HasExplicitItems][u64 count][u32 path_idx...].
  // Each target path is interned into the path table. Returns the block index.
  uint64_t StorePathListOp(const std::vector<std::string>& targets) {
    std::vector<uint32_t> idxs;
    idxs.reserve(targets.size());
    for (const auto& t : targets) {
      if (t.empty()) continue;  // skip target-less marker
      idxs.push_back(InternPath(t));
    }
    const uint8_t header = 0x03;  // IsExplicit (0x01) | HasExplicitItems (0x02)
    uint64_t count = idxs.size();
    size_t items_bytes = 0;
    size_t blob_size = 0;
    if (!safe::mul(count, size_t(4), &items_bytes) ||
        !safe::add(size_t(9), items_bytes, &blob_size)) {
      result_.error = "PathListOp payload size overflow";
      return 0;
    }
    std::vector<uint8_t> blob(blob_size);
    blob[0] = header;
    std::memcpy(blob.data() + 1, &count, 8);
    for (size_t i = 0; i < idxs.size(); ++i) {
      std::memcpy(blob.data() + 9 + i * 4, &idxs[i], 4);
    }
    DataBlock block;
    block.type = TypeId::Invalid;
    block.data = std::move(blob);
    return InternBlock(std::move(block));
  }

  // Build a VariantSelectionMap value_data_ block (CrateTypeId 45):
  // [u64 count][(u32 key_string_idx, u32 val_string_idx) * count]. Strings are
  // interned into the string table. Returns the block index.
  uint64_t StoreVariantSelectionMap(
      const std::vector<std::pair<std::string, std::string>>& sels) {
    uint64_t count = sels.size();
    size_t items_bytes = 0;
    size_t blob_size = 0;
    if (!safe::mul(count, size_t(8), &items_bytes) ||
        !safe::add(size_t(8), items_bytes, &blob_size)) {
      result_.error = "VariantSelectionMap payload size overflow";
      return 0;
    }
    std::vector<uint8_t> blob(blob_size);
    std::memcpy(blob.data(), &count, 8);
    for (size_t i = 0; i < sels.size(); ++i) {
      uint32_t k = InternString(sels[i].first);
      uint32_t v = InternString(sels[i].second);
      std::memcpy(blob.data() + 8 + i * 8, &k, 4);
      std::memcpy(blob.data() + 8 + i * 8 + 4, &v, 4);
    }
    DataBlock block;
    block.type = TypeId::Invalid;
    block.data = std::move(blob);
    return InternBlock(std::move(block));
  }

  // Encode an SdfReferenceListOp / SdfPayloadListOp in pxr's on-disk layout:
  //   [u8 ListOpHeader=0x03 explicit][u64 n] then per item
  //   [u32 assetStrIdx][u32 pathIdx][f64 offset][f64 scale]
  //   (+ [u64 customDataCount=0] for references only).
  // Items come from next's canonical arc strings
  // ("@asset@<prim>?layerOffset=offset:scale"; empty asset = internal arc).
  uint64_t StoreReferenceListOp(const std::vector<std::string>& arcs,
                                bool is_payload, const ArcEdit* edits) {
    std::vector<uint8_t> blob;
    auto put_bytes = [&](const void* d, size_t n) {
      const uint8_t* b = static_cast<const uint8_t*>(d);
      blob.insert(blob.end(), b, b + n);
    };
    auto put_items = [&](const std::vector<std::string>& items) {
      uint64_t count = items.size();
      put_bytes(&count, 8);
      for (const std::string& arc : items) {
        EncodeArcItem(arc, is_payload, put_bytes);
      }
    };
    // Authored non-explicit list-op edits round-trip natively via pxr's
    // sublist header bits; a bare/explicit list is one explicit run.
    if (edits && edits->authored && !edits->is_explicit) {
      uint8_t bits = 0;
      if (!edits->added.empty()) bits |= 0x04;
      if (!edits->prepended.empty()) bits |= 0x20;
      if (!edits->appended.empty()) bits |= 0x40;
      if (!edits->deleted.empty()) bits |= 0x08;
      if (!edits->ordered.empty()) bits |= 0x10;
      blob.push_back(bits);
      // Run order matches the reader: added, prepended, appended, deleted,
      // ordered.
      if (!edits->added.empty()) put_items(edits->added);
      if (!edits->prepended.empty()) put_items(edits->prepended);
      if (!edits->appended.empty()) put_items(edits->appended);
      if (!edits->deleted.empty()) put_items(edits->deleted);
      if (!edits->ordered.empty()) put_items(edits->ordered);
      DataBlock block;
      block.type = TypeId::Invalid;
      block.data = std::move(blob);
      return InternBlock(std::move(block));
    }
    if (arcs.empty()) {
      // Explicit-clear (`references = None`): pxr's form is IsExplicit with
      // NO items sublist. Writing 0x03 + a zero-item run made the re-read
      // look like a no-opinion listop and dropped the clear.
      blob.push_back(0x01);  // IsExplicit only
    } else {
      blob.push_back(0x03);  // IsExplicit | HasExplicitItems
      put_items(arcs);
    }
    DataBlock block;
    block.type = TypeId::Invalid;
    block.data = std::move(blob);
    return InternBlock(std::move(block));
  }

  // Encode a single SdfReference/SdfPayload item from next's canonical arc
  // string ("@asset@<prim>?layerOffset=offset:scale").
  template <typename PutBytes>
  void EncodeArcItem(const std::string& arc, bool is_payload,
                     PutBytes& put_bytes) {
    {
      // Parse the canonical arc string (mirrors Compositor::ParseReference).
      std::string asset, prim_path;
      double offset = 0.0, scale = 1.0;
      std::string str = arc;
      if (!str.empty() && str[0] == '@') {
        size_t end = str.find('@', 1);
        if (end != std::string::npos) {
          asset = str.substr(1, end - 1);
          str = str.substr(end + 1);
        }
      }
      size_t ps = str.find('<');
      size_t pe = str.find('>');
      if (ps != std::string::npos && pe != std::string::npos && pe > ps) {
        prim_path = str.substr(ps + 1, pe - ps - 1);
      }
      size_t op = str.find("layerOffset=");
      if (op != std::string::npos) {
        const char* c = str.c_str() + op + 12;
        char* endp = nullptr;
        offset = std::strtod(c, &endp);
        if (endp && *endp == ':') scale = std::strtod(endp + 1, nullptr);
      }
      uint32_t asset_idx = InternString(asset);
      // An omitted prim path means "use the referenced layer's defaultPrim".
      // It is encoded as SdfPath::EmptyPath, not AbsoluteRootPath: writing "/"
      // makes OpenUSD re-emit the invalid/default-changing @asset@</> form.
      uint32_t path_idx = InternPath(prim_path);
      put_bytes(&asset_idx, 4);
      put_bytes(&path_idx, 4);
      put_bytes(&offset, 8);
      put_bytes(&scale, 8);
      if (!is_payload) {
        uint64_t custom_data_count = 0;  // reference customData: none
        put_bytes(&custom_data_count, 8);
      }
    }
  }

  // inheritPaths / specializes: PathListOp built from arc strings
  // ("</Base>"), optionally carrying non-explicit list-op sublists natively.
  uint64_t StorePathArcListOp(const std::vector<std::string>& arcs,
                              const ArcEdit* edits) {
    auto strip = [](const std::vector<std::string>& in) {
      std::vector<std::string> out;
      out.reserve(in.size());
      for (const std::string& a : in) {
        size_t ps = a.find('<');
        size_t pe = a.rfind('>');
        if (ps != std::string::npos && pe != std::string::npos && pe > ps) {
          out.push_back(a.substr(ps + 1, pe - ps - 1));
        } else if (!a.empty()) {
          out.push_back(a);
        }
      }
      return out;
    };
    std::vector<uint8_t> blob;
    auto put_run = [&](const std::vector<std::string>& paths) {
      uint64_t n = paths.size();
      const uint8_t* b = reinterpret_cast<const uint8_t*>(&n);
      blob.insert(blob.end(), b, b + 8);
      for (const std::string& path : paths) {
        uint32_t idx = InternPath(path);
        const uint8_t* ib = reinterpret_cast<const uint8_t*>(&idx);
        blob.insert(blob.end(), ib, ib + 4);
      }
    };
    if (edits && edits->authored && !edits->is_explicit) {
      std::vector<std::string> pre = strip(edits->prepended);
      std::vector<std::string> add = strip(edits->added);
      std::vector<std::string> app = strip(edits->appended);
      std::vector<std::string> del = strip(edits->deleted);
      std::vector<std::string> ord = strip(edits->ordered);
      uint8_t bits = 0;
      if (!add.empty()) bits |= 0x04;
      if (!pre.empty()) bits |= 0x20;
      if (!app.empty()) bits |= 0x40;
      if (!del.empty()) bits |= 0x08;
      if (!ord.empty()) bits |= 0x10;
      blob.push_back(bits);
      if (!add.empty()) put_run(add);
      if (!pre.empty()) put_run(pre);
      if (!app.empty()) put_run(app);
      if (!del.empty()) put_run(del);
      if (!ord.empty()) put_run(ord);
    } else {
      std::vector<std::string> items = strip(arcs);
      if (items.empty()) {
        blob.push_back(0x01);  // explicit-clear: IsExplicit, no items (pxr form)
      } else {
        blob.push_back(0x03);  // IsExplicit | HasExplicitItems
        put_run(items);
      }
    }
    DataBlock block;
    block.type = TypeId::Invalid;
    block.data = std::move(blob);
    return InternBlock(std::move(block));
  }

  // Build a StringListOp field ([u8 header=0x03][u64 n][u32 string_idx]*n) --
  // pxr's encoding for variantSetNames.
  CrateField MakeStringListOpField(const std::string& field_name,
                                   const std::vector<std::string>& names) {
    std::vector<uint8_t> raw(9 + names.size() * 4);
    raw[0] = 0x03;  // IsExplicit | HasExplicitItems
    uint64_t cnt = names.size();
    std::memcpy(raw.data() + 1, &cnt, 8);
    for (size_t i = 0; i < names.size(); ++i) {
      uint32_t idx = InternString(names[i]);
      std::memcpy(raw.data() + 9 + i * 4, &idx, 4);
    }
    uint64_t data_idx = InternBlock({TypeId::Invalid, std::move(raw)});
    CrateField f;
    f.token_index.value = InternToken(field_name);
    f.value_rep =
        ValueRep::Make(CrateTypeId::StringListOp, data_idx, false, false);
    return f;
  }

  CrateField MakeStringListOpField(const std::string& field_name,
                                   const StringListOpEdits& edits) {
    std::vector<uint8_t> raw;
    raw.reserve(1 + 5 * 8 +
                4 * (edits.explicit_items.size() + edits.added.size() +
                     edits.prepended.size() + edits.appended.size() +
                     edits.deleted.size() + edits.ordered.size()));
    uint8_t bits = 0;
    if (edits.is_explicit) {
      bits = edits.explicit_items.empty() ? 0x01 : 0x03;
    } else {
      if (!edits.added.empty()) bits |= 0x04;
      if (!edits.deleted.empty()) bits |= 0x08;
      if (!edits.ordered.empty()) bits |= 0x10;
      if (!edits.prepended.empty()) bits |= 0x20;
      if (!edits.appended.empty()) bits |= 0x40;
    }
    raw.push_back(bits);
    auto put_run = [&](const std::vector<std::string>& items) {
      const size_t base = raw.size();
      raw.resize(base + 8 + items.size() * 4);
      const uint64_t count = items.size();
      std::memcpy(raw.data() + base, &count, 8);
      for (size_t i = 0; i < items.size(); ++i) {
        const uint32_t idx = InternString(items[i]);
        std::memcpy(raw.data() + base + 8 + i * 4, &idx, 4);
      }
    };
    if (edits.is_explicit) {
      if (!edits.explicit_items.empty()) put_run(edits.explicit_items);
    } else {
      if (!edits.added.empty()) put_run(edits.added);
      if (!edits.prepended.empty()) put_run(edits.prepended);
      if (!edits.appended.empty()) put_run(edits.appended);
      if (!edits.deleted.empty()) put_run(edits.deleted);
      if (!edits.ordered.empty()) put_run(edits.ordered);
    }
    const uint64_t data_idx =
        InternBlock({TypeId::Invalid, std::move(raw)});
    CrateField field;
    field.token_index.value = InternToken(field_name);
    field.value_rep =
        ValueRep::Make(CrateTypeId::StringListOp, data_idx, false, false);
    return field;
  }

  CrateField MakeTokenListOpField(const std::string& field_name,
                                  const StringListOpEdits& edits) {
    std::vector<uint8_t> raw;
    raw.reserve(1 + 5 * 8 +
                4 * (edits.explicit_items.size() + edits.added.size() +
                     edits.prepended.size() + edits.appended.size() +
                     edits.deleted.size() + edits.ordered.size()));
    uint8_t bits = 0;
    if (edits.is_explicit) {
      bits = edits.explicit_items.empty() ? 0x01 : 0x03;
    } else {
      if (!edits.added.empty()) bits |= 0x04;
      if (!edits.deleted.empty()) bits |= 0x08;
      if (!edits.ordered.empty()) bits |= 0x10;
      if (!edits.prepended.empty()) bits |= 0x20;
      if (!edits.appended.empty()) bits |= 0x40;
    }
    raw.push_back(bits);
    auto put_run = [&](const std::vector<std::string>& items) {
      const size_t base = raw.size();
      raw.resize(base + 8 + items.size() * 4);
      const uint64_t count = items.size();
      std::memcpy(raw.data() + base, &count, 8);
      for (size_t i = 0; i < items.size(); ++i) {
        const uint32_t idx = InternToken(items[i]);
        std::memcpy(raw.data() + base + 8 + i * 4, &idx, 4);
      }
    };
    if (edits.is_explicit) {
      if (!edits.explicit_items.empty()) put_run(edits.explicit_items);
    } else {
      if (!edits.added.empty()) put_run(edits.added);
      if (!edits.prepended.empty()) put_run(edits.prepended);
      if (!edits.appended.empty()) put_run(edits.appended);
      if (!edits.deleted.empty()) put_run(edits.deleted);
      if (!edits.ordered.empty()) put_run(edits.ordered);
    }
    const uint64_t data_idx =
        InternBlock({TypeId::Invalid, std::move(raw)});
    CrateField field;
    field.token_index.value = InternToken(field_name);
    field.value_rep =
        ValueRep::Make(CrateTypeId::TokenListOp, data_idx, false, false);
    return field;
  }

  // Build a StringVector field ([u64 count][u32 string_idx]*count) -- pxr's
  // encoding for subLayers.
  CrateField MakeStringVectorField(const std::string& field_name,
                                   const std::vector<std::string>& items) {
    std::vector<uint8_t> raw(8 + items.size() * 4);
    uint64_t cnt = items.size();
    std::memcpy(raw.data(), &cnt, 8);
    for (size_t i = 0; i < items.size(); ++i) {
      uint32_t idx = InternString(items[i]);
      std::memcpy(raw.data() + 8 + i * 4, &idx, 4);
    }
    uint64_t data_idx = InternBlock({TypeId::Invalid, std::move(raw)});
    CrateField f;
    f.token_index.value = InternToken(field_name);
    f.value_rep =
        ValueRep::Make(CrateTypeId::StringVector, data_idx, false, false);
    return f;
  }

  // Build a TokenVector field ([u64 count][u32 token_idx]*count) -- used for
  // primChildren / variantSetChildren / variantChildren.
  CrateField MakeTokenVectorField(const std::string& field_name,
                                  const std::vector<std::string>& names) {
    size_t n = names.size();
    std::vector<uint8_t> raw(8 + n * 4);
    uint64_t cnt = n;
    std::memcpy(raw.data(), &cnt, 8);
    for (size_t i = 0; i < n; ++i) {
      uint32_t tok = InternToken(names[i]);
      std::memcpy(raw.data() + 8 + i * 4, &tok, 4);
    }
    uint64_t data_idx = InternBlock({TypeId::Invalid, std::move(raw)});
    CrateField f;
    f.token_index.value = InternToken(field_name);
    f.value_rep = ValueRep::Make(CrateTypeId::TokenVector, data_idx, false, false);
    return f;
  }

  std::string AttrTypeName(const PrimSpec& prim, const std::string& prop_name,
                           const Value* val, uint16_t slot_flags) {
    if (const std::string* tn = prim.property_type_name(prop_name)) {
      if (!tn->empty()) return *tn;
    }
    TypeId tid = val ? val->type_id() : static_cast<TypeId>(TypeId::Invalid);
    const char* base = GetTypeName(tid);
    std::string name = base ? base : "token";
    const bool is_array = (slot_flags & PropSlot::kFlagArray) ||
                          (val && val->is_array());
    if (is_array) name += "[]";
    return name;
  }

  // Typed spline (Crate type 59, >= 0.12.0): the raw USDA text parses to the
  // ts-binary blob. Payload framing: uint64 blobSize + blob + uint64
  // followed by an unordered_map<double, VtDictionary> for per-knot
  // customData.
  // Returns false (and sets result_.error in strict mode) when the text does
  // not parse; the caller then simply omits the field.
  bool BuildSplineField(const std::string& prop_path,
                        const std::string& type_name,
                        const std::string& spline_text, CrateField* out) {
    SplineData sd;
    std::string serr;
    if (!ParseSplineText(spline_text, &sd, &serr)) {
      if (options_.strict_aousd_conformance) {
        result_.error = "Cannot encode malformed spline " + prop_path + ": " + serr;
      }
      return false;
    }
    std::string base = type_name;
    if (base.size() >= 2 && base.compare(base.size() - 2, 2, "[]") == 0) {
      base.resize(base.size() - 2);
    }
    sd.value_desc = (base == "float") ? 2 : (base == "half") ? 3 : 1;

    std::vector<uint8_t> blob;
    if (!EncodeSplineBinary(sd, &blob, &serr)) {
      if (options_.strict_aousd_conformance) {
        result_.error = "Cannot encode spline " + prop_path + ": " + serr;
      }
      return false;
    }
    std::vector<uint8_t> raw(8 + blob.size(), 0);
    uint64_t blob_size = blob.size();
    std::memcpy(raw.data(), &blob_size, 8);
    if (!blob.empty()) std::memcpy(raw.data() + 8, blob.data(), blob.size());

    auto append = [&](const void* bytes, size_t size) {
      const uint8_t* p = static_cast<const uint8_t*>(bytes);
      raw.insert(raw.end(), p, p + size);
    };
    struct KnotDict {
      double time;
      Value value;
    };
    std::vector<KnotDict> custom;
    for (const SplineKnot& knot : sd.knots) {
      if (knot.custom_data_source.empty()) continue;
      const std::string wrapper =
          "#usda 1.0\ndef Scope \"P\" { int x (customData = " +
          knot.custom_data_source + ") = 0 }\n";
      AsciiParser parser;
      if (!parser.Parse(wrapper.data(), wrapper.size())) {
        if (options_.strict_aousd_conformance) {
          result_.error = "Cannot encode spline knot customData for " +
                          prop_path;
        }
        return false;
      }
      Stage parsed = parser.TakeStage();
      const Layer* layer = parsed.GetRootLayer();
      const PrimSpec* prim = layer ? layer->prim_at_path("/P") : nullptr;
      const PropMeta* meta = prim ? prim->property_meta("x") : nullptr;
      if (!meta || !meta->customData.is_dictionary()) {
        if (options_.strict_aousd_conformance) {
          result_.error = "Spline knot customData is not a dictionary for " +
                          prop_path;
        }
        return false;
      }
      custom.push_back({knot.time, meta->customData});
    }

    const uint64_t custom_count = custom.size();
    append(&custom_count, 8);
    DataBlock spline_block;
    spline_block.type = TypeId::Invalid;
    for (const KnotDict& item : custom) {
      append(&item.time, 8);
      const Dict* dict = item.value.as_dictionary();
      const uint64_t entry_count = dict ? dict->entries().size() : 0;
      append(&entry_count, 8);
      if (!dict) continue;
      for (const auto& entry : dict->entries()) {
        const uint32_t key = InternString(entry.first);
        const int64_t recursive_offset = 8;
        const ValueRep rep = EncodeValue(entry.second);
        append(&key, 4);
        append(&recursive_offset, 8);
        spline_block.embedded_value_rep_offsets.push_back(raw.size());
        const uint64_t rep_raw = rep.raw();
        append(&rep_raw, 8);
      }
    }
    spline_block.data = std::move(raw);
    const uint64_t data_idx = value_data_.size();
    value_data_.push_back(std::move(spline_block));

    // A version-2 blob (tangent algorithms) needs crate 0.13.0; version 1
    // needs 0.12.0.
    const int need = (SplineBinaryVersion(sd) >= 2) ? 13 : 12;
    if (required_version_minor_ < need) required_version_minor_ = need;

    out->token_index.value = InternToken("spline");
    out->value_rep = ValueRep::Make(CrateTypeId::Spline, data_idx, false, false);
    return true;
  }

  CrateField BuildTimeSamplesField(const PrimSpec& prim, PropNameId prop_id) {
    auto* samples = prim.time_samples(prop_id);
    uint64_t num_samples = samples ? samples->size() : 0;

    size_t times_bytes = 0;
    size_t times_total = 0;
    if (!safe::mul(num_samples, size_t(8), &times_bytes) ||
        !safe::add(size_t(8), times_bytes, &times_total)) {
      result_.error = "timeSamples times payload size overflow";
      return CrateField{};
    }
    std::vector<uint8_t> times_data(times_total);
    std::memcpy(times_data.data(), &num_samples, 8);
    size_t ti = 0;
    if (samples) {
      for (const auto& kv : *samples) {
        double t = kv.first;
        std::memcpy(times_data.data() + 8 + ti * 8, &t, 8);
        ti++;
      }
    }
    uint64_t times_block_idx = InternBlock({TypeId::Double, std::move(times_data)});

    std::vector<ValueRep> sample_reps(num_samples);
    ti = 0;
    if (samples) {
      for (const auto& kv : *samples) {
        const Value* ts_val = prim.time_sample_value(kv.second);
        sample_reps[ti] = ts_val
            ? EncodeValue(*ts_val)
            : ValueRep::Make(CrateTypeId::ValueBlock, 0, false, true);
        if (sample_reps[ti].type_id() == CrateTypeId::Invalid) {
          // Unencodable sample: degrade to a block rather than a type-enum-0
          // rep that makes pxr reject the layer.
          sample_reps[ti] = ValueRep::Make(CrateTypeId::ValueBlock, 0, false, true);
        }
        ti++;
      }
    }

    size_t reps_bytes = 0;
    size_t header_size = 0;
    if (!safe::mul(num_samples, size_t(8), &reps_bytes) ||
        !safe::add(size_t(32), reps_bytes, &header_size)) {
      result_.error = "timeSamples header payload size overflow";
      return CrateField{};
    }
    std::vector<uint8_t> header_data(header_size, 0);
    size_t hp = 0;
    int64_t fwd1 = 8;
    std::memcpy(header_data.data() + hp, &fwd1, 8); hp += 8;
    uint64_t times_rep_raw =
        ValueRep::Make(CrateTypeId::Double, times_block_idx, true, false).raw();
    std::memcpy(header_data.data() + hp, &times_rep_raw, 8); hp += 8;
    int64_t fwd2 = 8;
    std::memcpy(header_data.data() + hp, &fwd2, 8); hp += 8;
    std::memcpy(header_data.data() + hp, &num_samples, 8); hp += 8;
    for (size_t si = 0; si < num_samples; si++) {
      uint64_t vr = sample_reps[si].raw();
      std::memcpy(header_data.data() + hp, &vr, 8); hp += 8;
    }
    uint64_t header_idx = value_data_.size();
    value_data_.push_back({TypeId::Invalid, std::move(header_data)});

    CrateField ts_field;
    ts_field.token_index.value = InternToken("timeSamples");
    ts_field.value_rep =
        ValueRep::Make(CrateTypeId::TimeSamples, header_idx, false, false);
    return ts_field;
  }

  void EmitPropertySpec(const std::string& prop_path, SpecType st,
                        const std::vector<CrateField>& field_list) {
    std::vector<uint32_t> fs;
    fs.reserve(field_list.size());
    for (const auto& f : field_list) {
      fs.push_back(static_cast<uint32_t>(fields_.size()));
      fields_.push_back(f);
    }
    uint32_t fs_idx = static_cast<uint32_t>(fieldsets_.size());
    fieldsets_.push_back(std::move(fs));
    CrateSpec spec;
    spec.path_index.value = InternPath(prop_path);
    spec.fieldset_index.value = fs_idx;
    spec.spec_type = st;
    specs_.push_back(spec);
  }

  // Emit every authored PropMeta field as a property-spec crate field, with
  // pxr's schema value types (String for displayName/displayGroup/doc, Bool
  // for hidden, Token for the token-ish strings, Double weight, Int indices,
  // token[] allowedTokens, dictionaries for customData/assetInfo/sdrMetadata).
  void EmitPropMetaFields(const PropMeta& pm, std::vector<CrateField>& flds) {
    auto tok_field = [&](const char* name, const std::string& v) {
      CrateField f;
      f.token_index.value = InternToken(name);
      f.value_rep =
          ValueRep::Make(CrateTypeId::Token, InternToken(v), false, true);
      flds.push_back(f);
    };
    auto str_field = [&](const char* name, const std::string& v) {
      CrateField f;
      f.token_index.value = InternToken(name);
      f.value_rep =
          ValueRep::Make(CrateTypeId::String, InternString(v), false, true);
      flds.push_back(f);
    };
    auto int_field = [&](const char* name, int32_t v) {
      CrateField f;
      f.token_index.value = InternToken(name);
      f.value_rep = ValueRep::Make(
          CrateTypeId::Int, static_cast<uint64_t>(static_cast<uint32_t>(v)),
          false, true);
      flds.push_back(f);
    };
    auto dict_field = [&](const char* name, const Value& v) {
      if (!v.is_dictionary()) return;
      CrateField f;
      f.token_index.value = InternToken(name);
      f.value_rep = EncodeValue(v);
      flds.push_back(f);
    };

    if (pm.authored & PropMeta::kInterpolation)
      tok_field("interpolation", pm.interpolation);
    if (pm.authored & PropMeta::kColorSpace)
      tok_field("colorSpace", pm.colorSpace);
    if (pm.authored & PropMeta::kRenderType)
      tok_field("renderType", pm.renderType);
    if (pm.authored & PropMeta::kConnectability)
      tok_field("connectability", pm.connectability);
    if (pm.authored & PropMeta::kOutputName)
      tok_field("outputName", pm.outputName);
    if (pm.authored & PropMeta::kBindMaterialAs)
      tok_field("bindMaterialAs", pm.bindMaterialAs);
    if (pm.authored & PropMeta::kKind) tok_field("kind", pm.kind);
    if (pm.authored & PropMeta::kPermission)
      tok_field("permission", pm.permission);
    if (pm.authored & PropMeta::kDisplayName)
      str_field("displayName", pm.displayName);
    if (pm.authored & PropMeta::kDisplayGroup)
      str_field("displayGroup", pm.displayGroup);
    if (pm.authored & PropMeta::kDoc) str_field("documentation", pm.doc);
    if (pm.authored & PropMeta::kComment) str_field("comment", pm.comment);
    if (pm.authored & PropMeta::kElementSize)
      int_field("elementSize", pm.elementSize);
    if (pm.authored & PropMeta::kUnauthoredIdx)
      int_field("unauthoredValuesIndex", pm.unauthoredValuesIndex);
    if (pm.authored & PropMeta::kWeight) {
      CrateField f;
      f.token_index.value = InternToken("weight");
      f.value_rep = EncodeValue(Value(pm.weight));
      flds.push_back(f);
    }
    if (pm.authored & PropMeta::kHidden) {
      CrateField f;
      f.token_index.value = InternToken("hidden");
      f.value_rep = ValueRep::Make(CrateTypeId::Bool, pm.hidden ? 1 : 0,
                                   false, true);
      flds.push_back(f);
    }
    if ((pm.authored & PropMeta::kAllowedTokens) && !pm.allowedTokens.empty()) {
      // pxr's schema type is VtTokenArray (a Token rep with the array bit),
      // NOT TokenVector — pxr eagerly unpacks a TokenVector to a held type
      // its typed metadata APIs cannot extract, and even fails to re-read
      // its own usda print of it.
      CrateField f;
      f.token_index.value = InternToken("allowedTokens");
      f.value_rep = EncodeValue(Value::MakeTokenArray(pm.allowedTokens));
      flds.push_back(f);
    }
    if (pm.authored & PropMeta::kCustomData) dict_field("customData", pm.customData);
    if (pm.authored & PropMeta::kAssetInfo) dict_field("assetInfo", pm.assetInfo);
    if (pm.authored & PropMeta::kSdrMetadata) dict_field("sdrMetadata", pm.sdrMetadata);
    for (const auto& field : pm.unknownFields) {
      CrateField f;
      f.token_index.value = InternToken(field.name);
      f.value_rep = EncodeExtensionField(field);
      if (f.value_rep.type_id() == CrateTypeId::Invalid) {
        if (options_.strict_aousd_conformance && result_.error.empty()) {
          result_.error = "Cannot encode extension property field '" +
                          field.name + "'";
        }
        continue;
      }
      flds.push_back(f);
    }
  }

  std::vector<std::string> EmitPrimProperties(const PrimSpec& prim) {
    std::vector<std::string> prop_names;
    PropNameTable& name_table = GetPropNameTable();
    const std::string base = prim.path().str();
    std::unordered_set<std::string> emitted;

    for (const auto& slot : prim.properties().slots()) {
      const std::string prop_name(name_table.get(slot.name_id));
      // Defense against duplicate slots for one property (the parser now
      // upserts, but crate-read or API-built layers could still carry them):
      // two Attribute specs with the same path make pxr reject the layer.
      if (emitted.count(prop_name)) continue;
      const Value* val = prim.property_value(slot.name_id);
      const bool has_ts = prim.has_time_samples(slot.name_id);
      std::vector<CrateField> flds;

      CrateField tf;
      tf.token_index.value = InternToken("typeName");
      std::string tn = AttrTypeName(prim, prop_name, val, slot.flags);
      tf.value_rep =
          ValueRep::Make(CrateTypeId::Token, InternToken(tn), false, true);
      flds.push_back(tf);

      if (slot.flags & PropSlot::kFlagUniform) {
        CrateField vf;
        vf.token_index.value = InternToken("variability");
        vf.value_rep = ValueRep::Make(CrateTypeId::Variability, 1, false, true);
        flds.push_back(vf);
      }

      if (slot.flags & PropSlot::kFlagCustom) {
        CrateField cf;
        cf.token_index.value = InternToken("custom");
        cf.value_rep = ValueRep::Make(CrateTypeId::Bool, 1, false, true);
        flds.push_back(cf);
      }

      if (val) {
        CrateField df;
        df.token_index.value = InternToken("default");
        df.value_rep = EncodeValue(*val);
        // An unencodable value must not become a `default` field with type
        // enum 0 — pxr fails the whole layer on it. The typeName field above
        // still records the declared type.
        if (df.value_rep.type_id() != CrateTypeId::Invalid) {
          flds.push_back(df);
        }
      } else if (const ArrayEditData* aedit = prim.array_edit(slot.name_id)) {
        // Sparse array-edit default (VtArrayEdit, crate 0.14).
        CrateField df;
        df.token_index.value = InternToken("default");
        df.value_rep = EncodeArrayEdit(
            *aedit, static_cast<TypeId>(slot.value_type));
        if (df.value_rep.type_id() != CrateTypeId::Invalid) {
          flds.push_back(df);
        }
      }

      if (slot.flags & PropSlot::kFlagConnection) {
        if (const std::vector<Path>* conns = prim.connection(prop_name)) {
          std::vector<std::string> targets;
          targets.reserve(conns->size());
          for (const auto& p : *conns)
            if (!p.str().empty()) targets.push_back(p.str());
          // An authored-empty list is a connection BLOCK (`.connect = None`):
          // emit an explicit-empty PathListOp so it round-trips.
          const ArcEdit* edit = prim.connection_edit(prop_name);
          uint64_t blk = (edit && edit->authored)
                             ? StorePathArcListOp(targets, edit)
                             : StorePathListOp(targets);
          CrateField cf;
          cf.token_index.value = InternToken("connectionPaths");
          cf.value_rep =
              ValueRep::Make(CrateTypeId::PathListOp, blk, false, false);
          flds.push_back(cf);
        }
      }

      if (has_ts) flds.push_back(BuildTimeSamplesField(prim, slot.name_id));

      if (const std::string* spline_text = prim.spline_source(slot.name_id)) {
        CrateField sf;
        if (BuildSplineField(base + "." + prop_name, tn, *spline_text, &sf)) {
          flds.push_back(sf);
        } else if (!result_.error.empty()) {
          return prop_names;  // strict mode: abort on unencodable spline
        }
      }

      if (const PropMeta* pm = prim.property_meta(slot.name_id)) {
        EmitPropMetaFields(*pm, flds);
      }

      EmitPropertySpec(base + "." + prop_name, SpecType::Attribute, flds);
      prop_names.push_back(prop_name);
      emitted.insert(prop_name);
    }

    for (auto prop_id : prim.time_sampled_properties()) {
      const std::string prop_name(name_table.get(prop_id));
      if (emitted.count(prop_name)) continue;
      std::vector<CrateField> flds;
      const Value* sv = nullptr;
      if (auto* s = prim.time_samples(prop_id))
        if (!s->empty()) sv = prim.time_sample_value((*s)[0].second);
      CrateField tf;
      tf.token_index.value = InternToken("typeName");
      std::string tn = AttrTypeName(prim, prop_name, sv, 0);
      tf.value_rep =
          ValueRep::Make(CrateTypeId::Token, InternToken(tn), false, true);
      flds.push_back(tf);
      flds.push_back(BuildTimeSamplesField(prim, prop_id));
      if (const PropMeta* pm = prim.property_meta(prop_id)) {
        EmitPropMetaFields(*pm, flds);
      }
      EmitPropertySpec(base + "." + prop_name, SpecType::Attribute, flds);
      prop_names.push_back(prop_name);
      emitted.insert(prop_name);
    }

    for (const auto& rel_name : prim.relationship_names()) {
      std::vector<CrateField> flds;
      CrateField vf;
      vf.token_index.value = InternToken("variability");
      const bool varying =
          (prim.relationship_flags(rel_name) & PropSlot::kFlagVarying) != 0;
      vf.value_rep = ValueRep::Make(CrateTypeId::Variability,
                                    varying ? 0 : 1, false, true);
      flds.push_back(vf);
      if (prim.relationship_flags(rel_name) & PropSlot::kFlagCustom) {
        CrateField cf;
        cf.token_index.value = InternToken("custom");
        cf.value_rep = ValueRep::Make(CrateTypeId::Bool, 1, false, true);
        flds.push_back(cf);
      }
      // Authored list-op edits encode natively via sublist header bits
      // (StorePathArcListOp), like the composition-arc fields.
      const ArcEdit* rel_edit = nullptr;
      {
        const auto& edits = prim.relationship_edits();
        auto eit = edits.find(rel_name);
        if (eit != edits.end() && eit->second.authored &&
            (!eit->second.is_explicit ||
             (prim.relationship(rel_name) &&
              prim.relationship(rel_name)->empty()))) {
          rel_edit = &eit->second;
        }
      }
      if (rel_edit) {
        uint64_t blk = StorePathArcListOp({}, rel_edit);
        CrateField rf;
        rf.token_index.value = InternToken("targetPaths");
        rf.value_rep =
            ValueRep::Make(CrateTypeId::PathListOp, blk, false, false);
        flds.push_back(rf);
      } else if (const std::vector<Path>* tgts = prim.relationship(rel_name)) {
        std::vector<std::string> targets;
        for (const auto& p : *tgts)
          if (!p.str().empty()) targets.push_back(p.str());
        if (!targets.empty()) {
          uint64_t blk = StorePathListOp(targets);
          CrateField rf;
          rf.token_index.value = InternToken("targetPaths");
          rf.value_rep =
              ValueRep::Make(CrateTypeId::PathListOp, blk, false, false);
          flds.push_back(rf);
        }
      }
      // Relationships carry property metadata too (previously dropped).
      if (const PropMeta* pm =
              prim.property_meta(GetPropNameTable().intern(rel_name))) {
        EmitPropMetaFields(*pm, flds);
      }
      EmitPropertySpec(base + "." + rel_name, SpecType::Relationship, flds);
      prop_names.push_back(rel_name);
    }

    return prop_names;
  }
