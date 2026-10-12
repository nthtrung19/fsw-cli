#pragma once

#include "service/command_service.hpp"
#include "ui/tlm_context.hpp"

#include <string>
#include <vector>

namespace cli { class Cli; }

namespace mcs::ui {

// Full-screen interactive session (FTXUI):
//
//   TOP     telemetry: link status line, then one of the views
//             F1 dashboard  packet panels + EVS event log
//             F2 monitor    every sent/received packet, with sequence/length checks
//             F4 stats      per-MID count, rate, gaps, length errors
//   BOTTOM  commanding: output of the typed commands, input line with tab
//           completion and history, key hints.
//
// Commands run on the UI thread; the telemetry receiver thread (tlm.service)
// only fills the store and asks for a redraw. Starts and stops tlm.service.
// `banner` lines are shown first in the command output. Returns the exit code.
int runTui(cli::Cli& cli, CommandService& service, TlmContext& tlm,
           const std::vector<std::string>& banner);

} // namespace mcs::ui
