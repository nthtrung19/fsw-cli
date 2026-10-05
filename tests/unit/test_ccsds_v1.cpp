#include "core/errors.hpp"
#include "core/hex.hpp"
#include "protocol/ccsds_v1_format.hpp"

#include <gtest/gtest.h>

using namespace fswcli;

namespace {

constexpr std::uint16_t kDsMid = 0x194B;

std::string build(CcsdsV1Format& f, std::uint8_t cc, Bytes payload = {}, std::uint16_t mid = kDsMid)
{
    return toHex(f.build({mid, cc, std::move(payload)}).bytes);
}

} // namespace

// ---- golden vectors (docs/DESIGN.md §2.2), sequence count 0 ------------------

TEST(CcsdsV1, GoldenNoop)
{
    CcsdsV1Format f(Endian::Little);
    EXPECT_EQ(build(f, 0), "19 4B C0 00 00 01 6C 00");
}

TEST(CcsdsV1, GoldenReset)
{
    CcsdsV1Format f(Endian::Little);
    EXPECT_EQ(build(f, 1), "19 4B C0 00 00 01 6D 01");
}

TEST(CcsdsV1, GoldenSetAppStateEnable)
{
    CcsdsV1Format f(Endian::Little);
    EXPECT_EQ(build(f, 2, {0x01, 0x00, 0x00, 0x00}), "19 4B C0 00 00 05 6B 02 01 00 00 00");
}

TEST(CcsdsV1, GoldenSetAppStateDisable)
{
    CcsdsV1Format f(Endian::Little);
    EXPECT_EQ(build(f, 2, {0x00, 0x00, 0x00, 0x00}), "19 4B C0 00 00 05 6A 02 00 00 00 00");
}

// ---- behaviour -----------------------------------------------------------------

TEST(CcsdsV1, ChecksumMakesPacketXorToZero)
{
    CcsdsV1Format f(Endian::Little);
    for (std::size_t n = 0; n < 40; n += 3) {
        const Bytes payload(n, static_cast<std::uint8_t>(n * 7));
        const Bytes packet = f.build({kDsMid, 5, payload}).bytes;
        EXPECT_EQ(CcsdsV1Format::computeChecksum(packet), 0) << "payload size " << n;
    }
}

TEST(CcsdsV1, LengthFieldIsTotalMinusSeven)
{
    CcsdsV1Format f(Endian::Little);
    for (std::size_t n : {0U, 1U, 4U, 100U, 1000U}) {
        const Bytes p = f.build({kDsMid, 0, Bytes(n, 0)}).bytes;
        ASSERT_EQ(p.size(), 8 + n);
        EXPECT_EQ((p[4] << 8) | p[5], static_cast<int>(p.size() - 7));
    }
}

TEST(CcsdsV1, SequenceIncrementsPerApid)
{
    CcsdsV1Format f(Endian::Little);
    const auto seqOf = [](const Bytes& p) { return ((p[2] & 0x3F) << 8) | p[3]; };

    EXPECT_EQ(seqOf(f.build({kDsMid, 0, {}}).bytes), 0);
    EXPECT_EQ(seqOf(f.build({kDsMid, 0, {}}).bytes), 1);
    EXPECT_EQ(seqOf(f.build({0x1880, 0, {}}).bytes), 0);   // other APID: own counter
    EXPECT_EQ(seqOf(f.build({kDsMid, 0, {}}).bytes), 2);

    const BuiltPacket p = f.build({kDsMid, 0, {}});
    EXPECT_EQ(p.note, "apid=0x14B seq=3");
}

TEST(CcsdsV1, SequenceWrapsAfter0x3FFF)
{
    CcsdsV1Format f(Endian::Little);
    Bytes last;
    for (int i = 0; i <= 0x3FFF; ++i) {
        last = f.build({kDsMid, 0, {}}).bytes;
    }
    EXPECT_EQ(last[2], 0xFF);   // flags 11 + count 0x3FFF
    EXPECT_EQ(last[3], 0xFF);
    const Bytes wrapped = f.build({kDsMid, 0, {}}).bytes;
    EXPECT_EQ(wrapped[2], 0xC0);
    EXPECT_EQ(wrapped[3], 0x00);
}

TEST(CcsdsV1, BigEndianTargetSwapsSecondaryHeader)
{
    CcsdsV1Format f(Endian::Big);
    const Bytes p = f.build({kDsMid, 2, {0x00, 0x01, 0x00, 0x00}}).bytes;
    EXPECT_EQ(p[6], 0x02);   // function code first
    EXPECT_EQ(CcsdsV1Format::computeChecksum(p), 0);
    EXPECT_EQ(toHex(p), "19 4B C0 00 00 05 02 6B 00 01 00 00");
}

TEST(CcsdsV1, ChecksumCanBeDisabled)
{
    CcsdsV1Format f(Endian::Little, /*checksum=*/false);
    EXPECT_EQ(build(f, 0), "19 4B C0 00 00 01 00 00");
    EXPECT_NE(f.describe().find("no checksum"), std::string::npos);
}

TEST(CcsdsV1, RejectsInvalidMessages)
{
    CcsdsV1Format f(Endian::Little);
    EXPECT_THROW(f.build({0x094B, 0, {}}), EncodeError);   // telemetry MID (type bit 0)
    EXPECT_THROW(f.build({0x114B, 0, {}}), EncodeError);   // no secondary header flag
    EXPECT_THROW(f.build({0x394B, 0, {}}), EncodeError);   // version bits set
    EXPECT_THROW(f.build({kDsMid, 128, {}}), EncodeError); // function code > 127
    EXPECT_THROW(f.build({kDsMid, 0, Bytes(CcsdsV1Format::kMaxPacketSize - 7, 0)}), EncodeError);
    EXPECT_NO_THROW(f.build({kDsMid, 0, Bytes(CcsdsV1Format::kMaxPacketSize - 8, 0)}));
}

TEST(CcsdsV1, Describe)
{
    EXPECT_EQ(CcsdsV1Format(Endian::Little).describe(), "ccsds_v1 (little-endian)");
}
