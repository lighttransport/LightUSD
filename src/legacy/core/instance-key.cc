// SPDX-License-Identifier: Apache 2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// instance-key.cc - Instance key computation
//

#include "instance-key.hh"

#include "../hash-util.hh"

namespace lightusd {

bool ComputeInstanceKeyFromPrimSpec(const PrimSpec &ps, InstanceKey *key) {
  if (!key) return false;

  // Only compute keys for instanceable prims
  if (!ps.metas().has_instanceable() || !ps.metas().get_instanceable()) {
    return false;
  }

  SpookyHash hasher;
  hasher.Init(0xDEADBEEF, 0xCAFEBABE);

  // Hash type name
  const std::string &tn = ps.typeName();
  if (!tn.empty()) {
    hasher.Update(tn.data(), tn.size());
  }

  // Hash references
  if (ps.metas().references.has_value()) {
    for (const auto &listop : *ps.metas().references) {
      uint8_t qual = static_cast<uint8_t>(listop.first);
      hasher.Update(&qual, sizeof(qual));
      for (const auto &ref : listop.second) {
        std::string ap = ref.asset_path.GetAssetPath();
        hasher.Update(ap.data(), ap.size());
        std::string pp = ref.prim_path.prim_part();
        hasher.Update(pp.data(), pp.size());
        hasher.Update(&ref.layerOffset._offset, sizeof(ref.layerOffset._offset));
        hasher.Update(&ref.layerOffset._scale, sizeof(ref.layerOffset._scale));
      }
    }
  }

  // Hash payloads
  if (ps.metas().payload.has_value()) {
    for (const auto &listop : *ps.metas().payload) {
      uint8_t qual = static_cast<uint8_t>(listop.first);
      hasher.Update(&qual, sizeof(qual));
      for (const auto &pl : listop.second) {
        std::string ap = pl.asset_path.GetAssetPath();
        hasher.Update(ap.data(), ap.size());
        std::string pp = pl.prim_path.prim_part();
        hasher.Update(pp.data(), pp.size());
        hasher.Update(&pl.layerOffset._offset, sizeof(pl.layerOffset._offset));
        hasher.Update(&pl.layerOffset._scale, sizeof(pl.layerOffset._scale));
      }
    }
  }

  // Hash inherits
  if (ps.metas().inherits.has_value()) {
    for (const auto &listop : *ps.metas().inherits) {
      uint8_t qual = static_cast<uint8_t>(listop.first);
      hasher.Update(&qual, sizeof(qual));
      for (const auto &p : listop.second) {
        std::string pp = p.prim_part();
        hasher.Update(pp.data(), pp.size());
      }
    }
  }

  // Hash specializes
  if (ps.metas().specializes.has_value()) {
    for (const auto &listop : *ps.metas().specializes) {
      uint8_t qual = static_cast<uint8_t>(listop.first);
      hasher.Update(&qual, sizeof(qual));
      for (const auto &p : listop.second) {
        std::string pp = p.prim_part();
        hasher.Update(pp.data(), pp.size());
      }
    }
  }

  // Hash variant selections (sorted by set name for determinism)
  if (ps.metas().variants.has_value()) {
    // VariantSelectionMap is std::map, so already sorted
    for (const auto &v : *ps.metas().variants) {
      hasher.Update(v.first.data(), v.first.size());
      hasher.Update(v.second.data(), v.second.size());
    }
  }

  hasher.Final(&key->hash_lo, &key->hash_hi);
  return true;
}

bool ComputeInstanceKeyFromPrimMetas(const PrimMeta &metas,
                                     const std::string &type_name,
                                     InstanceKey *key) {
  if (!key) return false;

  if (!metas.has_instanceable() || !metas.get_instanceable()) {
    return false;
  }

  SpookyHash hasher;
  hasher.Init(0xDEADBEEF, 0xCAFEBABE);

  // Hash type name
  if (!type_name.empty()) {
    hasher.Update(type_name.data(), type_name.size());
  }

  // Hash references
  if (metas.references.has_value()) {
    for (const auto &listop : *metas.references) {
      uint8_t qual = static_cast<uint8_t>(listop.first);
      hasher.Update(&qual, sizeof(qual));
      for (const auto &ref : listop.second) {
        std::string ap = ref.asset_path.GetAssetPath();
        hasher.Update(ap.data(), ap.size());
        std::string pp = ref.prim_path.prim_part();
        hasher.Update(pp.data(), pp.size());
        hasher.Update(&ref.layerOffset._offset, sizeof(ref.layerOffset._offset));
        hasher.Update(&ref.layerOffset._scale, sizeof(ref.layerOffset._scale));
      }
    }
  }

  // Hash payloads
  if (metas.payload.has_value()) {
    for (const auto &listop : *metas.payload) {
      uint8_t qual = static_cast<uint8_t>(listop.first);
      hasher.Update(&qual, sizeof(qual));
      for (const auto &pl : listop.second) {
        std::string ap = pl.asset_path.GetAssetPath();
        hasher.Update(ap.data(), ap.size());
        std::string pp = pl.prim_path.prim_part();
        hasher.Update(pp.data(), pp.size());
        hasher.Update(&pl.layerOffset._offset, sizeof(pl.layerOffset._offset));
        hasher.Update(&pl.layerOffset._scale, sizeof(pl.layerOffset._scale));
      }
    }
  }

  // Hash inherits
  if (metas.inherits.has_value()) {
    for (const auto &listop : *metas.inherits) {
      uint8_t qual = static_cast<uint8_t>(listop.first);
      hasher.Update(&qual, sizeof(qual));
      for (const auto &p : listop.second) {
        std::string pp = p.prim_part();
        hasher.Update(pp.data(), pp.size());
      }
    }
  }

  // Hash specializes
  if (metas.specializes.has_value()) {
    for (const auto &listop : *metas.specializes) {
      uint8_t qual = static_cast<uint8_t>(listop.first);
      hasher.Update(&qual, sizeof(qual));
      for (const auto &p : listop.second) {
        std::string pp = p.prim_part();
        hasher.Update(pp.data(), pp.size());
      }
    }
  }

  // Hash variant selections (sorted by set name for determinism)
  if (metas.variants.has_value()) {
    for (const auto &v : *metas.variants) {
      hasher.Update(v.first.data(), v.first.size());
      hasher.Update(v.second.data(), v.second.size());
    }
  }

  hasher.Final(&key->hash_lo, &key->hash_hi);
  return true;
}

}  // namespace lightusd
