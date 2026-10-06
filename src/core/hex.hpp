#pragma once

#include "core/bytes.hpp"

#include <string>
#include <string_view>

namespace mcs {

// "19 4B C0 00" (uppercase, single spaces, no trailing space).
std::string toHex(const Bytes& data);

// Multi-line dump, 16 bytes per line, offset prefix:
//   0000: 19 4B C0 00 00 05 6B 02 01 00 00 00
std::string hexdump(const Bytes& data, std::string_view indent = "");

// Parses hex text. Accepts tokens separated by spaces, tabs or commas; each
// token may carry a 0x prefix and may hold several bytes ("194BC0").
// Examples: "19 4B C0", "0x19,0x4b", "194bc0". Throws ParseError.
Bytes parseHex(std::string_view text);

} // namespace mcs
