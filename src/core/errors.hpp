#pragma once

#include <stdexcept>
#include <string>

namespace fswcli {

// Base class of every error raised by fswcli code.
struct FswcliError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Invalid user input (command arguments, hex strings, ...).
struct ParseError : FswcliError {
    using FswcliError::FswcliError;
};

// Invalid or inconsistent configuration (targets file, catalog, plug-in options).
struct ConfigError : FswcliError {
    using FswcliError::FswcliError;
};

// A message cannot be encoded into a packet (bad MID, payload too large, ...).
struct EncodeError : FswcliError {
    using FswcliError::FswcliError;
};

// Opening a link or sending bytes failed.
struct TransportError : FswcliError {
    using FswcliError::FswcliError;
};

} // namespace fswcli
