#include "config/target_config.hpp"
#include "config/tlm_catalog_loader.hpp"
#include "core/errors.hpp"
#include "core/text.hpp"
#include "telemetry/ccsds_tlm_parser.hpp"
#include "telemetry/tlm_catalog.hpp"
#include "telemetry/tlm_decoder.hpp"
#include "telemetry/telemetry_service.hpp"
#include "telemetry/tlm_store.hpp"
#include "unit/test_helpers.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <map>
#include <mutex>
#include <thread>

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

// Every telemetry file the shipped "fsw" target lists.
TelemetryCatalog shippedCatalog()
{
    const TargetsFile targets = loadTargetsFile(kSourceDir + "/config/targets.json");
    const std::optional<std::string> name = "fsw";
    const TargetConfig& fsw = selectTarget(targets, name);
    return loadTelemetryCatalog(fsw.telemetry->catalogFiles);
}

std::vector<std::string> fieldNames(const TlmPacketDef& p)
{
    std::vector<std::string> out;
    for (const auto& f : p.fields) {
        out.push_back(f.visible() ? f.name : "pad" + std::to_string(f.size));
    }
    return out;
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

TEST(TlmParser, LittleEndianHeaderTime)
{
    // cFE 6.7 memcpy's the time in CPU order: little-endian on the x86-64 PC build.
    const Bytes packet = tlmPacket(0x0800, 1, {}, 0x01020304, 0x8001, Endian::Little);
    EXPECT_EQ(packet[6], 0x04);
    const auto r = parseTelemetry(packet, Endian::Little);
    ASSERT_TRUE(r.frame) << r.error;
    EXPECT_EQ(r.frame->seconds, 0x01020304U);
    EXPECT_EQ(r.frame->subseconds, 0x8001);
    EXPECT_EQ(r.frame->mid, 0x0800);   // the primary header stays big-endian
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

TEST(TlmCatalogLoader, CountExpandsArrays)
{
    TempDir dir;
    const auto file = dir.write("arr.json", R"({ "app": "x", "packets": [
        { "name": "p", "mid": "0x0900", "fields": [
          { "name": "mask", "type": "u16", "count": 3, "hex": true, "help": "h" },
          { "type": "padding", "size": 2, "count": 3 },
          { "name": "q", "type": "f64", "count": [2, 2] },
          { "name": "label", "type": "string", "size": 4, "count": 2 } ] } ] })");
    const TelemetryCatalog c = loadTelemetryCatalog({file});
    const TlmPacketDef& p = *c.find(0x0900);
    EXPECT_EQ(fieldNames(p), (std::vector<std::string>{"mask_0", "mask_1", "mask_2", "pad6", "q_0_0",
                                                        "q_0_1", "q_1_0", "q_1_1", "label_0", "label_1"}));
    EXPECT_TRUE(p.findField("mask_2")->hex);
    EXPECT_EQ(p.findField("mask_2")->help, "h");
    EXPECT_EQ(p.findField("q_0_0")->offset, 12U);
    EXPECT_EQ(p.findField("q_1_1")->offset, 36U);
    EXPECT_EQ(p.findField("label_1")->offset, 48U);
    EXPECT_EQ(p.payloadSize(), 52U);
}

TEST(TlmCatalogLoader, GroupsExpandNestedStructs)
{
    TempDir dir;
    const auto file = dir.write("grp.json", R"({ "app": "x", "packets": [
        { "name": "p", "mid": "0x0900", "fields": [
          { "name": "n", "type": "u8" },
          { "name": "chan", "count": 2, "fields": [
            { "name": "id", "type": "u8" },
            { "type": "padding", "size": 1 },
            { "name": "inner", "fields": [ { "name": "v", "type": "u16", "count": 2 } ] } ] } ] } ] })");
    const TelemetryCatalog c = loadTelemetryCatalog({file});
    const TlmPacketDef& p = *c.find(0x0900);
    EXPECT_EQ(fieldNames(p), (std::vector<std::string>{"n", "chan_0_id", "pad1", "chan_0_inner_v_0",
                                                        "chan_0_inner_v_1", "chan_1_id", "pad1",
                                                        "chan_1_inner_v_0", "chan_1_inner_v_1"}));
    EXPECT_EQ(p.findField("chan_1_id")->offset, 7U);
    EXPECT_EQ(p.findField("chan_1_inner_v_1")->offset, 11U);
    EXPECT_FALSE(p.variableLength);
}

TEST(TlmCatalogLoader, RejectsBadCountsAndGroups)
{
    TempDir dir;
    const auto errorOf = [&](const std::string& field) {
        const auto file = dir.write("bad.json", R"({ "app": "x", "packets": [
            { "name": "p", "mid": "0x0900", "fields": [ )" + field + " ] } ] }");
        try {
            loadTelemetryCatalog({file});
        } catch (const ConfigError& e) {
            return std::string(e.what());
        }
        return std::string("<no error>");
    };
    EXPECT_NE(errorOf(R"({ "name": "a", "type": "u8", "count": 0 })").find("at least 1"), std::string::npos);
    EXPECT_NE(errorOf(R"({ "name": "a", "type": "u8", "count": [] })").find("empty array"), std::string::npos);
    EXPECT_NE(errorOf(R"({ "name": "a", "type": "u8", "count": "x" })").find("'count'"), std::string::npos);
    EXPECT_NE(errorOf(R"({ "name": "a", "type": "u8", "count": [100, 100] })").find("max 4096"),
              std::string::npos);
    EXPECT_NE(errorOf(R"({ "name": "g", "type": "u8", "fields": [] })").find("'type'"), std::string::npos);
    EXPECT_NE(errorOf(R"({ "name": "g", "fields": {} })").find("'fields' must be an array"),
              std::string::npos);
    EXPECT_NE(errorOf(R"({ "fields": [] })").find("'name'"), std::string::npos);
}

// ---- shipped telemetry files ------------------------------------------------------

TEST(TlmShipped, SizesMatchTheFsw)
{
    // sizeof() of each FSW telemetry struct, header included (NTPM PC build,
    // mcs_tlm_reference.md Part 2). The CF PDU (0x092D) has variable length.
    const std::map<std::uint16_t, std::size_t> fswSizeof{
        {0x0800, 156}, {0x080B, 200}, {0x0810, 248}, {0x080F, 76},                    // es
        {0x0801, 156}, {0x0808, 168}, {0x0809, 44},                                   // evs
        {0x0803, 48},  {0x080A, 584}, {0x080D, 144}, {0x080E, 20},                    // sb
        {0x0804, 304}, {0x080C, 200},                                                 // tbl
        {0x0805, 44},  {0x0806, 212},                                                 // time
        {0x09A0, 28},  {0x09A2, 80},  {0x09A3, 30},                                   // to
        {0x0934, 16},                                                                 // ci
        {0x0997, 64},  {0x0998, 126},                                                 // sch
        {0x098A, 88},                                                                 // sc
        {0x096D, 176}, {0x096E, 16},                                                  // hs
        {0x0920, 368}, {0x0921, 444}, {0x0922, 360},                                  // cf
        {0x095A, 23},  {0x095B, 96},  {0x095C, 1608}, {0x095D, 4216}, {0x095E, 588},  // fm
        {0x0960, 84},  {0x0961, 84},
        {0x0948, 108}, {0x0949, 1356},                                                // ds
        {0x0977, 220},                                                                // lc
        {0x0911, 16},  {0x0912, 408}, {0x0913, 80},  {0x0916, 48},  {0x0917, 144},    // aocs
        {0x0918, 40},  {0x0919, 416},
        {0x0909, 28},  {0x090A, 416}, {0x0901, 24},  {0x0903, 24},  {0x0904, 24},     // main
        {0x0905, 24},  {0x0906, 56},  {0x0907, 64},  {0x0908, 88},
        {0x09F1, 24},                                                                 // pl
        {0x0883, 32},                                                                 // fdir
    };
    const TelemetryCatalog c = shippedCatalog();
    for (const auto& [mid, size] : fswSizeof) {
        const TlmPacketDef* p = c.find(mid);
        ASSERT_NE(p, nullptr) << toHexString(mid, 4);
        EXPECT_EQ(kTlmHeaderSize + p->payloadSize(), size) << p->fullName();
        EXPECT_FALSE(p->variableLength) << p->fullName();
    }
    const TlmPacketDef* pdu = c.find(0x092D);
    ASSERT_NE(pdu, nullptr);
    EXPECT_TRUE(pdu->variableLength);
    EXPECT_EQ(c.packets().size(), fswSizeof.size() + 1);   // nothing else is defined
}

TEST(TlmShipped, OffsetsAfterCompilerHoles)
{
    // Payload offsets (absolute - 12) from the FSW offsetof output.
    const TelemetryCatalog c = shippedCatalog();
    const auto offset = [&](std::uint16_t mid, const std::string& name) -> std::size_t {
        const TlmFieldDef* f = c.find(mid)->findField(name);
        return f != nullptr ? f->offset : 9999;
    };
    EXPECT_EQ(offset(0x0810, "pool_handle"), 4U);
    EXPECT_EQ(offset(0x0810, "block_stats_16_num_free"), 228U);
    EXPECT_EQ(offset(0x0803, "mem_pool_handle"), 20U);
    EXPECT_EQ(offset(0x0804, "mem_pool_handle"), 68U);
    EXPECT_EQ(offset(0x080C, "owner_app_name"), 160U);
    EXPECT_EQ(offset(0x0801, "app_data_15_enable_status"), 142U);
    EXPECT_EQ(offset(0x0906, "mode_stage"), 8U);
    EXPECT_EQ(offset(0x0907, "ats_table_handler"), 50U);
    EXPECT_EQ(offset(0x0908, "time_deactive_prop"), 68U);
    EXPECT_EQ(offset(0x09A2, "dw1"), 16U);
    EXPECT_EQ(offset(0x09A2, "df1"), 36U);
    EXPECT_EQ(offset(0x0912, "qbn_1_3"), 132U);
    EXPECT_EQ(offset(0x0912, "rw_status_3"), 393U);
    EXPECT_EQ(offset(0x0920, "chan_1_sem_value"), 352U);
    EXPECT_EQ(offset(0x0921, "app_dst_file"), 368U);
    EXPECT_EQ(offset(0x095C, "entry_19_mode"), 1592U);
    EXPECT_EQ(offset(0x095E, "file_sys_7_name"), 512U);
    EXPECT_EQ(offset(0x0949, "file_15_name"), 1280U);
    EXPECT_EQ(offset(0x0977, "ap_results_0"), 100U);
    EXPECT_EQ(offset(0x080A, "pipe_stats_63_peak_in_use"), 570U);
}

TEST(TlmShipped, DecodesTheToDataTypePattern)
{
    // TO_SendDataTypePktCmd fills TO_TypeDefPacket_t with constants to check
    // byte order and packing (x86-64 little-endian, gcc bitfields LSB first).
    Bytes payload(68, 0);
    const auto put = [&](std::size_t at, const void* v, std::size_t n) { std::memcpy(&payload[at], v, n); };
    const std::uint16_t synch = 0x6969, bits = 0x4A79;
    const std::int16_t w1 = 0x2468, w2 = 0x7FFF;
    const std::int32_t dw1 = 0x12345678, dw2 = static_cast<std::int32_t>(0x87654321U);
    const float f1 = 90.01f;
    const double df1 = 99.9;
    put(0, &synch, 2);
    put(2, &bits, 2);
    payload[5] = 1;      // bl2
    payload[6] = 16;     // b1
    payload[9] = 0x45;   // b4
    put(10, &w1, 2);
    put(12, &w2, 2);
    put(16, &dw1, 4);
    put(20, &dw2, 4);
    put(24, &f1, 4);
    put(36, &df1, 8);
    std::memcpy(&payload[52], "ABCDEFGHIJ", 10);

    const TelemetryCatalog c = shippedCatalog();
    TlmStore store(c, Endian::Little);
    store.ingest(tlmPacket(0x09A2, 0, payload, 1000, 0, Endian::Little));
    const TlmSnapshot s = store.snapshot();
    ASSERT_EQ(s.packets.size(), 1U);
    const PacketState& p = s.packets[0];
    EXPECT_EQ(p.seconds, 1000U);
    EXPECT_EQ(valueOf(p, "synch"), "0x6969");
    EXPECT_EQ(valueOf(p, "bits"), "0x4A79");
    EXPECT_EQ(valueOf(p, "bl2"), "1");
    EXPECT_EQ(valueOf(p, "b1"), "16");
    EXPECT_EQ(valueOf(p, "b4"), "0x45");
    EXPECT_EQ(valueOf(p, "w1"), "0x2468");
    EXPECT_EQ(valueOf(p, "w2"), "0x7FFF");
    EXPECT_EQ(valueOf(p, "dw1"), "0x12345678");
    EXPECT_EQ(valueOf(p, "dw2"), "0x87654321");
    EXPECT_EQ(valueOf(p, "f1"), "90.01");
    EXPECT_EQ(valueOf(p, "df1"), "99.9");
    EXPECT_EQ(valueOf(p, "str"), "ABCDEFGHIJ");
    EXPECT_EQ(s.counters.received, 1U);
    EXPECT_EQ(s.counters.lengthErrors, 0U);
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

    const TlmSnapshot s = store.snapshot();
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

    store.ingest(tlmPacket(0x0808, 0, evsLongEventPayload("CI", 5, 3, "Invalid length"), 100, 0,
                           Endian::Little));
    store.ingest(tlmPacket(0x0899, 0, Bytes(30, 0)));   // a spare MID: not in the catalog
    store.ingest(tlmPacket(0x0899, 1, Bytes(30, 0)));
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
    EXPECT_EQ(s.unknown[0].mid, 0x0899);
    EXPECT_EQ(s.unknown[0].count, 2U);
    EXPECT_EQ(s.unknown[0].lastLength, 42U);
    EXPECT_EQ(s.unknown[0].sequence, 1);
    EXPECT_EQ(s.unknown[0].lastPacket, tlmPacket(0x0899, 1, Bytes(30, 0)));
    EXPECT_EQ(s.counters.unknown, 2U);
    EXPECT_EQ(s.counters.malformed, 1U);
    EXPECT_EQ(s.counters.received, 4U);
    EXPECT_EQ(s.counters.gaps, 0U);
}

TEST(TlmStore, SequenceGapsIncludingWrap)
{
    const TelemetryCatalog c = catalogWithDsAndEvs();
    TlmStore store(c, Endian::Little);

    store.ingest(tlmPacket(0x08B8, 0x3FFE, dsHkPayload(0)));
    store.ingest(tlmPacket(0x08B8, 0x3FFF, dsHkPayload(0)));
    store.ingest(tlmPacket(0x08B8, 0, dsHkPayload(0)));   // wrap: not a gap
    store.ingest(tlmPacket(0x08B8, 3, dsHkPayload(0)));   // 1 and 2 lost

    const TlmSnapshot s = store.snapshot();
    ASSERT_EQ(s.packets.size(), 1U);
    EXPECT_EQ(s.packets[0].gaps, 1U);
    EXPECT_EQ(s.counters.gaps, 1U);

    store.ingest(tlmPacket(0x0899, 5, {}));
    store.ingest(tlmPacket(0x0899, 7, {}));   // unknown MIDs are checked too
    EXPECT_EQ(store.snapshot().unknown[0].gaps, 1U);
    EXPECT_EQ(store.counters().gaps, 2U);
}

TEST(TlmStore, LengthMismatchStillDecodes)
{
    const TelemetryCatalog c = catalogWithDsAndEvs();
    TlmStore store(c, Endian::Little);
    store.ingest(tlmPacket(0x08B8, 0, {9, 0}));

    const TlmSnapshot s = store.snapshot();
    EXPECT_EQ(valueOf(s.packets[0], "cmd_accepted"), "9");
    EXPECT_EQ(s.packets[0].lengthErrors, 1U);
    EXPECT_EQ(s.counters.lengthErrors, 1U);
}

TEST(TlmStore, VariableLengthPackets)
{
    TlmPacketDef pdu;
    pdu.app = "cf";
    pdu.name = "pdu";
    pdu.mid = 0x092D;
    pdu.variableLength = true;
    pdu.fields.push_back(field("octet1", FieldType::U8));
    TelemetryCatalog c;
    c.add(pdu);
    TlmStore store(c, Endian::Little);

    store.ingest(tlmPacket(0x092D, 0, Bytes(40, 0x24)));   // longer than the fields: fine
    EXPECT_EQ(store.counters().lengthErrors, 0U);
    store.ingest(tlmPacket(0x092D, 1, {}));                 // shorter: length error
    EXPECT_EQ(store.counters().lengthErrors, 1U);
    EXPECT_EQ(store.snapshot().packets[0].lengthErrors, 1U);
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

    store.clear();
    const TlmSnapshot s = store.snapshot();
    EXPECT_TRUE(s.packets.empty());
    EXPECT_TRUE(s.events.empty());
    EXPECT_EQ(s.counters.received, 0U);
    EXPECT_EQ(s.counters.gaps, 0U);
}

namespace {

// Hands out queued datagrams, then times out.
class QueueReceiver : public IReceiver {
public:
    explicit QueueReceiver(std::vector<Bytes> packets) : packets_(std::move(packets)) {}
    std::optional<Bytes> receive(std::chrono::milliseconds timeout) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (next_ < packets_.size()) {
            return packets_[next_++];
        }
        std::this_thread::sleep_for(std::min(timeout, std::chrono::milliseconds(5)));
        return std::nullopt;
    }
    std::string describe() const override { return "queue://"; }

private:
    std::mutex mutex_;
    std::vector<Bytes> packets_;
    std::size_t next_ = 0;
};

} // namespace

TEST(TelemetryService, SinksSeeEveryDatagramAfterTheStore)
{
    TelemetryCatalog catalog;
    TlmStore store(catalog, Endian::Little);
    const Bytes a = test::tlmPacket(0x092D, 1, {0x04, 0x00});
    const Bytes b = test::tlmPacket(0x0899, 2, {0x01});
    TelemetryService service(std::make_unique<QueueReceiver>(std::vector<Bytes>{a, b}), store);
    std::mutex mutex;
    std::vector<Bytes> seen;
    std::size_t storedWhenSeen = 0;
    service.addSink([&](const Bytes& d) {
        std::lock_guard<std::mutex> lock(mutex);
        seen.push_back(d);
        storedWhenSeen = store.counters().received;
    });
    service.addSink([](const Bytes&) { throw std::runtime_error("sink failed"); });
    service.start({});
    EXPECT_THROW(service.addSink([](const Bytes&) {}), std::logic_error);
    for (int i = 0; i < 200; ++i) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (seen.size() == 2) {
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    service.stop();
    ASSERT_EQ(seen.size(), 2u);
    EXPECT_EQ(seen[0], a);
    EXPECT_EQ(seen[1], b);
    EXPECT_EQ(storedWhenSeen, 2u);   // the store had the packet before the sink ran
}
