#include "config/tlm_catalog_loader.hpp"
#include "core/errors.hpp"
#include "telemetry/ccsds_tlm_parser.hpp"
#include "telemetry/tlm_catalog.hpp"
#include "telemetry/tlm_decoder.hpp"
#include "telemetry/tlm_store.hpp"
#include "unit/test_helpers.hpp"

#include <gtest/gtest.h>

using namespace mcs;
using namespace mcs::test;

namespace {

const std::string kSourceDir = mcs_SOURCE_DIR;

TlmFieldDef field(std::string name, FieldType type, std::size_t size = 0)
{
    TlmFieldDef f;
    f.name = std::move(name);
    f.type = type;
    f.size = size != 0 ? size : naturalSize(type);
    return f;
}

TlmFieldDef padding(std::size_t size)
{
    TlmFieldDef f;
    f.type = FieldType::Padding;
    f.size = size;
    return f;
}

// ds.hk: u8 cmd_accepted, u8 pad, u16 app_state (enum), u32 file_bytes (B), i16 temp, u16 flags (hex)
TlmPacketDef dsHk()
{
    TlmPacketDef p;
    p.app = "ds";
    p.name = "hk";
    p.mid = 0x08B8;
    p.fields.push_back(field("cmd_accepted", FieldType::U8));
    p.fields.push_back(padding(1));
    auto state = field("app_state", FieldType::U16);
    state.enumValues = {{"disable", 0}, {"enable", 1}};
    p.fields.push_back(state);
    auto bytes = field("file_bytes", FieldType::U32);
    bytes.units = "B";
    p.fields.push_back(bytes);
    p.fields.push_back(field("temp", FieldType::I16));
    auto flags = field("flags", FieldType::U16);
    flags.hex = true;
    p.fields.push_back(flags);
    return p;
}

// The definition as the catalog stores it: field offsets filled in.
TlmPacketDef laidOut(TlmPacketDef p)
{
    TelemetryCatalog c;
    c.add(std::move(p));
    return c.packets().front();
}

TelemetryCatalog catalogWithDsAndEvs()
{
    TelemetryCatalog c;
    c.add(dsHk());
    for (auto& p : loadTlmFile(kSourceDir + "/config/telemetry/evs.json")) {
        c.add(std::move(p));
    }
    return c;
}

// cmd_accepted, pad, app_state, file_bytes, temp, flags (little-endian)
Bytes dsHkPayload(std::uint8_t accepted, std::uint16_t state = 1)
{
    return {accepted, 0, static_cast<std::uint8_t>(state), 0, 0x00, 0x04, 0, 0, 0xFE, 0xFF, 0x0F, 0x00};
}

std::string valueOf(const PacketState& p, const std::string& name)
{
    for (const auto& v : p.values) {
        if (v.name == name) {
            return v.text;
        }
    }
    return "<none>";
}

TlmTime at(int ms)
{
    return TlmTime{} + std::chrono::milliseconds(ms);
}

} // namespace

// ---- parser ------------------------------------------------------------------

TEST(TlmParser, HeaderTimeAndPayload)
{
    const Bytes packet = tlmPacket(0x0808, 0x1234, {0xAA, 0xBB}, 0x01020304, 0x8000);
    const auto r = parseTelemetry(packet);
    ASSERT_TRUE(r.frame) << r.error;
    EXPECT_EQ(r.frame->mid, 0x0808);
    EXPECT_EQ(r.frame->apid, 0x008);
    EXPECT_EQ(r.frame->sequence, 0x1234);
    EXPECT_EQ(r.frame->length, 14U);
    EXPECT_EQ(r.frame->seconds, 0x01020304U);
    EXPECT_EQ(r.frame->subseconds, 0x8000);
    EXPECT_EQ(r.frame->payload, (Bytes{0xAA, 0xBB}));
}

TEST(TlmParser, GoldenBytes)
{
    // ES HK-like header: MID 0x0800, seq 5, 12-byte packet, time 10 s.
    const Bytes packet{0x08, 0x00, 0xC0, 0x05, 0x00, 0x05, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x00};
    const auto r = parseTelemetry(packet);
    ASSERT_TRUE(r.frame) << r.error;
    EXPECT_EQ(r.frame->mid, 0x0800);
    EXPECT_EQ(r.frame->sequence, 5);
    EXPECT_EQ(r.frame->seconds, 10U);
    EXPECT_TRUE(r.frame->payload.empty());
}

TEST(TlmParser, RejectsBadPackets)
{
    EXPECT_NE(parseTelemetry({0x08, 0x00, 0xC0}).error.find("too short"), std::string::npos);

    Bytes command = tlmPacket(0x194B, 0, {});
    EXPECT_NE(parseTelemetry(command).error.find("command packet"), std::string::npos);

    Bytes truncated = tlmPacket(0x0808, 0, {1, 2, 3, 4});
    truncated.pop_back();
    EXPECT_NE(parseTelemetry(truncated).error.find("length field says 16"), std::string::npos);
}

// ---- catalog -------------------------------------------------------------------

TEST(TlmCatalog, OffsetsAndLookup)
{
    TelemetryCatalog c;
    c.add(dsHk());
    const TlmPacketDef* p = c.find(0x08B8);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->payloadSize(), 12U);
    EXPECT_EQ(p->findField("file_bytes")->offset, 4U);
    EXPECT_EQ(c.find(0x0001), nullptr);
}

TEST(TlmCatalog, RejectsInconsistentDefinitions)
{
    TelemetryCatalog c;
    c.add(dsHk());

    auto sameMid = dsHk();
    sameMid.name = "other";
    EXPECT_THROW(c.add(sameMid), ConfigError);

    auto badSize = dsHk();
    badSize.mid = 0x0001;
    badSize.fields[0].size = 2;
    EXPECT_THROW(TelemetryCatalog().add(badSize), ConfigError);

    auto dup = dsHk();
    dup.fields.push_back(field("temp", FieldType::U8));
    EXPECT_THROW(TelemetryCatalog().add(dup), ConfigError);

    auto event = dsHk();
    event.isEvent = true;   // no app_name / event_id / event_type
    EXPECT_THROW(TelemetryCatalog().add(event), ConfigError);
}

TEST(TlmCatalog, ShippedEvsFile)
{
    const TelemetryCatalog c = loadTelemetryCatalog({kSourceDir + "/config/telemetry/evs.json"});
    const TlmPacketDef* longEvent = c.find(0x0808);
    ASSERT_NE(longEvent, nullptr);
    EXPECT_TRUE(longEvent->isEvent);
    EXPECT_EQ(kTlmHeaderSize + longEvent->payloadSize(), 168U);   // sizeof(CFE_EVS_LongEventTlm_t)
    ASSERT_NE(c.find(0x0809), nullptr);
    EXPECT_EQ(kTlmHeaderSize + c.find(0x0809)->payloadSize(), 44U);
}

TEST(TlmCatalogLoader, ReportsFileAndLocation)
{
    TempDir dir;
    const auto bad = dir.write("bad.json", R"({ "app": "ds", "packets": [
        { "name": "hk", "mid": "0x08B8", "fields": [ { "name": "x", "type": "u7" } ] } ] })");
    try {
        loadTelemetryCatalog({bad});
        FAIL() << "no error";
    } catch (const ConfigError& e) {
        const std::string msg = e.what();
        EXPECT_NE(msg.find("bad.json"), std::string::npos) << msg;
        EXPECT_NE(msg.find("packets[0] 'hk' fields[0]"), std::string::npos) << msg;
        EXPECT_NE(msg.find("unknown type 'u7'"), std::string::npos) << msg;
    }

    const auto dupMid = dir.write("dup.json", R"({ "app": "x", "packets": [
        { "name": "a", "mid": "0x0808", "fields": [] } ] })");
    EXPECT_THROW(loadTelemetryCatalog({kSourceDir + "/config/telemetry/evs.json", dupMid}),
                 ConfigError);
}

// ---- decoder -------------------------------------------------------------------

TEST(TlmDecoder, LittleEndianFields)
{
    const auto values = decodeTelemetry(laidOut(dsHk()), dsHkPayload(12), Endian::Little);
    ASSERT_EQ(values.size(), 5U);   // padding is not shown
    EXPECT_EQ(values[0].text, "12");
    EXPECT_EQ(values[1].text, "enable");
    EXPECT_EQ(values[1].raw, 1);
    EXPECT_EQ(values[2].text, "1024 B");   // 0x00000400
    EXPECT_EQ(values[3].text, "-2");
    EXPECT_EQ(values[4].text, "0x000F");
}

TEST(TlmDecoder, BigEndianAndUnknownEnum)
{
    const Bytes payload{7, 0, 0x00, 0x05, 0x00, 0x00, 0x01, 0x00, 0xFF, 0xFE, 0x00, 0x0F};
    const auto values = decodeTelemetry(laidOut(dsHk()), payload, Endian::Big);
    EXPECT_EQ(values[1].text, "? (5)");
    EXPECT_EQ(values[2].text, "256 B");
    EXPECT_EQ(values[3].text, "-2");
    EXPECT_EQ(values[4].text, "0x000F");
}

TEST(TlmDecoder, StringsAndShortPayload)
{
    const TelemetryCatalog c = catalogWithDsAndEvs();
    const Bytes payload = evsLongEventPayload("DS", 12, 2, "No-op command");
    const auto values = decodeTelemetry(*c.find(0x0808), payload, Endian::Little);
    EXPECT_EQ(values[0].text, "DS");
    EXPECT_EQ(values[1].text, "12");
    EXPECT_EQ(values[2].text, "INFO");
    EXPECT_EQ(values[3].text, "66");
    EXPECT_EQ(values[5].text, "No-op command");

    const auto shortValues = decodeTelemetry(laidOut(dsHk()), {3, 0, 1}, Endian::Little);
    EXPECT_EQ(shortValues[0].text, "3");
    EXPECT_TRUE(shortValues[1].missing);   // app_state needs bytes 2..3
    EXPECT_TRUE(shortValues[4].missing);
}

TEST(TlmDecoder, Floats)
{
    TlmPacketDef p;
    p.app = "x";
    p.name = "y";
    auto volts = field("volts", FieldType::F32);
    volts.units = "V";
    p.fields.push_back(volts);
    p.fields.push_back(field("big", FieldType::F64));
    const Bytes payload{0x00, 0x00, 0x50, 0x40,   // 3.25f LE
                        0, 0, 0, 0, 0, 0, 0xF8, 0x3F};   // 1.5 LE
    const auto values = decodeTelemetry(laidOut(p), payload, Endian::Little);
    EXPECT_EQ(values[0].text, "3.25 V");
    EXPECT_EQ(values[1].text, "1.5");
}

// ---- store ---------------------------------------------------------------------

TEST(TlmStore, LatestValuesAndChanges)
{
    const TelemetryCatalog c = catalogWithDsAndEvs();
    TlmStore store(c, Endian::Little);

    store.ingest(tlmPacket(0x08B8, 1, dsHkPayload(1)), at(0));
    store.ingest(tlmPacket(0x08B8, 2, dsHkPayload(2, 0)), at(1000));

    const TlmSnapshot s = store.snapshot(at(1000));
    ASSERT_EQ(s.packets.size(), 1U);
    const PacketState& p = s.packets[0];
    EXPECT_EQ(p.def->fullName(), "ds.hk");
    EXPECT_EQ(p.count, 2U);
    EXPECT_EQ(p.sequence, 2);
    EXPECT_EQ(valueOf(p, "cmd_accepted"), "2");
    EXPECT_EQ(valueOf(p, "app_state"), "disable");
    EXPECT_EQ(p.trend[0], 1);
    EXPECT_EQ(p.trend[1], -1);
    EXPECT_EQ(p.changedAt[0], at(1000));
    EXPECT_EQ(p.changedAt[2], TlmTime{});   // file_bytes never changed
    EXPECT_EQ(s.counters.received, 2U);
    EXPECT_EQ(s.counters.gaps, 0U);
}

TEST(TlmStore, EventsUnknownAndMalformed)
{
    const TelemetryCatalog c = catalogWithDsAndEvs();
    TlmStore store(c, Endian::Little);

    store.ingest(tlmPacket(0x0808, 0, evsLongEventPayload("CI", 5, 3, "Invalid length"), 100, 0));
    store.ingest(tlmPacket(0x0801, 0, Bytes(30, 0)));
    store.ingest(tlmPacket(0x0801, 1, Bytes(30, 0)));
    store.ingest({0x08, 0x00, 0xC0});

    const TlmSnapshot s = store.snapshot();
    ASSERT_EQ(s.events.size(), 1U);
    EXPECT_EQ(s.events[0].app, "CI");
    EXPECT_EQ(s.events[0].eventId, 5);
    EXPECT_EQ(s.events[0].eventType, 3);
    EXPECT_EQ(s.events[0].typeName, "ERROR");
    EXPECT_EQ(s.events[0].message, "Invalid length");
    EXPECT_EQ(s.events[0].seconds, 100U);

    ASSERT_EQ(s.unknown.size(), 1U);
    EXPECT_EQ(s.unknown[0].mid, 0x0801);
    EXPECT_EQ(s.unknown[0].count, 2U);
    EXPECT_EQ(s.unknown[0].lastLength, 42U);
    EXPECT_EQ(s.counters.unknown, 2U);
    EXPECT_EQ(s.counters.malformed, 1U);
    EXPECT_EQ(s.counters.received, 4U);

    const auto all = store.monitorTail({}, 100);
    ASSERT_EQ(all.size(), 4U);
    EXPECT_EQ(all[0].name, "evs.long_event");
    EXPECT_EQ(all[1].status, PacketStatus::Unknown);
    EXPECT_EQ(all[3].status, PacketStatus::Malformed);
    EXPECT_NE(all[3].detail.find("too short"), std::string::npos);
}

TEST(TlmStore, SequenceGapsIncludingWrap)
{
    const TelemetryCatalog c = catalogWithDsAndEvs();
    TlmStore store(c, Endian::Little);

    store.ingest(tlmPacket(0x08B8, 0x3FFE, dsHkPayload(0)));
    store.ingest(tlmPacket(0x08B8, 0x3FFF, dsHkPayload(0)));
    store.ingest(tlmPacket(0x08B8, 0, dsHkPayload(0)));   // wrap: not a gap
    store.ingest(tlmPacket(0x08B8, 3, dsHkPayload(0)));   // 1 and 2 lost

    const auto records = store.monitorTail({}, 10);
    ASSERT_EQ(records.size(), 4U);
    EXPECT_EQ(records[2].status, PacketStatus::Ok);
    EXPECT_EQ(records[3].status, PacketStatus::SeqGap);
    EXPECT_EQ(records[3].detail, "expected seq 1, got 3");

    const TlmSnapshot s = store.snapshot();
    ASSERT_EQ(s.stats.size(), 1U);
    EXPECT_EQ(s.stats[0].gaps, 1U);
    EXPECT_EQ(s.counters.gaps, 1U);
}

TEST(TlmStore, LengthMismatchStillDecodes)
{
    const TelemetryCatalog c = catalogWithDsAndEvs();
    TlmStore store(c, Endian::Little);
    store.ingest(tlmPacket(0x08B8, 0, {9, 0}));

    const auto records = store.monitorTail({}, 10);
    ASSERT_EQ(records.size(), 1U);
    EXPECT_EQ(records[0].status, PacketStatus::LengthMismatch);
    EXPECT_EQ(records[0].detail, "payload 2 bytes, definition 12");
    const TlmSnapshot s = store.snapshot();
    EXPECT_EQ(valueOf(s.packets[0], "cmd_accepted"), "9");
    EXPECT_EQ(s.stats[0].lengthErrors, 1U);
}

TEST(TlmStore, RateOverWindow)
{
    const TelemetryCatalog c = catalogWithDsAndEvs();
    TlmStore store(c, Endian::Little);
    for (int i = 0; i < 10; ++i) {
        store.ingest(tlmPacket(0x08B8, static_cast<std::uint16_t>(i), dsHkPayload(0)), at(i * 1000));
    }
    // At t=9.5 s the 5-second window holds the packets of 5..9 s.
    EXPECT_DOUBLE_EQ(store.snapshot(at(9500)).stats[0].rateHz, 1.0);
    EXPECT_DOUBLE_EQ(store.snapshot(at(60000)).stats[0].rateHz, 0.0);
}

TEST(TlmStore, TxRecordsAndFilters)
{
    const TelemetryCatalog c = catalogWithDsAndEvs();
    TlmStore store(c, Endian::Little);

    store.recordTx({0x19, 0x4B, 0xC0, 0x07, 0x00, 0x01, 0x6C, 0x00}, "fsw ds noop");
    store.ingest(tlmPacket(0x08B8, 0, dsHkPayload(1)));
    store.ingest(tlmPacket(0x0808, 0, evsLongEventPayload("DS", 1, 2, "x")));

    const auto tx = store.monitorTail({}, 10).front();
    EXPECT_EQ(tx.dir, PacketDir::Tx);
    EXPECT_EQ(tx.mid, 0x194B);
    EXPECT_EQ(tx.sequence, 7);
    EXPECT_EQ(tx.app, "ds");
    EXPECT_EQ(tx.name, "fsw ds noop");

    MonitorFilter byApp;
    byApp.app = "ds";
    EXPECT_EQ(store.monitorTail(byApp, 10).size(), 2U);   // TX + ds.hk, not the evs event
    EXPECT_EQ(byApp.describe(), "app ds");

    MonitorFilter byMid;
    byMid.mid = 0x0808;
    ASSERT_EQ(store.monitorTail(byMid, 10).size(), 1U);
    EXPECT_EQ(byMid.describe(), "mid 0x0808");

    EXPECT_EQ(store.monitorTail({}, 2).size(), 2U);   // limited to the newest rows
    EXPECT_EQ(store.monitorTail({}, 2).back().mid, 0x0808);
    EXPECT_EQ(store.counters().sent, 1U);
}

TEST(TlmStore, BoundedBuffersAndClear)
{
    const TelemetryCatalog c = catalogWithDsAndEvs();
    TlmStore store(c, Endian::Little);
    const Bytes event = tlmPacket(0x0808, 0, evsLongEventPayload("DS", 1, 2, "x"));
    for (std::size_t i = 0; i < TlmStore::kMaxEvents + 10; ++i) {
        store.ingest(event);
    }
    EXPECT_EQ(store.snapshot().events.size(), TlmStore::kMaxEvents);

    for (std::size_t i = 0; i < TlmStore::kMaxMonitor; ++i) {
        store.ingest({0x00});
    }
    EXPECT_EQ(store.monitorTail({}, TlmStore::kMaxMonitor * 2).size(), TlmStore::kMaxMonitor);

    store.clear();
    const TlmSnapshot s = store.snapshot();
    EXPECT_TRUE(s.packets.empty());
    EXPECT_TRUE(s.events.empty());
    EXPECT_EQ(s.counters.received, 0U);
    EXPECT_TRUE(store.monitorTail({}, 10).empty());
}
