#pragma once

#include "protocol/command_message.hpp"

#include <string>

namespace fswcli {

// Plug-in kind 1: builds a complete packet from a protocol-independent command.
// May keep per-packet state such as sequence counters.
//
// Implementations: CcsdsV1Format. To add one, implement this interface and
// register a factory in registry/builtin_plugins.cpp (see docs/DESIGN.md §6).
class IPacketFormat {
public:
    virtual ~IPacketFormat() = default;

    // Throws EncodeError if the message cannot be represented in this format.
    virtual BuiltPacket build(const CommandMessage& message) = 0;

    // Human-readable description, e.g. "ccsds_v1 (little-endian)".
    virtual std::string describe() const = 0;
};

} // namespace fswcli
