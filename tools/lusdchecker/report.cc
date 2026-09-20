// SPDX-License-Identifier: Apache-2.0
#include "report.hh"
#include <fstream>
#include <set>
#include <vector>

namespace lusdchecker {
using Json = lightusd::minijson::Value;
namespace {
std::string Key(const Json& issue) {
  // A JSON tuple avoids delimiter collisions in user-authored messages/paths.
  return Json::array({issue["ruleId"], issue["location"], issue["severity"],
                      issue["message"]}).dump();
}
bool IsIssue(const Json& i) {
  return i.is_object() && i["ruleId"].is_string() &&
         !i["ruleId"].get_string().empty() && i["location"].is_string() &&
         i["message"].is_string() &&
         (i["severity"].get_string() == "error" ||
          i["severity"].get_string() == "warning");
}
std::string Uri(const std::string& path) {
  const char* hex = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : path) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '/' || c == '-' || c == '_' || c == '.')
      out += char(c);
    else if (c == '\\') out += '/';
    else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
  }
  return out;
}
}  // namespace

bool ApplyBaseline(Json* report, const std::string& path, std::string* error) {
  std::set<std::string> accepted;
  if (!path.empty()) {
    constexpr size_t cap = 16 * 1024 * 1024;
    std::ifstream in(path, std::ios::binary);
    if (!in) { *error = "cannot read baseline: " + path; return false; }
    std::string text;
    char block[8192];
    while (in) {
      in.read(block, sizeof(block));
      const size_t n = size_t(in.gcount());
      if (n > cap - text.size()) { *error = "baseline exceeds 16 MiB"; return false; }
      text.append(block, n);
    }
    Json baseline;
    lightusd::minijson::Error parse_error;
    if (in.bad() || !lightusd::minijson::Parse(text, &baseline, &parse_error) ||
        baseline["tool"].get_string() != "lusdchecker" ||
        !baseline["issues"].is_array()) {
      *error = "baseline must be a lusdchecker JSON report"; return false;
    }
    for (const Json& issue : baseline["issues"]) {
      if (!IsIssue(issue)) { *error = "malformed baseline issue"; return false; }
      accepted.insert(Key(issue));
    }
  }
  size_t existing = 0, fresh = 0;
  bool passed = !(*report)["variantLimitHit"].get_bool() &&
      (!(*report)["strict"].get_bool() || (*report)["parserWarnings"].get_string().empty());
  for (Json& issue : (*report)["issues"]) {
    // A baseline cannot bless incomplete validation coverage.
    const std::string rule = issue["ruleId"].get_string();
    const bool known = rule.rfind("checker.", 0) != 0 && accepted.count(Key(issue));
    if (!path.empty()) issue["baselineState"] = known ? "unchanged" : "new";
    if (known) ++existing;
    else {
      ++fresh;
      if (issue["severity"].get_string() == "error" || (*report)["strict"].get_bool())
        passed = false;
    }
  }
  (*report)["newIssueCount"] = uint64_t(fresh);
  (*report)["existingIssueCount"] = uint64_t(existing);
  (*report)["gatePassed"] = path.empty() ? (*report)["valid"].get_bool() : passed;
  return true;
}

Json ToSarif(const Json& report) {
  Json results = Json::array(), rules = Json::array();
  std::set<std::string> rule_ids;
  for (const Json& issue : report["issues"]) rule_ids.insert(issue["ruleId"].get_string());
  for (const auto& id : rule_ids) rules.push_back(Json{{"id", id}});
  for (const Json& issue : report["issues"]) {
    Json result{{"ruleId", issue["ruleId"]}, {"level", issue["severity"]},
                {"message", Json{{"text", issue["message"]}}}};
    Json location{{"logicalLocations", Json::array({Json{{"fullyQualifiedName", issue["location"]}}})}};
    if (report["input"].get_string() != "-")
      location["physicalLocation"] = Json{{"artifactLocation", Json{{"uri", Uri(report["input"].get_string())}}}};
    result["locations"] = Json::array({location});
    if (issue.contains("baselineState")) result["baselineState"] = issue["baselineState"];
    results.push_back(std::move(result));
  }
  Json invocation{{"executionSuccessful", true}};
  if (!report["parserWarnings"].get_string().empty())
    invocation["toolExecutionNotifications"] = Json::array({Json{{"level", "warning"},
        {"message", Json{{"text", report["parserWarnings"]}}}}});
  Json run{{"tool", Json{{"driver", Json{{"name", "lusdchecker"}, {"rules", rules}}}}},
      {"results", results}, {"invocations", Json::array({invocation})},
      {"properties", Json{{"valid", report["valid"]}, {"gatePassed", report["gatePassed"]},
          {"newIssueCount", report["newIssueCount"]}, {"existingIssueCount", report["existingIssueCount"]},
          {"variantLimitHit", report["variantLimitHit"]}, {"checkedGroups", report["checkedGroups"]},
          {"skippedGroups", report["skippedGroups"]}}}};
  return Json{{"version", "2.1.0"},
      {"$schema", "https://docs.oasis-open.org/sarif/sarif/v2.1.0/cos02/schemas/sarif-schema-2.1.0.json"},
      {"runs", Json::array({run})}};
}
}  // namespace lusdchecker
