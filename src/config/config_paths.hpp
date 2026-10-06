#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace mcs {

// Where the targets file is looked for, in order:
//   1. the --config option (must exist if given)
//   2. $mcs_CONFIG
//   3. ./config/targets.json
//   4. <exe dir>/../config/targets.json and <exe dir>/../../config/targets.json
//      (build tree: build/src/mcs -> <repo>/config)
//   5. <exe dir>/../share/mcs/targets.json (installed layout)
std::vector<std::filesystem::path> targetsFileCandidates(const std::optional<std::string>& explicitPath);

// First existing candidate. Throws ConfigError listing every place searched.
std::filesystem::path findTargetsFile(const std::optional<std::string>& explicitPath);

} // namespace mcs
