#include "core/errors.hpp"
#include "core/hex.hpp"

#include <gtest/gtest.h>

using namespace mcs;

TEST(Hex, ToHex)
{
    EXPECT_EQ(toHex({0x19, 0x4B, 0xC0, 0x00}), "19 4B C0 00");
    EXPECT_EQ(toHex({}), "");
}

TEST(Hex, HexdumpWrapsAt16)
{
    Bytes data(18, 0xAB);
    const std::string dump = hexdump(data, "  ");
    EXPECT_EQ(dump,
              "  0000: AB AB AB AB AB AB AB AB AB AB AB AB AB AB AB AB\n"
              "  0010: AB AB\n");
}

TEST(Hex, ParseAcceptsCommonForms)
{
    const Bytes expected{0x19, 0x4B, 0xC0};
    EXPECT_EQ(parseHex("19 4B C0"), expected);
    EXPECT_EQ(parseHex("19 4b c0"), expected);
    EXPECT_EQ(parseHex("0x19,0x4B,0xC0"), expected);
    EXPECT_EQ(parseHex("194BC0"), expected);
    EXPECT_EQ(parseHex("0x194BC0"), expected);
    EXPECT_EQ(parseHex("  19\t4BC0 "), expected);
}

TEST(Hex, ParseRejectsBadInput)
{
    EXPECT_THROW(parseHex(""), ParseError);
    EXPECT_THROW(parseHex("   "), ParseError);
    EXPECT_THROW(parseHex("1"), ParseError);
    EXPECT_THROW(parseHex("19 4G"), ParseError);
    EXPECT_THROW(parseHex("0x"), ParseError);
}

TEST(Hex, RoundTrip)
{
    const Bytes data{0x00, 0x7F, 0x80, 0xFF};
    EXPECT_EQ(parseHex(toHex(data)), data);
}
