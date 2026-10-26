#include "console/console_store.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace mcs;

namespace {

Bytes bytesOf(const std::string& text)
{
    return Bytes(text.begin(), text.end());
}

std::vector<std::string> texts(const ConsoleStore& store)
{
    std::vector<std::string> out;
    for (const auto& line : store.snapshot().lines) {
        out.push_back(line.text);
    }
    return out;
}

const ConsoleTime kT0 = ConsoleClock::now();

ConsoleTime at(int ms)
{
    return kT0 + std::chrono::milliseconds(ms);
}

} // namespace

TEST(ConsoleStore, LinesSpanDatagramsAndDatagramsCarrySeveralLines)
{
    ConsoleStore store;
    store.ingest(bytesOf("hel"), at(0));
    store.ingest(bytesOf("lo\r\nwor"), at(10));
    store.ingest(bytesOf("ld\nthird\n\n"), at(20));
    EXPECT_EQ(texts(store), (std::vector<std::string>{"hello", "world", "third", ""}));

    const ConsoleSnapshot s = store.snapshot();
    EXPECT_EQ(s.lines[0].rxTime, at(0));    // when its first byte arrived
    EXPECT_EQ(s.lines[1].rxTime, at(10));
    EXPECT_EQ(s.datagrams, 3U);
    EXPECT_EQ(s.bytes, 20U);
    EXPECT_EQ(s.lastRx, at(20));
}

TEST(ConsoleStore, UnfinishedLineShowsOnlyAfterAPause)
{
    ConsoleStore store;
    store.ingest(bytesOf("SHLL [/] # "), at(0));
    store.flushIdle(at(100));
    EXPECT_TRUE(texts(store).empty());

    store.flushIdle(at(300));
    EXPECT_EQ(texts(store), (std::vector<std::string>{"SHLL [/] # "}));

    // A late "\r" alone is a finished line, not an empty one.
    store.ingest(bytesOf("abc\r"), at(400));
    store.flushIdle(at(800));
    store.ingest(bytesOf("\r"), at(900));
    store.flushIdle(at(1300));
    EXPECT_EQ(texts(store), (std::vector<std::string>{"SHLL [/] # ", "abc"}));
}

TEST(ConsoleStore, TextIsMadeDisplayable)
{
    EXPECT_EQ(sanitizeConsoleText("a\tb"), "a       b");
    EXPECT_EQ(sanitizeConsoleText("\x1b[1;31mERROR\x1b[0m done"), "ERROR done");
    EXPECT_EQ(sanitizeConsoleText(std::string("x\0y\x07", 4)), "x·y·");
    EXPECT_EQ(sanitizeConsoleText("caf\xC3\xA9"), "caf\xC3\xA9");          // valid UTF-8 kept
    EXPECT_EQ(sanitizeConsoleText("bad\xC3(\xFF"), "bad·(·");    // invalid bytes replaced
    EXPECT_EQ(sanitizeConsoleText("mid\rline"), "mid·line");
}

TEST(ConsoleStore, LongLinesAreSplitOutsideUtf8Sequences)
{
    ConsoleStore store;
    std::string text(ConsoleStore::kMaxLineLength - 1, 'x');
    text += "\xC3\xA9tail\n";   // 'é' straddles the limit
    store.ingest(bytesOf(text), at(0));
    const auto lines = texts(store);
    ASSERT_EQ(lines.size(), 2U);
    EXPECT_EQ(lines[0], std::string(ConsoleStore::kMaxLineLength - 1, 'x'));
    EXPECT_EQ(lines[1], "\xC3\xA9tail");
}

TEST(ConsoleStore, RingKeepsTheNewestLines)
{
    ConsoleStore store;
    std::string text;
    for (std::size_t i = 0; i < ConsoleStore::kMaxLines + 5; ++i) {
        text += std::to_string(i) + "\n";
    }
    store.ingest(bytesOf(text), at(0));
    const ConsoleSnapshot s = store.snapshot();
    ASSERT_EQ(s.lines.size(), ConsoleStore::kMaxLines);
    EXPECT_EQ(s.lines.front().text, "5");
    EXPECT_EQ(s.dropped, 5U);

    store.clear();
    EXPECT_TRUE(store.snapshot().lines.empty());
    EXPECT_EQ(store.snapshot().datagrams, 0U);
}
