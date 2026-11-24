#include "texture-util.hh"
#include "image-convert.hh"
#include "core/safe-arithmetic.hh"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstring>

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Warray-bounds"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#endif

// stb_image_resize2 implementation define is in src/image-util.cc

#include "external/stb_image_resize2.h"

#ifdef __clang__
#pragma clang diagnostic pop
#endif

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif

namespace lightusd {
namespace tydra {

namespace {

uint8_t ScalarFactorToByte(float factor) {
	if (!std::isfinite(factor)) {
		factor = 0.0f;
	}
	factor = (std::max)(0.0f, (std::min)(1.0f, factor));
	return static_cast<uint8_t>(factor * 255.0f);
}

} // namespace

bool BuildOcclusionRoughnessMetallicTexture(
		const float occlusionFactor,
		const float roughnessFactor,
		const float metallicFactor,
		const std::vector<uint8_t> &occlusionImageData,
		const size_t occlusionImageWidth,
		const size_t occlusionImageHeight,
		const size_t occlusionImageChannels,
		const size_t occlusionChannel,
		const std::vector<uint8_t> &roughnessImageData,
		const size_t roughnessImageWidth,
		const size_t roughnessImageHeight,
		const size_t roughnessImageChannels,
		const size_t roughnessChannel,
		const std::vector<uint8_t> &metallicImageData,
		const size_t metallicImageWidth,
		const size_t metallicImageHeight,
		const size_t metallicImageChannels,
		const size_t metallicChannel,
		std::vector<uint8_t> &dst, // RGBA
    size_t &dstWidth,
    size_t &dstHeight)
{
	// Validate channel counts only for present images. Empty inputs are allowed
	// and use the scalar factors below.
	auto validate_channel = [](const std::vector<uint8_t> &data, size_t channels, size_t channel) -> bool {
		if (data.empty()) return true;
		if (channels < 1 || channels > 4) return false;
		return channel < channels;
	};
	if (!validate_channel(occlusionImageData, occlusionImageChannels, occlusionChannel) ||
	    !validate_channel(roughnessImageData, roughnessImageChannels, roughnessChannel) ||
	    !validate_channel(metallicImageData, metallicImageChannels, metallicChannel)) {
		dst.clear(); dstWidth = 0; dstHeight = 0; return false;
	}

	// Validate source data buffer sizes.
	auto validate_buf = [](const std::vector<uint8_t> &data, size_t w, size_t h, size_t ch) -> bool {
		if (data.empty()) return true;
		size_t expected;
		if (!safe::mul3(w, h, ch, &expected)) return false;
		return data.size() >= expected;
	};
	if (!validate_buf(occlusionImageData, occlusionImageWidth, occlusionImageHeight, occlusionImageChannels)) {
		dst.clear(); dstWidth = 0; dstHeight = 0; return false;
	}
	if (!validate_buf(roughnessImageData, roughnessImageWidth, roughnessImageHeight, roughnessImageChannels)) {
		dst.clear(); dstWidth = 0; dstHeight = 0; return false;
	}
	if (!validate_buf(metallicImageData, metallicImageWidth, metallicImageHeight, metallicImageChannels)) {
		dst.clear(); dstWidth = 0; dstHeight = 0; return false;
	}

	// Validate positive dimensions for non-empty images.
	if ((!occlusionImageData.empty() && (occlusionImageWidth == 0 || occlusionImageHeight == 0)) ||
	    (!roughnessImageData.empty() && (roughnessImageWidth == 0 || roughnessImageHeight == 0)) ||
	    (!metallicImageData.empty() && (metallicImageWidth == 0 || metallicImageHeight == 0))) {
		dst.clear(); dstWidth = 0; dstHeight = 0; return false;
	}

	size_t maxImageWidth = 1;
	size_t maxImageHeight = 1;
	if (!occlusionImageData.empty()) {
		maxImageWidth = (std::max)(maxImageWidth, occlusionImageWidth);
		maxImageHeight = (std::max)(maxImageHeight, occlusionImageHeight);
	}
	if (!roughnessImageData.empty()) {
		maxImageWidth = (std::max)(maxImageWidth,  roughnessImageWidth);
		maxImageHeight = (std::max)(maxImageHeight, roughnessImageHeight);
	}
	if (!metallicImageData.empty()) {
		maxImageWidth = (std::max)(maxImageWidth, metallicImageWidth);
		maxImageHeight = (std::max)(maxImageHeight, metallicImageHeight);
	}

	std::vector<uint8_t> occlusionBuf;
	std::vector<uint8_t> roughnessBuf;
	std::vector<uint8_t> metallicBuf;

	if (!occlusionImageData.empty()) {
		if ((maxImageWidth != occlusionImageWidth) || (maxImageHeight != occlusionImageHeight)) {
			stbir_pixel_layout layout;
			if (occlusionImageChannels == 1) {
				layout = STBIR_1CHANNEL;
			} else if (occlusionImageChannels == 2) {
				layout = STBIR_2CHANNEL;
			} else if (occlusionImageChannels == 3) {
				layout = STBIR_RGB;
			} else { // assume RGBA
				layout = STBIR_RGBA;
			}

			size_t resize_size;
			if (!safe::mul3(maxImageWidth, maxImageHeight, occlusionImageChannels, &resize_size)) {
				return false;
			}
			occlusionBuf.resize(resize_size);

			if (occlusionImageWidth > INT_MAX || occlusionImageHeight > INT_MAX ||
			    maxImageWidth > INT_MAX || maxImageHeight > INT_MAX) {
				return false;
			}
			if (!stbir_resize_uint8_linear(occlusionImageData.data(), int(occlusionImageWidth), int(occlusionImageHeight), 0, occlusionBuf.data(), int(maxImageWidth), int(maxImageHeight), 0, layout)) {
				return false;
			}
		} else {
			occlusionBuf = occlusionImageData;
		}
	} else {
		occlusionBuf = occlusionImageData;
	}

	if (!metallicImageData.empty()) {
		if ((maxImageWidth != metallicImageWidth) || (maxImageHeight != metallicImageHeight)) {
			stbir_pixel_layout layout;
			if (metallicImageChannels == 1) {
				layout = STBIR_1CHANNEL;
			} else if (metallicImageChannels == 2) {
				layout = STBIR_2CHANNEL;
			} else if (metallicImageChannels == 3) {
				layout = STBIR_RGB;
			} else { // assume RGBA
				layout = STBIR_RGBA;
			}

			size_t resize_size;
			if (!safe::mul3(maxImageWidth, maxImageHeight, metallicImageChannels, &resize_size)) {
				return false;
			}
			metallicBuf.resize(resize_size);

			if (metallicImageWidth > INT_MAX || metallicImageHeight > INT_MAX ||
			    maxImageWidth > INT_MAX || maxImageHeight > INT_MAX) {
				return false;
			}
			if (!stbir_resize_uint8_linear(metallicImageData.data(), int(metallicImageWidth), int(metallicImageHeight), 0, metallicBuf.data(), int(maxImageWidth), int(maxImageHeight), 0, layout)) {
				return false;
			}
		} else {
			metallicBuf = metallicImageData;
		}
	}

	if (!roughnessImageData.empty()) {
		if ((maxImageWidth != roughnessImageWidth) || (maxImageHeight != roughnessImageHeight)) {
			stbir_pixel_layout layout;
			if (roughnessImageChannels == 1) {
				layout = STBIR_1CHANNEL;
			} else if (roughnessImageChannels == 2) {
				layout = STBIR_2CHANNEL;
			} else if (roughnessImageChannels == 3) {
				layout = STBIR_RGB;
			} else { // assume RGBA
				layout = STBIR_RGBA;
			}

			size_t resize_size;
			if (!safe::mul3(maxImageWidth, maxImageHeight, roughnessImageChannels, &resize_size)) {
				return false;
			}
			roughnessBuf.resize(resize_size);

			if (roughnessImageWidth > INT_MAX || roughnessImageHeight > INT_MAX ||
			    maxImageWidth > INT_MAX || maxImageHeight > INT_MAX) {
				return false;
			}
			if (!stbir_resize_uint8_linear(roughnessImageData.data(), int(roughnessImageWidth), int(roughnessImageHeight), 0, roughnessBuf.data(), int(maxImageWidth), int(maxImageHeight), 0, layout)) {
				return false;
			}
		} else {
			roughnessBuf = roughnessImageData;
		}
	}

	uint8_t occlusionValue = ScalarFactorToByte(occlusionFactor);
	uint8_t metallicValue = ScalarFactorToByte(metallicFactor);
	uint8_t roughnessValue = ScalarFactorToByte(roughnessFactor);

	size_t resize_size;
	if (!safe::mul3(maxImageWidth, maxImageHeight, size_t(3), &resize_size)) {
		return false;
	}
	dst.resize(resize_size);

	size_t loop_bound;
	if (!safe::mul(maxImageWidth, maxImageHeight, &loop_bound)) {
		return false;
	}
	for (size_t i = 0; i < loop_bound; i++) {
		// Use the first component of texel when input is a texture.
		uint8_t r = occlusionBuf.size() ? occlusionBuf[i * occlusionImageChannels + occlusionChannel] : occlusionValue;
		uint8_t g = roughnessBuf.size() ? roughnessBuf[i * roughnessImageChannels + roughnessChannel] : roughnessValue;
		uint8_t b = metallicBuf.size() ? metallicBuf[i * metallicImageChannels + metallicChannel] : metallicValue;

		dst[3 * i + 0] = r;
		dst[3 * i + 1] = g;
		dst[3 * i + 2] = b;
	}

	dstWidth = maxImageWidth;
	dstHeight = maxImageHeight;

	return true;
}

namespace {

bool ValidateImageBuffer(const Image &im, std::string *err,
                         const char *prefix) {
  if (im.bpp != 8 || im.width <= 0 || im.height <= 0 || im.channels < 1 ||
      im.channels > 4) {
    if (err) (*err) = std::string(prefix) + ": referenced input must be a valid 8-bit image.";
    return false;
  }
  size_t expected;
  if (!safe::mul3(size_t(im.width), size_t(im.height), size_t(im.channels),
                  &expected)) {
    if (err) (*err) = std::string(prefix) + ": referenced input size overflow.";
    return false;
  }
  if (im.data.size() < expected) {
    if (err) (*err) = std::string(prefix) + ": referenced input buffer too small.";
    return false;
  }
  return true;
}

stbir_pixel_layout PixelLayoutFromChannels(int channels) {
  if (channels == 1) {
    return STBIR_1CHANNEL;
  } else if (channels == 2) {
    return STBIR_2CHANNEL;
  } else if (channels == 3) {
    return STBIR_RGB;
  } else if (channels == 4) {
    return STBIR_RGBA;
  }
  return STBIR_1CHANNEL;  // should not reach here (callers validate 1-4)
}

bool LooksSRGB(const std::string &colorspace) {
  std::string s;
  s.reserve(colorspace.size());
  for (char c : colorspace) {
    s.push_back(char(std::tolower(static_cast<unsigned char>(c))));
  }
  // "sRGB", "srgb", "sRGB - Texture", "sRGB-texture" etc.
  return s.find("srgb") != std::string::npos;
}

}  // namespace

bool ResizeImage(const Image &src, int dstWidth, int dstHeight, Image *dst,
                 ResizeFilter filter, std::string *err) {
  if (dst == nullptr) {
    if (err) (*err) = "ResizeImage: dst is null.";
    return false;
  }
  if (src.width <= 0 || src.height <= 0 || src.channels < 1 || src.channels > 4) {
    if (err) (*err) = "ResizeImage: invalid source image (channels must be 1-4).";
    return false;
  }
  // 8-bit (LDR), fp16 and fp32 (HDR, e.g. EXR) per-channel images are supported.
  const bool is_float =
      (src.bpp == 32) && (src.format == Image::PixelFormat::Float);
  const bool is_half =
      (src.bpp == 16) && (src.format == Image::PixelFormat::Float);
  const bool is_uint16=src.bpp==16 && src.format==Image::PixelFormat::UInt;
  if (src.bpp != 8 && !is_float && !is_half && !is_uint16) {
    if (err) {
      (*err) =
          "ResizeImage: only 8-bit, uint16, fp16 or fp32 per-channel images are "
          "supported.";
    }
    return false;
  }
  if (dstWidth <= 0 || dstHeight <= 0) {
    if (err) (*err) = "ResizeImage: invalid destination size.";
    return false;
  }
  // Cap destination dimensions to prevent excessive memory allocation.
  constexpr int kMaxResizeDimension = 16384;
  if (dstWidth > kMaxResizeDimension || dstHeight > kMaxResizeDimension) {
    if (err) (*err) = "ResizeImage: destination dimensions exceed 16384 limit.";
    return false;
  }

  const size_t bytes_per_channel = size_t(src.bpp / 8);

  // Validate source buffer size (in bytes).
  size_t src_elems;
  if (!safe::mul3(size_t(src.width), size_t(src.height), size_t(src.channels),
                  &src_elems)) {
    if (err) (*err) = "ResizeImage: source size overflow.";
    return false;
  }
  if(src_elems>SIZE_MAX/bytes_per_channel) {if(err)*err="ResizeImage: source byte size overflow";return false;}
  const size_t src_expected = src_elems * bytes_per_channel;
  if (src.data.size() < src_expected) {
    if (err) (*err) = "ResizeImage: source buffer too small.";
    return false;
  }

  Image out;
  out.width = dstWidth;
  out.height = dstHeight;
  out.channels = src.channels;
  out.bpp = src.bpp;
  out.format = src.format;
  out.colorspace = src.colorspace;
  out.uri = src.uri;

  size_t dst_elems;
  if (!safe::mul3(size_t(dstWidth), size_t(dstHeight), size_t(src.channels),
                  &dst_elems)) {
    if (err) (*err) = "ResizeImage: destination size overflow.";
    return false;
  }
  if(dst_elems>SIZE_MAX/bytes_per_channel) {if(err)*err="ResizeImage: destination byte size overflow";return false;}
  const size_t dst_size = dst_elems * bytes_per_channel;
  out.data.resize(dst_size);

  const stbir_pixel_layout layout = PixelLayoutFromChannels(src.channels);

  bool use_srgb = false;
  if (filter == ResizeFilter::SRGB) {
    use_srgb = true;
  } else if (filter == ResizeFilter::Auto) {
    use_srgb = LooksSRGB(src.colorspace);
  }

  // No-op fast path.
  if ((dstWidth == src.width) && (dstHeight == src.height)) {
    std::copy(src.data.begin(), src.data.begin() + std::ptrdiff_t(dst_size),
              out.data.begin());
    (*dst) = std::move(out);
    return true;
  }

  void *r = nullptr;
  if(is_uint16 && use_srgb) {
    Image linear=src;linear.bpp=32;linear.format=Image::PixelFormat::Float;
    linear.data.resize(src_elems*sizeof(float));
    for(size_t i=0;i<src_elems;++i) {
      uint16_t sample;std::memcpy(&sample,src.data.data()+i*2,2);
      float value=float(sample)/65535.0f;
      const size_t c=i%size_t(src.channels);
      const bool alpha=(src.channels==2 && c==1) || (src.channels==4 && c==3);
      if(!alpha)value=value<=0.04045f?value/12.92f:std::pow((value+0.055f)/1.055f,2.4f);
      std::memcpy(linear.data.data()+i*4,&value,4);
    }
    Image resized;
    if(!ResizeImage(linear,dstWidth,dstHeight,&resized,ResizeFilter::Linear,err))return false;
    for(size_t i=0;i<dst_elems;++i) {
      float value;std::memcpy(&value,resized.data.data()+i*4,4);
      const size_t c=i%size_t(src.channels);
      const bool alpha=(src.channels==2 && c==1) || (src.channels==4 && c==3);
      if(!alpha)value=value<=0.0031308f?12.92f*value:1.055f*std::pow(value,1.0f/2.4f)-0.055f;
      const uint16_t sample=uint16_t(std::lround(std::clamp(value,0.0f,1.0f)*65535));
      std::memcpy(out.data.data()+i*2,&sample,2);
    }
    *dst=std::move(out);return true;
  } else if(is_uint16) {
    r=stbir_resize(src.data.data(),src.width,src.height,0,out.data.data(),dstWidth,dstHeight,0,
                   layout,STBIR_TYPE_UINT16,STBIR_EDGE_CLAMP,STBIR_FILTER_DEFAULT);
  } else if (is_half) {
    // fp16 HDR resize in linear space (stbir handles half<->float internally).
    r = stbir_resize(src.data.data(), src.width, src.height, 0,
                     out.data.data(), dstWidth, dstHeight, 0, layout,
                     STBIR_TYPE_HALF_FLOAT, STBIR_EDGE_CLAMP,
                     STBIR_FILTER_DEFAULT);
  } else if (is_float) {
    // HDR/float resize is always done in linear space.
    r = stbir_resize_float_linear(
        reinterpret_cast<const float *>(src.data.data()), src.width, src.height,
        0, reinterpret_cast<float *>(out.data.data()), dstWidth, dstHeight, 0,
        layout);
  } else if (use_srgb) {
    r = stbir_resize_uint8_srgb(src.data.data(), src.width, src.height, 0,
                                out.data.data(), dstWidth, dstHeight, 0, layout);
  } else {
    r = stbir_resize_uint8_linear(src.data.data(), src.width, src.height, 0,
                                  out.data.data(), dstWidth, dstHeight, 0,
                                  layout);
  }
  if (r == nullptr) {
    if (err) (*err) = "ResizeImage: stb resize failed.";
    return false;
  }

  (*dst) = std::move(out);
  return true;
}

bool PackChannels(const std::vector<Image> &inputs, const ChannelPackSpec &spec,
                  Image *dst, std::string *err) {
  if (dst == nullptr) {
    if (err) (*err) = "PackChannels: dst is null.";
    return false;
  }
  if (spec.out_channels < 1 || spec.out_channels > 4) {
    if (err) (*err) = "PackChannels: out_channels must be 1-4.";
    return false;
  }

  const ChannelSource *slots[4] = {&spec.r, &spec.g, &spec.b, &spec.a};

  // Validate referenced inputs and determine output dims.
  int out_w = spec.out_width;
  int out_h = spec.out_height;
  for (int c = 0; c < spec.out_channels; c++) {
    const ChannelSource &cs = *slots[c];
    if (cs.input_index < 0) {
      continue;
    }
    if (size_t(cs.input_index) >= inputs.size()) {
      if (err) (*err) = "PackChannels: input_index out of range.";
      return false;
    }
    const Image &im = inputs[size_t(cs.input_index)];
    if (!ValidateImageBuffer(im, err, "PackChannels")) {
      return false;
    }
    if (cs.channel < 0 || cs.channel >= im.channels) {
      if (err) (*err) = "PackChannels: channel index out of range for input.";
      return false;
    }
    if (out_w == 0) out_w = im.width;
    if (out_h == 0) out_h = im.height;
    out_w = (std::max)(out_w, im.width);
    out_h = (std::max)(out_h, im.height);
  }

  if (out_w <= 0 || out_h <= 0) {
    if (err) (*err) = "PackChannels: could not determine output size (no inputs and no explicit size).";
    return false;
  }
  constexpr int kMaxPackDimension = 16384;
  if (out_w > kMaxPackDimension || out_h > kMaxPackDimension) {
    if (err) (*err) = "PackChannels: output dimensions exceed 16384 limit.";
    return false;
  }

  // Resize each referenced input to (out_w, out_h) once and cache by index.
  std::vector<Image> resized(inputs.size());
  std::vector<bool> have_resized(inputs.size(), false);
  for (int c = 0; c < spec.out_channels; c++) {
    const ChannelSource &cs = *slots[c];
    if (cs.input_index < 0) continue;
    size_t idx = size_t(cs.input_index);
    if (have_resized[idx]) continue;
    const Image &im = inputs[idx];
    if (im.width == out_w && im.height == out_h) {
      resized[idx] = im;
    } else {
      // Data maps: resize in linear space to avoid gamma shifting data values.
      if (!ResizeImage(im, out_w, out_h, &resized[idx], ResizeFilter::Linear,
                       err)) {
        return false;
      }
    }
    have_resized[idx] = true;
  }

  Image out;
  out.width = out_w;
  out.height = out_h;
  out.channels = spec.out_channels;
  out.bpp = 8;
  out.format = Image::PixelFormat::UInt;

  size_t out_size;
  if (!safe::mul3(size_t(out_w), size_t(out_h), size_t(spec.out_channels),
                  &out_size)) {
    if (err) (*err) = "PackChannels: output size overflow.";
    return false;
  }
  constexpr size_t kMaxPackBytes = size_t(256) * 1024 * 1024;
  if (out_size > kMaxPackBytes) {
    if (err) (*err) = "PackChannels: output image exceeds 256 MiB limit.";
    return false;
  }
  out.data.resize(out_size);

  size_t npixels;
  if (!safe::mul(size_t(out_w), size_t(out_h), &npixels)) {
    if (err) (*err) = "PackChannels: pixel count overflow.";
    return false;
  }

  for (size_t i = 0; i < npixels; i++) {
    for (int c = 0; c < spec.out_channels; c++) {
      const ChannelSource &cs = *slots[c];
      uint8_t v = cs.constant;
      if (cs.input_index >= 0) {
        const Image &im = resized[size_t(cs.input_index)];
        v = im.data[i * size_t(im.channels) + size_t(cs.channel)];
      }
      out.data[i * size_t(spec.out_channels) + size_t(c)] = v;
    }
  }

  (*dst) = std::move(out);
  return true;
}

namespace {

// Encode one texture at a given dimension cap (Size strategy) or JPEG quality
// (Quality strategy). Returns the encoded bytes + final ext/dims.
bool EncodeFitTexture(const FitTextureInput &in, const FitTextureOptions &opts,
                      FitStrategy strategy, int dimCap, int quality,
                      FitTextureOutput *out) {
  // Validate source image data size.
  size_t expected;
  if (!safe::mul3(size_t(in.image.width), size_t(in.image.height), size_t(in.image.channels), &expected)) {
    return false;
  }
  if (in.image.data.size() < expected) {
    return false;
  }

  Image img = in.image;

  // Apply a longest-edge cap when requested.
  if (dimCap > 0) {
    const int longest = (std::max)(img.width, img.height);
    if (longest > dimCap) {
      const double s = double(dimCap) / double(longest);
      const int nw = (std::max)(1, int(img.width * s + 0.5));
      const int nh = (std::max)(1, int(img.height * s + 0.5));
      Image resized;
      std::string rerr;
      if (ResizeImage(img, nw, nh, &resized, ResizeFilter::Auto, &rerr)) {
        img = std::move(resized);
      }
      // If resize fails, continue with original (best-effort).
    }
  }

  image::WriteOption wopt;
  wopt.png_encoder = opts.png_encoder;

  std::string ext;
  std::string format;
  if(strategy==FitStrategy::Quality || in.ext=="jpg" || in.ext=="jpeg") {
    wopt.format=image::WriteImageFormat::JPEG;wopt.jpeg_quality=quality;
    format="jpeg";ext="jpg";
  } else if(in.ext=="exr" || in.ext=="hdr") {
    wopt.format=image::WriteImageFormat::EXR;format="exr";ext="exr";
  } else {wopt.format=image::WriteImageFormat::PNG;format="png";ext="png";}
  if(strategy==FitStrategy::Size)wopt.jpeg_quality=opts.jpeg_quality;
  auto prepared=image::PrepareImageForEncoding(std::move(img),format,size_t(512)<<20);
  if(!prepared)return false;
  img=std::move(*prepared);

  auto enc = image::WriteImageToMemory(img, wopt);
  if (!enc) {
    return false;
  }
  out->bytes = std::move(enc.value());
  out->ext = ext;
  out->width = img.width;
  out->height = img.height;
  out->changed = true;
  return true;
}

}  // namespace

bool FitTexturesToBudget(const std::vector<FitTextureInput> &inputs,
                         const FitTextureOptions &opts,
                         std::vector<FitTextureOutput> *out, std::string *warn,
                         std::string *err) {
  if (out == nullptr) {
    if (err) (*err) = "FitTexturesToBudget: out is null.";
    return false;
  }
  out->assign(inputs.size(), FitTextureOutput{});

  // Indices of textures we can shrink, and the fixed overhead of the rest.
  std::vector<size_t> idx;
  size_t fixed = 0;
  int maxOrigDim = 1;
  for (size_t i = 0; i < inputs.size(); i++) {
    const auto &in = inputs[i];
    // 8-bit textures can be shrunk by either strategy. fp32/EXR can only be
    // shrunk by resizing (size strategy); the quality strategy would transcode
    // to JPEG and discard the HDR data, so EXR is passed through there.
    const bool is8 = in.image.bpp == 8;
    const bool isFloatExr = in.image.bpp == 32 &&
                            in.image.format == Image::PixelFormat::Float &&
                            opts.strategy == FitStrategy::Size;
    if (in.reencodable && (is8 || isFloatExr) && in.image.width > 0 &&
        in.image.height > 0) {
      idx.push_back(i);
      maxOrigDim = (std::max)(maxOrigDim, (std::max)(in.image.width, in.image.height));
    } else {
      // Keep original bytes; count as fixed overhead.
      (*out)[i].bytes = in.original_bytes;
      (*out)[i].ext = in.ext;
      (*out)[i].width = in.image.width;
      (*out)[i].height = in.image.height;
      (*out)[i].changed = false;
      if (!safe::add(fixed, in.original_bytes.size(), &fixed)) {
        if (err) (*err) = "FitTexturesToBudget: fixed texture byte size overflow.";
        return false;
      }
    }
  }

  if (opts.start_max_size > 0) {
    maxOrigDim = (std::min)(maxOrigDim, opts.start_max_size);
  }

  const size_t budget =
      (opts.target_total_bytes > fixed) ? (opts.target_total_bytes - fixed) : 0;

  // Helper: encode all shrinkable textures at a knob, return total bytes.
  std::vector<FitTextureOutput> probe(idx.size());
  auto encodeAll = [&](int dimCap, int quality, size_t *total) -> bool {
    size_t sum = 0;
    for (size_t k = 0; k < idx.size(); k++) {
      if (!EncodeFitTexture(inputs[idx[k]], opts, opts.strategy, dimCap, quality,
                            &probe[k])) {
        return false;
      }
      if (!safe::add(sum, probe[k].bytes.size(), &sum)) {
        return false;
      }
    }
    *total = sum;
    return true;
  };

  // Binary search the lever for the best value that fits `budget`.
  // Size:    knob = dimension cap in [min_texture_size, maxOrigDim] (higher = bigger).
  // Quality: knob = JPEG quality in [min_jpeg_quality, 95]      (higher = bigger).
  int lo, hi, bestKnob;
  if (opts.strategy == FitStrategy::Quality) {
    lo = (std::max)(1, opts.min_jpeg_quality);
    hi = 95;
    bestKnob = lo;
  } else {
    lo = (std::max)(1, opts.min_texture_size);
    hi = (std::max)(lo, maxOrigDim);
    bestKnob = lo;
  }

  size_t total = 0;
  // If the largest setting already fits, take it (no shrink needed beyond cap).
  {
    int dimCap = (opts.strategy == FitStrategy::Quality) ? opts.start_max_size : hi;
    int quality = (opts.strategy == FitStrategy::Quality) ? hi : opts.jpeg_quality;
    if (!encodeAll(dimCap, quality, &total)) {
      if (err) (*err) = "FitTexturesToBudget: texture encode failed.";
      return false;
    }
    if (opts.target_total_bytes > 0 && fixed >= opts.target_total_bytes) {
      bestKnob = lo;
    } else if (budget == 0 || total <= budget) {
      bestKnob = hi;
    } else {
      // Search downward for the largest knob that fits.
      int a = lo, b = hi;
      bestKnob = lo;
      // Up to ~16 iterations (converges for any practical range).
      for (int it = 0; it < 16 && a <= b; it++) {
        int mid = a + (b - a) / 2;
        int dCap = (opts.strategy == FitStrategy::Quality) ? opts.start_max_size : mid;
        int q = (opts.strategy == FitStrategy::Quality) ? mid : opts.jpeg_quality;
        size_t t = 0;
        if (!encodeAll(dCap, q, &t)) {
          if (err) (*err) = "FitTexturesToBudget: texture encode failed.";
          return false;
        }
        if (t <= budget) {
          bestKnob = mid;  // fits; try larger
          a = mid + 1;
        } else {
          b = mid - 1;     // too big; go smaller
        }
      }
    }
  }

  // Final encode at the chosen knob and fill outputs.
  {
    int dimCap = (opts.strategy == FitStrategy::Quality) ? opts.start_max_size : bestKnob;
    int quality = (opts.strategy == FitStrategy::Quality) ? bestKnob : opts.jpeg_quality;
    if (!encodeAll(dimCap, quality, &total)) {
      if (err) (*err) = "FitTexturesToBudget: texture encode failed.";
      return false;
    }
    for (size_t k = 0; k < idx.size(); k++) {
      (*out)[idx[k]] = std::move(probe[k]);
    }
  }

  size_t final_total = 0;
  if (!safe::add(total, fixed, &final_total)) {
    if (err) (*err) = "FitTexturesToBudget: final texture byte size overflow.";
    return false;
  }
  if (opts.target_total_bytes > 0 && final_total > opts.target_total_bytes) {
    if (warn) {
      (*warn) += "Could not fit textures within the target budget (" +
                 std::to_string(opts.target_total_bytes) +
                 " bytes); using the smallest allowed setting (~" +
                 std::to_string(final_total) + " bytes).\n";
    }
  }

  return true;
}

} // namespace tydra
} // namespace lightusd
