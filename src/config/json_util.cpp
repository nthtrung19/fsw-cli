#include "config/json_util.hpp"

#include "core/errors.hpp"
#include "core/text.hpp"

#include <algorithm>
#include <fstream>
#include <limits>
#include <vector>

namespace fswcli::jsonutil {

namespace {

[[noreturn]] void fail(const std::string& ctx, const std::string& message)
{
    throw ConfigError(ctx + ": " + message);
}

std::string typeName(const Json& v)
{
    return v.type_name();
}

} // namespace

Json readFile(const std::filesystem::path& file)
{
    std::ifstream in(file);
    if (!in) {
        throw ConfigError(file.string() + ": cannot open file");
    }
    try {
        return Json::parse(in, nullptr, /*allow_exceptions=*/true, /*ignore_comments=*/true);
    } catch (const nlohmann::json::parse_error& e) {
        // e.what() already contains "line X, column Y".
        throw ConfigError(file.string() + ": invalid JSON: " + e.what());
    }
}

void requireObject(const Json& value, const std::string& ctx)
{
    if (!value.is_object()) {
        fail(ctx, "expected an object, got " + typeName(value));
    }
}

void checkKeys(const Json& object, std::initializer_list<std::string_view> allowed,
               const std::string& ctx)
{
    requireObject(object, ctx);
    for (const auto& item : object.items()) {
        if (std::find(allowed.begin(), allowed.end(), item.key()) == allowed.end()) {
            const std::vector<std::string> names(allowed.begin(), allowed.end());
            fail(ctx, "unknown key '" + item.key() + "' (allowed: " + join(names, ", ") + ")");
        }
    }
}

const Json& require(const Json& object, std::string_view key, std::string_view ctx)
{
    const std::string context(ctx);
    requireObject(object, context);
    const auto it = object.find(key);
    if (it == object.end()) {
        fail(context, "missing required key '" + std::string(key) + "'");
    }
    return *it;
}

std::string getString(const Json& object, const std::string& key, const std::string& ctx)
{
    const Json& v = require(object, key, ctx);
    if (!v.is_string()) {
        fail(ctx, "'" + key + "' must be a string, got " + typeName(v));
    }
    return v.get<std::string>();
}

std::string optString(const Json& object, const std::string& key, const std::string& ctx,
                      std::string fallback)
{
    const auto it = object.find(key);
    if (it == object.end()) {
        return fallback;
    }
    if (!it->is_string()) {
        fail(ctx, "'" + key + "' must be a string, got " + typeName(*it));
    }
    return it->get<std::string>();
}

bool optBool(const Json& object, const std::string& key, const std::string& ctx, bool fallback)
{
    const auto it = object.find(key);
    if (it == object.end()) {
        return fallback;
    }
    if (!it->is_boolean()) {
        fail(ctx, "'" + key + "' must be true or false, got " + typeName(*it));
    }
    return it->get<bool>();
}

std::uint64_t toUInt(const Json& value, const std::string& ctx, std::uint64_t max)
{
    std::optional<std::uint64_t> parsed;
    if (value.is_number_unsigned()) {
        parsed = value.get<std::uint64_t>();
    } else if (value.is_number_integer()) {
        parsed = std::nullopt;   // negative
    } else if (value.is_string()) {
        parsed = parseUnsigned(value.get<std::string>());
    }
    if (!parsed || *parsed > max) {
        fail(ctx, "expected an integer in [0, " + std::to_string(max) + "] (decimal or \"0x..\"), got "
                      + value.dump());
    }
    return *parsed;
}

std::int64_t toInt(const Json& value, const std::string& ctx)
{
    std::optional<std::int64_t> parsed;
    if (value.is_number_integer() && !value.is_number_unsigned()) {
        parsed = value.get<std::int64_t>();
    } else if (value.is_number_unsigned()) {
        const auto u = value.get<std::uint64_t>();
        if (u <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            parsed = static_cast<std::int64_t>(u);
        }
    } else if (value.is_string()) {
        parsed = parseSigned(value.get<std::string>());
    }
    if (!parsed) {
        fail(ctx, "expected an integer (decimal or \"0x..\"), got " + value.dump());
    }
    return *parsed;
}

std::uint64_t getUInt(const Json& object, const std::string& key, const std::string& ctx,
                      std::uint64_t max)
{
    return toUInt(require(object, key, ctx), ctx + ": '" + key + "'", max);
}

std::optional<std::uint64_t> optUInt(const Json& object, const std::string& key,
                                     const std::string& ctx, std::uint64_t max)
{
    const auto it = object.find(key);
    if (it == object.end()) {
        return std::nullopt;
    }
    return toUInt(*it, ctx + ": '" + key + "'", max);
}

std::optional<std::int64_t> optInt(const Json& object, const std::string& key,
                                   const std::string& ctx)
{
    const auto it = object.find(key);
    if (it == object.end()) {
        return std::nullopt;
    }
    return toInt(*it, ctx + ": '" + key + "'");
}

Options toOptions(const Json& object, const std::string& ctx)
{
    requireObject(object, ctx);
    Options opts;
    for (const auto& item : object.items()) {
        if (item.key() == "type") {
            continue;
        }
        const Json& v = item.value();
        if (v.is_string()) {
            opts[item.key()] = v.get<std::string>();
        } else if (v.is_boolean()) {
            opts[item.key()] = v.get<bool>() ? "true" : "false";
        } else if (v.is_number_integer() || v.is_number_float()) {
            opts[item.key()] = v.dump();
        } else {
            fail(ctx, "option '" + item.key() + "' must be a string, number or boolean");
        }
    }
    return opts;
}

} // namespace fswcli::jsonutil
