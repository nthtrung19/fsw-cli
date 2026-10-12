#include "telemetry/tlm_decoder.hpp"

#include "core/text.hpp"

#include <cstring>
#include <iomanip>
#include <sstream>

namespace mcs {

namespace {

std::uint64_t readUnsigned(const Bytes& b, std::size_t at, std::size_t width, Endian endian)
{
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < width; ++i) {
        const std::size_t index = endian == Endian::Big ? at + i : at + width - 1 - i;
        v = (v << 8) | b[index];
    }
    return v;
}

std::int64_t signExtend(std::uint64_t v, std::size_t width)
{
    if (width >= 8) {
        return static_cast<std::int64_t>(v);
    }
    const std::uint64_t signBit = std::uint64_t{1} << (width * 8 - 1);
    if ((v & signBit) == 0) {
        return static_cast<std::int64_t>(v);
    }
    return static_cast<std::int64_t>(v | ~((signBit << 1) - 1));
}

std::string formatDouble(double v)
{
    std::ostringstream out;
    out << std::setprecision(6) << v;
    return out.str();
}

std::string withUnits(std::string text, const std::string& units)
{
    return units.empty() ? text : text + " " + units;
}

// `decimal` is the plain value text (unsigned fields may exceed int64).
std::string integerText(const TlmFieldDef& f, std::int64_t value, std::uint64_t bits,
                        const std::string& decimal)
{
    for (const auto& e : f.enumValues) {
        if (e.value == value) {
            return e.name;
        }
    }
    if (!f.enumValues.empty()) {
        return "? (" + std::to_string(value) + ")";
    }
    if (f.hex) {
        return toHexString(bits, static_cast<int>(f.size * 2));
    }
    return withUnits(decimal, f.units);
}

DecodedValue decodeField(const TlmFieldDef& f, const Bytes& payload, Endian endian)
{
    DecodedValue v;
    v.name = f.name;
    if (f.offset + f.size > payload.size()) {
        v.missing = true;
        v.text = "—";   // em dash
        return v;
    }

    if (f.type == FieldType::String) {
        const auto begin = payload.begin() + static_cast<std::ptrdiff_t>(f.offset);
        std::string s(begin, begin + static_cast<std::ptrdiff_t>(f.size));
        s.resize(std::strlen(s.c_str()));   // cut at the first NUL
        for (char& c : s) {
            if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) >= 0x7F) {
                c = '.';
            }
        }
        v.text = s;
        return v;
    }

    const std::uint64_t bits = readUnsigned(payload, f.offset, f.size, endian);
    if (isUnsignedInt(f.type)) {
        const auto value = static_cast<std::int64_t>(bits);   // raw wraps above 2^63; text does not
        v.raw = value;
        v.number = static_cast<double>(bits);
        v.text = integerText(f, value, bits, std::to_string(bits));
    } else if (isSignedInt(f.type)) {
        const std::int64_t value = signExtend(bits, f.size);
        v.raw = value;
        v.number = static_cast<double>(value);
        v.text = integerText(f, value, bits, std::to_string(value));
    } else if (f.type == FieldType::F32) {
        float x = 0;
        const auto u = static_cast<std::uint32_t>(bits);
        std::memcpy(&x, &u, sizeof x);
        v.number = static_cast<double>(x);
        v.text = withUnits(formatDouble(static_cast<double>(x)), f.units);
    } else if (f.type == FieldType::F64) {
        double x = 0;
        std::memcpy(&x, &bits, sizeof x);
        v.number = x;
        v.text = withUnits(formatDouble(x), f.units);
    }
    return v;
}

} // namespace

std::vector<DecodedValue> decodeTelemetry(const TlmPacketDef& def, const Bytes& payload,
                                          Endian endian)
{
    std::vector<DecodedValue> out;
    out.reserve(def.fields.size());
    for (const auto& f : def.fields) {
        if (f.visible()) {
            out.push_back(decodeField(f, payload, endian));
        }
    }
    return out;
}

} // namespace mcs
