#pragma once

#include "core/bytes.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace fswcli {

struct LogEntry {
    std::string target;
    std::string commandLine;   // e.g. "fsw ds set_app_state enable"
    std::string note;          // e.g. "apid=0x14B seq=0"
    Bytes wire;                // bytes handed to the transport
    bool dryRun = false;
};

// Audit trail: appends one line per sent packet to <dir>/fswcli-YYYYMMDD.log
//   2026-10-05T22:30:01.123+07:00 target=sil cmd="fsw ds noop" apid=0x14B seq=0 bytes=8 wire=19 4B ...
// A logging failure never prevents sending; it is reported once.
class PacketLog {
public:
    explicit PacketLog(std::filesystem::path directory);

    // Returns a warning message the first time writing fails, nullopt otherwise.
    std::optional<std::string> record(const LogEntry& entry);

    // File used for today's entries.
    std::filesystem::path currentFile() const;

    // Local time, ISO 8601 with milliseconds and UTC offset.
    static std::string timestamp();

private:
    std::filesystem::path directory_;
    bool warned_ = false;
};

} // namespace fswcli
