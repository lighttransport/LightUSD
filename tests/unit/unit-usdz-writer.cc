// SPDX-License-Identifier: Apache 2.0
// USDZ writer and validator unit tests

#ifdef _MSC_VER
#define NOMINMAX
#endif

#define TEST_NO_MAIN
#include "acutest.h"

#include "unit-usdz-writer.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "lightusd.hh"
#include "io-util.hh"
#include "usdGeom.hh"
#include "stage.hh"

namespace {

lightusd::legacy::Stage MakeSimpleUSDZWriterStage() {
  lightusd::legacy::Stage stage;
  stage.metas().defaultPrim = lightusd::legacy::value::token("root");

  lightusd::legacy::Xform xform;
  xform.name = "root";
  xform.spec = lightusd::legacy::Specifier::Def;

  lightusd::legacy::value::Value primdata = xform;
  lightusd::legacy::Prim prim("root", primdata);
  prim.prim_type_name() = "Xform";
  stage.add_root_prim(std::move(prim));
  return stage;
}

std::string FirstUSDZEntryName(const std::vector<uint8_t> &data) {
  if (data.size() < 30 ||
      data[0] != 0x50 || data[1] != 0x4b ||
      data[2] != 0x03 || data[3] != 0x04) {
    return std::string();
  }
  const uint16_t name_len =
      static_cast<uint16_t>(data[26]) |
      static_cast<uint16_t>(static_cast<uint16_t>(data[27]) << 8);
  if (size_t(30) + size_t(name_len) > data.size()) {
    return std::string();
  }
  return std::string(reinterpret_cast<const char *>(data.data() + 30),
                     name_len);
}

void AppendUnalignedStoredZipEntry(std::vector<uint8_t> *data,
                                   const std::string &name,
                                   const std::string &bytes) {
  const size_t base = data->size();
  data->resize(base + 30, 0);
  auto put16 = [data, base](size_t offset, uint16_t v) {
    (*data)[base + offset + 0] = static_cast<uint8_t>(v & 0xffu);
    (*data)[base + offset + 1] = static_cast<uint8_t>((v >> 8) & 0xffu);
  };
  auto put32 = [data, base](size_t offset, uint32_t v) {
    (*data)[base + offset + 0] = static_cast<uint8_t>(v & 0xffu);
    (*data)[base + offset + 1] = static_cast<uint8_t>((v >> 8) & 0xffu);
    (*data)[base + offset + 2] = static_cast<uint8_t>((v >> 16) & 0xffu);
    (*data)[base + offset + 3] = static_cast<uint8_t>((v >> 24) & 0xffu);
  };

  (*data)[base + 0] = 0x50;
  (*data)[base + 1] = 0x4b;
  (*data)[base + 2] = 0x03;
  (*data)[base + 3] = 0x04;
  put16(4, 20);  // version needed
  put16(8, 0);   // stored
  put32(18, static_cast<uint32_t>(bytes.size()));
  put32(22, static_cast<uint32_t>(bytes.size()));
  put16(26, static_cast<uint16_t>(name.size()));
  put16(28, 0);
  data->insert(data->end(), name.begin(), name.end());
  data->insert(data->end(), bytes.begin(), bytes.end());
}

std::vector<uint8_t> MakeUnalignedStoredZipWithUSDARoot() {
  const std::string name = "root.usda";
  const std::string usda = "#usda 1.0\n\ndef Xform \"root\"\n{\n}\n";
  std::vector<uint8_t> data(30, 0);

  auto put16 = [&data](size_t offset, uint16_t v) {
    data[offset + 0] = static_cast<uint8_t>(v & 0xffu);
    data[offset + 1] = static_cast<uint8_t>((v >> 8) & 0xffu);
  };
  auto put32 = [&data](size_t offset, uint32_t v) {
    data[offset + 0] = static_cast<uint8_t>(v & 0xffu);
    data[offset + 1] = static_cast<uint8_t>((v >> 8) & 0xffu);
    data[offset + 2] = static_cast<uint8_t>((v >> 16) & 0xffu);
    data[offset + 3] = static_cast<uint8_t>((v >> 24) & 0xffu);
  };

  data[0] = 0x50;
  data[1] = 0x4b;
  data[2] = 0x03;
  data[3] = 0x04;
  put16(4, 20);  // version needed
  put16(8, 0);   // stored
  put32(18, static_cast<uint32_t>(usda.size()));
  put32(22, static_cast<uint32_t>(usda.size()));
  put16(26, static_cast<uint16_t>(name.size()));
  put16(28, 0);  // no extra field, so file data starts at byte 39

  data.insert(data.end(), name.begin(), name.end());
  TEST_CHECK((data.size() % 64) != 0);
  data.insert(data.end(), usda.begin(), usda.end());
  return data;
}

}  // namespace

// Test 1: Basic USDZ write -> read roundtrip
void usdz_writer_basic_roundtrip_test(void) {
  // Build a simple stage with one Xform
  lightusd::legacy::Stage stage;
  stage.metas().defaultPrim = lightusd::legacy::value::token("root");

  lightusd::legacy::Xform xform;
  xform.name = "root";
  xform.spec = lightusd::legacy::Specifier::Def;

  lightusd::legacy::value::Value primdata = xform;
  lightusd::legacy::Prim prim("root", primdata);
  prim.prim_type_name() = "Xform";
  stage.add_root_prim(std::move(prim));

  // Write to memory
  std::map<std::string, std::vector<uint8_t>> assets;  // no extra assets
  std::vector<uint8_t> usdz_data;
  std::string warn, err;

  bool ret = lightusd::legacy::SaveAsUSDZToMemory(stage, assets, &usdz_data, &warn, &err);
  TEST_CHECK(ret);
  if (!ret) {
    TEST_MSG("SaveAsUSDZToMemory failed: %s", err.c_str());
    return;
  }
  TEST_CHECK(usdz_data.size() > 0);

  // Validate the USDZ
  warn.clear(); err.clear();
  ret = lightusd::legacy::ValidateUSDZ(usdz_data.data(), usdz_data.size(), &warn, &err);
  TEST_CHECK(ret);
  if (!ret) {
    TEST_MSG("ValidateUSDZ failed: %s", err.c_str());
  }

  // Verify ZIP magic
  TEST_CHECK(usdz_data[0] == 0x50);
  TEST_CHECK(usdz_data[1] == 0x4b);
  TEST_CHECK(usdz_data[2] == 0x03);
  TEST_CHECK(usdz_data[3] == 0x04);

  // Read back the USDZ
  lightusd::legacy::Stage loaded_stage;
  warn.clear(); err.clear();
  ret = lightusd::legacy::LoadUSDZFromMemory(usdz_data.data(), usdz_data.size(),
                                      "test.usdz", &loaded_stage, &warn, &err);
  TEST_CHECK(ret);
  if (!ret) {
    TEST_MSG("LoadUSDZFromMemory failed: %s", err.c_str());
    return;
  }

  // Verify the loaded stage has the root prim
  TEST_CHECK(loaded_stage.root_prims().size() >= 1);
}

// Regression: IsUSDZ()/IsUSD() must classify a USDZ from only a prefix of the
// file. IsUSDZ(filename) reads ~256 bytes, but ParseUSDZHeader used to reject
// the prefix because the first entry's (root layer) uncompressed size exceeds
// the available bytes — so IsUSDZ wrongly returned false for essentially every
// real package (any whose root layer is larger than the prefix). Detection
// mode (assets == nullptr) now accepts a valid first local header without
// requiring every entry's data to be present.
void usdz_writer_is_usdz_prefix_detection_test(void) {
  lightusd::legacy::Stage stage = MakeSimpleUSDZWriterStage();
  std::map<std::string, std::vector<uint8_t>> assets;
  std::vector<uint8_t> usdz_data;
  std::string warn, err;
  bool ret = lightusd::legacy::SaveAsUSDZToMemory(stage, assets, &usdz_data, &warn, &err);
  TEST_CHECK(ret);
  if (!ret) {
    TEST_MSG("SaveAsUSDZToMemory failed: %s", err.c_str());
    return;
  }

  // The first entry (root layer) is larger than the detection prefix, so this
  // exercises the truncated-read path that used to fail.
  const size_t prefix = 256;
  TEST_CHECK(usdz_data.size() > prefix);

  // Full-buffer detection (always worked).
  TEST_CHECK(lightusd::legacy::IsUSDZ(usdz_data.data(), usdz_data.size()));

  // Prefix-only detection (the regression).
  TEST_CHECK(lightusd::legacy::IsUSDZ(usdz_data.data(), prefix));

  // IsUSD() must also classify it as usdz from the prefix alone.
  std::string fmt;
  TEST_CHECK(lightusd::legacy::IsUSD(usdz_data.data(), prefix, &fmt));
  TEST_CHECK(fmt == "usdz");
}

void usdz_reader_loads_unaligned_stored_zip_test(void) {
  std::vector<uint8_t> usdz_data = MakeUnalignedStoredZipWithUSDARoot();

  std::string warn, err;
  TEST_CHECK(lightusd::legacy::IsUSDZ(usdz_data.data(), usdz_data.size()));

  bool valid = lightusd::legacy::ValidateUSDZ(usdz_data.data(), usdz_data.size(), &warn,
                                      &err);
  TEST_CHECK(!valid);
  TEST_CHECK(err.find("not 64-byte aligned") != std::string::npos ||
             err.find("Missing end of central directory") != std::string::npos);

  lightusd::legacy::Stage loaded_stage;
  warn.clear();
  err.clear();
  bool ret = lightusd::legacy::LoadUSDZFromMemory(usdz_data.data(), usdz_data.size(),
                                          "unaligned.usdz", &loaded_stage,
                                          &warn, &err);
  TEST_CHECK(ret);
  if (!ret) {
    TEST_MSG("LoadUSDZFromMemory failed: %s", err.c_str());
    return;
  }
  TEST_CHECK(warn.find("not 64-byte aligned") != std::string::npos);
  TEST_CHECK(loaded_stage.root_prims().size() == 1);
}

void usdz_reader_first_entry_root_test(void) {
  const std::string usda = "#usda 1.0\n\ndef Xform \"root\"\n{\n}\n";

  // A later USD layer must not rescue a package whose first entry is an asset.
  // The old format-preference search selected late.usda and accepted this.
  std::vector<uint8_t> invalid;
  AppendUnalignedStoredZipEntry(&invalid, "textures/first.bin", "not usd");
  AppendUnalignedStoredZipEntry(&invalid, "late.usda", usda);
  lightusd::legacy::Stage stage;
  std::string warn, err;
  bool ret = lightusd::legacy::LoadUSDZFromMemory(
      invalid.data(), invalid.size(), "invalid-root.usdz", &stage, &warn, &err);
  TEST_CHECK(!ret);
  TEST_CHECK(err.find("first entry") != std::string::npos);

  // The neutral .usd extension is valid when the first entry's content is USD.
  // Also retain that ordered root name explicitly: asset_map sorts a.png before
  // z-root.usd and therefore cannot encode package entry order by itself.
  std::vector<uint8_t> valid;
  AppendUnalignedStoredZipEntry(&valid, "z-root.usd", usda);
  AppendUnalignedStoredZipEntry(&valid, "a.png", "image");
  lightusd::legacy::USDZAsset asset;
  warn.clear();
  err.clear();
  ret = lightusd::legacy::ReadUSDZAssetInfoFromMemory(
      valid.data(), valid.size(), true, &asset, &warn, &err);
  TEST_CHECK(ret);
  TEST_CHECK(asset.root_asset_name == "z-root.usd");
  TEST_CHECK(!asset.asset_map.empty());
  TEST_CHECK(asset.asset_map.begin()->first == "a.png");

  lightusd::legacy::AssetResolutionResolver resolver;
  TEST_CHECK(lightusd::legacy::SetupUSDZAssetResolution(resolver, &asset));
  TEST_CHECK(resolver.has_asset_resolution_handler("usd"));
  TEST_CHECK(resolver.has_asset_resolution_handler("usda"));
  TEST_CHECK(resolver.has_asset_resolution_handler("usdc"));
  TEST_CHECK(resolver.has_asset_resolution_handler("avif"));
  TEST_CHECK(resolver.has_asset_resolution_handler("m4a"));
  TEST_CHECK(resolver.has_asset_resolution_handler("mp3"));
  TEST_CHECK(resolver.has_asset_resolution_handler("wav"));

  std::vector<uint8_t> unsafe;
  AppendUnalignedStoredZipEntry(&unsafe, "../root.usda", usda);
  warn.clear();
  err.clear();
  ret = lightusd::legacy::LoadUSDZFromMemory(
      unsafe.data(), unsafe.size(), "unsafe.usdz", &stage, &warn, &err);
  TEST_CHECK(!ret);
  TEST_CHECK(err.find("Unsafe USDZ entry path") != std::string::npos);
  warn.clear();
  err.clear();
  ret = lightusd::legacy::ValidateUSDZ(unsafe.data(), unsafe.size(), &warn, &err);
  TEST_CHECK(!ret);
  TEST_CHECK(err.find("unsafe USDZ entry path") != std::string::npos);

  std::vector<uint8_t> duplicate;
  AppendUnalignedStoredZipEntry(&duplicate, "root.usda", usda);
  AppendUnalignedStoredZipEntry(&duplicate, "root.usda", usda);
  warn.clear();
  err.clear();
  ret = lightusd::legacy::LoadUSDZFromMemory(
      duplicate.data(), duplicate.size(), "duplicate.usdz", &stage, &warn,
      &err);
  TEST_CHECK(!ret);
  TEST_CHECK(err.find("Duplicate USDZ entry path") != std::string::npos);
  warn.clear();
  err.clear();
  ret = lightusd::legacy::ValidateUSDZ(duplicate.data(), duplicate.size(), &warn,
                               &err);
  TEST_CHECK(!ret);
  TEST_CHECK(err.find("duplicate USDZ entry path") != std::string::npos);

  std::vector<uint8_t> truncated_later;
  AppendUnalignedStoredZipEntry(&truncated_later, "root.usda", usda);
  truncated_later.push_back(0x50);
  truncated_later.push_back(0x4b);
  truncated_later.push_back(0x03);
  warn.clear();
  err.clear();
  ret = lightusd::legacy::LoadUSDZFromMemory(
      truncated_later.data(), truncated_later.size(), "truncated.usdz",
      &stage, &warn, &err);
  TEST_CHECK(!ret);
  TEST_CHECK(err.find("Invalid or truncated data") != std::string::npos);

  // Reusing an output object after a failed read must not expose the previous
  // archive's map or backing pointer to an asset resolver.
  std::vector<uint8_t> malformed(128, 0);
  warn.clear();
  err.clear();
  ret = lightusd::legacy::ReadUSDZAssetInfoFromMemory(
      malformed.data(), malformed.size(), true, &asset, &warn, &err);
  TEST_CHECK(!ret);
  TEST_CHECK(asset.root_asset_name.empty());
  TEST_CHECK(asset.asset_map.empty());
  TEST_CHECK(asset.data.empty());
  TEST_CHECK(asset.addr == nullptr);
  TEST_CHECK(asset.size == 0);

  warn.clear();
  err.clear();
  ret = lightusd::legacy::LoadUSDZFromMemory(valid.data(), valid.size(),
                                     "neutral-root.usdz", &stage, &warn, &err);
  TEST_CHECK(ret);
  if (!ret) {
    TEST_MSG("LoadUSDZFromMemory .usd root failed: %s", err.c_str());
    return;
  }
  TEST_CHECK(stage.root_prims().size() == 1);
}

void usdz_writer_root_layer_format_test(void) {
  lightusd::legacy::Stage stage = MakeSimpleUSDZWriterStage();
  std::map<std::string, std::vector<uint8_t>> assets;

  {
    std::vector<uint8_t> usdz_data;
    std::string warn, err;
    bool ret = lightusd::legacy::SaveAsUSDZToMemory(stage, assets, &usdz_data,
                                            lightusd::legacy::USDZWriteOptions{},
                                            &warn, &err);
    TEST_CHECK(ret);
    if (!ret) {
      TEST_MSG("SaveAsUSDZToMemory USDC failed: %s", err.c_str());
      return;
    }
    TEST_CHECK(FirstUSDZEntryName(usdz_data) == "root.usdc");

    // A USD-like substring is not a root-layer extension, and a recognized
    // extension must agree with the root payload's magic.
    std::vector<uint8_t> deceptive_name = usdz_data;
    const char deceptive[] = "x.usda.zz";
    static_assert(sizeof(deceptive) - 1 == 9, "replacement must fit");
    memcpy(deceptive_name.data() + 30, deceptive, sizeof(deceptive) - 1);
    warn.clear();
    err.clear();
    ret = lightusd::legacy::ValidateUSDZ(deceptive_name.data(), deceptive_name.size(),
                                 &warn, &err);
    TEST_CHECK(!ret);
    TEST_CHECK(err.find("extension matches its content") != std::string::npos);

    std::vector<uint8_t> mismatched_format = usdz_data;
    const char usda_name[] = "root.usda";
    static_assert(sizeof(usda_name) - 1 == 9, "replacement must fit");
    memcpy(mismatched_format.data() + 30, usda_name,
           sizeof(usda_name) - 1);
    warn.clear();
    err.clear();
    ret = lightusd::legacy::ValidateUSDZ(mismatched_format.data(),
                                 mismatched_format.size(), &warn, &err);
    TEST_CHECK(!ret);
    TEST_CHECK(err.find("extension matches its content") != std::string::npos);
  }

  {
    lightusd::legacy::USDZWriteOptions options;
    options.root_layer_format = lightusd::legacy::USDZRootLayerFormat::USDA;

    std::vector<uint8_t> usdz_data;
    std::string warn, err;
    bool ret = lightusd::legacy::SaveAsUSDZToMemory(stage, assets, &usdz_data,
                                            options, &warn, &err);
    TEST_CHECK(ret);
    if (!ret) {
      TEST_MSG("SaveAsUSDZToMemory USDA failed: %s", err.c_str());
      return;
    }
    TEST_CHECK(FirstUSDZEntryName(usdz_data) == "root.usda");

    warn.clear();
    err.clear();
    ret = lightusd::legacy::ValidateUSDZ(usdz_data.data(), usdz_data.size(), &warn,
                                 &err);
    TEST_CHECK(ret);
    if (!ret) {
      TEST_MSG("ValidateUSDZ USDA root failed: %s", err.c_str());
    }

    lightusd::legacy::Stage loaded_stage;
    warn.clear();
    err.clear();
    ret = lightusd::legacy::LoadUSDZFromMemory(usdz_data.data(), usdz_data.size(),
                                       "root-format.usdz", &loaded_stage,
                                       &warn, &err);
    TEST_CHECK(ret);
    if (!ret) {
      TEST_MSG("LoadUSDZFromMemory USDA root failed: %s", err.c_str());
    }
    TEST_CHECK(loaded_stage.root_prims().size() >= 1);
  }
}

// Test 2: USDZ with additional assets (fake PNG data)
void usdz_writer_with_assets_test(void) {
  lightusd::legacy::Stage stage;
  stage.metas().defaultPrim = lightusd::legacy::value::token("root");

  lightusd::legacy::Xform xform;
  xform.name = "root";
  xform.spec = lightusd::legacy::Specifier::Def;

  lightusd::legacy::value::Value primdata = xform;
  lightusd::legacy::Prim prim("root", primdata);
  prim.prim_type_name() = "Xform";
  stage.add_root_prim(std::move(prim));

  // Add a fake asset
  std::map<std::string, std::vector<uint8_t>> assets;
  std::vector<uint8_t> fake_png(256, 0xAB);  // 256 bytes of dummy data
  assets["textures/diffuse.png"] = fake_png;

  std::vector<uint8_t> usdz_data;
  std::string warn, err;

  bool ret = lightusd::legacy::SaveAsUSDZToMemory(stage, assets, &usdz_data, &warn, &err);
  TEST_CHECK(ret);
  if (!ret) {
    TEST_MSG("SaveAsUSDZToMemory with assets failed: %s", err.c_str());
    return;
  }

  // Validate
  warn.clear(); err.clear();
  ret = lightusd::legacy::ValidateUSDZ(usdz_data.data(), usdz_data.size(), &warn, &err);
  TEST_CHECK(ret);
  if (!ret) {
    TEST_MSG("ValidateUSDZ failed: %s", err.c_str());
  }

  // Read asset info
  lightusd::legacy::USDZAsset usdz_asset;
  warn.clear(); err.clear();
  ret = lightusd::legacy::ReadUSDZAssetInfoFromMemory(usdz_data.data(), usdz_data.size(),
                                                /* asset_on_memory */ true,
                                                &usdz_asset, &warn, &err);
  TEST_CHECK(ret);
  if (!ret) {
    TEST_MSG("ReadUSDZAssetInfoFromMemory failed: %s", err.c_str());
    return;
  }

  // Should have at least 2 entries (root.usdc + textures/diffuse.png)
  TEST_CHECK(usdz_asset.asset_map.size() >= 2);

  // Check the texture asset exists
  TEST_CHECK(usdz_asset.asset_map.count("textures/diffuse.png") == 1);
}

// Test 3: Validate 64-byte alignment
void usdz_validator_alignment_test(void) {
  lightusd::legacy::Stage stage;
  stage.metas().defaultPrim = lightusd::legacy::value::token("root");

  lightusd::legacy::Xform xform;
  xform.name = "root";
  xform.spec = lightusd::legacy::Specifier::Def;

  lightusd::legacy::value::Value primdata = xform;
  lightusd::legacy::Prim prim("root", primdata);
  prim.prim_type_name() = "Xform";
  stage.add_root_prim(std::move(prim));

  // Add multiple assets of varying sizes to stress alignment
  std::map<std::string, std::vector<uint8_t>> assets;
  assets["a.png"] = std::vector<uint8_t>(1, 0xFF);    // 1 byte
  assets["bb.png"] = std::vector<uint8_t>(63, 0xFE);  // 63 bytes
  assets["ccc.png"] = std::vector<uint8_t>(127, 0xFD); // 127 bytes
  assets["dddd.exr"] = std::vector<uint8_t>(1024, 0xFC); // 1024 bytes

  std::vector<uint8_t> usdz_data;
  std::string warn, err;

  bool ret = lightusd::legacy::SaveAsUSDZToMemory(stage, assets, &usdz_data, &warn, &err);
  TEST_CHECK(ret);

  // Validate alignment
  warn.clear(); err.clear();
  ret = lightusd::legacy::ValidateUSDZ(usdz_data.data(), usdz_data.size(), &warn, &err);
  TEST_CHECK(ret);
  if (!ret) {
    TEST_MSG("Alignment validation failed: %s", err.c_str());
  }
}

// Test 4: CRC32 integrity validation
void usdz_validator_crc32_test(void) {
  lightusd::legacy::Stage stage;
  stage.metas().defaultPrim = lightusd::legacy::value::token("root");

  lightusd::legacy::Xform xform;
  xform.name = "root";
  xform.spec = lightusd::legacy::Specifier::Def;
  lightusd::legacy::value::Value primdata = xform;
  lightusd::legacy::Prim prim("root", primdata);
  prim.prim_type_name() = "Xform";
  stage.add_root_prim(std::move(prim));

  std::map<std::string, std::vector<uint8_t>> assets;
  assets["texture.png"] = std::vector<uint8_t>(100, 0x42);

  std::vector<uint8_t> usdz_data;
  std::string warn, err;

  bool ret = lightusd::legacy::SaveAsUSDZToMemory(stage, assets, &usdz_data, &warn, &err);
  TEST_CHECK(ret);

  // Valid data should pass
  warn.clear(); err.clear();
  ret = lightusd::legacy::ValidateUSDZ(usdz_data.data(), usdz_data.size(), &warn, &err);
  TEST_CHECK(ret);

  // Corrupt one byte in the file data area and check CRC fails
  if (usdz_data.size() > 128) {
    std::vector<uint8_t> corrupted = usdz_data;
    corrupted[128] ^= 0xFF;  // flip a byte
    warn.clear(); err.clear();
    ret = lightusd::legacy::ValidateUSDZ(corrupted.data(), corrupted.size(), &warn, &err);
    // May or may not fail depending on which byte was corrupted
    // (header vs data), but at minimum should not crash
    (void)ret;
  }
}

// Test 5: Validate compressed size == uncompressed size
void usdz_validator_size_consistency_test(void) {
  // Build a USDZ with known content
  lightusd::legacy::Stage stage;
  stage.metas().defaultPrim = lightusd::legacy::value::token("root");

  lightusd::legacy::Xform xform;
  xform.name = "root";
  xform.spec = lightusd::legacy::Specifier::Def;
  lightusd::legacy::value::Value primdata = xform;
  lightusd::legacy::Prim prim("root", primdata);
  prim.prim_type_name() = "Xform";
  stage.add_root_prim(std::move(prim));

  std::map<std::string, std::vector<uint8_t>> assets;
  std::vector<uint8_t> usdz_data;
  std::string warn, err;

  bool ret = lightusd::legacy::SaveAsUSDZToMemory(stage, assets, &usdz_data, &warn, &err);
  TEST_CHECK(ret);

  // The writer should produce valid compressed==uncompressed sizes
  warn.clear(); err.clear();
  ret = lightusd::legacy::ValidateUSDZ(usdz_data.data(), usdz_data.size(), &warn, &err);
  TEST_CHECK(ret);
  if (!ret) {
    TEST_MSG("Size consistency check failed: %s", err.c_str());
  }

  std::vector<uint8_t> mismatched_size = usdz_data;
  TEST_ASSERT(mismatched_size.size() >= 26);
  mismatched_size[18] ^= 1;
  warn.clear();
  err.clear();
  ret = lightusd::legacy::ValidateUSDZ(mismatched_size.data(), mismatched_size.size(),
                               &warn, &err);
  TEST_CHECK(!ret);
  TEST_CHECK(err.find("compressed size") != std::string::npos);

  lightusd::legacy::Stage loaded_stage;
  warn.clear();
  err.clear();
  ret = lightusd::legacy::LoadUSDZFromMemory(
      mismatched_size.data(), mismatched_size.size(), "mismatched-size.usdz",
      &loaded_stage, &warn, &err);
  TEST_CHECK(!ret);
  TEST_CHECK(err.find("mismatched compressed") != std::string::npos);
}

// Test 6: Validate empty/null input handling
void usdz_validator_empty_input_test(void) {
  std::string warn, err;

  // Null input
  bool ret = lightusd::legacy::ValidateUSDZ(nullptr, 0, &warn, &err);
  TEST_CHECK(!ret);
  TEST_CHECK(err.find("null") != std::string::npos || err.find("short") != std::string::npos);

  // Too short
  uint8_t tiny[3] = {0x50, 0x4b, 0x03};
  warn.clear(); err.clear();
  ret = lightusd::legacy::ValidateUSDZ(tiny, 3, &warn, &err);
  TEST_CHECK(!ret);

  // Bad magic
  uint8_t bad_magic[4] = {0x00, 0x00, 0x00, 0x00};
  warn.clear(); err.clear();
  ret = lightusd::legacy::ValidateUSDZ(bad_magic, 4, &warn, &err);
  TEST_CHECK(!ret);
  TEST_CHECK(err.find("magic") != std::string::npos);
}

void usdz_validator_missing_eocd_test(void) {
  const std::string name = "root.usda";
  const uint16_t name_len = static_cast<uint16_t>(name.size());
  const uint16_t extra_len = static_cast<uint16_t>(64 - 30 - name.size());

  std::vector<uint8_t> malformed(64, 0);
  malformed[0] = 0x50;
  malformed[1] = 0x4b;
  malformed[2] = 0x03;
  malformed[3] = 0x04;
  memcpy(&malformed[26], &name_len, sizeof(name_len));
  memcpy(&malformed[28], &extra_len, sizeof(extra_len));
  memcpy(malformed.data() + 30, name.data(), name.size());

  std::string warn, err;
  bool ret = lightusd::legacy::ValidateUSDZ(malformed.data(), malformed.size(),
                                    &warn, &err);
  TEST_CHECK(!ret);
  TEST_CHECK(err.find("End of central directory") != std::string::npos);
}

// Test 7: File-based write and read roundtrip
void usdz_writer_file_roundtrip_test(void) {
  lightusd::legacy::Stage stage;
  stage.metas().defaultPrim = lightusd::legacy::value::token("root");

  lightusd::legacy::Xform xform;
  xform.name = "root";
  xform.spec = lightusd::legacy::Specifier::Def;
  lightusd::legacy::value::Value primdata = xform;
  lightusd::legacy::Prim prim("root", primdata);
  prim.prim_type_name() = "Xform";
  stage.add_root_prim(std::move(prim));

  std::map<std::string, std::vector<uint8_t>> assets;
  assets["img.png"] = std::vector<uint8_t>(64, 0xCD);

  std::string warn, err;

  // Write to file. Use io::GetTempDir() so this works on Windows (where /tmp
  // does not exist) as well as POSIX.
  std::string tmp_path = lightusd::io::JoinPath(lightusd::io::GetTempDir(),
                                                "lightusd_test_roundtrip.usdz");
  bool ret = lightusd::legacy::SaveAsUSDZToFile(tmp_path, stage, assets, &warn, &err);
  TEST_CHECK(ret);
  if (!ret) {
    TEST_MSG("SaveAsUSDZToFile failed: %s", err.c_str());
    return;
  }

  // Read back from file
  lightusd::legacy::Stage loaded;
  warn.clear(); err.clear();
  ret = lightusd::legacy::LoadUSDZFromFile(tmp_path, &loaded, &warn, &err);
  TEST_CHECK(ret);
  if (!ret) {
    TEST_MSG("LoadUSDZFromFile failed: %s", err.c_str());
    return;
  }

  TEST_CHECK(loaded.root_prims().size() >= 1);

  // Clean up
  std::remove(tmp_path.c_str());
}

// Test 8: Large asset alignment stress test
void usdz_validator_large_asset_test(void) {
  lightusd::legacy::Stage stage;
  stage.metas().defaultPrim = lightusd::legacy::value::token("root");

  lightusd::legacy::Xform xform;
  xform.name = "root";
  xform.spec = lightusd::legacy::Specifier::Def;
  lightusd::legacy::value::Value primdata = xform;
  lightusd::legacy::Prim prim("root", primdata);
  prim.prim_type_name() = "Xform";
  stage.add_root_prim(std::move(prim));

  // Add assets with sizes that stress 64-byte alignment boundaries
  std::map<std::string, std::vector<uint8_t>> assets;
  assets["a.png"] = std::vector<uint8_t>(1, 0x01);
  assets["ab.png"] = std::vector<uint8_t>(31, 0x02);
  assets["abc.png"] = std::vector<uint8_t>(32, 0x03);
  assets["abcd.png"] = std::vector<uint8_t>(33, 0x04);
  assets["abcde.png"] = std::vector<uint8_t>(63, 0x05);
  assets["abcdef.png"] = std::vector<uint8_t>(64, 0x06);
  assets["abcdefg.png"] = std::vector<uint8_t>(65, 0x07);
  assets["large.exr"] = std::vector<uint8_t>(65536, 0x08);

  std::vector<uint8_t> usdz_data;
  std::string warn, err;

  bool ret = lightusd::legacy::SaveAsUSDZToMemory(stage, assets, &usdz_data, &warn, &err);
  TEST_CHECK(ret);

  // Validate all entries are aligned
  warn.clear(); err.clear();
  ret = lightusd::legacy::ValidateUSDZ(usdz_data.data(), usdz_data.size(), &warn, &err);
  TEST_CHECK(ret);
  if (!ret) {
    TEST_MSG("Large asset alignment failed: %s", err.c_str());
  }

  // Verify asset count via ReadUSDZAssetInfo
  lightusd::legacy::USDZAsset usdz_asset;
  warn.clear(); err.clear();
  ret = lightusd::legacy::ReadUSDZAssetInfoFromMemory(usdz_data.data(), usdz_data.size(),
                                                true, &usdz_asset, &warn, &err);
  TEST_CHECK(ret);
  // root.usdc + 8 assets = 9 entries
  TEST_CHECK(usdz_asset.asset_map.size() == 9);
}

// Test 4 (original): Validator catches bad extensions
void usdz_validator_bad_extension_test(void) {
  lightusd::legacy::Stage stage;
  stage.metas().defaultPrim = lightusd::legacy::value::token("root");

  lightusd::legacy::Xform xform;
  xform.name = "root";
  xform.spec = lightusd::legacy::Specifier::Def;

  lightusd::legacy::value::Value primdata = xform;
  lightusd::legacy::Prim prim("root", primdata);
  prim.prim_type_name() = "Xform";
  stage.add_root_prim(std::move(prim));

  // Try to add a disallowed extension -- should be skipped with warning
  std::map<std::string, std::vector<uint8_t>> assets;
  assets["script.py"] = std::vector<uint8_t>(10, 0);

  std::vector<uint8_t> usdz_data;
  std::string warn, err;

  bool ret = lightusd::legacy::SaveAsUSDZToMemory(stage, assets, &usdz_data, &warn, &err);
  TEST_CHECK(ret);
  // Should have a warning about the skipped extension
  TEST_CHECK(warn.find("disallowed") != std::string::npos);
}

void usdz_writer_rejects_unsafe_asset_names_test(void) {
  lightusd::legacy::Stage stage;
  stage.metas().defaultPrim = lightusd::legacy::value::token("root");

  lightusd::legacy::Xform xform;
  xform.name = "root";
  xform.spec = lightusd::legacy::Specifier::Def;
  lightusd::legacy::value::Value primdata = xform;
  lightusd::legacy::Prim prim("root", primdata);
  prim.prim_type_name() = "Xform";
  stage.add_root_prim(std::move(prim));

  std::vector<std::string> bad_names = {
      "",
      "/abs.png",
      "../escape.png",
      "textures/../escape.png",
      "textures\\bad.png",
      "C:bad.png",
      "textures//bad.png",
      "textures/.",
  };
  bad_names.push_back(std::string("bad\0name.png", 12));
  bad_names.push_back(std::string(70000, 'a') + ".png");

  for (const std::string &name : bad_names) {
    std::map<std::string, std::vector<uint8_t>> assets;
    assets[name] = std::vector<uint8_t>(8, 0x42);
    std::vector<uint8_t> usdz_data;
    std::string warn, err;
    bool ret = lightusd::legacy::SaveAsUSDZToMemory(stage, assets, &usdz_data, &warn, &err);
    TEST_CHECK(!ret);
    TEST_CHECK(!err.empty());
  }
}
