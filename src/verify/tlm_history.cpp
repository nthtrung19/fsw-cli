#include "verify/tlm_history.hpp"

#include "telemetry/ccsds_tlm_parser.hpp"

namespace mcs {

const std::deque<TlmSample>& TlmHistory::View::samples(std::uint16_t mid) const
{
    static const std::deque<TlmSample> kNone;
    const auto it = history_.samples_.find(mid);
    return it != history_.samples_.end() ? it->second : kNone;
}

TlmHistory::TlmHistory(const TelemetryCatalog& catalog, Endian endian)
    : catalog_(catalog), endian_(endian)
{
}

void TlmHistory::ingest(const Bytes& datagram, TlmTime now)
{
    ingest(decodePacket(catalog_, datagram, endian_), now);
}

void TlmHistory::ingest(const DecodedPacket& packet, TlmTime now)
{
    if (!packet.parsed.frame || packet.def == nullptr) {
        return;
    }
    const TlmFrame& frame = *packet.parsed.frame;
    const TlmPacketDef* def = packet.def;
    TlmSample sample{now, packet.values};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (def->isEvent) {
            events_.push_back(makeEventRecord(*def, sample.values, frame.seconds, frame.subseconds, now));
            if (events_.size() > kMaxEvents) {
                events_.pop_front();
            }
        }
        auto& samples = samples_[frame.mid];
        samples.push_back(std::move(sample));
        if (samples.size() > kSamplesPerMid) {
            samples.pop_front();
        }
    }
    arrived_.notify_all();
}

bool TlmHistory::waitUntil(std::chrono::steady_clock::time_point deadline, const Check& check) const
{
    std::unique_lock<std::mutex> lock(mutex_);
    const View view(*this);
    while (!check(view)) {
        if (arrived_.wait_until(lock, deadline) == std::cv_status::timeout) {
            return check(view);
        }
    }
    return true;
}

} // namespace mcs
