#include "ui/completion.hpp"
#include "ui/help_format.hpp"

#include <gtest/gtest.h>

#include <algorithm>

using namespace mcs::ui;

namespace {

std::vector<HelpItem> topLevel()
{
    return {{"help", "This help message"},
            {"exit", "Quit the session"},
            {"history", "Show the history"},
            {"fsw", "Send commands to flight software (target 'sil')"},
            {"target", "Show the active target, its protocol stack and catalog"},
            {"verbose <on|off>", "Show or set hexdump of every sent packet"},
            {"raw <hex bytes...>", "Send pre-built packet bytes, e.g. raw 19 4B C0 00 00 01 6C 00"},
            {"arm", "Allow the next critical command to be sent"}};
}

std::vector<std::string> texts(const std::vector<HelpLine>& lines)
{
    std::vector<std::string> out;
    for (const auto& l : lines) {
        out.push_back(l.text);
    }
    return out;
}

// The names marked in the lines, in order.
std::vector<std::string> names(const std::vector<HelpLine>& lines)
{
    std::vector<std::string> out;
    for (const auto& l : lines) {
        for (const auto& span : l.names) {
            out.push_back(l.text.substr(span.offset, span.length));
        }
    }
    return out;
}

// Every word of every item appears somewhere: nothing is cut off.
void expectAllWords(const std::vector<HelpItem>& items, const std::vector<HelpLine>& lines)
{
    std::string all;
    for (const auto& l : lines) {
        all += l.text + " ";
    }
    for (const auto& item : items) {
        for (const auto* text : {&item.usage, &item.description}) {
            std::size_t pos = 0;
            while (pos < text->size()) {
                const auto end = std::min(text->find(' ', pos), text->size());
                const std::string word = text->substr(pos, end - pos);
                EXPECT_NE(all.find(word), std::string::npos) << "missing '" << word << "'";
                pos = end + 1;
            }
        }
    }
}

} // namespace

TEST(HelpFormat, TwoColumnsWhenWide)
{
    const auto items = topLevel();
    const auto lines = formatHelp(items, 120);
    for (const auto& l : texts(lines)) {
        EXPECT_EQ(l.find("│"), std::string::npos) << l;   // no title, no separator line
        EXPECT_EQ(l.find("Commands available"), std::string::npos) << l;
        EXPECT_LE(displayWidth(l), 120U) << l;
    }
    // Two columns need fewer rows than one.
    EXPECT_LT(lines.size(), formatHelp(items, 70).size());
    expectAllWords(items, lines);
    // Entries keep their order, reading down the left column first.
    EXPECT_EQ(lines[0].text.rfind("  help", 0), 0U) << lines[0].text;
    // Rows with an entry on both sides mark both names.
    EXPECT_EQ(lines[0].names.size(), 2U);
}

TEST(HelpFormat, NamesAreMarkedForColour)
{
    const auto items = topLevel();
    for (const int width : {120, 70}) {
        std::vector<std::string> expected;
        for (const auto& item : items) {
            expected.push_back(item.usage);
        }
        auto got = names(formatHelp(items, width));
        std::sort(got.begin(), got.end());
        std::sort(expected.begin(), expected.end());
        EXPECT_EQ(got, expected) << "width " << width;
    }
    const HelpLine line{"  help  This help message", {{2, 4}}};
    EXPECT_EQ(toAnsi(line), "  \x1b[36mhelp\x1b[0m  This help message");
}

TEST(HelpFormat, OneColumnWhenNarrow)
{
    const auto items = topLevel();
    const auto lines = formatHelp(items, 70);
    ASSERT_GE(lines.size(), items.size());
    for (const auto& l : texts(lines)) {
        EXPECT_LE(displayWidth(l), 70U) << l;
    }
    for (const auto& l : lines) {
        EXPECT_LE(l.names.size(), 1U) << l.text;
    }
    expectAllWords(items, lines);
}

TEST(HelpFormat, LongDescriptionWrapsUnderItsColumn)
{
    const std::vector<HelpItem> items{
        {"noop", "No-op: increments the command counter and reports the DS version string"},
        {"reset", "Reset housekeeping counters"}};
    const auto lines = texts(formatHelp(items, 60));   // one column
    ASSERT_GE(lines.size(), 3U);
    const std::size_t descColumn = lines[0].find("No-op:");
    ASSERT_NE(descColumn, std::string::npos);
    // The continuation starts exactly under the description.
    EXPECT_EQ(lines[1].find_first_not_of(' '), descColumn) << lines[1];
    expectAllWords(items, formatHelp(items, 60));
}

TEST(HelpFormat, LongUsageGetsItsOwnLine)
{
    const std::vector<HelpItem> items{
        {"set_dest_path <file_table_index: u16 ..15> <pathname: string[64]>", "Set the pathname"},
        {"noop", "No-op"}};
    const auto lines = formatHelp(items, 70);
    EXPECT_EQ(lines[0].text, "  set_dest_path <file_table_index: u16 ..15> <pathname: string[64]>");
    EXPECT_NE(lines[1].text.find("Set the pathname"), std::string::npos);
    EXPECT_GT(lines[1].text.find_first_not_of(' '), 4U);   // indented under the description column
    expectAllWords(items, lines);
}

TEST(HelpFormat, BalancedColumns)
{
    std::vector<HelpItem> items;
    for (int i = 0; i < 19; ++i) {
        items.push_back({"cmd" + std::to_string(i), "Does thing " + std::to_string(i)});
    }
    const auto lines = formatHelp(items, 100);
    EXPECT_EQ(lines.size(), 10U);   // 10 rows left, 9 right
    EXPECT_EQ(names(lines).size(), 19U);
}

TEST(HelpFormat, WrapLineKeepsIndent)
{
    const auto lines = wrapLine("  sent fsw ds noop -> udp://127.0.0.1:1234  (8 bytes)", 24);
    ASSERT_GT(lines.size(), 1U);
    for (const auto& l : lines) {
        EXPECT_EQ(l.rfind("  ", 0), 0U) << l;
        EXPECT_LE(displayWidth(l), 24U) << l;
    }
    EXPECT_EQ(wrapLine("short", 24), std::vector<std::string>{"short"});
    // A word longer than the line is split, not lost.
    const auto split = wrapLine(std::string(30, 'x'), 10);
    EXPECT_EQ(split.size(), 3U);
}

TEST(TabGrid, ColumnMajorRows)
{
    const std::vector<std::string> words{"add_mid", "close_all", "close_file", "hk", "noop", "reset", "set_app_state"};
    // column width 13 + 3 = 16; width 40 -> (37 + 3) / 16 = 2 columns, 4 rows
    const auto rows = gridRows(words, 40);
    ASSERT_EQ(rows.size(), 4U);
    EXPECT_EQ(rows[0], "   add_mid         noop");
    EXPECT_EQ(rows[1], "   close_all       reset");
    EXPECT_EQ(rows[3], "   hk");
    for (const auto& r : rows) {
        EXPECT_LE(r.size(), 40U) << r;
    }
}

TEST(TabGrid, ManyColumnsWhenWideOneWhenNarrow)
{
    const std::vector<std::string> words{"a", "bb", "ccc", "dddd", "eeeee"};
    EXPECT_EQ(gridRows(words, 200).size(), 1U);
    EXPECT_EQ(gridRows(words, 5).size(), words.size());
    EXPECT_TRUE(gridRows({}, 80).empty());
}
