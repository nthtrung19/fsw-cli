#pragma once

#include "telemetry/tlm_def.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace mcs {

// The telemetry packets known for the active target, looked up by MID.
// Pure data: knows nothing about files, sockets or the UI.
class TelemetryCatalog {
public:
    // Validates the packet, fills in field offsets and adds it. Throws
    // ConfigError naming the packet on: bad names, duplicate MIDs or packet
    // names, duplicate field names, sizes that do not fit the type, enum
    // values on non-integer fields, event packets without the event fields.
    void add(TlmPacketDef packet);

    const TlmPacketDef* find(std::uint16_t mid) const;

    // In the order they were added (catalog file order).
    const std::vector<TlmPacketDef>& packets() const { return packets_; }

    // Distinct app names in catalog order.
    std::vector<std::string> apps() const;

private:
    std::vector<TlmPacketDef> packets_;
    std::unordered_map<std::uint16_t, std::size_t> byMid_;
};

} // namespace mcs
