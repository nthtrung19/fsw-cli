#pragma once

#include "catalog/catalog.hpp"

#include <filesystem>
#include <vector>

namespace mcs {

// Loads one app definition file (see config/catalog/ds.json and docs/DESIGN.md §4.8).
// Throws ConfigError with the file name and the location inside it.
AppDef loadAppFile(const std::filesystem::path& file);

// Loads and validates several app files into one catalog.
CommandCatalog loadCatalog(const std::vector<std::filesystem::path>& files);

} // namespace mcs
