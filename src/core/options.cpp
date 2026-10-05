#include "core/options.hpp"

#include "core/errors.hpp"
#include "core/text.hpp"

#include <algorithm>
#include <vector>

namespace fswcli {

namespace {

[[noreturn]] void fail(std::string_view owner, const std::string& message)
{
    throw ConfigError(std::string(owner) + ": " + message);
}

std::uint64_t toUInt(const std::string& key, const std::string& text, std::uint64_t min,
                     std::uint64_t max, std::string_view owner)
{
    const auto value = parseUnsigned(text);
    if (!value || *value < min || *value > max) {
        fail(owner, "option '" + key + "' must be an integer in [" + std::to_string(min)
                        + ", " + std::to_string(max) + "], got '" + text + "'");
    }
    return *value;
}

} // namespace

void requireKnownKeys(const Options& opts, std::initializer_list<std::string_view> known,
                      std::string_view owner)
{
    for (const auto& [key, value] : opts) {
        (void)value;
        if (std::find(known.begin(), known.end(), key) == known.end()) {
            std::vector<std::string> names(known.begin(), known.end());
            fail(owner, "unknown option '" + key + "'"
                            + (names.empty() ? std::string(" (takes no options)")
                                             : " (known: " + join(names, ", ") + ")"));
        }
    }
}

std::string requireString(const Options& opts, const std::string& key, std::string_view owner)
{
    const auto it = opts.find(key);
    if (it == opts.end() || it->second.empty()) {
        fail(owner, "missing required option '" + key + "'");
    }
    return it->second;
}

std::string optString(const Options& opts, const std::string& key, std::string fallback)
{
    const auto it = opts.find(key);
    return it == opts.end() ? std::move(fallback) : it->second;
}

std::uint64_t requireUInt(const Options& opts, const std::string& key, std::uint64_t min,
                          std::uint64_t max, std::string_view owner)
{
    return toUInt(key, requireString(opts, key, owner), min, max, owner);
}

std::uint64_t optUInt(const Options& opts, const std::string& key, std::uint64_t min,
                      std::uint64_t max, std::uint64_t fallback, std::string_view owner)
{
    const auto it = opts.find(key);
    return it == opts.end() ? fallback : toUInt(key, it->second, min, max, owner);
}

bool optBool(const Options& opts, const std::string& key, bool fallback, std::string_view owner)
{
    const auto it = opts.find(key);
    if (it == opts.end()) {
        return fallback;
    }
    const std::string v = toLower(it->second);
    if (v == "true" || v == "1" || v == "yes" || v == "on") return true;
    if (v == "false" || v == "0" || v == "no" || v == "off") return false;
    fail(owner, "option '" + key + "' must be true or false, got '" + it->second + "'");
}

} // namespace fswcli
