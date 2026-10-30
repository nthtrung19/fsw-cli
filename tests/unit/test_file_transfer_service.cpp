// FileTransferService: the CFDP engine wired to the pipeline (uplink), the
// telemetry stream (downlink) and the command service (file get).

#include "cfdp/gsfc_engine.hpp"
#include "cfdp/pdu_info.hpp"
#include "config/catalog_loader.hpp"
#include "encode/payload_encoder.hpp"
#include "protocol/ccsds_v1_format.hpp"
#include "service/file_transfer_service.hpp"
#include "unit/test_helpers.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <fstream>
#include <iterator>
#include <sstream>
#include <thread>

using namespace mcs;
using namespace mcs::cfdp;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

const std::string kSourceDir = mcs_SOURCE_DIR;

std::string readFile(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string pattern(std::size_t size)
{
    std::string s(size, '\0');
    for (std::size_t i = 0; i < size; ++i) {
        s[i] = static_cast<char>((i * 31 + 7) & 0xFF);
    }
    return s;
}

fs::path writeBinary(const fs::path& p, const std::string& content)
{
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << content;
    return p;
}

// Records what is sent; the worker thread sends while the test reads.
class LockedTransport : public ITransport {
public:
    explicit LockedTransport(bool dryRun = false) : dryRun_(dryRun) {}
    void send(const Bytes& data) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sent_.push_back(data);
    }
    std::string describe() const override { return "locked://"; }
    bool isDryRun() const override { return dryRun_; }
    std::vector<Bytes> sent() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return sent_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<Bytes> sent_;
    bool dryRun_;
};

// The PDUs CF would send for a class 1 playback of `remote`: produced by a
// GsfcEngine playing CF, with the Metadata names set the way CF sets them
// (source = the file path on the FSW, destination as given in playback_file).
std::vector<Bytes> cfPlayback(const test::TempDir& tmp, const std::string& remote, const std::string& content)
{
    const fs::path src = writeBinary(tmp.path() / "flight" / "file.bin", content);
    Config c;
    c.localEntity = "0.24";
    c.remoteEntity = "0.23";
    c.downlinkDir = tmp.path() / "flight-dl";
    c.chunkSize = 4000;   // like CF's table
    c.pdusPerSecond = 1000000;
    std::vector<Bytes> pdus;
    bool done = false;
    {
        GsfcEngine flight(
            c, [&](const Bytes& p, const PduInfo&) { pdus.push_back(p); },
            [&](EngineEvent e, const TransferStatus&) { done = done || e == EngineEvent::Finished; }, {});
        std::string error;
        EXPECT_TRUE(flight.put({src, remote, 1}, error)) << error;
        for (int i = 0; i < 100 && !done; ++i) {
            flight.cycle();
        }
    }
    // Rewrite the Metadata names: header, 0x07, flags, size(4), LV source, LV destination.
    Bytes& md = pdus.front();
    const auto info = inspectPdu(md);
    const std::size_t d = info->headerLength;
    Bytes rebuilt(md.begin(), md.begin() + static_cast<long>(d + 6));
    for (const std::string& name : {remote, remote}) {
        rebuilt.push_back(static_cast<std::uint8_t>(name.size()));
        rebuilt.insert(rebuilt.end(), name.begin(), name.end());
    }
    const std::size_t dataLength = rebuilt.size() - d;
    rebuilt[1] = static_cast<std::uint8_t>(dataLength >> 8);
    rebuilt[2] = static_cast<std::uint8_t>(dataLength);
    md = rebuilt;
    return pdus;
}

// What TO sends: each PDU in a CF_SPACE_TO_GND_PDU_MID telemetry packet.
std::vector<Bytes> asTelemetry(const std::vector<Bytes>& pdus)
{
    std::vector<Bytes> out;
    std::uint16_t seq = 0;
    for (const auto& p : pdus) {
        out.push_back(test::tlmPacket(0x092D, seq++, p, 0, 0, Endian::Little));
    }
    return out;
}

// One CFDP PDU out of a sent CF command packet (8-byte command header).
Bytes pduOf(const Bytes& packet)
{
    return Bytes(packet.begin() + 8, packet.end());
}

struct Rig {
    explicit Rig(const test::TempDir& tmp, bool dryRun = false, unsigned pdusPerSecond = 100000)
        : catalog(loadCatalog({kSourceDir + "/config/telecommand/cf.json"})),
          transport(new LockedTransport(dryRun)),
          pipeline(std::make_unique<CcsdsV1Format>(Endian::Little), {}, std::unique_ptr<ITransport>(transport)),
          log(tmp.path() / "logs"),
          commands(catalog, PayloadEncoder(Endian::Little), pipeline, &log, "fsw"),
          service(makeConfig(tmp, pdusPerSecond), pipeline, commands, &log, "fsw")
    {
        service.start();
    }

    static Config makeConfig(const test::TempDir& tmp, unsigned pdusPerSecond)
    {
        Config c;
        c.downlinkDir = tmp.path() / "downlink";
        c.pdusPerSecond = pdusPerSecond;
        return c;
    }

    std::string notices()
    {
        for (const auto& n : service.takeNotices()) {
            noticeText += n + "\n";
        }
        return noticeText;
    }

    CommandCatalog catalog;
    LockedTransport* transport;   // owned by the pipeline
    Pipeline pipeline;
    PacketLog log;
    CommandService commands;
    FileTransferService service;
    std::string noticeText;
};

} // namespace

TEST(FileTransferService, PutSendsEachPduAsACfCommandPacket)
{
    test::TempDir tmp;
    Rig rig(tmp);
    const std::string content = pattern(2000);
    const fs::path file = writeBinary(tmp.path() / "test.bin", content);
    std::string error;
    const auto id = rig.service.put(file, "/cf/test.bin", 1, error);
    ASSERT_TRUE(id) << error;
    const auto result = rig.service.waitFor(*id);
    ASSERT_TRUE(result.status) << result.error;
    EXPECT_EQ(result.status->state, State::Done);

    const auto sent = rig.transport->sent();
    ASSERT_EQ(sent.size(), 6u);   // MD, 4 x FD (512 B), EOF
    std::string data;
    for (const auto& packet : sent) {
        EXPECT_EQ(packet[0], 0x19);   // CF_INCOMING_PDU_MID 0x192D, a command
        EXPECT_EQ(packet[1], 0x2D);
        EXPECT_EQ(packet[7], 0x00);   // command code 0 (little-endian: byte 7)
        const auto info = inspectPdu(pduOf(packet));
        ASSERT_TRUE(info);
        if (info->kind == PduKind::FileData) {
            data.append(packet.begin() + 8 + static_cast<long>(info->headerLength + 4), packet.end());
        }
    }
    EXPECT_EQ(inspectPdu(pduOf(sent.front()))->kind, PduKind::Metadata);
    EXPECT_EQ(inspectPdu(pduOf(sent.back()))->kind, PduKind::Eof);
    EXPECT_EQ(data, content);

    const std::string notices = rig.notices();
    EXPECT_NE(notices.find("started " + id->toString() + "  ↑ "), std::string::npos) << notices;
    EXPECT_NE(notices.find("✔ put " + id->toString() + " done: /cf/test.bin 2.0 KiB"), std::string::npos) << notices;

    // Audit trail: start/end lines and the directive PDUs with their bytes.
    const std::string log = readFile(rig.log.currentFile());
    EXPECT_NE(log.find("cfdp=start id=" + id->toString() + " dir=up class=1"), std::string::npos) << log;
    EXPECT_NE(log.find("cfdp=end id=" + id->toString() + " result=DONE"), std::string::npos) << log;
    EXPECT_NE(log.find("cmd=\"cfdp MD " + id->toString() + " -> 0.24\""), std::string::npos) << log;
    EXPECT_NE(log.find("cmd=\"cfdp EOF "), std::string::npos) << log;
    EXPECT_EQ(log.find("cmd=\"cfdp FD "), std::string::npos) << log;   // FileData counted, not logged
}

TEST(FileTransferService, TellsTheUiOnlyWhenThereIsSomethingNew)
{
    std::atomic<int> changes{0};   // before the rig: its worker calls onChange until it stops
    test::TempDir tmp;
    Rig rig(tmp);
    rig.service.setOnChange([&changes] { ++changes; });

    // The worker publishes every 20 ms; with nothing to transfer the UI is not woken.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_EQ(changes.load(), 0);

    const fs::path file = writeBinary(tmp.path() / "a.bin", pattern(4000));
    std::string error;
    const auto id = rig.service.put(file, "/cf/a.bin", 1, error);
    ASSERT_TRUE(id) << error;
    ASSERT_TRUE(rig.service.waitFor(*id).status);
    EXPECT_GT(changes.load(), 0);   // progress and the end were shown

    // Finished and unchanged: quiet again.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    changes = 0;
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_EQ(changes.load(), 0);
    rig.service.setOnChange({});
}

TEST(FileTransferService, GetSendsPlaybackAndStoresTheDownlinkedFile)
{
    test::TempDir tmp;
    const std::string content = pattern(10000);
    const auto pdus = cfPlayback(tmp, "/cf/log/hk_0001.dat", content);
    Rig rig(tmp);
    std::ostringstream out;
    const auto ticket = rig.service.get("/cf/log/hk_0001.dat", std::nullopt, 1, out);
    ASSERT_TRUE(ticket) << out.str();
    EXPECT_NE(out.str().find("sent fsw cf playback_file class1 0 0 keep 0.23 /cf/log/hk_0001.dat /cf/log/hk_0001.dat"),
              std::string::npos) << out.str();
    EXPECT_NE(out.str().find("waiting for CF to start /cf/log/hk_0001.dat"), std::string::npos);

    // The playback_file command: MID 0x1923, command code 2.
    const auto sent = rig.transport->sent();
    ASSERT_EQ(sent.size(), 1u);
    EXPECT_EQ(sent[0][0], 0x19);
    EXPECT_EQ(sent[0][1], 0x23);
    EXPECT_EQ(sent[0][7], 2);

    // Until CF starts, the get is listed as waiting.
    std::this_thread::sleep_for(50ms);
    ASSERT_FALSE(rig.service.transfers().empty());
    EXPECT_EQ(rig.service.transfers().front().state, State::Waiting);
    EXPECT_EQ(rig.service.activeCounts().waiting, 1u);

    for (const auto& datagram : asTelemetry(pdus)) {
        rig.service.onDatagram(datagram);
    }
    rig.service.onDatagram(test::tlmPacket(0x0899, 1, {1, 2, 3}));   // other telemetry is ignored
    const auto result = rig.service.waitForGet(*ticket);
    ASSERT_TRUE(result.status) << result.error;
    EXPECT_EQ(result.status->state, State::Done) << result.status->condition;
    EXPECT_TRUE(result.status->requested);
    EXPECT_EQ(result.status->id.source, "0.24");
    const fs::path stored = tmp.path() / "downlink" / "cf" / "log" / "hk_0001.dat";
    EXPECT_EQ(fs::path(result.status->local), stored);
    EXPECT_EQ(readFile(stored), content);
    const std::string notices = rig.notices();
    EXPECT_NE(notices.find("↓ /cf/log/hk_0001.dat (9.8 KiB) → " + stored.string()), std::string::npos) << notices;
    EXPECT_NE(notices.find("✔ get " + result.status->id.toString() + " done"), std::string::npos) << notices;
    EXPECT_EQ(rig.service.activeCounts().total(), 0u);
}

TEST(FileTransferService, GetToALocalPathMovesTheFileThere)
{
    test::TempDir tmp;
    const auto pdus = cfPlayback(tmp, "/cf/a.bin", pattern(3000));
    Rig rig(tmp);
    std::ostringstream out;
    const fs::path mine = tmp.path() / "mine" / "a-copy.bin";
    const auto ticket = rig.service.get("/cf/a.bin", mine, 1, out);
    ASSERT_TRUE(ticket) << out.str();
    for (const auto& datagram : asTelemetry(pdus)) {
        rig.service.onDatagram(datagram);
    }
    const auto result = rig.service.waitForGet(*ticket);
    ASSERT_TRUE(result.status) << result.error;
    EXPECT_EQ(fs::path(result.status->local), mine);
    EXPECT_EQ(readFile(mine), pattern(3000));
    EXPECT_FALSE(fs::exists(tmp.path() / "downlink" / "cf" / "a.bin"));
}

TEST(FileTransferService, UnrequestedDownlinksAreSavedToo)
{
    test::TempDir tmp;
    const auto pdus = cfPlayback(tmp, "/cf/ch0poll0/x.dat", pattern(500));
    Rig rig(tmp);
    for (const auto& datagram : asTelemetry(pdus)) {
        rig.service.onDatagram(datagram);
    }
    for (int i = 0; i < 100 && rig.service.transfers().empty(); ++i) {
        std::this_thread::sleep_for(10ms);
    }
    ASSERT_FALSE(rig.service.transfers().empty());
    const auto result = rig.service.waitFor(rig.service.transfers().front().id);
    ASSERT_TRUE(result.status) << result.error;
    EXPECT_EQ(result.status->state, State::Done);
    EXPECT_FALSE(result.status->requested);
    EXPECT_EQ(readFile(tmp.path() / "downlink" / "cf" / "ch0poll0" / "x.dat"), pattern(500));
    EXPECT_NE(rig.notices().find("(not requested by 'file get')"), std::string::npos);
}

TEST(FileTransferService, GetThatCfNeverStartsFailsWithAHint)
{
    test::TempDir tmp;
    Rig rig(tmp);
    rig.service.setStartTimeout(1s);
    std::ostringstream out;
    const auto ticket = rig.service.get("/cf/missing.bin", std::nullopt, 1, out);
    ASSERT_TRUE(ticket) << out.str();
    const auto result = rig.service.waitForGet(*ticket);
    ASSERT_TRUE(result.status) << result.error;
    EXPECT_EQ(result.status->state, State::Failed);
    EXPECT_EQ(result.status->condition, "NOT_STARTED");
    EXPECT_NE(result.status->note.find("no telemetry received"), std::string::npos) << result.status->note;
    EXPECT_NE(rig.notices().find("✘ get /cf/missing.bin failed"), std::string::npos);
    EXPECT_EQ(rig.service.transfers().front().state, State::Failed);
}

TEST(FileTransferService, RefusesWhatCannotWork)
{
    test::TempDir tmp;
    {
        Rig rig(tmp, /*dryRun=*/true);
        std::ostringstream out;
        EXPECT_FALSE(rig.service.get("/cf/a.bin", std::nullopt, 1, out));
        EXPECT_NE(out.str().find("needs a live target"), std::string::npos) << out.str();
        std::string error;
        const fs::path file = writeBinary(tmp.path() / "a.bin", "data");
        EXPECT_FALSE(rig.service.put(file, "/cf/a.bin", 2, error));
        EXPECT_NE(error.find("class 2 needs a live target"), std::string::npos) << error;
        EXPECT_TRUE(rig.transport->sent().empty());
    }
    {
        Rig rig(tmp);
        rig.service.setReceiving(false, "port 5011 in use");
        std::ostringstream out;
        EXPECT_FALSE(rig.service.get("/cf/a.bin", std::nullopt, 1, out));
        EXPECT_NE(out.str().find("port 5011 in use"), std::string::npos) << out.str();
        EXPECT_FALSE(rig.service.get("cf/a.bin", std::nullopt, 1, out));
        std::string error;
        const fs::path file = tmp.path() / "a.bin";
        EXPECT_FALSE(rig.service.put(file, "/cf/a.bin", 2, error));
        EXPECT_NE(error.find("port 5011 in use"), std::string::npos) << error;
        EXPECT_TRUE(rig.transport->sent().empty());
        // Class 1 needs nothing back, so it still works.
        const auto id = rig.service.put(file, "/cf/a.bin", 1, error);
        ASSERT_TRUE(id) << error;
        EXPECT_EQ(rig.service.waitFor(*id).status->state, State::Done);
    }
}

TEST(FileTransferService, DryRunPutShowsTheDirectivePdus)
{
    test::TempDir tmp;
    Rig rig(tmp, /*dryRun=*/true);
    const fs::path file = writeBinary(tmp.path() / "a.bin", pattern(1000));
    std::string error;
    const auto id = rig.service.put(file, "/cf/a.bin", 1, error);
    ASSERT_TRUE(id) << error;
    EXPECT_EQ(rig.service.waitFor(*id).status->state, State::Done);
    const std::string notices = rig.notices();
    EXPECT_NE(notices.find("[dry-run] cfdp MD " + id->toString()), std::string::npos) << notices;
    EXPECT_NE(notices.find("[dry-run] cfdp EOF " + id->toString()), std::string::npos) << notices;
}

TEST(FileTransferService, OneTransferPerLocalFile)
{
    test::TempDir tmp;
    Rig rig(tmp, false, /*pdusPerSecond=*/50);   // 400 PDUs: still running for the second put
    const fs::path file = writeBinary(tmp.path() / "big.bin", pattern(512 * 400));
    std::string error;
    const auto id = rig.service.put(file, "/cf/big.bin", 1, error);
    ASSERT_TRUE(id) << error;
    std::this_thread::sleep_for(50ms);
    ASSERT_EQ(rig.service.activeCounts().up, 1u);
    EXPECT_FALSE(rig.service.put(file, "/cf/big2.bin", 1, error));
    EXPECT_NE(error.find("already being transferred"), std::string::npos) << error;
    ASSERT_TRUE(rig.service.control(RequestKind::Abandon, id->toString(), error)) << error;
    EXPECT_EQ(rig.service.waitFor(*id).status->state, State::Abandoned);
}

TEST(FileTransferService, CancelAllForgetsWaitingGets)
{
    test::TempDir tmp;
    Rig rig(tmp);
    std::ostringstream out;
    const auto ticket = rig.service.get("/cf/a.bin", std::nullopt, 1, out);
    ASSERT_TRUE(ticket) << out.str();
    std::string error;
    EXPECT_TRUE(rig.service.control(RequestKind::Cancel, "all", error)) << error;
    EXPECT_EQ(rig.service.waitForGet(*ticket).error, "the request was cancelled");
    EXPECT_FALSE(rig.service.control(RequestKind::Cancel, "0.24_99", error));
    EXPECT_NE(error.find("no active transfer"), std::string::npos) << error;
}

TEST(FileTransferService, IsSafeToUseFromManyThreads)
{
    test::TempDir tmp;
    Rig rig(tmp);
    std::atomic<bool> stop{false};
    std::vector<std::thread> readers;
    for (int i = 0; i < 3; ++i) {
        readers.emplace_back([&] {
            while (!stop) {
                (void)rig.service.transfers();
                (void)rig.service.activeCounts();
                (void)rig.service.takeNotices();
                rig.service.onDatagram(test::tlmPacket(0x0899, 1, {1}));
            }
        });
    }
    std::vector<TransactionId> ids;
    for (int i = 0; i < 5; ++i) {
        const fs::path file = writeBinary(tmp.path() / ("f" + std::to_string(i) + ".bin"), pattern(3000));
        std::string error;
        const auto id = rig.service.put(file, "/cf/f" + std::to_string(i) + ".bin", 1, error);
        ASSERT_TRUE(id) << error;
        ids.push_back(*id);
    }
    for (const auto& id : ids) {
        EXPECT_EQ(rig.service.waitFor(id).status->state, State::Done);
    }
    stop = true;
    for (auto& t : readers) {
        t.join();
    }
}
