#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fswcli {

using Bytes = std::vector<std::uint8_t>;

// Byte order of multi-byte values.
enum class Endian { Little, Big };

// "little" / "big" (case-insensitive). Throws ParseError otherwise.
Endian parseEndian(std::string_view text);

// "little" / "big"
std::string toString(Endian endian);

} // namespace fswcli
