#include "catalog/arg_completion.hpp"
#include "unit/test_helpers.hpp"

#include <gtest/gtest.h>

using namespace mcs;

namespace {

std::vector<std::string> names(const ArgCompletion& c)
{
    std::vector<std::string> out;
    for (const auto& v : c.values) {
        out.push_back(v.first);
    }
    return out;
}

bool contains(const std::string& text, const std::string& fragment)
{
    return text.find(fragment) != std::string::npos;
}

CommandDef commandWith(std::vector<FieldDef> fields)
{
    return {"cmd", 9, "test command", std::move(fields), false, 0x194B};
}

} // namespace

TEST(ArgCompletion, EnumNamesCompleteCaseInsensitively)
{
    const CommandDef set = test::dsApp().commands[2];   // set_app_state <state: disable|enable>
    const ArgCompletion all = completeArgument(set, 0, "");
    EXPECT_EQ(names(all), (std::vector<std::string>{"disable", "enable"}));
    EXPECT_EQ(all.values[0].second, "state = 0");
    EXPECT_EQ(all.values[1].second, "state = 1");
    EXPECT_TRUE(all.hint.empty());

    EXPECT_EQ(names(completeArgument(set, 0, "EN")), std::vector<std::string>{"enable"});
}

TEST(ArgCompletion, EnumDescriptionIsFieldAndValueNotHelp)
{
    // The field help ("EnableState") next to "off" would read as a description of "off".
    FieldDef state = FieldDef::enumeration("state", FieldType::U16, {{"off", 0}, {"on", 1}});
    state.help = "EnableState";
    const ArgCompletion c = completeArgument(commandWith({state}), 0, "o");
    EXPECT_EQ(names(c), (std::vector<std::string>{"off", "on"}));
    EXPECT_EQ(c.values[0].second, "state = 0");
    EXPECT_EQ(c.values[1].second, "state = 1");
}

TEST(ArgCompletion, UnmatchedEnumTextFallsBackToHint)
{
    const CommandDef set = test::dsApp().commands[2];
    for (const char* typed : {"x", "1"}) {   // a number is still accepted, just not completed
        const ArgCompletion c = completeArgument(set, 0, typed);
        EXPECT_TRUE(c.values.empty()) << typed;
        EXPECT_EQ(c.hint, "<state: disable|enable>  (argument 1 of 1)") << typed;
    }
}

TEST(ArgCompletion, NumberAndStringFieldsGiveHints)
{
    FieldDef count = FieldDef::number("count", FieldType::U8);
    count.min = 1;
    count.max = 10;
    count.help = "How many";
    FieldDef path = FieldDef::string("path", 64);
    const CommandDef cmd = commandWith({count, FieldDef::padding(2), path});

    const ArgCompletion first = completeArgument(cmd, 0, "");
    EXPECT_TRUE(first.values.empty());
    EXPECT_EQ(first.hint, "<count: u8 1..10>   How many  (argument 1 of 2)");

    // Padding is not an argument: index 1 is the string.
    const ArgCompletion second = completeArgument(cmd, 1, "/ra");
    EXPECT_TRUE(contains(second.hint, "<path: string[64]>")) << second.hint;
    EXPECT_TRUE(contains(second.hint, "(argument 2 of 2, at most 63 characters)")) << second.hint;
}

TEST(ArgCompletion, DefaultedArgumentIsOptional)
{
    FieldDef port = FieldDef::number("port", FieldType::U16);
    port.defaultValue = "5011";
    const ArgCompletion c = completeArgument(commandWith({port}), 0, "");
    EXPECT_EQ(c.hint, "<port: u16 = 5011>  (argument 1 of 1, optional)");
}

TEST(ArgCompletion, NoMoreArguments)
{
    const std::string expected = "(no more arguments: press Enter to send)";
    EXPECT_EQ(completeArgument(test::dsApp().commands[0], 0, "").hint, expected);   // noop
    EXPECT_EQ(completeArgument(test::dsApp().commands[2], 1, "").hint, expected);   // past the end
}
