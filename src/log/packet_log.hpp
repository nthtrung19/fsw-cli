#pragma once

#include "core/bytes.hpp"

#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>

namespace mcs {

struct LogEntry {
    std::string target;
    std::string commandLine;   // e.g. "fsw ds set_app_state enable"
    std::string note;          // e.g. "apid=0x14B seq=0"
    Bytes wire;                // bytes handed to the transport
    bool dryRun = false;
};

// Audit trail: appends one line per sent packet to <dir>/mcs-YYYYMMDD.log
//   2026-10-05T22:30:01.123+07:00 target=fsw cmd="fsw ds noop" apid=0x14B seq=0 bytes=8 wire=19 4B ...
// and one line per event that is not a packet (file transfer start/end):
//   2026-10-05T22:30:02.456+07:00 target=fsw cfdp=done id=0.23_1729061234 ...
// A logging failure never prevents sending; it is reported once.
// Thread-safe: commands and file transfers log from different threads.
class PacketLog {
public:
    explicit PacketLog(std::filesystem::path directory);

    // Returns a warning message the first time writing fails, nullopt otherwise.
    std::optional<std::string> record(const LogEntry& entry);

    // "<timestamp> target=<target> <fields>"; same warning rule as record().
    std::optional<std::string> recordEvent(const std::string& target, const std::string& fields);

    // File used for today's entries.
    std::filesystem::path currentFile() const;

    // Local time, ISO 8601 with milliseconds and UTC offset.
    static std::string timestamp();

private:
    std::optional<std::string> append(const std::string& line);

    std::filesystem::path directory_;
    std::mutex mutex_;
    std::ofstream out_;                  // stays open between lines
    std::filesystem::path openFile_;     // the file out_ writes to
    bool warned_ = false;
};

} // namespace mcs
