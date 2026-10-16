#include "ui/transfer_format.hpp"

#include <gtest/gtest.h>

using namespace mcs;
using namespace mcs::cfdp;
using namespace mcs::ui;

namespace {

std::size_t columns(const std::string& text)
{
    std::size_t n = 0;
    for (const char c : text) {
        n += (static_cast<unsigned char>(c) & 0xC0) != 0x80 ? 1 : 0;
    }
    return n;
}

TransferStatus putStatus()
{
    TransferStatus s;
    s.id = {"0.23", 1729061234};
    s.direction = Direction::Uplink;
    s.cls = 2;
    s.local = "test.bin";
    s.remote = "/cf/test.bin";
    s.fileSize = 1048576;
    s.sizeKnown = true;
    s.bytesDone = 797576;
    s.rateBps = 48200;
    s.etaSeconds = 5.2;
    s.state = State::Active;
    return s;
}

TransferStatus getStatus()
{
    TransferStatus s;
    s.id = {"0.24", 12};
    s.direction = Direction::Downlink;
    s.cls = 1;
    s.requested = true;
    s.remote = "/cf/log/hk_0001.dat";
    s.local = "/home/op/mcs/downlink/cf/log/hk_0001.dat";
    s.fileSize = 839680;
    s.sizeKnown = true;
    s.bytesDone = 176333;
    s.rateBps = 15900;
    s.etaSeconds = 41;
    s.state = State::Active;
    return s;
}

} // namespace

TEST(TransferFormat, Badge)
{
    EXPECT_EQ(transferBadge({}), "");
    EXPECT_EQ(transferBadge({1, 1, 0}), "FILES ↑1 ↓1");
    EXPECT_EQ(transferBadge({0, 1, 1}), "FILES ↓1 · 1 waiting");
    EXPECT_EQ(transferBadge({0, 0, 2}), "FILES 2 waiting");
}

TEST(TransferFormat, ShortenMiddleKeepsBothEnds)
{
    EXPECT_EQ(shortenMiddle("/cf/a.bin", 20), "/cf/a.bin");
    const std::string s = shortenMiddle("/home/op/mcs/downlink/cf/log/hk_0001.dat", 24);
    EXPECT_EQ(columns(s), 24u);
    EXPECT_EQ(s.rfind("hk_0001.dat"), s.size() - 11);   // the file name survives
    EXPECT_EQ(s.rfind("/home", 0), 0u);
    EXPECT_NE(s.find("…"), std::string::npos);
}

TEST(TransferFormat, ProgressBar)
{
    EXPECT_EQ(progressBar(0, 4), "░░░░");
    EXPECT_EQ(progressBar(50, 4), "██░░");
    EXPECT_EQ(progressBar(100, 4), "████");
    EXPECT_EQ(progressBar(150, 4), "████");
    EXPECT_EQ(columns(progressBar(76, 20)), 20u);
}

TEST(TransferFormat, StripRowFitsTheWidth)
{
    for (const int width : {80, 120, 200}) {
        for (const auto& s : {putStatus(), getStatus()}) {
            const bool compact = width < 120;
            const StripRow row = formatStripRow(s, width, compact);
            EXPECT_LE(columns(toText(row)), static_cast<std::size_t>(width)) << width << ": " << toText(row);
            EXPECT_EQ(row.id.empty(), compact);
            EXPECT_EQ(row.eta.empty(), compact);
            EXPECT_EQ(columns(row.bar), 20u);
        }
    }
    const StripRow up = formatStripRow(putStatus(), 160, false);
    EXPECT_EQ(up.arrow, "↑");
    EXPECT_EQ(up.kind, "put");
    EXPECT_EQ(up.id, "0.23_1729061234");
    EXPECT_EQ(up.files, "test.bin → /cf/test.bin");
    EXPECT_EQ(up.cls, "c2");
    EXPECT_EQ(up.percent, " 76%");
    EXPECT_EQ(up.rate, "48.2 KB/s");
    EXPECT_EQ(up.eta, "5.2 s");

    const StripRow down = formatStripRow(getStatus(), 160, false);
    EXPECT_EQ(down.arrow, "↓");
    EXPECT_EQ(down.kind, "get");
    EXPECT_EQ(down.files.rfind("/cf/log/hk_0001.dat → ", 0), 0u) << down.files;
}

TEST(TransferFormat, WaitingAndUnrequestedRows)
{
    TransferStatus waiting;
    waiting.direction = Direction::Downlink;
    waiting.state = State::Waiting;
    waiting.remote = "/cf/img/cam_07.raw";
    waiting.local = "downlink/cf/img/cam_07.raw";
    waiting.note = "waiting for CF to start · 8 s";
    const StripRow w = formatStripRow(waiting, 120, false);
    EXPECT_TRUE(w.waiting);
    EXPECT_TRUE(w.bar.empty());
    EXPECT_EQ(w.note, "waiting for CF to start · 8 s");

    TransferStatus in = getStatus();
    in.requested = false;
    EXPECT_EQ(formatStripRow(in, 120, false).kind, "in");

    TransferStatus unknown = getStatus();
    unknown.sizeKnown = false;
    EXPECT_EQ(formatStripRow(unknown, 120, false).percent, "  ?%");
}

TEST(TransferFormat, ListAndStatus)
{
    EXPECT_EQ(listLines({}).front().rfind("no transfers yet", 0), 0u);

    TransferStatus done = getStatus();
    done.state = State::Done;
    done.bytesDone = done.fileSize;
    done.end = done.start + std::chrono::milliseconds(52400);
    TransferStatus failed = putStatus();
    failed.state = State::Failed;
    failed.condition = "NAK_LIMIT_REACHED";
    const auto lines = listLines({putStatus(), done, failed});
    ASSERT_EQ(lines.size(), 5u);
    EXPECT_EQ(lines[0], "ACTIVE");
    EXPECT_NE(lines[1].find("0.23_1729061234"), std::string::npos);
    EXPECT_NE(lines[1].find(" 76%"), std::string::npos);
    EXPECT_EQ(lines[2], "RECENT");
    EXPECT_EQ(lines[3].rfind("  ✔ 0.24_12", 0), 0u) << lines[3];
    EXPECT_NE(lines[3].find("52.4 s"), std::string::npos) << lines[3];
    EXPECT_NE(lines[4].find("FAILED NAK_LIMIT_REACHED"), std::string::npos) << lines[4];

    const auto status = statusLines(failed);
    EXPECT_EQ(status.front(), "0.23_1729061234  put · class 2 · FAILED  NAK_LIMIT_REACHED");
    EXPECT_NE(status[3].find("797576 / 1048576 (76%)"), std::string::npos) << status[3];
}
