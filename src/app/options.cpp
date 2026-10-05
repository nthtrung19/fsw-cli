#include "app/options.hpp"

#include <stdexcept>

namespace fswcli::app {

Options parseOptions(int argc, char* argv[])
{
    Options opts;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        auto requireValue = [&](const std::string& flag) -> std::string {
            if (i + 1 >= argc) {
                throw std::invalid_argument("option " + flag + " requires a value");
            }
            return argv[++i];
        };

        if (arg == "-h" || arg == "--help") {
            opts.showHelp = true;
        } else if (arg == "--version") {
            opts.showVersion = true;
        } else if (arg == "-c" || arg == "--command") {
            opts.commands.push_back(requireValue(arg));
        } else if (arg == "-f" || arg == "--file") {
            if (opts.script) {
                throw std::invalid_argument("only one -f/--file may be given");
            }
            opts.script = requireValue(arg);
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
           "Without options, starts an interactive session.\n"
           "\n"
           "Options:\n"
           "  -c, --command <cmd>   Run one command and exit (repeatable)\n"
           "  -f, --file <path>     Run commands from a file, one per line\n"
           "                        (blank lines and lines starting with # are ignored)\n"
           "  -h, --help            Show this help\n"
           "      --version         Show version\n"
           "\n"
           "Examples:\n"
           "  " + programName + " -c \"fsw ds noop\"\n"
           "  " + programName + " -f commands.txt\n";
}

} // namespace fswcli::app
