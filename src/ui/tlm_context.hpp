#pragma once

// Telemetry state shared by the full-screen session and the `tlm` menu
// commands. Everything except the store is used on the UI thread only.

#include "telemetry/telemetry_service.hpp"
#include "telemetry/tlm_store.hpp"

#include <string>

namespace mcs::ui {

enum class TlmView { Dashboard, Monitor, Stats };

const char* toString(TlmView view);

struct TlmViewState {
    TlmView view = TlmView::Dashboard;
    std::string panelApp;          // dashboard: only this app's panels; "" = all
    MonitorFilter monitorFilter;   // packet monitor
    bool paused = false;           // display frozen; receiving continues
};

struct TlmContext {
    TlmStore* store = nullptr;               // null: no telemetry block for the target
    TelemetryService* service = nullptr;     // null: no receiver (see receiverError)
    std::string receiverError;               // why the receiver could not be opened
    TlmViewState view;
};

} // namespace mcs::ui
