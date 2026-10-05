#pragma once

#include "core/bytes.hpp"

#include <string>

namespace fswcli {

// Plug-in kind 2: wraps an already-built packet before it is transported
// (e.g. a CSP header, KISS framing, a CRC, encryption). Layers are applied in
// the order listed in the target configuration.
//
// No implementation is shipped in the prototype; the interface fixes the
// extension point. To add one, implement this interface and register a
// factory in registry/builtin_plugins.cpp (see docs/DESIGN.md §6).
class IFramingLayer {
public:
    virtual ~IFramingLayer() = default;

    // Throws EncodeError if the data cannot be wrapped.
    virtual Bytes wrap(const Bytes& data) = 0;

    virtual std::string describe() const = 0;
};

} // namespace fswcli
