#pragma once

#include "core/bytes.hpp"

#include <chrono>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace mcs {

using ConsoleClock = std::chrono::system_clock;
using ConsoleTime = ConsoleClock::time_point;

// One line of FSW console text.
struct ConsoleLine {
    ConsoleTime rxTime{};      // when its first byte arrived
    std::string text;          // printable UTF-8, no line end
};

// Everything the UI needs, copied under the lock.
struct ConsoleSnapshot {
    std::vector<ConsoleLine> lines;            // oldest first
    std::size_t datagrams = 0;
    std::size_t bytes = 0;
    std::size_t dropped = 0;                   // old lines pushed out of the ring
    std::optional<ConsoleTime> lastRx;
};

// Rebuilds text lines from the raw console byte stream and keeps the most
// recent ones. Datagrams are cut wherever the sender (e.g. a serial-to-Ethernet
// bridge) chose, so a line may span several of them, and one may carry several
// lines.
//
// Thread-safe: ingest() and flushIdle() run on the receiver thread, the readers
// on the UI thread.
class ConsoleStore {
public:
    static constexpr std::size_t kMaxLines = 2000;
    static constexpr std::size_t kMaxLineLength = 1024;   // bytes; longer lines are split
    static constexpr std::chrono::milliseconds kPartialFlush{300};

    // One received datagram: a line ends at '\n' ("\r\n" too).
    void ingest(const Bytes& datagram, ConsoleTime now = ConsoleClock::now());

    // Emits the unfinished line once nothing arrived for kPartialFlush, so a
    // prompt or a line without its end still shows.
    void flushIdle(ConsoleTime now = ConsoleClock::now());

    // Forget lines and counters.
    void clear();

    ConsoleSnapshot snapshot() const;

private:
    void emit(std::string raw, ConsoleTime rxTime);   // under the lock

    mutable std::mutex mutex_;
    std::deque<ConsoleLine> lines_;
    std::string pending_;                      // raw bytes of the unfinished line
    ConsoleTime pendingSince_{};
    ConsoleTime lastData_{};
    std::size_t datagrams_ = 0;
    std::size_t bytes_ = 0;
    std::size_t dropped_ = 0;
    std::optional<ConsoleTime> lastRx_;
};

// Console bytes as displayable text: ANSI escape sequences are removed, tabs
// expanded, other control characters and invalid UTF-8 shown as '·'.
std::string sanitizeConsoleText(const std::string& raw);

} // namespace mcs
