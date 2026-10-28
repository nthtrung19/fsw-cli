#include "catalog/catalog.hpp"
#include "core/hex.hpp"
#include "protocol/ccsds_v1_format.hpp"
#include "service/command_service.hpp"
#include "unit/test_helpers.hpp"

#include <gtest/gtest.h>

#include <sstream>

using namespace mcs;

namespace {

class ServiceTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        AppDef ds = test::dsApp();
        CommandDef critical{"wipe", 9, "Dangerous", {}, true, 0x194B};
        ds.commands.push_back(critical);
        catalog_.add(ds);
        makeService(/*dryRun=*/false, /*withLog=*/false);
    }

    void makeService(bool dryRun, bool withLog)
    {
        sent_.clear();
        auto transport = std::make_unique<test::FakeTransport>(&sent_, dryRun);
        transport_ = transport.get();
        pipeline_ = std::make_unique<Pipeline>(std::make_unique<CcsdsV1Format>(Endian::Little),
                                               std::vector<std::unique_ptr<IFramingLayer>>{},
                                               std::move(transport));
        if (withLog) {
            log_ = std::make_unique<PacketLog>(dir_.path());
        }
        service_ = std::make_unique<CommandService>(catalog_, PayloadEncoder(Endian::Little),
                                                    *pipeline_, log_.get(), "fsw");
    }

    bool run(const std::string& cmd, const std::vector<std::string>& args = {}, bool confirmed = false)
    {
        out_.str("");
        return service_->execute("ds", cmd, args, out_, confirmed);
    }

    test::TempDir dir_;
    CommandCatalog catalog_;
    std::vector<Bytes> sent_;
    test::FakeTransport* transport_ = nullptr;
    std::unique_ptr<Pipeline> pipeline_;
    std::unique_ptr<PacketLog> log_;
    std::unique_ptr<CommandService> service_;
    std::ostringstream out_;
};

} // namespace

TEST_F(ServiceTest, SendsAndReports)
{
    EXPECT_TRUE(run("set_app_state", {"enable"}));
    ASSERT_EQ(sent_.size(), 1U);
    EXPECT_EQ(toHex(sent_[0]), "19 4B C0 00 00 05 6B 02 01 00 00 00");
    EXPECT_EQ(out_.str(),
              "sent fsw ds set_app_state enable -> fake://  (12 bytes, apid=0x14B seq=0)\n");
    EXPECT_EQ(service_->packetsSent(), 1U);
    EXPECT_EQ(service_->failures(), 0U);
}

TEST_F(ServiceTest, EachCommandIsSentOnItsOwnMid)
{
    AppDef hk;
    hk.name = "hk";
    hk.commands.push_back({"noop", 0, "", {}, false, 0x1934});
    hk.commands.push_back({"send", 0, "", {}, false, 0x1935});
    catalog_.add(hk);
    EXPECT_TRUE(service_->execute("hk", "noop", {}, out_));
    EXPECT_TRUE(service_->execute("hk", "send", {}, out_));
    ASSERT_EQ(sent_.size(), 2U);
    EXPECT_EQ(toHex(sent_[0]).substr(0, 5), "19 34");
    EXPECT_EQ(toHex(sent_[1]).substr(0, 5), "19 35");
}

TEST_F(ServiceTest, VerboseAddsHexdump)
{
    service_->setVerbose(true);
    EXPECT_TRUE(run("noop"));
    EXPECT_EQ(out_.str(), "sent fsw ds noop -> fake://  (8 bytes, apid=0x14B seq=0)\n"
                          "  19 4B C0 00 00 01 6C 00\n");
}

TEST_F(ServiceTest, DryRunPrintsPacket)
{
    makeService(/*dryRun=*/true, false);
    EXPECT_TRUE(run("noop"));
    EXPECT_EQ(out_.str(), "[dry-run] fsw ds noop  (8 bytes, apid=0x14B seq=0)\n"
                          "  19 4B C0 00 00 01 6C 00\n");
}

TEST_F(ServiceTest, InvalidArgumentsSendNothing)
{
    EXPECT_FALSE(run("set_app_state", {"maybe"}));
    EXPECT_FALSE(run("set_app_state"));
    EXPECT_TRUE(sent_.empty());
    EXPECT_EQ(service_->failures(), 2U);
    EXPECT_EQ(out_.str().rfind("error: set_app_state: expected 1 argument, got 0", 0), 0U) << out_.str();
}

TEST_F(ServiceTest, UnknownCommand)
{
    out_.str("");
    EXPECT_FALSE(service_->execute("fm", "noop", {}, out_));
    EXPECT_EQ(out_.str(), "error: unknown command 'fm noop' for target 'fsw'\n");
}

TEST_F(ServiceTest, TransportErrorIsReportedNotThrown)
{
    transport_->failNext = true;
    EXPECT_FALSE(run("noop"));
    EXPECT_EQ(out_.str(), "error: fake: link down\n");
    EXPECT_TRUE(run("noop"));   // the session continues
}

TEST_F(ServiceTest, CriticalCommandsNeedConfirmation)
{
    EXPECT_FALSE(run("wipe"));
    EXPECT_EQ(out_.str(), "error: 'wipe' is a critical command: type '!' before it\n");
    EXPECT_TRUE(sent_.empty());

    EXPECT_TRUE(run("wipe", {}, /*confirmed=*/true));
    EXPECT_EQ(sent_.size(), 1U);
    EXPECT_FALSE(run("wipe"));   // the confirmation covered that one command
    EXPECT_EQ(sent_.size(), 1U);
}

TEST_F(ServiceTest, ConfirmedNonCriticalCommandIsSent)
{
    EXPECT_TRUE(run("noop", {}, /*confirmed=*/true));
    EXPECT_EQ(sent_.size(), 1U);
}

TEST_F(ServiceTest, ConfirmedCommandIsStillValidated)
{
    EXPECT_FALSE(run("wipe", {"unexpected"}, /*confirmed=*/true));
    EXPECT_TRUE(sent_.empty());
}

TEST_F(ServiceTest, RawBytes)
{
    out_.str("");
    EXPECT_TRUE(service_->executeRaw({"19", "4B", "C0", "00", "00", "01", "6C", "00"}, out_));
    EXPECT_EQ(toHex(sent_.at(0)), "19 4B C0 00 00 01 6C 00");
    EXPECT_EQ(out_.str(), "sent raw 19 4B C0 00 00 01 6C 00 -> fake://  (8 bytes, raw)\n");

    out_.str("");
    EXPECT_FALSE(service_->executeRaw({"zz"}, out_));
    EXPECT_EQ(out_.str().rfind("error: raw: invalid hex digit", 0), 0U) << out_.str();
}

TEST_F(ServiceTest, WritesPacketLog)
{
    makeService(false, /*withLog=*/true);
    EXPECT_TRUE(run("set_app_state", {"disable"}));
    std::ifstream in(log_->currentFile());
    std::string line;
    std::getline(in, line);
    EXPECT_NE(line.find("target=fsw cmd=\"fsw ds set_app_state disable\" apid=0x14B seq=0 bytes=12"),
              std::string::npos) << line;
}

TEST_F(ServiceTest, ArgumentsWithSpacesAreQuotedInReports)
{
    AppDef app;
    app.name = "fm";
    app.commands.push_back({"delete", 4, "", {FieldDef::string("path", 32)}, false, 0x188C});
    catalog_.add(app);
    out_.str("");
    EXPECT_TRUE(service_->execute("fm", "delete", {"/cf/my file"}, out_));
    EXPECT_NE(out_.str().find("sent fsw fm delete \"/cf/my file\""), std::string::npos) << out_.str();
}

TEST_F(ServiceTest, RemembersWhenTheLastPacketWasSent)
{
    EXPECT_FALSE(service_->lastSentAt());
    const auto before = std::chrono::system_clock::now();
    ASSERT_TRUE(run("noop"));
    ASSERT_TRUE(service_->lastSentAt());
    EXPECT_GE(*service_->lastSentAt(), before);
    const auto first = *service_->lastSentAt();
    EXPECT_FALSE(run("set_app_state", {"maybe"}));   // nothing sent: unchanged
    EXPECT_EQ(*service_->lastSentAt(), first);
}

TEST_F(ServiceTest, CheckValidatesWithoutSending)
{
    std::ostringstream out;
    EXPECT_TRUE(service_->check("ds", "set_app_state", {"enable"}, true, out));
    EXPECT_FALSE(service_->check("ds", "set_app_state", {"maybe"}, true, out));
    EXPECT_FALSE(service_->check("ds", "nope", {}, true, out));
    EXPECT_FALSE(service_->check("ds", "set_app_state", {}, true, out));
    // Values from variables are not known yet: only the command and the count are checked.
    EXPECT_TRUE(service_->check("ds", "set_app_state", {"maybe"}, false, out));
    EXPECT_FALSE(service_->check("ds", "set_app_state", {"1", "2"}, false, out));
    EXPECT_FALSE(service_->check("ds", "nope", {}, false, out));
    EXPECT_TRUE(service_->checkRaw({"19", "4B"}, true, out));
    EXPECT_FALSE(service_->checkRaw({"zz"}, true, out));
    EXPECT_TRUE(service_->checkRaw({"$x"}, false, out));

    EXPECT_TRUE(sent_.empty());
    EXPECT_EQ(service_->packetsSent(), 0U);
    EXPECT_FALSE(service_->lastSentAt());
    EXPECT_EQ(service_->failures(), 6U);
    EXPECT_NE(out.str().find("error: set_app_state: invalid value 'maybe'"), std::string::npos);
    EXPECT_NE(out.str().find("expected 1 argument, got 2"), std::string::npos);
}

TEST_F(ServiceTest, CheckAppliesTheCriticalRule)
{
    std::ostringstream out;
    EXPECT_FALSE(service_->check("ds", "wipe", {}, true, out));
    EXPECT_NE(out.str().find("type '!' before it"), std::string::npos);
    EXPECT_TRUE(service_->check("ds", "wipe", {}, true, out, /*confirmed=*/true));
    EXPECT_TRUE(service_->check("ds", "wipe", {}, false, out, /*confirmed=*/true));
    EXPECT_TRUE(sent_.empty());
}
