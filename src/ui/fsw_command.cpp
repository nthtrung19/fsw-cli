#include "ui/fsw_command.hpp"

#include <cctype>

namespace mcs::ui {

FswCommand::FswCommand(std::string commandName, std::string help, std::vector<std::string> params,
                       Handler handler, ArgCompleter completer)
    : cli::Command(std::move(commandName)),
      help_(std::move(help)),
      params_(std::move(params)),
      handler_(std::move(handler)),
      completer_(std::move(completer))
{
}

std::vector<std::string> FswCommand::GetCompletionRecursive(const std::string& line) const
{
    const std::string prefix = Name() + ' ';
    if (!completer_ || line.rfind(prefix, 0) != 0) {
        return cli::Command::GetCompletionRecursive(line);   // completing the name
    }
    if (!IsEnabled()) {
        return {};
    }

    // Which argument: after a space a new one; otherwise the last word is partial.
    const std::string rest = line.substr(prefix.size());
    std::vector<std::string> tokens;
    cli::detail::split(tokens, rest);   // the same tokenizer that parses the command
    const bool newArgument = rest.empty() || std::isspace(static_cast<unsigned char>(rest.back()));
    std::size_t index = tokens.size();
    std::string partial;
    if (!newArgument && !tokens.empty()) {
        index = tokens.size() - 1;
        partial = tokens.back();
    }

    const ArgCompletion completion = completer_(index, partial);
    std::vector<std::string> out;
    if (completion.values.empty()) {
        if (!completion.hint.empty()) {
            out.push_back(line + cli::CompletionHintMarker + completion.hint);
        }
        return out;
    }
    // Keep what was typed before the partial word; replace the partial word.
    const std::string head = newArgument ? line : line.substr(0, line.find_last_of(" \t") + 1);
    for (const auto& [value, description] : completion.values) {
        out.push_back(head + value + cli::CompletionHelpSeparator + description);
    }
    return out;
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
