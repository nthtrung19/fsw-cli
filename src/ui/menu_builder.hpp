#pragma once

// ui/ (with app/) is the ONLY module allowed to include <cli/...>.

#include "service/command_service.hpp"
#include "ui/help_format.hpp"
#include "ui/tlm_context.hpp"

#include <cstddef>
#include <functional>
#include <memory>

namespace cli { class Cli; class Menu; }

namespace mcs::ui {

// Builds the whole menu tree from the catalog; no command is hard-coded:
//
//   mcs
//   ├── fsw
//   │   └── <app>              one submenu per catalog app
//   │       └── <cmd> <params> one FswCommand per catalog command
//   ├── target                 show the active target
//   ├── verbose [on|off]       hexdump every sent packet
//   ├── raw <hex bytes...>     send pre-built packet bytes
//   ├── arm                    allow the next critical command
//   └── tlm                    telemetry display (only with a TlmContext that has a store)
//       ├── view console|telemetry   F1 / F2
//       ├── show <packet>|all  tick a packet in the F2 checklist (ds.hk or 0x08B8)
//       ├── hide <packet>|all  untick it
//       ├── pause [on|off]
//       ├── clear
//       └── status
//
// `sessionErrors` is incremented when a session command (verbose, ...) is
// used wrongly; FSW command failures are counted by the service.
std::unique_ptr<cli::Menu> buildRootMenu(CommandService& service, std::size_t& sessionErrors,
                                         TlmContext* tlm = nullptr);

// Prints the commands "help" lists: name and description of each.
using HelpPrinter = std::function<void(std::ostream& out, const std::vector<HelpItem>& items)>;

// Makes "help" hand the current menu's commands to `print`.
void installHelp(cli::Cli& cli, HelpPrinter print);

// A printer for a terminal: formatHelp laid out for `width()` columns at the
// time help is typed, names in colour when `color` (ANSI).
HelpPrinter terminalHelp(std::function<int()> width, bool color);

} // namespace mcs::ui
