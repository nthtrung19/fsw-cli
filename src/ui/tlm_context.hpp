#pragma once

// Telemetry state shared by the full-screen session and the `tlm` menu
// commands. Everything except the store is used on the UI thread only.

#include "console/console_service.hpp"
#include "service/file_transfer_service.hpp"
#include "telemetry/telemetry_service.hpp"
#include "telemetry/tlm_store.hpp"

#include <cstdint>
#include <set>
#include <string>

namespace mcs::ui {

class ScriptRunner;

enum class TlmView { Console, Telemetry };   // F1, F2

const char* toString(TlmView view);

enum class F1Pane { Events, Console };       // top pane of F1, toggled with F3

const char* toString(F1Pane pane);

struct TlmViewState {
    TlmView view = TlmView::Console;
    F1Pane pane = F1Pane::Events;
    std::set<std::uint16_t> shown; // telemetry screen: MIDs ticked in the packet checklist
    bool paused = false;           // display frozen; receiving continues
};

struct TlmContext {
    TlmStore* store = nullptr;               // null: no telemetry block for the target
    TelemetryService* service = nullptr;     // null: no receiver (see receiverError)
    std::string receiverError;               // why the receiver could not be opened
    FileTransferService* files = nullptr;    // null: no "cfdp" block for the target
    ScriptRunner* script = nullptr;          // "run <script>" (full-screen session only)
    ConsoleStore* console = nullptr;         // null: no "console" block for the target
    ConsoleService* consoleService = nullptr;   // null: no console receiver (see consoleError)
    std::string consoleError;                // why the console receiver could not be opened
    TlmViewState view;
};

} // namespace mcs::ui
