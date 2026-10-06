#include "core/text.hpp"

#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace mcs {

std::string toLower(std::string_view text)
{
    std::string out(text);
    for (auto& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

bool iequals(std::string_view a, std::string_view b)
{
    return toLower(a) == toLower(b);
}

std::string trim(std::string_view text)
{
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r\n");
    return std::string(text.substr(first, last - first + 1));
}

std::string join(const std::vector<std::string>& parts, std::string_view separator)
{
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) {
            out += separator;
        }
        out += parts[i];
    }
    return out;
}

bool isIdentifier(std::string_view text)
{
    if (text.empty() || !std::islower(static_cast<unsigned char>(text.front()))) {
        return false;
    }
    for (const char c : text) {
        const auto uc = static_cast<unsigned char>(c);
        if (!(std::islower(uc) || std::isdigit(uc) || c == '_')) {
            return false;
        }
    }
    return true;
}

std::optional<std::uint64_t> parseUnsigned(std::string_view text)
{
    int base = 10;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
        text.remove_prefix(2);
    }
    if (text.empty()) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    const char* end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, value, base);
    if (ec != std::errc() || ptr != end) {
        return std::nullopt;
    }
    return value;
}

std::optional<std::int64_t> parseSigned(std::string_view text)
{
    bool negative = false;
    if (!text.empty() && (text.front() == '-' || text.front() == '+')) {
        negative = text.front() == '-';
        text.remove_prefix(1);
    }
    const auto magnitude = parseUnsigned(text);
    if (!magnitude) {
        return std::nullopt;
    }
    constexpr auto maxPositive = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (negative) {
        if (*magnitude > maxPositive + 1) {
            return std::nullopt;
        }
        if (*magnitude == maxPositive + 1) {
            return std::numeric_limits<std::int64_t>::min();
        }
        return -static_cast<std::int64_t>(*magnitude);
    }
    if (*magnitude > maxPositive) {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(*magnitude);
}

std::optional<double> parseDouble(std::string_view text)
{
    const std::string copy(text);   // strtod needs a NUL-terminated string
    if (copy.empty() || std::isspace(static_cast<unsigned char>(copy.front()))) {
        return std::nullopt;
    }
    char* end = nullptr;
    const double value = std::strtod(copy.c_str(), &end);
    if (end != copy.c_str() + copy.size() || !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

std::string toHexString(std::uint64_t value, int digits)
{
    static constexpr char kDigits[] = "0123456789ABCDEF";
    std::string out;
    do {
        out.insert(out.begin(), kDigits[value & 0xFU]);
        value >>= 4U;
    } while (value != 0);
    while (static_cast<int>(out.size()) < digits) {
        out.insert(out.begin(), '0');
    }
    return "0x" + out;
}

} // namespace mcs
