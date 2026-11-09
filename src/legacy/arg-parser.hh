// SPDX-License-Identifier: MIT
// Copyright 2025 - Present, Light Transport Entertainment, Inc.
//
// Simple arg parser.
//
#pragma once
#include <string>
#include <vector>

#include "tiny-hashmap.hh"

namespace lightusd {
namespace argparser {

struct Option {
    std::string name;
    std::string value;
    bool has_value;
    bool is_set;
    std::string help; // Help string for the option
};

class ArgParser {
public:
    ArgParser();

    // Register an option (e.g., "--input") with help string
    void add_option(const std::string& name, bool has_value, const std::string& help = "");

    // Parse argc/argv. Returns true on success, false on error.
    bool parse(int argc, char** argv);

    // Check if option is set
    bool is_set(const std::string& name) const;

    // Get value for option. Returns true if found, false otherwise.
    bool get(const std::string& name, std::string& value) const;

    // Get value for option as double. Returns true if found and conversion succeeds, false otherwise.
    bool get(const std::string& name, double& value) const;

    // Get positional arguments (non-option arguments)
    const std::vector<std::string>& positional() const LIGHTUSD_LIFETIMEBOUND;

    // Print help for all options
    void print_help() const;

private:
    lightusd::HashMap<std::string, Option> options_;
    std::vector<std::string> positional_args_;
};

} // namespace argparser
} // namespace lightusd
