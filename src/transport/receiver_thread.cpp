#include "transport/receiver_thread.hpp"

#include "core/errors.hpp"

namespace mcs {

ReceiverThread::ReceiverThread(std::unique_ptr<IReceiver> receiver, DatagramHandler onDatagram,
                               IdleHandler onIdle, const std::string& what)
    : receiver_(std::move(receiver)), onDatagram_(std::move(onDatagram)), onIdle_(std::move(onIdle))
{
    if (!receiver_) {
        throw ConfigError(what + " needs a receiver");
    }
}

ReceiverThread::~ReceiverThread()
{
    stop();
}

void ReceiverThread::start(UpdateCallback onUpdate)
{
    if (thread_.joinable()) {
        return;
    }
    onUpdate_ = std::move(onUpdate);
    stopping_ = false;
    thread_ = std::thread([this] { run(); });
}

void ReceiverThread::stop()
{
    stopping_ = true;
    if (thread_.joinable()) {
        thread_.join();
    }
}

std::string ReceiverThread::lastError() const
{
    std::lock_guard<std::mutex> lock(errorMutex_);
    return lastError_;
}

void ReceiverThread::run()
{
    while (!stopping_) {
        bool arrived = false;
        try {
            if (auto datagram = receiver_->receive(kPollInterval)) {
                arrived = true;
                onDatagram_(*datagram);
            } else if (onIdle_) {
                onIdle_();
            }
            std::lock_guard<std::mutex> lock(errorMutex_);
            lastError_.clear();
        } catch (const std::exception& e) {
            {
                std::lock_guard<std::mutex> lock(errorMutex_);
                lastError_ = e.what();
            }
            std::this_thread::sleep_for(kPollInterval);   // do not spin on a broken socket
        }
        if (onUpdate_ && !stopping_) {
            onUpdate_(arrived);
        }
    }
}

} // namespace mcs
