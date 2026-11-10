// SPDX-License-Identifier: Apache-2.0
#include "texture_gpu.hh"

#include "image-loader.hh"
#include "io-util.hh"
#include "ptx-loader.hh"
#include "texcomp.h"
#include "tir.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace lusdview_texture_bench;

namespace {
constexpr int kSkip = 77;

struct Options {
  std::string root;
  std::string report;
  std::string backend{"vulkan"};
  std::string device;
  std::string formats{"bc1,bc3,bc5,bc6h,bc7,astc"};
  std::string filter{"mitchell"};
  int iterations{5};
  int warmup{2};
  int mips{1};
  bool synthetic{false};
  bool allowSoftware{false};
  bool srgb{true};
  bool metadataOnly{false};
  int maxImages{0};
  int maxDimension{0};
};

struct ImageInput {
  std::string path;
  uint32_t width{0}, height{0};
  std::vector<uint8_t> rgba;
  std::vector<float> rgbf;
  bool hdr{false};
  bool ptex{false};
  uint32_t ptexFace{0};
  uint32_t ptexLevel{0};
  double loadMs{0};
  uint64_t sourceBytes{0};
  uint64_t mappedBytes{0};
  uint64_t rssDeltaBytes{0};
};
struct Record { std::string path, format, status, error; double loadMs{0}, uploadMs{0}, resizeMs{0}, compressMs{0}, gpuMs{0}, totalMs{0}, psnr{0}; uint64_t sourceBytes{0}, mappedBytes{0}, rssDeltaBytes{0}; };

uint64_t CurrentRSSBytes() {
#if defined(__linux__)
  std::ifstream status("/proc/self/status");
  std::string key;
  uint64_t kb = 0;
  while (status >> key) {
    if (key == "VmRSS:") { status >> kb; return kb * 1024u; }
    std::string ignored;
    std::getline(status, ignored);
  }
#endif
  return 0;
}

bool IsAppleDouble(const fs::path& path) {
  const std::string name = path.filename().string();
  return name.size() > 2 && name[0] == '.' && name[1] == '_';
}

void Usage() {
  std::printf("Usage: lusdview_texture_gpu_bench --root DIR [options]\n"
              "       lusdview_texture_gpu_bench --synthetic [options]\n"
              "  --backend vulkan|cuda|hip|all   (default vulkan)\n"
              "  --format bc1,bc3,bc5,bc6h,bc7,astc (default all)\n"
              "  --filter bilinear|mitchell      (default mitchell)\n"
              "  --iterations N --warmup N       (default 5/2)\n"
              "  --mips N (0 = full chain to 1x1; default 1)\n"
              "  --max-images N (limit decoded corpus images; default unlimited)\n"
              "  --max-dimension N (cap GPU output edge for bounded real-data tests)\n"
              "  --metadata-only (query dimensions without decoding pixels)\n"
              "  PTEX inputs decode face 0/mip 0 as bounded representative pages\n"
              "  --report FILE --device NAME --linear --allow-software\n");
}

bool Parse(int argc, char** argv, Options* o) {
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto value = [&](const char* name, std::string* out) {
      if (a == name && i + 1 < argc) { *out = argv[++i]; return true; }
      return false;
    };
    if (a == "--help" || a == "-h") { Usage(); return false; }
    if (value("--root", &o->root) || value("--backend", &o->backend) ||
        value("--format", &o->formats) || value("--filter", &o->filter) ||
        value("--report", &o->report) || value("--device", &o->device)) continue;
    if (a == "--synthetic") o->synthetic = true;
    else if (a == "--metadata-only") o->metadataOnly = true;
    else if (a == "--allow-software") o->allowSoftware = true;
    else if (a == "--linear") o->srgb = false;
    else if (a == "--iterations" && i + 1 < argc) o->iterations = std::atoi(argv[++i]);
    else if (a == "--warmup" && i + 1 < argc) o->warmup = std::atoi(argv[++i]);
    else if (a == "--mips" && i + 1 < argc) o->mips = std::atoi(argv[++i]);
    else if (a == "--max-images" && i + 1 < argc) o->maxImages = std::atoi(argv[++i]);
    else if (a == "--max-dimension" && i + 1 < argc) o->maxDimension = std::atoi(argv[++i]);
    else { std::fprintf(stderr, "unknown option: %s\n", a.c_str()); return false; }
  }
  if (!o->synthetic && o->root.empty()) { std::fprintf(stderr, "--root or --synthetic is required\n"); return false; }
  if (o->filter != "bilinear" && o->filter != "mitchell") { std::fprintf(stderr, "invalid --filter\n"); return false; }
  if (o->mips < 0) { std::fprintf(stderr, "invalid --mips\n"); return false; }
  if (o->maxImages < 0) { std::fprintf(stderr, "invalid --max-images\n"); return false; }
  if (o->maxDimension < 0) { std::fprintf(stderr, "invalid --max-dimension\n"); return false; }
  return true;
}

int RunMetadataOnly(const Options& options) {
  std::vector<std::string> paths;
  if (fs::is_regular_file(options.root)) {
    paths.push_back(options.root);
  } else if (fs::is_directory(options.root)) {
    for (const auto& entry : fs::recursive_directory_iterator(options.root)) {
      if (!entry.is_regular_file()) continue;
      if (IsAppleDouble(entry.path())) continue;
      const std::string ext = entry.path().extension().string();
      if (ext == ".tif" || ext == ".tiff" || ext == ".tex") paths.push_back(entry.path().string());
    }
    std::sort(paths.begin(), paths.end());
    if (options.maxImages > 0 && static_cast<int>(paths.size()) > options.maxImages)
      paths.resize(static_cast<size_t>(options.maxImages));
  }
  if (paths.empty()) return kSkip;
  int failures = 0;
  for (const auto& path : paths) {
    const auto begin = std::chrono::steady_clock::now();
    auto info = lightusd::image::GetImageInfoFromFile(path);
    const auto end = std::chrono::steady_clock::now();
    if (!info) {
      ++failures;
      std::fprintf(stderr, "FAIL metadata %s: %s\n", path.c_str(), info.error().c_str());
      continue;
    }
    std::printf("metadata %s %ux%u c%u %.3f ms\n", path.c_str(), info->width,
                info->height, info->channels,
                std::chrono::duration<double, std::milli>(end - begin).count());
  }
  return failures ? 1 : 0;
}

std::vector<CompressionFormat> Formats(const std::string& s) {
  std::vector<CompressionFormat> out;
  size_t begin = 0;
  while (begin < s.size()) {
    size_t end = s.find(',', begin); if (end == std::string::npos) end = s.size();
    const std::string f = s.substr(begin, end - begin);
    if (f == "bc1") out.push_back(CompressionFormat::BC1);
    if (f == "bc3") out.push_back(CompressionFormat::BC3);
    if (f == "bc5") out.push_back(CompressionFormat::BC5);
    if (f == "bc6h") out.push_back(CompressionFormat::BC6H);
    if (f == "bc7") out.push_back(CompressionFormat::BC7);
    if (f == "astc") out.push_back(CompressionFormat::ASTC);
    begin = end + 1;
  }
  return out;
}

bool IsLdr(const lightusd::Image& img) {
  return img.width > 0 && img.height > 0 && (img.bpp == 8 || img.bpp == 16) &&
         img.format == lightusd::Image::PixelFormat::UInt &&
         (img.channels == 3 || img.channels == 4);
}

float HalfToFloat(uint16_t h) {
  const uint32_t sign = (h & 0x8000u) << 16;
  const uint32_t exp = (h >> 10) & 0x1fu;
  const uint32_t mant = h & 0x3ffu;
  uint32_t bits;
  if (exp == 0) {
    if (!mant) bits = sign;
    else { float v = std::ldexp(static_cast<float>(mant), -24); return (sign ? -v : v); }
  } else if (exp == 31) bits = sign | 0x7f800000u | (mant << 13);
  else bits = sign | ((exp + 112u) << 23) | (mant << 13);
  float value; std::memcpy(&value, &bits, sizeof(value)); return value;
}

uint8_t ToneMap(float v) {
  if (!std::isfinite(v) || v < 0.0f) v = 0.0f;
  const float mapped = v / (1.0f + v);
  return static_cast<uint8_t>(std::lround(std::pow(std::clamp(mapped, 0.0f, 1.0f), 1.0f / 2.2f) * 255.0f));
}

bool Load(const std::string& path, ImageInput* out) {
  auto loaded = lightusd::image::LoadImageFromFile(path);
  if (!loaded) { std::fprintf(stderr, "SKIP image %s: %s\n", path.c_str(), loaded.error().c_str()); return false; }
  const lightusd::Image& image = loaded->image;
  if (image.width <= 0 || image.height <= 0 || image.channels < 3 || image.channels > 4 ||
      (image.bpp != 8 && image.bpp != 16 && image.bpp != 32)) return false;
  const uint64_t pixelCount = uint64_t(image.width) * uint64_t(image.height);
  const size_t pixelBytes = size_t(image.channels) * size_t(image.bpp / 8);
  if (pixelCount > SIZE_MAX / 16 || pixelCount > image.data.size() / pixelBytes) return false;
  if (image.format == lightusd::Image::PixelFormat::Float && image.width > 0 && image.height > 0 && image.channels >= 3 && (image.bpp == 16 || image.bpp == 32)) {
    out->path = path; out->width = static_cast<uint32_t>(image.width); out->height = static_cast<uint32_t>(image.height); out->hdr = true;
    const size_t n = static_cast<size_t>(out->width) * out->height; out->rgbf.resize(n * 3u); out->rgba.resize(n * 4u, 255);
    for (size_t i = 0; i < n; ++i) for (int c = 0; c < 3; ++c) {
      float v = image.bpp == 32 ? reinterpret_cast<const float*>(image.data.data())[i * image.channels + c] : HalfToFloat(reinterpret_cast<const uint16_t*>(image.data.data())[i * image.channels + c]);
      out->rgbf[i * 3u + c] = v; out->rgba[i * 4u + c] = ToneMap(v);
    }
    return true;
  }
  if (!IsLdr(image)) return false;
  out->path = path; out->width = static_cast<uint32_t>(image.width); out->height = static_cast<uint32_t>(image.height);
  out->rgba.resize(static_cast<size_t>(out->width) * out->height * 4u);
  const auto channel = [&](size_t index) {
    if (image.bpp == 8) return image.data[index];
    uint16_t value = 0;
    std::memcpy(&value, image.data.data() + index * 2, sizeof(value));
    return static_cast<uint8_t>((uint32_t(value) + 128u) / 257u);
  };
  for (size_t i = 0; i < static_cast<size_t>(out->width) * out->height; ++i) {
    out->rgba[i*4+0] = channel(i*size_t(image.channels)+0);
    out->rgba[i*4+1] = channel(i*size_t(image.channels)+1);
    out->rgba[i*4+2] = channel(i*size_t(image.channels)+2);
    out->rgba[i*4+3] = image.channels == 4 ? channel(i*4+3) : 255;
  }
  return true;
}

bool LoadPtexFace(const std::string& path, uint32_t face, uint32_t level,
                  ImageInput* out) {
  lightusd::ptx::Reader reader;
  std::string error;
  if (!lightusd::ptx::Reader::OpenFile(path, &reader, &error)) {
    std::fprintf(stderr, "SKIP PTEX %s: %s\n", path.c_str(), error.c_str());
    return false;
  }
  lightusd::ptx::FaceImage page;
  if (!reader.ReadFace(face, level, size_t(256ull * 1024ull * 1024ull),
                       &page, &error)) {
    std::fprintf(stderr, "SKIP PTEX %s face %u mip %u: %s\n", path.c_str(),
                 face, level, error.c_str());
    return false;
  }
  if (page.channels == 0 || page.channels > 4 || page.width == 0 ||
      page.height == 0) return false;
  const size_t pixels = size_t(page.width) * page.height;
  out->path = path + "#face" + std::to_string(face) + ":mip" +
              std::to_string(level);
  out->width = page.width;
  out->height = page.height;
  out->ptex = true;
  out->ptexFace = face;
  out->ptexLevel = level;
  if (page.dataType == lightusd::ptx::DataType::UInt8 ||
      page.dataType == lightusd::ptx::DataType::UInt16) {
    const size_t bytesPerSample = page.dataType == lightusd::ptx::DataType::UInt16 ? 2u : 1u;
    if (page.data.size() < pixels * page.channels * bytesPerSample) return false;
    out->rgba.resize(pixels * 4u, 255);
    for (size_t i = 0; i < pixels; ++i) {
      for (size_t c = 0; c < 3; ++c) {
        const size_t src = std::min(c, size_t(page.channels - 1));
        uint32_t v = page.data[(i * page.channels + src) * bytesPerSample];
        if (bytesPerSample == 2) {
          uint16_t h = 0;
          std::memcpy(&h, page.data.data() + (i * page.channels + src) * 2u, 2u);
          v = (uint32_t(h) + 128u) / 257u;
        }
        out->rgba[i * 4u + c] = static_cast<uint8_t>(v);
      }
      if (page.channels == 4) {
        out->rgba[i * 4u + 3] = bytesPerSample == 1
            ? page.data[i * 4u + 3u]
            : static_cast<uint8_t>((uint32_t(*reinterpret_cast<const uint16_t*>(page.data.data() + (i * 4u + 3u) * 2u)) + 128u) / 257u);
      }
    }
    return true;
  }
  const size_t bytesPerSample = page.dataType == lightusd::ptx::DataType::Half ? 2u : 4u;
  if (page.data.size() < pixels * page.channels * bytesPerSample || page.channels < 3) return false;
  out->hdr = true;
  out->rgbf.resize(pixels * 3u);
  out->rgba.resize(pixels * 4u, 255);
  for (size_t i = 0; i < pixels; ++i) {
    for (size_t c = 0; c < 3; ++c) {
      float v = 0.0f;
      const uint8_t* p = page.data.data() + (i * page.channels + c) * bytesPerSample;
      if (bytesPerSample == 2) { uint16_t h = 0; std::memcpy(&h, p, 2u); v = HalfToFloat(h); }
      else std::memcpy(&v, p, sizeof(v));
      out->rgbf[i * 3u + c] = v;
      out->rgba[i * 4u + c] = ToneMap(v);
    }
  }
  return true;
}

bool LoadInput(const std::string& path, bool ptex, ImageInput* out) {
  const auto begin = std::chrono::steady_clock::now();
  const uint64_t rssBefore = CurrentRSSBytes();
  bool ok = ptex ? LoadPtexFace(path, 0, 0, out) : Load(path, out);
  const auto end = std::chrono::steady_clock::now();
  out->loadMs = std::chrono::duration<double, std::milli>(end - begin).count();
  out->sourceBytes = static_cast<uint64_t>(fs::file_size(path));
  out->mappedBytes = lightusd::io::IsMMapSupported() ? out->sourceBytes : 0;
  const uint64_t rssAfter = CurrentRSSBytes();
  out->rssDeltaBytes = rssAfter > rssBefore ? rssAfter - rssBefore : 0;
  return ok;
}

std::vector<ImageInput> Discover(const Options& o) {
  std::vector<ImageInput> out;
  if (o.synthetic) {
    for (const auto& dims : {std::pair<uint32_t,uint32_t>{3,5}, {127,73}, {512,257}}) {
      ImageInput image; image.path = "synthetic_" + std::to_string(dims.first) + "x" + std::to_string(dims.second);
      image.width=dims.first; image.height=dims.second; image.hdr=true;
      image.rgba.resize(size_t(image.width)*image.height*4u); image.rgbf.resize(size_t(image.width)*image.height*3u);
      for (uint32_t y=0;y<image.height;++y) for(uint32_t x=0;x<image.width;++x){size_t p=(size_t(y)*image.width+x)*4u; size_t f=(size_t(y)*image.width+x)*3u; float r=4.0f*float(x)/std::max(1u,image.width-1), g=3.0f*float(y)/std::max(1u,image.height-1), b=2.0f*float((x+y)&255u)/255.0f; image.rgbf[f]=r; image.rgbf[f+1]=g; image.rgbf[f+2]=b; image.rgba[p]=ToneMap(r); image.rgba[p+1]=ToneMap(g); image.rgba[p+2]=ToneMap(b); image.rgba[p+3]=uint8_t(64u+(x*191u)/std::max(1u,image.width-1));}
      out.push_back(std::move(image));
      if (o.maxImages > 0 && static_cast<int>(out.size()) >= o.maxImages) return out;
    }
    return out;
  }
  if (fs::is_regular_file(o.root)) {
    ImageInput image;
    std::string suffix = fs::path(o.root).extension().string();
    std::transform(suffix.begin(), suffix.end(), suffix.begin(), [](char c) {
      return char(std::tolower(static_cast<unsigned char>(c)));
    });
    const bool ok = LoadInput(o.root, suffix == ".ptx", &image);
    if (ok) out.push_back(std::move(image));
    return out;
  }
  if (!fs::is_directory(o.root)) return out;
  const std::set<std::string> ext = {".png",".jpg",".jpeg",".bmp",".tga",".ppm",".exr",".hdr",".tif",".tiff",".tex",".ptx"};
  std::vector<std::string> paths;
  for (const auto& entry : fs::recursive_directory_iterator(o.root)) {
    if (!entry.is_regular_file()) continue;
    if (IsAppleDouble(entry.path())) continue;
    std::string suffix = entry.path().extension().string(); std::transform(suffix.begin(),suffix.end(),suffix.begin(),[](char c){return char(std::tolower(static_cast<unsigned char>(c)));});
    if (ext.count(suffix)) {
      paths.push_back(entry.path().string());
    }
  }
  std::sort(paths.begin(), paths.end());
  for (const auto& path : paths) {
      ImageInput image;
      const bool isPtex = fs::path(path).extension().string() == ".ptx";
      const bool ok = LoadInput(path, isPtex, &image);
      if (ok) {
        out.push_back(std::move(image));
        if (o.maxImages > 0 && static_cast<int>(out.size()) >= o.maxImages) break;
      }
  }
  return out;
}

double PSNR(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
  if (a.size() != b.size() || a.empty()) return 0.0;
  double mse=0.0;
  for(size_t i=0;i<a.size();++i){double d=double(a[i])-double(b[i]);mse+=d*d;}
  mse/=double(a.size()); return mse < 1e-12 ? 99.0 : 10.0*std::log10(255.0*255.0/mse);
}

bool Decode(CompressionFormat format, const std::vector<uint8_t>& blocks, uint32_t w, uint32_t h, std::vector<uint8_t>* rgba) {
  rgba->resize(size_t(w)*h*4u); const size_t row=size_t(w)*4u; tc_result r=TC_ERROR_UNSUPPORTED;
  if(format==CompressionFormat::BC1)r=tc_bc1_decompress_rgba8(blocks.data(),w,h,row,rgba->data(),rgba->size());
  if(format==CompressionFormat::BC3)r=tc_bc3_decompress_rgba8(blocks.data(),w,h,row,rgba->data(),rgba->size());
  if(format==CompressionFormat::BC5)r=tc_bc5_decompress_rgba8(blocks.data(),w,h,0,row,rgba->data(),rgba->size());
  if(format==CompressionFormat::BC7)r=tc_bc7_decompress_rgba8(blocks.data(),w,h,row,rgba->data(),rgba->size());
  if(format==CompressionFormat::ASTC)r=tc_astc_decompress_rgba8(blocks.data(),w,h,4,4,rgba->data(),rgba->size());
  return r == TC_SUCCESS;
}

double BC6HPSNR(const ImageInput& image, const std::vector<uint8_t>& blocks, uint32_t w, uint32_t h) {
  std::vector<float> decoded(size_t(w) * h * 4u);
  if (tc_bc6h_decompress_rgbaf(blocks.data(), w, h, 0, size_t(w) * 4u * sizeof(float), decoded.data(), decoded.size() * sizeof(float)) != TC_SUCCESS) return 0.0;
  double mse = 0.0; size_t count = 0;
  for (uint32_t y = 0; y < h; ++y) for (uint32_t x = 0; x < w; ++x) {
    const uint32_t sx = std::min(image.width - 1u, (x * image.width) / w), sy = std::min(image.height - 1u, (y * image.height) / h);
    const size_t a = (size_t(sy) * image.width + sx) * 3u, b = (size_t(y) * w + x) * 4u;
    for (uint32_t c = 0; c < 3; ++c) { const double d = double(image.rgbf[a + c]) - decoded[b + c]; mse += d * d; ++count; }
  }
  mse /= std::max<size_t>(1, count); return mse < 1e-18 ? 99.0 : 10.0 * std::log10(1.0 / mse);
}

bool BackendSelected(const std::string& requested, Backend backend) {
  return requested == "all" || requested == BackendName(backend);
}

void WriteReport(const std::string& path, const DeviceInfo& info, const std::vector<Record>& records) {
  if(path.empty()) return;
  std::ofstream out(path);
  if(!out) return;
  out << "{\n  \"backend\": \"" << info.backend << "\",\n  \"device\": \"" << info.name << "\",\n  \"records\": [\n";
  for(size_t i=0;i<records.size();++i){const auto&r=records[i];out<<"    {\"path\": \""<<r.path<<"\", \"format\": \""<<r.format<<"\", \"status\": \""<<r.status<<"\", \"source_bytes\": "<<r.sourceBytes<<", \"mapped_source_bytes\": "<<r.mappedBytes<<", \"load_ms\": "<<r.loadMs<<", \"rss_delta_bytes\": "<<r.rssDeltaBytes<<", \"upload_ms\": "<<r.uploadMs<<", \"resize_gpu_ms\": "<<r.resizeMs<<", \"compress_gpu_ms\": "<<r.compressMs<<", \"gpu_ms\": "<<r.gpuMs<<", \"total_ms\": "<<r.totalMs<<", \"psnr_db\": "<<r.psnr<<"}"<<(i+1==records.size()?"":" ,")<<"\n";}
  out << "  ]\n}\n";
}

int RunBackend(const Options& options, Backend backend, const std::vector<ImageInput>& images,
               const std::vector<CompressionFormat>& formats) {
  if (!BackendSelected(options.backend, backend)) return 0;
  std::string error;
  auto processor = CreateProcessor(backend, options.allowSoftware, options.device, &error);
  if (!processor) { std::fprintf(stderr, "SKIP %s: %s\n", BackendName(backend), error.c_str()); return kSkip; }
  const DeviceInfo& info = processor->device();
  std::printf("%s: %s\n", info.backend.c_str(), info.name.c_str());
  std::vector<Record> records;
  int failures = 0;
  for (const auto& image : images) for (const auto format : formats) {
    if (format == CompressionFormat::BC6H && !info.supportsBC6H) {
      std::fprintf(stderr, "SKIP %s %s: format is not implemented by %s\n", image.path.c_str(), FormatName(format), info.backend.c_str());
      continue;
    }
    if (format == CompressionFormat::BC6H && !image.hdr) {
      std::fprintf(stderr, "SKIP %s %s: BC6H requires HDR input\n", image.path.c_str(), FormatName(format));
      continue;
    }
    int levelCount = options.mips;
    if (levelCount == 0) {
      levelCount = 1;
      uint32_t w = image.width, h = image.height;
      while (w > 1 || h > 1) { w = std::max(1u, w / 2u); h = std::max(1u, h / 2u); ++levelCount; }
    }
    std::vector<uint8_t> mipRGBA = image.rgba;
    std::vector<float> mipRGBF = image.rgbf;
    uint32_t mipSourceWidth = image.width, mipSourceHeight = image.height;
    uint32_t outputBaseWidth = image.width, outputBaseHeight = image.height;
    if (options.maxDimension > 0) {
      const uint32_t limit = static_cast<uint32_t>(options.maxDimension);
      const uint32_t edge = std::max(outputBaseWidth, outputBaseHeight);
      if (edge > limit) {
        const double scale = static_cast<double>(limit) / static_cast<double>(edge);
        outputBaseWidth = std::max(1u, static_cast<uint32_t>(outputBaseWidth * scale));
        outputBaseHeight = std::max(1u, static_cast<uint32_t>(outputBaseHeight * scale));
      }
    }
    for (int mip = 0; mip < levelCount; ++mip) {
      TextureRequest req;
      req.rgba = mipRGBA.data(); req.rgbaBytes = mipRGBA.size();
      req.rgbf = mipRGBF.data(); req.rgbfBytes = mipRGBF.size() * sizeof(float);
      req.width = mipSourceWidth; req.height = mipSourceHeight;
      const int mipShift = options.mips == 1 ? 1 : mip;
      req.dstWidth = std::max(1u, outputBaseWidth >> mipShift);
      req.dstHeight = std::max(1u, outputBaseHeight >> mipShift);
      req.srgb = options.srgb; req.filter = options.filter == "mitchell" ? ResizeFilter::Mitchell : ResizeFilter::Bilinear; req.format = format;
      req.downloadResized = true;
      // Repeated timing iterations restart from the original host source;
      // device chaining is used when one pass walks the complete mip chain.
      req.deviceMipChain = options.iterations == 1 && levelCount > 1 && mip + 1 < levelCount;
      for (int i = 0; i < options.warmup; ++i) { TextureRequest warmReq=req; warmReq.deviceMipChain=false; TextureResult warm; processor->process(warmReq, &warm, &error); }
      Record rec; rec.path = image.path; rec.format = std::string(FormatName(format)) + "@mip" + std::to_string(mip); rec.status = "pass";
      rec.loadMs = image.loadMs; rec.sourceBytes = image.sourceBytes; rec.mappedBytes = image.mappedBytes; rec.rssDeltaBytes = image.rssDeltaBytes;
      double uploadSum = 0.0, resizeSum = 0.0, compressSum = 0.0, totalSum = 0.0; bool ok = true;
      TextureResult result;
      for (int i = 0; i < options.iterations; ++i) {
        if (!processor->process(req, &result, &error)) { ok = false; break; }
        uploadSum += result.timing.uploadMs; resizeSum += result.timing.resizeGpuMs; compressSum += result.timing.compressGpuMs; totalSum += result.timing.totalMs;
      }
      if (ok) {
        std::vector<uint8_t> decoded;
        bool decodedOK = format == CompressionFormat::BC6H ? true : Decode(format, result.compressed, result.width, result.height, &decoded);
        rec.psnr = format == CompressionFormat::BC6H ? BC6HPSNR(image, result.compressed, result.width, result.height) : (decodedOK ? PSNR(result.resizedRGBA, decoded) : 0.0);
        const double invIterations = 1.0 / std::max(1, options.iterations);
        ok = decodedOK; rec.uploadMs = uploadSum * invIterations; rec.resizeMs = resizeSum * invIterations; rec.compressMs = compressSum * invIterations; rec.gpuMs = rec.resizeMs + rec.compressMs; rec.totalMs = totalSum * invIterations;
      }
      rec.status = ok ? "pass" : "fail";
      if (!ok) { ++failures; std::fprintf(stderr, "FAIL %s %s: %s\n", image.path.c_str(), rec.format.c_str(), error.c_str()); }
      else { std::printf("  %-10s %ux%u %.3f ms GPU %.2f dB\n", rec.format.c_str(), result.width, result.height, rec.gpuMs, rec.psnr); }
      records.push_back(std::move(rec));
      if (ok && format != CompressionFormat::BC6H && (req.deviceMipChain || !result.resizedRGBA.empty())) {
        if (!result.resizedRGBA.empty()) mipRGBA = result.resizedRGBA;
        mipSourceWidth = result.width;
        mipSourceHeight = result.height;
      }
      if (ok && format == CompressionFormat::BC6H && !result.resizedRGBF.empty()) {
        mipRGBF = result.resizedRGBF;
        mipSourceWidth = result.width;
        mipSourceHeight = result.height;
      }
    }
  }
  WriteReport(options.report, info, records);
  return failures ? 1 : 0;
}
}  // namespace

int main(int argc, char** argv) {
  Options options; if(!Parse(argc,argv,&options))return 1;
  if (options.metadataOnly) return RunMetadataOnly(options);
  const auto images=Discover(options); if(images.empty()){std::fprintf(stderr,"SKIP: no supported RGBA8 images found\n");return kSkip;}
  const auto formats=Formats(options.formats); if(formats.empty()){std::fprintf(stderr,"no valid compression formats\n");return 1;}
  int ran=0, result=0; for(const auto backend:{Backend::Vulkan,Backend::CUDA,Backend::HIP}){if(!BackendSelected(options.backend,backend))continue;int r=RunBackend(options,backend,images,formats);if(r!=kSkip)ran=1;if(r==1)result=1;}
  return ran?result:kSkip;
}
