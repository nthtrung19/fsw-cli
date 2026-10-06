#pragma once

#include <optional>
#include <string>
#include <vector>

namespace mcs::app {

// Parsed command-line options of the mcs executable.
struct Options {
    std::optional<std::string> configFile;   // --config FILE
    std::optional<std::string> target;       // --target NAME
    bool dryRun = false;                     // --dry-run
    bool verbose = false;                    // --verbose
    std::string logDir = ".";                // --log-dir DIR
    bool noLog = false;                      // --no-log
    std::vector<std::string> commands;       // -c CMD (repeatable, in order)
    std::optional<std::string> script;       // -f FILE
    bool listTargets = false;                // --list-targets
    bool showHelp = false;                   // -h / --help
    bool showVersion = false;                // --version
};

// Throws std::invalid_argument with a user-readable message.
Options parseOptions(int argc, char* argv[]);

std::string usageText(const std::string& programName);

// Exit codes.
inline constexpr int kExitOk = 0;
inline constexpr int kExitCommandFailed = 1;
inline constexpr int kExitBadOptions = 2;
inline constexpr int kExitConfigError = 3;

} // namespace mcs::app
