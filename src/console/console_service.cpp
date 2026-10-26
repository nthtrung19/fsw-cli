#include "console/console_service.hpp"

#include "core/errors.hpp"

namespace mcs {

ConsoleService::ConsoleService(std::unique_ptr<IReceiver> receiver, ConsoleStore& store)
    : receiver_(std::move(receiver)), store_(store)
{
    if (!receiver_) {
        throw ConfigError("console service needs a receiver");
    }
}

ConsoleService::~ConsoleService()
{
    stop();
}

void ConsoleService::start(UpdateCallback onUpdate)
{
    if (thread_.joinable()) {
        return;
    }
    onUpdate_ = std::move(onUpdate);
    stopping_ = false;
    thread_ = std::thread([this] { run(); });
}

void ConsoleService::stop()
{
    stopping_ = true;
    if (thread_.joinable()) {
        thread_.join();
    }
}

std::string ConsoleService::lastError() const
{
    std::lock_guard<std::mutex> lock(errorMutex_);
    return lastError_;
}

void ConsoleService::run()
{
    while (!stopping_) {
        bool arrived = false;
        try {
            if (auto datagram = receiver_->receive(kPollInterval)) {
                store_.ingest(*datagram);
                arrived = true;
            } else {
                store_.flushIdle();
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
