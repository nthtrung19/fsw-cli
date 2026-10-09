#pragma once

#include "catalog/command_def.hpp"
#include "core/bytes.hpp"

#include <string>
#include <vector>

namespace mcs {

// Turns user argument strings into payload bytes, following a CommandDef.
//
// Rules:
//  - argument count must be between the number of required fields and the
//    number of user-visible fields; omitted trailing arguments take the
//    field's default, encoded as if it had been typed;
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

    // Encodes every field default on its own; throws ParseError for one that
    // is not a valid argument for its field.
    void checkDefaults(const CommandDef& command) const;

    Endian endian() const { return endian_; }

private:
    Endian endian_;
};

} // namespace mcs
