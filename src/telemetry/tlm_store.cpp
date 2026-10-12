#include "telemetry/tlm_store.hpp"

#include "core/text.hpp"
#include "telemetry/ccsds_tlm_parser.hpp"

#include <algorithm>

namespace mcs {

namespace {

constexpr std::uint16_t kSequenceModulo = 0x4000;

// "fsw ds noop ..." -> "ds"; anything else -> "".
std::string appOfCommandLine(const std::string& line)
{
    if (line.rfind("fsw ", 0) != 0) {
        return {};
    }
    const auto end = line.find(' ', 4);
    return line.substr(4, end == std::string::npos ? std::string::npos : end - 4);
}

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

const char* toString(PacketStatus status)
{
    switch (status) {
    case PacketStatus::Ok: return "ok";
    case PacketStatus::SeqGap: return "seq gap";
    case PacketStatus::LengthMismatch: return "length";
    case PacketStatus::Unknown: return "unknown";
    case PacketStatus::Malformed: return "malformed";
    }
    return "?";
}

bool MonitorFilter::matches(const PacketRecord& r) const
{
    if (mid && r.mid != *mid) {
        return false;
    }
    return app.empty() || r.app == app;
}

std::string MonitorFilter::describe() const
{
    if (mid) {
        return "mid " + toHexString(*mid, 4);
    }
    return app.empty() ? "all" : "app " + app;
}

TlmStore::TlmStore(const TelemetryCatalog& catalog, Endian endian)
    : catalog_(catalog), endian_(endian)
{
}

void TlmStore::pushMonitor(PacketRecord record)
{
    monitor_.push_back(std::move(record));
    if (monitor_.size() > kMaxMonitor) {
        monitor_.pop_front();
    }
}

TlmStore::MidTrack& TlmStore::track(std::uint16_t mid, const std::string& name, TlmTime now)
{
    MidTrack& t = tracks_[mid];
    t.stats.mid = mid;
    t.stats.name = name;
    ++t.stats.count;
    t.stats.lastRx = now;
    t.recent.push_back(now);
    while (!t.recent.empty() && t.recent.front() < now - kRateWindow) {
        t.recent.pop_front();
    }
    return t;
}

void TlmStore::ingest(const Bytes& datagram, TlmTime now)
{
    const TlmParseResult parsed = parseTelemetry(datagram);

    std::lock_guard<std::mutex> lock(mutex_);
    ++counters_.received;
    counters_.lastRx = now;

    PacketRecord record;
    record.time = now;
    record.dir = PacketDir::Rx;
    record.length = datagram.size();

    if (!parsed.frame) {
        ++counters_.malformed;
        if (const auto header = readPrimaryHeader(datagram)) {
            record.mid = header->mid;
            record.sequence = header->sequence;
        }
        record.status = PacketStatus::Malformed;
        record.detail = parsed.error;
        pushMonitor(std::move(record));
        return;
    }

    const TlmFrame& frame = *parsed.frame;
    const TlmPacketDef* def = catalog_.find(frame.mid);
    record.mid = frame.mid;
    record.sequence = frame.sequence;
    if (def != nullptr) {
        record.app = def->app;
        record.name = def->fullName();
    }

    MidTrack& t = track(frame.mid, record.name, now);
    if (t.lastSequence) {
        const auto expected = static_cast<std::uint16_t>((*t.lastSequence + 1) % kSequenceModulo);
        if (frame.sequence != expected) {
            ++t.stats.gaps;
            ++counters_.gaps;
            record.status = PacketStatus::SeqGap;
            record.detail = "expected seq " + std::to_string(expected) + ", got "
                          + std::to_string(frame.sequence);
        }
    }
    t.lastSequence = frame.sequence;

    if (def == nullptr) {
        ++counters_.unknown;
        UnknownMid& u = unknown_[frame.mid];
        u.mid = frame.mid;
        u.lastLength = frame.length;
        ++u.count;
        u.lastRx = now;
        record.status = PacketStatus::Unknown;
        pushMonitor(std::move(record));
        return;
    }

    if (frame.payload.size() != def->payloadSize()) {
        ++t.stats.lengthErrors;
        // A gap is the more urgent finding; keep it as the status if both apply.
        if (record.status == PacketStatus::Ok) {
            record.status = PacketStatus::LengthMismatch;
        }
        if (!record.detail.empty()) {
            record.detail += "; ";
        }
        record.detail += "payload " + std::to_string(frame.payload.size()) + " bytes, definition "
                       + std::to_string(def->payloadSize());
    }

    std::vector<DecodedValue> values = decodeTelemetry(*def, frame.payload, endian_);
    if (def->isEvent) {
        addEvent(*def, values, frame.seconds, frame.subseconds, now);
    }
    updatePacket(*def, std::move(values), frame.sequence, frame.seconds, frame.subseconds, now);
    pushMonitor(std::move(record));
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

void TlmStore::recordTx(const Bytes& packet, const std::string& commandLine, TlmTime now)
{
    PacketRecord record;
    record.time = now;
    record.dir = PacketDir::Tx;
    record.length = packet.size();
    record.app = appOfCommandLine(commandLine);
    record.name = commandLine;
    if (const auto header = readPrimaryHeader(packet)) {
        record.mid = header->mid;
        record.sequence = header->sequence;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    ++counters_.sent;
    pushMonitor(std::move(record));
}

void TlmStore::clear()
{
    std::lock_guard<std::mutex> lock(mutex_);
    packets_.clear();
    unknown_.clear();
    tracks_.clear();
    events_.clear();
    monitor_.clear();
    counters_ = TlmCounters{};
}

TlmSnapshot TlmStore::snapshot(TlmTime now) const
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

    const TlmTime windowStart = now - kRateWindow;
    const double windowSeconds = std::chrono::duration<double>(kRateWindow).count();
    for (const auto& entry : tracks_) {
        MidStats stats = entry.second.stats;
        const auto& recent = entry.second.recent;
        const auto inWindow = std::count_if(recent.begin(), recent.end(),
                                            [&](TlmTime t) { return t >= windowStart; });
        stats.rateHz = static_cast<double>(inWindow) / windowSeconds;
        s.stats.push_back(std::move(stats));
    }
    s.counters = counters_;
    return s;
}

std::vector<PacketRecord> TlmStore::monitorTail(const MonitorFilter& filter, std::size_t maxRows) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<PacketRecord> out;
    for (auto it = monitor_.rbegin(); it != monitor_.rend() && out.size() < maxRows; ++it) {
        if (filter.matches(*it)) {
            out.push_back(*it);
        }
    }
    std::reverse(out.begin(), out.end());
    return out;
}

TlmCounters TlmStore::counters() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return counters_;
}

} // namespace mcs
