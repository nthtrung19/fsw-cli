// Oracle test: packets built by mcs must be byte-identical to packets built
// on this (little-endian) host with cFE 6.7's own ccsds.h macros and
// CCSDS_LoadCheckSum() from ccsds.c, exactly as the FSW lays them out in memory.
//
// The mcs side uses the real shipped catalog (config/catalog/ds.json), so
// this also checks that the catalog matches the FSW structures.

#include "catalog/catalog.hpp"
#include "config/catalog_loader.hpp"
#include "encode/payload_encoder.hpp"
#include "protocol/ccsds_v1_format.hpp"

extern "C" {
#include "ccsds.h"
}

#include <gtest/gtest.h>

#include <cstring>

using namespace mcs;

namespace {

// Mirror of the FSW command struct (ds_msg.h), filled natively by the host.
struct DS_AppStateCmd_t {
    uint8 CmdHeader[8];
    uint16 EnableState;
    uint16 Padding;
};
static_assert(sizeof(DS_AppStateCmd_t) == 12, "unexpected padding");

struct DS_FilterParmsCmd_t {
    uint8 CmdHeader[8];
    uint16 MessageID;
    uint16 FilterParmsIndex;
    uint16 Algorithm_N;
    uint16 Algorithm_X;
    uint16 Algorithm_O;
    uint16 Padding;
};
static_assert(sizeof(DS_FilterParmsCmd_t) == 20, "unexpected padding");

struct DS_DestTypeCmd_t {
    uint8 CmdHeader[8];
    uint16 FileTableIndex;
    uint16 FileNameType;
};
static_assert(sizeof(DS_DestTypeCmd_t) == 12, "unexpected padding");

struct DS_DestPathCmd_t {
    uint8 CmdHeader[8];
    uint16 FileTableIndex;
    uint16 Padding;
    char Pathname[64];   // DS_PATHNAME_BUFSIZE = OS_MAX_PATH_LEN
};
static_assert(sizeof(DS_DestPathCmd_t) == 76, "unexpected padding");

struct DS_DestSizeCmd_t {
    uint8 CmdHeader[8];
    uint16 FileTableIndex;
    uint16 Padding;
    uint32 MaxFileSize;
};
static_assert(sizeof(DS_DestSizeCmd_t) == 16, "unexpected padding");

// cFE's macros (unmodified NASA code) do implicit narrowing; silence those
// warnings for the code that expands them, and only there.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"

// Builds a command packet the way cFE does: macros on a real header struct,
// payload written natively, checksum from ccsds.c.
Bytes cfeBuild(std::uint16_t mid, std::uint8_t fc, std::uint16_t seq, const void* cmd,
               std::size_t totalSize)
{
    alignas(8) std::uint8_t buffer[128] = {};
    std::memcpy(buffer, cmd, totalSize);   // payload (header bytes are overwritten below)
    auto* pkt = reinterpret_cast<CCSDS_CommandPacket_t*>(buffer);

    CCSDS_CLR_PRI_HDR(pkt->SpacePacket.Hdr);
    CCSDS_WR_SID(pkt->SpacePacket.Hdr, mid);
    CCSDS_WR_SEQ(pkt->SpacePacket.Hdr, seq);
    CCSDS_WR_LEN(pkt->SpacePacket.Hdr, totalSize);
    CCSDS_CLR_CMDSEC_HDR(pkt->Sec);
    CCSDS_WR_FC(pkt->Sec, fc);
    CCSDS_LoadCheckSum(pkt);

    EXPECT_TRUE(CCSDS_ValidCheckSum(pkt));
    return Bytes(buffer, buffer + totalSize);
}

#pragma GCC diagnostic pop

class Oracle : public ::testing::Test {
protected:
    void SetUp() override
    {
        const std::uint16_t probe = 1;
        std::uint8_t first = 0;
        std::memcpy(&first, &probe, 1);
        if (first != 1) {
            GTEST_SKIP() << "oracle needs a little-endian host (the target is little-endian)";
        }
        catalog_ = loadCatalog({std::string(mcs_SOURCE_DIR) + "/config/catalog/ds.json"});
    }

    // Full mcs path: catalog -> encoder -> CCSDS v1 format.
    Bytes ours(const std::string& cmd, const std::vector<std::string>& args, int repeat = 1)
    {
        const CommandDef* def = catalog_.findCommand("ds", cmd);
        EXPECT_NE(def, nullptr);
        BuiltPacket built;
        for (int i = 0; i < repeat; ++i) {
            built = format_.build({def->mid, def->cc, encoder_.encode(*def, args)});
        }
        return built.bytes;
    }

    CommandCatalog catalog_;
    PayloadEncoder encoder_{Endian::Little};
    CcsdsV1Format format_{Endian::Little};
};

TEST_F(Oracle, NoopMatchesCfe)
{
    std::uint8_t header[8] = {};
    EXPECT_EQ(ours("noop", {}), cfeBuild(0x194B, 0, 0, header, sizeof header));
}

TEST_F(Oracle, ResetMatchesCfe)
{
    std::uint8_t header[8] = {};
    EXPECT_EQ(ours("reset", {}), cfeBuild(0x194B, 1, 0, header, sizeof header));
}

TEST_F(Oracle, SetAppStateEnableMatchesCfe)
{
    DS_AppStateCmd_t cmd{};
    cmd.EnableState = 1;
    EXPECT_EQ(ours("set_app_state", {"enable"}), cfeBuild(0x194B, 2, 0, &cmd, sizeof cmd));
}

TEST_F(Oracle, SetAppStateDisableMatchesCfe)
{
    DS_AppStateCmd_t cmd{};
    cmd.EnableState = 0;
    EXPECT_EQ(ours("set_app_state", {"disable"}), cfeBuild(0x194B, 2, 0, &cmd, sizeof cmd));
}

TEST_F(Oracle, DsHkMatchesCfe)
{
    // DS_SEND_HK_MID: header only; DS checks the length, not the command code.
    std::uint8_t header[8] = {};
    EXPECT_EQ(ours("hk", {}), cfeBuild(0x194C, 0, 0, header, sizeof header));
}

TEST_F(Oracle, SequenceCountMatchesCfe)
{
    std::uint8_t header[8] = {};
    // Fifth packet on the APID carries sequence count 4.
    EXPECT_EQ(ours("noop", {}, 5), cfeBuild(0x194B, 0, 4, header, sizeof header));
}

TEST_F(Oracle, FswReadsBackOurFields)
{
    // Parse our packet with the FSW's read macros, as SB/DS would.
    Bytes packet = ours("set_app_state", {"enable"});
    auto* pkt = reinterpret_cast<CCSDS_CommandPacket_t*>(packet.data());
    EXPECT_EQ(CCSDS_RD_SID(pkt->SpacePacket.Hdr), 0x194B);
    EXPECT_EQ(CCSDS_RD_TYPE(pkt->SpacePacket.Hdr), CCSDS_CMD);
    EXPECT_EQ(CCSDS_RD_SHDR(pkt->SpacePacket.Hdr), CCSDS_HAS_SEC_HDR);
    EXPECT_EQ(CCSDS_RD_SEQFLG(pkt->SpacePacket.Hdr), 3);
    EXPECT_EQ(CCSDS_RD_LEN(pkt->SpacePacket.Hdr), 12);
    EXPECT_EQ(CCSDS_RD_FC(pkt->Sec), 2);
    EXPECT_TRUE(CCSDS_ValidCheckSum(pkt));

    DS_AppStateCmd_t cmd{};
    std::memcpy(&cmd, packet.data(), sizeof cmd);
    EXPECT_EQ(cmd.EnableState, 1);
    EXPECT_EQ(cmd.Padding, 0);
}

TEST_F(Oracle, SetFilterParmsMatchesCfe)
{
    DS_FilterParmsCmd_t cmd{};
    cmd.MessageID = 0x0801;
    cmd.FilterParmsIndex = 2;
    cmd.Algorithm_N = 1;
    cmd.Algorithm_X = 4;
    cmd.Algorithm_O = 3;
    EXPECT_EQ(ours("set_filter_parms", {"0x0801", "2", "1", "4", "3"}),
              cfeBuild(0x194B, 5, 0, &cmd, sizeof cmd));
}

TEST_F(Oracle, SetDestTypeMatchesCfe)
{
    DS_DestTypeCmd_t cmd{};
    cmd.FileTableIndex = 15;
    cmd.FileNameType = 2;   // DS_BY_TIME
    EXPECT_EQ(ours("set_dest_type", {"15", "time"}), cfeBuild(0x194B, 6, 0, &cmd, sizeof cmd));
}

TEST_F(Oracle, SetDestPathMatchesCfe)
{
    DS_DestPathCmd_t cmd{};
    cmd.FileTableIndex = 3;
    std::strcpy(cmd.Pathname, "/ram/ds/");
    EXPECT_EQ(ours("set_dest_path", {"3", "/ram/ds/"}), cfeBuild(0x194B, 8, 0, &cmd, sizeof cmd));
}

TEST_F(Oracle, SetDestSizeMatchesCfe)
{
    DS_DestSizeCmd_t cmd{};
    cmd.FileTableIndex = 7;
    cmd.MaxFileSize = 0x00123456;
    EXPECT_EQ(ours("set_dest_size", {"7", "0x123456"}), cfeBuild(0x194B, 11, 0, &cmd, sizeof cmd));
}

} // namespace
