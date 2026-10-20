#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace mcs::cfdp {

// The "cfdp" block of a target (config/targets.json), as plain values.
// Defaults match CF 2.2.1 as built for the fsw target (cf_cfgtable.c, cf_msgids.h).
struct Config {
    std::string localEntity = "0.23";       // this ground entity (dotted decimal, 2 bytes)
    std::string remoteEntity = "0.24";      // CF's entity
    std::uint16_t uplinkMid = 0x192D;       // CF_INCOMING_PDU_MID: PDUs sent as commands
    std::uint16_t downlinkMid = 0x092D;     // CF_SPACE_TO_GND_PDU_MID: PDUs received as telemetry
    std::size_t chunkSize = 512;            // FileData bytes per uplink PDU (CI datagram limit)
    unsigned pdusPerSecond = 100;           // uplink FileData pacing; directives are not paced
    std::filesystem::path downlinkDir = "downlink";
    unsigned ackTimeout = 10;               // seconds
    unsigned ackLimit = 2;
    unsigned nakTimeout = 5;                // seconds
    unsigned nakLimit = 3;
    unsigned inactivityTimeout = 30;        // seconds
    bool saveIncomplete = false;            // keep partial downlinks in downlinkDir/.part
};

} // namespace mcs::cfdp
