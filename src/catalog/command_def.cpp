#include "catalog/command_def.hpp"

#include <algorithm>

namespace mcs {

std::size_t CommandDef::payloadSize() const
{
    std::size_t total = 0;
    for (const auto& f : fields) {
        total += f.size;
    }
    return total;
}

std::size_t CommandDef::argCount() const
{
    std::size_t count = 0;
    for (const auto& f : fields) {
        if (f.userVisible()) {
            ++count;
        }
    }
    return count;
}

std::size_t CommandDef::requiredArgCount() const
{
    std::size_t count = 0;
    for (const auto& f : fields) {
        if (f.userVisible() && !f.defaultValue) {
            ++count;
        }
    }
    return count;
}

std::vector<std::string> CommandDef::paramDescriptions() const
{
    std::vector<std::string> out;
    for (const auto& f : fields) {
        if (f.userVisible()) {
            out.push_back(f.describe());
        }
    }
    return out;
}

std::string CommandDef::usage() const
{
    std::string out = name;
    for (const auto& p : paramDescriptions()) {
        out += " <" + p + ">";
    }
    return out;
}

std::vector<std::uint16_t> AppDef::mids() const
{
    std::vector<std::uint16_t> out;
    for (const auto& c : commands) {
        if (std::find(out.begin(), out.end(), c.mid) == out.end()) {
            out.push_back(c.mid);
        }
    }
    return out;
}

const CommandDef* AppDef::findCommand(std::string_view commandName) const
{
    for (const auto& c : commands) {
        if (c.name == commandName) {
            return &c;
        }
    }
    return nullptr;
}

} // namespace mcs
