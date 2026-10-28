#include "ui/fsw_command.hpp"

#include <gtest/gtest.h>

#include <sstream>

using namespace mcs;
using namespace mcs::ui;

namespace {

// What the completer was asked, and a fixed answer.
struct Seen {
    std::vector<std::string> previous;
    std::string partial;
};

FswCommand lineCommand(const std::string& name, Seen& seen)
{
    return FswCommand(name, "help", {}, [](std::ostream&, const std::vector<std::string>&) {},
                      FswCommand::LineCompleter([&seen](const std::vector<std::string>& previous,
                                                        const std::string& partial) {
                          seen = {previous, partial};
                          ArgCompletion c;
                          c.values.emplace_back("delete_dir", "Delete");
                          return c;
                      }));
}

std::string item(const std::string& line, const std::string& description)
{
    return line + cli::CompletionHelpSeparator + description;
}

} // namespace

TEST(FswCommand, BangCompletesTheRestOfTheLine)
{
    Seen seen;
    const FswCommand bang = lineCommand("!", seen);

    EXPECT_EQ(bang.GetCompletionRecursive("! "), std::vector<std::string>{item("! delete_dir", "Delete")});
    EXPECT_TRUE(seen.previous.empty());
    EXPECT_EQ(seen.partial, "");

    EXPECT_EQ(bang.GetCompletionRecursive("! fm del"), std::vector<std::string>{item("! fm delete_dir", "Delete")});
    EXPECT_EQ(seen.previous, std::vector<std::string>{"fm"});
    EXPECT_EQ(seen.partial, "del");
}

TEST(FswCommand, BangWithoutSpaceStillCompletes)
{
    // The cli tokenizer splits "!del" into "!" and "del".
    Seen seen;
    const FswCommand bang = lineCommand("!", seen);
    EXPECT_EQ(bang.GetCompletionRecursive("!del"), std::vector<std::string>{item("!delete_dir", "Delete")});
    EXPECT_EQ(seen.partial, "del");
}

TEST(FswCommand, UnlistedNameIsNeverOffered)
{
    Seen seen;
    FswCommand bang = lineCommand("!", seen);
    bang.setListed(false);
    EXPECT_TRUE(bang.GetCompletionRecursive("").empty());    // TAB on an empty line
    EXPECT_TRUE(bang.GetCompletionRecursive("!").empty());
    // Once typed, the rest of the line still completes.
    EXPECT_EQ(bang.GetCompletionRecursive("! "), std::vector<std::string>{item("! delete_dir", "Delete")});
    EXPECT_EQ(bang.GetCompletionRecursive("!del"), std::vector<std::string>{item("!delete_dir", "Delete")});

    std::ostringstream help;
    bang.Help(help);
    EXPECT_EQ(help.str(), "");
}

TEST(FswCommand, IndexCompleterAndNameCompletion)
{
    std::size_t asked = 99;
    const FswCommand verbose("verbose", "help", {}, [](std::ostream&, const std::vector<std::string>&) {},
                             [&asked](std::size_t index, const std::string&) {
                                 asked = index;
                                 ArgCompletion c;
                                 c.values.emplace_back("on", "hexdump on");
                                 return c;
                             });
    EXPECT_EQ(verbose.GetCompletionRecursive("verbose o"), std::vector<std::string>{item("verbose on", "hexdump on")});
    EXPECT_EQ(asked, 0U);
    EXPECT_EQ(verbose.GetCompletionRecursive("verb"), std::vector<std::string>{item("verbose", "help")});   // the name
    EXPECT_TRUE(verbose.GetCompletionRecursive("verbosex").empty());   // another word, not "verbose"
}
