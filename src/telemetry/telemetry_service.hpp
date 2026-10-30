#pragma once

#include "telemetry/decoded_packet.hpp"
#include "telemetry/tlm_store.hpp"
#include "transport/receiver.hpp"
#include "transport/receiver_thread.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace mcs {

// Runs the receiver thread:  receive (100 ms timeout) -> decode once ->
// sinks, then TlmStore::ingest.
//
// `onUpdate(packetArrived)` is called on the receiver thread after each packet
// and on every timeout, so a UI can redraw and animate from it. It must be
// cheap and thread-safe (e.g. post an event to the UI loop).
//
// Sinks get every received datagram too, already parsed and decoded with the
// store's catalog (before the store takes the decoded values), on the
// receiver thread: e.g. the script-check history, the file transfer service
// picking out CF's PDUs (packet.datagram). They must be quick and
// thread-safe; an exception is reported like a receive error, after the
// other sinks and the store have had the packet.
class TelemetryService {
public:
    using UpdateCallback = ReceiverThread::UpdateCallback;
    using Sink = std::function<void(const DecodedPacket& packet)>;

    static constexpr std::chrono::milliseconds kPollInterval = ReceiverThread::kPollInterval;

    TelemetryService(std::unique_ptr<IReceiver> receiver, TlmStore& store);

    // Only before start().
    void addSink(Sink sink);

    void start(UpdateCallback onUpdate) { thread_.start(std::move(onUpdate)); }
    void stop() { thread_.stop(); }   // returns within about one poll interval

    std::string describe() const { return thread_.describe(); }

    // Last receive error, "" if none since the last successful receive.
    std::string lastError() const { return thread_.lastError(); }

    TlmStore& store() { return store_; }
    const TlmStore& store() const { return store_; }

private:
    void onDatagram(const Bytes& datagram);

    TlmStore& store_;
    std::vector<Sink> sinks_;
    ReceiverThread thread_;   // last: stopped (destroyed) before the sinks and the store it uses
};

} // namespace mcs
