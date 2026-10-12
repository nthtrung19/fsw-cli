#pragma once

#include "telemetry/tlm_catalog.hpp"

#include <filesystem>
#include <vector>

namespace mcs {

// Loads one telemetry definition file (see config/telemetry/evs.json):
//
//   { "app": "ds", "help": "...",
//     "packets": [ { "name": "hk", "mid": "0x08B8", "help": "...", "event": false,
//                    "variable": false,
//                    "fields": [ { "name": "cmd_accepted", "type": "u8", "units": "", "hex": false,
//                                  "enum": { "name": value }, "help": "..." },
//                                { "name": "counts", "type": "u32", "count": 4 },
//                                { "name": "chan", "count": 2, "fields": [ ... ] },
//                                { "type": "padding", "size": 1 } ] } ] }
//
// "count" (N, or [N, M] row-major) repeats a field: counts_0 .. counts_3. An
// entry with "fields" is a nested struct: chan_0_<member>, chan_1_<member>.
// "variable": the payload may be longer than the fields (no length error).
//
// Throws ConfigError with the file name and the location inside it.
std::vector<TlmPacketDef> loadTlmFile(const std::filesystem::path& file);

// Loads and validates several files into one catalog.
TelemetryCatalog loadTelemetryCatalog(const std::vector<std::filesystem::path>& files);

} // namespace mcs
