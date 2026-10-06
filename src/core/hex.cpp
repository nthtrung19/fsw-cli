#include "core/hex.hpp"

#include "core/errors.hpp"

#include <cctype>

namespace mcs {

namespace {

constexpr char kHexDigits[] = "0123456789ABCDEF";

void appendByte(std::string& out, std::uint8_t b)
{
    out += kHexDigits[b >> 4U];
    out += kHexDigits[b & 0xFU];
}

int hexValue(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

} // namespace

std::string toHex(const Bytes& data)
{
    std::string out;
    out.reserve(data.size() * 3);
    for (std::size_t i = 0; i < data.size(); ++i) {
        if (i != 0) {
            out += ' ';
        }
        appendByte(out, data[i]);
    }
    return out;
}

std::string hexdump(const Bytes& data, std::string_view indent)
{
    std::string out;
    for (std::size_t offset = 0; offset < data.size(); offset += 16) {
        out += indent;
        appendByte(out, static_cast<std::uint8_t>(offset >> 8U));
        appendByte(out, static_cast<std::uint8_t>(offset & 0xFFU));
        out += ':';
        for (std::size_t i = offset; i < data.size() && i < offset + 16; ++i) {
            out += ' ';
            appendByte(out, data[i]);
        }
        out += '\n';
    }
    return out;
}

Bytes parseHex(std::string_view text)
{
    Bytes out;
    std::size_t i = 0;
    while (i < text.size()) {
        const char c = text[i];
        if (c == ' ' || c == '\t' || c == ',') {
            ++i;
            continue;
        }
        // One token: optional 0x prefix, then an even number of hex digits.
        std::size_t start = i;
        if (c == '0' && i + 1 < text.size() && (text[i + 1] == 'x' || text[i + 1] == 'X')) {
            start = i + 2;
        }
        std::size_t end = start;
        while (end < text.size() && text[end] != ' ' && text[end] != '\t' && text[end] != ',') {
            ++end;
        }
        const std::string_view token = text.substr(start, end - start);
        if (token.empty() || token.size() % 2 != 0) {
            throw ParseError("invalid hex token '" + std::string(text.substr(i, end - i))
                             + "' (need an even number of hex digits)");
        }
        for (std::size_t k = 0; k < token.size(); k += 2) {
            const int hi = hexValue(token[k]);
            const int lo = hexValue(token[k + 1]);
            if (hi < 0 || lo < 0) {
                throw ParseError("invalid hex digit in '" + std::string(token) + "'");
            }
            out.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
        }
        i = end;
    }
    if (out.empty()) {
        throw ParseError("no hex bytes given");
    }
    return out;
}

} // namespace mcs
