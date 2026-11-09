// SPDX-License-Identifier: MIT
//
#include <string>

#include "lightusd.hh"
#include "io-util.hh"
#include "core/security-policy.hh"
#include "usdFbx.hh"

//#include "math-util.inc"

#ifdef LIGHTUSD_USE_USDFBX

#include "external/OpenFBX/src/ofbx.h"

#endif

namespace lightusd {

namespace usdFbx {

namespace {



}

bool ReadFbxFromFile(const std::string &filepath, lightusd::GPrim *prim, std::string *err)
{
#if !defined(LIGHTUSD_USE_USDFBX)
  (void)filepath;
  (void)prim;
  if (err) {
    (*err) = "usdFbx is disabled in this build.\n";
  }
  return false;
#else

  std::vector<uint8_t> buf;
  if (!io::ReadWholeFile(&buf, err, filepath,
                         security_policy::GetMaxAssetReadBytes(),
                         /* user_ptr */ nullptr)) {
    return false;
  }

  std::string str(reinterpret_cast<const char *>(buf.data()), buf.size());

  return ReadFbxFromString(str, prim, err);

#endif

}


bool ReadFbxFromString(const std::string &str, lightusd::GPrim *prim, std::string *err)
{
#if !defined(LIGHTUSD_USE_USDFBX)
  (void)str;
  (void)prim;
  if (err) {
    (*err) = "usdFbx is disabled in this build.\n";
  }
  return false;
#else

  (void)str;
  (void)prim;
  if (err) {
    (*err) = "TODO: Implement usdFbx importer.\n";
  }

  return false;
#endif
}

} // namespace usdFbx

} // lightusd
