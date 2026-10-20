#pragma once

#include "core/bytes.hpp"
#include "telemetry/tlm_catalog.hpp"
#include "telemetry/tlm_decoder.hpp"
#include "telemetry/tlm_store.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <vector>

namespace mcs {

// One received packet of a catalog MID, decoded.
struct TlmSample {
    TlmTime rxTime{};
    std::vector<DecodedValue> values;   // one per visible field, as decodeTelemetry returns
};

// Recent telemetry for script checks ("expect", "save", ...).
//
// Fed by a TelemetryService sink on the receiver thread, so it sees every
// packet (the display's TlmStore keeps only the latest values). Keeps the last
// kSamplesPerMid packets of every catalog MID and the last kMaxEvents EVS
// events, each with its receive time, and wakes waiting checks on arrival.
class TlmHistory {
public:
    static constexpr std::size_t kSamplesPerMid = 64;
    static constexpr std::size_t kMaxEvents = 1000;

    // What a check looks at; only valid inside the callback (the lock is held).
    class View {
    public:
        // Samples of `mid`, oldest first (empty if none received).
        const std::deque<TlmSample>& samples(std::uint16_t mid) const;
        // Events, oldest first.
        const std::deque<EventRecord>& events() const { return history_.events_; }

    private:
        friend class TlmHistory;
        explicit View(const TlmHistory& history) : history_(history) {}
        const TlmHistory& history_;
    };
    using Check = std::function<bool(const View&)>;

    TlmHistory(const TelemetryCatalog& catalog, Endian endian);

    // One received datagram; malformed packets and unknown MIDs are ignored.
    void ingest(const Bytes& datagram, TlmTime now = TlmClock::now());

    // Calls `check` now and after every packet that arrives, until it returns
    // true (returns true) or `deadline` passes (returns false). `check` runs
    // with the history locked: it must be quick and must not call back in.
    bool waitUntil(std::chrono::steady_clock::time_point deadline, const Check& check) const;

    const TelemetryCatalog& catalog() const { return catalog_; }

private:
    const TelemetryCatalog& catalog_;
    Endian endian_;

    mutable std::mutex mutex_;
    mutable std::condition_variable arrived_;
    std::map<std::uint16_t, std::deque<TlmSample>> samples_;
    std::deque<EventRecord> events_;
};

} // namespace mcs
