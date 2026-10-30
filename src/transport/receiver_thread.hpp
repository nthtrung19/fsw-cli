#pragma once

#include "core/bytes.hpp"
#include "transport/receiver.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace mcs {

// The receive loop shared by TelemetryService and ConsoleService:
//   receive (kPollInterval timeout) -> onDatagram, or onIdle on a timeout,
//   then onUpdate(dataArrived) for the UI.
// An exception from the receiver or a handler is kept as lastError() and the
// loop pauses one poll interval (no spinning on a broken socket); the next
// successful receive clears it.
class ReceiverThread {
public:
    using DatagramHandler = std::function<void(const Bytes& datagram)>;
    using IdleHandler = std::function<void()>;
    using UpdateCallback = std::function<void(bool dataArrived)>;

    static constexpr std::chrono::milliseconds kPollInterval{100};

    // Throws ConfigError(`what` + " needs a receiver") when `receiver` is null.
    ReceiverThread(std::unique_ptr<IReceiver> receiver, DatagramHandler onDatagram, IdleHandler onIdle,
                   const std::string& what);
    ~ReceiverThread();   // stops the thread

    ReceiverThread(const ReceiverThread&) = delete;
    ReceiverThread& operator=(const ReceiverThread&) = delete;

    // `onUpdate` is called on the receiver thread after each datagram and on
    // every timeout; it must be cheap and thread-safe. A second start() while
    // running does nothing.
    void start(UpdateCallback onUpdate);
    void stop();          // returns within about one poll interval

    bool running() const { return thread_.joinable(); }
    std::string describe() const { return receiver_->describe(); }

    // Last receive or handler error, "" if none since the last successful receive.
    std::string lastError() const;

private:
    void run();

    std::unique_ptr<IReceiver> receiver_;
    DatagramHandler onDatagram_;
    IdleHandler onIdle_;
    UpdateCallback onUpdate_;
    std::atomic<bool> stopping_{false};
    std::thread thread_;
    mutable std::mutex errorMutex_;
    std::string lastError_;
};

} // namespace mcs
