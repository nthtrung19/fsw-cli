#pragma once

// Small string and number-parsing helpers shared by all modules.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mcs {

std::string toLower(std::string_view text);

bool iequals(std::string_view a, std::string_view b);

std::string trim(std::string_view text);

std::string join(const std::vector<std::string>& parts, std::string_view separator);

// [a-z0-9_]+ starting with a letter: the rule for app, command and field names.
bool isIdentifier(std::string_view text);

// Decimal ("42") or hexadecimal ("0x2A", "0X2a"). No sign, no octal.
// Returns nullopt on any syntax error or overflow.
std::optional<std::uint64_t> parseUnsigned(std::string_view text);

// Optional leading '-' or '+', then decimal or hexadecimal digits.
std::optional<std::int64_t> parseSigned(std::string_view text);

// Decimal floating point ("1.5", "-2e3"). Rejects trailing garbage, inf and nan.
std::optional<double> parseDouble(std::string_view text);

// "0x194B" style, uppercase, zero-padded to `digits`.
std::string toHexString(std::uint64_t value, int digits);

} // namespace mcs
