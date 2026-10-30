#pragma once

#include "console/console_store.hpp"
#include "transport/receiver.hpp"
#include "transport/receiver_thread.hpp"

#include <chrono>
#include <memory>
#include <string>

namespace mcs {

// Runs the console receiver thread:  receive (100 ms timeout) ->
// ConsoleStore::ingest, and flushes an unfinished line after a pause.
//
// `onUpdate(dataArrived)` is called on the receiver thread after each datagram
// and on every timeout, like TelemetryService's; it must be cheap and
// thread-safe.
class ConsoleService {
public:
    using UpdateCallback = ReceiverThread::UpdateCallback;

    static constexpr std::chrono::milliseconds kPollInterval = ReceiverThread::kPollInterval;

    ConsoleService(std::unique_ptr<IReceiver> receiver, ConsoleStore& store);

    void start(UpdateCallback onUpdate) { thread_.start(std::move(onUpdate)); }
    void stop() { thread_.stop(); }   // returns within about one poll interval

    std::string describe() const { return thread_.describe(); }

    // Last receive error, "" if none since the last successful receive.
    std::string lastError() const { return thread_.lastError(); }

    ConsoleStore& store() { return store_; }

private:
    ConsoleStore& store_;
    ReceiverThread thread_;   // last: stopped (destroyed) before the store it uses
};

} // namespace mcs
