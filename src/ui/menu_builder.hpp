#pragma once

// ui/ (with app/) is the ONLY module allowed to include <cli/...>.

#include "service/command_service.hpp"
#include "service/file_transfer_service.hpp"
#include "ui/help_format.hpp"
#include "ui/tlm_context.hpp"
#include "verify/verify_service.hpp"

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
//   ├── tlm                    telemetry display (only with a TlmContext that has a store)
//   │   ├── view console|telemetry   F1 / F2
//   │   ├── show <packet>|all  tick a packet in the F2 checklist (ds.hk or 0x08B8)
//   │   ├── hide <packet>|all  untick it
//   │   ├── pause [on|off]
//   │   ├── clear
//   │   └── status
//   ├── file                   file transfer with CF (only with a FileTransferService)
//   │   ├── put <local> <remote> [class1|class2]
//   │   ├── get <remote> [local] [class1|class2]
//   │   ├── list
//   │   ├── status <id>
//   │   ├── cancel <id>|all    abandon <id>|all
//   │   └── suspend <id>       resume <id>
//   ├── run [<script>|stop]    run a test script in the background (only with a ScriptRunner in
//   │                          the TlmContext: the full-screen session)
//   └── wait, timeout, expect, expect-event, expect-no-event, save, test, end
//                              script checks (only with a VerifyService; see verify_service.hpp)
//
// `sessionErrors` is incremented when a session command (verbose, ...) is
// used wrongly or a file transfer fails; FSW command failures are counted by
// the service.
//
// How "file put/get" behave depends on the session:
//   Batch       wait until the transfer ends (progress every 10 %), so the exit code tells
//   Plain       return at once; pending notices are printed by each "file" command
//   FullScreen  return at once; the full-screen session shows the notices
//   Check       check the arguments only (the pass over a script before it runs)
enum class FileMenuMode { Batch, Plain, FullScreen, Check };

// With a check-only `verify` (VerifyService::checkOnly) the whole tree only
// checks: FSW commands and "raw" go to CommandService::check / checkRaw, the
// file menu uses FileMenuMode::Check, and nothing is sent.
std::unique_ptr<cli::Menu> buildRootMenu(CommandService& service, std::size_t& sessionErrors,
                                         TlmContext* tlm = nullptr, FileTransferService* files = nullptr,
                                         FileMenuMode fileMode = FileMenuMode::Plain,
                                         VerifyService* verify = nullptr);

// Prints the commands "help" lists: name and description of each.
using HelpPrinter = std::function<void(std::ostream& out, const std::vector<HelpItem>& items)>;

// Makes "help" hand the current menu's commands to `print`.
void installHelp(cli::Cli& cli, HelpPrinter print);

// A printer for a terminal: formatHelp laid out for `width()` columns at the
// time help is typed, names in colour when `color` (ANSI).
HelpPrinter terminalHelp(std::function<int()> width, bool color);

// Columns of the terminal on stdout; 100 when it is not a terminal.
int terminalWidth();

} // namespace mcs::ui
