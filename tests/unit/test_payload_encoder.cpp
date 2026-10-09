#include "core/errors.hpp"
#include "encode/payload_encoder.hpp"
#include "unit/test_helpers.hpp"

#include <gtest/gtest.h>

using namespace mcs;

namespace {

CommandDef commandWith(std::vector<FieldDef> fields)
{
    return {"cmd", 9, "test command", std::move(fields), false, 0x194B};
}

std::string errorOf(const PayloadEncoder& enc, const CommandDef& cmd,
                    const std::vector<std::string>& args)
{
    try {
        enc.encode(cmd, args);
    } catch (const ParseError& e) {
        return e.what();
    }
    return "<no error>";
}

} // namespace

TEST(PayloadEncoder, SetAppStateLittleEndian)
{
    const PayloadEncoder enc(Endian::Little);
    const CommandDef set = test::dsApp().commands[2];
    EXPECT_EQ(enc.encode(set, {"enable"}), (Bytes{0x01, 0x00, 0x00, 0x00}));
    EXPECT_EQ(enc.encode(set, {"disable"}), (Bytes{0x00, 0x00, 0x00, 0x00}));
    EXPECT_EQ(enc.encode(set, {"ENABLE"}), (Bytes{0x01, 0x00, 0x00, 0x00}));
    EXPECT_EQ(enc.encode(set, {"1"}), (Bytes{0x01, 0x00, 0x00, 0x00}));   // numbers still allowed
}

TEST(PayloadEncoder, BigEndianTarget)
{
    const PayloadEncoder enc(Endian::Big);
    EXPECT_EQ(enc.encode(test::dsApp().commands[2], {"enable"}), (Bytes{0x00, 0x01, 0x00, 0x00}));
}

TEST(PayloadEncoder, NoFieldsGivesEmptyPayload)
{
    const PayloadEncoder enc(Endian::Little);
    EXPECT_TRUE(enc.encode(test::dsApp().commands[0], {}).empty());
}

TEST(PayloadEncoder, ArgumentCountIsChecked)
{
    const PayloadEncoder enc(Endian::Little);
    const CommandDef set = test::dsApp().commands[2];
    EXPECT_EQ(errorOf(enc, set, {}),
              "set_app_state: expected 1 argument, got 0\n  usage: set_app_state <state: disable|enable>");
    EXPECT_NE(errorOf(enc, set, {"enable", "extra"}).find("expected 1 argument, got 2"),
              std::string::npos);
    EXPECT_NE(errorOf(enc, test::dsApp().commands[0], {"x"}).find("expected 0 arguments, got 1"),
              std::string::npos);
}

TEST(PayloadEncoder, OmittedTrailingArgumentsUseDefaults)
{
    const PayloadEncoder enc(Endian::Little);
    FieldDef ip = FieldDef::string("ip", 4);
    ip.defaultValue = "abc";
    FieldDef port = FieldDef::number("port", FieldType::U16);
    port.defaultValue = "5011";
    const CommandDef cmd = commandWith({ip, port});

    EXPECT_EQ(enc.encode(cmd, {}), (Bytes{'a', 'b', 'c', 0, 0x93, 0x13}));
    EXPECT_EQ(enc.encode(cmd, {"xy"}), (Bytes{'x', 'y', 0, 0, 0x93, 0x13}));
    EXPECT_EQ(enc.encode(cmd, {"xy", "1"}), (Bytes{'x', 'y', 0, 0, 0x01, 0x00}));
    EXPECT_EQ(errorOf(enc, cmd, {"a", "1", "2"}),
              "cmd: expected 0 to 2 arguments, got 3\n  usage: cmd <ip: string[4] = abc> <port: u16 = 5011>");
}

TEST(PayloadEncoder, CheckDefaultsRejectsInvalidDefault)
{
    const PayloadEncoder enc(Endian::Little);
    FieldDef port = FieldDef::number("port", FieldType::U16);
    port.defaultValue = "70000";
    EXPECT_THROW(enc.checkDefaults(commandWith({port})), ParseError);
    port.defaultValue = "5011";
    EXPECT_NO_THROW(enc.checkDefaults(commandWith({port})));
}

TEST(PayloadEncoder, EnumFieldRejectsUnknownNameAndOutOfRange)
{
    const PayloadEncoder enc(Endian::Little);
    const CommandDef set = test::dsApp().commands[2];
    EXPECT_EQ(errorOf(enc, set, {"maybe"}),
              "set_app_state: invalid value 'maybe' for <state: disable|enable>: "
              "not one of the allowed names or a number");
    EXPECT_NE(errorOf(enc, set, {"70000"}).find("out of range for u16"), std::string::npos);
    EXPECT_NE(errorOf(enc, set, {"-1"}).find("not one of the allowed names"), std::string::npos);
}

TEST(PayloadEncoder, EnumFieldAcceptsOnlyListedNumbers)
{
    // The FSW only understands the listed values, so other numbers that fit
    // the type are refused too.
    const PayloadEncoder enc(Endian::Little);
    const CommandDef set = test::dsApp().commands[2];
    EXPECT_NO_THROW(enc.encode(set, {"0"}));
    EXPECT_NO_THROW(enc.encode(set, {"0x1"}));
    EXPECT_EQ(errorOf(enc, set, {"7"}),
              "set_app_state: invalid value '7' for <state: disable|enable>: not one of the allowed values");

    const CommandDef signedEnum = commandWith(
        {FieldDef::enumeration("mode", FieldType::I8, {{"low", -1}, {"high", 1}})});
    EXPECT_EQ(enc.encode(signedEnum, {"-1"}), (Bytes{0xFF}));
    EXPECT_EQ(enc.encode(signedEnum, {"HIGH"}), (Bytes{0x01}));
    EXPECT_THROW(enc.encode(signedEnum, {"0"}), ParseError);
}

TEST(PayloadEncoder, AllIntegerTypes)
{
    const PayloadEncoder enc(Endian::Little);
    const CommandDef cmd = commandWith({
        FieldDef::number("a", FieldType::U8),  FieldDef::number("b", FieldType::U32),
        FieldDef::number("c", FieldType::U64), FieldDef::number("d", FieldType::I8),
        FieldDef::number("e", FieldType::I16), FieldDef::number("f", FieldType::I32),
        FieldDef::number("g", FieldType::I64),
    });
    const Bytes out = enc.encode(cmd, {"0xFF", "0x01020304", "1", "-1", "-2", "-3", "-4"});
    EXPECT_EQ(out, (Bytes{0xFF, 0x04, 0x03, 0x02, 0x01, 1, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFE, 0xFF,
                          0xFD, 0xFF, 0xFF, 0xFF, 0xFC, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}));
}

TEST(PayloadEncoder, IntegerLimits)
{
    const PayloadEncoder enc(Endian::Little);
    const CommandDef u8 = commandWith({FieldDef::number("v", FieldType::U8)});
    const CommandDef i8 = commandWith({FieldDef::number("v", FieldType::I8)});
    EXPECT_NO_THROW(enc.encode(u8, {"255"}));
    EXPECT_THROW(enc.encode(u8, {"256"}), ParseError);
    EXPECT_THROW(enc.encode(u8, {"-1"}), ParseError);
    EXPECT_THROW(enc.encode(u8, {"abc"}), ParseError);
    EXPECT_NO_THROW(enc.encode(i8, {"-128"}));
    EXPECT_THROW(enc.encode(i8, {"128"}), ParseError);
    EXPECT_THROW(enc.encode(i8, {"-129"}), ParseError);
}

TEST(PayloadEncoder, MinMaxRange)
{
    const PayloadEncoder enc(Endian::Little);
    FieldDef f = FieldDef::number("count", FieldType::U16);
    f.min = 1;
    f.max = 10;
    FieldDef g = FieldDef::number("offset", FieldType::I16);
    g.min = -5;
    g.max = 5;
    const CommandDef cmd = commandWith({f, g});
    EXPECT_NO_THROW(enc.encode(cmd, {"1", "-5"}));
    EXPECT_NO_THROW(enc.encode(cmd, {"10", "5"}));
    EXPECT_NE(errorOf(enc, cmd, {"0", "0"}).find("below minimum 1"), std::string::npos);
    EXPECT_NE(errorOf(enc, cmd, {"11", "0"}).find("above maximum 10"), std::string::npos);
    EXPECT_NE(errorOf(enc, cmd, {"5", "-6"}).find("below minimum -5"), std::string::npos);
}

TEST(PayloadEncoder, Floats)
{
    const PayloadEncoder enc(Endian::Little);
    const CommandDef cmd = commandWith({FieldDef::number("a", FieldType::F32),
                                        FieldDef::number("b", FieldType::F64)});
    EXPECT_EQ(enc.encode(cmd, {"1.0", "-2"}),
              (Bytes{0x00, 0x00, 0x80, 0x3F, 0, 0, 0, 0, 0, 0, 0x00, 0xC0}));
    EXPECT_THROW(enc.encode(cmd, {"1e39", "0"}), ParseError);   // beyond f32
    EXPECT_THROW(enc.encode(cmd, {"x", "0"}), ParseError);
}

TEST(PayloadEncoder, Strings)
{
    const PayloadEncoder enc(Endian::Little);
    const CommandDef cmd = commandWith({FieldDef::string("name", 6), FieldDef::padding(2)});
    EXPECT_EQ(enc.encode(cmd, {"abc"}), (Bytes{'a', 'b', 'c', 0, 0, 0, 0, 0}));
    EXPECT_EQ(enc.encode(cmd, {"a b"}), (Bytes{'a', ' ', 'b', 0, 0, 0, 0, 0}));
    EXPECT_NO_THROW(enc.encode(cmd, {"abcde"}));
    EXPECT_NE(errorOf(enc, cmd, {"abcdef"}).find("too long (at most 5 characters)"),
              std::string::npos);
}

TEST(PayloadEncoder, PaddingBetweenFields)
{
    const PayloadEncoder enc(Endian::Little);
    const CommandDef cmd = commandWith({FieldDef::number("a", FieldType::U8), FieldDef::padding(3),
                                        FieldDef::number("b", FieldType::U32)});
    EXPECT_EQ(enc.encode(cmd, {"1", "2"}), (Bytes{1, 0, 0, 0, 2, 0, 0, 0}));
}
