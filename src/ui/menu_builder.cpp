#include "ui/menu_builder.hpp"

#include "core/text.hpp"
#include "ui/fsw_command.hpp"

#include <cli/cli.h>

namespace mcs::ui {

namespace {

std::unique_ptr<cli::Menu> buildFswMenu(CommandService& service)
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
                help += " [critical: 'arm' first]";
            }
            appMenu->Insert(std::make_unique<FswCommand>(
                cmd.name, help, cmd.paramDescriptions(),
                [&service, appName = app.name, cmdName = cmd.name](
                    std::ostream& out, const std::vector<std::string>& args) {
                    service.execute(appName, cmdName, args, out);
                }));
        }
        fsw->Insert(std::move(appMenu));
    }
    return fsw;
}

void insertSessionCommands(cli::Menu& root, CommandService& service, std::size_t& sessionErrors)
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
        }));

    root.Insert(std::make_unique<FswCommand>(
        "raw", "Send pre-built packet bytes, e.g. raw 19 4B C0 00 00 01 6C 00",
        std::vector<std::string>{"hex bytes..."},
        [&service](std::ostream& out, const std::vector<std::string>& args) {
            service.executeRaw(args, out);
        }));

    root.Insert(std::make_unique<FswCommand>(
        "arm", "Allow the next critical command to be sent", std::vector<std::string>{},
        [&service](std::ostream& out, const std::vector<std::string>&) {
            service.arm();
            out << "armed: the next critical command will be sent\n";
        }));
}

} // namespace

std::unique_ptr<cli::Menu> buildRootMenu(CommandService& service, std::size_t& sessionErrors)
{
    auto root = std::make_unique<cli::Menu>("mcs");
    root->Insert(buildFswMenu(service));
    insertSessionCommands(*root, service, sessionErrors);
    return root;
}

} // namespace mcs::ui
