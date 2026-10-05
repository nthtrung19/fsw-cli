#include "core/byte_writer.hpp"
#include "core/errors.hpp"

#include <gtest/gtest.h>

using namespace fswcli;

TEST(ByteWriter, LittleEndianIntegers)
{
    ByteWriter w(Endian::Little);
    w.u8(0x01);
    w.u16(0x0203);
    w.u32(0x04050607);
    w.u64(0x08090A0B0C0D0E0FULL);
    EXPECT_EQ(w.data(), (Bytes{0x01, 0x03, 0x02, 0x07, 0x06, 0x05, 0x04,
                               0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08}));
}

TEST(ByteWriter, BigEndianIntegers)
{
    ByteWriter w(Endian::Big);
    w.u16(0x0203);
    w.u32(0x04050607);
    EXPECT_EQ(w.data(), (Bytes{0x02, 0x03, 0x04, 0x05, 0x06, 0x07}));
}

TEST(ByteWriter, SignedTwosComplement)
{
    ByteWriter w(Endian::Little);
    w.i8(-1);
    w.i16(-2);
    w.i32(-3);
    EXPECT_EQ(w.data(), (Bytes{0xFF, 0xFE, 0xFF, 0xFD, 0xFF, 0xFF, 0xFF}));
}

TEST(ByteWriter, Floats)
{
    ByteWriter le(Endian::Little);
    le.f32(1.0F);       // 0x3F800000
    le.f64(-2.0);       // 0xC000000000000000
    EXPECT_EQ(le.data(), (Bytes{0x00, 0x00, 0x80, 0x3F, 0, 0, 0, 0, 0, 0, 0x00, 0xC0}));

    ByteWriter be(Endian::Big);
    be.f32(1.0F);
    EXPECT_EQ(be.data(), (Bytes{0x3F, 0x80, 0x00, 0x00}));
}

TEST(ByteWriter, FixedStringIsNulFilled)
{
    ByteWriter w(Endian::Little);
    w.fixedString("ab", 5);
    EXPECT_EQ(w.data(), (Bytes{'a', 'b', 0, 0, 0}));
}

TEST(ByteWriter, FixedStringNeedsRoomForNul)
{
    ByteWriter w(Endian::Little);
    EXPECT_NO_THROW(w.fixedString("abcd", 5));
    EXPECT_THROW(w.fixedString("abcde", 5), EncodeError);
}

TEST(ByteWriter, ZerosBytesAndTake)
{
    ByteWriter w(Endian::Little);
    w.zeros(2);
    w.bytes({0xAA, 0xBB});
    EXPECT_EQ(w.size(), 4U);
    const Bytes out = w.take();
    EXPECT_EQ(out, (Bytes{0, 0, 0xAA, 0xBB}));
}

TEST(ByteWriter, PatchU16BE)
{
    Bytes b{0, 0, 0, 0};
    ByteWriter::patchU16BE(b, 1, 0x1234);
    EXPECT_EQ(b, (Bytes{0, 0x12, 0x34, 0}));
    EXPECT_THROW(ByteWriter::patchU16BE(b, 3, 1), std::out_of_range);
}
