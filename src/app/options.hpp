#pragma once

#include <optional>
#include <string>
#include <vector>

namespace fswcli::app {

// Parsed command-line options for the fswcli executable.
struct Options {
    std::vector<std::string> commands;   // -c "<cmd>"   (repeatable, run in order)
    std::optional<std::string> script;   // -f <file>    (one command per line)
    bool showHelp = false;               // -h / --help
    bool showVersion = false;            // --version
};

// Throws std::invalid_argument with a user-readable message on bad input.
Options parseOptions(int argc, char* argv[]);

std::string usageText(const std::string& programName);

} // namespace fswcli::app
