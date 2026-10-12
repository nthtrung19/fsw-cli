#pragma once

#include "telemetry/tlm_store.hpp"
#include "transport/receiver.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace mcs {

// Runs the receiver thread:  receive (100 ms timeout) -> TlmStore::ingest.
//
// `onUpdate(packetArrived)` is called on the receiver thread after each packet
// and on every timeout, so a UI can redraw and animate from it. It must be
// cheap and thread-safe (e.g. post an event to the UI loop).
class TelemetryService {
public:
    using UpdateCallback = std::function<void(bool packetArrived)>;

    static constexpr std::chrono::milliseconds kPollInterval{100};

    TelemetryService(std::unique_ptr<IReceiver> receiver, TlmStore& store);
    ~TelemetryService();   // stops the thread

    TelemetryService(const TelemetryService&) = delete;
    TelemetryService& operator=(const TelemetryService&) = delete;

    void start(UpdateCallback onUpdate);
    void stop();           // returns within about one poll interval

    std::string describe() const { return receiver_->describe(); }

    // Last receive error, "" if none since the last successful receive.
    std::string lastError() const;

    TlmStore& store() { return store_; }
    const TlmStore& store() const { return store_; }

private:
    void run();

    std::unique_ptr<IReceiver> receiver_;
    TlmStore& store_;
    UpdateCallback onUpdate_;
    std::atomic<bool> stopping_{false};
    std::thread thread_;
    mutable std::mutex errorMutex_;
    std::string lastError_;
};

} // namespace mcs
