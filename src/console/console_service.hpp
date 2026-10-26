#pragma once

#include "console/console_store.hpp"
#include "transport/receiver.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace mcs {

// Runs the console receiver thread:  receive (100 ms timeout) ->
// ConsoleStore::ingest, and flushes an unfinished line after a pause.
//
// `onUpdate(dataArrived)` is called on the receiver thread after each datagram
// and on every timeout, like TelemetryService's; it must be cheap and
// thread-safe.
class ConsoleService {
public:
    using UpdateCallback = std::function<void(bool dataArrived)>;

    static constexpr std::chrono::milliseconds kPollInterval{100};

    ConsoleService(std::unique_ptr<IReceiver> receiver, ConsoleStore& store);
    ~ConsoleService();   // stops the thread

    ConsoleService(const ConsoleService&) = delete;
    ConsoleService& operator=(const ConsoleService&) = delete;

    void start(UpdateCallback onUpdate);
    void stop();         // returns within about one poll interval

    std::string describe() const { return receiver_->describe(); }

    // Last receive error, "" if none since the last successful receive.
    std::string lastError() const;

    ConsoleStore& store() { return store_; }

private:
    void run();

    std::unique_ptr<IReceiver> receiver_;
    ConsoleStore& store_;
    UpdateCallback onUpdate_;
    std::atomic<bool> stopping_{false};
    std::thread thread_;
    mutable std::mutex errorMutex_;
    std::string lastError_;
};

} // namespace mcs
