// SPDX-License-Identifier: Apache-2.0
// Texture pipeline test: tir resize, texcomp block compression, texpipe mip
// chains, and the FinalizeDrawTextures usage classification, all through the
// lusdview wrapper (texture_tools.hh) + BuildDrawScene. Compiled with and
// without LUSDVIEW_WITH_TEXTOOLS; most checks skip in the OFF build.
#include "mesh_build.hh"
#include "renderer.hh"
#include "texture_tools.hh"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "tydra/render-data.hh"

#if defined(LUSDVIEW_WITH_TEXTOOLS)
#include "texcomp.h"  // tc_bc7_decompress_rgba8 (reference round-trip)
#include "texpipe.h"  // tp_ktx2_write_uni / tp_ktx2_read (KTX2 fixtures)
#include "image-loader.hh"  // image::LoadImageFromMemory (core KTX2 decode)
#endif

namespace {

namespace tydra = lightusd::tydra;

int g_fail = 0;

#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
      ++g_fail;                                                        \
    }                                                                  \
  } while (0)

light3d::Image MakeImage(int w, int h,
                         void (*gen)(int, int, uint8_t*)) {
  light3d::Image img;
  img.width = w;
  img.height = h;
  img.channels = 4;
  img.data.resize(static_cast<size_t>(w) * h * 4);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      gen(x, y, img.data.data() + (static_cast<size_t>(y) * w + x) * 4);
    }
  }
  return img;
}

int AddImage(tydra::RenderScene* scene, tydra::ColorSpace colorSpace, int w,
             int h, const light3d::Image& src) {
  tydra::BufferData buf;
  buf.data = src.data;
  const int bufferId = static_cast<int>(scene->buffers.size());
  scene->buffers.push_back(std::move(buf));

  tydra::TextureImage img;
  img.buffer_id = bufferId;
  img.decoded = true;
  img.width = w;
  img.height = h;
  img.channels = 4;
  img.texelComponentType = tydra::ComponentType::UInt8;
  img.colorSpace = colorSpace;
  img.usdColorSpace = colorSpace;
  img.asset_identifier =
      "asset_" + std::to_string(scene->images.size()) + ".png";
  const int imageId = static_cast<int>(scene->images.size());
  scene->images.push_back(std::move(img));
  return imageId;
}

int AddTexture(tydra::RenderScene* scene, int imageId,
               tydra::UVTexture::Channel channel) {
  tydra::UVTexture tex;
  tex.prim_name = "tex" + std::to_string(scene->textures.size());
  tex.texture_image_id = imageId;
  tex.connectedOutputChannel = channel;
  tex.wrapS = tydra::UVTexture::WrapMode::REPEAT;
  tex.wrapT = tydra::UVTexture::WrapMode::REPEAT;
  tex.scale = {1.0f, 1.0f, 1.0f, 1.0f};
  tex.bias = {0.0f, 0.0f, 0.0f, 0.0f};
  const int texId = static_cast<int>(scene->textures.size());
  scene->textures.push_back(std::move(tex));
  return texId;
}

template <typename T>
int AddDecodedImage(tydra::RenderScene* scene, tydra::ColorSpace colorSpace,
                    int w, int h, int channels,
                    tydra::ComponentType componentType,
                    const std::vector<T>& pixels,
                    const char* assetIdentifier) {
  tydra::BufferData buf;
  buf.data.resize(pixels.size() * sizeof(T));
  std::memcpy(buf.data.data(), pixels.data(), buf.data.size());
  const int bufferId = static_cast<int>(scene->buffers.size());
  scene->buffers.push_back(std::move(buf));

  tydra::TextureImage img;
  img.buffer_id = bufferId;
  img.decoded = true;
  img.width = w;
  img.height = h;
  img.channels = channels;
  img.texelComponentType = componentType;
  img.colorSpace = colorSpace;
  img.usdColorSpace = colorSpace;
  img.asset_identifier = assetIdentifier;
  const int imageId = static_cast<int>(scene->images.size());
  scene->images.push_back(std::move(img));
  return imageId;
}

void TestSolidResize() {
  // A constant sRGB image must survive decode->filter->re-encode within 1 LSB.
  light3d::Image img = MakeImage(64, 64, [](int, int, uint8_t* px) {
    px[0] = 200; px[1] = 100; px[2] = 50; px[3] = 255;
  });
  std::string err;
  CHECK(lusdview::TexToolsResizeRGBA8(&img, 16, 16, /*srgb=*/true, &err));
  CHECK(img.width == 16 && img.height == 16);
  for (size_t i = 0; i < img.data.size(); i += 4) {
    CHECK(std::abs(int(img.data[i + 0]) - 200) <= 1);
    CHECK(std::abs(int(img.data[i + 1]) - 100) <= 1);
    CHECK(std::abs(int(img.data[i + 2]) - 50) <= 1);
    CHECK(img.data[i + 3] == 255);
    if (g_fail) break;
  }
}

void TestMipDims() {
  light3d::Image img = MakeImage(8, 4, [](int x, int y, uint8_t* px) {
    px[0] = uint8_t(x * 32); px[1] = uint8_t(y * 64); px[2] = 0; px[3] = 255;
  });
  lusdview::TexUsage usage;
  std::vector<light3d::Image> mips;
  CHECK(lusdview::TexToolsBuildMips(img, usage, &mips));
  CHECK(mips.size() == 3);  // 4x2, 2x1, 1x1
  if (mips.size() == 3) {
    CHECK(mips[0].width == 4 && mips[0].height == 2);
    CHECK(mips[1].width == 2 && mips[1].height == 1);
    CHECK(mips[2].width == 1 && mips[2].height == 1);
  }
}

void TestTextureRegionMipDimensions() {
  int w = 0, h = 0;
  CHECK(lusdview::TextureMipDimensions(7, 5, 4, 0, &w, &h) &&
        w == 7 && h == 5);
  CHECK(lusdview::TextureMipDimensions(7, 5, 4, 1, &w, &h) &&
        w == 3 && h == 2);
  CHECK(lusdview::TextureMipDimensions(7, 5, 4, 2, &w, &h) &&
        w == 1 && h == 1);
  CHECK(lusdview::TextureMipDimensions(7, 5, 4, 3, &w, &h) &&
        w == 1 && h == 1);
  CHECK(!lusdview::TextureMipDimensions(7, 5, 4, 4, &w, &h));
  CHECK(!lusdview::TextureMipDimensions(7, 5, 4, -1, &w, &h));
  CHECK(!lusdview::TextureMipDimensions(7, 5, 4, 1, nullptr, &h));
}

void TestSrgbCorrectMip() {
  // 2x2 half-black/half-white: the 1x1 level must average in LINEAR light
  // (=> sRGB byte ~188), not in sRGB bytes (=> ~128).
  light3d::Image img = MakeImage(2, 2, [](int x, int, uint8_t* px) {
    const uint8_t v = (x == 0) ? 0 : 255;
    px[0] = v; px[1] = v; px[2] = v; px[3] = 255;
  });
  lusdview::TexUsage usage;
  usage.srgb = true;
  std::vector<light3d::Image> mips;
  CHECK(lusdview::TexToolsBuildMips(img, usage, &mips));
  CHECK(mips.size() == 1);
  if (mips.size() == 1) {
    CHECK(mips[0].width == 1 && mips[0].height == 1);
    const int v = mips[0].data[0];
    CHECK(v > 170 && v < 200);  // linear avg 0.5 -> sRGB ~188
  }
}

void TestCompressedSizes() {
  // NPOT 7x5 image -> 2x2 blocks; BC1 8 B/block, BC3/BC7 16 B/block.
  light3d::Image img = MakeImage(7, 5, [](int x, int y, uint8_t* px) {
    px[0] = uint8_t(x * 36); px[1] = uint8_t(y * 51); px[2] = 128; px[3] = 255;
  });
  lusdview::DrawCompressedImageCPU c;
  CHECK(lusdview::TexToolsCompress(img, false, lusdview::DrawCompressedFormat::BC1, &c));
  CHECK(c.data.size() == 2 * 2 * 8);
  CHECK(lusdview::TexToolsCompress(img, false, lusdview::DrawCompressedFormat::BC3, &c));
  CHECK(c.data.size() == 2 * 2 * 16);
  CHECK(lusdview::TexToolsCompress(img, true, lusdview::DrawCompressedFormat::BC7, &c));
  CHECK(c.data.size() == 2 * 2 * 16);
}

#if defined(LUSDVIEW_WITH_TEXTOOLS)
void TestBc7Roundtrip() {
  light3d::Image img = MakeImage(16, 16, [](int x, int y, uint8_t* px) {
    px[0] = uint8_t(x * 16); px[1] = uint8_t(y * 16);
    px[2] = uint8_t((x + y) * 8); px[3] = 255;
  });
  lusdview::DrawCompressedImageCPU c;
  CHECK(lusdview::TexToolsCompress(img, false, lusdview::DrawCompressedFormat::BC7, &c));
  std::vector<uint8_t> back(img.data.size());
  CHECK(tc_bc7_decompress_rgba8(c.data.data(), 16, 16, 16 * 4, back.data(),
                                back.size()) == TC_SUCCESS);
  double mse = 0.0;
  for (size_t i = 0; i < back.size(); ++i) {
    const double d = double(back[i]) - double(img.data[i]);
    mse += d * d;
  }
  mse /= double(back.size());
  const double psnr = 10.0 * std::log10(255.0 * 255.0 / (mse > 0 ? mse : 1e-9));
  if (psnr <= 30.0) std::printf("BC7 roundtrip PSNR = %.2f dB\n", psnr);
  CHECK(psnr > 30.0);  // QUICKBC7 BC7 on this gradient measures ~34 dB
}
#endif

void TestAlphaCoverage() {
  // 16x16 with exactly half the texels opaque. Alpha-tested mips must keep
  // roughly half the texels above the cutoff instead of averaging to gray.
  light3d::Image img = MakeImage(16, 16, [](int x, int y, uint8_t* px) {
    const bool solid = ((x / 2) + (y / 2)) % 2 == 0;
    px[0] = 255; px[1] = 255; px[2] = 255;
    px[3] = solid ? 255 : 0;
  });
  lusdview::TexUsage usage;
  usage.alphaTested = true;
  usage.alphaCutoff = 0.5f;
  std::vector<light3d::Image> mips;
  CHECK(lusdview::TexToolsBuildMips(img, usage, &mips));
  CHECK(mips.size() == 4);
  if (mips.size() >= 2) {
    const light3d::Image& m = mips[1];  // 4x4
    int above = 0, total = m.width * m.height;
    for (int i = 0; i < total; ++i) {
      if (m.data[static_cast<size_t>(i) * 4 + 3] >= 128) ++above;
    }
    const float coverage = float(above) / float(total);
    CHECK(coverage > 0.3f && coverage < 0.7f);
  }
}

void TestPipelineClassification() {
  // Full BuildDrawScene: normal-map texture classified + mips built + BCn
  // payloads per level with a single format.
  tydra::RenderScene scene;
  light3d::Image base = MakeImage(16, 16, [](int x, int y, uint8_t* px) {
    px[0] = uint8_t(x * 16); px[1] = uint8_t(y * 16); px[2] = 200; px[3] = 255;
  });
  light3d::Image normal = MakeImage(16, 16, [](int, int, uint8_t* px) {
    px[0] = 128; px[1] = 128; px[2] = 255; px[3] = 255;
  });
  const int baseImage = AddImage(&scene, tydra::ColorSpace::sRGB_Texture, 16, 16, base);
  const int normalImage = AddImage(&scene, tydra::ColorSpace::Raw, 16, 16, normal);
  const int baseTex = AddTexture(&scene, baseImage, tydra::UVTexture::Channel::RGB);
  const int normalTex = AddTexture(&scene, normalImage, tydra::UVTexture::Channel::RGB);

  tydra::OpenPBRSurfaceShader shader;
  shader.base_color.value = {1.0f, 1.0f, 1.0f};
  shader.base_color.texture_id = baseTex;
  shader.normal.texture_id = normalTex;

  tydra::RenderMaterial material;
  material.name = "textured";
  material.openPBRShader = shader;
  material.computeMaterialTag();
  scene.materials.push_back(std::move(material));

  // Pass 1: UNCOMPRESSED, so the RGBA mip chains survive for content checks
  // (the compressed path frees them once the compressed chain is built).
  {
    lusdview::TextureRuntimeOptions opt;
    opt.generateMips = true;
    opt.compression = lusdview::TextureCompressionMode::Off;
    lusdview::DrawScene out;
    lusdview::BuildDrawScene(scene, &out, nullptr, nullptr, opt);
    CHECK(out.materials.size() == 1);
    if (out.materials.size() != 1) return;
    const lusdview::DrawMaterialCPU& m = out.materials[0];
    CHECK(m.baseColorTex >= 0 && m.normalTex >= 0);
    if (m.baseColorTex < 0 || m.normalTex < 0) return;
    const lusdview::DrawTextureCPU& bt =
        out.textures[static_cast<size_t>(m.baseColorTex)];
    const lusdview::DrawTextureCPU& nt =
        out.textures[static_cast<size_t>(m.normalTex)];
    CHECK(!bt.isNormalMap);
    CHECK(nt.isNormalMap);
    CHECK(m.baseColorSample.colorSpace == lusdview::DrawColorSpace::sRGB);
    CHECK(m.normalSample.colorSpace == lusdview::DrawColorSpace::Raw);
    CHECK(m.baseColorSample.wrapS == lusdview::WrapMode::Repeat);
    CHECK(m.normalSample.wrapT == lusdview::WrapMode::Repeat);
    if (!lusdview::TexToolsAvailable()) return;
    CHECK(bt.mipImages.size() == 4);  // 8,4,2,1
    CHECK(nt.mipImages.size() == 4);
    // Flat +Z normal map must stay flat at every level.
    for (const light3d::Image& mip : nt.mipImages) {
      CHECK(std::abs(int(mip.data[0]) - 128) <= 2);
      CHECK(std::abs(int(mip.data[1]) - 128) <= 2);
      CHECK(int(mip.data[2]) >= 252);
    }
  }
  if (!lusdview::TexToolsAvailable()) return;

  // Pass 2: COMPRESSED (BCn). Format choice must be usage-aware, the compressed
  // chain complete, and the now-dead RGBA chain freed (memory contract).
  {
    lusdview::TextureRuntimeOptions opt;
    opt.generateMips = true;
    opt.compression = lusdview::TextureCompressionMode::BCn;
    lusdview::DrawScene out;
    lusdview::BuildDrawScene(scene, &out, nullptr, nullptr, opt);
    CHECK(out.materials.size() == 1);
    if (out.materials.size() != 1) return;
    const lusdview::DrawMaterialCPU& m = out.materials[0];
    if (m.baseColorTex < 0 || m.normalTex < 0) return;
    const lusdview::DrawTextureCPU& bt =
        out.textures[static_cast<size_t>(m.baseColorTex)];
    const lusdview::DrawTextureCPU& nt =
        out.textures[static_cast<size_t>(m.normalTex)];
    // Compressed chain: one format, one payload per level (8,4,2,1).
    CHECK(bt.requestedCompressed);
    CHECK(bt.compressed.format != lusdview::DrawCompressedFormat::None);
    CHECK(bt.compressed.mips.size() == 4);
    int expectW = 8;
    for (size_t l = 0; l < bt.compressed.mips.size(); ++l) {
      CHECK(bt.compressed.mips[l].width == expectW);
      CHECK(!bt.compressed.mips[l].data.empty());
      expectW /= 2;
    }
    // The RGBA chain is an intermediate on this path; it must be FREED once the
    // compressed chain is built (it retained ~21 MB/4K texture of dead CPU RAM).
    CHECK(bt.mipImages.empty());
    CHECK(nt.mipImages.empty());
    // Block-format choice must be usage-aware: an OPAQUE base color under BCn
    // goes to BC1 (a 2-endpoint color line is fine for color), but a NORMAL map
    // must NOT -- BC1/BC3 cross-contaminate its independent X/Y. It goes to a
    // full-channel format (BC7 on this BC-capable default cap set) instead.
    // This is the T4 regression: normal maps used to be squeezed onto BC1.
    CHECK(bt.compressed.format == lusdview::DrawCompressedFormat::BC1);
    CHECK(nt.compressed.format != lusdview::DrawCompressedFormat::BC1);
    CHECK(nt.compressed.format != lusdview::DrawCompressedFormat::BC3);
    CHECK(nt.compressed.format == lusdview::DrawCompressedFormat::BC7);
  }
}

void TestDecodedFormatNormalization() {
  tydra::RenderScene scene;
  const int grayImage = AddDecodedImage<uint8_t>(
      &scene, tydra::ColorSpace::Raw, 2, 1, 1, tydra::ComponentType::UInt8,
      {32, 200}, "gray_u8.synthetic");
  const int rgb16Image = AddDecodedImage<uint16_t>(
      &scene, tydra::ColorSpace::Raw, 1, 1, 3, tydra::ComponentType::UInt16,
      {0, 32768, 65535}, "rgb_u16.synthetic");
  const int floatRgImage = AddDecodedImage<float>(
      &scene, tydra::ColorSpace::Raw, 1, 1, 2, tydra::ComponentType::Float,
      {0.25f, 0.5f}, "rg_f32.synthetic");
  const int halfRgbaImage = AddDecodedImage<uint16_t>(
      &scene, tydra::ColorSpace::Raw, 1, 1, 4, tydra::ComponentType::Half,
      {0x0000u, 0x3800u, 0x3c00u, 0x3400u}, "rgba_f16.synthetic");
  AddTexture(&scene, grayImage, tydra::UVTexture::Channel::RGB);
  AddTexture(&scene, rgb16Image, tydra::UVTexture::Channel::RGB);
  AddTexture(&scene, floatRgImage, tydra::UVTexture::Channel::RGB);
  AddTexture(&scene, halfRgbaImage, tydra::UVTexture::Channel::RGB);

  lusdview::DrawScene out;
  lusdview::BuildDrawScene(scene, &out);

  CHECK(out.textures.size() == 4);
  if (out.textures.size() != 4) return;
  const light3d::Image& gray = out.textures[0].image;
  CHECK(gray.width == 2 && gray.height == 1 && gray.channels == 4);
  CHECK(gray.data[0] == 32 && gray.data[1] == 32 &&
        gray.data[2] == 32 && gray.data[3] == 255);
  CHECK(gray.data[4] == 200 && gray.data[5] == 200 &&
        gray.data[6] == 200 && gray.data[7] == 255);

  const light3d::Image& rgb16 = out.textures[1].image;
  CHECK(rgb16.width == 1 && rgb16.height == 1 && rgb16.channels == 4);
  CHECK(rgb16.data[0] == 0);
  CHECK(std::abs(int(rgb16.data[1]) - 128) <= 1);
  CHECK(rgb16.data[2] == 255);
  CHECK(rgb16.data[3] == 255);

  const light3d::Image& floatRg = out.textures[2].image;
  CHECK(floatRg.width == 1 && floatRg.height == 1 && floatRg.channels == 4);
  CHECK(std::abs(int(floatRg.data[0]) - 64) <= 1);
  CHECK(std::abs(int(floatRg.data[1]) - 64) <= 1);
  CHECK(std::abs(int(floatRg.data[2]) - 64) <= 1);
  CHECK(std::abs(int(floatRg.data[3]) - 128) <= 1);

  const light3d::Image& halfRgba = out.textures[3].image;
  CHECK(halfRgba.width == 1 && halfRgba.height == 1 && halfRgba.channels == 4);
  CHECK(halfRgba.data[0] == 0);
  CHECK(std::abs(int(halfRgba.data[1]) - 128) <= 1);
  CHECK(halfRgba.data[2] == 255);
  CHECK(std::abs(int(halfRgba.data[3]) - 64) <= 1);
}

}  // namespace

#if defined(LUSDVIEW_WITH_TEXTOOLS)
// Kept-compressed KTX2 passthrough + core KTX2 load. Builds a uni KTX2 in memory
// and exercises: the core image loader's KTX2->RGBA8 decode, and the lusdview
// device-adaptive block adaption (uni -> ASTC byte-copy / BC7 transcode / RGBA8
// fallback) used by TryKeepCompressedTexture.
static double Psnr(const uint8_t* a, const uint8_t* b, size_t n) {
  double se = 0;
  for (size_t i = 0; i < n; ++i) {
    const double d = double(a[i]) - double(b[i]);
    se += d * d;
  }
  const double mse = se / double(n);
  return mse <= 0 ? 1e9 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

void TestKeepCompressedKtx2() {
  const uint32_t W = 64, H = 64;
  light3d::Image src = MakeImage(int(W), int(H), [](int x, int y, uint8_t* p) {
    p[0] = uint8_t(x * 4); p[1] = uint8_t(y * 4);
    p[2] = uint8_t(((x / 8) ^ (y / 8)) & 1 ? 220 : 40); p[3] = 255;
  });

  // RGBA8 -> uni -> KTX2 (single level).
  const size_t uniSize = tc_uni_compressed_size(W, H);
  std::vector<uint8_t> uni(uniSize);
  CHECK(tc_uni_compress_rgba8(src.data.data(), W, H, size_t(W) * 4u, uni.data(),
                              uniSize) == TC_SUCCESS);
  const uint8_t* levels[1] = {uni.data()};
  size_t sizes[1] = {uniSize};
  uint32_t lw[1] = {W}, lh[1] = {H};
  const size_t ktxSize = tp_ktx2_uni_size(sizes, 1);
  std::vector<uint8_t> ktx(ktxSize);
  CHECK(TP_OK(tp_ktx2_write_uni(levels, sizes, lw, lh, 1, ktx.data(), ktxSize,
                                nullptr)));

  // Core loader: KTX2 -> RGBA8.
  {
    auto r = lightusd::image::LoadImageFromMemory(ktx.data(), ktx.size(),
                                                  "mem://uni.ktx2");
    CHECK(bool(r));
    if (r) {
      const auto& im = r.value().image;
      CHECK(im.width == int(W) && im.height == int(H) && im.channels == 4);
      CHECK(im.data.size() == size_t(W) * H * 4u);
      CHECK(Psnr(src.data.data(), im.data.data(), im.data.size()) >= 25.0);
    }
  }

  // Reader reports the uni intermediate.
  {
    tp_ktx2_image k;
    CHECK(TP_OK(tp_ktx2_read(ktx.data(), ktx.size(), &k)));
    CHECK(k.is_uni && k.vk_format == 0u && k.width == W && k.height == H);
  }

  using lusdview::DrawCompressedFormat;
  const DrawCompressedFormat srcFmt = DrawCompressedFormat::ASTC_4x4;  // uni ~ astc

  // caps: BC only -> transcode uni to BC7.
  {
    lusdview::TextureCompressCaps caps; caps.bc = true;
    lusdview::DrawCompressedImageCPU comp; light3d::Image rgba;
    CHECK(lusdview::TexToolsAdaptCompressed(uni.data(), uniSize, true, srcFmt, W,
                                            H, caps, &comp, &rgba));
    CHECK(comp.format == DrawCompressedFormat::BC7 && !comp.data.empty());
    CHECK(comp.data.size() == tc_bc7_compressed_size(W, H));
    std::vector<uint8_t> dec(size_t(W) * H * 4u);
    CHECK(tc_bc7_decompress_rgba8(comp.data.data(), W, H, size_t(W) * 4u,
                                  dec.data(), dec.size()) == TC_SUCCESS);
    CHECK(Psnr(src.data.data(), dec.data(), dec.size()) >= 25.0);
  }

  // caps: ASTC -> byte-copy (uni blocks are valid ASTC 4x4).
  {
    lusdview::TextureCompressCaps caps; caps.astc = true;
    lusdview::DrawCompressedImageCPU comp; light3d::Image rgba;
    CHECK(lusdview::TexToolsAdaptCompressed(uni.data(), uniSize, true, srcFmt, W,
                                            H, caps, &comp, &rgba));
    CHECK(comp.format == DrawCompressedFormat::ASTC_4x4);
    CHECK(comp.data.size() == uniSize);  // exact byte-copy
  }

  // caps: none -> RGBA8 fallback (TextureCompressCaps defaults bc=true, so
  // explicitly clear every capability here).
  {
    lusdview::TextureCompressCaps caps;
    caps.bc = caps.astc = caps.etc2 = caps.bc5 = caps.bc6h = false;
    lusdview::DrawCompressedImageCPU comp; light3d::Image rgba;
    CHECK(lusdview::TexToolsAdaptCompressed(uni.data(), uniSize, true, srcFmt, W,
                                            H, caps, &comp, &rgba));
    CHECK(comp.data.empty());
    CHECK(rgba.width == int(W) && rgba.height == int(H) &&
          rgba.data.size() == size_t(W) * H * 4u);
    CHECK(Psnr(src.data.data(), rgba.data.data(), rgba.data.size()) >= 25.0);
  }

  // Per-level adaption (mip-chain carry): uni level -> BC7.
  {
    std::vector<uint8_t> lvl;
    CHECK(lusdview::TexToolsAdaptCompressedLevel(
        uni.data(), uniSize, true, srcFmt, DrawCompressedFormat::BC7, W, H, &lvl));
    CHECK(lvl.size() == tc_bc7_compressed_size(W, H));
  }
  std::printf("  keep-compressed KTX2 (core decode + adapt bc7/astc/rgba/level): ok\n");
}
#endif  // LUSDVIEW_WITH_TEXTOOLS

int main() {
  if (!lusdview::TexToolsAvailable()) {
    std::printf("textools disabled; running classification-only checks\n");
    TestPipelineClassification();
    TestDecodedFormatNormalization();
    if (g_fail) return 1;
    std::printf("OK (textools OFF)\n");
    return 0;
  }
  TestSolidResize();
  TestMipDims();
  TestTextureRegionMipDimensions();
  TestSrgbCorrectMip();
  TestCompressedSizes();
#if defined(LUSDVIEW_WITH_TEXTOOLS)
  TestBc7Roundtrip();
  TestKeepCompressedKtx2();
#endif
  TestAlphaCoverage();
  TestPipelineClassification();
  TestDecodedFormatNormalization();
  if (g_fail) {
    std::printf("%d check(s) FAILED\n", g_fail);
    return 1;
  }
  std::printf("OK\n");
  return 0;
}
