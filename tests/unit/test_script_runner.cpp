#include "catalog/catalog.hpp"
#include "config/tlm_catalog_loader.hpp"
#include "protocol/ccsds_v1_format.hpp"
#include "telemetry/tlm_catalog.hpp"
#include "ui/script_runner.hpp"
#include "unit/test_helpers.hpp"
#include "verify/tlm_history.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <sstream>
#include <thread>

using namespace mcs;
using namespace mcs::ui;
using namespace std::chrono_literals;

namespace {

const std::string kSourceDir = mcs_SOURCE_DIR;
constexpr std::uint16_t kDsHk = 0x0948;

// Counts the packets it is given (from any thread).
class CountingTransport : public ITransport {
public:
    explicit CountingTransport(std::atomic<int>& sent) : sent_(sent) {}
    void send(const Bytes&) override { ++sent_; }
    std::string describe() const override { return "fake://"; }
    bool isDryRun() const override { return false; }

private:
    std::atomic<int>& sent_;
};

TelemetryCatalog telemetry()
{
    TlmPacketDef hk;
    hk.app = "ds";
    hk.name = "hk";
    hk.mid = kDsHk;
    TlmFieldDef counter;
    counter.name = "cmd_accepted_counter";
    counter.type = FieldType::U8;
    counter.size = 1;
    hk.fields.push_back(counter);
    TelemetryCatalog c;
    c.add(hk);
    for (auto& p : loadTlmFile(kSourceDir + "/config/telemetry/evs.json")) {
        c.add(std::move(p));
    }
    return c;
}

// A DS app on the "FSW" side: ds.hk every 10 ms, its counter = commands sent so far.
struct Fixture {
    CommandCatalog commands;
    std::atomic<int> sent{0};
    Pipeline pipeline{std::make_unique<CcsdsV1Format>(Endian::Little), {},
                      std::make_unique<CountingTransport>(sent)};
    TelemetryCatalog tlm = telemetry();
    TlmHistory history{tlm, Endian::Little};
    std::atomic<bool> feeding{false};
    std::thread feeder;
    test::TempDir dir;

    Fixture() { commands.add(test::dsApp()); }
    ~Fixture()
    {
        feeding = false;
        if (feeder.joinable()) {
            feeder.join();
        }
    }

    void startFeeding()
    {
        feeding = true;
        feeder = std::thread([this] {
            while (feeding) {
                history.ingest(test::tlmPacket(kDsHk, 0, {static_cast<std::uint8_t>(sent.load())}, 0, 0,
                                               Endian::Little));
                std::this_thread::sleep_for(10ms);
            }
        });
    }

    ScriptTarget target()
    {
        return ScriptTarget{commands, Endian::Little, "fsw", pipeline, nullptr, nullptr, tlm, &history, "", false};
    }

    static std::vector<ScriptLine> lines(const std::vector<std::string>& texts)
    {
        std::vector<ScriptLine> out;
        for (std::size_t i = 0; i < texts.size(); ++i) {
            out.push_back({texts[i], "t.mcs:" + std::to_string(i + 1)});
        }
        return out;
    }

    // Waits (up to 5 s) for the runner's script to end.
    static bool finished(const ScriptRunner& runner)
    {
        const auto end = std::chrono::steady_clock::now() + 5s;
        while (runner.status().running && std::chrono::steady_clock::now() < end) {
            std::this_thread::sleep_for(10ms);
        }
        return !runner.status().running;
    }
};

bool contains(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

std::string joined(const std::vector<std::string>& lines)
{
    std::string s;
    for (const auto& l : lines) {
        s += l + "\n";
    }
    return s;
}

} // namespace

// ---- runScript / checkScript ----------------------------------------------------------------

TEST(RunScript, PassingScript)
{
    Fixture f;
    f.startFeeding();
    std::ostringstream out;
    std::vector<std::size_t> progress;
    const int result = runScript(f.target(),
                                 Fixture::lines({"timeout 2", "test T1 \"noop\" req R1",
                                                 "save n ds.hk cmd_accepted_counter", "fsw ds noop",
                                                 "expect ds.hk cmd_accepted_counter == $n+1", "end"}),
                                 out, nullptr, [&](std::size_t i) { progress.push_back(i); });
    EXPECT_EQ(result, 0) << out.str();
    EXPECT_TRUE(contains(out.str(), "sent fsw ds noop -> fake://")) << out.str();
    EXPECT_TRUE(contains(out.str(), "=== T1 PASS  (1 check(s))")) << out.str();
    EXPECT_TRUE(contains(out.str(), "result: PASS  tests: 1 passed, 0 failed")) << out.str();
    EXPECT_EQ(progress, (std::vector<std::size_t>{0, 1, 2, 3, 4, 5}));
    EXPECT_EQ(f.sent.load(), 1);
}

TEST(RunScript, FailuresCarryTheLocation)
{
    Fixture f;
    f.startFeeding();
    std::ostringstream out;
    const int result = runScript(f.target(),
                                 Fixture::lines({"fsw ds noop", "fsw ds set_app_state $x",
                                                 "expect ds.hk cmd_accepted_counter == 9 within 0.1"}),
                                 out);
    EXPECT_EQ(result, 1);
    EXPECT_TRUE(contains(out.str(), "t.mcs:2: error: undefined variable $x")) << out.str();
    EXPECT_TRUE(contains(out.str(), "t.mcs:3: FAIL expect ds.hk cmd_accepted_counter == 9 within 0.1 s"))
        << out.str();
    EXPECT_TRUE(contains(out.str(), "result: FAIL  checks: 0 passed, 1 failed; commands sent: 1; errors: 1"))
        << out.str();
}

TEST(CheckScript, ReportsEveryErrorWithItsLocation)
{
    Fixture f;
    std::ostringstream out;
    const std::size_t errors = checkScript(
        f.target(), Fixture::lines({"fsw ds noop", "fsw ds nop", "expect ds.hk nope == 1", "test A"}), "t.mcs",
        out);
    EXPECT_EQ(errors, 3U);
    EXPECT_TRUE(contains(out.str(), "t.mcs:2: error: unknown command: fsw ds nop")) << out.str();
    EXPECT_TRUE(contains(out.str(), "t.mcs:3: error: expect: ds.hk has no field 'nope'")) << out.str();
    EXPECT_TRUE(contains(out.str(), "t.mcs: error: test: test A has no 'end'")) << out.str();
    EXPECT_EQ(f.sent.load(), 0);
}

// ---- ScriptRunner ---------------------------------------------------------------------------

TEST(ScriptRunner, ACheckErrorStartsNothing)
{
    Fixture f;
    ScriptRunner runner(f.target());
    std::ostringstream out;
    const auto path = f.dir.write("typo.mcs", "fsw ds noop\n# comment\nfsw ds nop\n").string();
    EXPECT_FALSE(runner.start(path, out));
    EXPECT_TRUE(contains(out.str(), path + ":3: error: unknown command: fsw ds nop")) << out.str();
    EXPECT_TRUE(contains(out.str(), path + ": not run: 1 error(s)")) << out.str();
    EXPECT_FALSE(runner.status().running);
    EXPECT_FALSE(runner.status().result);
    EXPECT_EQ(f.sent.load(), 0);

    std::ostringstream missing;
    EXPECT_FALSE(runner.start(f.dir.path().string() + "/none.mcs", missing));
    EXPECT_TRUE(contains(missing.str(), "error: run: cannot open script file"));
}

TEST(ScriptRunner, RunsInTheBackgroundAndCollectsTheOutput)
{
    Fixture f;
    f.startFeeding();
    ScriptRunner runner(f.target());
    std::atomic<int> changes{0};
    runner.setOnChange([&] { ++changes; });
    std::ostringstream out;
    const auto path = f.dir.write("ok.mcs", "fsw ds noop\nexpect ds.hk cmd_accepted_counter == 1 within 2\n").string();
    ASSERT_TRUE(runner.start(path, out)) << out.str();
    EXPECT_TRUE(contains(out.str(), "running " + path + ": 2 line(s)"));
    ASSERT_TRUE(Fixture::finished(runner));

    const ScriptRunner::Status s = runner.status();
    EXPECT_EQ(s.name, "ok.mcs");
    EXPECT_EQ(s.line, 2U);
    EXPECT_EQ(s.lines, 2U);
    ASSERT_TRUE(s.result);
    EXPECT_EQ(*s.result, 0);
    const std::vector<std::string> lines = runner.takeOutput();
    ASSERT_EQ(lines.size(), 3U) << joined(lines);
    EXPECT_TRUE(contains(lines[0], "sent fsw ds noop"));
    EXPECT_TRUE(contains(lines[1], "PASS expect ds.hk cmd_accepted_counter == 1"));
    EXPECT_TRUE(contains(lines[2], "result: PASS"));
    EXPECT_TRUE(runner.takeOutput().empty());
    EXPECT_GE(changes.load(), 3);
}

TEST(ScriptRunner, StopEndsAWait)
{
    Fixture f;
    ScriptRunner runner(f.target());
    std::ostringstream out;
    ASSERT_TRUE(runner.start(f.dir.write("wait.mcs", "wait 30\nfsw ds noop\n").string(), out));
    std::this_thread::sleep_for(100ms);
    const auto start = std::chrono::steady_clock::now();
    runner.stop();
    runner.join();
    EXPECT_LT(std::chrono::steady_clock::now() - start, 1s);
    ASSERT_TRUE(runner.status().result);
    EXPECT_EQ(*runner.status().result, 1);
    EXPECT_TRUE(contains(joined(runner.takeOutput()), "stopped at "));
    EXPECT_EQ(f.sent.load(), 0);   // the line after the wait never ran
}

TEST(ScriptRunner, StopEndsAWaitingCheck)
{
    Fixture f;
    f.startFeeding();
    ScriptRunner runner(f.target());
    std::ostringstream out;
    ASSERT_TRUE(runner.start(f.dir.write("x.mcs", "expect ds.hk cmd_accepted_counter == 99 within 30\n").string(),
                             out));
    std::this_thread::sleep_for(100ms);
    const auto start = std::chrono::steady_clock::now();
    runner.stop();
    runner.join();
    EXPECT_LT(std::chrono::steady_clock::now() - start, 1s);
    const std::string output = joined(runner.takeOutput());
    EXPECT_TRUE(contains(output, "FAIL expect ds.hk cmd_accepted_counter == 99 within 30 s  (stopped)")) << output;
    EXPECT_TRUE(contains(output, "stopped\n")) << output;
    EXPECT_EQ(*runner.status().result, 1);
}

TEST(ScriptRunner, StoppingInsideATestFailsIt)
{
    Fixture f;
    ScriptRunner runner(f.target());
    std::ostringstream out;
    ASSERT_TRUE(runner.start(f.dir.write("t.mcs", "test T1\nwait 30\nend\n").string(), out));
    std::this_thread::sleep_for(100ms);
    runner.stop();
    runner.join();
    const std::string output = joined(runner.takeOutput());
    EXPECT_TRUE(contains(output, "=== T1 FAIL  (0 check(s), stopped)")) << output;
    EXPECT_TRUE(contains(output, "result: FAIL  tests: 0 passed, 1 failed")) << output;
    EXPECT_FALSE(contains(output, "has no 'end'")) << output;
}

TEST(ScriptRunner, OneScriptAtATime)
{
    Fixture f;
    ScriptRunner runner(f.target());
    std::ostringstream out;
    ASSERT_TRUE(runner.start(f.dir.write("a.mcs", "wait 30\n").string(), out));
    std::ostringstream second;
    EXPECT_FALSE(runner.start(f.dir.write("b.mcs", "fsw ds noop\n").string(), second));
    EXPECT_TRUE(contains(second.str(), "error: run: a.mcs is still running ('run stop' ends it)"));
    runner.stop();
    runner.join();
    // Once it has ended, the next one starts.
    EXPECT_TRUE(runner.start(f.dir.path().string() + "/b.mcs", out));
    EXPECT_TRUE(Fixture::finished(runner));
    EXPECT_EQ(f.sent.load(), 1);
}
