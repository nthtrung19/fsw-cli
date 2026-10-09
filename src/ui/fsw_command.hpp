#pragma once

#include "catalog/arg_completion.hpp"

#include <cli/cli.h>

#include <functional>
#include <ostream>
#include <string>
#include <vector>

namespace mcs::ui {

// A leaf node of the cli menu tree that passes its raw argument tokens to a
// handler. Used for every FSW command and session command.
//
// We subclass cli::Command instead of using Menu::Insert(lambda) so the help
// text shows real parameter names (the library prints "<list of strings>"
// for free-form commands).
class FswCommand : public cli::Command {
public:
    using Handler = std::function<void(std::ostream& out, const std::vector<std::string>& args)>;
    // Tab completion for argument `index` (0-based), given what has been typed of it.
    using ArgCompleter = std::function<ArgCompletion(std::size_t index, const std::string& partial)>;

    FswCommand(std::string commandName, std::string help, std::vector<std::string> params,
               Handler handler, ArgCompleter completer = {});

    bool Exec(const std::vector<std::string>& cmdLine, cli::CliSession& session) override;
    void Help(std::ostream& out) const override;
    // Shown next to the command in the tab-completion list
    // (external/patches/cli-tab-completion.patch).
    std::string Description() const override { return help_; }
    // The command name, or (once it is typed) its arguments via the completer.
    std::vector<std::string> GetCompletionRecursive(const std::string& line) const override;

private:
    std::string help_;
    std::vector<std::string> params_;
    Handler handler_;
    ArgCompleter completer_;
};

} // namespace mcs::ui
