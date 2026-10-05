#include "ui/menu_builder.hpp"

#include <cli/cli.h>

namespace fswcli::ui {

namespace {

// ---------------------------------------------------------------------------
// TEMPORARY (M1): placeholder description of apps and commands.
// Replaced in M3 by the real command catalog (catalog/), which will also
// carry MIDs, command codes and parameter schemas.
// ---------------------------------------------------------------------------
struct PlaceholderCommand {
    std::string name;
    std::string help;
    std::vector<std::string> params;   // e.g. "state: enable|disable"
};

struct PlaceholderApp {
    std::string name;
    std::string help;
    std::vector<PlaceholderCommand> commands;
};

std::vector<PlaceholderApp> placeholderApps()
{
    return {
        {"ds", "Data Storage application", {
            {"noop",          "No-op, increments the command counter", {}},
            {"reset",         "Reset housekeeping counters",           {}},
            {"set_app_state", "Enable/disable DS",                     {"state: enable|disable"}},
        }},
    };
}

// ---------------------------------------------------------------------------
// One FSW command as a node of the cli menu tree.
//
// We use our own cli::Command subclass instead of Menu::Insert(lambda) so we
// control the help text (the library prints "<list of strings>" for generic
// commands) and can later add tab-completion of parameter values.
// Argument tokens are passed through unparsed; validation happens in the
// service layer, so adding apps/commands never requires new UI code.
// ---------------------------------------------------------------------------
class FswCommand : public cli::Command {
public:
    FswCommand(std::string app, std::string cmdName, std::string help,
               std::vector<std::string> params, CommandHandler handler)
        : cli::Command(std::move(cmdName)),
          app_(std::move(app)),
          help_(std::move(help)),
          params_(std::move(params)),
          handler_(std::move(handler))
    {
    }

    bool Exec(const std::vector<std::string>& cmdLine, cli::CliSession& session) override
    {
        if (!IsEnabled() || cmdLine.empty() || cmdLine[0] != Name()) {
            return false;
        }
        const std::vector<std::string> args(cmdLine.begin() + 1, cmdLine.end());
        handler_(session.OutStream(), app_, Name(), args);
        return true;
    }

    void Help(std::ostream& out) const override
    {
        if (!IsEnabled()) {
            return;
        }
        out << " - " << Name();
        for (const auto& p : params_) {
            out << " <" << p << '>';
        }
        out << "\n\t" << help_ << "\n";
    }

private:
    std::string app_;
    std::string help_;
    std::vector<std::string> params_;
    CommandHandler handler_;
};

} // namespace

std::unique_ptr<cli::Menu> buildRootMenu(const CommandHandler& handler)
{
    auto root = std::make_unique<cli::Menu>("fswcli");
    auto fsw  = std::make_unique<cli::Menu>("fsw", "Send commands to flight software");

    for (const auto& app : placeholderApps()) {
        auto appMenu = std::make_unique<cli::Menu>(app.name, app.help);
        for (const auto& cmd : app.commands) {
            appMenu->Insert(std::make_unique<FswCommand>(
                app.name, cmd.name, cmd.help, cmd.params, handler));
        }
        fsw->Insert(std::move(appMenu));
    }

    root->Insert(std::move(fsw));
    return root;
}

} // namespace fswcli::ui
