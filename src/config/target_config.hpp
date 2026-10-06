#pragma once

#include "core/bytes.hpp"
#include "core/options.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace mcs {

// A plug-in reference from the configuration: its registered type name and
// its options (every other key of the JSON object).
struct PluginSpec {
    std::string type;
    Options options;
};

// One target profile from targets.json.
struct TargetConfig {
    std::string name;
    std::string description;
    Endian endian = Endian::Little;
    std::vector<std::filesystem::path> catalogFiles;   // absolute or relative to cwd
    PluginSpec format;
    std::vector<PluginSpec> layers;
    PluginSpec transport;
};

struct TargetsFile {
    std::filesystem::path file;
    std::string defaultTarget;
    std::vector<TargetConfig> targets;   // in file order

    const TargetConfig* find(const std::string& name) const;
};

// Parses and validates targets.json. Catalog paths are resolved relative to
// the directory of the targets file. Throws ConfigError.
TargetsFile loadTargetsFile(const std::filesystem::path& file);

// The named target, or the default one if `name` is empty.
// Throws ConfigError listing the available targets if not found.
const TargetConfig& selectTarget(const TargetsFile& targets, const std::optional<std::string>& name);

} // namespace mcs
