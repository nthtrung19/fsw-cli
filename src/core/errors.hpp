#pragma once

#include <stdexcept>
#include <string>

namespace mcs {

// Base class of every error raised by mcs code.
struct mcsError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Invalid user input (command arguments, hex strings, ...).
struct ParseError : mcsError {
    using mcsError::mcsError;
};

// Invalid or inconsistent configuration (targets file, catalog, plug-in options).
struct ConfigError : mcsError {
    using mcsError::mcsError;
};

// A message cannot be encoded into a packet (bad MID, payload too large, ...).
struct EncodeError : mcsError {
    using mcsError::mcsError;
};

// Opening a link or sending bytes failed.
struct TransportError : mcsError {
    using mcsError::mcsError;
};

} // namespace mcs
