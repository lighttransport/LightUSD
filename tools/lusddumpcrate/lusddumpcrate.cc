// SPDX-License-Identifier: Apache 2.0
// Copyright 2025, Light Transport Entertainment Inc.
//
// LightUSD Crate Dump Tool
//
// Dumps low-level USDC Crate file structure in YAML or JSON format
// for efficient debugging and investigation.
//

#include <iostream>
#include <fstream>
#include <iomanip>
#include <vector>
#include <string>
#include <cstring>
#include <sstream>
#include "minijson.hh"

// LightUSD reader headers
#include "lightusd.hh"
#include "usdc-reader.hh"
#include "stream-reader.hh"
#include "crate-format.hh"
#include "crate-reader.hh"
#include "value-types.hh"
#include "core/prim.hh"
#include "core/prim-spec.hh"

using namespace lightusd;
using namespace lightusd::crate;

enum class OutputFormat {
  YAML,
  JSON
};

struct DumpOptions {
  OutputFormat format = OutputFormat::YAML;
  bool show_bootstrap = true;
  bool show_toc = true;
  bool show_tokens = true;
  bool show_strings = true;
  bool show_fields = true;
  bool show_fieldsets = true;
  bool show_paths = true;
  bool show_specs = true;
  bool show_hex = false;
  int max_tokens = -1;  // -1 = unlimited
  int max_strings = -1;
  int max_fields = -1;
  int max_fieldsets = -1;
  int max_paths = -1;
  int max_specs = -1;
  std::string token_filter;
  std::string path_filter;
};

static bool ContainsFilter(const std::string& value, const std::string& filter) {
  return filter.empty() || (value.find(filter) != std::string::npos);
}

template <typename PathLike>
static std::string CratePathString(const PathLike& path) {
  std::string full_path = path.full_path_name();
  if (!path.prop_part().empty()) {
    const std::string suffix = "." + path.prop_part();
    if (full_path.size() < suffix.size() ||
        full_path.compare(full_path.size() - suffix.size(), suffix.size(), suffix) != 0) {
      full_path += suffix;
    }
  }
  return full_path;
}

// Helper function to convert SpecType to string
static std::string GetSpecTypeName(SpecType type) {
  switch (type) {
    case SpecType::Unknown: return "Unknown";
    case SpecType::Attribute: return "Attribute";
    case SpecType::Connection: return "Connection";
    case SpecType::Expression: return "Expression";
    case SpecType::Mapper: return "Mapper";
    case SpecType::MapperArg: return "MapperArg";
    case SpecType::Prim: return "Prim";
    case SpecType::PseudoRoot: return "PseudoRoot";
    case SpecType::Relationship: return "Relationship";
    case SpecType::RelationshipTarget: return "RelationshipTarget";
    case SpecType::Variant: return "Variant";
    case SpecType::VariantSet: return "VariantSet";
    case SpecType::Invalid: return "Invalid";
    default: return "Unknown_" + std::to_string(static_cast<int>(type));
  }
}

class CrateDumper {
public:
  CrateDumper(const std::string& filename, const DumpOptions& opts)
    : filename_(filename), opts_(opts), indent_(0) {}

  bool Dump() {
    // Open and read the file
    std::ifstream ifs(filename_, std::ios::binary);
    if (!ifs) {
      std::cerr << "Error: Cannot open file: " << filename_ << std::endl;
      return false;
    }

    // Read entire file into memory
    ifs.seekg(0, std::ios::end);
    size_t file_size = ifs.tellg();
    ifs.seekg(0, std::ios::beg);

    std::vector<uint8_t> data(file_size);
    ifs.read(reinterpret_cast<char*>(data.data()), file_size);
    ifs.close();

    // Create stream reader
    StreamReader sr(data.data(), data.size(), /* swap_endian */ false);

    // Create crate reader config
    CrateReaderConfig config;
    config.numThreads = 1;

    // Create crate reader
    CrateReader reader(&sr, config);

    // Read all sections
    if (!reader.ReadBootStrap()) {
      std::cerr << "Error: Failed to read bootstrap" << std::endl;
      return false;
    }

    if (!reader.ReadTOC()) {
      std::cerr << "Error: Failed to read TOC" << std::endl;
      return false;
    }

    if (!reader.ReadTokens()) {
      std::cerr << "Error: Failed to read tokens" << std::endl;
      return false;
    }

    if (!reader.ReadStrings()) {
      std::cerr << "Error: Failed to read strings" << std::endl;
      return false;
    }

    if (!reader.ReadFields()) {
      std::cerr << "Error: Failed to read fields" << std::endl;
      return false;
    }

    if (!reader.ReadFieldSets()) {
      std::cerr << "Error: Failed to read fieldsets" << std::endl;
      return false;
    }

    if (!reader.ReadPaths()) {
      std::cerr << "Error: Failed to read paths" << std::endl;
      return false;
    }

    if (!reader.ReadSpecs()) {
      std::cerr << "Error: Failed to read specs" << std::endl;
      return false;
    }

    // Start output
    if (opts_.format == OutputFormat::YAML) {
      DumpYAML(reader, data.data(), file_size);
    } else {
      DumpJSON(reader, data.data(), file_size);
    }

    return static_cast<bool>(std::cout);
  }

private:
  void DumpYAML(const CrateReader& reader, const uint8_t* data, size_t size) {
    out() << "usdc_crate:" << std::endl;
    indent_++;

    out() << "file: \"" << filename_ << "\"" << std::endl;
    out() << "size: " << size << std::endl;

    if (opts_.show_bootstrap) {
      DumpBootstrapYAML(data);
    }

    if (opts_.show_toc) {
      DumpTOCYAML(reader, data, size);
    }

    if (opts_.show_tokens) {
      DumpTokensYAML(reader);
    }

    if (opts_.show_strings) {
      DumpStringsYAML(reader);
    }

    if (opts_.show_fields) {
      DumpFieldsYAML(reader);
    }

    if (opts_.show_fieldsets) {
      DumpFieldSetsYAML(reader);
    }

    if (opts_.show_paths) {
      DumpPathsYAML(reader);
    }

    if (opts_.show_specs) {
      DumpSpecsYAML(reader);
    }

    indent_--;
  }

  void DumpBootstrapYAML(const uint8_t* data) {
    out() << "bootstrap:" << std::endl;
    indent_++;
    out() << "byte_offset: 0" << std::endl;
    out() << "byte_size: 72" << std::endl;

    // Read bootstrap header (72 bytes) - manually extract fields
    char ident[9] = {0};
    memcpy(ident, data, 8);
    out() << "magic: \"" << ident << "\"" << std::endl;

    uint8_t version[3];
    memcpy(version, data + 8, 3);
    out() << "version: [" << static_cast<int>(version[0]) << ", "
          << static_cast<int>(version[1]) << ", "
          << static_cast<int>(version[2]) << "]" << std::endl;

    uint64_t toc_offset;
    memcpy(&toc_offset, data + 16, 8);
    out() << "toc_offset: " << toc_offset << std::endl;

    if (opts_.show_hex) {
      out() << "hex: ";
      for (size_t i = 0; i < 16; i++) {
        std::cout << std::hex << std::setw(2) << std::setfill('0')
                  << static_cast<int>(data[i]) << " ";
      }
      std::cout << std::dec << std::endl;
    }

    indent_--;
  }

  void DumpTOCYAML(const CrateReader& reader, const uint8_t* data, size_t size) {
    out() << "table_of_contents:" << std::endl;
    indent_++;

    // Read TOC offset from bootstrap
    uint64_t toc_offset;
    memcpy(&toc_offset, data + 16, 8);
    out() << "byte_offset: " << toc_offset << std::endl;

    if (toc_offset >= size) {
      out() << "error: \"TOC offset beyond file size\"" << std::endl;
      indent_--;
      return;
    }

    // Read number of sections (uint64_t at TOC offset)
    uint64_t num_sections;
    memcpy(&num_sections, data + toc_offset, 8);
    out() << "num_sections: " << num_sections << std::endl;

    if (num_sections > 0 && num_sections < 100) {
      out() << "sections:" << std::endl;
      indent_++;

      size_t section_offset = toc_offset + 8;
      for (uint64_t i = 0; i < num_sections; i++) {
        if (section_offset + 32 > size) break;

        // Section structure: char name[16] + int64_t start + int64_t size
        char name[17] = {0};
        memcpy(name, data + section_offset, 16);

        int64_t start, sec_size;
        memcpy(&start, data + section_offset + 16, 8);
        memcpy(&sec_size, data + section_offset + 24, 8);

        out() << "- name: \"" << name << "\"" << std::endl;
        indent_++;
        out() << "byte_offset: " << start << std::endl;
        out() << "byte_size: " << sec_size << std::endl;

        // Show element counts for known sections
        if (strcmp(name, "TOKENS") == 0) {
          out() << "num_elements: " << reader.GetTokens().size() << std::endl;
        } else if (strcmp(name, "STRINGS") == 0) {
          out() << "num_elements: " << reader.GetStringIndices().size() << std::endl;
        } else if (strcmp(name, "FIELDS") == 0) {
          out() << "num_elements: " << reader.GetFields().size() << std::endl;
        } else if (strcmp(name, "FIELDSETS") == 0) {
          out() << "num_elements: " << reader.GetFieldsetIndices().size() << std::endl;
        } else if (strcmp(name, "PATHS") == 0) {
          out() << "num_elements: " << reader.GetPaths().size() << std::endl;
        } else if (strcmp(name, "SPECS") == 0) {
          out() << "num_elements: " << reader.GetSpecs().size() << std::endl;
        }
        indent_--;

        section_offset += 32;
      }

      indent_--;
    }

    indent_--;
  }

  void DumpTokensYAML(const CrateReader& reader) {
    out() << "tokens:" << std::endl;
    indent_++;

    const auto& tokens = reader.GetTokens();
    out() << "count: " << tokens.size() << std::endl;

    if (!tokens.empty()) {
      out() << "values:" << std::endl;
      indent_++;

      int emitted = 0;
      int limit = (opts_.max_tokens > 0) ? opts_.max_tokens : static_cast<int>(tokens.size());

      for (size_t i = 0; i < tokens.size() && emitted < limit; i++) {
        if (!ContainsFilter(tokens[i].str(), opts_.token_filter)) {
          continue;
        }
        out() << "- index: " << i << std::endl;
        indent_++;
        out() << "value: \"" << EscapeYAML(tokens[i].str()) << "\"" << std::endl;
        indent_--;
        emitted++;
      }

      if (opts_.max_tokens > 0 && emitted >= opts_.max_tokens) {
        out() << "# ... (token output limited)" << std::endl;
      }

      indent_--;
    }

    indent_--;
  }

  void DumpStringsYAML(const CrateReader& reader) {
    out() << "strings:" << std::endl;
    indent_++;

    const auto& strings = reader.GetStringIndices();
    out() << "count: " << strings.size() << std::endl;

    if (!strings.empty()) {
      out() << "values:" << std::endl;
      indent_++;

      int limit = (opts_.max_strings > 0) ? std::min(opts_.max_strings, static_cast<int>(strings.size()))
                                           : strings.size();

      for (int i = 0; i < limit; i++) {
        out() << "- index: " << i << std::endl;
        indent_++;
        out() << "token_index: " << strings[i].value << std::endl;
        if (strings[i].value < reader.GetTokens().size()) {
          out() << "value: \"" << EscapeYAML(reader.GetTokens()[strings[i].value].str()) << "\"" << std::endl;
        }
        indent_--;
      }

      if (opts_.max_strings > 0 && strings.size() > static_cast<size_t>(opts_.max_strings)) {
        out() << "# ... (" << (strings.size() - opts_.max_strings) << " more)" << std::endl;
      }

      indent_--;
    }

    indent_--;
  }

  void DumpFieldsYAML(const CrateReader& reader) {
    out() << "fields:" << std::endl;
    indent_++;

    const auto& fields = reader.GetFields();
    out() << "count: " << fields.size() << std::endl;

    if (!fields.empty()) {
      out() << "values:" << std::endl;
      indent_++;

      int emitted = 0;
      int limit = (opts_.max_fields > 0) ? opts_.max_fields : static_cast<int>(fields.size());

      for (size_t i = 0; i < fields.size() && emitted < limit; i++) {
        const auto& field = fields[i];
        std::string field_name;
        if (field.token_index.value < reader.GetTokens().size()) {
          field_name = reader.GetTokens()[field.token_index.value].str();
        }
        if (!ContainsFilter(field_name, opts_.token_filter)) {
          continue;
        }
        out() << "- index: " << i << std::endl;
        indent_++;

        out() << "token_index: " << field.token_index.value << std::endl;
        if (!field_name.empty()) {
          out() << "name: \"" << EscapeYAML(field_name) << "\"" << std::endl;
        }

        out() << "value_rep:" << std::endl;
        indent_++;
        DumpValueRepYAML(field.value_rep, reader);
        indent_--;

        indent_--;
        emitted++;
      }

      if (opts_.max_fields > 0 && emitted >= opts_.max_fields) {
        out() << "# ... (field output limited)" << std::endl;
      }

      indent_--;
    }

    indent_--;
  }

  void DumpValueRepYAML(const ValueRep& rep, const CrateReader& reader) {
    out() << "data: 0x" << std::hex << rep.GetData() << std::dec << std::endl;
    out() << "type_code: " << rep.GetType() << std::endl;
    out() << "is_inlined: " << rep.IsInlined() << std::endl;
    out() << "is_compressed: " << rep.IsCompressed() << std::endl;
    out() << "is_array: " << rep.IsArray() << std::endl;

    // Show payload (offset for non-inlined values)
    uint64_t payload = rep.GetPayload();
    out() << "payload: " << payload;
    if (!rep.IsInlined()) {
      out() << " # byte offset to data";
    }
    out() << std::endl;

    // Try to get the actual type name
    std::string type_str = "unknown";
    int type_code = rep.GetType();

    // Map type code to name
    auto type_result = GetCrateDataType(type_code);
    if (type_result) {
      type_str = type_result.value().name ? type_result.value().name : "unnamed";
    } else {
      type_str = "INVALID_TYPE_" + std::to_string(type_code);
      if (type_code > 56) {
        out() << "error: \"Type code " << type_code << " exceeds max valid type (56)\"" << std::endl;
      }
    }
    out() << "type_info: \"" << type_str << "\"" << std::endl;
  }

  void DumpFieldSetsYAML(const CrateReader& reader) {
    out() << "fieldsets:" << std::endl;
    indent_++;

    const auto& fieldsets = reader.GetFieldsetIndices();
    out() << "raw_count: " << fieldsets.size() << std::endl;

    // Count actual fieldsets (separated by sentinels)
    int fieldset_count = 0;
    for (const auto& idx : fieldsets) {
      if (idx.value == ~0u) fieldset_count++;
    }
    out() << "fieldset_count: " << fieldset_count << std::endl;

    if (!fieldsets.empty()) {
      out() << "values:" << std::endl;
      indent_++;

      int current_fieldset = 0;
      int limit = (opts_.max_fieldsets > 0) ? opts_.max_fieldsets : fieldset_count;

      std::vector<uint32_t> current_fields;
      int offset = 0;

      for (size_t i = 0; i < fieldsets.size() && current_fieldset < limit; i++) {
        if (fieldsets[i].value == ~0u) {
          // End of fieldset
          out() << "- index: " << current_fieldset << std::endl;
          indent_++;
          out() << "offset: " << offset << std::endl;
          out() << "field_count: " << current_fields.size() << std::endl;
          out() << "field_indices: [";
          for (size_t j = 0; j < current_fields.size(); j++) {
            if (j > 0) std::cout << ", ";
            std::cout << current_fields[j];
          }
          std::cout << "]" << std::endl;
          indent_--;

          current_fields.clear();
          current_fieldset++;
          offset = i + 1;
        } else {
          current_fields.push_back(fieldsets[i].value);
        }
      }

      if (opts_.max_fieldsets > 0 && fieldset_count > opts_.max_fieldsets) {
        out() << "# ... (" << (fieldset_count - opts_.max_fieldsets) << " more)" << std::endl;
      }

      indent_--;
    }

    indent_--;
  }

  void DumpPathsYAML(const CrateReader& reader) {
    out() << "paths:" << std::endl;
    indent_++;

    const auto& paths = reader.GetPaths();
    out() << "count: " << paths.size() << std::endl;

    if (!paths.empty()) {
      out() << "values:" << std::endl;
      indent_++;

      int emitted = 0;
      int limit = (opts_.max_paths > 0) ? opts_.max_paths : static_cast<int>(paths.size());

      for (size_t i = 0; i < paths.size() && emitted < limit; i++) {
        const auto& path = paths[i];
        std::string full_path = CratePathString(path);
        if (!ContainsFilter(full_path, opts_.path_filter)) {
          continue;
        }
        out() << "- index: " << i << std::endl;
        indent_++;
        out() << "prim: \"" << EscapeYAML(path.full_path_name()) << "\"" << std::endl;
        if (!path.prop_part().empty()) {
          out() << "property: \"" << EscapeYAML(path.prop_part()) << "\"" << std::endl;
        }
        indent_--;
        emitted++;
      }

      if (opts_.max_paths > 0 && emitted >= opts_.max_paths) {
        out() << "# ... (path output limited)" << std::endl;
      }

      indent_--;
    }

    indent_--;
  }

  void DumpSpecsYAML(const CrateReader& reader) {
    out() << "specs:" << std::endl;
    indent_++;

    const auto& specs = reader.GetSpecs();
    out() << "count: " << specs.size() << std::endl;

    if (!specs.empty()) {
      out() << "values:" << std::endl;
      indent_++;

      int emitted = 0;
      int limit = (opts_.max_specs > 0) ? opts_.max_specs : static_cast<int>(specs.size());

      for (size_t i = 0; i < specs.size() && emitted < limit; i++) {
        const auto& spec = specs[i];
        std::string full_path;
        if (spec.path_index.value < reader.GetPaths().size()) {
          const auto& path = reader.GetPaths()[spec.path_index.value];
          full_path = CratePathString(path);
        }
        if (!ContainsFilter(full_path, opts_.path_filter)) {
          continue;
        }
        out() << "- index: " << i << std::endl;
        indent_++;

        out() << "path_index: " << spec.path_index.value << std::endl;
        if (spec.path_index.value < reader.GetPaths().size()) {
          const auto& path = reader.GetPaths()[spec.path_index.value];
          out() << "path: \"" << EscapeYAML(CratePathString(path)) << "\"" << std::endl;
        }

        out() << "fieldset_index: " << spec.fieldset_index.value << std::endl;
        out() << "spec_type: " << static_cast<int>(spec.spec_type) << std::endl;
        out() << "spec_type_name: \"" << GetSpecTypeName(spec.spec_type) << "\"" << std::endl;

        indent_--;
        emitted++;
      }

      if (opts_.max_specs > 0 && emitted >= opts_.max_specs) {
        out() << "# ... (spec output limited)" << std::endl;
      }

      indent_--;
    }

    indent_--;
  }

  void DumpJSON(const CrateReader& reader, const uint8_t* data, size_t size) {
    using Json = lightusd::minijson::Value;
    Json root = Json::object();
    root["schemaVersion"] = 1;
    root["file"] = filename_;
    root["size"] = uint64_t(size);
    // Values are serialized as structural records, never decoded into large
    // arrays. Hex strings retain all 64 bits for JavaScript consumers.
    auto hex = [](uint64_t v) {
      std::ostringstream s;
      s << "0x" << std::hex << v;
      return s.str();
    };
    auto u64 = [&](size_t offset) {
      uint64_t v = 0;
      if (offset <= size && size - offset >= 8) {
        for (unsigned i = 0; i < 8; ++i)
          v |= uint64_t(data[offset + i]) << (i * 8);
      }
      return v;
    };
    if (opts_.show_bootstrap && size >= 24) {
      root["bootstrap"] = Json{{"magic", std::string(reinterpret_cast<const char*>(data), 8)},
          {"version", Json::array({data[8], data[9], data[10]})},
          {"toc_offset", u64(16)}};
      if (opts_.show_hex) {
        std::ostringstream s;
        for (size_t i = 0; i < 16; ++i)
          s << std::hex << std::setw(2) << std::setfill('0') << unsigned(data[i]);
        root["bootstrap"]["hex"] = s.str();
      }
    }
    if (opts_.show_toc) {
      const uint64_t offset = u64(16);
      Json sections = Json::array();
      if (offset <= size && size - size_t(offset) >= 8) {
        const uint64_t count = u64(size_t(offset));
        size_t pos = size_t(offset) + 8;
        for (uint64_t i = 0; i < count && size - pos >= 32; ++i, pos += 32) {
          size_t n = 0;
          while (n < 16 && data[pos + n]) ++n;
          sections.push_back(Json{{"name", std::string(reinterpret_cast<const char*>(data + pos), n)},
              {"byte_offset", u64(pos + 16)}, {"size", u64(pos + 24)}});
        }
        root["table_of_contents"] = Json{{"byte_offset", offset},
            {"num_sections", count}, {"sections", sections}};
      }
    }
    auto table = [&](const char* name, size_t count, int limit, auto record) {
      Json values = Json::array();
      size_t matched = 0;
      for (size_t i = 0; i < count; ++i) {
        Json item = record(i);
        if (item.is_null()) continue;
        ++matched;
        if (limit <= 0 || values.size() < size_t(limit))
          values.push_back(std::move(item));
      }
      root[name] = Json{{"count", uint64_t(count)}, {"matched", uint64_t(matched)},
          {"truncated", values.size() < matched}, {"values", values}};
    };
    const auto& tokens = reader.GetTokens();
    const auto& paths = reader.GetPaths();
    if (opts_.show_tokens) table("tokens", tokens.size(), opts_.max_tokens, [&](size_t i) -> Json {
      if (!ContainsFilter(tokens[i].str(), opts_.token_filter)) return nullptr;
      return Json{{"index", uint64_t(i)}, {"value", tokens[i].str()}};
    });
    const auto& strings = reader.GetStringIndices();
    if (opts_.show_strings) table("strings", strings.size(), opts_.max_strings, [&](size_t i) {
      Json item{{"index", uint64_t(i)}, {"token_index", strings[i].value}};
      if (strings[i].value < tokens.size()) item["value"] = tokens[strings[i].value].str();
      return item;
    });
    const auto& fields = reader.GetFields();
    if (opts_.show_fields) table("fields", fields.size(), opts_.max_fields, [&](size_t i) -> Json {
      const auto& f = fields[i];
      const std::string name = f.token_index.value < tokens.size() ? tokens[f.token_index.value].str() : "";
      if (!ContainsFilter(name, opts_.token_filter)) return nullptr;
      const auto& r = f.value_rep;
      const auto type = GetCrateDataType(r.GetType());
      return Json{{"index", uint64_t(i)}, {"token_index", f.token_index.value}, {"name", name},
          {"value_rep", Json{{"data", hex(r.GetData())}, {"type_code", r.GetType()},
              {"type_info", type && type.value().name ? type.value().name : "unknown"},
              {"is_inlined", r.IsInlined()}, {"is_compressed", r.IsCompressed()},
              {"is_array", r.IsArray()}, {"payload", r.GetPayload()}}}};
    });
    if (opts_.show_fieldsets) {
      Json values = Json::array(), current = Json::array();
      const auto& indices = reader.GetFieldsetIndices();
      size_t count = 0, offset = 0;
      for (size_t i = 0; i < indices.size(); ++i) {
        if (indices[i].value != ~0u) { current.push_back(indices[i].value); continue; }
        if (opts_.max_fieldsets <= 0 || count < size_t(opts_.max_fieldsets))
          values.push_back(Json{{"index", uint64_t(count)}, {"offset", uint64_t(offset)},
              {"field_count", uint64_t(current.size())}, {"field_indices", current}});
        ++count;
        offset = i + 1;
        current = Json::array();
      }
      root["fieldsets"] = Json{{"raw_count", uint64_t(indices.size())},
          {"fieldset_count", uint64_t(count)}, {"truncated", values.size() < count}, {"values", values}};
    }
    if (opts_.show_paths) table("paths", paths.size(), opts_.max_paths, [&](size_t i) -> Json {
      if (!ContainsFilter(CratePathString(paths[i]), opts_.path_filter)) return nullptr;
      return Json{{"index", uint64_t(i)}, {"prim", paths[i].full_path_name()},
          {"property", std::string(paths[i].prop_part())}, {"path", CratePathString(paths[i])}};
    });
    const auto& specs = reader.GetSpecs();
    if (opts_.show_specs) table("specs", specs.size(), opts_.max_specs, [&](size_t i) -> Json {
      const auto& s = specs[i];
      const std::string path = s.path_index.value < paths.size() ? CratePathString(paths[s.path_index.value]) : "";
      if (!ContainsFilter(path, opts_.path_filter)) return nullptr;
      return Json{{"index", uint64_t(i)}, {"path_index", s.path_index.value}, {"path", path},
          {"fieldset_index", s.fieldset_index.value}, {"spec_type", int(s.spec_type)},
          {"spec_type_name", GetSpecTypeName(s.spec_type)}};
    });
    std::cout << root.dump(2) << '\n';
  }

  std::ostream& out() {
    for (int i = 0; i < indent_; i++) {
      std::cout << "  ";
    }
    return std::cout;
  }

  std::string EscapeYAML(const std::string& s) {
    std::string result;
    for (char c : s) {
      if (c == '"') result += "\\\"";
      else if (c == '\\') result += "\\\\";
      else if (c == '\n') result += "\\n";
      else if (c == '\r') result += "\\r";
      else if (c == '\t') result += "\\t";
      else result += c;
    }
    return result;
  }

  std::string filename_;
  DumpOptions opts_;
  int indent_;
};

void PrintUsage(const char* prog) {
  std::cout << "Usage: " << prog << " [options] <input.usdc>\n\n";
  std::cout << "Options:\n";
  std::cout << "  -f, --format FORMAT   Output format: yaml (default) or json\n";
  std::cout << "  --no-bootstrap        Skip bootstrap section\n";
  std::cout << "  --no-toc              Skip table of contents\n";
  std::cout << "  --no-tokens           Skip tokens section\n";
  std::cout << "  --no-strings          Skip strings section\n";
  std::cout << "  --no-fields           Skip fields section\n";
  std::cout << "  --no-fieldsets        Skip fieldsets section\n";
  std::cout << "  --no-paths            Skip paths section\n";
  std::cout << "  --no-specs            Skip specs section\n";
  std::cout << "  --hex                 Show hex dumps\n";
  std::cout << "  --limit-tokens N      Limit tokens output to N items\n";
  std::cout << "  --limit-strings N     Limit strings output to N items\n";
  std::cout << "  --limit-fields N      Limit fields output to N items\n";
  std::cout << "  --limit-fieldsets N   Limit fieldsets output to N items\n";
  std::cout << "  --limit-paths N       Limit paths output to N items\n";
  std::cout << "  --limit-specs N       Limit specs output to N items\n";
  std::cout << "  --path-filter TEXT    Only show paths/specs containing TEXT\n";
  std::cout << "  --token-filter TEXT   Only show tokens/fields containing TEXT\n";
  std::cout << "  --limit N             Limit token/field/path/spec output to N items\n";
  std::cout << "  -h, --help            Show this help\n";
  std::cout << "\nExample:\n";
  std::cout << "  " << prog << " model.usdc\n";
  std::cout << "  " << prog << " --limit-fields 10 --no-tokens model.usdc\n";
}

int main(int argc, char** argv) {
  if (argc < 2) {
    PrintUsage(argv[0]);
    return 1;
  }

  DumpOptions opts;
  std::string input_file;

  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];

    if (arg == "-h" || arg == "--help") {
      PrintUsage(argv[0]);
      return 0;
    } else if (arg == "-f" || arg == "--format") {
      if (i + 1 < argc) {
        std::string fmt = argv[++i];
        if (fmt == "json") opts.format = OutputFormat::JSON;
        else if (fmt == "yaml") opts.format = OutputFormat::YAML;
        else {
          std::cerr << "Unknown format: " << fmt << std::endl;
          return 1;
        }
      }
    } else if (arg == "--no-bootstrap") {
      opts.show_bootstrap = false;
    } else if (arg == "--no-toc") {
      opts.show_toc = false;
    } else if (arg == "--no-tokens") {
      opts.show_tokens = false;
    } else if (arg == "--no-strings") {
      opts.show_strings = false;
    } else if (arg == "--no-fields") {
      opts.show_fields = false;
    } else if (arg == "--no-fieldsets") {
      opts.show_fieldsets = false;
    } else if (arg == "--no-paths") {
      opts.show_paths = false;
    } else if (arg == "--no-specs") {
      opts.show_specs = false;
    } else if (arg == "--hex") {
      opts.show_hex = true;
    } else if (arg == "--limit-tokens" && i + 1 < argc) {
      opts.max_tokens = std::atoi(argv[++i]);
    } else if (arg == "--limit-strings" && i + 1 < argc) {
      opts.max_strings = std::atoi(argv[++i]);
    } else if (arg == "--limit-fields" && i + 1 < argc) {
      opts.max_fields = std::atoi(argv[++i]);
    } else if (arg == "--limit-fieldsets" && i + 1 < argc) {
      opts.max_fieldsets = std::atoi(argv[++i]);
    } else if (arg == "--limit-paths" && i + 1 < argc) {
      opts.max_paths = std::atoi(argv[++i]);
    } else if (arg == "--limit-specs" && i + 1 < argc) {
      opts.max_specs = std::atoi(argv[++i]);
    } else if (arg == "--path-filter" && i + 1 < argc) {
      opts.path_filter = argv[++i];
    } else if (arg == "--token-filter" && i + 1 < argc) {
      opts.token_filter = argv[++i];
    } else if (arg == "--limit" && i + 1 < argc) {
      int limit = std::atoi(argv[++i]);
      if (limit < 1) {
        std::cerr << "Invalid limit: " << limit << std::endl;
        return 1;
      }
      opts.max_tokens = limit;
      opts.max_fields = limit;
      opts.max_fieldsets = limit;
      opts.max_paths = limit;
      opts.max_specs = limit;
    } else if (arg[0] != '-') {
      input_file = arg;
    } else {
      std::cerr << "Unknown option: " << arg << std::endl;
      return 1;
    }
  }

  if (input_file.empty()) {
    std::cerr << "Error: No input file specified" << std::endl;
    PrintUsage(argv[0]);
    return 1;
  }

  CrateDumper dumper(input_file, opts);
  if (!dumper.Dump()) {
    return 1;
  }

  return 0;
}
