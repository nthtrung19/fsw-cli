#pragma once

#include "catalog/command_def.hpp"
#include "core/bytes.hpp"

#include <string>
#include <vector>

namespace mcs {

// Turns user argument strings into payload bytes, following a CommandDef.
//
// Rules:
//  - argument count must equal the number of user-visible fields;
//  - integers: decimal, 0x hex, or an enum name (case-insensitive), checked
//    against the type limits and the field's min/max;
//  - floats: decimal notation, finite, within f32 range for f32 fields;
//  - strings: shorter than the field size (NUL-terminated, zero-filled);
//  - padding: zeros, never consumes an argument.
//
// Throws ParseError with a message that includes the command usage.
class PayloadEncoder {
public:
    explicit PayloadEncoder(Endian target) : endian_(target) {}

    Bytes encode(const CommandDef& command, const std::vector<std::string>& args) const;

    Endian endian() const { return endian_; }

private:
    Endian endian_;
};

} // namespace mcs
