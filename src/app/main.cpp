// fswcli: command-line tool for commanding cFS flight software.
//
// Start-up: options -> targets file -> target -> catalog -> pipeline -> service -> menu.
// Everything except the menu and the session lives in fswcli_core.

#include "app/options.hpp"
#include "config/catalog_loader.hpp"
#include "config/config_paths.hpp"
#include "config/pipeline_factory.hpp"
#include "config/target_config.hpp"
#include "core/errors.hpp"
#include "registry/plugin_registry.hpp"
#include "service/command_service.hpp"
#include "ui/menu_builder.hpp"

#include <cli/cli.h>
#include <cli/clifilesession.h>
#include <cli/clilocalsession.h>
#include <cli/loopscheduler.h>

#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

using namespace fswcli;

namespace {

const std::string kProgram = "fswcli";

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
            continue;
        }
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();   // tolerate Windows line endings
        }
        lines.push_back(line);
    }
    return lines;
}

void printTargets(const TargetsFile& targets, std::ostream& out)
{
    out << "Targets in " << targets.file.string() << ":\n";
    std::size_t width = 0;
    for (const auto& t : targets.targets) {
        width = std::max(width, t.name.size());
    }
    for (const auto& t : targets.targets) {
        std::string stack = t.format.type;
        for (const auto& l : t.layers) {
            stack += " -> " + l.type;
        }
        stack += " -> " + t.transport.type;
        out << (t.name == targets.defaultTarget ? "* " : "  ") << std::left
            << std::setw(static_cast<int>(width)) << t.name << "  " << std::setw(28) << stack << "  "
            << t.description << '\n';
    }
}

// Runs command lines without prompts. Returns the exit code.
int runBatch(cli::Cli& cli, CommandService& service, const std::size_t& sessionErrors,
             const std::vector<std::string>& lines)
{
    std::size_t cliErrors = 0;
    cli.WrongCommandHandler([&cliErrors](std::ostream& out, const std::string& cmd) {
        ++cliErrors;
        out << "error: unknown command: " << cmd << '\n';
    });
    cli.StdExceptionHandler([&cliErrors](std::ostream& out, const std::string& cmd,
                                         const std::exception& e) {
        ++cliErrors;
        out << "error: " << cmd << ": " << e.what() << '\n';
    });

    std::istringstream unusedInput;   // the session requires a valid istream
    cli::CliFileSession session(cli, unusedInput, std::cout);
    for (const auto& line : lines) {
        session.Feed(line);
    }
    const bool ok = cliErrors == 0 && sessionErrors == 0 && service.failures() == 0;
    return ok ? app::kExitOk : app::kExitCommandFailed;
}

int runInteractive(cli::Cli& cli, const CommandService& service)
{
    cli.WrongCommandHandler([](std::ostream& out, const std::string& cmd) {
        out << "Unknown command: " << cmd << " (type 'help')\n";
    });
    cli.StdExceptionHandler([](std::ostream& out, const std::string& cmd, const std::exception& e) {
        out << "error: " << cmd << ": " << e.what() << '\n';
    });

    std::cout << kProgram << ' ' << FSWCLI_VERSION << "  target '" << service.targetName()
              << "': " << service.pipeline().describe() << '\n'
              << service.catalog().commandCount() << " command(s) in "
              << service.catalog().apps().size() << " app(s). Type 'help', or e.g. 'fsw ds noop'.\n";

    cli::LoopScheduler scheduler;
    cli::CliLocalTerminalSession session(cli, scheduler, std::cout);
    session.ExitAction([&scheduler](std::ostream& out) {
        out << "Bye.\n";
        scheduler.Stop();
    });
    scheduler.Run();
    return app::kExitOk;
}

} // namespace

int main(int argc, char* argv[])
{
    app::Options opts;
    try {
        opts = app::parseOptions(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << kProgram << ": " << e.what() << "\n\n" << app::usageText(kProgram);
        return app::kExitBadOptions;
    }
    if (opts.showHelp) {
        std::cout << app::usageText(kProgram);
        return app::kExitOk;
    }
    if (opts.showVersion) {
        std::cout << kProgram << ' ' << FSWCLI_VERSION << '\n';
        return app::kExitOk;
    }

    // ---- configuration: everything here is a setup error (exit 3) -----------
    TargetsFile targets;
    std::unique_ptr<CommandCatalog> catalog;
    std::unique_ptr<Pipeline> pipeline;
    const TargetConfig* target = nullptr;
    try {
        targets = loadTargetsFile(findTargetsFile(opts.configFile));
        if (opts.listTargets) {
            printTargets(targets, std::cout);
            return app::kExitOk;
        }
        target = &selectTarget(targets, opts.target);
        catalog = std::make_unique<CommandCatalog>(loadCatalog(target->catalogFiles));
        pipeline = buildPipeline(*target, builtinPlugins(), opts.dryRun);
    } catch (const FswcliError& e) {
        std::cerr << kProgram << ": " << e.what() << '\n';
        return app::kExitConfigError;
    } catch (const std::exception& e) {
        std::cerr << kProgram << ": " << e.what() << '\n';
        return app::kExitConfigError;
    }

    // ---- session --------------------------------------------------------------
    try {
        std::unique_ptr<PacketLog> log;
        if (!opts.noLog) {
            log = std::make_unique<PacketLog>(opts.logDir);
        }
        CommandService service(*catalog, PayloadEncoder(target->endian), *pipeline, log.get(),
                               target->name);
        service.setVerbose(opts.verbose);

        std::size_t sessionErrors = 0;
        cli::Cli cli(ui::buildRootMenu(service, sessionErrors));

        if (!opts.commands.empty()) {
            return runBatch(cli, service, sessionErrors, opts.commands);
        }
        if (opts.script) {
            return runBatch(cli, service, sessionErrors, readScript(*opts.script));
        }
        return runInteractive(cli, service);
    } catch (const std::exception& e) {
        std::cerr << kProgram << ": " << e.what() << '\n';
        return app::kExitCommandFailed;
    }
}
