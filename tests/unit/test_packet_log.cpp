#include "log/packet_log.hpp"
#include "unit/test_helpers.hpp"

#include <gtest/gtest.h>

#include <fstream>
#include <regex>
#include <sstream>

using namespace mcs;

namespace {

std::string readAll(const std::filesystem::path& file)
{
    std::ifstream in(file);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace

TEST(PacketLog, TimestampFormat)
{
    const std::regex iso(R"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}[+-]\d{2}:\d{2})");
    EXPECT_TRUE(std::regex_match(PacketLog::timestamp(), iso)) << PacketLog::timestamp();
}

TEST(PacketLog, AppendsOneLinePerPacket)
{
    test::TempDir dir;
    PacketLog log(dir.path() / "logs");   // directory is created on demand
    EXPECT_FALSE(log.record({"sil", "fsw ds noop", "apid=0x14B seq=0", {0x19, 0x4B}, false}));
    EXPECT_FALSE(log.record({"sil", "fsw ds say \"hi\"", "apid=0x14B seq=1", {0xAA}, true}));

    const std::string file = log.currentFile().filename().string();
    EXPECT_TRUE(std::regex_match(file, std::regex(R"(mcs-\d{8}\.log)"))) << file;

    const std::string text = readAll(log.currentFile());
    EXPECT_NE(text.find(" target=sil cmd=\"fsw ds noop\" apid=0x14B seq=0 bytes=2 wire=19 4B\n"),
              std::string::npos) << text;
    EXPECT_NE(text.find("cmd=\"fsw ds say \\\"hi\\\"\" apid=0x14B seq=1 bytes=1 dryrun=true wire=AA\n"),
              std::string::npos) << text;
}

TEST(PacketLog, WarnsOnceWhenUnwritable)
{
    test::TempDir dir;
    const auto blocker = dir.write("not-a-dir", "x");   // a file where the directory should be
    PacketLog log(blocker);
    const auto first = log.record({"t", "c", "n", {1}, false});
    ASSERT_TRUE(first.has_value());
    EXPECT_NE(first->find("cannot write packet log"), std::string::npos);
    EXPECT_FALSE(log.record({"t", "c", "n", {1}, false}).has_value());   // only once
}
