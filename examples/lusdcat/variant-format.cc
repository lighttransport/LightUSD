#include "variant-format.hh"

#include <iomanip>
#include <vector>

namespace variant_format {

// Helper function to escape JSON string
static std::string escape_json_string(const std::string& str) {
  std::ostringstream oss;
  for (char c : str) {
    switch (c) {
      case '"':
        oss << "\\\"";
        break;
      case '\\':
        oss << "\\\\";
        break;
      case '\b':
        oss << "\\b";
        break;
      case '\f':
        oss << "\\f";
        break;
      case '\n':
        oss << "\\n";
        break;
      case '\r':
        oss << "\\r";
        break;
      case '\t':
        oss << "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          oss << "\\u" << std::setfill('0') << std::setw(4) << std::hex
              << static_cast<int>(static_cast<unsigned char>(c));
        } else {
          oss << c;
        }
    }
  }
  return oss.str();
}

// Helper to convert MetaVariable value to string for output
static std::string meta_value_to_string(const lightusd::legacy::MetaVariable& meta) {
  // Try to extract common types
  if (auto v = meta.get_value<std::string>()) {
    return *v;
  }
  if (auto v = meta.get_value<int32_t>()) {
    return std::to_string(*v);
  }
  if (auto v = meta.get_value<uint32_t>()) {
    return std::to_string(*v);
  }
  if (auto v = meta.get_value<int64_t>()) {
    return std::to_string(*v);
  }
  if (auto v = meta.get_value<uint64_t>()) {
    return std::to_string(*v);
  }
  if (auto v = meta.get_value<double>()) {
    std::ostringstream oss;
    oss << *v;
    return oss.str();
  }
  if (auto v = meta.get_value<float>()) {
    std::ostringstream oss;
    oss << *v;
    return oss.str();
  }
  if (auto v = meta.get_value<bool>()) {
    return *v ? "true" : "false";
  }
  if (auto v = meta.get_value<std::vector<std::string>>()) {
    std::ostringstream oss;
    oss << "[";
    for (size_t i = 0; i < v->size(); ++i) {
      if (i > 0) oss << ", ";
      oss << "\"" << escape_json_string((*v)[i]) << "\"";
    }
    oss << "]";
    return oss.str();
  }
  if (auto v = meta.get_value<std::vector<int32_t>>()) {
    std::ostringstream oss;
    oss << "[";
    for (size_t i = 0; i < v->size(); ++i) {
      if (i > 0) oss << ", ";
      oss << (*v)[i];
    }
    oss << "]";
    return oss.str();
  }
  if (auto v = meta.get_value<std::vector<double>>()) {
    std::ostringstream oss;
    oss << "[";
    for (size_t i = 0; i < v->size(); ++i) {
      if (i > 0) oss << ", ";
      oss << (*v)[i];
    }
    oss << "]";
    return oss.str();
  }

  // Fallback to type name
  return meta.type_name();
}

// Helper function to convert a value to YAML string
static std::string meta_value_to_yaml(const lightusd::legacy::MetaVariable& meta) {
  std::string str_val = meta_value_to_string(meta);

  // For strings, arrays, and complex types, quote the value
  if (auto v = meta.get_value<std::string>()) {
    return "\"" + escape_json_string(str_val) + "\"";
  }
  if (auto v = meta.get_value<std::vector<std::string>>()) {
    return str_val;  // Already formatted as [...]
  }
  if (auto v = meta.get_value<std::vector<int32_t>>()) {
    return str_val;  // Already formatted as [...]
  }
  if (auto v = meta.get_value<std::vector<double>>()) {
    return str_val;  // Already formatted as [...]
  }

  return str_val;
}

// Helper function to convert a value to JSON string
static std::string meta_value_to_json(const lightusd::legacy::MetaVariable& meta) {
  // Try to extract common types
  if (auto v = meta.get_value<std::string>()) {
    return "\"" + escape_json_string(*v) + "\"";
  }
  if (auto v = meta.get_value<int32_t>()) {
    return std::to_string(*v);
  }
  if (auto v = meta.get_value<uint32_t>()) {
    return std::to_string(*v);
  }
  if (auto v = meta.get_value<int64_t>()) {
    return std::to_string(*v);
  }
  if (auto v = meta.get_value<uint64_t>()) {
    return std::to_string(*v);
  }
  if (auto v = meta.get_value<double>()) {
    std::ostringstream oss;
    oss << *v;
    return oss.str();
  }
  if (auto v = meta.get_value<float>()) {
    std::ostringstream oss;
    oss << *v;
    return oss.str();
  }
  if (auto v = meta.get_value<bool>()) {
    return *v ? "true" : "false";
  }
  if (auto v = meta.get_value<std::vector<std::string>>()) {
    std::ostringstream oss;
    oss << "[";
    for (size_t i = 0; i < v->size(); ++i) {
      if (i > 0) oss << ", ";
      oss << "\"" << escape_json_string((*v)[i]) << "\"";
    }
    oss << "]";
    return oss.str();
  }
  if (auto v = meta.get_value<std::vector<int32_t>>()) {
    std::ostringstream oss;
    oss << "[";
    for (size_t i = 0; i < v->size(); ++i) {
      if (i > 0) oss << ", ";
      oss << (*v)[i];
    }
    oss << "]";
    return oss.str();
  }
  if (auto v = meta.get_value<std::vector<double>>()) {
    std::ostringstream oss;
    oss << "[";
    for (size_t i = 0; i < v->size(); ++i) {
      if (i > 0) oss << ", ";
      oss << (*v)[i];
    }
    oss << "]";
    return oss.str();
  }

  // Fallback
  return "\"" + escape_json_string(meta.type_name()) + "\"";
}

std::string dictionary_to_yaml(const lightusd::legacy::Dictionary& dict) {
  std::ostringstream oss;

  for (const auto& item : dict) {
    const std::string& key = item.first;
    const lightusd::legacy::MetaVariable& meta = item.second;

    // Check if value is a nested Dictionary
    if (auto nested_dict = meta.get_value<lightusd::legacy::Dictionary>()) {
      oss << key << ":\n";
      std::string nested_yaml = dictionary_to_yaml(*nested_dict);
      // Indent nested content
      std::istringstream iss(nested_yaml);
      std::string line;
      while (std::getline(iss, line)) {
        oss << "  " << line << "\n";
      }
    } else {
      oss << key << ": " << meta_value_to_yaml(meta) << "\n";
    }
  }

  return oss.str();
}

std::string dictionary_to_json(const lightusd::legacy::Dictionary& dict) {
  std::ostringstream oss;
  oss << "{\n";

  bool first = true;
  for (const auto& item : dict) {
    const std::string& key = item.first;
    const lightusd::legacy::MetaVariable& meta = item.second;

    if (!first) {
      oss << ",\n";
    }
    first = false;

    oss << "  \"" << escape_json_string(key) << "\": ";

    // Check if value is a nested Dictionary
    if (auto nested_dict = meta.get_value<lightusd::legacy::Dictionary>()) {
      std::string nested_json = dictionary_to_json(*nested_dict);
      // Indent nested JSON
      std::istringstream iss(nested_json);
      std::string line;
      bool first_line = true;
      while (std::getline(iss, line)) {
        if (!first_line) {
          oss << "\n    ";
        }
        if (first_line) {
          oss << line;
          first_line = false;
        } else {
          oss << line;
        }
      }
    } else {
      oss << meta_value_to_json(meta);
    }
  }

  oss << "\n}";
  return oss.str();
}

}  // namespace variant_format
