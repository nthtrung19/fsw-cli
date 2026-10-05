#pragma once

#include "core/bytes.hpp"

#include <cstdint>
#include <string>

namespace fswcli {

// A command independent of any protocol: who it is for (MID), which command
// (function code) and the already-encoded payload.
struct CommandMessage {
    std::uint16_t mid = 0;
    std::uint8_t cc = 0;
    Bytes payload;
};

// Result of building a packet: the bytes plus a short protocol-specific note
// for status lines and logs (e.g. "apid=0x14B seq=7").
struct BuiltPacket {
    Bytes bytes;
    std::string note;
};

} // namespace fswcli
