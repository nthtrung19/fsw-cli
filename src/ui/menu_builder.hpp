#pragma once

// ui/ is the ONLY module allowed to include <cli/...>.
// Everything else in the project must stay independent of the CLI library.

#include <functional>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

namespace cli { class Menu; }

namespace fswcli::ui {

// Called whenever the user runs "fsw <app> <cmd> [args...]".
// The UI layer only collects the tokens; validation and sending happen
// in the service layer (wired up from M3 onward).
using CommandHandler = std::function<void(std::ostream& out,
                                          const std::string& app,
                                          const std::string& cmd,
                                          const std::vector<std::string>& args)>;

// Builds the complete menu tree:
//   <root>
//   └── fsw
//       └── <app>
//           └── <cmd> [args...]
//
// M1: the app/command list is a temporary hard-coded placeholder.
// M3: it will be generated from the command catalog.
std::unique_ptr<cli::Menu> buildRootMenu(const CommandHandler& handler);

} // namespace fswcli::ui
