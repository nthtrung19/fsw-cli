#pragma once

#include "catalog/command_def.hpp"

#include <string_view>
#include <vector>

namespace fswcli {

// The set of apps and commands known for the active target.
// Pure data: knows nothing about files, protocols or the CLI.
class CommandCatalog {
public:
    // Validates the app and adds it. Throws ConfigError naming the app and
    // command on: bad names, duplicate app/command names, duplicate or
    // out-of-range command codes, inconsistent field sizes, enum values or
    // ranges that do not fit the field type.
    void add(AppDef app);

    const AppDef* findApp(std::string_view name) const;
    const CommandDef* findCommand(std::string_view app, std::string_view command) const;

    const std::vector<AppDef>& apps() const { return apps_; }
    std::size_t commandCount() const;

private:
    std::vector<AppDef> apps_;
};

} // namespace fswcli
