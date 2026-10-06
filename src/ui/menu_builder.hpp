#pragma once

// ui/ (with app/) is the ONLY module allowed to include <cli/...>.

#include "service/command_service.hpp"

#include <cstddef>
#include <memory>

namespace cli { class Menu; }

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
//   └── arm                    allow the next critical command
//
// `sessionErrors` is incremented when a session command (verbose, ...) is
// used wrongly; FSW command failures are counted by the service.
std::unique_ptr<cli::Menu> buildRootMenu(CommandService& service, std::size_t& sessionErrors);

} // namespace mcs::ui
