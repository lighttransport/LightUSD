#define TEST_NO_MAIN
#include "acutest.h"
#include "unit-ptx.h"

#include <cstdlib>
#include <cstdint>
#include <vector>
#include "ptx-loader.hh"
#include "external/miniz.h"

namespace {

void Put16(std::vector<uint8_t>* b, uint16_t v) {
  b->push_back(static_cast<uint8_t>(v));
  b->push_back(static_cast<uint8_t>(v >> 8));
}
void Put32(std::vector<uint8_t>* b, uint32_t v) {
  for (int i = 0; i < 4; ++i) b->push_back(static_cast<uint8_t>(v >> (8 * i)));
}
void Put64(std::vector<uint8_t>* b, uint64_t v) {
  Put32(b, static_cast<uint32_t>(v));
  Put32(b, static_cast<uint32_t>(v >> 32));
}
std::vector<uint8_t> Deflate(const std::vector<uint8_t>& src) {
  mz_ulong cap = mz_compressBound(static_cast<mz_ulong>(src.size()));
  std::vector<uint8_t> out(static_cast<size_t>(cap));
  mz_ulong n = cap;
  TEST_CHECK(mz_compress(out.data(), &n, src.data(),
                         static_cast<mz_ulong>(src.size())) == MZ_OK);
  out.resize(static_cast<size_t>(n));
  return out;
}

// Build a tiny valid Ptex stream: two RGBA faces, one 4x4 mip and one 2x2 mip.
// Keeping this in-memory makes the fixture portable and avoids checking in a
// generated binary while still exercising the exact container layout.
std::vector<uint8_t> SyntheticPtex(uint32_t encoding = 1, bool tiled = false, bool bad_tile = false, bool mixed = false) {
  std::vector<uint8_t> faceInfo;
  for (uint32_t face = 0; face < 2; ++face) {
    faceInfo.push_back(mixed ? uint8_t(face ? 3 : 1) : 2);  // ures
    faceInfo.push_back(mixed ? uint8_t(face ? 3 : 2) : 2);  // vres
    faceInfo.push_back(0);
    faceInfo.push_back(0);
    for (int e = 0; e < 4; ++e) Put32(&faceInfo, 0xffffffffu);
  }
  const auto faceInfoZ = Deflate(faceInfo);
  std::vector<uint8_t> constants(2 * 4, 0);
  const auto constantsZ = Deflate(constants);

  std::vector<std::vector<uint8_t>> levelBlocks;
  std::vector<uint32_t> levelHeaderSizes;
  for (int level = 0; level < 2; ++level) {
    const uint32_t edge = level == 0 ? 4u : 2u;
    std::vector<uint8_t> payload;
    std::vector<uint8_t> headers;
    for (uint32_t face = 0; face < 2; ++face) {
      if(mixed && level==1 && face==0)continue;
      const uint32_t w=mixed?(face?(level==0?8u:4u):2u):edge;
      const uint32_t h=mixed?(face?(level==0?8u:4u):4u):edge;
      std::vector<uint8_t> planar;
      planar.reserve(w * h * 4);
      for (int channel = 0; channel < 4; ++channel) {
        for (uint32_t y = 0; y < h; ++y) {
          for (uint32_t x = 0; x < w; ++x) {
            const uint8_t value = channel == 0 ? static_cast<uint8_t>(face ? 220 : 30)
                : channel == 1 ? static_cast<uint8_t>(mixed && face==0 ? x+y : x * 40 + level * 5)
                : channel == 2 ? static_cast<uint8_t>(y * 40 + face * 20) : 255;
            planar.push_back(value);
          }
        }
      }
      std::vector<uint8_t> compressed;
      if (encoding == 0) {
        for(size_t c=0;c<4;++c)compressed.push_back(planar[c*w*h]);
      } else {
        if(encoding==2)for(size_t i=planar.size()-1;i>0;--i)planar[i]=uint8_t(planar[i]-planar[i-1]);
        compressed=Deflate(planar);
      }
      if(tiled) {
        std::vector<uint8_t> tile_header;
        Put32(&tile_header,(encoding<<30)|uint32_t(compressed.size()));
        auto tile_header_z=Deflate(tile_header);
        std::vector<uint8_t> block;
        block.push_back(bad_tile?255:uint8_t(level==0?2:1));
        block.push_back(uint8_t(level==0?2:1));
        Put32(&block,uint32_t(tile_header_z.size()));
        block.insert(block.end(),tile_header_z.begin(),tile_header_z.end());
        block.insert(block.end(),compressed.begin(),compressed.end());
        compressed.swap(block);
      }
      Put32(&headers, ((tiled?3u:encoding) << 30) | static_cast<uint32_t>(compressed.size()));
      payload.insert(payload.end(), compressed.begin(), compressed.end());
    }
    const auto headersZ = Deflate(headers);
    levelHeaderSizes.push_back(static_cast<uint32_t>(headersZ.size()));
    std::vector<uint8_t> block = headersZ;
    block.insert(block.end(), payload.begin(), payload.end());
    levelBlocks.push_back(std::move(block));
  }

  std::vector<uint8_t> levelInfo;
  uint64_t levelDataSize = 0;
  for (size_t level = 0; level < levelBlocks.size(); ++level) {
    const auto& block = levelBlocks[level];
    Put64(&levelInfo, static_cast<uint64_t>(block.size()));
    // headerSize is the compressed header stream at the front of the block.
    Put32(&levelInfo, levelHeaderSizes[level]);
    Put32(&levelInfo, mixed && level==1 ? 1u : 2u);
    levelDataSize += block.size();
  }

  std::vector<uint8_t> out;
  out.reserve(64 + faceInfoZ.size() + constantsZ.size() + levelInfo.size() + levelDataSize);
  out.push_back('P'); out.push_back('t'); out.push_back('e'); out.push_back('x');
  Put32(&out, 1);             // major version
  Put32(&out, 1);             // quad mesh
  Put32(&out, 0);             // UInt8
  Put32(&out, 0xffffffffu);   // alpha channel
  Put16(&out, 4);             // channels
  Put16(&out, 2);             // levels
  Put32(&out, 2);             // faces
  Put32(&out, 0);             // extended header size
  Put32(&out, static_cast<uint32_t>(faceInfoZ.size()));
  Put32(&out, static_cast<uint32_t>(constantsZ.size()));
  Put32(&out, static_cast<uint32_t>(levelInfo.size()));
  Put32(&out, 0);             // reserved header word
  Put64(&out, levelDataSize);
  Put32(&out, 0);             // metadata size
  Put32(&out, 0);             // reserved header word
  out.insert(out.end(), faceInfoZ.begin(), faceInfoZ.end());
  out.insert(out.end(), constantsZ.begin(), constantsZ.end());
  out.insert(out.end(), levelInfo.begin(), levelInfo.end());
  for (const auto& block : levelBlocks) out.insert(out.end(), block.begin(), block.end());
  return out;
}

}  // namespace

void ptx_reader_rejects_invalid_input_test(void) {
  const uint8_t bad[] = {'P', 't', 'e', 'x', 1, 0, 0};
  lightusd::ptx::Reader reader;
  std::string err;
  TEST_CHECK(!lightusd::ptx::Reader::OpenMemory(bad, sizeof(bad), &reader, &err));
  TEST_CHECK(!err.empty());
}

void ptx_reader_island_fixture_test(void) {
  const char* path = std::getenv("LIGHTUSD_PTX_FIXTURE");
  if (!path || !*path) {
    TEST_MSG("LIGHTUSD_PTX_FIXTURE is unset (fixture check skipped)");
    return;
  }
  lightusd::ptx::Reader reader;
  std::string err;
  TEST_CHECK(lightusd::ptx::Reader::OpenFile(path, &reader, &err));
  if (!err.empty()) TEST_MSG("Ptex open error: %s", err.c_str());
  if (reader.info().faces == 0) return;
  lightusd::ptx::FaceImage image;
  TEST_CHECK(reader.ReadFace(0, 0, 64u * 1024u * 1024u, &image, &err));
  TEST_CHECK(image.width > 0 && image.height > 0);
  TEST_CHECK(!image.data.empty());
  for (uint32_t level = 1; level < reader.info().levels; ++level) {
    err.clear();
    lightusd::ptx::FaceImage mip;
    TEST_CHECK(reader.ReadFace(0, level, 64u * 1024u * 1024u, &mip, &err));
    TEST_CHECK(mip.width > 0 && mip.height > 0);
    TEST_CHECK(!mip.data.empty());
  }
}

void ptx_reader_synthetic_fixture_test(void) {
  for(uint32_t encoding=0;encoding<=2;++encoding)for(bool tiled:{false,true}) {
    if(tiled && encoding==0)continue;
    const auto bytes=SyntheticPtex(encoding,tiled);
    lightusd::ptx::Reader reader;std::string err;
    TEST_ASSERT(lightusd::ptx::Reader::OpenMemory(bytes.data(),bytes.size(),&reader,&err));
    for(uint32_t level=0;level<2;++level)for(uint32_t face=0;face<2;++face) {
      lightusd::ptx::FaceImage image;
      TEST_ASSERT(reader.ReadFace(face,level,1024,&image,&err));
      const uint32_t edge=level==0?4u:2u;
      TEST_CHECK(image.width==edge && image.height==edge && image.channels==4);
      TEST_ASSERT(image.data.size()==edge*edge*4);
      for(uint32_t y=0;y<edge;++y)for(uint32_t x=0;x<edge;++x) {
        const size_t p=(size_t(y)*edge+x)*4;
        TEST_CHECK(image.data[p]==(face?220:30));
        TEST_CHECK(image.data[p+1]==(encoding==0?0:x*40)+level*5);
        TEST_CHECK(image.data[p+2]==(encoding==0?0:y*40)+face*20);
        TEST_CHECK(image.data[p+3]==255);
      }
    }
  }
  {
    const auto bytes=SyntheticPtex(2,false,false,true);
    lightusd::ptx::Reader reader;std::string error;lightusd::ptx::FaceImage image;
    TEST_ASSERT(lightusd::ptx::Reader::OpenMemory(bytes.data(),bytes.size(),&reader,&error));
    TEST_ASSERT(reader.ReadFace(0,1,4096,&image,&error));
    const std::vector<uint8_t> expected={30,0,20,255,30,2,100,255};
    TEST_CHECK(image.width==1 && image.height==2 && image.data==expected);
    TEST_ASSERT(reader.ReadFace(1,1,4096,&image,&error));
    TEST_CHECK(image.width==4 && image.height==4 && image.data[0]==220 && image.data[1]==5);
  }
  const auto bytes=SyntheticPtex(2,true,true);
  lightusd::ptx::Reader reader;std::string error;lightusd::ptx::FaceImage image;
  TEST_ASSERT(lightusd::ptx::Reader::OpenMemory(bytes.data(),bytes.size(),&reader,&error));
  TEST_CHECK(!reader.ReadFace(0,0,1024,&image,&error));
}

// ---------------------------------------------------------------------------
// Differential test: the C11 lptex core must decode byte-identically to the
// frozen pre-rewrite C++ reader (ptx-legacy-reader.inc), including the same
// success/failure outcome and message, on valid and corrupted inputs.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "ptex/lptex.h"
#include "ptx-legacy-reader.inc"

namespace {

struct Rng {
  uint64_t s;
  uint32_t Next() {
    s ^= s << 13; s ^= s >> 7; s ^= s << 17;
    return static_cast<uint32_t>(s >> 11);
  }
  uint32_t Below(uint32_t n) { return n ? Next() % n : 0; }
};

// A randomized multi-face container: mixed resolutions, constant faces,
// every face encoding (constant, zip, diff-zip, tiled with several tiles),
// and reduction levels that store only a prefix of the sorted face order so
// the rest is generated by the reader.
std::vector<uint8_t> RandomPtex(uint64_t seed) {
  Rng rng{seed * 0x9e3779b97f4a7c15ull + 1};
  const uint32_t type = rng.Below(4);
  const size_t bpc = type == 0 ? 1 : type == 3 ? 4 : 2;
  const uint16_t channels = static_cast<uint16_t>(1 + rng.Below(4));
  const uint32_t faces = 1 + rng.Below(12);
  const uint16_t levels = static_cast<uint16_t>(1 + rng.Below(4));
  struct F { uint8_t ures, vres, flags; };
  std::vector<F> fi(faces);
  std::vector<uint8_t> faceInfo, constants;
  for (uint32_t f = 0; f < faces; ++f) {
    fi[f].ures = static_cast<uint8_t>(rng.Below(5));
    fi[f].vres = static_cast<uint8_t>(rng.Below(5));
    fi[f].flags = rng.Below(5) == 0 ? 1 : 0;
    faceInfo.push_back(fi[f].ures); faceInfo.push_back(fi[f].vres);
    faceInfo.push_back(static_cast<uint8_t>(rng.Next()));
    faceInfo.push_back(fi[f].flags);
    for (int e = 0; e < 4; ++e) Put32(&faceInfo, rng.Below(faces + 1));
    for (size_t b = 0; b < channels * bpc; ++b)
      constants.push_back(static_cast<uint8_t>(rng.Next()));
  }
  auto encode = [&](uint32_t w, uint32_t h, uint32_t enc) {
    std::vector<uint8_t> planar(size_t(w) * h * channels * bpc);
    for (auto& b : planar) b = static_cast<uint8_t>(rng.Next() & (rng.Below(2) ? 0xff : 0x0f));
    if (enc == 0) return std::vector<uint8_t>(planar.begin(), planar.begin() + channels * bpc);
    return Deflate(planar);
  };
  std::vector<std::vector<uint8_t>> blocks;
  std::vector<uint32_t> headerSizes, levelFaces;
  for (uint16_t level = 0; level < levels; ++level) {
    std::vector<uint32_t> order;
    for (uint32_t f = 0; f < faces; ++f)
      if (level == 0 || !(fi[f].flags & 1)) order.push_back(f);
    if (level) {
      std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
        const uint32_t ka = std::min(1u << fi[a].ures, 1u << fi[a].vres) >> level;
        const uint32_t kb = std::min(1u << fi[b].ures, 1u << fi[b].vres) >> level;
        if (ka != kb) return ka > kb;
        return a < b;
      });
    }
    uint32_t stored = level == 0 ? uint32_t(order.size())
                                 : rng.Below(uint32_t(order.size()) + 1);
    std::vector<uint8_t> headers, payload;
    for (uint32_t k = 0; k < stored; ++k) {
      const uint32_t f = order[k];
      const uint32_t w = std::max(1u, (1u << fi[f].ures) >> level);
      const uint32_t h = std::max(1u, (1u << fi[f].vres) >> level);
      uint32_t enc = rng.Below(4);
      std::vector<uint8_t> data;
      if (enc == 3) {
        const uint8_t tlu = static_cast<uint8_t>(rng.Below(3));
        const uint8_t tlv = static_cast<uint8_t>(rng.Below(3));
        const uint32_t tw = 1u << tlu, th = 1u << tlv;
        const uint32_t nx = (w + tw - 1) / tw, ny = (h + th - 1) / th;
        std::vector<uint8_t> th_bytes, tiles;
        for (uint32_t ty = 0; ty < ny; ++ty) for (uint32_t tx = 0; tx < nx; ++tx) {
          const uint32_t cw = std::min(tw, w - tx * tw), ch = std::min(th, h - ty * th);
          const uint32_t te = rng.Below(3);
          const auto t = encode(cw, ch, te);
          Put32(&th_bytes, (te << 30) | uint32_t(t.size()));
          tiles.insert(tiles.end(), t.begin(), t.end());
        }
        const auto thz = Deflate(th_bytes);
        data.push_back(tlu); data.push_back(tlv);
        Put32(&data, uint32_t(thz.size()));
        data.insert(data.end(), thz.begin(), thz.end());
        data.insert(data.end(), tiles.begin(), tiles.end());
      } else {
        data = encode(w, h, enc);
      }
      Put32(&headers, (enc << 30) | uint32_t(data.size()));
      payload.insert(payload.end(), data.begin(), data.end());
    }
    if (stored == 0) Put32(&headers, 0);
    const auto hz = Deflate(headers);
    std::vector<uint8_t> block = hz;
    block.insert(block.end(), payload.begin(), payload.end());
    headerSizes.push_back(uint32_t(hz.size()));
    levelFaces.push_back(stored);
    blocks.push_back(std::move(block));
  }
  const auto fz = Deflate(faceInfo), cz = Deflate(constants);
  std::vector<uint8_t> levelInfo;
  uint64_t dataSize = 0;
  for (size_t l = 0; l < blocks.size(); ++l) {
    Put64(&levelInfo, blocks[l].size());
    Put32(&levelInfo, headerSizes[l]);
    Put32(&levelInfo, levelFaces[l]);
    dataSize += blocks[l].size();
  }
  std::vector<uint8_t> out = {'P', 't', 'e', 'x'};
  Put32(&out, 1); Put32(&out, rng.Below(4) ? 1u : 0u); Put32(&out, type);
  Put32(&out, 0xffffffffu); Put16(&out, channels); Put16(&out, levels);
  Put32(&out, faces); Put32(&out, 0); Put32(&out, uint32_t(fz.size()));
  Put32(&out, uint32_t(cz.size())); Put32(&out, uint32_t(levelInfo.size()));
  Put32(&out, 0); Put64(&out, dataSize); Put32(&out, 0); Put32(&out, 0);
  out.insert(out.end(), fz.begin(), fz.end());
  out.insert(out.end(), cz.begin(), cz.end());
  out.insert(out.end(), levelInfo.begin(), levelInfo.end());
  for (const auto& b : blocks) out.insert(out.end(), b.begin(), b.end());
  return out;
}

struct DiffStats { size_t files = 0, opened = 0, reads = 0, ok_reads = 0, mismatches = 0; };

// Compare one buffer under both readers. Returns false on the first mismatch.
bool DiffBuffer(const uint8_t* data, size_t size, const char* label,
                DiffStats* st, const size_t* budgets, size_t nbudgets,
                uint32_t face_stride = 1) {
  ++st->files;
  ptx_legacy::Reader legacy;
  std::string lerr;
  const bool lok = ptx_legacy::Reader::OpenMemory(data, size, &legacy, &lerr);
  lptex_reader* r = nullptr;
  const lptex_status s = lptex_open_memory(data, size, nullptr, &r);
  const bool nok = s == LPTEX_OK;
  if (lok != nok || (!lok && lerr != lptex_status_str(s))) {
    TEST_MSG("%s: open mismatch legacy=%d '%s' lptex=%d '%s'", label, lok,
             lerr.c_str(), nok, lptex_status_str(s));
    lptex_close(r);
    ++st->mismatches;
    return false;
  }
  lightusd::ptx::Reader wrapped;
  std::string werr;
  const bool wok = lightusd::ptx::Reader::OpenMemory(data, size, &wrapped, &werr);
  if (wok != lok || werr != lerr) {
    TEST_MSG("%s: wrapper open mismatch '%s' vs '%s'", label, werr.c_str(), lerr.c_str());
    lptex_close(r);
    ++st->mismatches;
    return false;
  }
  if (!lok) return true;
  ++st->opened;
  const lptex_info* ni = lptex_reader_info(r);
  const auto& li = legacy.info();
  bool same = ni->faces == li.faces && ni->levels == li.levels &&
              ni->channels == li.channels && uint32_t(ni->type) == uint32_t(li.dataType) &&
              ni->mesh_type == li.meshType && ni->alpha_channel == li.alphaChannel;
  for (uint32_t f = 0; same && f < li.faces; ++f) {
    const auto& a = li.faceInfo[f];
    const lptex_face_info& b = ni->face_info[f];
    same = a.ures == b.ures && a.vres == b.vres && a.adjEdges == b.adj_edges &&
           a.flags == b.flags && std::memcmp(a.adjFaces, b.adj_faces, 16) == 0;
  }
  if (!same) {
    TEST_MSG("%s: info mismatch", label);
    lptex_close(r);
    ++st->mismatches;
    return false;
  }
  for (size_t bi = 0; bi < nbudgets; ++bi) {
    for (uint32_t level = 0; level <= li.levels; ++level) {
      for (uint32_t face = 0; face <= li.faces;
           face = face == li.faces ? face + 1 : std::min(li.faces, face + face_stride)) {
        ++st->reads;
        ptx_legacy::FaceImage limg;
        std::string e;
        const bool a = legacy.ReadFace(face, level, budgets[bi], &limg, &e);
        lptex_image nimg;
        const lptex_status rs = lptex_read_face(r, face, level, budgets[bi], &nimg);
        const bool b = rs == LPTEX_OK;
        bool eq = a == b;
        if (eq && a) {
          eq = limg.width == nimg.width && limg.height == nimg.height &&
               limg.channels == nimg.channels && limg.data.size() == nimg.size &&
               (nimg.size == 0 || std::memcmp(limg.data.data(), nimg.data, nimg.size) == 0);
          ++st->ok_reads;
        } else if (eq) {
          eq = e == lptex_status_str(rs);
        }
        if (eq) {
          lightusd::ptx::FaceImage wimg;
          std::string we;
          const bool c = wrapped.ReadFace(face, level, budgets[bi], &wimg, &we);
          eq = c == a && (a ? wimg.data == limg.data && wimg.width == limg.width &&
                                  wimg.height == limg.height
                            : we == e);
        }
        if (!eq) {
          TEST_MSG("%s: face %u level %u budget %zu: legacy=%d '%s' lptex=%d '%s'",
                   label, face, level, budgets[bi], a, e.c_str(), b,
                   lptex_status_str(rs));
          lptex_image_free(r, &nimg);
          lptex_close(r);
          ++st->mismatches;
          return false;
        }
        lptex_image_free(r, &nimg);
      }
    }
  }
  lptex_close(r);
  return true;
}

std::vector<uint8_t> ReadAll(const std::string& path) {
  std::vector<uint8_t> out;
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return out;
  uint8_t buf[1 << 16];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.insert(out.end(), buf, buf + n);
  std::fclose(f);
  return out;
}

}  // namespace

void ptx_reader_legacy_differential_test(void) {
  DiffStats st;
  const size_t budgets[] = {64u << 20, 300};
  std::vector<std::vector<uint8_t>> corpus;
  for (uint32_t enc = 0; enc <= 2; ++enc)
    for (int tiled = 0; tiled < 2; ++tiled)
      for (int mixed = 0; mixed < 2; ++mixed)
        corpus.push_back(SyntheticPtex(enc, tiled != 0, false, mixed != 0));
  corpus.push_back(SyntheticPtex(2, true, true));
  for (uint64_t seed = 0; seed < 400; ++seed) corpus.push_back(RandomPtex(seed));
  if (const char* root = std::getenv("LIGHTUSD_TEST_FIXTURE_DIR")) {
    const std::string fixture = std::string(root) + "/examples/lmetalview/tests/textures/ptex_faces.ptx";
    auto bytes = ReadAll(fixture);
    TEST_CHECK(!bytes.empty());
#ifndef LPTEX_NO_FILEIO
    // The C file opener must see the same container as the memory path.
    lptex_reader* fr = nullptr;
    TEST_CHECK(lptex_open_file(fixture.c_str(), nullptr, &fr) == LPTEX_OK);
    if (fr) {
      lptex_image a, b;
      lptex_reader* mr = nullptr;
      TEST_CHECK(lptex_open_memory(bytes.data(), bytes.size(), nullptr, &mr) == LPTEX_OK);
      TEST_CHECK(lptex_read_face(fr, 0, 0, 1u << 20, &a) == LPTEX_OK);
      TEST_CHECK(lptex_read_face(mr, 0, 0, 1u << 20, &b) == LPTEX_OK);
      TEST_CHECK(a.size == b.size && std::memcmp(a.data, b.data, a.size) == 0);
      lptex_image_free(fr, &a);
      lptex_image_free(mr, &b);
      lptex_close(mr);
      lptex_close(fr);
    }
    TEST_CHECK(lptex_open_file((std::string(root) + "/no-such.ptx").c_str(), nullptr, &fr) ==
               LPTEX_E_OPEN_FAILED);
    TEST_CHECK(lptex_open_file(root, nullptr, &fr) == LPTEX_E_FILE_TOO_LARGE);
#endif
    if (!bytes.empty()) corpus.push_back(std::move(bytes));
  }
  // LIGHTUSD_PTX_DUMP_CORPUS=dir writes the generated corpus (fuzzer seeds).
  if (const char* dir = std::getenv("LIGHTUSD_PTX_DUMP_CORPUS")) {
    for (size_t i = 0; i < corpus.size(); ++i) {
      const std::string path = std::string(dir) + "/synthetic-" + std::to_string(i) + ".ptx";
      if (FILE* f = std::fopen(path.c_str(), "wb")) {
        std::fwrite(corpus[i].data(), 1, corpus[i].size(), f);
        std::fclose(f);
      }
    }
  }
  char label[64];
  for (size_t i = 0; i < corpus.size(); ++i) {
    std::snprintf(label, sizeof(label), "corpus[%zu]", i);
    TEST_CHECK(DiffBuffer(corpus[i].data(), corpus[i].size(), label, &st, budgets, 2));
  }
  // Deterministic corruption: byte flips and truncations of every corpus entry.
  Rng rng{12345};
  for (size_t i = 0; i < corpus.size(); ++i) {
    for (int m = 0; m < 24; ++m) {
      const std::vector<uint8_t>& src = corpus[i];
      if (src.empty()) continue;
      const size_t keep = m % 6 == 5 ? rng.Below(uint32_t(src.size())) : src.size();
      std::vector<uint8_t> bytes(src.begin(), src.begin() + std::ptrdiff_t(keep));
      if (m % 6 != 5 && !bytes.empty()) {
        const int flips = 1 + int(rng.Below(4));
        for (int k = 0; k < flips; ++k)
          bytes[rng.Below(uint32_t(bytes.size()))] ^= static_cast<uint8_t>(1u << rng.Below(8));
      }
      std::snprintf(label, sizeof(label), "mutant[%zu.%d]", i, m);
      TEST_CHECK(DiffBuffer(bytes.data(), bytes.size(), label, &st, budgets, 2));
    }
  }
  // Optional real files, every face and level: LIGHTUSD_PTX_DIFF_FILES=a.ptx:b.ptx
  if (const char* list = std::getenv("LIGHTUSD_PTX_DIFF_FILES")) {
    std::string s(list);
    size_t pos = 0;
    while (pos <= s.size()) {
      const size_t end = std::min(s.find(':', pos), s.size());
      const std::string path = s.substr(pos, end - pos);
      pos = end + 1;
      if (path.empty()) continue;
      const auto bytes = ReadAll(path);
      TEST_CHECK(!bytes.empty());
      const size_t big[] = {64u << 20};
      const size_t before = st.ok_reads;
      // The legacy oracle sorts the face table on every reduction-level read;
      // sample at most ~1500 faces per file to keep large files tractable.
      uint32_t faces = 0;
      if (bytes.size() >= 28)
        faces = uint32_t(bytes[24]) | uint32_t(bytes[25]) << 8 |
                uint32_t(bytes[26]) << 16 | uint32_t(bytes[27]) << 24;
      const uint32_t stride = std::max(1u, faces / 1500u);
      TEST_CHECK(DiffBuffer(bytes.data(), bytes.size(), path.c_str(), &st, big, 1, stride));
      std::printf("  %s: %zu face/level decodes identical\n", path.c_str(), st.ok_reads - before);
    }
  }
  std::printf("  ptx differential: %zu buffers, %zu opened, %zu reads (%zu decoded), %zu mismatches\n",
              st.files, st.opened, st.reads, st.ok_reads, st.mismatches);
  TEST_CHECK(st.mismatches == 0);
}
