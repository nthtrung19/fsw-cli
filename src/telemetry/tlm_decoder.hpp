#pragma once

#include "core/bytes.hpp"
#include "telemetry/tlm_def.hpp"

#include <optional>
#include <string>
#include <vector>

namespace mcs {

// One decoded field, ready to display.
struct DecodedValue {
    std::string name;
    std::string text;                 // "12", "enable", "0x0001", "3.25 V", "CFE_ES"; "—" if missing
    std::optional<double> number;     // numeric fields only, for change direction / limits
    std::optional<std::int64_t> raw;  // integer fields only (enum value, counters)
    bool missing = false;             // payload too short for this field
};

// Decodes every visible field of `def` from a telemetry payload (bytes after
// the 12-byte header). Integers and floats are read in `endian`; strings are
// cut at the first NUL; enum values are shown by name ("enable"), unknown
// values as "? (7)". Fields beyond the end of a short payload are `missing`.
std::vector<DecodedValue> decodeTelemetry(const TlmPacketDef& def, const Bytes& payload,
                                          Endian endian);

} // namespace mcs
