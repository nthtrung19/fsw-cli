#pragma once

// PRIVATE to src/config: the only header in the project that includes
// nlohmann/json. Public config headers must not include it.

#include "core/options.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>

namespace fswcli::jsonutil {

// ordered_json keeps object keys in file order (needed for enum help text).
using Json = nlohmann::ordered_json;

// Reads and parses a file. Throws ConfigError("<file>: ...") on I/O or syntax errors.
Json readFile(const std::filesystem::path& file);

// All errors below throw ConfigError("<ctx>: <message>").
void requireObject(const Json& value, const std::string& ctx);
void checkKeys(const Json& object, std::initializer_list<std::string_view> allowed,
               const std::string& ctx);

// Parameters are string_view so callers can pass literals without binding
// temporaries to references (GCC -Wdangling-reference).
const Json& require(const Json& object, std::string_view key, std::string_view ctx);

std::string getString(const Json& object, const std::string& key, const std::string& ctx);
std::string optString(const Json& object, const std::string& key, const std::string& ctx,
                      std::string fallback = {});
bool optBool(const Json& object, const std::string& key, const std::string& ctx, bool fallback);

// Accepts a JSON integer or a string with a decimal / "0x" hex number.
std::uint64_t toUInt(const Json& value, const std::string& ctx, std::uint64_t max);
std::int64_t toInt(const Json& value, const std::string& ctx);

std::uint64_t getUInt(const Json& object, const std::string& key, const std::string& ctx,
                      std::uint64_t max);
std::optional<std::uint64_t> optUInt(const Json& object, const std::string& key,
                                     const std::string& ctx, std::uint64_t max);
std::optional<std::int64_t> optInt(const Json& object, const std::string& key,
                                   const std::string& ctx);

// Every member except "type" becomes a plug-in option; strings, numbers and
// booleans are converted to text.
Options toOptions(const Json& object, const std::string& ctx);

} // namespace fswcli::jsonutil
