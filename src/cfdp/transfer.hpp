#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace mcs::cfdp {

using TransferClock = std::chrono::system_clock;
using TransferTime = TransferClock::time_point;

// A CFDP transaction: the source entity plus the sequence number it chose.
struct TransactionId {
    std::string source;           // "0.23"
    std::uint32_t seq = 0;

    std::string toString() const; // "0.23_1729061234"
    bool operator==(const TransactionId& o) const { return seq == o.seq && source == o.source; }
    bool operator!=(const TransactionId& o) const { return !(*this == o); }
    bool operator<(const TransactionId& o) const
    {
        return source != o.source ? source < o.source : seq < o.seq;
    }
};

// "0.24_12" -> {"0.24", 12}; nullopt if malformed.
std::optional<TransactionId> parseTransactionId(const std::string& text);

enum class Direction { Uplink, Downlink };

enum class State {
    Waiting,     // file get sent, CF has not started the transfer yet
    Active,
    Suspended,
    Done,        // file delivered (receiver: checksum ok and renamed; class 1 sender: all sent)
    Failed,
    Cancelled,
    Abandoned,
};

const char* toString(State state);   // "ACTIVE", "DONE", ...
bool isFinished(State state);

// PDUs of one transaction, counted as they pass through mcs.
struct PduCounts {
    std::uint32_t metadata = 0;
    std::uint32_t fileData = 0;
    std::uint32_t eof = 0;
    std::uint32_t ack = 0;
    std::uint32_t nak = 0;
    std::uint32_t fin = 0;
};

// Everything shown about one transfer (file list, F1 strip, notices, log).
struct TransferStatus {
    TransactionId id;                 // empty source while Waiting
    Direction direction = Direction::Uplink;
    int cls = 1;                      // 1 or 2
    bool requested = true;            // false: a downlink nobody asked for (polling, manual playback)
    std::string local;                // ground path (downlink: final path once known)
    std::string remote;               // FSW path
    std::uint64_t fileSize = 0;
    bool sizeKnown = false;
    std::uint64_t bytesDone = 0;      // sent (uplink) or received (downlink)
    double rateBps = 0;               // over the last few seconds
    std::optional<double> etaSeconds;
    PduCounts sent;
    PduCounts received;
    std::uint64_t bytesResent = 0;    // uplink FileData sent again after a NAK
    State state = State::Active;
    std::string condition;            // "NO_ERROR", "NAK_LIMIT_REACHED", ... ("" if none yet)
    std::string note;                 // short phase text, e.g. "waiting for FIN"
    TransferTime start{};
    TransferTime end{};               // set when finished
    TransferTime lastProgress{};      // last PDU or byte progress

    double percent() const;           // 0..100 (0 if the size is unknown)
};

// Text for notices, logs and the UI.
std::string formatBytes(std::uint64_t bytes);   // "820 B", "12.0 KiB", "1.0 MiB"
std::string formatRate(double bytesPerSecond);  // "15.9 KB/s" (decimal, like link rates)
std::string formatSeconds(double seconds);      // "0.6 s", "21.7 s", "3 min 05 s"

} // namespace mcs::cfdp
