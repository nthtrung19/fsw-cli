#pragma once

#include "core/bytes.hpp"

#include <chrono>
#include <optional>
#include <string>

namespace mcs {

// Plug-in kind 4: receives telemetry bytes from the target. Kept separate
// from ITransport so senders stay simple (docs/DESIGN.md §4.5).
//
// Implementations: UdpReceiver. To add one implement this interface and
// register a factory in registry/builtin_plugins.cpp.
class IReceiver {
public:
    virtual ~IReceiver() = default;

    // Waits up to `timeout` for one packet. Returns nullopt on timeout.
    // Throws TransportError on failure.
    virtual std::optional<Bytes> receive(std::chrono::milliseconds timeout) = 0;

    // e.g. "udp://0.0.0.0:5011"
    virtual std::string describe() const = 0;
};

} // namespace mcs
