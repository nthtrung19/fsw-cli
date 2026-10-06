#pragma once

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

    FswCommand(std::string commandName, std::string help, std::vector<std::string> params, Handler handler);

    bool Exec(const std::vector<std::string>& cmdLine, cli::CliSession& session) override;
    void Help(std::ostream& out) const override;

private:
    std::string help_;
    std::vector<std::string> params_;
    Handler handler_;
};

} // namespace mcs::ui
