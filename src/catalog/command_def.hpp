#pragma once

#include "catalog/field_def.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fswcli {

// One ground command of an app: its function code and payload layout.
struct CommandDef {
    std::string name;               // "set_app_state"
    std::uint8_t cc = 0;            // function (command) code, 0..127
    std::string help;
    std::vector<FieldDef> fields;   // payload fields in order (may be empty)
    bool critical = false;          // must be armed before sending

    std::size_t payloadSize() const;
    std::size_t argCount() const;   // number of user-visible fields

    // e.g. {"state: disable|enable"}
    std::vector<std::string> paramDescriptions() const;

    // e.g. "set_app_state <state: disable|enable>"
    std::string usage() const;
};

// One FSW application and the commands it accepts on its command MID.
struct AppDef {
    std::string name;               // "ds"
    std::uint16_t mid = 0;          // command message ID (CCSDS stream ID)
    std::string help;
    std::vector<CommandDef> commands;

    const CommandDef* findCommand(std::string_view commandName) const;
};

} // namespace fswcli
