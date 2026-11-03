#include "scene_gpu.hh"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <type_traits>

namespace ld3d12 {
namespace {

bool Fail(std::string* err, const char* what, HRESULT hr) {
  if (err) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%s failed (HRESULT 0x%08lX)", what,
                  static_cast<unsigned long>(hr));
    *err = buf;
  }
  return false;
}

D3D12_RESOURCE_DESC BufferDesc(UINT64 bytes) {
  D3D12_RESOURCE_DESC rd = {};
  rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  rd.Width = bytes;
  rd.Height = 1;
  rd.DepthOrArraySize = 1;
  rd.MipLevels = 1;
  rd.SampleDesc.Count = 1;
  rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  return rd;
}

// Records uploads (buffers and textures) on one command list. Staging buffers
// stay alive in `staging_` until the list has been executed and waited on.
class Uploader {
 public:
  explicit Uploader(ID3D12Device* device) : device_(device) {}

  bool Begin(std::string* err) {
    HRESULT hr = device_->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator_));
    if (FAILED(hr)) return Fail(err, "CreateCommandAllocator(upload)", hr);
    hr = device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                    allocator_.Get(), nullptr,
                                    IID_PPV_ARGS(&list_));
    if (FAILED(hr)) return Fail(err, "CreateCommandList(upload)", hr);
    return true;
  }

  ComPtr<ID3D12Resource> Stage(UINT64 bytes, void** mapped, std::string* err) {
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    const D3D12_RESOURCE_DESC rd = BufferDesc(bytes);
    ComPtr<ID3D12Resource> stage;
    HRESULT hr = device_->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&stage));
    if (FAILED(hr)) {
      Fail(err, "CreateCommittedResource(upload)", hr);
      return nullptr;
    }
    const D3D12_RANGE none = {0, 0};
    hr = stage->Map(0, &none, mapped);
    if (FAILED(hr)) {
      Fail(err, "Map(staging)", hr);
      return nullptr;
    }
    staging_.push_back(stage);
    return stage;
  }

  // Default-heap buffer copied from `data`; ends in `final_state`.
  // `src_bytes` (default = bytes) is how much of `data` to copy when the
  // buffer is padded past the source size.
  bool Buffer(const void* data, UINT64 bytes, D3D12_RESOURCE_STATES final_state,
              ComPtr<ID3D12Resource>* out, std::string* err,
              UINT64 src_bytes = 0) {
    const D3D12_RESOURCE_DESC rd = BufferDesc(bytes);
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    HRESULT hr = device_->CreateCommittedResource(
        // Buffers always start in COMMON (the layer warns otherwise) and are
        // implicitly promoted to COPY_DEST by the first copy below.
        &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON,
        nullptr, IID_PPV_ARGS(out->GetAddressOf()));
    if (FAILED(hr)) return Fail(err, "CreateCommittedResource(default)", hr);
    void* mapped = nullptr;
    ComPtr<ID3D12Resource> stage = Stage(bytes, &mapped, err);
    if (!stage) return false;
    std::memset(mapped, 0, static_cast<size_t>(bytes));
    std::memcpy(mapped, data,
                static_cast<size_t>(src_bytes ? std::min(src_bytes, bytes) : bytes));
    stage->Unmap(0, nullptr);
    list_->CopyBufferRegion(out->Get(), 0, stage.Get(), 0, bytes);
    Transition(out->Get(), final_state);
    return true;
  }

  struct Level {
    const void* data;
    size_t bytes;
  };

  // Default-heap 2D texture with `levels.size()` mips copied from tightly
  // packed source rows (block rows for compressed formats).
  bool Texture(DXGI_FORMAT format, UINT width, UINT height,
               const std::vector<Level>& levels,
               ComPtr<ID3D12Resource>* out, std::string* err) {
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = width;
    rd.Height = height;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = static_cast<UINT16>(levels.size());
    rd.Format = format;
    rd.SampleDesc.Count = 1;
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    HRESULT hr = device_->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, IID_PPV_ARGS(out->GetAddressOf()));
    if (FAILED(hr)) return Fail(err, "CreateCommittedResource(texture)", hr);

    const UINT n = static_cast<UINT>(levels.size());
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(n);
    std::vector<UINT> rows(n);
    std::vector<UINT64> row_bytes(n);
    UINT64 total = 0;
    device_->GetCopyableFootprints(&rd, 0, n, 0, layouts.data(), rows.data(),
                                   row_bytes.data(), &total);
    for (UINT m = 0; m < n; ++m) {
      if (levels[m].bytes < row_bytes[m] * rows[m])
        return Fail(err, "texture level too small", E_INVALIDARG);
    }
    void* mapped = nullptr;
    ComPtr<ID3D12Resource> stage = Stage(total, &mapped, err);
    if (!stage) return false;
    for (UINT m = 0; m < n; ++m) {
      const uint8_t* src = static_cast<const uint8_t*>(levels[m].data);
      uint8_t* dst = static_cast<uint8_t*>(mapped) + layouts[m].Offset;
      for (UINT r = 0; r < rows[m]; ++r)
        std::memcpy(dst + static_cast<size_t>(r) * layouts[m].Footprint.RowPitch,
                    src + static_cast<size_t>(r) * row_bytes[m],
                    static_cast<size_t>(row_bytes[m]));
    }
    stage->Unmap(0, nullptr);
    for (UINT m = 0; m < n; ++m) {
      D3D12_TEXTURE_COPY_LOCATION dl = {};
      dl.pResource = out->Get();
      dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      dl.SubresourceIndex = m;
      D3D12_TEXTURE_COPY_LOCATION sl = {};
      sl.pResource = stage.Get();
      sl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      sl.PlacedFootprint = layouts[m];
      list_->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
    }
    Transition(out->Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return true;
  }

  // 2D texture array: `slices[s]` holds the `mips` levels of slice s, which
  // must all share one format and size. Subresource = slice * mips + mip.
  bool TextureArray(DXGI_FORMAT format, UINT width, UINT height, UINT mips,
                    const std::vector<const std::vector<Level>*>& slices,
                    ComPtr<ID3D12Resource>* out, std::string* err) {
    const UINT array_size = static_cast<UINT>(slices.size());
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = width;
    rd.Height = height;
    rd.DepthOrArraySize = static_cast<UINT16>(array_size);
    rd.MipLevels = static_cast<UINT16>(mips);
    rd.Format = format;
    rd.SampleDesc.Count = 1;
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    HRESULT hr = device_->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, IID_PPV_ARGS(out->GetAddressOf()));
    if (FAILED(hr)) return Fail(err, "CreateCommittedResource(texture array)", hr);

    const UINT n = mips * array_size;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(n);
    std::vector<UINT> rows(n);
    std::vector<UINT64> row_bytes(n);
    UINT64 total = 0;
    device_->GetCopyableFootprints(&rd, 0, n, 0, layouts.data(), rows.data(),
                                   row_bytes.data(), &total);
    for (UINT sl = 0; sl < array_size; ++sl)
      for (UINT m = 0; m < mips; ++m)
        if ((*slices[sl])[m].bytes < row_bytes[sl * mips + m] * rows[sl * mips + m])
          return Fail(err, "texture array level too small", E_INVALIDARG);
    void* mapped = nullptr;
    ComPtr<ID3D12Resource> stage = Stage(total, &mapped, err);
    if (!stage) return false;
    for (UINT sl = 0; sl < array_size; ++sl)
      for (UINT m = 0; m < mips; ++m) {
        const UINT i = sl * mips + m;
        const uint8_t* src = static_cast<const uint8_t*>((*slices[sl])[m].data);
        uint8_t* dst = static_cast<uint8_t*>(mapped) + layouts[i].Offset;
        for (UINT r = 0; r < rows[i]; ++r)
          std::memcpy(dst + static_cast<size_t>(r) * layouts[i].Footprint.RowPitch,
                      src + static_cast<size_t>(r) * row_bytes[i],
                      static_cast<size_t>(row_bytes[i]));
      }
    stage->Unmap(0, nullptr);
    for (UINT i = 0; i < n; ++i) {
      D3D12_TEXTURE_COPY_LOCATION dl = {};
      dl.pResource = out->Get();
      dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      dl.SubresourceIndex = i;
      D3D12_TEXTURE_COPY_LOCATION sl = {};
      sl.pResource = stage.Get();
      sl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      sl.PlacedFootprint = layouts[i];
      list_->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
    }
    Transition(out->Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return true;
  }

  bool Finish(Device& device, std::string* err) {
    if (!barriers_.empty())
      list_->ResourceBarrier(static_cast<UINT>(barriers_.size()),
                             barriers_.data());
    const HRESULT hr = list_->Close();
    if (FAILED(hr)) return Fail(err, "Close(upload)", hr);
    return device.ExecuteAndWait(list_.Get(), err);
  }

 private:
  void Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = after;
    barriers_.push_back(b);
  }

  ID3D12Device* device_;
  ComPtr<ID3D12CommandAllocator> allocator_;
  ComPtr<ID3D12GraphicsCommandList> list_;
  std::vector<ComPtr<ID3D12Resource>> staging_;
  std::vector<D3D12_RESOURCE_BARRIER> barriers_;
};

// Box-filter mip chain over interleaved 4-component pixels.
template <class T>
void BuildChain(const T* level0, UINT w, UINT h,
                std::vector<std::vector<uint8_t>>* chain,
                std::vector<UINT>* widths, std::vector<UINT>* heights) {
  chain->emplace_back(reinterpret_cast<const uint8_t*>(level0),
                      reinterpret_cast<const uint8_t*>(level0) +
                          size_t(w) * h * 4 * sizeof(T));
  widths->push_back(w);
  heights->push_back(h);
  while (w > 1 || h > 1) {
    const UINT nw = std::max(1u, w / 2), nh = std::max(1u, h / 2);
    std::vector<uint8_t> next(size_t(nw) * nh * 4 * sizeof(T));
    const T* src = reinterpret_cast<const T*>(chain->back().data());
    T* dst = reinterpret_cast<T*>(next.data());
    for (UINT y = 0; y < nh; ++y)
      for (UINT x = 0; x < nw; ++x) {
        const UINT x0 = std::min(2 * x, w - 1), x1 = std::min(2 * x + 1, w - 1);
        const UINT y0 = std::min(2 * y, h - 1), y1 = std::min(2 * y + 1, h - 1);
        for (int c = 0; c < 4; ++c) {
          const float s = float(src[(size_t(y0) * w + x0) * 4 + c]) +
                          float(src[(size_t(y0) * w + x1) * 4 + c]) +
                          float(src[(size_t(y1) * w + x0) * 4 + c]) +
                          float(src[(size_t(y1) * w + x1) * 4 + c]);
          dst[(size_t(y) * nw + x) * 4 + c] = static_cast<T>(
              std::is_floating_point<T>::value ? s * 0.25f : s * 0.25f + 0.5f);
        }
      }
    chain->push_back(std::move(next));
    w = nw;
    h = nh;
    widths->push_back(w);
    heights->push_back(h);
  }
}

float Clamp01(float v) { return std::min(1.0f, std::max(0.0f, v)); }

bool CompressedFormat(uint32_t f, bool srgb, DXGI_FORMAT* out) {
  switch (f) {
    case LMV_COMPRESSED_BC1:
      *out = srgb ? DXGI_FORMAT_BC1_UNORM_SRGB : DXGI_FORMAT_BC1_UNORM;
      return true;
    case LMV_COMPRESSED_BC3:
      *out = srgb ? DXGI_FORMAT_BC3_UNORM_SRGB : DXGI_FORMAT_BC3_UNORM;
      return true;
    case LMV_COMPRESSED_BC5:
      *out = DXGI_FORMAT_BC5_UNORM;
      return true;
    case LMV_COMPRESSED_BC6H:
      *out = DXGI_FORMAT_BC6H_UF16;
      return true;
    case LMV_COMPRESSED_BC7:
      *out = srgb ? DXGI_FORMAT_BC7_UNORM_SRGB : DXGI_FORMAT_BC7_UNORM;
      return true;
    default:
      return false;
  }
}

// An lmv image converted to what D3D12 can upload: a format, size and a full
// mip chain. `levels` points into `owned` (or the image's own BC blocks), so a
// PreparedImage must not be copied.
struct PreparedImage {
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  UINT width = 0, height = 0;
  std::vector<std::vector<uint8_t>> owned;
  std::vector<Uploader::Level> levels;
  PreparedImage() = default;
  PreparedImage(const PreparedImage&) = delete;
  PreparedImage& operator=(const PreparedImage&) = delete;
};

bool PrepareImage(const lmv_image_view& iv, PreparedImage* out) {
  if (iv.width == 0 || iv.height == 0) return false;
  out->width = iv.width;
  out->height = iv.height;
  std::vector<UINT> ws, hs;
  if (iv.compressed_format != 0) {
    if (!CompressedFormat(iv.compressed_format, iv.srgb != 0, &out->format) ||
        !iv.levels || iv.mip_levels == 0 || (iv.width % 4) || (iv.height % 4))
      return false;
    for (uint32_t m = 0; m < iv.mip_levels; ++m)
      out->levels.push_back({iv.levels[m].data, iv.levels[m].nbytes});
    return true;
  }
  if (iv.pixels && iv.is_float && (iv.channels == 3 || iv.channels == 4)) {
    const float* src = static_cast<const float*>(iv.pixels);
    std::vector<float> rgba(size_t(iv.width) * iv.height * 4, 1.0f);
    for (size_t i = 0; i < size_t(iv.width) * iv.height; ++i)
      for (uint32_t c = 0; c < iv.channels; ++c)
        rgba[i * 4 + c] = src[i * iv.channels + c];
    BuildChain<float>(rgba.data(), iv.width, iv.height, &out->owned, &ws, &hs);
    out->format = DXGI_FORMAT_R32G32B32A32_FLOAT;
  } else if (iv.pixels && !iv.is_float && iv.channels == 4) {
    BuildChain<uint8_t>(static_cast<const uint8_t*>(iv.pixels), iv.width,
                        iv.height, &out->owned, &ws, &hs);
    out->format = iv.srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
                          : DXGI_FORMAT_R8G8B8A8_UNORM;
  } else {
    return false;
  }
  for (const auto& c : out->owned) out->levels.push_back({c.data(), c.size()});
  return true;
}

// Texture images -> shader-visible SRV heap (bindless descriptor indices).
class ImageTable {
 public:
  ImageTable(Device& device, Uploader* up, const lmv_scene* scene,
             ID3D12DescriptorHeap* heap, std::vector<ComPtr<ID3D12Resource>>* tex)
      : device_(device), up_(up), scene_(scene), heap_(heap), tex_(tex),
        stride_(device.device()->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)),
        cache_(lmv_scene_image_count(scene), -2) {}

  // Descriptor 0 is a white texel so a stray index is harmless.
  bool AddWhite(std::string* err) {
    const uint8_t white[4] = {255, 255, 255, 255};
    return Add(DXGI_FORMAT_R8G8B8A8_UNORM, 1, 1, {{white, 4}}, err) == 0;
  }

  // Returns the descriptor index for an lmv image, uploading it on first use;
  // -1 when the image cannot be shown (it is then treated as unbound).
  int32_t Get(int32_t image_id, std::string* err) {
    if (image_id < 0 || static_cast<size_t>(image_id) >= cache_.size())
      return -1;
    int32_t& slot = cache_[static_cast<size_t>(image_id)];
    if (slot != -2) return slot;
    slot = -1;
    lmv_image_view iv = {};
    PreparedImage prep;
    if (lmv_scene_image(scene_, static_cast<uint32_t>(image_id), &iv) != LMV_OK ||
        !PrepareImage(iv, &prep))
      return slot;
    std::string local;
    const int idx = Add(prep.format, prep.width, prep.height, prep.levels, &local);
    if (idx < 0) {
      if (err && err->empty()) *err = local;
      return slot;
    }
    slot = idx;
    return slot;
  }

  // Uploads the UDIM tile images as one texture array (slice order = `tiles`
  // order) and returns its descriptor index, or -1 when the tiles are missing
  // or disagree on size, format or mip count.
  int32_t GetUdimArray(const std::vector<int32_t>& tile_images, std::string* err) {
    std::vector<std::unique_ptr<PreparedImage>> tiles;
    for (int32_t id : tile_images) {
      lmv_image_view iv = {};
      auto p = std::make_unique<PreparedImage>();
      if (id < 0 || lmv_scene_image(scene_, static_cast<uint32_t>(id), &iv) != LMV_OK ||
          !PrepareImage(iv, p.get()))
        return -1;
      if (!tiles.empty() &&
          (p->format != tiles[0]->format || p->width != tiles[0]->width ||
           p->height != tiles[0]->height ||
           p->levels.size() != tiles[0]->levels.size()))
        return -1;
      tiles.push_back(std::move(p));
    }
    if (tiles.empty()) return -1;
    std::vector<const std::vector<Uploader::Level>*> slices;
    for (const auto& t : tiles) slices.push_back(&t->levels);
    ComPtr<ID3D12Resource> res;
    std::string local;
    if (!up_->TextureArray(tiles[0]->format, tiles[0]->width, tiles[0]->height,
                           static_cast<UINT>(tiles[0]->levels.size()), slices,
                           &res, &local)) {
      if (err && err->empty()) *err = local;
      return -1;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.Format = tiles[0]->format;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2DArray.MipLevels = static_cast<UINT>(tiles[0]->levels.size());
    sv.Texture2DArray.ArraySize = static_cast<UINT>(tiles.size());
    D3D12_CPU_DESCRIPTOR_HANDLE h_cpu = heap_->GetCPUDescriptorHandleForHeapStart();
    h_cpu.ptr += static_cast<SIZE_T>(next_) * stride_;
    device_.device()->CreateShaderResourceView(res.Get(), &sv, h_cpu);
    tex_->push_back(res);
    return static_cast<int32_t>(next_++);
  }

  // Uploads a GGX-prefiltered environment mip chain (RGBA16F) and returns its
  // descriptor index, or -1.
  int32_t AddPrefilteredEnv(const PrefilteredEnv& env, std::string* err) {
    std::vector<Uploader::Level> levels;
    for (const auto& l : env.levels)
      levels.push_back({l.data(), l.size() * sizeof(uint16_t)});
    return Add(DXGI_FORMAT_R16G16B16A16_FLOAT, env.width, env.height, levels,
               err);
  }

  uint32_t count() const { return next_ - 1; }  // excluding the white texel

 private:
  int Add(DXGI_FORMAT format, UINT w, UINT h,
          const std::vector<Uploader::Level>& levels, std::string* err) {
    ComPtr<ID3D12Resource> res;
    if (!up_->Texture(format, w, h, levels, &res, err)) return -1;
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.Format = format;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = static_cast<UINT>(levels.size());
    D3D12_CPU_DESCRIPTOR_HANDLE h_cpu =
        heap_->GetCPUDescriptorHandleForHeapStart();
    h_cpu.ptr += static_cast<SIZE_T>(next_) * stride_;
    device_.device()->CreateShaderResourceView(res.Get(), &sv, h_cpu);
    tex_->push_back(res);
    return static_cast<int>(next_++);
  }

  Device& device_;
  Uploader* up_;
  const lmv_scene* scene_;
  ID3D12DescriptorHeap* heap_;
  std::vector<ComPtr<ID3D12Resource>>* tex_;
  UINT stride_;
  UINT next_ = 0;
  std::vector<int32_t> cache_;
};

}  // namespace

void SceneGPU::UpdateDeform(const lmv_scene* scene, UINT slot) {
  if (!deform_mapped) return;
  uint8_t* base = deform_mapped + UINT64(slot) * deform_slot_bytes;
  for (size_t i = 0; i < meshes.size(); ++i) {
    const MeshGPU& m = meshes[i];
    if (m.skinned) {
      uint32_t n = 0;
      const float* palette =
          lmv_scene_mesh_skin_matrices(scene, static_cast<uint32_t>(i), &n);
      if (palette && n)
        std::memcpy(base + m.palette_offset, palette,
                    size_t(std::min(n, m.joint_count)) * 64);
    }
    if (m.morph_targets > 0) {
      uint32_t n = 0;
      const float* w =
          lmv_scene_mesh_morph_weights(scene, static_cast<uint32_t>(i), &n);
      if (w && n)
        std::memcpy(base + m.weights_offset, w,
                    size_t(std::min(n, m.morph_targets)) * 4);
    }
  }
}

void SceneGPU::RefreshLights(const lmv_scene* scene, UINT slot) {
  lights = BuildLights(scene, &authored_lights);
  if (lights.size() > kMaxLights) lights.resize(kMaxLights);
  shadow_light = -1;
  for (size_t i = 0; i < lights.size(); ++i) {
    if (static_cast<int>(lights[i].position_type[3] + 0.5f) ==
        LMV_LIGHT_DIRECTIONAL) {
      shadow_light = static_cast<int32_t>(i);
      break;
    }
  }
  std::memcpy(light_ring_mapped + size_t(slot) * kMaxLights * sizeof(LightGPU),
              lights.data(), lights.size() * sizeof(LightGPU));
}

bool SceneGPU::Build(Device& device, const lmv_scene* scene,
                     std::string* err) {
  ID3D12Device* d3d = device.device();
  Uploader up(d3d);
  if (!up.Begin(err)) return false;

  // Shader-visible SRV heap: one white texel + at most one view per image.
  D3D12_DESCRIPTOR_HEAP_DESC hd = {};
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  // White texel + shadow map + one array view per UDIM texture + the images.
  uint32_t udim_textures = 0;
  for (uint32_t t = 0; t < lmv_scene_texture_count(scene); ++t) {
    lmv_texture_view tv = {};
    if (lmv_scene_texture(scene, t, &tv) == LMV_OK && tv.udim_tile_count > 0)
      ++udim_textures;
  }
  // (+1 for the prefiltered environment chain.)
  hd.NumDescriptors = lmv_scene_image_count(scene) + 3 + udim_textures;
  hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  HRESULT hr = d3d->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&srv_heap));
  if (FAILED(hr)) return Fail(err, "CreateDescriptorHeap(SRV)", hr);
  ImageTable images(device, &up, scene, srv_heap.Get(), &textures_);
  if (!images.AddWhite(err)) return false;

  // UDIM texture arrays and Ptex rect tables, indexed by texture id.
  struct TexAux {
    int32_t index = -1;  // SRV descriptor (array or atlas)
    int32_t aux = 0;     // LUT / rect-table offset
    int kind = kSlotKind2D;
  };
  std::vector<TexAux> tex_aux(lmv_scene_texture_count(scene));
  std::vector<int32_t> udim_lut;
  std::vector<float> ptex_rects;  // float4 entries
  bool has_ptex = false;
  for (uint32_t t = 0; t < tex_aux.size(); ++t) {
    lmv_texture_view tv = {};
    if (lmv_scene_texture(scene, t, &tv) != LMV_OK) continue;
    if (tv.udim_tile_count > 0 && tv.udim_tiles) {
      std::vector<int32_t> tile_images;
      for (uint32_t k = 0; k < tv.udim_tile_count; ++k)
        tile_images.push_back(tv.udim_tiles[k].image_id);
      const int32_t index = images.GetUdimArray(tile_images, err);
      if (index < 0) continue;
      const int32_t lut_offset = static_cast<int32_t>(udim_lut.size());
      udim_lut.resize(udim_lut.size() + 100, -1);
      for (uint32_t k = 0; k < tv.udim_tile_count; ++k) {
        const int32_t slot_in_lut = static_cast<int32_t>(tv.udim_tiles[k].udim) - 1001;
        if (slot_in_lut >= 0 && slot_in_lut < 100)
          udim_lut[static_cast<size_t>(lut_offset + slot_in_lut)] = static_cast<int32_t>(k);
      }
      tex_aux[t] = {index, lut_offset, kSlotKindUdim};
    } else if (tv.is_ptex && tv.ptex_rects && tv.ptex_face_count > 0) {
      const int32_t index = images.Get(tv.image_id, err);
      if (index < 0) continue;
      tex_aux[t] = {index, static_cast<int32_t>(ptex_rects.size() / 4), kSlotKindPtex};
      ptex_rects.insert(ptex_rects.end(),
                        {static_cast<float>(tv.ptex_face_count), 0.0f, 0.0f, 0.0f});
      for (uint32_t k = 0; k < tv.ptex_face_count; ++k)
        ptex_rects.insert(ptex_rects.end(),
                          {tv.ptex_rects[k].x, tv.ptex_rects[k].y,
                           tv.ptex_rects[k].w, tv.ptex_rects[k].h});
      has_ptex = true;
    }
  }
  if (!udim_lut.empty() &&
      !up.Buffer(udim_lut.data(), udim_lut.size() * sizeof(int32_t),
                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &udim_lut_buffer, err))
    return false;
  if (!ptex_rects.empty() &&
      !up.Buffer(ptex_rects.data(), ptex_rects.size() * sizeof(float),
                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &ptex_rect_buffer, err))
    return false;

  // Materials.
  const uint32_t material_count = lmv_scene_material_count(scene);
  materials.assign(material_count + 1, MaterialGPU());  // + fallback
  for (uint32_t i = 0; i < material_count; ++i) {
    lmv_material_view mv = {};
    if (lmv_scene_material(scene, i, &mv) != LMV_OK || !mv.pbr) continue;
    MaterialGPU& g = materials[i];
    const float* p = mv.pbr;
    for (int c = 0; c < 3; ++c) {
      g.base[c] = p[LMV_PBR_BASE_COLOR + c];
      g.emission[c] = p[LMV_PBR_EMISSION_COLOR + c];
    }
    g.base[3] = p[LMV_PBR_BASE_WEIGHT];
    g.emission[3] = p[LMV_PBR_EMISSION];
    g.params[0] = Clamp01(p[LMV_PBR_METALNESS]);
    g.params[1] = Clamp01(p[LMV_PBR_SPECULAR_ROUGHNESS]);
    g.params[2] = static_cast<float>(mv.alpha_mode);
    g.params[3] = mv.alpha_cutoff;
    g.extra[0] = mv.normal_scale != 0.0f ? mv.normal_scale : 1.0f;
    g.extra[1] = Clamp01(p[LMV_PBR_OPACITY]);
    g.extra[2] = mv.coat_normal_scale > 0.0f ? mv.coat_normal_scale : 1.0f;
    g.extra[3] = std::max(p[LMV_PBR_SPECULAR_IOR], 1.0f);
    for (int c = 0; c < 3; ++c) {
      g.specular[c] = p[LMV_PBR_SPECULAR_COLOR + c];
      g.coat[c] = p[LMV_PBR_COAT_COLOR + c];
      g.sheen[c] = p[LMV_PBR_SHEEN_COLOR + c];
      g.transmission[c] = p[LMV_PBR_TRANSMISSION_COLOR + c];
      g.subsurface[c] = p[LMV_PBR_SUBSURFACE_COLOR + c];
    }
    g.specular[3] = p[LMV_PBR_SPECULAR_WEIGHT];
    g.coat[3] = p[LMV_PBR_COAT_WEIGHT];
    g.sheen[3] = p[LMV_PBR_SHEEN_WEIGHT];
    g.transmission[3] = p[LMV_PBR_TRANSMISSION];
    g.subsurface[3] = p[LMV_PBR_SUBSURFACE];
    g.coat_params[0] = p[LMV_PBR_COAT_ROUGHNESS];
    g.coat_params[1] = std::max(p[LMV_PBR_COAT_IOR], 1.0f);
    g.coat_params[2] = p[LMV_PBR_SHEEN_ROUGHNESS];
    g.coat_params[3] = p[LMV_PBR_THIN_WALLED];
    // Packed OpenPBR slots without a named index in lmv.h (see tydra's
    // PackRealtimePbrMaterial): 15 transmission depth, 23 subsurface scale,
    // 48/49/50 thin-film weight/thickness/ior, 57 anisotropy, 67 coat-affects-
    // colour.
    g.misc[0] = p[LMV_PBR_TRANSMISSION_DEPTH];
    g.misc[1] = p[23];
    g.misc[2] = p[67];
    g.thin[0] = p[48];
    g.thin[1] = p[49];
    g.thin[2] = std::max(p[50], 1.0f);
    g.thin[3] = p[57];

    const int32_t tex_ids[kSlotCount] = {
        mv.tex_base_color, mv.tex_metallic, mv.tex_roughness,
        mv.tex_normal,     mv.tex_emission, mv.tex_opacity,
        mv.tex_coat_normal};
    for (int s = 0; s < kSlotCount; ++s) {
      if (tex_ids[s] < 0) continue;
      lmv_texture_view tv = {};
      if (lmv_scene_texture(scene, static_cast<uint32_t>(tex_ids[s]), &tv) !=
          LMV_OK)
        continue;
      const TexAux& ta = tex_aux[static_cast<size_t>(tex_ids[s])];
      int32_t index = ta.index;
      if (ta.kind == kSlotKind2D) {
        if (tv.udim_tile_count > 0 || tv.is_ptex) {  // tiles/faces failed to build
          ++unsupported_textures;
          continue;
        }
        index = images.Get(tv.image_id, err);
      }
      if (index < 0) {
        ++unsupported_textures;
        continue;
      }
      SlotGPU& slot = g.slots[s];
      slot.index = index;
      slot.aux = ta.aux;
      slot.flags = static_cast<int32_t>((tv.wrap_s & 15u) |
                                        ((tv.wrap_t & 15u) << 4) |
                                        ((tv.channel & 15u) << 8) |
                                        ((tv.uv_set & 15u) << 12) |
                                        (static_cast<uint32_t>(ta.kind) << kSlotKindShift));
      slot.rotation = tv.uv_rotation;
      for (int k = 0; k < 2; ++k) {
        slot.uv_os[k] = tv.uv_offset[k];
        slot.uv_os[2 + k] = tv.uv_scale[k];
      }
      std::memcpy(slot.bias, tv.bias, sizeof(slot.bias));
      std::memcpy(slot.scale, tv.scale, sizeof(slot.scale));
    }
  }
  // Lighting: authored lights or the studio rig, dome environment, SH9.
  {
    D3D12_HEAP_PROPERTIES lhp = {};
    lhp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC lrd = {};
    lrd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    lrd.Width = UINT64(kMaxLights) * sizeof(LightGPU) * kFrameCount;
    lrd.Height = 1;
    lrd.DepthOrArraySize = 1;
    lrd.MipLevels = 1;
    lrd.SampleDesc.Count = 1;
    lrd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    hr = d3d->CreateCommittedResource(
        &lhp, D3D12_HEAP_FLAG_NONE, &lrd, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&light_ring));
    if (FAILED(hr)) return Fail(err, "CreateCommittedResource(lights)", hr);
    const D3D12_RANGE none = {0, 0};
    void* mapped = nullptr;
    hr = light_ring->Map(0, &none, &mapped);
    if (FAILED(hr)) return Fail(err, "Map(lights)", hr);
    light_ring_mapped = static_cast<uint8_t*>(mapped);
  }
  for (UINT slot = 0; slot < kFrameCount; ++slot) RefreshLights(scene, slot);
  lmv_scene_working_to_display_linear(scene, color_matrix);
  lmv_scene_bounds(scene, bounds_min, bounds_max);
  const int32_t dome_image = FindDomeImage(scene);
  if (dome_image >= 0) {
    const int32_t index = images.Get(dome_image, err);
    if (index >= 0 &&
        ComputeEnvironmentSH(scene, dome_image, color_matrix, sh)) {
      env_index = index;
      PrefilteredEnv pre;
      std::string soft_err;  // a failed prefilter falls back to the raw image
      if (!std::getenv("LD3D12VIEW_NO_PREFILTER") &&
          BuildPrefilteredEnvironment(scene, dome_image, &pre))
        env_spec_index = images.AddPrefilteredEnv(pre, &soft_err);
    }
  }
  if (shadow_light >= 0) {
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = rd.Height = kShadowSize;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R32_TYPELESS;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_CLEAR_VALUE cv = {};
    cv.Format = DXGI_FORMAT_D32_FLOAT;
    cv.DepthStencil.Depth = 1.0f;
    hr = d3d->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &cv,
        IID_PPV_ARGS(&shadow_map));
    if (FAILED(hr)) return Fail(err, "CreateCommittedResource(shadow)", hr);
    D3D12_DESCRIPTOR_HEAP_DESC dh = {};
    dh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dh.NumDescriptors = 1;
    hr = d3d->CreateDescriptorHeap(&dh, IID_PPV_ARGS(&shadow_dsv_heap));
    if (FAILED(hr)) return Fail(err, "CreateDescriptorHeap(shadow DSV)", hr);
    D3D12_DEPTH_STENCIL_VIEW_DESC dv = {};
    dv.Format = DXGI_FORMAT_D32_FLOAT;
    dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    d3d->CreateDepthStencilView(
        shadow_map.Get(), &dv,
        shadow_dsv_heap->GetCPUDescriptorHandleForHeapStart());
    shadow_srv_index = static_cast<int32_t>(hd.NumDescriptors) - 1;
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.Format = DXGI_FORMAT_R32_FLOAT;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE h = srv_heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(shadow_srv_index) *
             d3d->GetDescriptorHandleIncrementSize(
                 D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    d3d->CreateShaderResourceView(shadow_map.Get(), &sv, h);
  }

  texture_count = images.count();
  if (!up.Buffer(materials.data(), materials.size() * sizeof(MaterialGPU),
                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &material_buffer,
                 err))
    return false;

  // Geometry.
  const uint32_t mesh_count = lmv_scene_mesh_count(scene);
  meshes.clear();
  meshes.resize(mesh_count);
  for (uint32_t i = 0; i < mesh_count; ++i) {
    lmv_mesh_view mv = {};
    if (lmv_scene_mesh(scene, i, &mv) != LMV_OK) continue;
    if (!mv.vertices || !mv.indices || mv.vertex_count == 0 ||
        mv.index_count == 0)
      continue;
    MeshGPU& m = meshes[i];
    const UINT64 vbytes = UINT64(mv.vertex_count) * sizeof(lmv_vertex);
    const UINT64 ibytes = UINT64(mv.index_count) * sizeof(uint32_t);
    if (!up.Buffer(mv.vertices, vbytes,
                   D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
                   &m.vertex_buffer, err) ||
        !up.Buffer(mv.indices, ibytes, D3D12_RESOURCE_STATE_INDEX_BUFFER,
                   &m.index_buffer, err))
      return false;
    m.vbv = {m.vertex_buffer->GetGPUVirtualAddress(),
             static_cast<UINT>(vbytes), sizeof(lmv_vertex)};
    m.ibv = {m.index_buffer->GetGPUVirtualAddress(), static_cast<UINT>(ibytes),
             DXGI_FORMAT_R32_UINT};
    m.index_count = mv.index_count;
    std::memcpy(m.bbox_min, mv.bbox_min, sizeof(m.bbox_min));
    std::memcpy(m.bbox_max, mv.bbox_max, sizeof(m.bbox_max));
    m.material_id = mv.material_id;
    if (has_ptex && mv.face_ids &&
        !up.Buffer(mv.face_ids, vbytes / sizeof(lmv_vertex) * sizeof(uint32_t),
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, &m.face_ids, err))
      return false;
    if (mv.skinned) {
      uint32_t ni = 0, nw = 0, nm = 0;
      const uint16_t* ji = lmv_scene_mesh_joint_indices(
          scene, i, &ni);
      const float* jw = lmv_scene_mesh_joint_weights(scene, i, &nw);
      lmv_scene_mesh_skin_matrices(scene, i, &nm);
      if (ji && jw && nm > 0 && ni >= mv.vertex_count * 4 &&
          nw >= mv.vertex_count * 4) {
        const UINT64 ibytes4 = (UINT64(mv.vertex_count) * 4 * 2 + 3) & ~3ull;
        if (!up.Buffer(ji, ibytes4 ? ibytes4 : 4,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       &m.joint_indices, err, ni * 2) ||
            !up.Buffer(jw, UINT64(mv.vertex_count) * 16,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       &m.joint_weights, err))
          return false;
        m.skinned = true;
        m.joint_count = nm;
      }
    }
    if (mv.morph_targets > 0) {
      const uint32_t targets = mv.morph_targets;
      std::vector<float> inter(size_t(mv.vertex_count) * targets * 3, 0.0f);
      for (uint32_t t = 0; t < targets; ++t) {
        uint32_t n = 0;
        const float* d = lmv_scene_mesh_morph_deltas(scene, i, t, &n);
        if (!d || n < mv.vertex_count * 3) continue;
        for (uint32_t v = 0; v < mv.vertex_count; ++v)
          for (int c = 0; c < 3; ++c)
            inter[(size_t(v) * targets + t) * 3 + c] = d[v * 3 + c];
      }
      uint32_t nw = 0;
      const float* w = lmv_scene_mesh_morph_weights(scene, i, &nw);
      if (w && nw >= targets &&
          up.Buffer(inter.data(), inter.size() * 4,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    &m.morph_deltas, err))
        m.morph_targets = targets;
    }
    if (m.deforms()) {
      auto align = [](UINT64 v) {
        return (v + kDeformAlign - 1) & ~(kDeformAlign - 1);
      };
      if (m.skinned) {
        m.palette_offset = deform_slot_bytes;
        deform_slot_bytes += align(UINT64(m.joint_count) * 64);
      }
      if (m.morph_targets) {
        m.weights_offset = deform_slot_bytes;
        deform_slot_bytes += align(UINT64(m.morph_targets) * 4);
      }
    }
    if (mv.subsets && mv.subset_count)
      m.subsets.assign(mv.subsets, mv.subsets + mv.subset_count);
  }

  // Points and curves.
  static_assert(sizeof(lmv_splat) == 32, "lmv_splat layout changed");
  static_assert(sizeof(lmv_line_segment) == 44, "lmv_line_segment layout changed");
  const uint32_t splats = lmv_scene_splat_count(scene);
  if (splats > 0 && lmv_scene_splats(scene)) {
    if (!up.Buffer(lmv_scene_splats(scene), UINT64(splats) * sizeof(lmv_splat),
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   &splat_buffer, err))
      return false;
    splat_count = splats;
  }
  const uint32_t line_segments = lmv_scene_line_count(scene);
  if (line_segments > 0 && lmv_scene_lines(scene)) {
    if (!up.Buffer(lmv_scene_lines(scene),
                   UINT64(line_segments) * sizeof(lmv_line_segment),
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   &line_buffer, err))
      return false;
    line_count = line_segments;
  }

  // Per-frame deform data: one persistently mapped slot per swapchain buffer.
  if (deform_slot_bytes == 0) deform_slot_bytes = kDeformAlign;
  {
    D3D12_HEAP_PROPERTIES dhp = {};
    dhp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC drd = {};
    drd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    drd.Width = deform_slot_bytes * kFrameCount;
    drd.Height = 1;
    drd.DepthOrArraySize = 1;
    drd.MipLevels = 1;
    drd.SampleDesc.Count = 1;
    drd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    hr = d3d->CreateCommittedResource(
        &dhp, D3D12_HEAP_FLAG_NONE, &drd, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&deform_ring));
    if (FAILED(hr)) return Fail(err, "CreateCommittedResource(deform)", hr);
    const D3D12_RANGE none = {0, 0};
    void* mapped = nullptr;
    hr = deform_ring->Map(0, &none, &mapped);
    if (FAILED(hr)) return Fail(err, "Map(deform)", hr);
    deform_mapped = static_cast<uint8_t*>(mapped);
    std::memset(deform_mapped, 0, size_t(deform_slot_bytes * kFrameCount));
  }
  for (UINT slot = 0; slot < kFrameCount; ++slot) UpdateDeform(scene, slot);

  return up.Finish(device, err);  // staging buffers die after this
}

}  // namespace ld3d12
