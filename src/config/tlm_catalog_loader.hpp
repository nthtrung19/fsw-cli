#pragma once

#include "telemetry/tlm_catalog.hpp"

#include <filesystem>
#include <vector>

namespace mcs {

// Loads one telemetry definition file (see config/telemetry/evs.json):
//
//   { "app": "ds", "help": "...",
//     "packets": [ { "name": "hk", "mid": "0x08B8", "help": "...", "event": false,
//                    "fields": [ { "name": "cmd_accepted", "type": "u8", "units": "", "hex": false,
//                                  "enum": { "name": value }, "help": "..." },
//                                { "type": "padding", "size": 1 } ] } ] }
//
// Throws ConfigError with the file name and the location inside it.
std::vector<TlmPacketDef> loadTlmFile(const std::filesystem::path& file);

// Loads and validates several files into one catalog.
TelemetryCatalog loadTelemetryCatalog(const std::vector<std::filesystem::path>& files);

} // namespace mcs
