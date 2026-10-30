#pragma once

#include "core/bytes.hpp"
#include "telemetry/ccsds_tlm_parser.hpp"
#include "telemetry/tlm_catalog.hpp"
#include "telemetry/tlm_decoder.hpp"

#include <vector>

namespace mcs {

// One received datagram, parsed and decoded once for every consumer (the
// store, the script-check history, ...).
struct DecodedPacket {
    const Bytes* datagram = nullptr;        // the received bytes, valid while the packet is handled
    TlmParseResult parsed;                  // no frame: not a valid telemetry packet
    const TlmPacketDef* def = nullptr;      // null: malformed or a MID without definition
    std::vector<DecodedValue> values;       // one per visible field of def
};

// Parses `datagram` and, when its MID is defined in `catalog`, decodes the
// payload. `datagram` must outlive the result.
DecodedPacket decodePacket(const TelemetryCatalog& catalog, const Bytes& datagram, Endian endian);

} // namespace mcs
