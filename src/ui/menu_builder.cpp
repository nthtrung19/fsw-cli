#include "ui/menu_builder.hpp"

#include "core/text.hpp"
#include "ui/fsw_command.hpp"
#include "ui/help_format.hpp"
#include "ui/menu_parts.hpp"

#include <cli/cli.h>

#include <memory>
#include <sys/ioctl.h>
#include <unistd.h>

namespace mcs::ui {

namespace {

// Name of the command that confirms a critical command typed after it.
constexpr const char* kConfirmCommand = "!";

// Where a "!" command sits, and so what follows it: the rest of the line as
// that menu would take it.
enum class ConfirmScope {
    App,    // app menu:  ! <command> [args...]
    Fsw,    // fsw menu:  ! <app> <command> [args...]
    Root,   // top level: ! fsw <app> <command> [args...]
};

// "!" in front of a command line sends the command even if it is critical.
// It confirms that one command only. `app` is set for ConfirmScope::App.
std::unique_ptr<FswCommand> confirmCommand(CommandService& service, const VerifyService* checkWith,
                                           std::size_t& sessionErrors, ConfirmScope scope,
                                           const AppDef* app = nullptr)
{
    const std::string usage = scope == ConfirmScope::App   ? "! <command> [args...]"
                              : scope == ConfirmScope::Fsw ? "! <app> <command> [args...]"
                                                           : "! fsw <app> <command> [args...]";
    const std::string example = scope == ConfirmScope::App   ? "! delete_dir /cf/x"
                                : scope == ConfirmScope::Fsw ? "! fm delete_dir /cf/x"
                                                             : "! fsw fm delete_dir /cf/x";
    std::vector<std::string> params{"command", "args..."};
    if (scope != ConfirmScope::App) {
        params.insert(params.begin(), "app");
    }
    if (scope == ConfirmScope::Root) {
        params.insert(params.begin(), "fsw");
    }

    auto command = std::make_unique<FswCommand>(
        kConfirmCommand, "Send a command, also a critical one, e.g. " + example, params,
        [&service, checkWith, &sessionErrors, scope, app, usage](std::ostream& out,
                                                                 const std::vector<std::string>& args) {
            std::size_t at = 0;
            const AppDef* target = app;
            if (scope == ConfirmScope::Root && (args.empty() || args[0] != "fsw")) {
                ++sessionErrors;
                out << "error: usage: " << usage << '\n';
                return;
            }
            if (scope == ConfirmScope::Root) {
                at = 1;
            }
            if (scope != ConfirmScope::App && args.size() > at) {
                target = service.catalog().findApp(args[at]);
                if (target == nullptr) {
                    ++sessionErrors;
                    out << "error: unknown app '" << args[at] << "' for target '" << service.targetName() << "'\n";
                    return;
                }
                ++at;
            }
            if (target == nullptr || args.size() <= at) {
                ++sessionErrors;
                out << "error: usage: " << usage << '\n';
                return;
            }
            const std::vector<std::string> rest(args.begin() + static_cast<std::ptrdiff_t>(at) + 1, args.end());
            if (checkWith != nullptr) {
                service.check(target->name, args[at], rest, checkWith->valuesKnown(), out, true);
            } else {
                service.execute(target->name, args[at], rest, out, true);
            }
        },
        // The catalog outlives the menu, so apps can be held by pointer.
        FswCommand::LineCompleter([&catalog = service.catalog(), scope, app](
                                      const std::vector<std::string>& previous, const std::string& partial) {
            ArgCompletion c;
            std::size_t at = 0;
            if (scope == ConfirmScope::Root) {
                if (previous.empty()) {
                    if (std::string("fsw").rfind(partial, 0) == 0) {
                        c.values.emplace_back("fsw", "Send commands to flight software");
                    } else {
                        c.hint = "<fsw>";
                    }
                    return c;
                }
                if (previous[0] != "fsw") {
                    c.hint = "(only fsw commands follow '!')";
                    return c;
                }
                at = 1;
            }
            const AppDef* target = app;
            if (scope != ConfirmScope::App) {
                if (previous.size() == at) {
                    for (const auto& a : catalog.apps()) {
                        if (a.name.rfind(partial, 0) == 0) {
                            c.values.emplace_back(a.name, a.help);
                        }
                    }
                    if (c.values.empty()) {
                        c.hint = "<app>";
                    }
                    return c;
                }
                target = catalog.findApp(previous[at]);
                if (target == nullptr) {
                    c.hint = "(unknown app '" + previous[at] + "')";
                    return c;
                }
                ++at;
            }
            return completeCommandLine(
                *target, std::vector<std::string>(previous.begin() + static_cast<std::ptrdiff_t>(at), previous.end()),
                partial);
        }));
    // Not offered by TAB or help: the error on a critical command is what tells about it.
    command->setListed(false);
    return command;
}

// `checkWith`: check-only session (the commands are checked, not sent).
std::unique_ptr<cli::Menu> buildFswMenu(CommandService& service, const VerifyService* checkWith,
                                        std::size_t& sessionErrors)
{
    auto fsw = std::make_unique<cli::Menu>("fsw", "Send commands to flight software (target '"
                                                      + service.targetName() + "')");

    for (const auto& app : service.catalog().apps()) {
        auto appMenu = std::make_unique<cli::Menu>(
            app.name, app.help.empty() ? "Commands for " + app.name : app.help);

        const bool severalMids = app.mids().size() > 1;
        for (const auto& cmd : app.commands) {
            std::string help = cmd.help;
            if (severalMids) {
                help += " [mid " + toHexString(cmd.mid, 4) + "]";
            }
            if (cmd.critical) {
                help += " [critical: type '!' before it]";
            }
            appMenu->Insert(std::make_unique<FswCommand>(
                cmd.name, help, cmd.paramDescriptions(),
                [&service, checkWith, appName = app.name, cmdName = cmd.name](
                    std::ostream& out, const std::vector<std::string>& args) {
                    if (checkWith != nullptr) {
                        service.check(appName, cmdName, args, checkWith->valuesKnown(), out);
                    } else {
                        service.execute(appName, cmdName, args, out);
                    }
                },
                // The catalog outlives the menu, so the definition can be held by pointer.
                [def = &cmd](std::size_t index, const std::string& partial) {
                    return completeArgument(*def, index, partial);
                }));
        }
        appMenu->Insert(confirmCommand(service, checkWith, sessionErrors, ConfirmScope::App, &app));
        fsw->Insert(std::move(appMenu));
    }
    fsw->Insert(confirmCommand(service, checkWith, sessionErrors, ConfirmScope::Fsw));
    return fsw;
}

void insertSessionCommands(cli::Menu& root, CommandService& service, std::size_t& sessionErrors,
                           const VerifyService* checkWith)
{
    root.Insert(std::make_unique<FswCommand>(
        "target", "Show the active target, its protocol stack and catalog", std::vector<std::string>{},
        [&service](std::ostream& out, const std::vector<std::string>&) {
            out << "target:   " << service.targetName() << '\n'
                << "pipeline: " << service.pipeline().describe() << '\n'
                << "catalog:  " << service.catalog().apps().size() << " app(s), "
                << service.catalog().commandCount() << " command(s)\n";
            for (const auto& app : service.catalog().apps()) {
                std::string mids;
                for (const std::uint16_t mid : app.mids()) {
                    mids += (mids.empty() ? "" : ",") + toHexString(mid, 4);
                }
                out << "          " << app.name << "  mid=" << mids << "  "
                    << app.commands.size() << " command(s)\n";
            }
        }));

    root.Insert(std::make_unique<FswCommand>(
        "verbose", "Show or set hexdump of every sent packet", std::vector<std::string>{"on|off"},
        [&service, &sessionErrors](std::ostream& out, const std::vector<std::string>& args) {
            if (args.size() == 1 && (iequals(args[0], "on") || iequals(args[0], "off"))) {
                service.setVerbose(iequals(args[0], "on"));
            } else if (!args.empty()) {
                ++sessionErrors;
                out << "error: usage: verbose [on|off]\n";
                return;
            }
            out << "verbose is " << (service.verbose() ? "on" : "off") << '\n';
        },
        [](std::size_t index, const std::string& partial) {
            ArgCompletion c;
            if (index > 0) {
                c.hint = "(no more arguments: press Enter)";
                return c;
            }
            for (const char* v : {"on", "off"}) {
                if (std::string(v).rfind(toLower(partial), 0) == 0) {
                    c.values.emplace_back(v, std::string("hexdump ") + v);
                }
            }
            if (c.values.empty()) {
                c.hint = "<on|off>   or nothing to show the current setting";
            }
            return c;
        }));

    root.Insert(std::make_unique<FswCommand>(
        "raw", "Send pre-built packet bytes, e.g. raw 19 4B C0 00 00 01 6C 00",
        std::vector<std::string>{"hex bytes..."},
        [&service, checkWith](std::ostream& out, const std::vector<std::string>& args) {
            if (checkWith != nullptr) {
                service.checkRaw(args, checkWith->valuesKnown(), out);
            } else {
                service.executeRaw(args, out);
            }
        }));

    root.Insert(confirmCommand(service, checkWith, sessionErrors, ConfirmScope::Root));
}

} // namespace

int terminalWidth()
{
    winsize ws{};
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        return ws.ws_col;
    }
    return 100;
}

const char* toString(TlmView view)
{
    switch (view) {
    case TlmView::Console: return "console";
    case TlmView::Telemetry: return "telemetry";
    }
    return "?";
}

const char* toString(F1Pane pane)
{
    switch (pane) {
    case F1Pane::Events: return "events";
    case F1Pane::Console: return "console";
    }
    return "?";
}

std::unique_ptr<cli::Menu> buildRootMenu(CommandService& service, std::size_t& sessionErrors,
                                         TlmContext* tlm, FileTransferService* files, FileMenuMode fileMode,
                                         VerifyService* verify)
{
    const VerifyService* checkWith = verify != nullptr && verify->checkOnly() ? verify : nullptr;
    auto root = std::make_unique<cli::Menu>("mcs");
    root->Insert(buildFswMenu(service, checkWith, sessionErrors));
    insertSessionCommands(*root, service, sessionErrors, checkWith);
    if (tlm != nullptr && tlm->store != nullptr) {
        root->Insert(menus::buildTlmMenu(*tlm, sessionErrors));
    }
    if (tlm != nullptr && tlm->script != nullptr) {
        root->Insert(menus::buildRunCommand(*tlm->script, sessionErrors));
    }
    if (files != nullptr) {
        root->Insert(menus::buildFileMenu(*files, sessionErrors, checkWith != nullptr ? FileMenuMode::Check : fileMode));
    }
    if (verify != nullptr) {
        menus::insertVerifyCommands(*root, *verify);
    }
    return root;
}

void installHelp(cli::Cli& cli, HelpPrinter print)
{
    cli.HelpHandler([print = std::move(print)](std::ostream& out, const std::vector<std::string>&,
                                               const std::vector<cli::HelpEntry>& entries) {
        // Name and description only: parameters show up in the Tab hints.
        std::vector<HelpItem> items;
        for (const auto& e : entries) {
            std::string name = e.usage.substr(0, e.usage.find(' '));
            if (name == kConfirmCommand) {
                continue;   // unlisted (see confirmCommand)
            }
            items.push_back({std::move(name), e.description});
        }
        print(out, items);
    });
}

HelpPrinter terminalHelp(std::function<int()> width, bool color)
{
    return [width = std::move(width), color](std::ostream& out, const std::vector<HelpItem>& items) {
        for (const auto& line : formatHelp(items, width())) {
            out << (color ? toAnsi(line) : line.text) << '\n';
        }
    };
}

} // namespace mcs::ui
