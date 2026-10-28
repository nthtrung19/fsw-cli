#include "ui/fsw_command.hpp"

#include <cctype>

namespace mcs::ui {

FswCommand::FswCommand(std::string commandName, std::string help, std::vector<std::string> params,
                       Handler handler, ArgCompleter completer)
    : FswCommand(std::move(commandName), std::move(help), std::move(params), std::move(handler),
                 completer ? LineCompleter([completer = std::move(completer)](
                                               const std::vector<std::string>& previous,
                                               const std::string& partial) {
                     return completer(previous.size(), partial);
                 })
                           : LineCompleter())
{
}

FswCommand::FswCommand(std::string commandName, std::string help, std::vector<std::string> params,
                       Handler handler, LineCompleter completer)
    : cli::Command(std::move(commandName)),
      help_(std::move(help)),
      params_(std::move(params)),
      handler_(std::move(handler)),
      completer_(std::move(completer))
{
}

std::vector<std::string> FswCommand::GetCompletionRecursive(const std::string& line) const
{
    // The same tokenizer that parses the command; it also splits "!delete_dir" into
    // "!" and "delete_dir", so the arguments of "!" may follow without a space.
    std::vector<std::string> tokens;
    cli::detail::split(tokens, line);
    const bool newArgument = !line.empty() && std::isspace(static_cast<unsigned char>(line.back()));
    if (!completer_ || tokens.empty() || tokens[0] != Name() || (tokens.size() == 1 && !newArgument)) {
        if (!listed_) {
            return {};   // never offered by name
        }
        return cli::Command::GetCompletionRecursive(line);   // completing the name
    }
    if (!IsEnabled()) {
        return {};
    }

    // Which argument: after a space a new one; otherwise the last word is partial.
    std::vector<std::string> previous(tokens.begin() + 1, tokens.end());
    std::string partial;
    if (!newArgument) {
        partial = previous.back();
        previous.pop_back();
    }

    const ArgCompletion completion = completer_(previous, partial);
    std::vector<std::string> out;
    if (completion.values.empty()) {
        if (!completion.hint.empty()) {
            out.push_back(line + cli::CompletionHintMarker + completion.hint);
        }
        return out;
    }
    // Keep what was typed before the partial word; replace the partial word.
    const std::size_t lastSpace = line.find_last_of(" \t");
    const std::string head = newArgument               ? line
                             : lastSpace != std::string::npos ? line.substr(0, lastSpace + 1)
                                                              : line.substr(0, line.size() - partial.size());
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
    if (!IsEnabled() || !listed_) {
        return;
    }
    out << " - " << Name();
    for (const auto& p : params_) {
        out << " <" << p << '>';
    }
    out << "\n\t" << help_ << "\n";
}

} // namespace mcs::ui
