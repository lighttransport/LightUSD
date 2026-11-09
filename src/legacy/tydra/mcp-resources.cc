#include "mcp-resources.hh"

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif

#include "external/jsonhpp/nlohmann/json.hpp"

#ifdef __clang__
#pragma clang diagnostic pop
#endif

using namespace nlohmann;

namespace lightusd {
namespace tydra {
namespace mcp {

namespace {

static bool ListResourcesImpl(const Context &ctx, json &result) {

  result["resources"] = nlohmann::json::array();

  for (const auto &res : ctx.assets) {

    json res_j;

    res_j["name"] = res.second.name;
    res_j["mimeType"] = "application/octet-stream"; // FIXME
    // TODO: size, title, description

    result["resources"].push_back(res_j);
  }

  return true;
}

} // namespace

bool GetResourcesList(const Context &ctx, nlohmann::json &result) {

  return ListResourcesImpl(ctx, result);
}

bool ReadResource(const Context &ctx, const std::string &name, nlohmann::json &result) {
  // TODO: multiple resources


  if (!ctx.assets.count(name)) {
    // TODO: report error
    return false;
  }

  const auto &asset = ctx.assets.at(name);

  json res;
  res["type"] = "text"; // FIXME
  res["text"] = asset.data;

  result["contents"] = json::array();
  result["contents"].push_back(res);

  return true;
}


} // namespace mcp
} // namespace tydra
} // namespace lightusd
