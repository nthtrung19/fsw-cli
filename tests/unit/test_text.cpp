#include "core/bytes.hpp"
#include "core/errors.hpp"
#include "core/text.hpp"

#include <gtest/gtest.h>

using namespace mcs;

TEST(Text, ParseUnsignedDecimalAndHex)
{
    EXPECT_EQ(parseUnsigned("0"), 0U);
    EXPECT_EQ(parseUnsigned("42"), 42U);
    EXPECT_EQ(parseUnsigned("0x194B"), 0x194BU);
    EXPECT_EQ(parseUnsigned("0X194b"), 0x194BU);
    EXPECT_EQ(parseUnsigned("18446744073709551615"), UINT64_MAX);
}

TEST(Text, ParseUnsignedRejectsGarbage)
{
    EXPECT_FALSE(parseUnsigned(""));
    EXPECT_FALSE(parseUnsigned("0x"));
    EXPECT_FALSE(parseUnsigned("-1"));
    EXPECT_FALSE(parseUnsigned("12a"));
    EXPECT_FALSE(parseUnsigned(" 1"));
    EXPECT_FALSE(parseUnsigned("18446744073709551616"));   // overflow
}

TEST(Text, ParseSigned)
{
    EXPECT_EQ(parseSigned("-5"), -5);
    EXPECT_EQ(parseSigned("+7"), 7);
    EXPECT_EQ(parseSigned("-0x10"), -16);
    EXPECT_EQ(parseSigned("-9223372036854775808"), INT64_MIN);
    EXPECT_FALSE(parseSigned("9223372036854775808"));
    EXPECT_FALSE(parseSigned("--1"));
}

TEST(Text, ParseDouble)
{
    EXPECT_DOUBLE_EQ(*parseDouble("1.5"), 1.5);
    EXPECT_DOUBLE_EQ(*parseDouble("-2e3"), -2000.0);
    EXPECT_FALSE(parseDouble("1.5x"));
    EXPECT_FALSE(parseDouble("inf"));
    EXPECT_FALSE(parseDouble("nan"));
    EXPECT_FALSE(parseDouble(""));
}

TEST(Text, Identifiers)
{
    EXPECT_TRUE(isIdentifier("set_app_state"));
    EXPECT_TRUE(isIdentifier("ds2"));
    EXPECT_FALSE(isIdentifier("2ds"));
    EXPECT_FALSE(isIdentifier("Ds"));
    EXPECT_FALSE(isIdentifier("set-app"));
    EXPECT_FALSE(isIdentifier(""));
}

TEST(Text, Helpers)
{
    EXPECT_EQ(toHexString(0x14B, 3), "0x14B");
    EXPECT_EQ(toHexString(0x5, 4), "0x0005");
    EXPECT_EQ(trim("  a b \t"), "a b");
    EXPECT_EQ(join({"a", "b", "c"}, ", "), "a, b, c");
    EXPECT_TRUE(iequals("ENABLE", "enable"));
}

TEST(Text, Endian)
{
    EXPECT_EQ(parseEndian("little"), Endian::Little);
    EXPECT_EQ(parseEndian("BIG"), Endian::Big);
    EXPECT_THROW(parseEndian("middle"), ParseError);
    EXPECT_EQ(toString(Endian::Little), "little");
}
