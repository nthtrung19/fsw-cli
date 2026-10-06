#include "core/bytes.hpp"

#include "core/errors.hpp"
#include "core/text.hpp"

namespace mcs {

Endian parseEndian(std::string_view text)
{
    const std::string lower = toLower(text);
    if (lower == "little") {
        return Endian::Little;
    }
    if (lower == "big") {
        return Endian::Big;
    }
    throw ParseError("invalid endian '" + std::string(text) + "' (expected 'little' or 'big')");
}

std::string toString(Endian endian)
{
    return endian == Endian::Little ? "little" : "big";
}

} // namespace mcs
