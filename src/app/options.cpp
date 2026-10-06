#include "app/options.hpp"

#include <stdexcept>

namespace mcs::app {

Options parseOptions(int argc, char* argv[])
{
    Options opts;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        auto value = [&]() -> std::string {
            if (i + 1 >= argc) {
                throw std::invalid_argument("option " + arg + " requires a value");
            }
            return argv[++i];
        };

        if (arg == "-h" || arg == "--help") {
            opts.showHelp = true;
        } else if (arg == "--version") {
            opts.showVersion = true;
        } else if (arg == "--config") {
            opts.configFile = value();
        } else if (arg == "-t" || arg == "--target") {
            opts.target = value();
        } else if (arg == "-n" || arg == "--dry-run") {
            opts.dryRun = true;
        } else if (arg == "-v" || arg == "--verbose") {
            opts.verbose = true;
        } else if (arg == "--log-dir") {
            opts.logDir = value();
        } else if (arg == "--no-log") {
            opts.noLog = true;
        } else if (arg == "-c" || arg == "--command") {
            opts.commands.push_back(value());
        } else if (arg == "-f" || arg == "--file") {
            if (opts.script) {
                throw std::invalid_argument("only one -f/--file may be given");
            }
            opts.script = value();
        } else if (arg == "--list-targets") {
            opts.listTargets = true;
        } else {
            throw std::invalid_argument("unknown option: " + arg);
        }
    }

    if (opts.script && !opts.commands.empty()) {
        throw std::invalid_argument("-c and -f cannot be combined");
    }
    return opts;
}

std::string usageText(const std::string& programName)
{
    return "Usage: " + programName + " [options]\n"
           "\n"
           "Sends commands to cFS flight software. Without -c or -f, starts an\n"
           "interactive session.\n"
           "\n"
           "Target selection:\n"
           "      --config FILE     Targets file (default: $mcs_CONFIG, ./config/targets.json,\n"
           "                        or the config/ directory next to the executable)\n"
           "  -t, --target NAME     Target profile to use (default: default_target in the file)\n"
           "      --list-targets    List the targets in the file and exit\n"
           "\n"
           "Sending:\n"
           "  -n, --dry-run         Build and print packets, send nothing\n"
           "  -v, --verbose         Print a hexdump of every sent packet\n"
           "      --log-dir DIR     Directory of the daily packet log (default: .)\n"
           "      --no-log          Do not write the packet log\n"
           "\n"
           "Batch mode:\n"
           "  -c, --command CMD     Run one command and exit (repeatable, run in order)\n"
           "  -f, --file FILE       Run commands from a file, one per line ('#' = comment)\n"
           "\n"
           "  -h, --help            Show this help\n"
           "      --version         Show version\n"
           "\n"
           "Exit codes: 0 ok, 1 a command failed, 2 bad options, 3 configuration error.\n"
           "\n"
           "Examples:\n"
           "  " + programName + "\n"
           "  " + programName + " -c \"fsw ds noop\"\n"
           "  " + programName + " --dry-run -c \"fsw ds set_app_state enable\"\n"
           "  " + programName + " --target sil -f commands.txt\n";
}

} // namespace mcs::app
