#include "config/tlm_catalog_loader.hpp"
#include "core/errors.hpp"
#include "telemetry/tlm_catalog.hpp"
#include "unit/test_helpers.hpp"
#include "verify/condition.hpp"
#include "verify/tlm_history.hpp"
#include "verify/variables.hpp"
#include "verify/verify_service.hpp"

#include <gtest/gtest.h>

#include <sstream>
#include <thread>

using namespace mcs;
using namespace mcs::test;
using namespace std::chrono_literals;

namespace {

const std::string kSourceDir = mcs_SOURCE_DIR;
constexpr std::uint16_t kDsHk = 0x0948;

TlmFieldDef field(std::string name, FieldType type, std::size_t size = 0)
{
    TlmFieldDef f;
    f.name = std::move(name);
    f.type = type;
    f.size = size != 0 ? size : naturalSize(type);
    return f;
}

// ds.hk for these tests: u8 cmd_accepted_counter, u8 pad, u16 app_state (enum),
// f32 rate, f64 temp, i16 offset, string[8] file
TlmPacketDef dsHk()
{
    TlmPacketDef p;
    p.app = "ds";
    p.name = "hk";
    p.mid = kDsHk;
    p.fields.push_back(field("cmd_accepted_counter", FieldType::U8));
    TlmFieldDef pad;
    pad.type = FieldType::Padding;
    pad.size = 1;
    p.fields.push_back(pad);
    auto state = field("app_state", FieldType::U16);
    state.enumValues = {{"disable", 0}, {"enable", 1}};
    p.fields.push_back(state);
    p.fields.push_back(field("rate", FieldType::F32));
    p.fields.push_back(field("temp", FieldType::F64));
    p.fields.push_back(field("offset", FieldType::I16));
    p.fields.push_back(field("file", FieldType::String, 8));
    return p;
}

Bytes dsHkPayload(std::uint8_t accepted, std::uint16_t state = 1, float rate = 90.01f, double temp = 21.5,
                  std::int16_t offset = -3, const std::string& file = "a.dat")
{
    Bytes p{accepted, 0, static_cast<std::uint8_t>(state), static_cast<std::uint8_t>(state >> 8)};
    const auto put = [&p](const void* v, std::size_t n) {
        const auto* b = static_cast<const std::uint8_t*>(v);
        p.insert(p.end(), b, b + n);
    };
    put(&rate, 4);
    put(&temp, 8);
    put(&offset, 2);
    Bytes name(8, 0);
    std::copy(file.begin(), file.end(), name.begin());
    p.insert(p.end(), name.begin(), name.end());
    return p;
}

TelemetryCatalog catalog()
{
    TelemetryCatalog c;
    c.add(dsHk());
    for (auto& p : loadTlmFile(kSourceDir + "/config/telemetry/evs.json")) {
        c.add(std::move(p));
    }
    return c;
}

const TlmFieldDef& fieldOf(const TelemetryCatalog& c, const std::string& name)
{
    return *c.find(kDsHk)->findField(name);
}

DecodedValue decoded(const TelemetryCatalog& c, const Bytes& payload, const std::string& name)
{
    for (auto& v : decodeTelemetry(*c.find(kDsHk), payload, Endian::Little)) {
        if (v.name == name) {
            return v;
        }
    }
    return {};
}

Bytes event(const std::string& app, std::uint16_t id, std::uint16_t type, const std::string& text)
{
    return tlmPacket(0x0808, 0, evsLongEventPayload(app, id, type, text), 0, 0, Endian::Little);
}

Bytes hk(std::uint8_t accepted, std::uint16_t state = 1)
{
    return tlmPacket(kDsHk, 0, dsHkPayload(accepted, state), 0, 0, Endian::Little);
}

// A VerifyService on a real TlmHistory, with the "last command" time under test control.
struct Fixture {
    TelemetryCatalog cat = catalog();
    TlmHistory history{cat, Endian::Little};
    VariableTable vars;
    std::optional<TlmTime> lastCommand;
    std::ostringstream out;
    VerifyService verify{cat, &history, "", vars, [this] { return lastCommand; }, false};

    Fixture() { verify.setTimeout({"0.2"}, out); out.str(""); }

    // A command "sent" now; packets ingested after this count.
    TlmTime commandSent()
    {
        lastCommand = TlmClock::now();
        return *lastCommand;
    }

    std::string take()
    {
        std::string s = out.str();
        out.str("");
        return s;
    }
};

bool contains(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

} // namespace

// ---- Condition -------------------------------------------------------------------------

TEST(Condition, IntegersEnumsAndOperators)
{
    const TelemetryCatalog c = catalog();
    const DecodedValue five = decoded(c, dsHkPayload(5), "cmd_accepted_counter");
    const auto counter = [&](const char* op, const char* v) {
        return Condition::parse(fieldOf(c, "cmd_accepted_counter"), op, v).matches(five);
    };
    EXPECT_TRUE(counter("==", "5"));
    EXPECT_TRUE(counter("==", "0x05"));
    EXPECT_FALSE(counter("!=", "5"));
    EXPECT_TRUE(counter("<", "6"));
    EXPECT_TRUE(counter("<=", "5"));
    EXPECT_FALSE(counter(">", "5"));
    EXPECT_TRUE(counter(">=", "5"));

    const DecodedValue enabled = decoded(c, dsHkPayload(0, 1), "app_state");
    EXPECT_TRUE(Condition::parse(fieldOf(c, "app_state"), "==", "enable").matches(enabled));
    EXPECT_TRUE(Condition::parse(fieldOf(c, "app_state"), "==", "ENABLE").matches(enabled));
    EXPECT_TRUE(Condition::parse(fieldOf(c, "app_state"), "==", "1").matches(enabled));
    EXPECT_FALSE(Condition::parse(fieldOf(c, "app_state"), "==", "disable").matches(enabled));
    EXPECT_EQ(Condition::parse(fieldOf(c, "app_state"), "==", "Enable").describe(), "== enable");

    const DecodedValue minus3 = decoded(c, dsHkPayload(0), "offset");
    EXPECT_TRUE(Condition::parse(fieldOf(c, "offset"), "==", "-3").matches(minus3));
    EXPECT_TRUE(Condition::parse(fieldOf(c, "offset"), "<", "0").matches(minus3));
}

TEST(Condition, FloatsAndStrings)
{
    const TelemetryCatalog c = catalog();
    const Bytes payload = dsHkPayload(0);
    // f32 fields compare at f32 precision: 90.01 is not exact in binary.
    EXPECT_TRUE(Condition::parse(fieldOf(c, "rate"), "==", "90.01").matches(decoded(c, payload, "rate")));
    EXPECT_TRUE(Condition::parse(fieldOf(c, "rate"), ">", "90").matches(decoded(c, payload, "rate")));
    EXPECT_TRUE(Condition::parse(fieldOf(c, "temp"), "==", "21.5").matches(decoded(c, payload, "temp")));
    EXPECT_TRUE(Condition::parse(fieldOf(c, "temp"), "<=", "22").matches(decoded(c, payload, "temp")));

    EXPECT_TRUE(Condition::parse(fieldOf(c, "file"), "==", "a.dat").matches(decoded(c, payload, "file")));
    EXPECT_TRUE(Condition::parse(fieldOf(c, "file"), "!=", "b.dat").matches(decoded(c, payload, "file")));
    EXPECT_EQ(Condition::parse(fieldOf(c, "file"), "==", "a b").describe(), "== \"a b\"");
}

TEST(Condition, RejectsBadOperatorsAndValues)
{
    const TelemetryCatalog c = catalog();
    const auto error = [&](const char* name, const char* op, const char* value) -> std::string {
        try {
            Condition::parse(fieldOf(c, name), op, value);
        } catch (const ParseError& e) {
            return e.what();
        }
        return "";
    };
    EXPECT_TRUE(contains(error("cmd_accepted_counter", "=", "1"), "unknown operator '='"));
    EXPECT_TRUE(contains(error("cmd_accepted_counter", "==", "-1"), "not an unsigned integer"));
    EXPECT_TRUE(contains(error("cmd_accepted_counter", "==", "1.5"), "invalid value '1.5'"));
    EXPECT_TRUE(contains(error("app_state", "==", "maybe"), "not a number or one of the field's names"));
    EXPECT_TRUE(contains(error("offset", "==", "x"), "not an integer"));
    EXPECT_TRUE(contains(error("rate", "==", "fast"), "not a number"));
    EXPECT_TRUE(contains(error("file", "<", "a"), "only == and !="));
    // A value from a variable is not known in the check pass: only the operator is checked.
    EXPECT_NO_THROW(Condition::parse(fieldOf(c, "app_state"), "==", "maybe", false));
    EXPECT_THROW(Condition::parse(fieldOf(c, "app_state"), "=~", "1", false), ParseError);
}

TEST(Condition, MissingValueNeverMatches)
{
    const TelemetryCatalog c = catalog();
    DecodedValue missing;
    missing.missing = true;
    EXPECT_FALSE(Condition::parse(fieldOf(c, "cmd_accepted_counter"), "!=", "1").matches(missing));
}

// ---- variables -------------------------------------------------------------------------

TEST(Variables, ExpandValuesAndArithmetic)
{
    VariableTable vars;
    vars.set("n", {"5", 5, std::nullopt});
    vars.set("t", {"21.5", std::nullopt, 21.5});
    vars.set("name", {"my file", std::nullopt, std::nullopt});
    vars.set("state", {"enable", 1, std::nullopt});

    EXPECT_EQ(vars.expand("expect ds.hk c == $n"), "expect ds.hk c == 5");
    EXPECT_EQ(vars.expand("expect ds.hk c == $n+1 within 5"), "expect ds.hk c == 6 within 5");
    EXPECT_EQ(vars.expand("$n-7"), "-2");
    EXPECT_EQ(vars.expand("$t+0.5 $t-1"), "22 20.5");
    EXPECT_EQ(vars.expand("== $state"), "== 1");
    EXPECT_EQ(vars.expand("== $name"), "== \"my file\"");
    EXPECT_EQ(vars.expand("cost $$5 for $n"), "cost $5 for 5");
    EXPECT_EQ(vars.expand("$n+x"), "5+x");   // not arithmetic: left as text
    EXPECT_EQ(vars.expand("no variables"), "no variables");
}

TEST(Variables, Errors)
{
    VariableTable vars;
    vars.set("name", {"abc", std::nullopt, std::nullopt});
    vars.set("n", {"5", 5, std::nullopt});
    const auto error = [&](const char* line) -> std::string {
        try {
            vars.expand(line);
        } catch (const ParseError& e) {
            return e.what();
        }
        return "";
    };
    EXPECT_TRUE(contains(error("== $m"), "undefined variable $m (set it with 'save m"));
    EXPECT_TRUE(contains(error("== $"), "must be followed by a variable name"));
    EXPECT_TRUE(contains(error("== $1"), "must be followed by a variable name"));
    EXPECT_TRUE(contains(error("== $name+1"), "is text ('abc'), not a number"));
    EXPECT_TRUE(contains(error("== $n+1.5"), "is an integer"));
}

TEST(Variables, ReferencedAndNames)
{
    EXPECT_EQ(VariableTable::referenced("save n x y"), std::vector<std::string>{});
    EXPECT_EQ(VariableTable::referenced("a $n+1 b $$ c $m"), (std::vector<std::string>{"n", "m"}));
    EXPECT_THROW(VariableTable::referenced("a $ b"), ParseError);
    EXPECT_TRUE(VariableTable::isName("cmd_count2"));
    EXPECT_TRUE(VariableTable::isName("_x"));
    EXPECT_FALSE(VariableTable::isName("2x"));
    EXPECT_FALSE(VariableTable::isName("a-b"));
    EXPECT_FALSE(VariableTable::isName(""));
}

TEST(Variables, FromDecodedValues)
{
    const TelemetryCatalog c = catalog();
    const Bytes payload = dsHkPayload(7, 1);
    EXPECT_EQ(variableValue(decoded(c, payload, "cmd_accepted_counter")).integer, 7);
    EXPECT_EQ(variableValue(decoded(c, payload, "app_state")).integer, 1);
    EXPECT_EQ(variableValue(decoded(c, payload, "temp")).number, 21.5);
    EXPECT_EQ(variableValue(decoded(c, payload, "file")).text, "a.dat");
}

// ---- durations -------------------------------------------------------------------------

TEST(Durations, ParseAndFormat)
{
    EXPECT_EQ(parseDuration("2"), 2000ms);
    EXPECT_EQ(parseDuration("0.5"), 500ms);
    EXPECT_EQ(parseDuration("2s"), 2000ms);
    EXPECT_EQ(parseDuration("250ms"), 250ms);
    EXPECT_EQ(parseDuration("0"), 0ms);
    EXPECT_THROW(parseDuration("-1"), ParseError);
    EXPECT_THROW(parseDuration("soon"), ParseError);
    EXPECT_THROW(parseDuration("100000"), ParseError);
    EXPECT_EQ(formatDuration(5000ms), "5 s");
    EXPECT_EQ(formatDuration(500ms), "0.5 s");
    EXPECT_EQ(formatDuration(1250ms), "1.25 s");
}

// ---- history -----------------------------------------------------------------------------

TEST(TlmHistory, KeepsSamplesAndEvents)
{
    const TelemetryCatalog c = catalog();
    TlmHistory h(c, Endian::Little);
    for (int i = 0; i < 70; ++i) {
        h.ingest(hk(static_cast<std::uint8_t>(i)));
    }
    h.ingest(event("DS", 1, 2, "No-op"));
    h.ingest(tlmPacket(0x0899, 0, Bytes(10, 0)));   // not in the catalog: ignored
    h.ingest({0x08, 0x00});                          // malformed: ignored

    std::size_t samples = 0;
    std::string first;
    std::string app;
    EXPECT_TRUE(h.waitUntil(std::chrono::steady_clock::now(), [&](const TlmHistory::View& v) {
        samples = v.samples(kDsHk).size();
        first = v.samples(kDsHk).front().values[0].text;
        app = v.events().empty() ? "" : v.events().back().app;
        return v.samples(0x0899).empty();
    }));
    EXPECT_EQ(samples, TlmHistory::kSamplesPerMid);
    EXPECT_EQ(first, "6");   // the oldest 6 were dropped
    EXPECT_EQ(app, "DS");
}

TEST(TlmHistory, WakesAWaitingCheck)
{
    const TelemetryCatalog c = catalog();
    TlmHistory h(c, Endian::Little);
    std::thread feeder([&] {
        std::this_thread::sleep_for(50ms);
        h.ingest(hk(1));
    });
    const auto start = std::chrono::steady_clock::now();
    const bool ok = h.waitUntil(start + 5s, [](const TlmHistory::View& v) { return !v.samples(kDsHk).empty(); });
    feeder.join();
    EXPECT_TRUE(ok);
    EXPECT_LT(std::chrono::steady_clock::now() - start, 2s);

    EXPECT_FALSE(h.waitUntil(std::chrono::steady_clock::now() + 20ms,
                             [](const TlmHistory::View& v) { return v.samples(kDsHk).size() > 1; }));
}

// ---- expect ------------------------------------------------------------------------------

TEST(VerifyExpect, PassesOnAPacketAfterTheCommand)
{
    Fixture f;
    const TlmTime sent = f.commandSent();
    f.history.ingest(hk(4), sent - 1s);       // before the command: ignored
    f.history.ingest(hk(5), sent + 300ms);
    EXPECT_TRUE(f.verify.expect({"ds.hk", "cmd_accepted_counter", "==", "5"}, f.out));
    EXPECT_EQ(f.take(), "PASS expect ds.hk cmd_accepted_counter == 5  (got 5 after 300 ms)\n");
    EXPECT_TRUE(f.verify.expect({"0x0948", "app_state", "==", "enable", "within", "1"}, f.out));
    EXPECT_TRUE(f.verify.expect({"ds.hk"}, f.out));
    EXPECT_EQ(f.verify.counts().checksPassed, 3U);
}

TEST(VerifyExpect, IgnoresTelemetryFromBeforeTheCommand)
{
    Fixture f;
    const TlmTime sent = f.commandSent();
    f.history.ingest(hk(5), sent - 100ms);
    EXPECT_FALSE(f.verify.expect({"ds.hk", "cmd_accepted_counter", "==", "5"}, f.out));
    EXPECT_EQ(f.take(), "FAIL expect ds.hk cmd_accepted_counter == 5 within 0.2 s  (no ds.hk packet since the last command)\n");
}

TEST(VerifyExpect, ReportsTheLastValueOnTimeout)
{
    Fixture f;
    const TlmTime sent = f.commandSent();
    f.history.ingest(hk(3), sent + 10ms);
    f.history.ingest(hk(4), sent + 20ms);
    EXPECT_FALSE(f.verify.expect({"ds.hk", "cmd_accepted_counter", ">=", "5", "within", "0.05"}, f.out));
    EXPECT_EQ(f.take(), "FAIL expect ds.hk cmd_accepted_counter >= 5 within 0.05 s  "
                        "(last 4 of 2 packet(s) since the last command)\n");
    EXPECT_EQ(f.verify.counts().checksFailed, 1U);
    EXPECT_EQ(f.verify.failures(), 1U);
}

TEST(VerifyExpect, PassesWhenThePacketArrivesLater)
{
    Fixture f;
    f.commandSent();
    std::thread feeder([&] {
        std::this_thread::sleep_for(50ms);
        f.history.ingest(hk(1));
    });
    EXPECT_TRUE(f.verify.expect({"ds.hk", "cmd_accepted_counter", "==", "1", "within", "3"}, f.out));
    feeder.join();
}

TEST(VerifyExpect, BeforeAnyCommandTheWindowIsTheStart)
{
    Fixture f;
    f.history.ingest(hk(1));
    EXPECT_TRUE(f.verify.expect({"ds.hk", "cmd_accepted_counter", "==", "1"}, f.out));
    EXPECT_FALSE(f.verify.expect({"ds.hk", "cmd_accepted_counter", "==", "2"}, f.out));
    EXPECT_TRUE(contains(f.take(), "since the start"));
}

TEST(VerifyExpect, BadWordsAreErrors)
{
    Fixture f;
    const auto error = [&](const VerifyService::Args& args) {
        EXPECT_FALSE(f.verify.expect(args, f.out));
        return f.take();
    };
    EXPECT_TRUE(contains(error({}), "error: expect: usage: expect <packet>"));
    EXPECT_TRUE(contains(error({"ds.hk", "cmd_accepted_counter", "=="}), "usage"));
    EXPECT_TRUE(contains(error({"ds.hc"}), "unknown telemetry packet 'ds.hc' (did you mean ds.hk?)"));
    EXPECT_TRUE(contains(error({"xyz.abc"}), "unknown telemetry packet 'xyz.abc' (a name like ds.hk"));
    EXPECT_TRUE(contains(error({"ds.hk", "nope", "==", "1"}), "ds.hk has no field 'nope'\n"));
    EXPECT_TRUE(contains(error({"ds.hk", "cmd_acepted_counter", "==", "1"}),
                         "has no field 'cmd_acepted_counter' (did you mean cmd_accepted_counter?)"));
    EXPECT_TRUE(contains(error({"ds.hk", "app_state", "==", "maybe"}), "invalid value 'maybe'"));
    EXPECT_TRUE(contains(error({"ds.hk", "within", "x"}), "invalid duration 'x'"));
    EXPECT_EQ(f.verify.counts().errors, 8U);
    EXPECT_EQ(f.verify.counts().checksFailed, 0U);
}

TEST(VerifyExpect, WithoutTelemetryEveryCheckFails)
{
    const TelemetryCatalog c = catalog();
    VariableTable vars;
    VerifyService v(c, nullptr, "udp port 5011: address in use", vars, [] { return std::nullopt; }, false);
    std::ostringstream out;
    EXPECT_FALSE(v.expect({"ds.hk"}, out));
    EXPECT_EQ(out.str(), "error: expect: telemetry is not received: udp port 5011: address in use\n");

    const TelemetryCatalog none;
    VerifyService noCatalog(none, nullptr, "target 'x' has no \"telemetry\" block", vars,
                            [] { return std::nullopt; }, false);
    std::ostringstream out2;
    EXPECT_FALSE(noCatalog.expectEvent({"DS"}, out2));
    EXPECT_TRUE(contains(out2.str(), "target 'x' has no \"telemetry\" block"));
}

// ---- events ------------------------------------------------------------------------------

TEST(VerifyEvents, ExpectEventFilters)
{
    Fixture f;
    const TlmTime sent = f.commandSent();
    f.history.ingest(event("DS", 1, 2, "Old"), sent - 1s);
    f.history.ingest(event("DS", 7, 3, "Invalid command length"), sent + 100ms);
    f.history.ingest(event("DS", 1, 2, "No-op command, Version 2.5"), sent + 400ms);

    EXPECT_TRUE(f.verify.expectEvent({"DS", "id", "1", "type", "INFO"}, f.out));
    EXPECT_EQ(f.take(), "PASS expect-event DS id 1 type INFO  (after 400 ms: \"No-op command, Version 2.5\")\n");
    EXPECT_TRUE(f.verify.expectEvent({"ds", "text", "no-op"}, f.out));   // case-insensitive
    EXPECT_TRUE(f.verify.expectEvent({"DS", "type", "error"}, f.out));
    EXPECT_TRUE(f.verify.expectEvent({"DS", "type", "3"}, f.out));
    f.take();
    EXPECT_FALSE(f.verify.expectEvent({"DS", "id", "9", "within", "0.05"}, f.out));
    EXPECT_EQ(f.take(), "FAIL expect-event DS id 9 within 0.05 s  (none of 2 event(s) since the last command)\n");
    EXPECT_FALSE(f.verify.expectEvent({"CI", "within", "0"}, f.out));
}

TEST(VerifyEvents, ExpectNoEvent)
{
    Fixture f;
    const TlmTime sent = f.commandSent();
    f.history.ingest(event("DS", 7, 3, "Old error"), sent - 1s);
    EXPECT_TRUE(f.verify.expectNoEvent({"DS", "type", "ERROR", "for", "0.05"}, f.out));
    EXPECT_EQ(f.take(), "PASS expect-no-event DS type ERROR for 0.05 s\n");

    f.history.ingest(event("DS", 7, 3, "Invalid command length"), sent + 10ms);
    EXPECT_FALSE(f.verify.expectNoEvent({"DS", "type", "ERROR", "for", "5"}, f.out));   // fails at once
    EXPECT_EQ(f.take(), "FAIL expect-no-event DS type ERROR for 5 s  "
                        "(id 7 ERROR after 10 ms: \"Invalid command length\")\n");
}

TEST(VerifyEvents, BadFilters)
{
    Fixture f;
    const auto error = [&](const VerifyService::Args& args) {
        EXPECT_FALSE(f.verify.expectEvent(args, f.out));
        return f.take();
    };
    EXPECT_TRUE(contains(error({}), "usage"));
    EXPECT_TRUE(contains(error({"DS", "id"}), "usage"));
    EXPECT_TRUE(contains(error({"DS", "id", "x"}), "event id 'x' is not a number"));
    EXPECT_TRUE(contains(error({"DS", "type", "LOUD"}), "unknown event type 'LOUD' (UNDEF, DEBUG, INFO"));
    EXPECT_TRUE(contains(error({"DS", "for", "1"}), "unknown word 'for'"));
}

// ---- save, wait, timeout -------------------------------------------------------------------

TEST(VerifySave, SavesTheLatestValue)
{
    Fixture f;
    f.history.ingest(hk(3));
    f.history.ingest(hk(4));
    EXPECT_TRUE(f.verify.save({"n", "ds.hk", "cmd_accepted_counter"}, f.out));
    EXPECT_EQ(f.take(), "n = 4  (ds.hk cmd_accepted_counter)\n");
    EXPECT_EQ(f.vars.expand("== $n+1"), "== 5");

    EXPECT_FALSE(f.verify.save({"9x", "ds.hk", "cmd_accepted_counter"}, f.out));
    EXPECT_TRUE(contains(f.take(), "'9x' is not a variable name"));
}

TEST(VerifySave, FailsWithoutAPacket)
{
    Fixture f;
    EXPECT_FALSE(f.verify.save({"n", "ds.hk", "cmd_accepted_counter", "within", "0.02"}, f.out));
    EXPECT_EQ(f.take(), "FAIL save n ds.hk cmd_accepted_counter within 0.02 s  (no ds.hk packet received)\n");
    EXPECT_EQ(f.vars.find("n"), nullptr);
}

TEST(VerifyWait, SleepsAndSetsTheTimeout)
{
    Fixture f;
    std::vector<std::chrono::milliseconds> slept;
    f.verify.setSleep([&](std::chrono::milliseconds d) { slept.push_back(d); });
    EXPECT_TRUE(f.verify.wait({"1.5"}, f.out));
    EXPECT_FALSE(f.verify.wait({}, f.out));
    EXPECT_EQ(slept, std::vector<std::chrono::milliseconds>{1500ms});
    f.take();
    EXPECT_TRUE(f.verify.setTimeout({"10"}, f.out));
    EXPECT_EQ(f.take(), "timeout: 10 s\n");
    EXPECT_EQ(f.verify.timeout(), 10000ms);
}

// ---- tests ---------------------------------------------------------------------------------

TEST(VerifyTests, CountsChecksPerTest)
{
    Fixture f;
    f.history.ingest(hk(1));
    EXPECT_TRUE(f.verify.beginTest({"DS-001", "NOOP works", "req", "DS-R1,DS-R2"}, f.out));
    EXPECT_TRUE(f.verify.expect({"ds.hk"}, f.out));
    EXPECT_TRUE(f.verify.endTest({}, f.out));
    EXPECT_TRUE(f.verify.beginTest({"DS-002"}, f.out));
    EXPECT_FALSE(f.verify.expect({"ds.hk", "cmd_accepted_counter", "==", "9", "within", "0"}, f.out));
    EXPECT_FALSE(f.verify.endTest({}, f.out));
    EXPECT_TRUE(f.verify.beginTest({"DS-003", "req", "DS-R3"}, f.out));
    f.verify.noteFailure();   // a command of the test failed
    EXPECT_FALSE(f.verify.endTest({}, f.out));

    const std::string out = f.take();
    EXPECT_TRUE(contains(out, "=== test DS-001  NOOP works  [DS-R1, DS-R2]\nPASS expect ds.hk"));
    EXPECT_TRUE(contains(out, "=== DS-001 PASS  (1 check(s))\n"));
    EXPECT_TRUE(contains(out, "=== DS-002 FAIL  (1 of 1 check(s) failed)\n"));
    EXPECT_TRUE(contains(out, "=== test DS-003  [DS-R3]\n=== DS-003 FAIL  (0 check(s), 1 error(s))\n"));
    EXPECT_EQ(f.verify.counts().testsPassed, 1U);
    EXPECT_EQ(f.verify.counts().testsFailed, 2U);

    f.verify.printSummary(f.out, 2, 1);
    EXPECT_EQ(f.take(), "result: FAIL  tests: 1 passed, 2 failed; checks: 1 passed, 1 failed; "
                        "commands sent: 2; errors: 1\n");
}

TEST(VerifyTests, StructureErrors)
{
    Fixture f;
    EXPECT_FALSE(f.verify.endTest({}, f.out));
    EXPECT_TRUE(contains(f.take(), "error: end: 'end' without 'test'"));
    EXPECT_FALSE(f.verify.beginTest({}, f.out));
    EXPECT_TRUE(f.verify.beginTest({"A"}, f.out));
    EXPECT_FALSE(f.verify.beginTest({"B"}, f.out));
    EXPECT_TRUE(contains(f.take(), "error: test: test B starts inside test A: add 'end' before it"));
    EXPECT_FALSE(f.verify.endTest({"x"}, f.out));
    EXPECT_FALSE(f.verify.finish(f.out));
    EXPECT_TRUE(contains(f.take(), "error: test: test A has no 'end'"));
    EXPECT_FALSE(f.verify.beginTest({"A"}, f.out));
    EXPECT_TRUE(contains(f.take(), "test A is defined twice"));
    EXPECT_TRUE(f.verify.finish(f.out));
}

TEST(VerifyTests, SummaryOfAPassingRun)
{
    Fixture f;
    EXPECT_FALSE(f.verify.anyRun());
    f.history.ingest(hk(1));
    f.verify.expect({"ds.hk"}, f.out);
    EXPECT_TRUE(f.verify.anyRun());
    f.take();
    f.verify.printSummary(f.out, 0, 0);
    EXPECT_EQ(f.take(), "result: PASS  checks: 1 passed, 0 failed; commands sent: 0; errors: 0\n");
}

// ---- check only -----------------------------------------------------------------------------

TEST(VerifyCheckOnly, ReadsWordsButNeverWaits)
{
    const TelemetryCatalog c = catalog();
    VariableTable vars;
    VerifyService v(c, nullptr, "not needed", vars, [] { return std::nullopt; }, true);
    v.setSleep([](std::chrono::milliseconds) { FAIL() << "check only must not sleep"; });
    std::ostringstream out;
    const auto start = std::chrono::steady_clock::now();
    EXPECT_TRUE(v.wait({"60"}, out));
    EXPECT_TRUE(v.expect({"ds.hk", "cmd_accepted_counter", "==", "1", "within", "60"}, out));
    EXPECT_TRUE(v.expectEvent({"DS", "id", "1", "within", "60"}, out));
    EXPECT_TRUE(v.expectNoEvent({"DS", "type", "ERROR", "for", "60"}, out));
    EXPECT_TRUE(v.save({"n", "ds.hk", "cmd_accepted_counter"}, out));
    EXPECT_TRUE(v.save({"t", "ds.hk", "temp"}, out));
    EXPECT_TRUE(v.beginTest({"T1"}, out));
    EXPECT_TRUE(v.endTest({}, out));
    EXPECT_LT(std::chrono::steady_clock::now() - start, 1s);
    EXPECT_EQ(out.str(), "");   // nothing printed but errors

    // "save" defines its variable with a placeholder of the field's kind.
    EXPECT_EQ(vars.expand("$n+1 $t+0.5"), "1 0.5");

    EXPECT_FALSE(v.expect({"ds.hk", "app_state", "==", "maybe"}, out));
    v.setValuesKnown(false);
    EXPECT_TRUE(v.expect({"ds.hk", "app_state", "==", "maybe"}, out));
    EXPECT_FALSE(v.expect({"ds.hk", "nope", "==", "1"}, out));   // names are still checked
    EXPECT_EQ(v.counts().errors, 2U);
}
