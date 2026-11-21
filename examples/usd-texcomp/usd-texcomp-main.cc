// usd-texcomp — author GPU-compressed texture companions for a USD layer.
//
// For every `UsdUVTexture.inputs:file` that points at an ordinary image
// (png/jpg/...), compress it to a `.ktx2` carrying the tinyexr `uni`
// private transcodable intermediate with a full mip chain, write it next to the
// output, and record it on the attribute as a *legacy-safe hint*:
//
//     asset inputs:file = @diffuse.png@ ( customData = { asset ktx2 = @diffuse.ktx2@ } )
//
// `inputs:file` is left untouched, so the asset still opens in stock USD tools
// and stays USDZ-legal; lightusd-aware consumers (lusdview
// --texture-keep-compressed on) find the companion and upload/transcode its GPU
// blocks directly. This is the producer side of doc/texcomp.md.
//
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "image-loader.hh"
#include "io-util.hh"
#include "load-usd.hh"
#include "writer/usda-writer.hh"

extern "C" {
#include "texpipe.h"  // tp_ktx2_write_uni[_zstd] (pulls in texcomp.h)
#include "tir.h"      // tir_resize (mip chain)
}

#if defined(LIGHTUSD_WITH_ZSTD_COMPRESSION)
#include "external/zstd.h"  // ZSTD_compress for supercompressionScheme 2
#endif

namespace {

#if defined(LIGHTUSD_WITH_ZSTD_COMPRESSION)
// Zstd callbacks for tp_ktx2_write_uni_zstd (texpipe carries no zstd itself).
size_t ZstdBound(void * /*user*/, size_t src_size) {
  return ZSTD_compressBound(src_size);
}
size_t ZstdCompress(void * /*user*/, uint8_t *dst, size_t dst_cap,
                    const uint8_t *src, size_t src_size) {
  const size_t r = ZSTD_compress(dst, dst_cap, src, src_size, 19);
  return ZSTD_isError(r) ? 0u : r;
}
constexpr bool kZstdAvailable = true;
#else
constexpr bool kZstdAvailable = false;
#endif

std::string LowerExt(const std::string &p) {
  const std::string e = lightusd::io::GetFileExtension(p);
  std::string lo;
  for (char c : e) lo += char(std::tolower(static_cast<unsigned char>(c)));
  return lo;
}

bool IsHdrExt(const std::string &p) {
  const std::string e = LowerExt(p);
  return e == "exr" || e == "hdr";
}

bool HasImageExt(const std::string &p) {
  const std::string e = LowerExt(p);
  return e == "png" || e == "jpg" || e == "jpeg" || e == "bmp" || e == "tga" ||
         e == "exr" || e == "hdr";
}

// Load an HDR image as tightly-packed float RGB (3 channels), for the BC6H path.
bool LoadRGBf(const std::string &path, std::vector<float> *out, int *w, int *h,
              std::string *err) {
  auto r = lightusd::image::LoadImageFromFile(path);
  if (!r) {
    if (err) *err = r.error();
    return false;
  }
  const lightusd::Image &im = r.value().image;
  if (im.format != lightusd::Image::PixelFormat::Float || im.width <= 0 ||
      im.height <= 0 || im.channels < 3) {
    if (err) *err = "not a float RGB(A) image";
    return false;
  }
  const size_t n = size_t(im.width) * size_t(im.height);
  const int ch = im.channels;
  out->resize(n * 3u);
  if (im.bpp == 32) {
    const float *src = reinterpret_cast<const float *>(im.data.data());
    for (size_t i = 0; i < n; ++i)
      for (int c = 0; c < 3; ++c) (*out)[i * 3 + size_t(c)] = src[i * size_t(ch) + size_t(c)];
  } else {
    if (err) *err = "unsupported HDR bit depth (need fp32)";
    return false;
  }
  *w = im.width;
  *h = im.height;
  return true;
}

// Float RGB -> BC6H KTX2 (mipped). BC6H is a direct GPU HDR format (not the
// transcodable `uni`), so it uploads as-is where BPTC is supported and decodes
// to float / tone-maps elsewhere.
bool EncodeBc6hKtx2(std::vector<float> &rgb, int w, int h, bool mips,
                    std::vector<uint8_t> *out, int *levels_out,
                    std::string *err) {
  tir_image_view v{rgb.data(), w, h, 3, TIR_F32, 0};
  tp_options o;
  tp_options_init(&o, TP_CONTENT_COLOR, TP_CODEC_BC6H);
  o.container = TP_CONTAINER_KTX2;
  if (!mips) o.max_levels = 1;
  uint8_t *buf = nullptr;
  size_t bsz = 0;
  if (!TP_OK(tp_process(nullptr, &v, 1, &o, &buf, &bsz))) {
    if (err) *err = "BC6H tp_process failed";
    return false;
  }
  out->assign(buf, buf + bsz);
  tp_free(nullptr, buf);
  // level count = full halving chain unless capped
  int n = 1;
  if (mips) {
    int m = w > h ? w : h;
    while (m > 1) { m >>= 1; ++n; }
  }
  *levels_out = n;
  return true;
}

std::string ReplaceExtWithKtx2(const std::string &p) {
  const size_t dot = p.find_last_of('.');
  return (dot == std::string::npos ? p : p.substr(0, dot)) + ".ktx2";
}

// Load an image and normalize it to tightly-packed 8-bit RGBA.
bool LoadRGBA8(const std::string &path, std::vector<uint8_t> *out, int *w, int *h,
               std::string *err) {
  auto r = lightusd::image::LoadImageFromFile(path);
  if (!r) {
    if (err) *err = r.error();
    return false;
  }
  const lightusd::Image &im = r.value().image;
  if (im.bpp != 8 || im.width <= 0 || im.height <= 0) {
    if (err) *err = "not an 8-bit image (uni is LDR RGBA8 only)";
    return false;
  }
  const size_t n = size_t(im.width) * size_t(im.height);
  out->assign(n * 4u, 255);
  const int ch = im.channels;
  if (ch < 1 || ch > 4) {
    if (err) *err = "unsupported channel count";
    return false;
  }
  for (size_t i = 0; i < n; ++i) {
    for (int c = 0; c < 4; ++c) {
      if (c < ch) {
        (*out)[i * 4 + size_t(c)] = im.data[i * size_t(ch) + size_t(c)];
      } else if (c == 3) {
        (*out)[i * 4 + 3] = 255;  // opaque when the source has no alpha
      } else {
        (*out)[i * 4 + size_t(c)] = (ch == 1) ? im.data[i] : 0;  // splat gray
      }
    }
  }
  *w = im.width;
  *h = im.height;
  return true;
}

// RGBA8 -> private uni mip chain -> KTX2 bytes. `zstd` supercompresses the
// level payloads (supercompressionScheme 2). This carrier is decoded by
// lightusd/textools; it is not Basis UASTC.
bool EncodeUniKtx2(const std::vector<uint8_t> &rgba, int w, int h, bool mips,
                   bool zstd, std::vector<uint8_t> *out, int *levels_out,
                   std::string *err) {
  std::vector<std::vector<uint8_t>> uni;
  std::vector<uint32_t> lw, lh;
  int cw = w, ch = h;
  for (int l = 0;; ++l) {
    std::vector<uint8_t> level;
    if (l == 0) {
      level = rgba;
    } else {
      level.resize(size_t(cw) * size_t(ch) * 4u);
      tir_image_view src{const_cast<uint8_t *>(rgba.data()), w, h, 4, TIR_U8, 0};
      tir_image_view dst{level.data(), cw, ch, 4, TIR_U8, 0};
      tir_options o;
      tir_options_init(&o);
      if (!TIR_OK(tir_resize(nullptr, &src, &dst, &o))) {
        if (err) *err = "tir_resize failed";
        return false;
      }
    }
    const size_t usz = tc_uni_compressed_size(uint32_t(cw), uint32_t(ch));
    std::vector<uint8_t> u(usz);
    if (tc_uni_compress_rgba8(level.data(), uint32_t(cw), uint32_t(ch),
                              size_t(cw) * 4u, u.data(), usz) != TC_SUCCESS) {
      if (err) *err = "uni compress failed";
      return false;
    }
    uni.push_back(std::move(u));
    lw.push_back(uint32_t(cw));
    lh.push_back(uint32_t(ch));
    if (!mips || (cw == 1 && ch == 1)) break;
    cw = cw > 1 ? cw / 2 : 1;
    ch = ch > 1 ? ch / 2 : 1;
  }
  std::vector<const uint8_t *> lp;
  std::vector<size_t> ls;
  for (auto &u : uni) {
    lp.push_back(u.data());
    ls.push_back(u.size());
  }
  const int n = int(uni.size());

#if defined(LIGHTUSD_WITH_ZSTD_COMPRESSION)
  if (zstd) {
    uint8_t *buf = nullptr;
    size_t bsz = 0;
    if (!TP_OK(tp_ktx2_write_uni_zstd(nullptr, &ZstdBound, &ZstdCompress, nullptr,
                                      lp.data(), ls.data(), uint32_t(w),
                                      uint32_t(h), n, TP_UNI_ALPHA, &buf,
                                      &bsz))) {
      if (err) *err = "tp_ktx2_write_uni_zstd failed";
      return false;
    }
    out->assign(buf, buf + bsz);
    tp_free(nullptr, buf);
    *levels_out = n;
    return true;
  }
#else
  (void)zstd;
#endif

  const size_t ksz = tp_ktx2_uni_size(ls.data(), n);
  if (ksz == 0) {
    if (err) *err = "ktx2 layout overflow";
    return false;
  }
  out->resize(ksz);
  if (!TP_OK(tp_ktx2_write_uni_ex(lp.data(), ls.data(), uint32_t(w), uint32_t(h),
                                  n, TP_UNI_ALPHA, out->data(), ksz, nullptr))) {
    if (err) *err = "tp_ktx2_write_uni failed";
    return false;
  }
  *levels_out = n;
  return true;
}

// Record `src -> ktx2` on every asset-valued attribute whose value is `src`,
// as a customData "ktx2" hint. `inputs:file` itself is left unchanged.
size_t AddKtx2Hints(lightusd::PrimSpec &ps,
                    const std::map<std::string, std::string> &hint) {
  size_t n = 0;
  for (const auto &slot : ps.properties().slots()) {
    const auto* value = ps.property_value(slot.name_id);
    const auto path = value ? value->as_asset_path() : lightusd::StringView{};
    if (!path) continue;
    const auto it = hint.find(*path);
    if (it == hint.end()) continue;
    auto& meta = ps.ensure_property_meta(slot.name_id);
    if (!meta.customData.is_dictionary())
      meta.customData = lightusd::Value::MakeDictionary();
    meta.customData.as_dictionary().set("ktx2", lightusd::Value::MakeAssetPath(it->second));
    meta.authored |= lightusd::PropMeta::kCustomData;
    ++n;
  }
  return n;
}

void CollectAssetPaths(const lightusd::PrimSpec &ps, std::vector<std::string> *out) {
  for (const auto &slot : ps.properties().slots()) {
    const auto* value = ps.property_value(slot.name_id);
    const auto path = value ? value->as_asset_path() : lightusd::StringView{};
    if (path && !path->empty() && HasImageExt(*path)) out->push_back(*path);
  }
}

void Usage() {
  std::printf(
      "usd-texcomp — author GPU-compressed (.ktx2) texture companions\n"
      "\n"
      "usage: usd-texcomp <input.usd[a|c]> -o <output.usda> [--mips on|off]\n"
      "                   [--zstd on|off]   (Zstd-supercompress the .ktx2; default on)\n"
      "\n"
      "For each UsdUVTexture inputs:file image, writes <name>.ktx2 (private uni,\n"
      "mipped) next to the output and adds a legacy-safe hint on the attribute:\n"
      "  asset inputs:file = @t.png@ ( customData = { asset ktx2 = @t.ktx2@ } )\n"
      "inputs:file is unchanged, so stock USD tools and USDZ are unaffected.\n"
      "Consume with: lusdview <output.usda> --texture-keep-compressed on\n");
}

}  // namespace

int main(int argc, char **argv) {
  std::string in_path, out_path;
  bool mips = true;
  bool zstd = kZstdAvailable;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "-o") && (i + 1) < argc) {
      out_path = argv[++i];
    } else if (!std::strcmp(argv[i], "--mips") && (i + 1) < argc) {
      mips = !std::strcmp(argv[++i], "on");
    } else if (!std::strcmp(argv[i], "--zstd") && (i + 1) < argc) {
      zstd = kZstdAvailable && !std::strcmp(argv[++i], "on");
    } else if (!std::strcmp(argv[i], "-h") || !std::strcmp(argv[i], "--help")) {
      Usage();
      return 0;
    } else if (in_path.empty()) {
      in_path = argv[i];
    }
  }
  if (in_path.empty() || out_path.empty()) {
    Usage();
    return 1;
  }

  lightusd::Stage stage;
  std::string warn, err;
  if (!lightusd::LoadUSD(in_path, &stage, &warn, &err)) {
    std::fprintf(stderr, "failed to load %s: %s\n", in_path.c_str(), err.c_str());
    return 1;
  }
  if (!warn.empty()) std::fprintf(stderr, "warn: %s\n", warn.c_str());

  auto& layer = *stage.GetRootLayer();
  const std::string in_dir = lightusd::io::GetBaseDir(in_path);
  const std::string out_dir = lightusd::io::GetBaseDir(out_path);

  // Collect the referenced image assets.
  std::vector<std::string> assets;
  for (size_t i = 0; i < layer.prim_count(); ++i)
    CollectAssetPaths(*layer.prim(static_cast<uint32_t>(i)), &assets);
  std::sort(assets.begin(), assets.end());
  assets.erase(std::unique(assets.begin(), assets.end()), assets.end());
  if (assets.empty()) {
    std::fprintf(stderr, "no image textures found in %s\n", in_path.c_str());
    return 1;
  }

  std::map<std::string, std::string> hint;  // src asset path -> ktx2 asset path
  size_t total_src = 0, total_ktx = 0;
  for (const std::string &a : assets) {
    const std::string src = in_dir.empty() ? a : lightusd::io::JoinPath(in_dir, a);
    const bool is_hdr = IsHdrExt(a);
    int w = 0, h = 0;
    std::string lerr;
    std::vector<uint8_t> ktx;
    int levels = 0;
    size_t src_px_bytes = 0;
    const char *kind = nullptr;

    if (is_hdr) {
      // HDR (EXR/.hdr) -> BC6H, the GPU-native HDR block format.
      std::vector<float> rgb;
      if (!LoadRGBf(src, &rgb, &w, &h, &lerr) ||
          !EncodeBc6hKtx2(rgb, w, h, mips, &ktx, &levels, &lerr)) {
        std::fprintf(stderr, "skip %s: %s\n", a.c_str(), lerr.c_str());
        continue;
      }
      src_px_bytes = size_t(w) * size_t(h) * 4u * sizeof(float);  // RGBA32F
      kind = "bc6h";
    } else {
      std::vector<uint8_t> rgba;
      if (!LoadRGBA8(src, &rgba, &w, &h, &lerr) ||
          !EncodeUniKtx2(rgba, w, h, mips, zstd, &ktx, &levels, &lerr)) {
        std::fprintf(stderr, "skip %s: %s\n", a.c_str(), lerr.c_str());
        continue;
      }
      src_px_bytes = size_t(w) * size_t(h) * 4u;  // RGBA8
      kind = zstd ? "zstd-uni" : "uni";
    }
    const std::string rel = ReplaceExtWithKtx2(a);
    const std::string dst = out_dir.empty() ? rel : lightusd::io::JoinPath(out_dir, rel);
    std::ofstream f(dst, std::ios::binary);
    if (!f) {
      std::fprintf(stderr, "skip %s: cannot write %s\n", a.c_str(), dst.c_str());
      continue;
    }
    f.write(reinterpret_cast<const char *>(ktx.data()),
            std::streamsize(ktx.size()));
    if (!f) {
      std::fprintf(stderr, "skip %s: write failed\n", a.c_str());
      continue;
    }
    f.close();

    const size_t src_bytes = src_px_bytes;
    total_src += src_bytes;
    total_ktx += ktx.size();
    hint[a] = rel;
    std::printf(
        "  %s -> %s (%dx%d, %d level(s), %zu KiB %s-KTX2 vs %zu KiB uncompressed)\n",
        a.c_str(), rel.c_str(), w, h, levels, ktx.size() / 1024, kind,
        src_bytes / 1024);
  }

  if (hint.empty()) {
    std::fprintf(stderr, "no textures compressed\n");
    return 1;
  }

  size_t nhint = 0;
  for (size_t i = 0; i < layer.prim_count(); ++i)
    nhint += AddKtx2Hints(*layer.prim_mutable(static_cast<uint32_t>(i)), hint);

  const std::string usda = lightusd::WriteLayerToString(layer);
  std::ofstream o(out_path);
  if (!o) {
    std::fprintf(stderr, "cannot write %s\n", out_path.c_str());
    return 1;
  }
  o << usda;
  o.close();

  std::printf(
      "wrote %s (%zu texture(s), %zu hint(s)); GPU blocks %zu KiB vs %zu KiB "
      "RGBA8 (%.1fx)\n",
      out_path.c_str(), hint.size(), nhint, total_ktx / 1024, total_src / 1024,
      total_ktx ? double(total_src) / double(total_ktx) : 0.0);
  return 0;
}
