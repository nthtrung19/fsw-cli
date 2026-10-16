// GsfcEngine: the GSFC CFDP library (as in CF 2.2.1) behind ICfdpEngine.
//
// Only one engine can exist per process, so the class 1 round trip runs the
// sender first, keeps its PDUs, then feeds them to a receiver engine that
// plays CF's role (entity 0.24). Class 2 needs both entities at once: see
// test_cfdp_two_entities.cpp.

#include "cfdp/gsfc_engine.hpp"
#include "cfdp/pdu_info.hpp"
#include "core/text.hpp"

#include "cfdp_data_structures.h"
#include "test_helpers.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <fstream>
#include <iterator>
#include <thread>

using namespace mcs;
using namespace mcs::cfdp;
namespace fs = std::filesystem;

namespace {

std::string readFile(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string pattern(std::size_t size)
{
    std::string s(size, '\0');
    for (std::size_t i = 0; i < size; ++i) {
        s[i] = static_cast<char>((i * 7 + 3) & 0xFF);
    }
    return s;
}

fs::path writeBinary(const fs::path& p, const std::string& content)
{
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << content;
    return p;
}

// CFDP modular checksum, written independently of the library: the file as
// big-endian 32-bit words aligned on file offsets, summed modulo 2^32.
std::uint32_t modularChecksum(const std::string& data)
{
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i < data.size(); ++i) {
        sum += static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[i])) << (8 * (3 - i % 4));
    }
    return sum;
}

std::uint32_t be32(const Bytes& b, std::size_t at)
{
    return (std::uint32_t{b[at]} << 24) | (std::uint32_t{b[at + 1]} << 16) | (std::uint32_t{b[at + 2]} << 8)
         | b[at + 3];
}

struct Recorder {
    std::vector<Bytes> pdus;
    std::vector<std::pair<EngineEvent, TransferStatus>> events;
    std::vector<std::string> log;

    ICfdpEngine::PduOut out()
    {
        return [this](const Bytes& pdu, const PduInfo&) { pdus.push_back(pdu); };
    }
    ICfdpEngine::Notify notify()
    {
        return [this](EngineEvent e, const TransferStatus& s) { events.emplace_back(e, s); };
    }
    ICfdpEngine::Log logger()
    {
        return [this](int level, const std::string& text) { log.push_back(std::to_string(level) + " " + text); };
    }
    const TransferStatus* finished() const
    {
        for (const auto& [e, s] : events) {
            if (e == EngineEvent::Finished) {
                return &s;
            }
        }
        return nullptr;
    }
    std::string logText() const { return join(log, "\n"); }
};

Config groundConfig(const fs::path& downlinkDir)
{
    Config c;
    c.downlinkDir = downlinkDir;
    c.chunkSize = 512;
    c.pdusPerSecond = 1000000;   // pacing out of the way
    return c;
}

Config flightConfig(const fs::path& downlinkDir)
{
    Config c = groundConfig(downlinkDir);
    c.localEntity = "0.24";
    c.remoteEntity = "0.23";
    return c;
}

// Runs a class 1 put of `content` from the ground engine; returns its PDUs.
std::vector<Bytes> sendClass1(const test::TempDir& tmp, const std::string& name, const std::string& content,
                              const std::string& remote, Recorder& rec)
{
    const fs::path file = writeBinary(tmp.path() / "src" / name, content);
    GsfcEngine engine(groundConfig(tmp.path() / "ground-dl"), rec.out(), rec.notify(), rec.logger());
    std::string error;
    const auto id = engine.put({file, remote, 1}, error);
    EXPECT_TRUE(id) << error << "\n" << rec.logText();
    for (int i = 0; i < 100 && rec.finished() == nullptr; ++i) {
        engine.cycle();
    }
    return rec.pdus;
}

// Feeds `pdus` to a receiver engine playing CF (0.24); returns its recorder.
void receive(const fs::path& downlinkDir, const std::vector<Bytes>& pdus, Recorder& rec)
{
    GsfcEngine engine(flightConfig(downlinkDir), rec.out(), rec.notify(), rec.logger());
    for (const auto& pdu : pdus) {
        engine.givePdu(pdu);
        engine.cycle();
    }
    for (int i = 0; i < 10; ++i) {
        engine.cycle();
    }
}

} // namespace

TEST(GsfcEngine, LibraryTypesHaveFlightSizes)
{
    // The same sizes as in the 32-bit flight build (see external/cfdp-gsfc/README.md).
    EXPECT_EQ(sizeof(u_int_1), 1u);
    EXPECT_EQ(sizeof(u_int_2), 4u);
    EXPECT_EQ(sizeof(u_int_4), 4u);
    EXPECT_EQ(sizeof(s_int_4), 4u);
}

TEST(GsfcEngine, OnlyOneEngineAtATime)
{
    test::TempDir tmp;
    Recorder rec;
    GsfcEngine first(groundConfig(tmp.path()), rec.out(), rec.notify(), rec.logger());
    EXPECT_THROW(GsfcEngine(groundConfig(tmp.path()), rec.out(), rec.notify(), rec.logger()), std::logic_error);
}

TEST(GsfcEngine, RejectsBadEntityIds)
{
    test::TempDir tmp;
    Config c = groundConfig(tmp.path());
    c.localEntity = "23";   // one byte: CF accepts only 2-byte IDs
    EXPECT_THROW(GsfcEngine(c, {}, {}, {}), ConfigError);
    // The failed constructor must not block the next engine.
    EXPECT_NO_THROW(GsfcEngine(groundConfig(tmp.path()), {}, {}, {}));
}

TEST(GsfcEngine, Class1PutSendsMetadataFileDataAndEof)
{
    test::TempDir tmp;
    Recorder rec;
    const std::string content = pattern(1300);
    const auto pdus = sendClass1(tmp, "test.bin", content, "/cf/test.bin", rec);
    ASSERT_EQ(pdus.size(), 5u) << rec.logText();   // MD, 3 x FD (512 + 512 + 276), EOF

    // Metadata: 12-byte header, ground 0.23 -> CF 0.24, class 1.
    const auto md = inspectPdu(pdus[0]);
    ASSERT_TRUE(md);
    EXPECT_EQ(md->kind, PduKind::Metadata);
    EXPECT_EQ(md->headerLength, 12u);
    EXPECT_EQ(md->id.source, "0.23");
    EXPECT_EQ(md->destination, "0.24");
    EXPECT_EQ(md->cls, 1);
    EXPECT_FALSE(md->towardSender);
    const std::size_t d = md->headerLength;
    EXPECT_EQ(be32(pdus[0], d + 2), 1300u);                      // file size
    const std::size_t srcLen = pdus[0][d + 6];
    EXPECT_EQ(std::string(pdus[0].begin() + static_cast<long>(d + 7),
                          pdus[0].begin() + static_cast<long>(d + 7 + srcLen)), "test.bin");
    const std::size_t dstLen = pdus[0][d + 7 + srcLen];
    EXPECT_EQ(std::string(pdus[0].begin() + static_cast<long>(d + 8 + srcLen),
                          pdus[0].begin() + static_cast<long>(d + 8 + srcLen + dstLen)), "/cf/test.bin");

    // FileData: consecutive offsets, at most chunkSize bytes, the file's bytes.
    std::string data;
    for (std::size_t i = 1; i <= 3; ++i) {
        const auto fd = inspectPdu(pdus[i]);
        ASSERT_TRUE(fd);
        EXPECT_EQ(fd->kind, PduKind::FileData);
        EXPECT_EQ(fd->offset, data.size());
        EXPECT_LE(fd->fileDataLength, 512u);
        const auto begin = pdus[i].begin() + static_cast<long>(fd->headerLength + 4);
        data.append(begin, begin + static_cast<long>(fd->fileDataLength));
    }
    EXPECT_EQ(data, content);

    // EOF: no error, our independent checksum, the file size.
    const auto eof = inspectPdu(pdus[4]);
    ASSERT_TRUE(eof);
    EXPECT_EQ(eof->kind, PduKind::Eof);
    EXPECT_EQ(eof->condition, 0);
    EXPECT_EQ(be32(pdus[4], eof->headerLength + 2), modularChecksum(content));
    EXPECT_EQ(be32(pdus[4], eof->headerLength + 6), 1300u);

    const TransferStatus* done = rec.finished();
    ASSERT_NE(done, nullptr);
    EXPECT_EQ(done->state, State::Done);
    EXPECT_EQ(done->direction, Direction::Uplink);
    EXPECT_EQ(done->remote, "/cf/test.bin");
    EXPECT_EQ(done->bytesDone, 1300u);
    EXPECT_EQ(done->sent.fileData, 3u);
}

TEST(GsfcEngine, Class1RoundTripStoresFileUnderDownlinkDir)
{
    test::TempDir tmp;
    Recorder sender;
    const std::string content = pattern(10000);
    const auto pdus = sendClass1(tmp, "hk_0001.dat", content, "/cf/log/hk_0001.dat", sender);

    Recorder receiver;
    const fs::path dl = tmp.path() / "downlink";
    receive(dl, pdus, receiver);
    const TransferStatus* done = receiver.finished();
    ASSERT_NE(done, nullptr) << receiver.logText();
    EXPECT_EQ(done->state, State::Done) << done->condition << "\n" << receiver.logText();
    EXPECT_EQ(done->direction, Direction::Downlink);
    EXPECT_FALSE(done->requested);
    EXPECT_EQ(done->remote, "hk_0001.dat");   // the Metadata source name (the sender's alias)
    EXPECT_EQ(fs::path(done->local), dl / "cf" / "log" / "hk_0001.dat");
    EXPECT_EQ(readFile(dl / "cf" / "log" / "hk_0001.dat"), content);
    EXPECT_EQ(done->received.fileData, 20u);
    EXPECT_TRUE(fs::is_empty(dl / ".part"));   // no temp file left

    // Started came first, with the names already known.
    ASSERT_FALSE(receiver.events.empty());
    EXPECT_EQ(receiver.events.front().first, EngineEvent::Started);
    EXPECT_EQ(receiver.events.front().second.fileSize, 10000u);
}

TEST(GsfcEngine, ReceivedFileNeverOverwritesAnExistingOne)
{
    test::TempDir tmp;
    const fs::path dl = tmp.path() / "downlink";
    writeBinary(dl / "cf" / "a.bin", "old");
    Recorder sender;
    const auto pdus = sendClass1(tmp, "a.bin", pattern(100), "/cf/a.bin", sender);
    Recorder receiver;
    receive(dl, pdus, receiver);
    ASSERT_NE(receiver.finished(), nullptr);
    EXPECT_EQ(fs::path(receiver.finished()->local), dl / "cf" / "a(1).bin");
    EXPECT_EQ(readFile(dl / "cf" / "a.bin"), "old");
    EXPECT_EQ(readFile(dl / "cf" / "a(1).bin"), pattern(100));
}

TEST(GsfcEngine, LostFileDataFailsClass1WithoutLeavingAFile)
{
    test::TempDir tmp;
    Recorder sender;
    auto pdus = sendClass1(tmp, "a.bin", pattern(3000), "/cf/a.bin", sender);
    ASSERT_GE(pdus.size(), 4u);
    pdus.erase(pdus.begin() + 2);   // drop the second FileData
    Recorder receiver;
    const fs::path dl = tmp.path() / "downlink";
    receive(dl, pdus, receiver);
    const TransferStatus* done = receiver.finished();
    ASSERT_NE(done, nullptr);
    EXPECT_EQ(done->state, State::Failed);
    EXPECT_TRUE(done->condition == "FILE_SIZE_ERROR" || done->condition == "FILE_CHECKSUM_FAILURE")
        << done->condition;
    EXPECT_FALSE(fs::exists(dl / "cf" / "a.bin"));
    EXPECT_TRUE(fs::is_empty(dl / ".part"));
}

TEST(GsfcEngine, ReceivedNamesThatLeaveTheDownlinkDirAreRefused)
{
    test::TempDir tmp;
    Recorder sender;
    // The sender's own check stops "..", so build the PDUs with a legal name
    // and rewrite the destination in the Metadata.
    auto pdus = sendClass1(tmp, "a.bin", pattern(100), "/cf/xx/a.bin", sender);
    auto& md = pdus.front();
    const auto info = inspectPdu(md);
    ASSERT_TRUE(info);
    const std::size_t d = info->headerLength;
    const std::size_t srcLen = md[d + 6];
    const std::size_t dst = d + 8 + srcLen;   // "/cf/xx/a.bin"
    md[dst + 4] = '.';
    md[dst + 5] = '.';                         // "/cf/../a.bin"
    Recorder receiver;
    const fs::path dl = tmp.path() / "downlink";
    receive(dl, pdus, receiver);
    ASSERT_NE(receiver.finished(), nullptr);
    EXPECT_EQ(receiver.finished()->state, State::Failed);
    EXPECT_EQ(receiver.finished()->condition, "FILESTORE_REJECTION");
    EXPECT_FALSE(fs::exists(dl / "a.bin"));
    EXPECT_FALSE(fs::exists(tmp.path() / "a.bin"));
}

TEST(GsfcEngine, PutChecksTheRequestBeforeSendingAnything)
{
    test::TempDir tmp;
    Recorder rec;
    const fs::path file = writeBinary(tmp.path() / "a.bin", "data");
    GsfcEngine engine(groundConfig(tmp.path() / "dl"), rec.out(), rec.notify(), rec.logger());
    std::string error;
    EXPECT_FALSE(engine.put({tmp.path() / "missing.bin", "/cf/a.bin", 1}, error));
    EXPECT_NE(error.find("no such file"), std::string::npos) << error;
    EXPECT_FALSE(engine.put({file, "cf/a.bin", 1}, error));
    EXPECT_NE(error.find("must start with '/'"), std::string::npos) << error;
    EXPECT_FALSE(engine.put({file, "/cf/a b.bin", 1}, error));
    EXPECT_FALSE(engine.put({file, "/cf/" + std::string(70, 'x'), 1}, error));
    EXPECT_FALSE(engine.put({file, "/cf/a.bin", 3}, error));
    engine.cycle();
    EXPECT_TRUE(rec.pdus.empty());
    EXPECT_TRUE(engine.transfers().empty());
}

TEST(GsfcEngine, LongOrOddLocalNamesUseAShortAlias)
{
    test::TempDir tmp;
    Recorder rec;
    const std::string longName = "my results " + std::string(60, 'r') + ".dat";
    const fs::path file = writeBinary(tmp.path() / longName, "data");
    GsfcEngine engine(groundConfig(tmp.path() / "dl"), rec.out(), rec.notify(), rec.logger());
    std::string error;
    ASSERT_TRUE(engine.put({file, "/cf/r.dat", 1}, error)) << error << rec.logText();
    engine.cycle();
    ASSERT_FALSE(rec.pdus.empty());
    const auto md = inspectPdu(rec.pdus.front());
    ASSERT_TRUE(md);
    const std::size_t srcLen = rec.pdus.front()[md->headerLength + 6];
    EXPECT_LE(srcLen, 40u);
    const std::string alias(rec.pdus.front().begin() + static_cast<long>(md->headerLength + 7),
                            rec.pdus.front().begin() + static_cast<long>(md->headerLength + 7 + srcLen));
    EXPECT_EQ(alias.find(' '), std::string::npos);
    EXPECT_EQ(alias.substr(alias.size() - 4), ".dat");
    EXPECT_EQ(fs::path(engine.transfers().front().local), fs::absolute(file));
}

TEST(GsfcEngine, PacingLimitsFileDataButNotDirectives)
{
    test::TempDir tmp;
    Recorder rec;
    const fs::path file = writeBinary(tmp.path() / "big.bin", pattern(512 * 60));
    Config c = groundConfig(tmp.path() / "dl");
    c.pdusPerSecond = 40;   // bucket of 2, then 40 per second
    GsfcEngine engine(c, rec.out(), rec.notify(), rec.logger());
    std::string error;
    ASSERT_TRUE(engine.put({file, "/cf/big.bin", 1}, error)) << error;
    engine.cycle();
    ASSERT_FALSE(rec.pdus.empty());
    EXPECT_EQ(inspectPdu(rec.pdus.front())->kind, PduKind::Metadata);   // not delayed

    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(500)) {
        engine.cycle();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    std::size_t fd = 0;
    for (const auto& p : rec.pdus) {
        fd += inspectPdu(p)->kind == PduKind::FileData ? 1u : 0u;
    }
    EXPECT_GE(fd, 10u);   // about 2 + 40 * 0.5
    EXPECT_LE(fd, 26u);
    const auto status = engine.transfers().front();
    EXPECT_EQ(status.state, State::Active);
    EXPECT_GT(status.bytesDone, 0u);
}

TEST(GsfcEngine, CancelAndAbandonNeedAKnownTransaction)
{
    test::TempDir tmp;
    Recorder rec;
    const fs::path file = writeBinary(tmp.path() / "big.bin", pattern(512 * 40));
    Config c = groundConfig(tmp.path() / "dl");
    c.pdusPerSecond = 10;
    GsfcEngine engine(c, rec.out(), rec.notify(), rec.logger());
    std::string error;
    EXPECT_FALSE(engine.request(RequestKind::Cancel, "nonsense", error));
    EXPECT_FALSE(engine.request(RequestKind::Cancel, "0.23_5", error));
    EXPECT_NE(error.find("no active transfer"), std::string::npos);

    const auto id = engine.put({file, "/cf/big.bin", 1}, error);
    ASSERT_TRUE(id) << error;
    engine.cycle();
    ASSERT_TRUE(engine.request(RequestKind::Abandon, id->toString(), error)) << error << rec.logText();
    engine.cycle();
    const TransferStatus* done = rec.finished();
    ASSERT_NE(done, nullptr);
    EXPECT_EQ(done->state, State::Abandoned);
    EXPECT_EQ(engine.transfers().front().state, State::Abandoned);
}

TEST(GsfcEngine, DownlinkPathRules)
{
    const fs::path dl = "/data/downlink";
    EXPECT_EQ(GsfcEngine::downlinkPath(dl, "/cf/a.bin"), dl / "cf" / "a.bin");
    EXPECT_EQ(GsfcEngine::downlinkPath(dl, "cftesting/x.dat"), dl / "cftesting" / "x.dat");
    EXPECT_EQ(GsfcEngine::downlinkPath(dl, "//cf//a.bin"), dl / "cf" / "a.bin");
    EXPECT_FALSE(GsfcEngine::downlinkPath(dl, ""));
    EXPECT_FALSE(GsfcEngine::downlinkPath(dl, "/"));
    EXPECT_FALSE(GsfcEngine::downlinkPath(dl, "/cf/../../etc/passwd"));
    EXPECT_FALSE(GsfcEngine::downlinkPath(dl, "/../a.bin"));
    EXPECT_FALSE(GsfcEngine::downlinkPath(dl, "/cf/"));
}

TEST(GsfcEngine, RemotePathRules)
{
    EXPECT_EQ(GsfcEngine::checkRemotePath("/cf/a.bin"), "");
    EXPECT_NE(GsfcEngine::checkRemotePath("cf/a.bin"), "");
    EXPECT_NE(GsfcEngine::checkRemotePath("/cf/a b.bin"), "");
    EXPECT_NE(GsfcEngine::checkRemotePath("/cf/"), "");
    EXPECT_NE(GsfcEngine::checkRemotePath("/" + std::string(63, 'x')), "");
    EXPECT_EQ(GsfcEngine::checkRemotePath("/" + std::string(62, 'x')), "");
}

TEST(PduInfo, RejectsTruncatedPdus)
{
    EXPECT_FALSE(inspectPdu({}));
    EXPECT_FALSE(inspectPdu({0x04, 0x00, 0x10, 0x13}));   // header promises more than given
    const auto id = parseTransactionId("0.24_12");
    ASSERT_TRUE(id);
    EXPECT_EQ(id->source, "0.24");
    EXPECT_EQ(id->seq, 12u);
    EXPECT_FALSE(parseTransactionId("0.24"));
    EXPECT_FALSE(parseTransactionId("a.b_1"));
}

// Golden data from the real CF 2.2.1 on the SIL (tests/data/cfdp/README.md):
// the 0x092D telemetry packets of a class 1 playback, captured from the UDP
// stream, must rebuild the original file in mcs's engine.
TEST(GsfcEngine, RebuildsAFileFromRealCfPackets)
{
    const fs::path data = fs::path(mcs_SOURCE_DIR) / "tests" / "data" / "cfdp";
    const fs::path capture = data / "cf_class1_playback.bin";
    const fs::path original = data / "cf_class1_playback.src";
    if (!fs::exists(capture) || !fs::exists(original)) {
        GTEST_SKIP() << "no capture from the SIL in " << data;
    }
    // Records: u32 big-endian length, then the whole UDP datagram.
    const std::string raw = readFile(capture);
    std::vector<Bytes> pdus;
    for (std::size_t at = 0; at + 4 <= raw.size();) {
        const auto* p = reinterpret_cast<const std::uint8_t*>(raw.data() + at);
        const std::size_t length = (std::size_t{p[0]} << 24) | (std::size_t{p[1]} << 16) | (std::size_t{p[2]} << 8) | p[3];
        ASSERT_LE(at + 4 + length, raw.size());
        ASSERT_GT(length, 12u);
        EXPECT_EQ((p[4] << 8) | p[5], 0x092D);   // CF_SPACE_TO_GND_PDU_MID, telemetry
        pdus.emplace_back(p + 4 + 12, p + 4 + length);
        at += 4 + length;
    }
    ASSERT_GE(pdus.size(), 3u);
    EXPECT_EQ(inspectPdu(pdus.front())->kind, PduKind::Metadata);
    EXPECT_EQ(inspectPdu(pdus.front())->headerLength, 12u);
    EXPECT_EQ(inspectPdu(pdus.front())->id.source, "0.24");
    EXPECT_EQ(inspectPdu(pdus.back())->kind, PduKind::Eof);

    test::TempDir tmp;
    Recorder rec;
    {
        GsfcEngine ground(groundConfig(tmp.path() / "downlink"), rec.out(), rec.notify(), rec.logger());
        for (const auto& pdu : pdus) {
            EXPECT_TRUE(ground.givePdu(pdu));
            ground.cycle();
        }
        ground.cycle();
    }
    const TransferStatus* done = rec.finished();
    ASSERT_NE(done, nullptr) << rec.logText();
    EXPECT_EQ(done->state, State::Done) << done->condition << "\n" << rec.logText();
    EXPECT_EQ(readFile(done->local), readFile(original));
}
