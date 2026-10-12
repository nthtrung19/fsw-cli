#include "ui/completion.hpp"

#include <gtest/gtest.h>

using namespace mcs::ui;

namespace {

std::string withHelp(const std::string& line, const std::string& help)
{
    return line + kCompletionHelpSeparator + help;
}

} // namespace

TEST(Completion, SingleMatchCompletesWithSpace)
{
    const auto r = applyCompletion("fsw d", {withHelp("fsw ds", "Data Storage")});
    ASSERT_TRUE(r.newLine);
    EXPECT_EQ(*r.newLine, "fsw ds ");
    EXPECT_TRUE(r.items.empty());
}

TEST(Completion, CommonPrefixExtendsTheLine)
{
    const auto r = applyCompletion("fsw ds set", {"fsw ds set_app_state", "fsw ds set_filter_file"});
    ASSERT_TRUE(r.newLine);
    EXPECT_EQ(*r.newLine, "fsw ds set_");
}

TEST(Completion, SeveralMatchesAreListedByLastWord)
{
    const auto r = applyCompletion("fsw ds ", {withHelp("fsw ds noop", "No-op"), "fsw ds reset"});
    EXPECT_FALSE(r.newLine);
    ASSERT_EQ(r.items.size(), 2U);
    EXPECT_EQ(r.items[0].word, "noop");
    EXPECT_EQ(r.items[0].description, "No-op");
    EXPECT_EQ(r.items[1].word, "reset");
    EXPECT_EQ(r.items[1].description, "");
}

TEST(Completion, HintsAreNeverInserted)
{
    const std::string hint = std::string("fsw ds set_app_state ") + kCompletionHintMarker + "<state>";
    const auto onlyHint = applyCompletion("fsw ds set_app_state ", {hint});
    EXPECT_FALSE(onlyHint.newLine);
    EXPECT_TRUE(onlyHint.items.empty());
    ASSERT_EQ(onlyHint.hints.size(), 1U);
    EXPECT_EQ(onlyHint.hints[0], "<state>");

    const auto withMatch = applyCompletion("x", {hint, "xyz"});
    ASSERT_TRUE(withMatch.newLine);
    EXPECT_EQ(*withMatch.newLine, "xyz ");
    EXPECT_TRUE(withMatch.hints.empty());
}

TEST(Completion, NothingToComplete)
{
    const auto r = applyCompletion("zzz", {});
    EXPECT_FALSE(r.newLine);
    EXPECT_TRUE(r.items.empty());
    EXPECT_TRUE(r.hints.empty());
}
