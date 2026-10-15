#pragma once

#include "core/bytes.hpp"
#include "telemetry/tlm_catalog.hpp"
#include "telemetry/tlm_decoder.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace mcs {

using TlmClock = std::chrono::system_clock;
using TlmTime = TlmClock::time_point;

// Latest state of one defined packet.
struct PacketState {
    const TlmPacketDef* def = nullptr;
    std::vector<DecodedValue> values;          // one per visible field
    std::vector<TlmTime> changedAt;            // per value: last time its text changed
    std::vector<int> trend;                    // per value: -1 down, 0, +1 up (last change)
    std::uint16_t sequence = 0;
    std::uint32_t seconds = 0;                 // FSW time of the last packet
    std::uint16_t subseconds = 0;
    std::size_t count = 0;
    std::size_t gaps = 0;                      // sequence count did not follow the previous packet
    std::size_t lengthErrors = 0;              // payload size differed from the definition
    TlmTime lastRx{};
};

// A MID received without a definition.
struct UnknownMid {
    std::uint16_t mid = 0;
    std::uint16_t sequence = 0;                // of the last packet
    Bytes lastPacket;                          // whole packet, header included (for a hexdump)
    std::size_t lastLength = 0;
    std::size_t count = 0;
    std::size_t gaps = 0;
    TlmTime lastRx{};
};

// One EVS event message.
struct EventRecord {
    TlmTime rxTime{};
    std::uint32_t seconds = 0;                 // FSW time
    std::uint16_t subseconds = 0;
    std::string app;
    std::int64_t eventId = 0;
    std::int64_t eventType = 0;                // 1 DEBUG, 2 INFO, 3 ERROR, 4 CRITICAL, 5 WARNING
    std::string typeName;                      // decoded enum text ("INFO")
    std::string message;
};

// Link counters over all MIDs.
struct TlmCounters {
    std::size_t received = 0;                  // all datagrams
    std::size_t malformed = 0;                 // not valid telemetry packets
    std::size_t unknown = 0;                   // packets with an undefined MID
    std::size_t gaps = 0;                      // sequence gaps
    std::size_t lengthErrors = 0;              // payload size differed from the definition
    std::optional<TlmTime> lastRx;
};

// Everything the UI needs, copied under the lock.
struct TlmSnapshot {
    std::vector<PacketState> packets;          // received defined packets, in catalog order
    std::vector<UnknownMid> unknown;           // by MID
    std::vector<EventRecord> events;           // oldest first
    TlmCounters counters;
};

// Decodes received packets and keeps the state shown by the UI.
// Thread-safe: ingest() runs on the receiver thread, the readers on the UI thread.
class TlmStore {
public:
    static constexpr std::size_t kMaxEvents = 500;

    TlmStore(const TelemetryCatalog& catalog, Endian endian);

    // One received datagram: parse, check, decode, update.
    void ingest(const Bytes& datagram, TlmTime now = TlmClock::now());

    // Forget values, events and counters.
    void clear();

    TlmSnapshot snapshot() const;

    TlmCounters counters() const;

    const TelemetryCatalog& catalog() const { return catalog_; }

private:
    void updatePacket(const TlmPacketDef& def, std::vector<DecodedValue> values,
                      std::uint16_t sequence, std::uint32_t seconds, std::uint16_t subseconds,
                      TlmTime now);
    void addEvent(const TlmPacketDef& def, const std::vector<DecodedValue>& values,
                  std::uint32_t seconds, std::uint16_t subseconds, TlmTime now);

    const TelemetryCatalog& catalog_;
    Endian endian_;

    mutable std::mutex mutex_;
    std::map<std::uint16_t, PacketState> packets_;
    std::map<std::uint16_t, UnknownMid> unknown_;
    std::map<std::uint16_t, std::uint16_t> lastSequence_;   // per MID, for gap detection
    std::deque<EventRecord> events_;
    TlmCounters counters_;
};

} // namespace mcs
