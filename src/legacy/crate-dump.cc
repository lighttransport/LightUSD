// SPDX-License-Identifier: Apache 2.0
// Copyright 2025, Light Transport Entertainment Inc.
//
// LightUSD Crate Dump Library Implementation
//

#include "core/native-io.hh"
#include "crate-dump.hh"

#include <iostream>
#include <fstream>
#include <iomanip>
#include <vector>
#include <cstring>
#include <algorithm>

#include "stream-reader.hh"
#include "crate-format.hh"
#include "crate-reader.hh"
#include "value-types.hh"
#include "core/prim-enums.hh"

// Disable specific clang warnings for this file
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wcovered-switch-default"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wsign-conversion"
#pragma clang diagnostic ignored "-Wshorten-64-to-32"
#endif

namespace lightusd {
namespace crate {

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

// Helper to escape YAML strings
static std::string EscapeYAML(const std::string& s) {
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

// Helper for indented output
class IndentedOutput {
public:
  IndentedOutput() : indent_(0) {}

  std::ostream& out() {
    for (int i = 0; i < indent_; i++) {
      std::cout << "  ";
    }
    return std::cout;
  }

  void indent() { indent_++; }
  void dedent() { indent_--; }

private:
  int indent_;
};

static void DumpValueRepYAML(const ValueRep& rep, const CrateReader& reader, IndentedOutput& io) {
  io.out() << "data: 0x" << std::hex << rep.GetData() << std::dec << std::endl;
  io.out() << "type_code: " << rep.GetType() << std::endl;
  io.out() << "is_inlined: " << rep.IsInlined() << std::endl;
  io.out() << "is_compressed: " << rep.IsCompressed() << std::endl;
  io.out() << "is_array: " << rep.IsArray() << std::endl;

  // Show payload (offset for non-inlined values)
  uint64_t payload = rep.GetPayload();
  io.out() << "payload: " << payload;
  if (!rep.IsInlined()) {
    std::cout << " # byte offset to data";
  }
  std::cout << std::endl;

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
      io.out() << "error: \"Type code " << type_code << " exceeds max valid type (56)\"" << std::endl;
    }
  }
  io.out() << "type_info: \"" << type_str << "\"" << std::endl;
}

static void DumpBootstrapYAML(const uint8_t* data, const DumpOptions& opts, IndentedOutput& io) {
  io.out() << "bootstrap:" << std::endl;
  io.indent();
  io.out() << "byte_offset: 0" << std::endl;
  io.out() << "byte_size: 72" << std::endl;

  // Read bootstrap header (72 bytes) - manually extract fields
  char ident[9] = {0};
  memcpy(ident, data, 8);
  io.out() << "magic: \"" << ident << "\"" << std::endl;

  uint8_t version[3];
  memcpy(version, data + 8, 3);
  io.out() << "version: [" << static_cast<int>(version[0]) << ", "
        << static_cast<int>(version[1]) << ", "
        << static_cast<int>(version[2]) << "]" << std::endl;

  uint64_t toc_offset;
  memcpy(&toc_offset, data + 16, 8);
  io.out() << "toc_offset: " << toc_offset << std::endl;

  if (opts.show_hex) {
    io.out() << "hex: ";
    for (size_t i = 0; i < 16; i++) {
      std::cout << std::hex << std::setw(2) << std::setfill('0')
                << static_cast<int>(data[i]) << " ";
    }
    std::cout << std::dec << std::endl;
  }

  io.dedent();
}

static void DumpTOCYAML(const CrateReader& reader, const uint8_t* data, size_t size, IndentedOutput& io) {
  io.out() << "table_of_contents:" << std::endl;
  io.indent();

  // Read TOC offset from bootstrap
  uint64_t toc_offset;
  memcpy(&toc_offset, data + 16, 8);
  io.out() << "byte_offset: " << toc_offset << std::endl;

  if (toc_offset >= size) {
    io.out() << "error: \"TOC offset beyond file size\"" << std::endl;
    io.dedent();
    return;
  }

  // Read number of sections (uint64_t at TOC offset)
  uint64_t num_sections;
  memcpy(&num_sections, data + toc_offset, 8);
  io.out() << "num_sections: " << num_sections << std::endl;

  if (num_sections > 0 && num_sections < 100) {
    io.out() << "sections:" << std::endl;
    io.indent();

    size_t section_offset = toc_offset + 8;
    for (uint64_t i = 0; i < num_sections; i++) {
      if (section_offset + 32 > size) break;

      // Section structure: char name[16] + int64_t start + int64_t size
      char name[17] = {0};
      memcpy(name, data + section_offset, 16);

      int64_t start, sec_size;
      memcpy(&start, data + section_offset + 16, 8);
      memcpy(&sec_size, data + section_offset + 24, 8);

      io.out() << "- name: \"" << name << "\"" << std::endl;
      io.indent();
      io.out() << "byte_offset: " << start << std::endl;
      io.out() << "byte_size: " << sec_size << std::endl;

      // Show element counts for known sections
      if (strcmp(name, "TOKENS") == 0) {
        io.out() << "num_elements: " << reader.GetTokens().size() << std::endl;
      } else if (strcmp(name, "STRINGS") == 0) {
        io.out() << "num_elements: " << reader.GetStringIndices().size() << std::endl;
      } else if (strcmp(name, "FIELDS") == 0) {
        io.out() << "num_elements: " << reader.GetFields().size() << std::endl;
      } else if (strcmp(name, "FIELDSETS") == 0) {
        io.out() << "num_elements: " << reader.GetFieldsetIndices().size() << std::endl;
      } else if (strcmp(name, "PATHS") == 0) {
        io.out() << "num_elements: " << reader.GetPaths().size() << std::endl;
      } else if (strcmp(name, "SPECS") == 0) {
        io.out() << "num_elements: " << reader.GetSpecs().size() << std::endl;
      }
      io.dedent();

      section_offset += 32;
    }

    io.dedent();
  }

  io.dedent();
}

static void DumpTokensYAML(const CrateReader& reader, const DumpOptions& opts, IndentedOutput& io) {
  io.out() << "tokens:" << std::endl;
  io.indent();

  const auto& tokens = reader.GetTokens();
  io.out() << "count: " << tokens.size() << std::endl;

  if (!tokens.empty()) {
    io.out() << "values:" << std::endl;
    io.indent();

    int emitted = 0;
    int limit = (opts.max_tokens > 0) ? opts.max_tokens : static_cast<int>(tokens.size());

    for (size_t i = 0; i < tokens.size() && emitted < limit; i++) {
      if (!ContainsFilter(tokens[i].str(), opts.token_filter)) {
        continue;
      }
      io.out() << "- index: " << i << std::endl;
      io.indent();
      io.out() << "value: \"" << EscapeYAML(tokens[i].str()) << "\"" << std::endl;
      io.dedent();
      emitted++;
    }

    if (opts.max_tokens > 0 && emitted >= opts.max_tokens) {
      io.out() << "# ... (token output limited";
      if (!opts.token_filter.empty()) {
        std::cout << ", filter=\"" << EscapeYAML(opts.token_filter) << "\"";
      }
      std::cout << ")" << std::endl;
    }

    io.dedent();
  }

  io.dedent();
}

static void DumpStringsYAML(const CrateReader& reader, const DumpOptions& opts, IndentedOutput& io) {
  io.out() << "strings:" << std::endl;
  io.indent();

  const auto& strings = reader.GetStringIndices();
  io.out() << "count: " << strings.size() << std::endl;

  if (!strings.empty()) {
    io.out() << "values:" << std::endl;
    io.indent();

    int limit = (opts.max_strings > 0) ? std::min(opts.max_strings, static_cast<int>(strings.size()))
                                       : strings.size();

    for (int i = 0; i < limit; i++) {
      io.out() << "- index: " << i << std::endl;
      io.indent();
      io.out() << "token_index: " << strings[i].value << std::endl;
      if (strings[i].value < reader.GetTokens().size()) {
        io.out() << "value: \"" << EscapeYAML(reader.GetTokens()[strings[i].value].str()) << "\"" << std::endl;
      }
      io.dedent();
    }

    if (opts.max_strings > 0 && strings.size() > static_cast<size_t>(opts.max_strings)) {
      io.out() << "# ... (" << (strings.size() - opts.max_strings) << " more)" << std::endl;
    }

    io.dedent();
  }

  io.dedent();
}

static void DumpFieldsYAML(const CrateReader& reader, const DumpOptions& opts, IndentedOutput& io) {
  io.out() << "fields:" << std::endl;
  io.indent();

  const auto& fields = reader.GetFields();
  io.out() << "count: " << fields.size() << std::endl;

  if (!fields.empty()) {
    io.out() << "values:" << std::endl;
    io.indent();

    int emitted = 0;
    int limit = (opts.max_fields > 0) ? opts.max_fields : static_cast<int>(fields.size());

    for (size_t i = 0; i < fields.size() && emitted < limit; i++) {
      const auto& field = fields[i];
      std::string field_name;
      if (field.token_index.value < reader.GetTokens().size()) {
        field_name = reader.GetTokens()[field.token_index.value].str();
      }
      if (!ContainsFilter(field_name, opts.token_filter)) {
        continue;
      }
      io.out() << "- index: " << i << std::endl;
      io.indent();

      io.out() << "token_index: " << field.token_index.value << std::endl;
      if (!field_name.empty()) {
        io.out() << "name: \"" << EscapeYAML(field_name) << "\"" << std::endl;
      }

      io.out() << "value_rep:" << std::endl;
      io.indent();
      DumpValueRepYAML(field.value_rep, reader, io);
      io.dedent();

      io.dedent();
      emitted++;
    }

    if (opts.max_fields > 0 && emitted >= opts.max_fields) {
      io.out() << "# ... (field output limited";
      if (!opts.token_filter.empty()) {
        std::cout << ", filter=\"" << EscapeYAML(opts.token_filter) << "\"";
      }
      std::cout << ")" << std::endl;
    }

    io.dedent();
  }

  io.dedent();
}

static void DumpFieldSetsYAML(const CrateReader& reader, const DumpOptions& opts, IndentedOutput& io) {
  io.out() << "fieldsets:" << std::endl;
  io.indent();

  const auto& fieldsets = reader.GetFieldsetIndices();
  io.out() << "raw_count: " << fieldsets.size() << std::endl;

  // Count actual fieldsets (separated by sentinels)
  int fieldset_count = 0;
  for (const auto& idx : fieldsets) {
    if (idx.value == ~0u) fieldset_count++;
  }
  io.out() << "fieldset_count: " << fieldset_count << std::endl;

  if (!fieldsets.empty()) {
    io.out() << "values:" << std::endl;
    io.indent();

    int current_fieldset = 0;
    int limit = (opts.max_fieldsets > 0) ? opts.max_fieldsets : fieldset_count;

    std::vector<uint32_t> current_fields;
    int offset = 0;

    for (size_t i = 0; i < fieldsets.size() && current_fieldset < limit; i++) {
      if (fieldsets[i].value == ~0u) {
        // End of fieldset
        io.out() << "- index: " << current_fieldset << std::endl;
        io.indent();
        io.out() << "offset: " << offset << std::endl;
        io.out() << "field_count: " << current_fields.size() << std::endl;
        io.out() << "field_indices: [";
        for (size_t j = 0; j < current_fields.size(); j++) {
          if (j > 0) std::cout << ", ";
          std::cout << current_fields[j];
        }
        std::cout << "]" << std::endl;
        io.dedent();

        current_fields.clear();
        current_fieldset++;
        offset = i + 1;
      } else {
        current_fields.push_back(fieldsets[i].value);
      }
    }

    if (opts.max_fieldsets > 0 && fieldset_count > opts.max_fieldsets) {
      io.out() << "# ... (" << (fieldset_count - opts.max_fieldsets) << " more)" << std::endl;
    }

    io.dedent();
  }

  io.dedent();
}

static void DumpPathsYAML(const CrateReader& reader, const DumpOptions& opts, IndentedOutput& io) {
  io.out() << "paths:" << std::endl;
  io.indent();

  const auto& paths = reader.GetPaths();
  io.out() << "count: " << paths.size() << std::endl;

  if (!paths.empty()) {
    io.out() << "values:" << std::endl;
    io.indent();

    int emitted = 0;
    int limit = (opts.max_paths > 0) ? opts.max_paths : static_cast<int>(paths.size());

    for (size_t i = 0; i < paths.size() && emitted < limit; i++) {
      const auto& path = paths[i];
      std::string full_path = CratePathString(path);
      if (!ContainsFilter(full_path, opts.path_filter)) {
        continue;
      }
      io.out() << "- index: " << i << std::endl;
      io.indent();
      io.out() << "prim: \"" << EscapeYAML(path.full_path_name()) << "\"" << std::endl;
      if (!path.prop_part().empty()) {
        io.out() << "property: \"" << EscapeYAML(path.prop_part()) << "\"" << std::endl;
      }
      io.dedent();
      emitted++;
    }

    if (opts.max_paths > 0 && emitted >= opts.max_paths) {
      io.out() << "# ... (path output limited";
      if (!opts.path_filter.empty()) {
        std::cout << ", filter=\"" << EscapeYAML(opts.path_filter) << "\"";
      }
      std::cout << ")" << std::endl;
    }

    io.dedent();
  }

  io.dedent();
}

static void DumpSpecsYAML(const CrateReader& reader, const DumpOptions& opts, IndentedOutput& io) {
  io.out() << "specs:" << std::endl;
  io.indent();

  const auto& specs = reader.GetSpecs();
  io.out() << "count: " << specs.size() << std::endl;

  if (!specs.empty()) {
    io.out() << "values:" << std::endl;
    io.indent();

    int emitted = 0;
    int limit = (opts.max_specs > 0) ? opts.max_specs : static_cast<int>(specs.size());

    for (size_t i = 0; i < specs.size() && emitted < limit; i++) {
      const auto& spec = specs[i];
      std::string full_path;
      if (spec.path_index.value < reader.GetPaths().size()) {
        const auto& path = reader.GetPaths()[spec.path_index.value];
        full_path = CratePathString(path);
      }
      if (!ContainsFilter(full_path, opts.path_filter)) {
        continue;
      }
      io.out() << "- index: " << i << std::endl;
      io.indent();

      io.out() << "path_index: " << spec.path_index.value << std::endl;
      if (spec.path_index.value < reader.GetPaths().size()) {
        const auto& path = reader.GetPaths()[spec.path_index.value];
        io.out() << "path: \"" << EscapeYAML(CratePathString(path)) << "\"" << std::endl;
      }

      io.out() << "fieldset_index: " << spec.fieldset_index.value << std::endl;
      io.out() << "spec_type: " << static_cast<int>(spec.spec_type) << std::endl;
      io.out() << "spec_type_name: \"" << GetSpecTypeName(spec.spec_type) << "\"" << std::endl;

      io.dedent();
      emitted++;
    }

    if (opts.max_specs > 0 && emitted >= opts.max_specs) {
      io.out() << "# ... (spec output limited";
      if (!opts.path_filter.empty()) {
        std::cout << ", filter=\"" << EscapeYAML(opts.path_filter) << "\"";
      }
      std::cout << ")" << std::endl;
    }

    io.dedent();
  }

  io.dedent();
}

bool DumpCrate(const std::string& filename, const DumpOptions& opts, std::string* err) {
  if (!::lightusd::NativeIOAvailable(err)) return false;

  // Open and read the file
  std::ifstream ifs(filename, std::ios::binary);
  if (!ifs) {
    if (err) *err = "Cannot open file: " + filename;
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
    if (err) *err = "Failed to read bootstrap";
    return false;
  }

  if (!reader.ReadTOC()) {
    if (err) *err = "Failed to read TOC";
    return false;
  }

  if (!reader.ReadTokens()) {
    if (err) *err = "Failed to read tokens";
    return false;
  }

  if (!reader.ReadStrings()) {
    if (err) *err = "Failed to read strings";
    return false;
  }

  if (!reader.ReadFields()) {
    if (err) *err = "Failed to read fields";
    return false;
  }

  if (!reader.ReadFieldSets()) {
    if (err) *err = "Failed to read fieldsets";
    return false;
  }

  if (!reader.ReadPaths()) {
    if (err) *err = "Failed to read paths";
    return false;
  }

  if (!reader.ReadSpecs()) {
    if (err) *err = "Failed to read specs";
    return false;
  }

  // Output
  IndentedOutput io;

  if (opts.format == OutputFormat::YAML) {
    io.out() << "usdc_crate:" << std::endl;
    io.indent();

    io.out() << "file: \"" << filename << "\"" << std::endl;
    io.out() << "size: " << file_size << std::endl;

    if (opts.show_bootstrap) {
      DumpBootstrapYAML(data.data(), opts, io);
    }

    if (opts.show_toc) {
      DumpTOCYAML(reader, data.data(), file_size, io);
    }

    if (opts.show_tokens) {
      DumpTokensYAML(reader, opts, io);
    }

    if (opts.show_strings) {
      DumpStringsYAML(reader, opts, io);
    }

    if (opts.show_fields) {
      DumpFieldsYAML(reader, opts, io);
    }

    if (opts.show_fieldsets) {
      DumpFieldSetsYAML(reader, opts, io);
    }

    if (opts.show_paths) {
      DumpPathsYAML(reader, opts, io);
    }

    if (opts.show_specs) {
      DumpSpecsYAML(reader, opts, io);
    }

    io.dedent();
  } else {
    // JSON implementation (not yet implemented)
    if (err) *err = "JSON output not yet implemented";
    return false;
  }

  return true;
}

}  // namespace crate
}  // namespace lightusd

#if defined(__clang__)
#pragma clang diagnostic pop
#endif
