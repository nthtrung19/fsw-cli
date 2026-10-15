#include "telemetry/tlm_store.hpp"

#include "telemetry/ccsds_tlm_parser.hpp"

namespace mcs {

namespace {

constexpr std::uint16_t kSequenceModulo = 0x4000;

const DecodedValue* findValue(const TlmPacketDef& def, const std::vector<DecodedValue>& values,
                              const std::string& name)
{
    std::size_t index = 0;
    for (const auto& f : def.fields) {
        if (!f.visible()) {
            continue;
        }
        if (f.name == name) {
            return index < values.size() ? &values[index] : nullptr;
        }
        ++index;
    }
    return nullptr;
}

} // namespace

TlmStore::TlmStore(const TelemetryCatalog& catalog, Endian endian)
    : catalog_(catalog), endian_(endian)
{
}

void TlmStore::ingest(const Bytes& datagram, TlmTime now)
{
    const TlmParseResult parsed = parseTelemetry(datagram, endian_);

    std::lock_guard<std::mutex> lock(mutex_);
    ++counters_.received;
    counters_.lastRx = now;

    if (!parsed.frame) {
        ++counters_.malformed;
        return;
    }
    const TlmFrame& frame = *parsed.frame;

    bool gap = false;
    if (const auto it = lastSequence_.find(frame.mid); it != lastSequence_.end()) {
        gap = frame.sequence != static_cast<std::uint16_t>((it->second + 1) % kSequenceModulo);
    }
    lastSequence_[frame.mid] = frame.sequence;
    if (gap) {
        ++counters_.gaps;
    }

    const TlmPacketDef* def = catalog_.find(frame.mid);
    if (def == nullptr) {
        ++counters_.unknown;
        UnknownMid& u = unknown_[frame.mid];
        u.mid = frame.mid;
        u.sequence = frame.sequence;
        u.lastPacket = datagram;
        u.lastLength = frame.length;
        ++u.count;
        u.gaps += gap ? 1 : 0;
        u.lastRx = now;
        return;
    }

    // A variable-length packet only needs the bytes its fields cover.
    const bool lengthOk = def->variableLength ? frame.payload.size() >= def->payloadSize()
                                              : frame.payload.size() == def->payloadSize();
    if (!lengthOk) {
        ++counters_.lengthErrors;
    }

    std::vector<DecodedValue> values = decodeTelemetry(*def, frame.payload, endian_);
    if (def->isEvent) {
        addEvent(*def, values, frame.seconds, frame.subseconds, now);
    }
    updatePacket(*def, std::move(values), frame.sequence, frame.seconds, frame.subseconds, now);
    PacketState& p = packets_[def->mid];
    p.gaps += gap ? 1 : 0;
    p.lengthErrors += lengthOk ? 0 : 1;
}

void TlmStore::updatePacket(const TlmPacketDef& def, std::vector<DecodedValue> values,
                            std::uint16_t sequence, std::uint32_t seconds,
                            std::uint16_t subseconds, TlmTime now)
{
    PacketState& p = packets_[def.mid];
    if (p.def == nullptr) {
        p.def = &def;
        p.changedAt.assign(values.size(), TlmTime{});
        p.trend.assign(values.size(), 0);
    } else {
        for (std::size_t i = 0; i < values.size() && i < p.values.size(); ++i) {
            if (values[i].text == p.values[i].text) {
                continue;
            }
            p.changedAt[i] = now;
            const auto& before = p.values[i].number;
            const auto& after = values[i].number;
            p.trend[i] = (before && after && *after != *before) ? (*after > *before ? 1 : -1) : 0;
        }
    }
    p.values = std::move(values);
    p.sequence = sequence;
    p.seconds = seconds;
    p.subseconds = subseconds;
    ++p.count;
    p.lastRx = now;
}

void TlmStore::addEvent(const TlmPacketDef& def, const std::vector<DecodedValue>& values,
                        std::uint32_t seconds, std::uint16_t subseconds, TlmTime now)
{
    EventRecord e;
    e.rxTime = now;
    e.seconds = seconds;
    e.subseconds = subseconds;
    if (const auto* v = findValue(def, values, "app_name")) {
        e.app = v->text;
    }
    if (const auto* v = findValue(def, values, "event_id")) {
        e.eventId = v->raw.value_or(0);
    }
    if (const auto* v = findValue(def, values, "event_type")) {
        e.eventType = v->raw.value_or(0);
        e.typeName = v->text;
    }
    if (const auto* v = findValue(def, values, "message")) {
        e.message = v->text;
    }
    events_.push_back(std::move(e));
    if (events_.size() > kMaxEvents) {
        events_.pop_front();
    }
}

void TlmStore::clear()
{
    std::lock_guard<std::mutex> lock(mutex_);
    packets_.clear();
    unknown_.clear();
    lastSequence_.clear();
    events_.clear();
    counters_ = TlmCounters{};
}

TlmSnapshot TlmStore::snapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    TlmSnapshot s;
    for (const auto& def : catalog_.packets()) {
        if (const auto it = packets_.find(def.mid); it != packets_.end()) {
            s.packets.push_back(it->second);
        }
    }
    for (const auto& entry : unknown_) {
        s.unknown.push_back(entry.second);
    }
    s.events.assign(events_.begin(), events_.end());
    s.counters = counters_;
    return s;
}

TlmCounters TlmStore::counters() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return counters_;
}

} // namespace mcs
