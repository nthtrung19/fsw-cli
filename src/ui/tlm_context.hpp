#pragma once

// Telemetry state shared by the full-screen session and the `tlm` menu
// commands. Everything except the store is used on the UI thread only.

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

struct TlmViewState {
    TlmView view = TlmView::Console;
    std::set<std::uint16_t> shown; // telemetry screen: MIDs ticked in the packet checklist
    bool paused = false;           // display frozen; receiving continues
};

struct TlmContext {
    TlmStore* store = nullptr;               // null: no telemetry block for the target
    TelemetryService* service = nullptr;     // null: no receiver (see receiverError)
    std::string receiverError;               // why the receiver could not be opened
    FileTransferService* files = nullptr;    // null: no "cfdp" block for the target
    ScriptRunner* script = nullptr;          // "run <script>" (full-screen session only)
    TlmViewState view;
};

} // namespace mcs::ui
