// SPDX-License-Identifier: MIT
// Copyright 2025 - Present, Light Transport Entertainment, Inc.
//
#include "arg-parser.hh"
#include "str-util.hh"

namespace lightusd {
namespace argparser {

ArgParser::ArgParser() {}

void ArgParser::add_option(const std::string& name, bool has_value, const std::string& help) {
    Option opt;
    opt.name = name;
    opt.has_value = has_value;
    opt.is_set = false;
    opt.value = "";
    opt.help = help;
    options_[name] = opt;
}

bool ArgParser::parse(int argc, char** argv) {
    positional_args_.clear();
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.size() > 0 && arg[0] == '-') {
            // Option
            auto it = options_.find(arg);
            if (it == options_.end()) {
                // Unknown option
                return false;
            }
            it->second.is_set = true;
            if (it->second.has_value) {
                if (i + 1 < argc) {
                    it->second.value = argv[i + 1];
                    ++i;
                } else {
                    // Missing value
                    return false;
                }
            }
        } else {
            // Positional argument
            positional_args_.push_back(arg);
        }
    }
    return true;
}

bool ArgParser::is_set(const std::string& name) const {
    auto it = options_.find(name);
    if (it != options_.end()) {
        return it->second.is_set;
    }
    return false;
}

bool ArgParser::get(const std::string& name, std::string& value) const {
    auto it = options_.find(name);
    if (it != options_.end() && it->second.is_set && it->second.has_value) {
        value = it->second.value;
        return true;
    }
    return false;
}

bool ArgParser::get(const std::string& name, double& value) const {
    auto it = options_.find(name);
    if (it != options_.end() && it->second.is_set && it->second.has_value) {
        double d = lightusd::atof(it->second.value.c_str());
        value = d;
        return true;
    }
    return false;
}

const std::vector<std::string>& ArgParser::positional() const
    LIGHTUSD_LIFETIMEBOUND {
  return positional_args_;
}

void ArgParser::print_help() const {
    for (const auto& kv : options_) {
        const auto& opt = kv.second;
        std::string value_hint = opt.has_value ? " <value>" : "";
        printf("  %s%s\n      %s\n", opt.name.c_str(), value_hint.c_str(), opt.help.c_str());
    }
}

} // namespace argparser
} // namespace lightusd
