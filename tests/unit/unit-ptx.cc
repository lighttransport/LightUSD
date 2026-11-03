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
