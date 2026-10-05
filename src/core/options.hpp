#pragma once

// Flat key/value options handed to plug-ins (formats, layers, transports).
// Plug-ins never see the configuration file format; they get strings and use
// these helpers to validate and convert them.

#include <cstdint>
#include <initializer_list>
#include <map>
#include <string>
#include <string_view>

namespace fswcli {

using Options = std::map<std::string, std::string>;

// Throws ConfigError if `opts` contains a key not in `known`.
// `owner` names the plug-in in the message, e.g. "transport 'udp'".
void requireKnownKeys(const Options& opts, std::initializer_list<std::string_view> known,
                      std::string_view owner);

std::string requireString(const Options& opts, const std::string& key, std::string_view owner);

std::string optString(const Options& opts, const std::string& key, std::string fallback);

// Integer in [min, max]; decimal or 0x hex.
std::uint64_t requireUInt(const Options& opts, const std::string& key,
                          std::uint64_t min, std::uint64_t max, std::string_view owner);

std::uint64_t optUInt(const Options& opts, const std::string& key, std::uint64_t min,
                      std::uint64_t max, std::uint64_t fallback, std::string_view owner);

// "true"/"false"/"1"/"0"/"yes"/"no"/"on"/"off" (case-insensitive).
bool optBool(const Options& opts, const std::string& key, bool fallback, std::string_view owner);

} // namespace fswcli
