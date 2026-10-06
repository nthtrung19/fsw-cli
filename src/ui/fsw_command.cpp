#include "ui/fsw_command.hpp"

namespace mcs::ui {

FswCommand::FswCommand(std::string commandName, std::string help, std::vector<std::string> params,
                       Handler handler)
    : cli::Command(std::move(commandName)),
      help_(std::move(help)),
      params_(std::move(params)),
      handler_(std::move(handler))
{
}

bool FswCommand::Exec(const std::vector<std::string>& cmdLine, cli::CliSession& session)
{
    if (!IsEnabled() || cmdLine.empty() || cmdLine[0] != Name()) {
        return false;
    }
    const std::vector<std::string> args(cmdLine.begin() + 1, cmdLine.end());
    handler_(session.OutStream(), args);
    return true;
}

void FswCommand::Help(std::ostream& out) const
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

} // namespace mcs::ui
