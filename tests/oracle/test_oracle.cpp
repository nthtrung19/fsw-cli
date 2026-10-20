// Oracle test: packets built by mcs must be byte-identical to packets built
// on this (little-endian) host with cFE 6.7's own ccsds.h macros and
// CCSDS_LoadCheckSum() from ccsds.c, exactly as the FSW lays them out in memory.
//
// The mcs side uses the real shipped catalog (config/telecommand/ds.json), so
// this also checks that the catalog matches the FSW structures.

#include "catalog/catalog.hpp"
#include "config/catalog_loader.hpp"
#include "encode/payload_encoder.hpp"
#include "protocol/ccsds_v1_format.hpp"

extern "C" {
#include "ccsds.h"
}

#include <gtest/gtest.h>

#include <cstddef>
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

// CI_EnableTOCmd_t: layout of TO_EnableOutputCmd_t, ENABLE_CRYPTO_LIB off.
struct CI_EnableTOCmd_t {
    uint8 CmdHeader[8];
    char cDestIp[16];   // TO_MAX_IP_STRING_SIZE
    uint16 usDestPort;
};
static_assert(sizeof(CI_EnableTOCmd_t) == 26, "unexpected padding");

// EVS command structs, copied from cfe_evs_msg.h (CFE_MISSION_MAX_API_LEN 20).
struct CFE_EVS_BitMaskCmd_t {
    uint8 CmdHeader[8];
    uint8 BitMask;
    uint8 Spare;
};
static_assert(sizeof(CFE_EVS_BitMaskCmd_t) == 10, "unexpected padding");

struct CFE_EVS_AppNameBitMaskCmd_t {
    uint8 CmdHeader[8];
    char AppName[20];
    uint8 BitMask;
    uint8 Spare;
};
static_assert(sizeof(CFE_EVS_AppNameBitMaskCmd_t) == 30, "unexpected padding");

// AOCS command structs, copied from aocs_cmds.h (explicit padding: the same layout on the
// i386 PC build).
struct AOCS_AttitudeCoordinateCmd_t {
    uint8 CmdHeader[8];
    double Coordinate[3];
    uint32 Time;
    uint32 Padding32;
};
static_assert(sizeof(AOCS_AttitudeCoordinateCmd_t) == 40, "unexpected padding");

struct AOCS_AttitudeNoArgumentCmd_t {
    uint8 CmdHeader[8];
    uint32 Time;
};
static_assert(sizeof(AOCS_AttitudeNoArgumentCmd_t) == 12, "unexpected padding");

struct AOCS_AttitudeQuaternionCmd_t {
    uint8 CmdHeader[8];
    double Quaternion[4];
    double TimePropInit;
    double TimeActiveProp;
    double TimeDeactiveProp;
    uint32 Time;
    uint32 Padding32;
};
static_assert(sizeof(AOCS_AttitudeQuaternionCmd_t) == 72, "unexpected padding");

struct AOCS_AttitudeQuaternionNoTimeCmd_t {
    uint8 CmdHeader[8];
    double Quaternion[4];
    uint32 Time;
    uint32 Padding32;
    double AngleOfRotationDegCmd;
    uint8 AxisOfRotationCmd;
    uint8 IsSunPointing;
    uint8 Padding8[6];
};
static_assert(sizeof(AOCS_AttitudeQuaternionNoTimeCmd_t) == 64, "unexpected padding");

// No length check in the FSW for the next three: the hidden holes must match exactly.
struct AOCS_ChangeControlGainValue_t {
    uint8 CmdHdr[8];
    uint8 IsTuningOnline;
    uint8 ControlMode;
    double Px, Ix, Dx, Py, Iy, Dy, Pz, Iz, Dz;
};
static_assert(sizeof(AOCS_ChangeControlGainValue_t) == 88, "unexpected padding");
static_assert(offsetof(AOCS_ChangeControlGainValue_t, Px) == 16, "6-byte hole before Px");

struct AOCS_ChangeTestSolution_t {
    uint8 CmdHdr[8];
    uint8 RWLControl_Mode;
    uint8 ADS_Mode;
    uint8 ACS_Mode;
};
static_assert(sizeof(AOCS_ChangeTestSolution_t) == 11, "unexpected padding");

// The PC build (lib_simulation_udp) is i386: doubles are 4-byte aligned in structs.
// pack(4) gives this host the same layout (sizeof and offsets read from aocs.so).
#pragma pack(push, 4)
struct AOCS_CalculateOrbitalStateTLE_t {
    uint8 CmdHdr[8];
    uint8 LineNumber;
    uint32 SatelliteCatologNumber;
    uint8 Checksum;
    uint8 Padding[2];
    double Inclination;
    double RightAscensionOfTheAscendingNode;
    double Eccentricity;
    double ArgumentOfPerigee;
    double MeanAnomaly;
    double MeanMotion;
    double RevolutionNumberAtEpoch;
};
#pragma pack(pop)
static_assert(sizeof(AOCS_CalculateOrbitalStateTLE_t) == 76, "unexpected padding");
static_assert(offsetof(AOCS_CalculateOrbitalStateTLE_t, SatelliteCatologNumber) == 12, "hole");
static_assert(offsetof(AOCS_CalculateOrbitalStateTLE_t, Inclination) == 20, "hole");

// SC command structs, copied from sc_msg.h.
struct SC_StartAtsCmd_t {
    uint8 CmdHeader[8];
    uint16 AtsId;
};
static_assert(sizeof(SC_StartAtsCmd_t) == 10, "unexpected padding");

struct SC_RtsCmd_t {
    uint8 CmdHeader[8];
    uint16 RtsId;
};
static_assert(sizeof(SC_RtsCmd_t) == 10, "unexpected padding");

struct SC_JumpAtsCmd_t {
    uint8 CmdHeader[8];
    uint32 NewTime;
};
static_assert(sizeof(SC_JumpAtsCmd_t) == 12, "unexpected padding");

struct SC_SetContinueAtsOnFailureCmd_t {
    uint8 CmdHeader[8];
    uint16 ContinueState;
};
static_assert(sizeof(SC_SetContinueAtsOnFailureCmd_t) == 10, "unexpected padding");

struct SC_AppendAtsCmd_t {
    uint8 CmdHeader[8];
    uint16 AtsId;
};
static_assert(sizeof(SC_AppendAtsCmd_t) == 10, "unexpected padding");

struct SC_RtsGrpCmd_t {
    uint8 CmdHeader[8];
    uint16 FirstRtsId;
    uint16 LastRtsId;
};
static_assert(sizeof(SC_RtsGrpCmd_t) == 12, "unexpected padding");
static_assert(offsetof(SC_RtsGrpCmd_t, LastRtsId) == 10, "unexpected padding");

// FDIR command structs, copied from fdir_platform_msg.h (OS_MAX_API_NAME 20).
struct FDIR_TriggerEtdCmd_t {
    uint8 CmdHeader[8];
    char ApplicationName[20];
    uint16 EventID;
    uint8 EventEnable;
    uint8 Padding[1];
};
static_assert(sizeof(FDIR_TriggerEtdCmd_t) == 32, "unexpected padding");
static_assert(offsetof(FDIR_TriggerEtdCmd_t, EventID) == 28, "unexpected padding");

struct FDIR_TriggerHqmIdx_t {
    uint8 CmdHeader[8];
    uint8 Target;
};
static_assert(sizeof(FDIR_TriggerHqmIdx_t) == 9, "unexpected padding");

struct FDIR_ReleaseHardware_t {
    uint8 CmdHeader[8];
    uint8 ID;
};
static_assert(sizeof(FDIR_ReleaseHardware_t) == 9, "unexpected padding");

struct FDIR_UpdateIOTimeout_t {
    uint8 CmdHeader[8];
    uint32 DevMask;
};
static_assert(sizeof(FDIR_UpdateIOTimeout_t) == 12, "unexpected padding");

// MAIN command structs, copied from main_msgdefs.h (lib_labsat_bbb). The header is
// CFE_SB_CmdHdr_t = CCSDS_CommandPacket_t, 2-byte aligned (uint16 in the secondary header).
struct MAIN_EPS_SetSwitchStateCmd_t {
    CCSDS_CommandPacket_t CmdHeader;
    uint32 SwitchMask;
};
static_assert(sizeof(MAIN_EPS_SetSwitchStateCmd_t) == 12, "unexpected padding");

struct MAIN_EPS_ChangeChargingStateCmd_t {
    CCSDS_CommandPacket_t CmdHeader;
    bool ChargingState;
};
static_assert(sizeof(MAIN_EPS_ChangeChargingStateCmd_t) == 10, "1-byte hole after the bool");

struct MAIN_MODE_ToCaptureSpotCmd_t {
    CCSDS_CommandPacket_t CmdHeader;
    double TargetCoordinate[3];
};
static_assert(sizeof(MAIN_MODE_ToCaptureSpotCmd_t) == 32, "unexpected padding");

struct MAIN_MODE_ToDownlinkCmd_t {
    CCSDS_CommandPacket_t CmdHeader;
    double GSCoordinate[3];
    int16 RTS_TableIndex;
    int16 ATS_TableIndex;
    int32 Padding;
};
static_assert(sizeof(MAIN_MODE_ToDownlinkCmd_t) == 40, "unexpected padding");
static_assert(offsetof(MAIN_MODE_ToDownlinkCmd_t, RTS_TableIndex) == 32, "unexpected padding");

struct MAIN_MODE_ToOrbitTransferCmd_t {
    CCSDS_CommandPacket_t CmdHeader;
    double TargetQuarternion[4];
    double TimePropInit;
    double TimeActiveProp;
    double TimeDeactiveProp;
};
static_assert(sizeof(MAIN_MODE_ToOrbitTransferCmd_t) == 64, "unexpected padding");

struct MAIN_MODE_ToInertialPointingCmd_t {
    CCSDS_CommandPacket_t CmdHeader;
    double TargetQuarternion[4];
    double AngleOfRotationDegCmd;
    uint8 AxisOfRotationCmd;
    uint8 IsSunPointing;
    uint8 Padding8[6];
};
static_assert(sizeof(MAIN_MODE_ToInertialPointingCmd_t) == 56, "unexpected padding");
static_assert(offsetof(MAIN_MODE_ToInertialPointingCmd_t, AxisOfRotationCmd) == 48, "hole");

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
        catalog_ = loadCatalog({std::string(mcs_SOURCE_DIR) + "/config/telecommand/ds.json",
                                std::string(mcs_SOURCE_DIR) + "/config/telecommand/ci.json",
                                std::string(mcs_SOURCE_DIR) + "/config/telecommand/aocs.json",
                                std::string(mcs_SOURCE_DIR) + "/config/telecommand/sc.json",
                                std::string(mcs_SOURCE_DIR) + "/config/telecommand/fdir.json",
                                std::string(mcs_SOURCE_DIR) + "/config/telecommand/main.json",
                                std::string(mcs_SOURCE_DIR) + "/config/telecommand/evs.json"});
    }

    // Full mcs path: catalog -> encoder -> CCSDS v1 format.
    Bytes ours(const std::string& cmd, const std::vector<std::string>& args, int repeat = 1)
    {
        return oursIn("ds", cmd, args, repeat);
    }

    Bytes oursIn(const std::string& app, const std::string& cmd,
                 const std::vector<std::string>& args, int repeat = 1)
    {
        const CommandDef* def = catalog_.findCommand(app, cmd);
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

TEST_F(Oracle, EvsEnableAppEventTypeMatchesCfe)
{
    CFE_EVS_AppNameBitMaskCmd_t cmd{};
    std::strcpy(cmd.AppName, "DS");
    cmd.BitMask = 0x01;   // CFE_EVS_DEBUG_BIT
    EXPECT_EQ(oursIn("evs", "enable_app_event_type", {"DS", "debug"}), cfeBuild(0x1801, 5, 0, &cmd, sizeof cmd));
}

TEST_F(Oracle, EvsEnableEventTypeMatchesCfe)
{
    CFE_EVS_BitMaskCmd_t cmd{};
    cmd.BitMask = 0x0F;   // DEBUG | INFORMATION | ERROR | CRITICAL
    EXPECT_EQ(oursIn("evs", "enable_event_type", {"all"}), cfeBuild(0x1801, 2, 0, &cmd, sizeof cmd));
}

TEST_F(Oracle, CiNoopMatchesCfe)
{
    std::uint8_t header[8] = {};
    EXPECT_EQ(oursIn("ci", "noop", {}), cfeBuild(0x1934, 0, 0, header, sizeof header));
}

TEST_F(Oracle, CiEnableToMatchesCfe)
{
    CI_EnableTOCmd_t cmd{};
    std::strcpy(cmd.cDestIp, "10.0.0.5");
    cmd.usDestPort = 6000;
    EXPECT_EQ(oursIn("ci", "enable_to", {"10.0.0.5", "6000"}),
              cfeBuild(0x1934, 2, 0, &cmd, sizeof cmd));
}

TEST_F(Oracle, CiEnableToDefaultsMatchCfe)
{
    CI_EnableTOCmd_t cmd{};
    std::strcpy(cmd.cDestIp, "127.0.0.1");
    cmd.usDestPort = 5011;
    EXPECT_EQ(oursIn("ci", "enable_to", {}), cfeBuild(0x1934, 2, 0, &cmd, sizeof cmd));
}

TEST_F(Oracle, CiHkMatchesCfe)
{
    // CI_SEND_HK_MID: header only.
    std::uint8_t header[8] = {};
    EXPECT_EQ(oursIn("ci", "hk", {}), cfeBuild(0x1935, 0, 0, header, sizeof header));
}

TEST_F(Oracle, AocsTargetSpotPointingMatchesCfe)
{
    AOCS_AttitudeCoordinateCmd_t cmd{};
    cmd.Coordinate[0] = 21.0285;
    cmd.Coordinate[1] = 105.8542;
    cmd.Coordinate[2] = 12.5;
    cmd.Time = 600;
    EXPECT_EQ(oursIn("aocs", "target_spot_pointing", {"21.0285", "105.8542", "12.5", "600"}),
              cfeBuild(0x1911, 5, 0, &cmd, sizeof cmd));
}

TEST_F(Oracle, AocsNadirPointingMatchesCfe)
{
    AOCS_AttitudeNoArgumentCmd_t cmd{};
    cmd.Time = 0x12345678;
    EXPECT_EQ(oursIn("aocs", "nadir_pointing", {"0x12345678"}),
              cfeBuild(0x1911, 6, 0, &cmd, sizeof cmd));
}

TEST_F(Oracle, AocsThrusterPointingMatchesCfe)
{
    AOCS_AttitudeQuaternionCmd_t cmd{};
    cmd.Quaternion[0] = 0.5;
    cmd.Quaternion[1] = -0.5;
    cmd.Quaternion[2] = 0.25;
    cmd.Quaternion[3] = 0.6614378277661477;
    cmd.TimePropInit = 10.0;
    cmd.TimeActiveProp = 20.5;
    cmd.TimeDeactiveProp = 31.25;
    cmd.Time = 900;
    EXPECT_EQ(oursIn("aocs", "thruster_pointing",
                     {"0.5", "-0.5", "0.25", "0.6614378277661477", "10", "20.5", "31.25", "900"}),
              cfeBuild(0x1911, 10, 0, &cmd, sizeof cmd));
}

TEST_F(Oracle, AocsInertialPointingMatchesCfe)
{
    AOCS_AttitudeQuaternionNoTimeCmd_t cmd{};
    cmd.Quaternion[0] = 0.0;
    cmd.Quaternion[1] = 0.0;
    cmd.Quaternion[2] = 0.7071067811865476;
    cmd.Quaternion[3] = 0.7071067811865476;
    cmd.Time = 300;
    cmd.AngleOfRotationDegCmd = 45.0;
    cmd.AxisOfRotationCmd = 3;   // AOCS_Z_AXIS
    cmd.IsSunPointing = 1;
    EXPECT_EQ(oursIn("aocs", "inertial_pointing",
                     {"0", "0", "0.7071067811865476", "0.7071067811865476", "300", "45", "z", "yes"}),
              cfeBuild(0x1911, 20, 0, &cmd, sizeof cmd));
}

TEST_F(Oracle, AocsChangeControlGainMatchesCfe)
{
    AOCS_ChangeControlGainValue_t cmd{};
    cmd.IsTuningOnline = 1;   // AOCS_TUNING_ONLINE
    cmd.ControlMode = 4;      // UC_AOCS_04 (nadir)
    cmd.Px = 1.5;
    cmd.Ix = 0.01;
    cmd.Dx = 2.0;
    cmd.Py = 1.6;
    cmd.Iy = 0.02;
    cmd.Dy = 2.1;
    cmd.Pz = 1.7;
    cmd.Iz = 0.03;
    cmd.Dz = 2.2;
    EXPECT_EQ(oursIn("aocs", "change_control_gain",
                     {"online", "nadir", "1.5", "0.01", "2.0", "1.6", "0.02", "2.1", "1.7", "0.03", "2.2"}),
              cfeBuild(0x1911, 26, 0, &cmd, sizeof cmd));
}

TEST_F(Oracle, AocsChangeTestSolutionMatchesCfe)
{
    AOCS_ChangeTestSolution_t cmd{};
    cmd.RWLControl_Mode = 1;   // AOCS_RWL_MODE_CURRENT
    cmd.ADS_Mode = 3;          // AOCS_ADS_MODE_ESTIMATOR
    cmd.ACS_Mode = 2;          // AOCS_NO_CONTROL_MTQ
    EXPECT_EQ(oursIn("aocs", "change_test_solution", {"current", "estimator", "no_control_mtq"}),
              cfeBuild(0x1911, 27, 0, &cmd, sizeof cmd));
}

TEST_F(Oracle, AocsOrbitStateFromTleMatchesCfe)
{
    AOCS_CalculateOrbitalStateTLE_t cmd{};
    cmd.LineNumber = 2;
    cmd.SatelliteCatologNumber = 25544;
    cmd.Checksum = 7;
    cmd.Inclination = 51.6416;
    cmd.RightAscensionOfTheAscendingNode = 247.4627;
    cmd.Eccentricity = 0.0006703;
    cmd.ArgumentOfPerigee = 130.5360;
    cmd.MeanAnomaly = 325.0288;
    cmd.MeanMotion = 15.72125391;
    cmd.RevolutionNumberAtEpoch = 56353;
    EXPECT_EQ(oursIn("aocs", "orbit_state_from_tle",
                     {"2", "25544", "7", "51.6416", "247.4627", "0.0006703", "130.5360", "325.0288",
                      "15.72125391", "56353"}),
              cfeBuild(0x1911, 28, 0, &cmd, sizeof cmd));
}

TEST_F(Oracle, AocsHkMatchesCfe)
{
    // AOCS_SEND_HK_MID: AOCS_NoArgsCmd_t, the command code is not read.
    std::uint8_t header[8] = {};
    EXPECT_EQ(oursIn("aocs", "hk", {}), cfeBuild(0x1912, 0, 0, header, sizeof header));
}

TEST_F(Oracle, ScNoopMatchesCfe)
{
    std::uint8_t header[8] = {};
    EXPECT_EQ(oursIn("sc", "noop", {}), cfeBuild(0x1989, 0, 0, header, sizeof header));
}

TEST_F(Oracle, ScStartAtsMatchesCfe)
{
    SC_StartAtsCmd_t cmd{};
    cmd.AtsId = 2;   // SC_ATSB
    EXPECT_EQ(oursIn("sc", "start_ats", {"b"}), cfeBuild(0x1989, 2, 0, &cmd, sizeof cmd));
}

TEST_F(Oracle, ScStartRtsMatchesCfe)
{
    SC_RtsCmd_t cmd{};
    cmd.RtsId = 64;   // SC_NUMBER_OF_RTS
    EXPECT_EQ(oursIn("sc", "start_rts", {"64"}), cfeBuild(0x1989, 4, 0, &cmd, sizeof cmd));
}

TEST_F(Oracle, ScJumpAtsMatchesCfe)
{
    SC_JumpAtsCmd_t cmd{};
    cmd.NewTime = 0x12345678;
    EXPECT_EQ(oursIn("sc", "jump_ats", {"0x12345678"}), cfeBuild(0x1989, 9, 0, &cmd, sizeof cmd));
}

TEST_F(Oracle, ScContinueAtsOnFailureMatchesCfe)
{
    SC_SetContinueAtsOnFailureCmd_t cmd{};
    cmd.ContinueState = 1;   // TRUE
    EXPECT_EQ(oursIn("sc", "continue_ats_on_failure", {"true"}),
              cfeBuild(0x1989, 10, 0, &cmd, sizeof cmd));
}

TEST_F(Oracle, ScAppendAtsMatchesCfe)
{
    SC_AppendAtsCmd_t cmd{};
    cmd.AtsId = 1;   // SC_ATSA
    EXPECT_EQ(oursIn("sc", "append_ats", {"a"}), cfeBuild(0x1989, 11, 0, &cmd, sizeof cmd));
}

TEST_F(Oracle, ScStartRtsGroupMatchesCfe)
{
    SC_RtsGrpCmd_t cmd{};
    cmd.FirstRtsId = 3;
    cmd.LastRtsId = 17;
    EXPECT_EQ(oursIn("sc", "start_rts_group", {"3", "17"}),
              cfeBuild(0x1989, 13, 0, &cmd, sizeof cmd));
}

TEST_F(Oracle, ScHkMatchesCfe)
{
    // SC_SEND_HK_MID: SC_NoArgsCmd_t, the command code is not read.
    std::uint8_t header[8] = {};
    EXPECT_EQ(oursIn("sc", "hk", {}), cfeBuild(0x198A, 0, 0, header, sizeof header));
}

TEST_F(Oracle, FdirTriggerEventWatchpointMatchesCfe)
{
    FDIR_TriggerEtdCmd_t cmd{};
    std::strcpy(cmd.ApplicationName, "AOCS");
    cmd.EventID = 0x1234;
    cmd.EventEnable = 1;   // WP_ON
    EXPECT_EQ(oursIn("fdir", "trigger_event_watchpoint", {"AOCS", "0x1234", "on"}),
              cfeBuild(0x1882, 2, 0, &cmd, sizeof cmd));
}

TEST_F(Oracle, FdirEnableHqmIdxMatchesCfe)
{
    FDIR_TriggerHqmIdx_t cmd{};
    cmd.Target = 17;   // OBC_0
    EXPECT_EQ(oursIn("fdir", "enable_hqm_idx", {"obc_0"}), cfeBuild(0x1882, 5, 0, &cmd, sizeof cmd));
}

TEST_F(Oracle, FdirUpdateIoTimeoutMatchesCfe)
{
    FDIR_UpdateIOTimeout_t cmd{};
    cmd.DevMask = 0x1A005;
    EXPECT_EQ(oursIn("fdir", "update_io_timeout", {"0x1A005"}),
              cfeBuild(0x1883, 9, 0, &cmd, sizeof cmd));
}

TEST_F(Oracle, FdirReleaseHardwareMatchesCfe)
{
    // FDIR_RELEASE_HW_MID: the command code is not read.
    FDIR_ReleaseHardware_t cmd{};
    cmd.ID = 13;   // RWL_2
    EXPECT_EQ(oursIn("fdir", "release_hardware", {"rwl_2"}), cfeBuild(0x1885, 0, 0, &cmd, sizeof cmd));
}

TEST_F(Oracle, FdirHkMatchesCfe)
{
    // FDIR_SEND_HK_MID, FDIR_SEND_APP_HK_TLM: FDIR_NoArgsCmd_t.
    std::uint8_t header[8] = {};
    EXPECT_EQ(oursIn("fdir", "hk", {}), cfeBuild(0x1884, 0, 0, header, sizeof header));
}

TEST_F(Oracle, MainSetSwitchStateMatchesCfe)
{
    MAIN_EPS_SetSwitchStateCmd_t cmd{};
    cmd.SwitchMask = 0x2A5;
    EXPECT_EQ(oursIn("main", "set_switch_state", {"0x2A5"}), cfeBuild(0x1900, 0, 0, &cmd, sizeof cmd));
}

TEST_F(Oracle, MainSetChargingStateMatchesCfe)
{
    MAIN_EPS_ChangeChargingStateCmd_t cmd{};
    cmd.ChargingState = true;   // CHARGING_ON
    EXPECT_EQ(oursIn("main", "set_charging_state", {"on"}), cfeBuild(0x1900, 1, 0, &cmd, sizeof cmd));
}

TEST_F(Oracle, MainToCaptureSpotMatchesCfe)
{
    MAIN_MODE_ToCaptureSpotCmd_t cmd{};
    cmd.TargetCoordinate[0] = 21.0285;
    cmd.TargetCoordinate[1] = 105.8542;
    cmd.TargetCoordinate[2] = 12.5;
    EXPECT_EQ(oursIn("main", "to_capture_spot", {"21.0285", "105.8542", "12.5"}),
              cfeBuild(0x1901, 6, 0, &cmd, sizeof cmd));
}

TEST_F(Oracle, MainToDownlinkMatchesCfe)
{
    MAIN_MODE_ToDownlinkCmd_t cmd{};
    cmd.GSCoordinate[0] = 21.0;
    cmd.GSCoordinate[1] = 105.75;
    cmd.GSCoordinate[2] = 30.0;
    cmd.RTS_TableIndex = 12;
    cmd.ATS_TableIndex = 2;
    EXPECT_EQ(oursIn("main", "to_downlink", {"21", "105.75", "30", "12", "2"}),
              cfeBuild(0x1901, 7, 0, &cmd, sizeof cmd));
}

TEST_F(Oracle, MainToOrbitTransferMatchesCfe)
{
    MAIN_MODE_ToOrbitTransferCmd_t cmd{};
    cmd.TargetQuarternion[0] = 0.5;
    cmd.TargetQuarternion[1] = -0.5;
    cmd.TargetQuarternion[2] = 0.25;
    cmd.TargetQuarternion[3] = 0.6614378277661477;
    cmd.TimePropInit = 10.0;
    cmd.TimeActiveProp = 20.5;
    cmd.TimeDeactiveProp = 31.25;
    EXPECT_EQ(oursIn("main", "to_orbit_transfer",
                     {"0.5", "-0.5", "0.25", "0.6614378277661477", "10", "20.5", "31.25"}),
              cfeBuild(0x1901, 8, 0, &cmd, sizeof cmd));
}

TEST_F(Oracle, MainToInertialPointingMatchesCfe)
{
    MAIN_MODE_ToInertialPointingCmd_t cmd{};
    cmd.TargetQuarternion[2] = 0.7071067811865476;
    cmd.TargetQuarternion[3] = 0.7071067811865476;
    cmd.AngleOfRotationDegCmd = 45.0;
    cmd.AxisOfRotationCmd = 3;   // AOCS_Z_AXIS
    cmd.IsSunPointing = 1;
    EXPECT_EQ(oursIn("main", "to_inertial_pointing",
                     {"0", "0", "0.7071067811865476", "0.7071067811865476", "45", "z", "yes"}),
              cfeBuild(0x1901, 10, 0, &cmd, sizeof cmd));
}

TEST_F(Oracle, MainHkMatchesCfe)
{
    // MAIN_HK_REQ_MID: MAIN_EPS_SampleEPSHkCmd_t, the command code is not read.
    std::uint8_t header[8] = {};
    EXPECT_EQ(oursIn("main", "hk", {}), cfeBuild(0x1906, 0, 0, header, sizeof header));
}

} // namespace
