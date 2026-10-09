#include "catalog/catalog.hpp"

#include "core/errors.hpp"
#include "core/text.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <utility>

namespace mcs {

namespace {

// Smallest/largest value representable by an integer field type.
std::pair<long double, long double> typeLimits(FieldType type)
{
    switch (type) {
        case FieldType::U8:  return {0, std::numeric_limits<std::uint8_t>::max()};
        case FieldType::U16: return {0, std::numeric_limits<std::uint16_t>::max()};
        case FieldType::U32: return {0, std::numeric_limits<std::uint32_t>::max()};
        case FieldType::U64: return {0, static_cast<long double>(std::numeric_limits<std::uint64_t>::max())};
        case FieldType::I8:  return {std::numeric_limits<std::int8_t>::min(), std::numeric_limits<std::int8_t>::max()};
        case FieldType::I16: return {std::numeric_limits<std::int16_t>::min(), std::numeric_limits<std::int16_t>::max()};
        case FieldType::I32: return {std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max()};
        default:             return {static_cast<long double>(std::numeric_limits<std::int64_t>::min()),
                                     static_cast<long double>(std::numeric_limits<std::int64_t>::max())};
    }
}

bool fits(FieldType type, std::int64_t value)
{
    const auto [lo, hi] = typeLimits(type);
    const auto v = static_cast<long double>(value);
    return v >= lo && v <= hi;
}

void validateField(const FieldDef& f, const std::string& where)
{
    const std::string at = where + (f.name.empty() ? std::string(" padding") : " field '" + f.name + "'");

    if (f.type == FieldType::Padding) {
        if (!f.name.empty()) {
            throw ConfigError(at + ": padding fields must not have a name");
        }
        if (f.defaultValue) {
            throw ConfigError(at + ": padding fields must not have a default");
        }
    } else if (!isIdentifier(f.name)) {
        throw ConfigError(at + ": field name must match [a-z][a-z0-9_]*");
    }

    if (f.size == 0) {
        throw ConfigError(at + ": size must be > 0");
    }
    if (naturalSize(f.type) != 0 && f.size != naturalSize(f.type)) {
        throw ConfigError(at + ": size " + std::to_string(f.size) + " does not match type "
                          + toString(f.type));
    }

    const bool constrained = !f.enumValues.empty() || f.min || f.max;
    if (constrained && !isInteger(f.type)) {
        throw ConfigError(at + ": enum/min/max are only allowed on integer fields");
    }

    std::set<std::string> enumNames;
    for (const auto& e : f.enumValues) {
        if (e.name.empty() || !enumNames.insert(toLower(e.name)).second) {
            throw ConfigError(at + ": enum names must be non-empty and unique");
        }
        if (parseSigned(e.name)) {
            throw ConfigError(at + ": enum name '" + e.name + "' must not look like a number");
        }
        if (!fits(f.type, e.value)) {
            throw ConfigError(at + ": enum value " + std::to_string(e.value) + " does not fit "
                              + toString(f.type));
        }
    }
    if (f.min && !fits(f.type, *f.min)) {
        throw ConfigError(at + ": min does not fit " + toString(f.type));
    }
    if (f.max && !fits(f.type, *f.max)) {
        throw ConfigError(at + ": max does not fit " + toString(f.type));
    }
    if (f.min && f.max && *f.min > *f.max) {
        throw ConfigError(at + ": min is greater than max");
    }
}

} // namespace

void CommandCatalog::add(AppDef app)
{
    const std::string appWhere = "app '" + app.name + "'";
    if (!isIdentifier(app.name)) {
        throw ConfigError(appWhere + ": name must match [a-z][a-z0-9_]*");
    }
    if (findApp(app.name) != nullptr) {
        throw ConfigError(appWhere + ": defined more than once");
    }

    // A MID is routed to one app in the FSW, so it belongs to one app here.
    for (const std::uint16_t mid : app.mids()) {
        for (const auto& other : apps_) {
            const auto otherMids = other.mids();
            if (std::find(otherMids.begin(), otherMids.end(), mid) != otherMids.end()) {
                throw ConfigError(appWhere + ": MID " + toHexString(mid, 4)
                                  + " is already used by app '" + other.name + "'");
            }
        }
    }

    std::set<std::string> names;                    // the menu is flat: fsw <app> <name>
    std::set<std::pair<unsigned, unsigned>> codes;  // (MID, cc): codes are per MID
    for (const auto& cmd : app.commands) {
        const std::string where = appWhere + " command '" + cmd.name + "'";
        if (!isIdentifier(cmd.name)) {
            throw ConfigError(where + ": name must match [a-z][a-z0-9_]*");
        }
        if (!names.insert(cmd.name).second) {
            throw ConfigError(where + ": defined more than once");
        }
        if (cmd.cc > 0x7F) {
            throw ConfigError(where + ": command code " + std::to_string(cmd.cc)
                              + " is out of range 0..127");
        }
        if (!codes.insert({cmd.mid, cmd.cc}).second) {
            throw ConfigError(where + ": command code " + std::to_string(cmd.cc)
                              + " is already used on MID " + toHexString(cmd.mid, 4));
        }

        std::set<std::string> fieldNames;
        bool defaultSeen = false;
        for (const auto& f : cmd.fields) {
            validateField(f, where);
            if (f.userVisible() && !fieldNames.insert(f.name).second) {
                throw ConfigError(where + ": field '" + f.name + "' defined more than once");
            }
            // Arguments are positional, so only trailing ones can be left out.
            if (f.userVisible()) {
                if (f.defaultValue) {
                    defaultSeen = true;
                } else if (defaultSeen) {
                    throw ConfigError(where + ": field '" + f.name
                                      + "' needs a default: it follows a field that has one");
                }
            }
        }
    }
    apps_.push_back(std::move(app));
}

const AppDef* CommandCatalog::findApp(std::string_view name) const
{
    for (const auto& a : apps_) {
        if (a.name == name) {
            return &a;
        }
    }
    return nullptr;
}

const CommandDef* CommandCatalog::findCommand(std::string_view app, std::string_view command) const
{
    const AppDef* a = findApp(app);
    return a != nullptr ? a->findCommand(command) : nullptr;
}

std::size_t CommandCatalog::commandCount() const
{
    std::size_t n = 0;
    for (const auto& a : apps_) {
        n += a.commands.size();
    }
    return n;
}

} // namespace mcs
