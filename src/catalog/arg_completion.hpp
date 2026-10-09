#pragma once

#include "catalog/command_def.hpp"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace mcs {

// What tab completion offers for one argument of a command.
struct ArgCompletion {
    // Enum names that match what was typed, each with the field and value it
    // sends ("state = 1"). These can be inserted into the command line.
    std::vector<std::pair<std::string, std::string>> values;
    // Shown when there are no values: what the argument expects, e.g.
    // "<count: u32 1..100>   Count  (argument 2 of 3)". Never inserted.
    std::string hint;
};

// index: position among the user-visible arguments (padding is not one);
// partial: what has been typed of that argument so far (may be empty).
ArgCompletion completeArgument(const CommandDef& command, std::size_t index, const std::string& partial);

} // namespace mcs
