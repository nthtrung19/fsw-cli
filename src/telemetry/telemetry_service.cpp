#include "telemetry/telemetry_service.hpp"

#include <exception>
#include <stdexcept>

namespace mcs {

TelemetryService::TelemetryService(std::unique_ptr<IReceiver> receiver, TlmStore& store)
    : store_(store),
      thread_(std::move(receiver), [this](const Bytes& datagram) { onDatagram(datagram); }, {},
              "telemetry service")
{
}

void TelemetryService::addSink(Sink sink)
{
    if (thread_.running()) {
        throw std::logic_error("TelemetryService::addSink after start");
    }
    sinks_.push_back(std::move(sink));
}

void TelemetryService::onDatagram(const Bytes& datagram)
{
    // Decoded once; the sinks see it first, then the store takes the values.
    // Every sink runs and the store always gets the packet, even if a sink
    // throws; the first sink error is reported afterwards.
    DecodedPacket packet = decodePacket(store_.catalog(), datagram, store_.endian());
    std::exception_ptr sinkError;
    for (const auto& sink : sinks_) {
        try {
            sink(packet);
        } catch (...) {
            if (!sinkError) {
                sinkError = std::current_exception();
            }
        }
    }
    store_.ingest(std::move(packet));
    if (sinkError) {
        std::rethrow_exception(sinkError);
    }
}

} // namespace mcs
