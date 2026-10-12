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
    TlmTime lastRx{};
};

// A MID received without a definition.
struct UnknownMid {
    std::uint16_t mid = 0;
    std::size_t lastLength = 0;
    std::size_t count = 0;
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

enum class PacketDir { Rx, Tx };

enum class PacketStatus {
    Ok,
    SeqGap,          // sequence count did not follow the previous packet of this MID
    LengthMismatch,  // payload size differs from the definition
    Unknown,         // no definition for the MID
    Malformed,       // not a valid telemetry packet
};

const char* toString(PacketStatus status);

// One line of the packet monitor (F2).
struct PacketRecord {
    TlmTime time{};
    PacketDir dir = PacketDir::Rx;
    std::uint16_t mid = 0;
    std::uint16_t sequence = 0;
    std::size_t length = 0;
    PacketStatus status = PacketStatus::Ok;
    std::string app;                           // "ds"; "" if unknown
    std::string name;                          // "ds.hk", "fsw ds noop", "" if unknown
    std::string detail;                        // "gap: expected 41, got 43", parse error, ...
};

// Per-MID link statistics (F4).
struct MidStats {
    std::uint16_t mid = 0;
    std::string name;                          // "" if unknown
    std::size_t count = 0;
    std::size_t gaps = 0;
    std::size_t lengthErrors = 0;
    double rateHz = 0;                         // over the last kRateWindow
    TlmTime lastRx{};
};

struct TlmCounters {
    std::size_t received = 0;                  // all datagrams
    std::size_t malformed = 0;
    std::size_t unknown = 0;                   // packets with an undefined MID
    std::size_t gaps = 0;
    std::size_t sent = 0;                      // TX packets recorded
    std::optional<TlmTime> lastRx;
};

// Which packets the monitor shows: everything, one app, or one MID.
struct MonitorFilter {
    std::string app;                           // "" = any
    std::optional<std::uint16_t> mid;

    bool matches(const PacketRecord& r) const;
    std::string describe() const;              // "all", "app ds", "mid 0x08B8"
};

// Everything the dashboard needs, copied under the lock.
struct TlmSnapshot {
    std::vector<PacketState> packets;          // received defined packets, in catalog order
    std::vector<UnknownMid> unknown;           // by MID
    std::vector<EventRecord> events;           // oldest first
    std::vector<MidStats> stats;               // by MID
    TlmCounters counters;
};

// Decodes received packets and keeps the state shown by the UI.
// Thread-safe: ingest() runs on the receiver thread, recordTx() and the
// readers on the UI thread.
class TlmStore {
public:
    static constexpr std::size_t kMaxEvents = 500;
    static constexpr std::size_t kMaxMonitor = 10000;
    static constexpr std::chrono::seconds kRateWindow{5};

    TlmStore(const TelemetryCatalog& catalog, Endian endian);

    // One received datagram: parse, check, decode, update.
    void ingest(const Bytes& datagram, TlmTime now = TlmClock::now());

    // One packet sent by the command service (CCSDS bytes before any layer).
    void recordTx(const Bytes& packet, const std::string& commandLine, TlmTime now = TlmClock::now());

    // Forget values, events, statistics and the monitor.
    void clear();

    TlmSnapshot snapshot(TlmTime now = TlmClock::now()) const;

    // The last `maxRows` monitor records matching `filter`, oldest first.
    std::vector<PacketRecord> monitorTail(const MonitorFilter& filter, std::size_t maxRows) const;

    TlmCounters counters() const;

    const TelemetryCatalog& catalog() const { return catalog_; }

private:
    struct MidTrack {
        MidStats stats;
        std::optional<std::uint16_t> lastSequence;
        std::deque<TlmTime> recent;            // receive times within the rate window
    };

    void pushMonitor(PacketRecord record);
    MidTrack& track(std::uint16_t mid, const std::string& name, TlmTime now);
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
    std::map<std::uint16_t, MidTrack> tracks_;
    std::deque<EventRecord> events_;
    std::deque<PacketRecord> monitor_;
    TlmCounters counters_;
};

} // namespace mcs
