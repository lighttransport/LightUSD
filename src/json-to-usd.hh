// SPDX-License-Identifier: Apache 2.0
// Experimental JSON to USD converter

#include <string>

#include "lightusd.hh"
#include "usdGeom.hh"  // GeomMesh etc. (no longer re-exported by lightusd.hh)

namespace lightusd {

/// Optional policy and asset-resolution controls for JSON runtime imports.
/// Embedded data URLs remain the default and require no resolver.
struct JSONToUSDOptions {
  const AssetResolutionResolver *resolver{nullptr};
  // Zero uses the normal JSON decoded-byte security limit.
  size_t max_external_buffer_bytes{0};
};

///
/// Convert JSON string to USD Stage
///
///
bool JSONToStage(const std::string &json_string, lightusd::Stage *stage, std::string *warn, std::string *err);
bool JSONToStage(const std::string &json_string, lightusd::Stage *stage,
                 std::string *warn, std::string *err,
                 const JSONToUSDOptions &options);

///
/// Convert JSON string to USD Prim
///
///
bool JSONToStage(const std::string &json_string, lightusd::Prim *prim, std::string *warn, std::string *err);
bool JSONToStage(const std::string &json_string, lightusd::Prim *prim,
                 std::string *warn, std::string *err,
                 const JSONToUSDOptions &options);

///
/// Convert JSON string to USD Layer
///
///
bool JSONToLayer(const std::string &json_string, lightusd::Layer *layer, std::string *warn, std::string *err);

///
/// Convert JSON string to PrimSpec
///
///
bool JSONToPrimSpec(const std::string &json_string, lightusd::PrimSpec *ps, std::string *warn, std::string *err);

///
/// Convert JSON object to GeomMesh
///
///
bool JSONToGeomMesh(const std::string &json_string, lightusd::GeomMesh *mesh, std::string *warn, std::string *err);
bool JSONToGeomMesh(const std::string &json_string, lightusd::GeomMesh *mesh,
                    std::string *warn, std::string *err,
                    const JSONToUSDOptions &options);

} // namespace lightusd
