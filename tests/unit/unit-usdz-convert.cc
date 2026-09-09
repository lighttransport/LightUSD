// SPDX-License-Identifier: Apache 2.0
// usdz-convert pipeline + texture op + fpnge unit tests

#ifdef _MSC_VER
#define NOMINMAX
#endif

#define TEST_NO_MAIN
#include "acutest.h"

#include "unit-usdz-convert.h"

#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#include "lightusd.hh"
#include "image-types.hh"
#include "image-loader.hh"
#include "image-writer.hh"
#include "io-util.hh"
#include "tydra/texture-util.hh"
#include "usdz-convert.hh"
#include "usdz-geometry-optimize.hh"
#include "usdz-material-optimize.hh"
#include "usdShade.hh"

namespace {

lightusd::Image MakeSolidImage(int w, int h, int channels, uint8_t r, uint8_t g,
                               uint8_t b, uint8_t a) {
  lightusd::Image img;
  img.width = w;
  img.height = h;
  img.channels = channels;
  img.bpp = 8;
  img.format = lightusd::Image::PixelFormat::UInt;
  img.data.resize(size_t(w) * size_t(h) * size_t(channels));
  for (int i = 0; i < w * h; i++) {
    uint8_t px[4] = {r, g, b, a};
    for (int c = 0; c < channels; c++) {
      img.data[size_t(i) * size_t(channels) + size_t(c)] = px[c];
    }
  }
  return img;
}

// Deterministic high-entropy image so PNG/JPEG sizes are non-trivial.
lightusd::Image MakeNoisyImage(int w, int h, int channels) {
  lightusd::Image img;
  img.width = w; img.height = h; img.channels = channels; img.bpp = 8;
  img.format = lightusd::Image::PixelFormat::UInt;
  img.data.resize(size_t(w) * size_t(h) * size_t(channels));
  uint32_t s = 0x12345678u;
  for (auto &b : img.data) {
    s = s * 1664525u + 1013904223u;  // LCG
    b = uint8_t(s >> 24);
  }
  return img;
}

std::vector<uint8_t> EncodePNG(const lightusd::Image &img) {
  lightusd::image::WriteOption wopt;
  wopt.format = lightusd::image::WriteImageFormat::PNG;
  auto enc = lightusd::image::WriteImageToMemory(img, wopt);
  return enc ? enc.value() : std::vector<uint8_t>{};
}

std::string TempDir() {
  const std::string base =
      lightusd::io::JoinPath(lightusd::io::GetTempDir(), "lusdzconvert_test");
  lightusd::io::CreateDirectories(base);
  return base;
}

std::string TestFixturePath(const std::string &rel) {
  if (const char *root = std::getenv("LIGHTUSD_TEST_FIXTURE_DIR")) {
    if (*root != '\0') {
      const std::string candidate = lightusd::io::JoinPath(root, rel);
      if (lightusd::io::FileExists(candidate)) return candidate;
    }
  }

  // ctest sets WORKING_DIRECTORY to the test executable's directory, whose depth
  // below the repo root varies by platform/generator: build/ for single-config
  // Makefile/Ninja, but build/<Config>/ or build/tests/unit/<Config>/ for MSVC
  // multi-config. Walk up several parent levels until the fixture resolves so
  // the same relative path works everywhere.
  std::string candidate = rel;
  for (int depth = 0; depth < 8; ++depth) {
    if (lightusd::io::FileExists(candidate)) {
      return candidate;
    }
    candidate = lightusd::io::JoinPath("..", candidate);
  }
  return rel;
}

bool CopyFile(const std::string &src, const std::string &dst) {
  std::vector<uint8_t> bytes;
  std::string ioerr;
  if (!lightusd::io::ReadWholeFile(&bytes, &ioerr, src, 0)) {
    return false;
  }
  return lightusd::io::WriteWholeFile(dst, bytes.data(), bytes.size(), &ioerr);
}

bool WritePNG(const std::string &path, const lightusd::Image &img) {
  lightusd::image::WriteOption wopt;
  wopt.format = lightusd::image::WriteImageFormat::PNG;
  auto enc = lightusd::image::WriteImageToMemory(img, wopt);
  if (!enc) {
    return false;
  }
  std::string werr;
  return lightusd::io::WriteWholeFile(path, enc.value().data(),
                                      enc.value().size(), &werr);
}

size_t CountAggregateFaces(const lightusd::Layer &layer) {
  using namespace lightusd;
  const PrimSpec *root = nullptr;
  std::string err;
  if (!layer.find_primspec_at(Path("/__LightUSD_MeshMerge", ""), &root,
                              &err) ||
      !root) {
    return 0;
  }
  size_t total = 0;
  for (const PrimSpec &child : root->children()) {
    auto it = child.props().find("faceVertexCounts");
    if (it == child.props().end() || !it->second.is_attribute()) {
      continue;
    }
    auto counts = it->second.get_attribute().get_value<std::vector<int32_t>>();
    if (counts) {
      total += counts->size();
    }
  }
  return total;
}

size_t CountAggregateMeshes(const lightusd::Layer &layer) {
  using namespace lightusd;
  const PrimSpec *root = nullptr;
  std::string err;
  if (!layer.find_primspec_at(Path("/__LightUSD_MeshMerge", ""), &root,
                              &err) ||
      !root) {
    return 0;
  }
  return root->children().size();
}

size_t TestFileSize(const std::string &path) {
  std::ifstream ifs(path, std::ios::binary | std::ios::ate);
  if (!ifs) {
    return 0;
  }
  const std::streampos pos = ifs.tellg();
  if (pos < 0) {
    return 0;
  }
  return static_cast<size_t>(pos);
}

bool WriteMinimalUSDA(const std::string &path) {
  const std::string usda =
      "#usda 1.0\n"
      "(\n"
      "    defaultPrim = \"root\"\n"
      ")\n"
      "\n"
      "def Xform \"root\"\n"
      "{\n"
      "}\n";
  std::string werr;
  return lightusd::io::WriteWholeFile(
      path, reinterpret_cast<const unsigned char *>(usda.data()), usda.size(),
      &werr);
}

bool WriteTexturedUSDA(const std::string &path, const std::string &texture_path) {
  const std::string usda =
      "#usda 1.0\n"
      "(\n"
      "    defaultPrim = \"root\"\n"
      "    upAxis = \"Y\"\n"
      ")\n"
      "\n"
      "def Xform \"root\"\n"
      "{\n"
      "    def Material \"mat\"\n"
      "    {\n"
      "        token outputs:surface.connect = </root/mat/surface.outputs:surface>\n"
      "        def Shader \"surface\"\n"
      "        {\n"
      "            uniform token info:id = \"UsdPreviewSurface\"\n"
      "            color3f inputs:diffuseColor.connect = </root/mat/tex.outputs:rgb>\n"
      "            token outputs:surface\n"
      "        }\n"
      "        def Shader \"tex\"\n"
      "        {\n"
      "            uniform token info:id = \"UsdUVTexture\"\n"
      "            asset inputs:file = @" + texture_path + "@\n"
      "            float3 outputs:rgb\n"
      "        }\n"
      "    }\n"
      "}\n";
  std::string werr;
  return lightusd::io::WriteWholeFile(
      path, reinterpret_cast<const unsigned char *>(usda.data()), usda.size(),
      &werr);
}

bool WriteTwoTexturedUSDA(const std::string &path, const std::string &tex0,
                          const std::string &tex1) {
  const std::string usda =
      "#usda 1.0\n"
      "(\n"
      "    defaultPrim = \"root\"\n"
      "    upAxis = \"Y\"\n"
      ")\n"
      "\n"
      "def Xform \"root\"\n"
      "{\n"
      "    def Material \"mat\"\n"
      "    {\n"
      "        token outputs:surface.connect = </root/mat/surface.outputs:surface>\n"
      "        def Shader \"surface\"\n"
      "        {\n"
      "            uniform token info:id = \"UsdPreviewSurface\"\n"
      "            color3f inputs:diffuseColor.connect = </root/mat/tex0.outputs:rgb>\n"
      "            token outputs:surface\n"
      "        }\n"
      "        def Shader \"tex0\"\n"
      "        {\n"
      "            uniform token info:id = \"UsdUVTexture\"\n"
      "            asset inputs:file = @" + tex0 + "@\n"
      "            float3 outputs:rgb\n"
      "        }\n"
      "        def Shader \"tex1\"\n"
      "        {\n"
      "            uniform token info:id = \"UsdUVTexture\"\n"
      "            asset inputs:file = @" + tex1 + "@\n"
      "            float3 outputs:rgb\n"
      "        }\n"
      "    }\n"
      "}\n";
  std::string werr;
  return lightusd::io::WriteWholeFile(
      path, reinterpret_cast<const unsigned char *>(usda.data()), usda.size(),
      &werr);
}

bool FindTextureFilePath(const lightusd::Prim &prim, std::string *out) {
  const lightusd::Shader *shd = prim.as<lightusd::Shader>();
  if (shd && shd->info_id == "UsdUVTexture") {
    const lightusd::UsdUVTexture *tex = shd->value.as<lightusd::UsdUVTexture>();
    if (tex) {
      const auto av = tex->file.get_value();
      if (av && av.value().is_scalar()) {
        lightusd::value::AssetPath ap;
        if (av.value().get_scalar(&ap)) {
          *out = ap.GetAssetPath();
          return true;
        }
      }
    }
  }
  for (const auto &child : prim.children()) {
    if (FindTextureFilePath(child, out)) {
      return true;
    }
  }
  return false;
}

bool FindTextureFilePath(const lightusd::Stage &stage, std::string *out) {
  for (const auto &prim : stage.root_prims()) {
    if (FindTextureFilePath(prim, out)) {
      return true;
    }
  }
  return false;
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

}  // namespace

// fpnge/fpng PNG encode -> decode roundtrip preserves pixels.
void usdz_convert_png_roundtrip_test(void) {
  using namespace lightusd;

  Image img = MakeSolidImage(16, 12, 4, 10, 20, 30, 40);
  // Add a varying pixel to avoid trivial all-same encoding.
  img.data[0] = 200;
  img.data[1] = 100;

  image::WriteOption wopt;
  wopt.format = image::WriteImageFormat::PNG;

  auto enc = image::WriteImageToMemory(img, wopt);
  TEST_CHECK(enc.has_value());
  if (!enc) {
    TEST_MSG("encode error: %s", enc.error().c_str());
    return;
  }
  TEST_CHECK(enc.value().size() > 8);

  auto dec = image::LoadImageFromMemory(enc.value().data(), enc.value().size(),
                                        "roundtrip.png");
  TEST_CHECK(dec.has_value());
  if (!dec) {
    TEST_MSG("decode error: %s", dec.error().c_str());
    return;
  }
  const Image &out = dec.value().image;
  TEST_CHECK(out.width == 16);
  TEST_CHECK(out.height == 12);
  TEST_CHECK(out.channels == 4);
  TEST_CHECK(out.data.size() == img.data.size());
  if (out.data.size() == img.data.size()) {
    TEST_CHECK(out.data[0] == 200);
    TEST_CHECK(out.data[1] == 100);
    TEST_CHECK(out.data[4] == 10);  // second pixel R
  }
}

// ResizeImage downsamples and preserves channel count.
void usdz_convert_resize_test(void) {
  using namespace lightusd;

  Image img = MakeSolidImage(8, 8, 4, 50, 60, 70, 255);
  Image out;
  std::string err;
  bool ok = tydra::ResizeImage(img, 4, 4, &out, tydra::ResizeFilter::Linear, &err);
  TEST_CHECK(ok);
  if (!ok) {
    TEST_MSG("resize error: %s", err.c_str());
    return;
  }
  TEST_CHECK(out.width == 4);
  TEST_CHECK(out.height == 4);
  TEST_CHECK(out.channels == 4);
  TEST_CHECK(out.data.size() == 4u * 4u * 4u);
  // Solid color should be preserved after resize.
  if (out.data.size() >= 4) {
    TEST_CHECK(out.data[0] == 50);
    TEST_CHECK(out.data[1] == 60);
    TEST_CHECK(out.data[2] == 70);
  }
}

// PackChannels merges two single-source images into R/G.
void usdz_convert_pack_channels_test(void) {
  using namespace lightusd;

  Image red = MakeSolidImage(4, 4, 1, 111, 0, 0, 0);    // 1ch value 111
  Image grn = MakeSolidImage(4, 4, 1, 222, 0, 0, 0);    // 1ch value 222

  std::vector<Image> inputs = {red, grn};
  tydra::ChannelPackSpec spec;
  spec.out_channels = 3;
  spec.r.input_index = 0;
  spec.r.channel = 0;
  spec.g.input_index = 1;
  spec.g.channel = 0;
  spec.b.input_index = -1;
  spec.b.constant = 7;

  Image packed;
  std::string err;
  bool ok = tydra::PackChannels(inputs, spec, &packed, &err);
  TEST_CHECK(ok);
  if (!ok) {
    TEST_MSG("pack error: %s", err.c_str());
    return;
  }
  TEST_CHECK(packed.width == 4);
  TEST_CHECK(packed.height == 4);
  TEST_CHECK(packed.channels == 3);
  if (packed.data.size() >= 3) {
    TEST_CHECK(packed.data[0] == 111);  // R from red
    TEST_CHECK(packed.data[1] == 222);  // G from green
    TEST_CHECK(packed.data[2] == 7);    // B constant
  }
}

// Empty ORM inputs should produce a scalar fallback texture. Missing maps are
// represented with empty buffers and zero channels by callers.
void usdz_convert_orm_scalar_fallback_test(void) {
  using namespace lightusd;

  std::vector<uint8_t> out;
  size_t out_w = 0;
  size_t out_h = 0;
  bool ok = tydra::BuildOcclusionRoughnessMetallicTexture(
      0.25f, 0.5f, 0.75f,
      {}, 0, 0, 0, 0,
      {}, 0, 0, 0, 0,
      {}, 0, 0, 0, 0,
      out, out_w, out_h);

  TEST_CHECK(ok);
  TEST_CHECK(out_w == 1);
  TEST_CHECK(out_h == 1);
  TEST_CHECK(out.size() == 3);
  if (out.size() == 3) {
    TEST_CHECK(out[0] == 63);
    TEST_CHECK(out[1] == 127);
    TEST_CHECK(out[2] == 191);
  }
}

void usdz_convert_orm_scalar_nonfinite_test(void) {
  using namespace lightusd;

  std::vector<uint8_t> out;
  size_t out_w = 0;
  size_t out_h = 0;
  bool ok = tydra::BuildOcclusionRoughnessMetallicTexture(
      std::numeric_limits<float>::infinity(),
      -1000.0f,
      std::numeric_limits<float>::quiet_NaN(),
      {}, 0, 0, 0, 0,
      {}, 0, 0, 0, 0,
      {}, 0, 0, 0, 0,
      out, out_w, out_h);

  TEST_CHECK(ok);
  TEST_CHECK(out_w == 1);
  TEST_CHECK(out_h == 1);
  TEST_CHECK(out.size() == 3);
  if (out.size() == 3) {
    TEST_CHECK(out[0] == 0);
    TEST_CHECK(out[1] == 0);
    TEST_CHECK(out[2] == 0);
  }
}

void usdz_convert_archive_collision_name_test(void) {
  using namespace lightusd;

  const std::string dir = TempDir();
  const std::string a_dir = lightusd::io::JoinPath(dir, "collision_a");
  const std::string b_dir = lightusd::io::JoinPath(dir, "collision_b");
  lightusd::io::CreateDirectories(a_dir);
  lightusd::io::CreateDirectories(b_dir);
  const std::string tex_a = lightusd::io::JoinPath(a_dir, "same.png");
  const std::string tex_b = lightusd::io::JoinPath(b_dir, "same.png");
  const std::string usda_path = lightusd::io::JoinPath(dir, "scene_collision.usda");
  const std::string usdz_path = lightusd::io::JoinPath(dir, "out_collision.usdz");

  auto write_png = [](const std::string &path, const Image &img) -> bool {
    image::WriteOption wopt;
    wopt.format = image::WriteImageFormat::PNG;
    auto enc = image::WriteImageToMemory(img, wopt);
    if (!enc) return false;
    std::string werr;
    return io::WriteWholeFile(path, enc.value().data(), enc.value().size(),
                              &werr);
  };

  TEST_CHECK(write_png(tex_a, MakeSolidImage(4, 4, 4, 255, 0, 0, 255)));
  TEST_CHECK(write_png(tex_b, MakeSolidImage(4, 4, 4, 0, 255, 0, 255)));
  TEST_CHECK(WriteTwoTexturedUSDA(usda_path, tex_a, tex_b));

  usdz::UsdzConvertOptions opts;
  opts.inputs.push_back(usda_path);
  opts.output = usdz_path;
  opts.flatten = true;

  usdz::UsdzConvertStats stats;
  std::string warn, err;
  bool ok = usdz::Convert(opts, &stats, &warn, &err);
  TEST_CHECK(ok);
  if (!ok) {
    TEST_MSG("convert error: %s", err.c_str());
    return;
  }
  TEST_CHECK(stats.num_textures == 2);

  USDZAsset asset;
  std::string awarn, aerr;
  bool read_ok = ReadUSDZAssetInfoFromFile(usdz_path, &asset, &awarn, &aerr);
  TEST_CHECK(read_ok);
  if (read_ok) {
    TEST_CHECK(asset.asset_map.count("textures/same.png") == 1);
    TEST_CHECK(asset.asset_map.count("textures/same_1.png") == 1);
  }
}

// FitTexturesToBudget shrinks a set of textures to a tight byte budget.
void usdz_convert_fit_budget_test(void) {
  using namespace lightusd;

  std::vector<tydra::FitTextureInput> inputs;
  for (auto dim : {256, 512, 128}) {
    tydra::FitTextureInput fi;
    fi.image = MakeNoisyImage(dim, dim, 3);
    fi.original_bytes = EncodePNG(fi.image);
    fi.ext = "png";
    fi.reencodable = true;
    TEST_CHECK(!fi.original_bytes.empty());
    inputs.push_back(std::move(fi));
  }

  size_t fullTotal = 0;
  for (const auto &fi : inputs) fullTotal += fi.original_bytes.size();
  TEST_CHECK(fullTotal > 0);

  auto sumOut = [](const std::vector<tydra::FitTextureOutput> &outs) {
    size_t s = 0;
    for (const auto &o : outs) s += o.bytes.size();
    return s;
  };

  // --- Size strategy: tight budget should shrink dimensions. ---
  {
    tydra::FitTextureOptions opt;
    opt.target_total_bytes = fullTotal / 4;
    opt.strategy = tydra::FitStrategy::Size;
    opt.min_texture_size = 16;

    std::vector<tydra::FitTextureOutput> outs;
    std::string warn, err;
    bool ok = tydra::FitTexturesToBudget(inputs, opt, &outs, &warn, &err);
    TEST_CHECK(ok);
    if (!ok) { TEST_MSG("fit(size) err: %s", err.c_str()); return; }
    TEST_CHECK(outs.size() == inputs.size());
    const size_t total = sumOut(outs);
    // Shrunk below full size, and ideally within budget.
    TEST_CHECK(total < fullTotal);
    for (const auto &o : outs) TEST_CHECK(o.ext == "png");
    // The largest (512) input must have been downscaled.
    TEST_CHECK(outs[1].width < 512);
  }

  // --- Quality strategy: outputs are JPEG and smaller than full PNG. ---
  {
    tydra::FitTextureOptions opt;
    opt.target_total_bytes = fullTotal / 4;
    opt.strategy = tydra::FitStrategy::Quality;
    opt.min_jpeg_quality = 20;

    std::vector<tydra::FitTextureOutput> outs;
    std::string warn, err;
    bool ok = tydra::FitTexturesToBudget(inputs, opt, &outs, &warn, &err);
    TEST_CHECK(ok);
    if (!ok) { TEST_MSG("fit(quality) err: %s", err.c_str()); return; }
    TEST_CHECK(outs.size() == inputs.size());
    for (const auto &o : outs) TEST_CHECK(o.ext == "jpg");
    TEST_CHECK(sumOut(outs) < fullTotal);
  }
}

// End-to-end: USDA + on-disk PNG -> resized/re-encoded -> USDZ (validated).
void usdz_convert_pipeline_test(void) {
  using namespace lightusd;

  const std::string dir = TempDir();
  const std::string png_path = lightusd::io::JoinPath(dir, "tex.png");
  const std::string usda_path = lightusd::io::JoinPath(dir, "scene.usda");
  const std::string usdz_path = lightusd::io::JoinPath(dir, "out.usdz");

  // 1) Write a 64x64 PNG texture on disk.
  Image tex = MakeSolidImage(64, 64, 4, 200, 150, 100, 255);
  {
    image::WriteOption wopt;
    wopt.format = image::WriteImageFormat::PNG;
    auto enc = image::WriteImageToMemory(tex, wopt);
    TEST_CHECK(enc.has_value());
    if (!enc) return;
    std::string werr;
    bool wok = io::WriteWholeFile(png_path, enc.value().data(),
                                  enc.value().size(), &werr);
    TEST_CHECK(wok);
    if (!wok) {
      TEST_MSG("write png: %s", werr.c_str());
      return;
    }
  }

  // 2) Write a USDA referencing the texture.
  {
    const std::string usda =
        "#usda 1.0\n"
        "(\n"
        "    defaultPrim = \"root\"\n"
        "    upAxis = \"Y\"\n"
        ")\n"
        "\n"
        "def Xform \"root\"\n"
        "{\n"
        "    def Material \"mat\"\n"
        "    {\n"
        "        token outputs:surface.connect = </root/mat/surface.outputs:surface>\n"
        "\n"
        "        def Shader \"surface\"\n"
        "        {\n"
        "            uniform token info:id = \"UsdPreviewSurface\"\n"
        "            color3f inputs:diffuseColor.connect = </root/mat/tex.outputs:rgb>\n"
        "            token outputs:surface\n"
        "        }\n"
        "\n"
        "        def Shader \"tex\"\n"
        "        {\n"
        "            uniform token info:id = \"UsdUVTexture\"\n"
        "            asset inputs:file = @tex.png@\n"
        "            float3 outputs:rgb\n"
        "        }\n"
        "    }\n"
        "}\n";
    std::string werr;
    bool wok = io::WriteWholeFile(
        usda_path, reinterpret_cast<const unsigned char *>(usda.data()),
        usda.size(), &werr);
    TEST_CHECK(wok);
    if (!wok) {
      TEST_MSG("write usda: %s", werr.c_str());
      return;
    }
  }

  // 3) Convert: flatten + resize textures to 32px + force PNG (fpnge).
  usdz::UsdzConvertOptions opts;
  opts.inputs.push_back(usda_path);
  opts.output = usdz_path;
  opts.flatten = true;
  opts.arkit_compatible = true;
  opts.max_texture_size = 32;
  opts.texture_format = usdz::OutputTextureFormat::PNG;

  usdz::UsdzConvertStats stats;
  std::string warn, err;
  bool ok = usdz::Convert(opts, &stats, &warn, &err);
  TEST_CHECK(ok);
  if (!ok) {
    TEST_MSG("convert error: %s", err.c_str());
    return;
  }

  TEST_CHECK(stats.num_textures >= 1);
  TEST_CHECK(stats.num_textures_resized >= 1);

  // 4) Read back + validate the USDZ.
  std::vector<uint8_t> usdz_bytes;
  std::string ioerr;
  bool rok = io::ReadWholeFile(&usdz_bytes, &ioerr, usdz_path, 0);
  TEST_CHECK(rok);
  if (!rok) {
    TEST_MSG("read usdz: %s", ioerr.c_str());
    return;
  }
  std::string vwarn, verr;
  bool valid = lightusd::ValidateUSDZ(usdz_bytes.data(), usdz_bytes.size(),
                                      &vwarn, &verr);
  TEST_CHECK(valid);
  if (!valid) {
    TEST_MSG("validate: %s", verr.c_str());
  }
}

void usdz_convert_flat_output_size_stats_test(void) {
  using namespace lightusd;

  const std::string dir = TempDir();
  const std::string usda_path = lightusd::io::JoinPath(dir, "flat_stats.usda");
  TEST_CHECK(WriteMinimalUSDA(usda_path));

  const struct {
    usdz::OutputFormat format;
    const char *filename;
  } cases[] = {
      {usdz::OutputFormat::USDC, "flat_stats.usdc"},
      {usdz::OutputFormat::USDA, "flat_stats_out.usda"},
  };

  for (const auto &tc : cases) {
    usdz::UsdzConvertOptions opts;
    opts.inputs.push_back(usda_path);
    opts.output = lightusd::io::JoinPath(dir, tc.filename);
    opts.output_format = tc.format;
    opts.flatten = true;

    usdz::UsdzConvertStats stats;
    std::string warn, err;
    bool ok = usdz::Convert(opts, &stats, &warn, &err);
    TEST_CHECK(ok);
    if (!ok) {
      TEST_MSG("convert error: %s", err.c_str());
      continue;
    }

    const size_t actual_size = TestFileSize(opts.output);
    TEST_CHECK(actual_size > 0);
    TEST_CHECK(stats.output_size == actual_size);
  }
}

void usdz_convert_usdz_root_layer_format_test(void) {
  using namespace lightusd;

  const std::string dir = TempDir();
  const std::string png_path = lightusd::io::JoinPath(dir, "root_format_tex.png");
  const std::string usda_path =
      lightusd::io::JoinPath(dir, "root_format_scene.usda");
  const std::string usdz_path =
      lightusd::io::JoinPath(dir, "root_format_out.usdz");

  {
    Image tex = MakeSolidImage(4, 4, 4, 20, 40, 60, 255);
    image::WriteOption wopt;
    wopt.format = image::WriteImageFormat::PNG;
    auto enc = image::WriteImageToMemory(tex, wopt);
    TEST_CHECK(enc.has_value());
    if (!enc) return;
    std::string werr;
    TEST_CHECK(io::WriteWholeFile(png_path, enc.value().data(),
                                  enc.value().size(), &werr));
  }

  TEST_CHECK(WriteTexturedUSDA(usda_path, "root_format_tex.png"));

  usdz::UsdzConvertOptions opts;
  opts.inputs.push_back(usda_path);
  opts.output = usdz_path;
  opts.flatten = true;
  opts.usdz_root_layer_format = usdz::USDZRootLayerFormat::USDA;

  usdz::UsdzConvertStats stats;
  std::string warn, err;
  bool ok = usdz::Convert(opts, &stats, &warn, &err);
  TEST_CHECK(ok);
  if (!ok) {
    TEST_MSG("convert error: %s", err.c_str());
    return;
  }
  TEST_CHECK(stats.num_textures == 1);

  std::vector<uint8_t> usdz_bytes;
  std::string ioerr;
  bool rok = io::ReadWholeFile(&usdz_bytes, &ioerr, usdz_path, 0);
  TEST_CHECK(rok);
  if (!rok) {
    TEST_MSG("read usdz: %s", ioerr.c_str());
    return;
  }

  TEST_CHECK(FirstUSDZEntryName(usdz_bytes) == "root.usda");
  std::string vwarn, verr;
  TEST_CHECK(ValidateUSDZ(usdz_bytes.data(), usdz_bytes.size(), &vwarn,
                          &verr));

  Stage loaded_stage;
  std::string lwarn, lerr;
  TEST_CHECK(LoadUSDFromFile(usdz_path, &loaded_stage, &lwarn, &lerr));
}

void usdz_convert_arkit_forces_flattened_usdc_root_test(void) {
  using namespace lightusd;

  const std::string dir = TempDir();
  const std::string png_path = lightusd::io::JoinPath(dir, "arkit_tex.png");
  const std::string usda_path =
      lightusd::io::JoinPath(dir, "arkit_scene.usda");
  const std::string usdz_path =
      lightusd::io::JoinPath(dir, "arkit_out.usdz");

  {
    Image tex = MakeSolidImage(4, 4, 4, 80, 90, 100, 255);
    image::WriteOption wopt;
    wopt.format = image::WriteImageFormat::PNG;
    auto enc = image::WriteImageToMemory(tex, wopt);
    TEST_CHECK(enc.has_value());
    if (!enc) return;
    std::string werr;
    TEST_CHECK(io::WriteWholeFile(png_path, enc.value().data(),
                                  enc.value().size(), &werr));
  }

  TEST_CHECK(WriteTexturedUSDA(usda_path, "arkit_tex.png"));

  usdz::UsdzConvertOptions opts;
  opts.inputs.push_back(usda_path);
  opts.output = usdz_path;
  opts.flatten = false;
  opts.arkit_compatible = true;
  opts.usdz_root_layer_format = usdz::USDZRootLayerFormat::USDA;

  usdz::UsdzConvertStats stats;
  std::string warn, err;
  bool ok = usdz::Convert(opts, &stats, &warn, &err);
  TEST_CHECK(ok);
  if (!ok) {
    TEST_MSG("convert error: %s", err.c_str());
    return;
  }
  TEST_CHECK(warn.find("requires a flattened package") != std::string::npos);
  TEST_CHECK(warn.find("requires a USDC root layer") != std::string::npos);

  std::vector<uint8_t> usdz_bytes;
  std::string ioerr;
  bool rok = io::ReadWholeFile(&usdz_bytes, &ioerr, usdz_path, 0);
  TEST_CHECK(rok);
  if (!rok) {
    TEST_MSG("read usdz: %s", ioerr.c_str());
    return;
  }

  TEST_CHECK(FirstUSDZEntryName(usdz_bytes) == "root.usdc");
  std::string vwarn, verr;
  TEST_CHECK(ValidateUSDZ(usdz_bytes.data(), usdz_bytes.size(), &vwarn,
                          &verr));
}

// Standalone RepackTextureFiles: two grayscale PNGs -> packed RG image.
void usdz_convert_repack_files_test(void) {
  using namespace lightusd;

  const std::string dir = TempDir();
  const std::string a_path = lightusd::io::JoinPath(dir, "gloss.png");
  const std::string b_path = lightusd::io::JoinPath(dir, "rough.png");
  const std::string out_path = lightusd::io::JoinPath(dir, "packed.png");

  auto write_png = [](const std::string &path, const Image &img) -> bool {
    image::WriteOption wopt;
    wopt.format = image::WriteImageFormat::PNG;
    auto enc = image::WriteImageToMemory(img, wopt);
    if (!enc) return false;
    std::string werr;
    return io::WriteWholeFile(path, enc.value().data(), enc.value().size(),
                              &werr);
  };

  TEST_CHECK(write_png(a_path, MakeSolidImage(8, 8, 1, 90, 0, 0, 0)));
  TEST_CHECK(write_png(b_path, MakeSolidImage(8, 8, 1, 180, 0, 0, 0)));

  usdz::RepackSpec spec;
  spec.out_channels = 3;
  spec.r.input_file = a_path;
  spec.r.channel = 0;
  spec.g.input_file = b_path;
  spec.g.channel = 0;
  spec.b.input_file.clear();
  spec.b.constant = 0;

  std::string warn, err;
  bool ok = usdz::RepackTextureFiles(spec, out_path, image::PngEncoder::Auto,
                                     &warn, &err);
  TEST_CHECK(ok);
  if (!ok) {
    TEST_MSG("repack error: %s", err.c_str());
    return;
  }

  // Decode the packed output and verify channel values.
  auto dec = image::LoadImageFromFile(out_path);
  TEST_CHECK(dec.has_value());
  if (!dec) return;
  const Image &p = dec.value().image;
  // The decoder may expand a 3-channel PNG to RGBA; the first pixel's R/G/B
  // are what matter for correctness.
  TEST_CHECK(p.channels >= 3);
  if (p.data.size() >= 3) {
    TEST_CHECK(p.data[0] == 90);   // R from gloss
    TEST_CHECK(p.data[1] == 180);  // G from roughness
    TEST_CHECK(p.data[2] == 0);    // B constant
  }
}

// JPEG encode -> decode roundtrip preserves approximate pixel values.
void usdz_convert_jpeg_roundtrip_test(void) {
  using namespace lightusd;

  // Use a solid-color image (JPEG is lossy, so avoid exact pixel comparisons
  // for high-frequency content).
  Image img = MakeSolidImage(16, 12, 3, 100, 150, 200, 255);

  image::WriteOption wopt;
  wopt.format = image::WriteImageFormat::JPEG;
  wopt.jpeg_quality = 95;

  auto enc = image::WriteImageToMemory(img, wopt);
  TEST_CHECK(enc.has_value());
  if (!enc) {
    TEST_MSG("jpeg encode error: %s", enc.error().c_str());
    return;
  }
  TEST_CHECK(enc.value().size() > 8);

  auto dec = image::LoadImageFromMemory(enc.value().data(), enc.value().size(),
                                        "roundtrip.jpg");
  TEST_CHECK(dec.has_value());
  if (!dec) {
    TEST_MSG("jpeg decode error: %s", dec.error().c_str());
    return;
  }
  const Image &out = dec.value().image;
  TEST_CHECK(out.width == 16);
  TEST_CHECK(out.height == 12);
  TEST_CHECK(out.channels >= 3);
  // Solid color should survive JPEG roundtrip at high quality (within
  // tolerance for lossy codec).
  if (out.data.size() >= 3) {
    // Allow +-10 for JPEG quantization on a solid color at q95.
    int dr = std::abs(int(out.data[0]) - 100);
    int dg = std::abs(int(out.data[1]) - 150);
    int db = std::abs(int(out.data[2]) - 200);
    TEST_CHECK(dr <= 10);
    TEST_CHECK(dg <= 10);
    TEST_CHECK(db <= 10);
  }
}

// RemapTextureAssetPaths handles an empty stage gracefully (returns 0).
void usdz_convert_remap_asset_paths_test(void) {
  using namespace lightusd;

  Stage stage;
  std::map<std::string, std::string> remap;
  remap["old_tex.png"] = "new_tex.jpg";

  // An empty stage has no textures, so count should be 0.
  size_t count = usdz::RemapTextureAssetPaths(stage, remap);
  TEST_CHECK(count == 0);
}

void usdz_convert_regression_texture_remap_test(void) {
  using namespace lightusd;

  const std::string dir = lightusd::io::JoinPath(TempDir(), "regression_remap");
  lightusd::io::CreateDirectories(dir);
  const std::string src_fixture = TestFixturePath(
      "tests/unit/fixtures/usdzconvert-regression-textured-two-materials.usda");
  const std::string usda_path = lightusd::io::JoinPath(dir, "scene.usda");
  const std::string png_path = lightusd::io::JoinPath(dir, "Texture.png");
  const std::string usdz_path = lightusd::io::JoinPath(dir, "out.usdz");

  TEST_CHECK(CopyFile(src_fixture, usda_path));
  TEST_CHECK(WritePNG(png_path, MakeSolidImage(8, 8, 3, 90, 120, 180, 255)));

  usdz::UsdzConvertOptions opts;
  opts.inputs.push_back(usda_path);
  opts.output = usdz_path;
  opts.flatten = true;
  opts.texture_format = usdz::OutputTextureFormat::JPEG;
  opts.jpeg_quality = 85;

  usdz::UsdzConvertStats stats;
  std::string warn, err;
  bool ok = usdz::Convert(opts, &stats, &warn, &err);
  TEST_CHECK(ok);
  if (!ok) {
    TEST_MSG("convert error: %s", err.c_str());
    return;
  }
  TEST_CHECK(stats.num_textures == 1);
  TEST_CHECK(stats.num_textures_reencoded == 1);

  USDZAsset asset;
  std::string awarn, aerr;
  bool asset_ok = ReadUSDZAssetInfoFromFile(usdz_path, &asset, &awarn, &aerr);
  TEST_CHECK(asset_ok);
  if (asset_ok) {
    size_t jpg_count = 0;
    size_t png_count = 0;
    for (const auto &kv : asset.asset_map) {
      if (kv.first.find("Texture.jpg") != std::string::npos) {
        jpg_count++;
      }
      if (kv.first.find("Texture.png") != std::string::npos) {
        png_count++;
      }
    }
    TEST_CHECK(jpg_count == 1);
    TEST_CHECK(png_count == 0);
  }

  Stage loaded_stage;
  std::string lwarn, lerr;
  bool loaded = LoadUSDFromFile(usdz_path, &loaded_stage, &lwarn, &lerr);
  TEST_CHECK(loaded);
  if (!loaded) {
    TEST_MSG("LoadUSDFromFile failed: %s", lerr.c_str());
    return;
  }

  std::string texture_path;
  TEST_CHECK(FindTextureFilePath(loaded_stage, &texture_path));
  TEST_CHECK(texture_path.find("Texture.jpg") != std::string::npos);
  TEST_CHECK(texture_path.find("Texture.png") == std::string::npos);
}

void usdz_convert_regression_material_preview_dedupe_test(void) {
  using namespace lightusd;

  Layer layer;
  std::string warn, err;
  bool loaded = LoadLayerFromFile(
      TestFixturePath(
          "tests/unit/fixtures/usdzconvert-regression-textured-two-materials.usda"),
      &layer, &warn, &err);
  TEST_CHECK(loaded);
  if (!loaded) {
    TEST_MSG("LoadLayerFromFile failed: %s", err.c_str());
    return;
  }

  usdz::UsdzConvertOptions opts;
  opts.material_optimization = usdz::MaterialOptimizationMode::Preview;
  usdz::MaterialOptimizationStats stats;
  bool ok =
      usdz::OptimizeMaterialsInLayer(opts, &layer, &stats, &warn, &err);
  TEST_CHECK(ok);
  TEST_CHECK(stats.num_materials_before == 2);
  TEST_CHECK(stats.num_materials_after == 1);
  TEST_CHECK(stats.num_materials_deduped == 1);
  TEST_CHECK(stats.num_materials_preview_converted == 2);

  const PrimSpec *mesh_b = nullptr;
  ok = layer.find_primspec_at(Path("/Root/MeshB", ""), &mesh_b, &err);
  TEST_CHECK(ok);
  TEST_CHECK(mesh_b != nullptr);
  if (mesh_b) {
    auto binding_it = mesh_b->props().find("material:binding");
    TEST_CHECK(binding_it != mesh_b->props().end());
    if (binding_it != mesh_b->props().end()) {
      auto target = binding_it->second.get_relationTarget();
      TEST_CHECK(target.has_value());
      if (target) {
        TEST_CHECK(target->prim_part() == "/Root/MatA");
      }
    }
  }
}

void usdz_convert_regression_invalid_index_skip_test(void) {
  using namespace lightusd;

  Layer layer;
  std::string warn, err;
  bool loaded = LoadLayerFromFile(
      TestFixturePath(
          "tests/unit/fixtures/usdzconvert-regression-invalid-indices.usda"),
      &layer, &warn, &err);
  TEST_CHECK(loaded);
  if (!loaded) {
    TEST_MSG("LoadLayerFromFile failed: %s", err.c_str());
    return;
  }

  usdz::UsdzConvertOptions opts;
  opts.geometry_optimization = usdz::GeometryOptimizationMode::MergeMeshes;
  usdz::GeometryOptimizationStats stats;
  bool ok =
      usdz::OptimizeGeometryInLayer(opts, &layer, &stats, &warn, &err);
  TEST_CHECK(ok);
  TEST_CHECK(stats.num_meshes_before == 1);
  TEST_CHECK(stats.num_meshes_eligible == 0);
  TEST_CHECK(stats.num_meshes_skipped == 1);
  TEST_CHECK(stats.num_mesh_aggregates == 0);
  TEST_CHECK(stats.num_meshes_after == 1);
}

void usdz_convert_material_dedupe_test(void) {
  using namespace lightusd;

  Layer layer;
  std::string warn, err;
  bool loaded = LoadLayerFromFile(
      TestFixturePath("tests/usda/material-optimize-dedupe-001.usda"), &layer,
      &warn, &err);
  TEST_CHECK(loaded);
  if (!loaded) {
    TEST_MSG("LoadLayerFromFile failed: %s", err.c_str());
    return;
  }

  usdz::UsdzConvertOptions opts;
  opts.material_optimization = usdz::MaterialOptimizationMode::Dedupe;
  usdz::MaterialOptimizationStats stats;
  bool ok =
      usdz::OptimizeMaterialsInLayer(opts, &layer, &stats, &warn, &err);
  TEST_CHECK(ok);
  TEST_CHECK(stats.num_materials_before == 2);
  TEST_CHECK(stats.num_materials_after == 1);
  TEST_CHECK(stats.num_materials_deduped == 1);

  const PrimSpec *mesh_b = nullptr;
  ok = layer.find_primspec_at(Path("/Root/MeshB", ""), &mesh_b, &err);
  TEST_CHECK(ok);
  TEST_CHECK(mesh_b != nullptr);
  if (mesh_b) {
    TEST_CHECK(mesh_b->children().empty());
    auto it = mesh_b->props().find("material:binding");
    TEST_CHECK(it != mesh_b->props().end());
    if (it != mesh_b->props().end()) {
      auto target = it->second.get_relationTarget();
      TEST_CHECK(target.has_value());
      if (target) {
        TEST_CHECK(target->prim_part() == "/Root/MeshA/Mat");
      }
    }
  }
}

void usdz_convert_material_preview_atlas_fallback_test(void) {
  using namespace lightusd;

  {
    Layer layer;
    std::string warn, err;
    bool loaded = LoadLayerFromFile(
        TestFixturePath("tests/usda/material-optimize-preview-001.usda"),
        &layer, &warn, &err);
    TEST_CHECK(loaded);
    if (!loaded) {
      TEST_MSG("LoadLayerFromFile failed: %s", err.c_str());
      return;
    }

    usdz::UsdzConvertOptions opts;
    opts.material_optimization = usdz::MaterialOptimizationMode::Preview;
    usdz::MaterialOptimizationStats stats;
    bool ok =
        usdz::OptimizeMaterialsInLayer(opts, &layer, &stats, &warn, &err);
    TEST_CHECK(ok);
    TEST_CHECK(stats.num_materials_before == 2);
    TEST_CHECK(stats.num_materials_after == 1);
    TEST_CHECK(stats.num_materials_deduped == 1);
    TEST_CHECK(stats.num_materials_preview_converted == 2);
    TEST_CHECK(warn.find("preview mode") == std::string::npos);
  }

  for (usdz::MaterialOptimizationMode mode :
       {usdz::MaterialOptimizationMode::Preview,
        usdz::MaterialOptimizationMode::Atlas}) {
    Layer layer;
    std::string warn, err;
    bool loaded = LoadLayerFromFile(
        TestFixturePath("tests/usda/material-optimize-dedupe-001.usda"),
        &layer, &warn, &err);
    TEST_CHECK(loaded);
    if (!loaded) {
      TEST_MSG("LoadLayerFromFile failed: %s", err.c_str());
      continue;
    }

    usdz::UsdzConvertOptions opts;
    opts.material_optimization = mode;
    usdz::MaterialOptimizationStats stats;
    bool ok =
        usdz::OptimizeMaterialsInLayer(opts, &layer, &stats, &warn, &err);
    TEST_CHECK(ok);
    TEST_CHECK(stats.num_materials_before == 2);
    TEST_CHECK(stats.num_materials_after == 1);
    TEST_CHECK(stats.num_materials_deduped == 1);
    if (mode == usdz::MaterialOptimizationMode::Atlas) {
      TEST_CHECK(warn.find("atlas mode") != std::string::npos);
    } else {
      TEST_CHECK(warn.find("preview mode") == std::string::npos);
    }

    Layer unique_layer;
    warn.clear();
    err.clear();
    loaded = LoadLayerFromFile(
        TestFixturePath("tests/usda/material-optimize-unique-001.usda"),
        &unique_layer, &warn, &err);
    TEST_CHECK(loaded);
    if (!loaded) {
      TEST_MSG("LoadLayerFromFile failed: %s", err.c_str());
      continue;
    }

    stats = usdz::MaterialOptimizationStats{};
    ok = usdz::OptimizeMaterialsInLayer(opts, &unique_layer, &stats, &warn,
                                        &err);
    TEST_CHECK(ok);
    TEST_CHECK(stats.num_materials_before == 2);
    TEST_CHECK(stats.num_materials_after == 2);
    TEST_CHECK(stats.num_materials_deduped == 0);
    if (mode == usdz::MaterialOptimizationMode::Atlas) {
      TEST_CHECK(warn.find("atlas mode") != std::string::npos);
    } else {
      TEST_CHECK(warn.find("preview mode") == std::string::npos);
    }
  }
}

void usdz_convert_material_preview_transitive_key_test(void) {
  using namespace lightusd;

  Layer layer;
  std::string warn, err;
  bool loaded = LoadLayerFromFile(
      TestFixturePath("tests/usda/material-optimize-preview-transitive-001.usda"),
      &layer, &warn, &err);
  TEST_CHECK(loaded);
  if (!loaded) {
    TEST_MSG("LoadLayerFromFile failed: %s", err.c_str());
    return;
  }

  usdz::UsdzConvertOptions opts;
  opts.material_optimization = usdz::MaterialOptimizationMode::Preview;
  usdz::MaterialOptimizationStats stats;
  bool ok = usdz::OptimizeMaterialsInLayer(opts, &layer, &stats, &warn,
                                           &err);
  TEST_CHECK(ok);
  TEST_CHECK(stats.num_materials_before == 2);
  TEST_CHECK(stats.num_materials_after == 2);
  TEST_CHECK(stats.num_materials_deduped == 0);

  const PrimSpec *mesh_b = nullptr;
  ok = layer.find_primspec_at(Path("/Root/MeshB", ""), &mesh_b, &err);
  TEST_CHECK(ok);
  TEST_CHECK(mesh_b != nullptr);
  if (mesh_b) {
    auto binding_it = mesh_b->props().find("material:binding");
    TEST_CHECK(binding_it != mesh_b->props().end());
    if (binding_it != mesh_b->props().end()) {
      auto target = binding_it->second.get_relationTarget();
      TEST_CHECK(target.has_value());
      if (target) {
        TEST_CHECK(target->prim_part() == "/Root/MatB");
      }
    }
  }
}

void usdz_convert_geometry_merge_test(void) {
  using namespace lightusd;

  Layer layer;
  std::string warn, err;
  bool loaded = LoadLayerFromFile(
      TestFixturePath("tests/usda/geometry-optimize-merge-001.usda"), &layer,
      &warn, &err);
  TEST_CHECK(loaded);
  if (!loaded) {
    TEST_MSG("LoadLayerFromFile failed: %s", err.c_str());
    return;
  }

  usdz::UsdzConvertOptions opts;
  opts.geometry_optimization = usdz::GeometryOptimizationMode::MergeMeshes;
  usdz::GeometryOptimizationStats stats;
  bool ok =
      usdz::OptimizeGeometryInLayer(opts, &layer, &stats, &warn, &err);
  TEST_CHECK(ok);
  TEST_CHECK(stats.num_meshes_before == 2);
  TEST_CHECK(stats.num_meshes_eligible == 2);
  TEST_CHECK(stats.num_meshes_merged == 2);
  TEST_CHECK(stats.num_mesh_aggregates == 1);
  TEST_CHECK(stats.num_meshes_after == 1);
  TEST_CHECK(stats.num_faces_merged == 2);
  TEST_CHECK(stats.num_points_merged == 6);

  const PrimSpec *old_mesh = nullptr;
  ok = layer.find_primspec_at(Path("/Root/A/Mesh", ""), &old_mesh, &err);
  TEST_CHECK(ok);
  TEST_CHECK(old_mesh != nullptr);
  if (old_mesh) {
    TEST_CHECK(old_mesh->metas().has_active());
    TEST_CHECK(!old_mesh->metas().get_active());
    TEST_CHECK(old_mesh->props().find("faceVertexCounts") ==
               old_mesh->props().end());
  }

  const PrimSpec *merged = nullptr;
  ok = layer.find_primspec_at(
      Path("/__LightUSD_MeshMerge/Merged_0", ""), &merged, &err);
  TEST_CHECK(ok);
  TEST_CHECK(merged != nullptr);
  if (!merged) {
    return;
  }

  auto points_it = merged->props().find("points");
  TEST_CHECK(points_it != merged->props().end());
  if (points_it != merged->props().end()) {
    auto points =
        points_it->second.get_attribute().get_value<std::vector<value::point3f>>();
    TEST_CHECK(points.has_value());
    if (points && points->size() == 6) {
      TEST_CHECK((*points)[0].x == 1.0f);
      TEST_CHECK((*points)[0].y == 0.0f);
      TEST_CHECK((*points)[3].x == 3.0f);
      TEST_CHECK((*points)[3].y == 0.0f);
    } else {
      TEST_CHECK(false);
    }
  }

  auto binding_it = merged->props().find("material:binding");
  TEST_CHECK(binding_it != merged->props().end());
  if (binding_it != merged->props().end()) {
    auto target = binding_it->second.get_relationTarget();
    TEST_CHECK(target.has_value());
    if (target) {
      TEST_CHECK(target->prim_part() == "/Root/Mat");
    }
  }

  auto normals_it = merged->props().find("normals");
  TEST_CHECK(normals_it != merged->props().end());
  if (normals_it != merged->props().end()) {
    const Attribute &attr = normals_it->second.get_attribute();
    TEST_CHECK(attr.metas().has_interpolation());
    TEST_CHECK(attr.metas().get_interpolation_enum() ==
               Interpolation::FaceVarying);
    auto normals = attr.get_value<std::vector<value::normal3f>>();
    TEST_CHECK(normals.has_value());
    if (normals && normals->size() == 6) {
      TEST_CHECK((*normals)[0].z == 1.0f);
      TEST_CHECK((*normals)[1].y == 1.0f);
      TEST_CHECK((*normals)[2].x == 1.0f);
    } else {
      TEST_CHECK(false);
    }
  }

  auto st_it = merged->props().find("primvars:st");
  TEST_CHECK(st_it != merged->props().end());
  if (st_it != merged->props().end()) {
    const Attribute &attr = st_it->second.get_attribute();
    TEST_CHECK(attr.metas().has_interpolation());
    TEST_CHECK(attr.metas().get_interpolation_enum() ==
               Interpolation::FaceVarying);
    auto st = attr.get_value<std::vector<value::texcoord2f>>();
    TEST_CHECK(st.has_value());
    if (st && st->size() == 6) {
      TEST_CHECK((*st)[0].s == 0.0f);
      TEST_CHECK((*st)[0].t == 0.0f);
      TEST_CHECK((*st)[1].s == 1.0f);
      TEST_CHECK((*st)[2].t == 1.0f);
    } else {
      TEST_CHECK(false);
    }
    TEST_CHECK(merged->props().find("primvars:st:indices") ==
               merged->props().end());
  }
}

void usdz_convert_geometry_subset_merge_test(void) {
  using namespace lightusd;

  Layer layer;
  std::string warn, err;
  bool loaded = LoadLayerFromFile(
      TestFixturePath("tests/usda/geometry-optimize-subset-skip-001.usda"),
      &layer, &warn, &err);
  TEST_CHECK(loaded);
  if (!loaded) {
    TEST_MSG("LoadLayerFromFile failed: %s", err.c_str());
    return;
  }

  usdz::UsdzConvertOptions opts;
  opts.geometry_optimization = usdz::GeometryOptimizationMode::MergeMeshes;
  usdz::GeometryOptimizationStats stats;
  bool ok =
      usdz::OptimizeGeometryInLayer(opts, &layer, &stats, &warn, &err);
  TEST_CHECK(ok);
  TEST_CHECK(stats.num_meshes_before == 2);
  TEST_CHECK(stats.num_meshes_eligible == 2);
  TEST_CHECK(stats.num_meshes_skipped == 0);
  TEST_CHECK(stats.num_meshes_merged == 2);
  TEST_CHECK(stats.num_mesh_aggregates == 1);
  TEST_CHECK(stats.num_meshes_after == 1);
}

void usdz_convert_geometry_subset_partition_test(void) {
  using namespace lightusd;

  Layer layer;
  std::string warn, err;
  bool loaded = LoadLayerFromFile(
      TestFixturePath("tests/usda/geometry-optimize-subset-partial-001.usda"),
      &layer, &warn, &err);
  TEST_CHECK(loaded);
  if (!loaded) {
    TEST_MSG("LoadLayerFromFile failed: %s", err.c_str());
    return;
  }

  usdz::UsdzConvertOptions opts;
  opts.geometry_optimization = usdz::GeometryOptimizationMode::MergeMeshes;
  usdz::GeometryOptimizationStats stats;
  bool ok =
      usdz::OptimizeGeometryInLayer(opts, &layer, &stats, &warn, &err);
  TEST_CHECK(ok);
  TEST_CHECK(stats.num_meshes_before == 2);
  TEST_CHECK(stats.num_meshes_eligible == 2);
  TEST_CHECK(stats.num_meshes_merged == 2);
  TEST_CHECK(stats.num_mesh_aggregates == 2);
  TEST_CHECK(stats.num_faces_merged == 4);
  TEST_CHECK(stats.num_meshes_after == 2);
  TEST_CHECK(CountAggregateMeshes(layer) == 2);
  TEST_CHECK(CountAggregateFaces(layer) == 4);
}

void usdz_convert_geometry_subset_overlap_skip_test(void) {
  using namespace lightusd;

  Layer layer;
  std::string warn, err;
  bool loaded = LoadLayerFromFile(
      TestFixturePath("tests/usda/geometry-optimize-subset-overlap-001.usda"),
      &layer, &warn, &err);
  TEST_CHECK(loaded);
  if (!loaded) {
    TEST_MSG("LoadLayerFromFile failed: %s", err.c_str());
    return;
  }

  usdz::UsdzConvertOptions opts;
  opts.geometry_optimization = usdz::GeometryOptimizationMode::MergeMeshes;
  usdz::GeometryOptimizationStats stats;
  bool ok =
      usdz::OptimizeGeometryInLayer(opts, &layer, &stats, &warn, &err);
  TEST_CHECK(ok);
  TEST_CHECK(stats.num_meshes_before == 2);
  TEST_CHECK(stats.num_meshes_eligible == 0);
  TEST_CHECK(stats.num_meshes_skipped == 2);
  TEST_CHECK(stats.num_mesh_aggregates == 0);
  TEST_CHECK(stats.num_meshes_after == 2);
  TEST_CHECK(CountAggregateMeshes(layer) == 0);
}

void usdz_convert_geometry_descendant_material_skip_test(void) {
  using namespace lightusd;

  Layer layer;
  std::string warn, err;
  bool loaded = LoadLayerFromFile(
      TestFixturePath("tests/usda/geometry-optimize-descendant-material-001.usda"),
      &layer, &warn, &err);
  TEST_CHECK(loaded);
  if (!loaded) {
    TEST_MSG("LoadLayerFromFile failed: %s", err.c_str());
    return;
  }

  usdz::UsdzConvertOptions opts;
  opts.geometry_optimization = usdz::GeometryOptimizationMode::MergeMeshes;
  usdz::GeometryOptimizationStats stats;
  bool ok =
      usdz::OptimizeGeometryInLayer(opts, &layer, &stats, &warn, &err);
  TEST_CHECK(ok);
  TEST_CHECK(stats.num_meshes_before == 2);
  TEST_CHECK(stats.num_meshes_eligible == 0);
  TEST_CHECK(stats.num_meshes_skipped == 2);
  TEST_CHECK(stats.num_mesh_aggregates == 0);
  TEST_CHECK(stats.num_meshes_after == 2);
}

void usdz_convert_geometry_authored_semantics_skip_test(void) {
  using namespace lightusd;

  Layer layer;
  std::string warn, err;
  bool loaded = LoadLayerFromFile(
      TestFixturePath("tests/usda/geometry-optimize-authored-semantics-001.usda"),
      &layer, &warn, &err);
  TEST_CHECK(loaded);
  if (!loaded) {
    TEST_MSG("LoadLayerFromFile failed: %s", err.c_str());
    return;
  }

  usdz::UsdzConvertOptions opts;
  opts.geometry_optimization = usdz::GeometryOptimizationMode::MergeMeshes;
  usdz::GeometryOptimizationStats stats;
  bool ok =
      usdz::OptimizeGeometryInLayer(opts, &layer, &stats, &warn, &err);
  TEST_CHECK(ok);
  TEST_CHECK(stats.num_meshes_before == 6);
  TEST_CHECK(stats.num_meshes_eligible == 0);
  TEST_CHECK(stats.num_meshes_skipped == 6);
  TEST_CHECK(stats.num_mesh_aggregates == 0);
  TEST_CHECK(stats.num_meshes_after == 6);
}

void usdz_convert_geometry_normal_inverse_transpose_test(void) {
  using namespace lightusd;

  Layer layer;
  std::string warn, err;
  bool loaded = LoadLayerFromFile(
      TestFixturePath("tests/usda/geometry-optimize-nonuniform-normal-001.usda"),
      &layer, &warn, &err);
  TEST_CHECK(loaded);
  if (!loaded) {
    TEST_MSG("LoadLayerFromFile failed: %s", err.c_str());
    return;
  }

  usdz::UsdzConvertOptions opts;
  opts.geometry_optimization = usdz::GeometryOptimizationMode::MergeMeshes;
  usdz::GeometryOptimizationStats stats;
  bool ok =
      usdz::OptimizeGeometryInLayer(opts, &layer, &stats, &warn, &err);
  TEST_CHECK(ok);
  TEST_CHECK(stats.num_meshes_merged == 2);

  const PrimSpec *merged = nullptr;
  ok = layer.find_primspec_at(
      Path("/__LightUSD_MeshMerge/Merged_0", ""), &merged, &err);
  TEST_CHECK(ok);
  TEST_CHECK(merged != nullptr);
  if (!merged) {
    return;
  }
  auto normals_it = merged->props().find("normals");
  TEST_CHECK(normals_it != merged->props().end());
  if (normals_it != merged->props().end()) {
    auto normals =
        normals_it->second.get_attribute().get_value<std::vector<value::normal3f>>();
    TEST_CHECK(normals.has_value());
    if (normals && !normals->empty()) {
      TEST_CHECK(std::fabs((*normals)[0].x - 0.4472136f) < 1.0e-5f);
      TEST_CHECK(std::fabs((*normals)[0].y - 0.8944272f) < 1.0e-5f);
      TEST_CHECK(std::fabs((*normals)[0].z) < 1.0e-5f);
    }
  }
}

void usdz_convert_geometry_multi_binding_skip_test(void) {
  using namespace lightusd;

  Layer layer;
  std::string warn, err;
  bool loaded = LoadLayerFromFile(
      TestFixturePath(
          "tests/unit/fixtures/geometry-optimize-multi-binding-skip-001.usda"),
      &layer, &warn, &err);
  TEST_CHECK(loaded);
  if (!loaded) {
    TEST_MSG("LoadLayerFromFile failed: %s", err.c_str());
    return;
  }

  usdz::UsdzConvertOptions opts;
  opts.geometry_optimization = usdz::GeometryOptimizationMode::MergeMeshes;
  usdz::GeometryOptimizationStats stats;
  bool ok =
      usdz::OptimizeGeometryInLayer(opts, &layer, &stats, &warn, &err);
  TEST_CHECK(ok);
  TEST_CHECK(stats.num_meshes_before == 2);
  TEST_CHECK(stats.num_meshes_eligible == 0);
  TEST_CHECK(stats.num_meshes_skipped == 2);
  TEST_CHECK(stats.num_mesh_aggregates == 0);
  TEST_CHECK(stats.num_meshes_after == 2);
  TEST_CHECK(CountAggregateMeshes(layer) == 0);
}

void usdz_convert_geometry_display_constant_size_skip_test(void) {
  using namespace lightusd;

  Layer layer;
  std::string warn, err;
  bool loaded = LoadLayerFromFile(
      TestFixturePath("tests/usda/geometry-optimize-display-constant-size-001.usda"),
      &layer, &warn, &err);
  TEST_CHECK(loaded);
  if (!loaded) {
    TEST_MSG("LoadLayerFromFile failed: %s", err.c_str());
    return;
  }

  usdz::UsdzConvertOptions opts;
  opts.geometry_optimization = usdz::GeometryOptimizationMode::MergeMeshes;
  usdz::GeometryOptimizationStats stats;
  bool ok =
      usdz::OptimizeGeometryInLayer(opts, &layer, &stats, &warn, &err);
  TEST_CHECK(ok);
  TEST_CHECK(stats.num_meshes_before == 2);
  TEST_CHECK(stats.num_meshes_eligible == 0);
  TEST_CHECK(stats.num_meshes_skipped == 2);
  TEST_CHECK(stats.num_mesh_aggregates == 0);
  TEST_CHECK(stats.num_meshes_after == 2);
  TEST_CHECK(CountAggregateMeshes(layer) == 0);
}

// Error-path: invalid inputs should return errors, not crash.
void usdz_convert_error_path_test(void) {
  using namespace lightusd;

  // 1) Convert with nonexistent input file.
  {
    usdz::UsdzConvertOptions opts;
    opts.inputs.push_back("/nonexistent/path/to/file.usda");
    opts.output = "/tmp/should_not_exist.usdz";
    usdz::UsdzConvertStats stats;
    std::string warn, err;
    bool ok = usdz::Convert(opts, &stats, &warn, &err);
    TEST_CHECK(!ok);
    TEST_CHECK(!err.empty());
  }

  // 2) Repack with invalid channel count.
  {
    usdz::RepackSpec spec;
    spec.out_channels = 5;  // invalid
    std::string warn, err;
    bool ok = usdz::RepackTextureFiles(spec, "/tmp/should_not_exist.png",
                                 image::PngEncoder::Auto, &warn, &err);
    TEST_CHECK(!ok);
    TEST_CHECK(!err.empty());
  }

  // 3) Resize with zero dimensions.
  {
    Image img = MakeSolidImage(4, 4, 4, 0, 0, 0, 255);
    Image out;
    std::string err;
    bool ok = tydra::ResizeImage(img, 0, 0, &out, tydra::ResizeFilter::Linear,
                                 &err);
    TEST_CHECK(!ok);
  }
}

// Adversarial: feed truncated/corrupted image data to decoders; must not crash.
void usdz_convert_adversarial_image_test(void) {
  using namespace lightusd;

  // 1) Truncated PNG (valid header, then garbage).
  {
    const uint8_t truncated_png[] = {
      0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a,
      0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
      0x00, 0x00, 0x00, 0x01
    };
    auto dec = image::LoadImageFromMemory(truncated_png, sizeof(truncated_png),
                                          "truncated.png");
    TEST_CHECK(!dec.has_value());
  }

  // 2) Truncated JPEG.
  {
    const uint8_t truncated_jpg[] = {0xff, 0xd8, 0xff, 0xe0, 0x00, 0x10};
    auto dec = image::LoadImageFromMemory(truncated_jpg, sizeof(truncated_jpg),
                                          "truncated.jpg");
    TEST_CHECK(!dec.has_value());
  }

  // 3) Zero-length input.
  {
    auto dec = image::LoadImageFromMemory(nullptr, 0, "empty.png");
    TEST_CHECK(!dec.has_value());
  }

  // 4) Completely random bytes.
  {
    uint8_t garbage[64];
    for (int i = 0; i < 64; i++) garbage[i] = uint8_t(i * 37 + 13);
    auto dec = image::LoadImageFromMemory(garbage, sizeof(garbage),
                                          "garbage.png");
    TEST_CHECK(!dec.has_value());
  }

  // 5) ResizeImage with oversized destination.
  {
    Image img = MakeSolidImage(4, 4, 4, 0, 0, 0, 255);
    Image out;
    std::string err;
    bool ok = tydra::ResizeImage(img, 20000, 20000, &out,
                                 tydra::ResizeFilter::Linear, &err);
    TEST_CHECK(!ok);
    TEST_CHECK(!err.empty());
  }

  // 6) ResizeImage with negative destination dimensions.
  {
    Image img = MakeSolidImage(4, 4, 4, 0, 0, 0, 255);
    Image out;
    std::string err;
    bool ok = tydra::ResizeImage(img, -1, -1, &out,
                                 tydra::ResizeFilter::Linear, &err);
    TEST_CHECK(!ok);
  }
}

// PackChannels error paths.
void usdz_convert_pack_channels_error_test(void) {
  using namespace lightusd;

  // 1) Empty inputs vector.
  {
    std::vector<Image> inputs;
    tydra::ChannelPackSpec spec;
    spec.out_channels = 3;
    spec.r.input_index = 0;
    spec.r.channel = 0;
    Image packed;
    std::string err;
    bool ok = tydra::PackChannels(inputs, spec, &packed, &err);
    TEST_CHECK(!ok);
    TEST_CHECK(!err.empty());
  }

  // 2) out_channels = 0.
  {
    std::vector<Image> inputs = {MakeSolidImage(4, 4, 1, 10, 0, 0, 0)};
    tydra::ChannelPackSpec spec;
    spec.out_channels = 0;
    Image packed;
    std::string err;
    bool ok = tydra::PackChannels(inputs, spec, &packed, &err);
    TEST_CHECK(!ok);
  }

  // 3) out_channels = 5.
  {
    std::vector<Image> inputs = {MakeSolidImage(4, 4, 1, 10, 0, 0, 0)};
    tydra::ChannelPackSpec spec;
    spec.out_channels = 5;
    Image packed;
    std::string err;
    bool ok = tydra::PackChannels(inputs, spec, &packed, &err);
    TEST_CHECK(!ok);
  }

  // 4) Input index out of range.
  {
    std::vector<Image> inputs = {MakeSolidImage(4, 4, 1, 10, 0, 0, 0)};
    tydra::ChannelPackSpec spec;
    spec.out_channels = 1;
    spec.r.input_index = 99;
    spec.r.channel = 0;
    Image packed;
    std::string err;
    bool ok = tydra::PackChannels(inputs, spec, &packed, &err);
    TEST_CHECK(!ok);
  }

  // 5) 4-channel output with alpha.
  {
    Image r = MakeSolidImage(4, 4, 1, 50, 0, 0, 0);
    Image g = MakeSolidImage(4, 4, 1, 100, 0, 0, 0);
    Image b = MakeSolidImage(4, 4, 1, 150, 0, 0, 0);
    Image a = MakeSolidImage(4, 4, 1, 200, 0, 0, 0);
    std::vector<Image> inputs = {r, g, b, a};
    tydra::ChannelPackSpec spec;
    spec.out_channels = 4;
    spec.r.input_index = 0; spec.r.channel = 0;
    spec.g.input_index = 1; spec.g.channel = 0;
    spec.b.input_index = 2; spec.b.channel = 0;
    spec.a.input_index = 3; spec.a.channel = 0;
    Image packed;
    std::string err;
    bool ok = tydra::PackChannels(inputs, spec, &packed, &err);
    TEST_CHECK(ok);
    if (ok) {
      TEST_CHECK(packed.channels == 4);
      TEST_CHECK(packed.width == 4);
      TEST_CHECK(packed.height == 4);
      if (packed.data.size() >= 4) {
        TEST_CHECK(packed.data[0] == 50);
        TEST_CHECK(packed.data[1] == 100);
        TEST_CHECK(packed.data[2] == 150);
        TEST_CHECK(packed.data[3] == 200);
      }
    }
  }

  // 6) Same-size input with undersized data must be rejected before sampling.
  {
    Image bad = MakeSolidImage(4, 4, 1, 10, 0, 0, 0);
    bad.data.resize(1);
    std::vector<Image> inputs = {bad};
    tydra::ChannelPackSpec spec;
    spec.out_channels = 1;
    spec.r.input_index = 0;
    spec.r.channel = 0;
    Image packed;
    std::string err;
    bool ok = tydra::PackChannels(inputs, spec, &packed, &err);
    TEST_CHECK(!ok);
    TEST_CHECK(!err.empty());
  }

  // 7) Constant-only output with excessive dimensions must be capped.
  {
    usdz::RepackSpec spec;
    spec.out_channels = 1;
    spec.out_width = 20000;
    spec.out_height = 20000;
    spec.r.input_file.clear();
    spec.r.constant = 128;
    std::string warn, err;
    bool ok = usdz::RepackTextureFiles(spec, "/tmp/should_not_exist.png",
                                       image::PngEncoder::Auto, &warn, &err);
    TEST_CHECK(!ok);
    TEST_CHECK(!err.empty());
  }
}

// FitTexturesToBudget error paths and edge cases.
void usdz_convert_fit_budget_error_test(void) {
  using namespace lightusd;

  // 1) Empty inputs.
  {
    std::vector<tydra::FitTextureInput> inputs;
    tydra::FitTextureOptions opt;
    opt.target_total_bytes = 1000;
    opt.strategy = tydra::FitStrategy::Size;
    opt.min_texture_size = 16;
    std::vector<tydra::FitTextureOutput> outs;
    std::string warn, err;
    bool ok = tydra::FitTexturesToBudget(inputs, opt, &outs, &warn, &err);
    TEST_CHECK(ok);
    TEST_CHECK(outs.empty());
  }

  // 2) Non-reencodable textures only.
  {
    std::vector<tydra::FitTextureInput> inputs;
    for (int i = 0; i < 3; i++) {
      tydra::FitTextureInput fi;
      fi.image = MakeSolidImage(32, 32, 3, 100, 100, 100, 255);
      fi.original_bytes = EncodePNG(fi.image);
      fi.ext = "png";
      fi.reencodable = false;
      inputs.push_back(std::move(fi));
    }
    tydra::FitTextureOptions opt;
    opt.target_total_bytes = 100;
    opt.strategy = tydra::FitStrategy::Size;
    opt.min_texture_size = 8;
    std::vector<tydra::FitTextureOutput> outs;
    std::string warn, err;
    bool ok = tydra::FitTexturesToBudget(inputs, opt, &outs, &warn, &err);
    TEST_CHECK(ok);
    TEST_CHECK(outs.size() == inputs.size());
  }

  // 3) ResizeFilter::SRGB path.
  {
    Image img = MakeSolidImage(16, 8, 3, 128, 128, 128, 255);
    Image out;
    std::string err;
    bool ok = tydra::ResizeImage(img, 8, 4, &out,
                                 tydra::ResizeFilter::SRGB, &err);
    TEST_CHECK(ok);
    if (ok) {
      TEST_CHECK(out.width == 8);
      TEST_CHECK(out.height == 4);
      TEST_CHECK(out.channels == 3);
    }
  }

  // 4) ResizeFilter::Auto path.
  {
    Image img = MakeSolidImage(16, 16, 4, 128, 128, 128, 255);
    img.colorspace = "sRGB";
    Image out;
    std::string err;
    bool ok = tydra::ResizeImage(img, 8, 8, &out,
                                 tydra::ResizeFilter::Auto, &err);
    TEST_CHECK(ok);
    if (ok) {
      TEST_CHECK(out.width == 8);
      TEST_CHECK(out.height == 8);
    }
  }

  // 5) Fixed overhead over budget should still shrink reencodable textures
  // to the configured floor instead of keeping the largest setting.
  {
    std::vector<tydra::FitTextureInput> inputs;
    tydra::FitTextureInput shrinkable;
    shrinkable.image = MakeNoisyImage(128, 128, 3);
    shrinkable.ext = "png";
    shrinkable.reencodable = true;
    inputs.push_back(std::move(shrinkable));

    tydra::FitTextureInput fixed;
    fixed.original_bytes.assign(1024, uint8_t(7));
    fixed.ext = "exr";
    fixed.reencodable = false;
    inputs.push_back(std::move(fixed));

    tydra::FitTextureOptions opt;
    opt.target_total_bytes = 16;
    opt.strategy = tydra::FitStrategy::Size;
    opt.min_texture_size = 16;

    std::vector<tydra::FitTextureOutput> outs;
    std::string warn, err;
    bool ok = tydra::FitTexturesToBudget(inputs, opt, &outs, &warn, &err);
    TEST_CHECK(ok);
    if (ok) {
      TEST_CHECK(outs.size() == 2);
      TEST_CHECK(outs[0].width == 16);
      TEST_CHECK(outs[0].height == 16);
      TEST_CHECK(!warn.empty());
    }
  }
}

void usdz_convert_missing_texture_reference_test(void) {
  using namespace lightusd;

  const std::string dir = TempDir();

  // A safe relative missing texture should remain unchanged, not be rewritten
  // to a sanitized archive path that is not actually present.
  {
    const std::string usda_path =
        lightusd::io::JoinPath(dir, "scene_missing_safe.usda");
    const std::string usdz_path =
        lightusd::io::JoinPath(dir, "out_missing_safe.usdz");
    TEST_CHECK(WriteTexturedUSDA(usda_path, "missing.png"));

    usdz::UsdzConvertOptions opts;
    opts.inputs.push_back(usda_path);
    opts.output = usdz_path;
    opts.flatten = true;

    usdz::UsdzConvertStats stats;
    std::string warn, err;
    bool ok = usdz::Convert(opts, &stats, &warn, &err);
    TEST_CHECK(ok);
    TEST_CHECK(!warn.empty());
    TEST_CHECK(stats.num_textures == 0);
    if (!ok) {
      TEST_MSG("convert error: %s", err.c_str());
      return;
    }

    Stage stage;
    std::string lwarn, lerr;
    bool loaded = LoadUSDFromFile(usdz_path, &stage, &lwarn, &lerr);
    TEST_CHECK(loaded);
    if (loaded) {
      std::string texture_path;
      TEST_CHECK(FindTextureFilePath(stage, &texture_path));
      TEST_CHECK(texture_path == "missing.png");
    }
  }

  // A missing path that escapes the package root should fail instead of being
  // preserved or rewritten to a broken internal reference.
  {
    const std::string usda_path =
        lightusd::io::JoinPath(dir, "scene_missing_unsafe.usda");
    const std::string usdz_path =
        lightusd::io::JoinPath(dir, "out_missing_unsafe.usdz");
    TEST_CHECK(WriteTexturedUSDA(usda_path, "../missing.png"));

    usdz::UsdzConvertOptions opts;
    opts.inputs.push_back(usda_path);
    opts.output = usdz_path;
    opts.flatten = true;

    usdz::UsdzConvertStats stats;
    std::string warn, err;
    bool ok = usdz::Convert(opts, &stats, &warn, &err);
    TEST_CHECK(!ok);
    TEST_CHECK(err.find("Unsafe texture path") != std::string::npos);
  }
}

// Pipeline test with JPEG output format.
void usdz_convert_pipeline_jpeg_test(void) {
  using namespace lightusd;

  const std::string dir = TempDir();
  const std::string png_path = lightusd::io::JoinPath(dir, "tex_jpg.png");
  const std::string usda_path = lightusd::io::JoinPath(dir, "scene_jpg.usda");
  const std::string usdz_path = lightusd::io::JoinPath(dir, "out_jpg.usdz");

  Image tex = MakeSolidImage(64, 64, 4, 200, 150, 100, 255);
  {
    image::WriteOption wopt;
    wopt.format = image::WriteImageFormat::PNG;
    auto enc = image::WriteImageToMemory(tex, wopt);
    TEST_CHECK(enc.has_value());
    if (!enc) return;
    std::string werr;
    bool wok = io::WriteWholeFile(png_path, enc.value().data(),
                                  enc.value().size(), &werr);
    TEST_CHECK(wok);
    if (!wok) return;
  }

  {
    const std::string usda =
        "#usda 1.0\n"
        "(\n"
        "    defaultPrim = \"root\"\n"
        "    upAxis = \"Y\"\n"
        ")\n"
        "\n"
        "def Xform \"root\"\n"
        "{\n"
        "    def Material \"mat\"\n"
        "    {\n"
        "        token outputs:surface.connect = </root/mat/surface.outputs:surface>\n"
        "        def Shader \"surface\"\n"
        "        {\n"
        "            uniform token info:id = \"UsdPreviewSurface\"\n"
        "            color3f inputs:diffuseColor.connect = </root/mat/tex.outputs:rgb>\n"
        "            token outputs:surface\n"
        "        }\n"
        "        def Shader \"tex\"\n"
        "        {\n"
        "            uniform token info:id = \"UsdUVTexture\"\n"
        "            asset inputs:file = @tex_jpg.png@\n"
        "            float3 outputs:rgb\n"
        "        }\n"
        "    }\n"
        "}\n";
    std::string werr;
    bool wok = io::WriteWholeFile(
        usda_path, reinterpret_cast<const unsigned char *>(usda.data()),
        usda.size(), &werr);
    TEST_CHECK(wok);
    if (!wok) return;
  }

  usdz::UsdzConvertOptions opts;
  opts.inputs.push_back(usda_path);
  opts.output = usdz_path;
  opts.flatten = true;
  opts.arkit_compatible = true;
  opts.max_texture_size = 32;
  opts.texture_format = usdz::OutputTextureFormat::JPEG;
  opts.jpeg_quality = 80;

  usdz::UsdzConvertStats stats;
  std::string warn, err;
  bool ok = usdz::Convert(opts, &stats, &warn, &err);
  TEST_CHECK(ok);
  if (!ok) {
    TEST_MSG("convert error: %s", err.c_str());
    return;
  }
  TEST_CHECK(stats.num_textures >= 1);

  std::vector<uint8_t> usdz_bytes;
  std::string ioerr;
  bool rok = io::ReadWholeFile(&usdz_bytes, &ioerr, usdz_path, 0);
  TEST_CHECK(rok);
  if (!rok) return;
  std::string vwarn, verr;
  bool valid = lightusd::ValidateUSDZ(usdz_bytes.data(), usdz_bytes.size(),
                                      &vwarn, &verr);
  TEST_CHECK(valid);
  if (!valid) {
    TEST_MSG("validate: %s", verr.c_str());
  }
}

// Cleanup: remove temp files created by earlier tests.
void usdz_convert_cleanup_test(void) {
  const std::string base =
      lightusd::io::JoinPath(lightusd::io::GetTempDir(), "lusdzconvert_test");
  if (lightusd::io::IsDirectory(base)) {
    lightusd::io::RemoveAll(base);
    // Not a test failure if cleanup fails (e.g. file in use).
  }
  TEST_CHECK(true);  // Always passes; this is a bookkeeping test.
}

// EXR encode + decode roundtrip. Guards two fixes: WriteImageToMemory EXR
// output (previously a stub) and the inverted LoadEXRFromMemory success check
// (which made every successful EXR decode report failure).
void usdz_convert_exr_roundtrip_test(void) {
  using namespace lightusd;

  Image img;
  img.width = 4; img.height = 4; img.channels = 4; img.bpp = 32;
  img.format = Image::PixelFormat::Float;
  img.data.resize(size_t(4) * 4 * 4 * sizeof(float));
  {
    float *fp = reinterpret_cast<float *>(img.data.data());
    for (int i = 0; i < 4 * 4; i++) {
      fp[i * 4 + 0] = 0.25f;
      fp[i * 4 + 1] = 0.5f;
      fp[i * 4 + 2] = 0.75f;
      fp[i * 4 + 3] = 1.0f;
    }
  }

  image::WriteOption wopt;
  wopt.format = image::WriteImageFormat::EXR;
  auto enc = image::WriteImageToMemory(img, wopt);
  TEST_CHECK(bool(enc));
  if (!enc) { TEST_MSG("EXR encode: %s", enc.error().c_str()); return; }
  const auto &b = enc.value();
  TEST_CHECK(b.size() >= 4);
  TEST_CHECK(b[0] == 0x76 && b[1] == 0x2f && b[2] == 0x31 && b[3] == 0x01);

  auto dec = image::LoadImageFromMemory(b.data(), b.size(), "rt.exr");
  TEST_CHECK(bool(dec));
  if (!dec) { TEST_MSG("EXR decode: %s", dec.error().c_str()); return; }
  const Image &out = dec.value().image;
  TEST_CHECK(out.width == 4 && out.height == 4);
  TEST_CHECK(out.format == Image::PixelFormat::Float &&
             (out.bpp == 16 || out.bpp == 32));
  if (out.bpp == 32) {
    const float *of = reinterpret_cast<const float *>(out.data.data());
    TEST_CHECK(std::fabs(of[0] - 0.25f) < 0.01f);
    TEST_CHECK(std::fabs(of[1] - 0.5f) < 0.01f);
    TEST_CHECK(std::fabs(of[2] - 0.75f) < 0.01f);
  } else {
    const uint16_t *of = reinterpret_cast<const uint16_t *>(out.data.data());
    // The EXR writer stores float input as HALF; verify exact half payloads.
    TEST_CHECK(of[0] == 0x3400 && of[1] == 0x3800 && of[2] == 0x3A00);
  }
}

// fp32 (HDR) resize support in ResizeImage (previously 8-bit only).
void usdz_convert_resize_float_test(void) {
  using namespace lightusd;

  Image img;
  img.width = 8; img.height = 8; img.channels = 3; img.bpp = 32;
  img.format = Image::PixelFormat::Float;
  img.data.resize(size_t(8) * 8 * 3 * sizeof(float));
  {
    float *fp = reinterpret_cast<float *>(img.data.data());
    for (size_t i = 0; i < size_t(8) * 8 * 3; i++) fp[i] = 0.5f;
  }

  Image out;
  std::string err;
  bool ok = tydra::ResizeImage(img, 4, 4, &out, tydra::ResizeFilter::Auto, &err);
  TEST_CHECK(ok);
  if (!ok) { TEST_MSG("float resize: %s", err.c_str()); return; }
  TEST_CHECK(out.width == 4 && out.height == 4 && out.channels == 3);
  TEST_CHECK(out.bpp == 32 && out.format == Image::PixelFormat::Float);
  const float *of = reinterpret_cast<const float *>(out.data.data());
  TEST_CHECK(std::fabs(of[0] - 0.5f) < 0.001f);
}
