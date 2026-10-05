// fswcli: command-line tool for commanding cFS flight software.
//
// Modes:
//   fswcli                   interactive session
//   fswcli -c "<cmd>" ...    run command(s) and exit
//   fswcli -f <file>         run a command script and exit
//
// Exit code (batch modes): 0 if every command succeeded, 1 otherwise.

#include "app/options.hpp"
#include "ui/menu_builder.hpp"

#include <cli/cli.h>
#include <cli/clifilesession.h>
#include <cli/clilocalsession.h>
#include <cli/loopscheduler.h>

#include <fstream>
#include <iostream>
#include <sstream>

namespace {

// M1 placeholder: just echoes what would be sent.
// From M3 this calls CommandService::execute().
void placeholderHandler(std::ostream& out, const std::string& app,
                        const std::string& cmd, const std::vector<std::string>& args)
{
    out << "[M1] fsw " << app << ' ' << cmd;
    for (const auto& a : args) {
        out << ' ' << a;
    }
    out << "  (not sent: packet encoding not implemented yet)\n";
}

// Runs a list of command lines without prompts. Returns the number of failures.
int runBatch(cli::Cli& cli, const std::vector<std::string>& lines)
{
    int failures = 0;
    cli.WrongCommandHandler([&failures](std::ostream& out, const std::string& cmd) {
        ++failures;
        out << "error: unknown command: " << cmd << '\n';
    });
    cli.StdExceptionHandler([&failures](std::ostream& out, const std::string& cmd,
                                        const std::exception& e) {
        ++failures;
        out << "error: " << cmd << ": " << e.what() << '\n';
    });

    std::istringstream unusedInput;   // the session needs a valid istream
    cli::CliFileSession session(cli, unusedInput, std::cout);
    for (const auto& line : lines) {
        session.Feed(line);
    }
    return failures;
}

std::vector<std::string> readScript(const std::string& path)
{
    std::ifstream file(path);
    if (!file) {
        throw std::runtime_error("cannot open script file: " + path);
    }
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(file, line)) {
        const auto first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos || line[first] == '#') {
            continue;   // blank line or comment
        }
        lines.push_back(line);
    }
    return lines;
}

int runInteractive(cli::Cli& cli)
{
    cli.WrongCommandHandler([](std::ostream& out, const std::string& cmd) {
        out << "Unknown command: " << cmd << " (type 'help')\n";
    });
    cli.StdExceptionHandler([](std::ostream& out, const std::string& cmd,
                               const std::exception& e) {
        out << "error: " << cmd << ": " << e.what() << '\n';
    });

    cli::LoopScheduler scheduler;
    cli::CliLocalTerminalSession session(cli, scheduler, std::cout);
    session.ExitAction([&scheduler](std::ostream& out) {
        out << "Bye.\n";
        scheduler.Stop();
    });
    scheduler.Run();
    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    const std::string program = "fswcli";

    fswcli::app::Options opts;
    try {
        opts = fswcli::app::parseOptions(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << program << ": " << e.what() << "\n\n"
                  << fswcli::app::usageText(program);
        return 2;
    }

    if (opts.showHelp) {
        std::cout << fswcli::app::usageText(program);
        return 0;
    }
    if (opts.showVersion) {
        std::cout << program << ' ' << FSWCLI_VERSION << '\n';
        return 0;
    }

    try {
        cli::Cli cli(fswcli::ui::buildRootMenu(placeholderHandler));

        if (!opts.commands.empty()) {
            return runBatch(cli, opts.commands) == 0 ? 0 : 1;
        }
        if (opts.script) {
            return runBatch(cli, readScript(*opts.script)) == 0 ? 0 : 1;
        }
        return runInteractive(cli);
    } catch (const std::exception& e) {
        std::cerr << program << ": " << e.what() << '\n';
        return 1;
    }
}
