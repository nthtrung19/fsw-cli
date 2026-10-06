#pragma once

#include "core/bytes.hpp"

#include <string>

namespace mcs {

// Plug-in kind 3: delivers bytes to the target.
//
// Implementations: UdpTransport, DryRunTransport. To add one (TCP, serial, ...)
// implement this interface and register a factory in
// registry/builtin_plugins.cpp (see docs/DESIGN.md §6).
class ITransport {
public:
    virtual ~ITransport() = default;

    // Throws TransportError on failure.
    virtual void send(const Bytes& data) = 0;

    // e.g. "udp://127.0.0.1:1234"
    virtual std::string describe() const = 0;

    // True if bytes are not actually delivered anywhere.
    virtual bool isDryRun() const { return false; }
};

} // namespace mcs
