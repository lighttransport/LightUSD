// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.

#include "texture-cache.hh"
#include "next/resolver/asset-resolver.hh"
#include "next/safe-file-size.hh"
#include "usdz-entry-match.hh"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <cctype>
#include <limits>
#include <utility>

#include "image-loader.hh"
#include "ptx-loader.hh"
#include "next/reader/usdz-reader.hh"
#include "safe-arithmetic.hh"
#include "tydra/texture-util.hh"
#include "value-types.hh"

namespace lightusd {
namespace tydra {
namespace next {

namespace {

// Match a USD asset path against a .usdz entry name. Entries are archive-
// relative and may carry a directory prefix the authored path omits, so accept
// an exact match, a path-suffix match on a directory boundary, or a basename
// match as the last resort.
bool UsdzEntryMatches(const std::string& entry, const std::string& asset) {
  std::string a = asset;
  if (a.rfind("./", 0) == 0) a = a.substr(2);
  if (entry == a) return true;
  if (entry.size() > a.size() &&
      entry.compare(entry.size() - a.size(), a.size(), a) == 0 &&
      entry[entry.size() - a.size() - 1] == '/') {
    return true;
  }
  auto base = [](const std::string& s) {
    const size_t p = s.find_last_of('/');
    return p == std::string::npos ? s : s.substr(p + 1);
  };
  return base(entry) == base(a);
}

bool SourceImageWithinLimit(const TextureDecodeOptions& opt,
                            const uint8_t* data, size_t size,
                            const std::string& uri) {
  if (opt.max_source_bytes == 0) return true;
  auto info = ::lightusd::image::GetImageInfoFromMemory(data, size, uri);
  if (!info) return false;
  size_t pixels = 0;
  size_t samples = 0;
  size_t estimate = 0;
  if (!safe::mul(static_cast<size_t>(info.value().width),
                 static_cast<size_t>(info.value().height), &pixels) ||
      !safe::mul(pixels,
                 std::max<size_t>(4, info.value().channels), &samples) ||
      !safe::mul(samples, size_t{4}, &estimate)) {
    return false;
  }
  return static_cast<uint64_t>(estimate) <= opt.max_source_bytes;
}

bool LoadSourceImage(const TextureDecodeOptions& opt, const std::string& asset,
                     const std::vector<size_t>* usdz_candidates,
                     ::lightusd::Image* out) {
  if (asset.empty()) return false;
  if (opt.usdz && usdz_candidates) {
    // Candidates are in entry order, so the first match is the same entry the
    // full scan used to pick.
    for (size_t i : *usdz_candidates) {
      if (!UsdzEntryMatches(opt.usdz->EntryName(i), asset)) continue;
      if (!SourceImageWithinLimit(opt, opt.usdz->EntryData(i),
                                  opt.usdz->EntrySize(i),
                                  opt.usdz->EntryName(i))) {
        return false;
      }
      auto res = ::lightusd::image::LoadImageFromMemory(
          opt.usdz->EntryData(i), opt.usdz->EntrySize(i),
          opt.usdz->EntryName(i));
      if (!res) return false;
      *out = std::move(res.value().image);
      return true;
    }
  }
  std::string path = asset;
  if (!::lightusd::next::AssetResolver::IsAbsolutePath(path) &&
      !opt.base_dir.empty()) {
    path = opt.base_dir + "/" + path;
  }
  if (opt.max_source_bytes != 0) {
    // Metadata preflight is mmap-backed for TIFF/BigTIFF, so a multi-gigabyte
    // texture is not copied merely to determine its decoded size.
    std::ifstream probe(path, std::ios::binary | std::ios::ate);
    if (!probe.is_open()) return false;
    const std::streamoff encoded_size = probe.tellg();
    if (encoded_size < 0 ||
        static_cast<uint64_t>(encoded_size) > opt.max_source_bytes) {
      return false;
    }
    auto info = ::lightusd::image::GetImageInfoFromFile(path);
    if (!info) return false;
    size_t pixels = 0;
    size_t samples = 0;
    size_t decoded_bytes = 0;
    if (!safe::mul(static_cast<size_t>(info.value().width),
                   static_cast<size_t>(info.value().height), &pixels) ||
        !safe::mul(pixels, std::max<size_t>(4, info.value().channels), &samples) ||
        !safe::mul(samples, size_t{4}, &decoded_bytes) ||
        static_cast<uint64_t>(decoded_bytes) > opt.max_source_bytes) {
      return false;
    }
  }
  auto res = ::lightusd::image::LoadImageFromFile(path);
  if (!res) return false;
  *out = std::move(res.value().image);
  return true;
}

// Raw bytes of an asset, resolved exactly like LoadSourceImage (usdz entry first,
// then base_dir-relative on disk).
bool ReadSourceBytes(const TextureDecodeOptions& opt, const std::string& asset,
                     const std::vector<size_t>* usdz_candidates,
                     std::vector<uint8_t>* out) {
  if (asset.empty() || !out) return false;
  if (opt.usdz && usdz_candidates) {
    for (size_t i : *usdz_candidates) {
      if (!UsdzEntryMatches(opt.usdz->EntryName(i), asset)) continue;
      const uint8_t* p = opt.usdz->EntryData(i);
      const size_t n = opt.usdz->EntrySize(i);
      if (!p || !n ||
          (opt.max_source_bytes != 0 &&
           static_cast<uint64_t>(n) > opt.max_source_bytes)) {
        return false;
      }
      out->assign(p, p + n);
      return true;
    }
  }
  std::string path = asset;
  if (!::lightusd::next::AssetResolver::IsAbsolutePath(path) &&
      !opt.base_dir.empty()) {
    path = opt.base_dir + "/" + path;
  }
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) return false;
  size_t n = 0;
  if (!::lightusd::next::SafeStreamSize(f, 0, &n)) return false;
  if (opt.max_source_bytes != 0 &&
      static_cast<uint64_t>(n) > opt.max_source_bytes) {
    return false;
  }
  f.seekg(0, std::ios::beg);
  out->resize(n);
  return bool(f.read(reinterpret_cast<char*>(out->data()),
                     static_cast<std::streamsize>(n)));
}

// Narrow a 16-bit UInt image to 8-bit in place. Loaders hand back 16-bit PNGs
// as-is, but every consumer downstream wants 8-bit RGBA.
bool NarrowTo8Bit(::lightusd::Image* img) {
  if (img->bpp != 16 ||
      img->format != ::lightusd::Image::PixelFormat::UInt) {
    return true;
  }
  if (img->width <= 0 || img->height <= 0 || img->channels <= 0) return false;
  size_t pixels = 0;
  size_t samples = 0;
  size_t source_bytes = 0;
  if (!safe::mul(static_cast<size_t>(img->width),
                 static_cast<size_t>(img->height), &pixels) ||
      !safe::mul(pixels, static_cast<size_t>(img->channels), &samples) ||
      !safe::mul(samples, sizeof(uint16_t), &source_bytes) ||
      img->data.size() < source_bytes) {
    return false;
  }
  std::vector<uint8_t> narrowed(samples);
  for (size_t i = 0; i < samples; ++i) {
    uint16_t v = 0;
    std::memcpy(&v, img->data.data() + i * sizeof(uint16_t), sizeof(v));
    narrowed[i] = static_cast<uint8_t>((uint32_t(v) + 128u) / 257u);
  }
  img->data = std::move(narrowed);
  img->bpp = 8;
  return true;
}

// Take 1/2/3/4-channel 8-bit texels. With `force_rgba`, grayscale replicates
// into RGB and a missing alpha channel becomes opaque; otherwise the source
// channel count is kept as-is.
bool ToDecoded(::lightusd::Image* src, bool force_rgba,
               DecodedImage* out) {
  if (!src || !out || src->width <= 0 || src->height <= 0) {
    return false;
  }
  if (src->format == ::lightusd::Image::PixelFormat::Float &&
      (src->bpp == 16 || src->bpp == 32)) {
    const size_t ch = static_cast<size_t>(src->channels);
    size_t npix = 0, samples = 0, source_bytes = 0;
    if (ch < 1 || ch > 4 ||
        !safe::mul(static_cast<size_t>(src->width),
                   static_cast<size_t>(src->height), &npix) ||
        !safe::mul(npix, ch, &samples) ||
        !safe::mul(samples, static_cast<size_t>(src->bpp / 8),
                   &source_bytes) || src->data.size() < source_bytes) {
      return false;
    }
    out->width = static_cast<uint32_t>(src->width);
    out->height = static_cast<uint32_t>(src->height);
    out->channels = 3;
    out->hdr = true;
    out->float_pixels.resize(npix * 3u);
    auto sample = [&](size_t index) -> float {
      if (src->bpp == 32) {
        float v = 0.0f;
        std::memcpy(&v, src->data.data() + index * sizeof(float), sizeof(v));
        return v;
      }
      uint16_t h = 0;
      std::memcpy(&h, src->data.data() + index * sizeof(uint16_t), sizeof(h));
      ::lightusd::value::half halfValue{};
      halfValue.value = h;
      return ::lightusd::value::half_to_float(halfValue);
    };
    for (size_t i = 0; i < npix; ++i) {
      const size_t s = i * ch;
      float r = sample(s);
      float g = ch > 1 ? sample(s + 1) : r;
      float b = ch > 2 ? sample(s + 2) : g;
      out->float_pixels[i * 3u + 0] = r;
      out->float_pixels[i * 3u + 1] = g;
      out->float_pixels[i * 3u + 2] = b;
    }
    return true;
  }
  if (src->bpp != 8 || src->format != ::lightusd::Image::PixelFormat::UInt)
    return false;
  const size_t ch = static_cast<size_t>(src->channels);
  if (ch < 1 || ch > 4) return false;
  size_t npix = 0;
  size_t source_bytes = 0;
  if (!safe::mul(static_cast<size_t>(src->width),
                 static_cast<size_t>(src->height), &npix) ||
      !safe::mul(npix, ch, &source_bytes) ||
      src->data.size() < source_bytes) {
    return false;
  }

  out->width = static_cast<uint32_t>(src->width);
  out->height = static_cast<uint32_t>(src->height);

  // The loader's byte buffer already has the exact representation needed by
  // these cases. Move it instead of copying a full image and keeping both
  // buffers resident during large-scene conversion. Resize first in case a
  // decoder returned trailing bytes in its vector.
  if (!force_rgba || ch == 4) {
    out->channels = static_cast<uint8_t>(ch);
    src->data.resize(source_bytes);
    out->pixels = std::move(src->data);
    return true;
  }

  size_t rgba_bytes = 0;
  if (!safe::mul(npix, size_t{4}, &rgba_bytes)) return false;
  out->channels = 4;
  out->pixels.assign(rgba_bytes, 255);
  for (size_t i = 0; i < npix; ++i) {
    const uint8_t* s = src->data.data() + i * ch;
    uint8_t* d = out->pixels.data() + i * 4;
    switch (ch) {
      case 1:
        d[0] = d[1] = d[2] = s[0];
        break;
      case 2:
        d[0] = d[1] = d[2] = s[0];
        d[3] = s[1];
        break;
      case 3:
        d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
        break;
      default:
        d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
        break;
    }
  }
  return true;
}

bool ResizeDecoded(DecodedImage* img, uint32_t w, uint32_t h, bool srgb) {
  if (w == 0 || h == 0 || img->width == 0 || img->height == 0) return false;
  if (img->width == w && img->height == h) return true;

  if (img->hdr) {
    ::lightusd::Image src;
    src.width = static_cast<int>(img->width);
    src.height = static_cast<int>(img->height);
    src.channels = 3;
    src.bpp = 32;
    src.format = ::lightusd::Image::PixelFormat::Float;
    src.data.resize(img->float_pixels.size() * sizeof(float));
    std::memcpy(src.data.data(), img->float_pixels.data(), src.data.size());
    ::lightusd::Image dst;
    std::string err;
    if (!ResizeImage(src, static_cast<int>(w), static_cast<int>(h), &dst,
                     ResizeFilter::Linear, &err)) return false;
    img->width = static_cast<uint32_t>(dst.width);
    img->height = static_cast<uint32_t>(dst.height);
    img->channels = 3;
    img->float_pixels.resize(dst.data.size() / sizeof(float));
    std::memcpy(img->float_pixels.data(), dst.data.data(), dst.data.size());
    return true;
  }

  ::lightusd::Image src;
  src.width = static_cast<int>(img->width);
  src.height = static_cast<int>(img->height);
  src.channels = static_cast<int>(img->channels);
  src.bpp = 8;
  src.format = ::lightusd::Image::PixelFormat::UInt;
  // Move, don't copy: img->pixels is overwritten with dst.data below, so the
  // source buffer is dead the moment ResizeImage returns. Copying held source
  // + copy + destination (~3x an 8K RGBA image = 800 MB) instead of 2x, on
  // exactly the oversized-texture path the budget exists to protect.
  src.data = std::move(img->pixels);

  ::lightusd::Image dst;
  const ResizeFilter filter = srgb ? ResizeFilter::SRGB : ResizeFilter::Linear;
  std::string err;
  if (!ResizeImage(src, static_cast<int>(w), static_cast<int>(h), &dst, filter,
                   &err)) {
    img->pixels = std::move(src.data);  // give the moved-from source back
    return false;
  }
  img->width = static_cast<uint32_t>(dst.width);
  img->height = static_cast<uint32_t>(dst.height);
  img->channels = static_cast<uint8_t>(dst.channels);
  img->pixels = std::move(dst.data);
  return true;
}

bool HasPtxExtension(const std::string& asset) {
  if (asset.size() < 4) return false;
  const size_t p = asset.size() - 3;
  return std::tolower(static_cast<unsigned char>(asset[p])) == 'p' &&
         std::tolower(static_cast<unsigned char>(asset[p + 1])) == 't' &&
         std::tolower(static_cast<unsigned char>(asset[p + 2])) == 'x';
}

// Scale both edges by `ratio`, never below 1 texel.
void ScaledExtent(const DecodedImage& img, double ratio, uint32_t* w,
                  uint32_t* h) {
  *w = std::max(1u, static_cast<uint32_t>(std::floor(img.width * ratio)));
  *h = std::max(1u, static_cast<uint32_t>(std::floor(img.height * ratio)));
}

}  // namespace

const std::vector<size_t>* TextureDecoder::UsdzCandidates(
    const std::string& asset) const {
  if (!options_.usdz) return nullptr;
  if (!usdz_index_built_) {
    usdz_index_built_ = true;
    const size_t n = options_.usdz->NumEntries();
    usdz_by_base_.reserve(n);
    for (size_t i = 0; i < n; ++i) {
      usdz_by_base_[UsdzAssetBaseKey(options_.usdz->EntryName(i))].push_back(i);
    }
  }
  auto it = usdz_by_base_.find(UsdzAssetBaseKey(asset));
  return it == usdz_by_base_.end() ? nullptr : &it->second;
}

bool TextureDecoder::ReadAssetBytes(const std::string& asset,
                                   std::vector<uint8_t>* out) const {
  return ReadSourceBytes(options_, asset, UsdzCandidates(asset), out);
}

bool TextureDecoder::AttachBudgetLease(DecodedImage* image, bool srgb) {
  if (!image) return false;
  const uint64_t max_size =
      static_cast<uint64_t>((std::numeric_limits<size_t>::max)());
  uint64_t bytes = image->byte_size();
  if (bytes > max_size) return false;
  if (budget_state_->try_add(bytes)) {
    image->budget_lease =
        std::make_shared<::lightusd::next::TextureBudgetLease>(
            budget_state_, bytes);
    return true;
  }

  const uint64_t resident =
      budget_state_->resident.load(std::memory_order_relaxed);
  const uint64_t limit = budget_state_->get_limit();
  if (limit == 0 || bytes == 0) return false;
  // Already at or past the cap: with a resolution floor configured the image is
  // still admitted (shrunk to the floor) rather than dropped, so a scene that
  // crosses the budget mid-decode goes blurry instead of losing textures.
  const uint64_t remaining = resident >= limit ? 0 : limit - resident;
  if (remaining == 0 && options_.min_edge == 0) return false;
  const double ratio =
      remaining == 0 ? 0.0 : std::sqrt(double(remaining) / double(bytes));
  uint32_t w = 0;
  uint32_t h = 0;
  ScaledExtent(*image, std::min(1.0, ratio), &w, &h);
  if (options_.min_edge > 0) {
    // Never shrink below the floor; ScaledExtent can round a small ratio down
    // to a degenerate extent.
    const uint32_t longest = (std::max)(image->width, image->height);
    const uint32_t floor_edge = (std::min)(options_.min_edge, longest);
    if ((std::max)(w, h) < floor_edge) {
      const double up = double(floor_edge) / double((std::max)(longest, 1u));
      ScaledExtent(*image, (std::min)(1.0, up), &w, &h);
    }
  }
  if (w == 0 || h == 0) return false;
  if (!ResizeDecoded(image, w, h, srgb)) return false;
  ++downscaled_;
  bytes = image->byte_size();
  if (!budget_state_->try_add(bytes)) {
    if (options_.min_edge == 0) return false;
    // Soft budget: admit it over the cap rather than lose the texture.
    budget_state_->resident.fetch_add(bytes, std::memory_order_relaxed);
  }
  image->budget_lease =
      std::make_shared<::lightusd::next::TextureBudgetLease>(
          budget_state_, bytes);
  return true;
}

bool TextureDecoder::Decode(const std::string& asset, bool srgb,
                            DecodedImage* out) {
  if (!out) return false;

  ::lightusd::Image src;
  if (!LoadSourceImage(options_, asset, UsdzCandidates(asset), &src)) return false;
  if (!NarrowTo8Bit(&src)) return false;

  DecodedImage img;
  if (!ToDecoded(&src, options_.force_rgba, &img)) return false;
  src.data.clear();
  src.data.shrink_to_fit();

  // Size cap first: it is the cheap, predictable lever, and shrinking here also
  // shrinks what the budget below has to account for.
  if (options_.max_edge > 0) {
    const uint32_t longest = std::max(img.width, img.height);
    if (longest > options_.max_edge) {
      const double ratio = double(options_.max_edge) / double(longest);
      uint32_t w = 0, h = 0;
      ScaledExtent(img, ratio, &w, &h);
      if (ResizeDecoded(&img, w, h, srgb)) ++downscaled_;
    }
  }

  if (!AttachBudgetLease(&img, srgb)) return false;
  *out = std::move(img);
  return true;
}

bool TextureDecoder::DecodePtexFace(const std::string& asset, uint32_t face,
                                    uint32_t level, bool srgb, DecodedImage* out) {
  if (!out || !HasPtxExtension(asset)) return false;
  const std::string key = asset + "#" + std::to_string(face) + ":" +
                          std::to_string(level) + (srgb ? ":s" : ":l");
  auto cached = ptex_cache_.find(key);
  if (cached != ptex_cache_.end()) {
    ptex_lru_.splice(ptex_lru_.end(), ptex_lru_, cached->second.lru);
    *out = cached->second.image;
    return true;
  }

  std::vector<uint8_t> source;
  if (!ReadAssetBytes(asset, &source)) return false;
  ::lightusd::ptx::Reader reader;
  std::string err;
  if (!::lightusd::ptx::Reader::OpenMemory(source.data(), source.size(),
                                           &reader, &err)) return false;
  ::lightusd::ptx::FaceImage faceImage;
  const uint64_t configured_budget = options_.max_source_bytes
                                         ? options_.max_source_bytes
                                         : options_.budget_bytes;
  const size_t budget = configured_budget == 0
                            ? size_t(256ull * 1024ull * 1024ull)
                            : static_cast<size_t>(std::min<uint64_t>(
                                  configured_budget,
                                  static_cast<uint64_t>((std::numeric_limits<size_t>::max)())));
  if (!reader.ReadFace(face, level, budget, &faceImage, &err) ||
      faceImage.dataType != ::lightusd::ptx::DataType::UInt8 ||
      faceImage.channels == 0 || faceImage.channels > 4)
    return false;
  size_t pixels = 0;
  size_t samples = 0;
  size_t rgba_bytes = 0;
  if (!safe::mul(static_cast<size_t>(faceImage.width),
                 static_cast<size_t>(faceImage.height), &pixels) ||
      !safe::mul(pixels, static_cast<size_t>(faceImage.channels), &samples) ||
      faceImage.data.size() < samples || !safe::mul(pixels, size_t{4},
                                                     &rgba_bytes)) {
    return false;
  }
  out->width = faceImage.width;
  out->height = faceImage.height;
  out->channels = 4;
  out->pixels.assign(rgba_bytes, 255);
  for (size_t i = 0; i < pixels; ++i) {
    const uint8_t* s = faceImage.data.data() + i * faceImage.channels;
    uint8_t* d = out->pixels.data() + i * 4;
    d[0] = s[0];
    d[1] = faceImage.channels > 1 ? s[1] : s[0];
    d[2] = faceImage.channels > 2 ? s[2] : s[0];
    d[3] = faceImage.channels > 3 ? s[3] : 255;
  }
  if (options_.max_edge > 0 && std::max(out->width, out->height) > options_.max_edge) {
    const double ratio = double(options_.max_edge) /
                         double(std::max(out->width, out->height));
    uint32_t w = std::max(1u, static_cast<uint32_t>(std::floor(out->width * ratio)));
    uint32_t h = std::max(1u, static_cast<uint32_t>(std::floor(out->height * ratio)));
    if (!ResizeDecoded(out, w, h, srgb)) return false;
    ++downscaled_;
  }
  if (!AttachBudgetLease(out, srgb)) return false;

  // Keep a bounded LRU of decoded face pages. The source PTX bytes and the
  // temporary planar decode are released before returning, so RSS is bounded
  // by the page cache rather than by the complete texture set.
  const uint64_t cap = options_.budget_bytes > 0
                           ? std::min<uint64_t>(options_.budget_bytes,
                                               128ull * 1024ull * 1024ull)
                           : 64ull * 1024ull * 1024ull;
  if (cap > 0 && out->byte_size() <= cap) {
    while (!ptex_lru_.empty() && ptex_cache_bytes_ + out->byte_size() > cap) {
      const std::string& old = ptex_lru_.front();
      auto it = ptex_cache_.find(old);
      if (it != ptex_cache_.end()) {
        ptex_cache_bytes_ -= it->second.image.byte_size();
        ptex_cache_.erase(it);
      }
      ptex_lru_.pop_front();
    }
    ptex_lru_.push_back(key);
    auto pos = std::prev(ptex_lru_.end());
    ptex_cache_bytes_ += out->byte_size();
    ptex_cache_.emplace(key, PtexCacheEntry{*out, pos});
  }
  return true;
}

std::string ReplaceUdimToken(const std::string& asset, int udim) {
  std::string out = asset;
  const size_t pos = out.find("<UDIM>");
  if (pos != std::string::npos) {
    out.replace(pos, 6, std::to_string(udim));
  }
  return out;
}

}  // namespace next
}  // namespace tydra
}  // namespace lightusd
