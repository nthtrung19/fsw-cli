// mcs: command-line tool for commanding cFS flight software.
//
// Start-up: options -> targets file -> target -> catalog -> pipeline -> service -> menu.
// The full-screen session and batch runs also load the target's telemetry
// catalog and open its receiver (batch: for script checks). Everything except
// the menu and the sessions lives in mcs_core.

#include "app/options.hpp"
#include "config/catalog_loader.hpp"
#include "config/config_paths.hpp"
#include "config/pipeline_factory.hpp"
#include "config/target_config.hpp"
#include "config/tlm_catalog_loader.hpp"
#include "console/console_service.hpp"
#include "core/errors.hpp"
#include "registry/plugin_registry.hpp"
#include "service/command_service.hpp"
#include "service/file_transfer_service.hpp"
#include "telemetry/telemetry_service.hpp"
#include "ui/menu_builder.hpp"
#include "ui/script_runner.hpp"
#include "ui/tui_session.hpp"
#include "verify/tlm_history.hpp"

#include <cli/cli.h>
#include <cli/clilocalsession.h>
#include <cli/loopscheduler.h>

#include <iomanip>
#include <iostream>
#include <optional>
#include <unistd.h>

using namespace mcs;

namespace {

const std::string kProgram = "mcs";

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

void setInteractiveHandlers(cli::Cli& cli)
{
    cli.WrongCommandHandler([](std::ostream& out, const std::string& cmd) {
        out << "Unknown command: " << cmd << " (type 'help')\n";
    });
    cli.StdExceptionHandler([](std::ostream& out, const std::string& cmd, const std::exception& e) {
        out << "error: " << cmd << ": " << e.what() << '\n';
    });
    // The full-screen session installs its own (names coloured in the output pane).
    ui::installHelp(cli, ui::terminalHelp(ui::terminalWidth, ::isatty(STDOUT_FILENO) != 0));
}

std::vector<std::string> bannerLines(const CommandService& service)
{
    return {kProgram + ' ' + mcs_VERSION + "  target '" + service.targetName()
                + "': " + service.pipeline().describe(),
            std::to_string(service.catalog().commandCount()) + " command(s) in "
                + std::to_string(service.catalog().apps().size())
                + " app(s). Type 'help', or e.g. 'fsw ds noop'."};
}

int runInteractive(cli::Cli& cli, const CommandService& service)
{
    setInteractiveHandlers(cli);
    for (const auto& line : bannerLines(service)) {
        std::cout << line << '\n';
    }

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
        std::cout << kProgram << ' ' << mcs_VERSION << '\n';
        return app::kExitOk;
    }

    const bool batch = !opts.commands.empty() || opts.script.has_value();
    const bool fullScreen = !batch && !opts.plain && ::isatty(STDIN_FILENO) && ::isatty(STDOUT_FILENO);

    // ---- configuration: everything here is a setup error (exit 3) -----------
    TargetsFile targets;
    std::unique_ptr<CommandCatalog> catalog;
    std::unique_ptr<Pipeline> pipeline;
    const TargetConfig* target = nullptr;
    // Telemetry: full-screen session, batch runs (script checks), and any
    // session of a target with file transfer ("cfdp": CF's PDUs arrive as
    // telemetry). A receiver that cannot be opened (port in use, ...) is
    // reported, not a setup error. --check needs the definitions only.
    std::unique_ptr<TelemetryCatalog> tlmCatalog;
    std::unique_ptr<IReceiver> tlmReceiver;
    std::string tlmReceiverError;
    // FSW console text (full-screen session only); a receiver that cannot be
    // opened is reported in its pane, like telemetry's.
    std::unique_ptr<IReceiver> consoleReceiver;
    std::string consoleReceiverError;
    try {
        targets = loadTargetsFile(findTargetsFile(opts.configFile));
        if (opts.listTargets) {
            printTargets(targets, std::cout);
            return app::kExitOk;
        }
        target = &selectTarget(targets, opts.target);
        catalog = std::make_unique<CommandCatalog>(loadCatalog(target->catalogFiles));
        pipeline = buildPipeline(*target, builtinPlugins(), opts.dryRun);
        if ((fullScreen || batch || target->cfdp) && target->telemetry) {
            tlmCatalog = std::make_unique<TelemetryCatalog>(
                loadTelemetryCatalog(target->telemetry->catalogFiles));
            if (opts.checkOnly) {
                tlmReceiverError = "--check: telemetry is not received";
            } else {
                try {
                    tlmReceiver = buildReceiver(*target, builtinPlugins(), opts.tlmPort);
                } catch (const TransportError& e) {
                    tlmReceiverError = e.what();
                }
            }
        }
        if (fullScreen && target->console) {
            try {
                consoleReceiver = buildConsoleReceiver(*target, builtinPlugins());
            } catch (const TransportError& e) {
                consoleReceiverError = e.what();
            }
        }
    } catch (const mcsError& e) {
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

        ui::TlmContext tlm;
        std::unique_ptr<TlmStore> tlmStore;
        std::unique_ptr<TlmHistory> history;   // recent telemetry for script checks
        std::unique_ptr<TelemetryService> tlmService;
        if (tlmCatalog) {
            tlmStore = std::make_unique<TlmStore>(*tlmCatalog, target->endian);
            tlm.store = tlmStore.get();
            tlm.receiverError = tlmReceiverError;
            if (tlmReceiver) {
                tlmService = std::make_unique<TelemetryService>(std::move(tlmReceiver), *tlmStore);
                tlm.service = tlmService.get();
                if (batch || fullScreen) {
                    history = std::make_unique<TlmHistory>(*tlmCatalog, target->endian);
                    tlmService->addSink([h = history.get()](const DecodedPacket& packet) { h->ingest(packet); });
                }
            }
        }

        std::unique_ptr<ConsoleStore> consoleStore;
        std::unique_ptr<ConsoleService> consoleService;   // started by the full-screen session
        if (fullScreen && target->console) {
            consoleStore = std::make_unique<ConsoleStore>();
            tlm.console = consoleStore.get();
            tlm.consoleError = consoleReceiverError;
            if (consoleReceiver) {
                consoleService = std::make_unique<ConsoleService>(std::move(consoleReceiver), *consoleStore);
                tlm.consoleService = consoleService.get();
            }
        }

        // File transfer: one worker owns the CFDP engine; CF's PDUs reach it
        // through a telemetry sink.
        std::unique_ptr<FileTransferService> files;
        if (target->cfdp) {
            files = std::make_unique<FileTransferService>(*target->cfdp, *pipeline, service, log.get(),
                                                          target->name);
            files->setReceiving(tlmService != nullptr, tlmReceiverError);
            if (tlmService) {
                tlmService->addSink([f = files.get()](const DecodedPacket& packet) { f->onDatagram(*packet.datagram); });
            }
            try {
                files->start();
            } catch (const mcsError& e) {
                std::cerr << kProgram << ": " << e.what() << '\n';
                return app::kExitConfigError;
            }
            tlm.files = files.get();
        }
        // The receiver's sinks point into `files` and `history`: stop the receiver first.
        struct StopReceiverFirst {
            TelemetryService* service;
            ~StopReceiverFirst()
            {
                if (service != nullptr) {
                    service->stop();
                }
            }
        } stopReceiverFirst{tlmService.get()};
        if (tlmService && !fullScreen) {
            tlmService->start({});   // the full-screen session starts it itself
        }
        if (files && !tlmService && !batch) {
            std::cerr << "warning: telemetry is not received (" << tlmReceiverError
                      << "): file get and class 2 file put are unavailable\n";
        }

        // Test scripts: batch runs (-f, -c) and "run <script>" in the full-screen session.
        const TelemetryCatalog noTelemetryCatalog;
        const ui::ScriptTarget scriptTarget{
            *catalog, target->endian, target->name, *pipeline, log.get(), files.get(),
            tlmCatalog ? *tlmCatalog : noTelemetryCatalog, history.get(),
            !target->telemetry ? "target '" + target->name + "' has no \"telemetry\" block" : tlmReceiverError,
            opts.verbose};

        if (batch) {
            const std::vector<ui::ScriptLine> lines = opts.script ? ui::readScript(*opts.script)
                                                                  : ui::commandLines(opts.commands);
            // A script is checked as a whole before anything is sent.
            if (opts.script || opts.checkOnly) {
                const std::string name = opts.script.value_or("");
                const std::size_t errors = ui::checkScript(scriptTarget, lines, name, std::cout);
                if (errors != 0) {
                    std::cout << (name.empty() ? "" : name + ": ") << "not run: " << errors << " error(s)\n";
                    return app::kExitCommandFailed;
                }
                if (opts.checkOnly) {
                    std::cout << (name.empty() ? "" : name + ": ") << "ok, " << lines.size()
                              << " line(s) checked, nothing sent\n";
                    return app::kExitOk;
                }
            }
            return ui::runScript(scriptTarget, lines, std::cout) == 0 ? app::kExitOk : app::kExitCommandFailed;
        }

        // Joined before the receiver stops and the services it uses go away.
        std::optional<ui::ScriptRunner> scriptRunner;
        if (fullScreen) {
            scriptRunner.emplace(scriptTarget);
            tlm.script = &*scriptRunner;
        }

        std::size_t sessionErrors = 0;
        const ui::FileMenuMode fileMode = fullScreen ? ui::FileMenuMode::FullScreen : ui::FileMenuMode::Plain;
        cli::Cli cli(ui::buildRootMenu(service, sessionErrors, fullScreen ? &tlm : nullptr, files.get(), fileMode));

        if (fullScreen) {
            setInteractiveHandlers(cli);
            return ui::runTui(cli, service, tlm);
        }
        return runInteractive(cli, service);
    } catch (const std::exception& e) {
        std::cerr << kProgram << ": " << e.what() << '\n';
        return app::kExitCommandFailed;
    }
}
