#include "telemetry/telemetry_service.hpp"

#include "core/errors.hpp"

#include <stdexcept>

namespace mcs {

TelemetryService::TelemetryService(std::unique_ptr<IReceiver> receiver, TlmStore& store)
    : receiver_(std::move(receiver)), store_(store)
{
    if (!receiver_) {
        throw ConfigError("telemetry service needs a receiver");
    }
}

TelemetryService::~TelemetryService()
{
    stop();
}

void TelemetryService::addSink(Sink sink)
{
    if (thread_.joinable()) {
        throw std::logic_error("TelemetryService::addSink after start");
    }
    sinks_.push_back(std::move(sink));
}

void TelemetryService::start(UpdateCallback onUpdate)
{
    if (thread_.joinable()) {
        return;
    }
    onUpdate_ = std::move(onUpdate);
    stopping_ = false;
    thread_ = std::thread([this] { run(); });
}

void TelemetryService::stop()
{
    stopping_ = true;
    if (thread_.joinable()) {
        thread_.join();
    }
}

std::string TelemetryService::lastError() const
{
    std::lock_guard<std::mutex> lock(errorMutex_);
    return lastError_;
}

void TelemetryService::run()
{
    while (!stopping_) {
        bool arrived = false;
        try {
            if (auto packet = receiver_->receive(kPollInterval)) {
                store_.ingest(*packet);
                arrived = true;
                for (const auto& sink : sinks_) {
                    sink(*packet);
                }
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
