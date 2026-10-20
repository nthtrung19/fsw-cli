#pragma once

#include "telemetry/tlm_catalog.hpp"
#include "telemetry/tlm_store.hpp"
#include "verify/condition.hpp"
#include "verify/tlm_history.hpp"
#include "verify/variables.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <ostream>
#include <set>
#include <string>
#include <vector>

namespace mcs {

// Script checks. One method per script command, taking the words after it:
//
//   wait <seconds>                       pause
//   timeout <seconds>                    default 'within' of the following checks (5 s)
//   expect <packet> [within <s>]         a packet arrives
//   expect <packet> <field> <op> <value> [within <s>]
//                                        a packet arrives whose field matches (see Condition)
//   expect-event <APP> [id <n>] [type <T>] [text "<substring>"] [within <s>]
//   expect-no-event <APP> [id <n>] [type <T>] [text "<substring>"] [for <s>]
//   save <var> <packet> <field> [within <s>]
//                                        the latest value, for $var in later lines
//   test <ID> ["title"] [req <R1,R2,...>] ... end
//                                        group lines; a test passes if all of them do
//
// <packet> is "ds.hk" or a catalog MID ("0x0948"). Durations are seconds
// ("2", "0.5", "2s") or milliseconds ("500ms").
//
// Checks count what arrived since the last command was sent (or since the
// start, before the first command), so an event that arrives during a "wait"
// after the command still counts and older telemetry does not. "expect" and
// "expect-event" pass on the first match and fail at the deadline;
// "expect-no-event" waits the whole time and fails on a match.
//
// Every method writes one PASS/FAIL line (or an "error: ..." line for bad
// words) and returns false on failure. Failures are counted here, not by the
// caller.
//
// Check only (the pass over a whole script before it runs): the words are
// read and the names resolved against the catalog, nothing waits and no
// telemetry is needed; "save" defines its variable with a placeholder.
class VerifyService {
public:
    using Args = std::vector<std::string>;
    using Sleep = std::function<void(std::chrono::milliseconds)>;

    static constexpr std::chrono::milliseconds kDefaultTimeout{5000};

    struct Counts {
        std::size_t checksPassed = 0;
        std::size_t checksFailed = 0;
        std::size_t testsPassed = 0;
        std::size_t testsFailed = 0;
        std::size_t errors = 0;          // bad words, missing telemetry
    };

    // `history` null: telemetry is not received (`noTelemetry` says why; every
    // check fails with it). `lastCommand`: when the last command was sent.
    VerifyService(const TelemetryCatalog& catalog, const TlmHistory* history, std::string noTelemetry,
                  VariableTable& variables, std::function<std::optional<TlmTime>()> lastCommand,
                  bool checkOnly);

    bool wait(const Args& args, std::ostream& out);
    bool setTimeout(const Args& args, std::ostream& out);
    bool expect(const Args& args, std::ostream& out);
    bool expectEvent(const Args& args, std::ostream& out);
    bool expectNoEvent(const Args& args, std::ostream& out);
    bool save(const Args& args, std::ostream& out);
    bool beginTest(const Args& args, std::ostream& out);
    bool endTest(const Args& args, std::ostream& out);

    // Check only: the next command's words come from variables, whose values
    // are not known yet; check names and syntax, not values.
    void setValuesKnown(bool known) { valuesKnown_ = known; }
    bool valuesKnown() const { return valuesKnown_; }
    bool checkOnly() const { return checkOnly_; }

    // Another line of the script failed (a command, a transfer, ...): it
    // fails the current test.
    void noteFailure();

    // End of the script: a test without "end" is an error. After a stop
    // (`stopped`), an open test just ends as FAIL (stopped).
    bool finish(std::ostream& out, bool stopped = false);

    const Counts& counts() const { return counts_; }

    // Failed checks and errors, for the exit code.
    std::size_t failures() const { return counts_.checksFailed + counts_.errors; }

    // A check or test ran: the batch run prints a summary.
    bool anyRun() const;

    // "result: PASS  tests: 1 passed, 0 failed; checks: ...; commands sent: 2; errors: 0".
    // `otherErrors`: failed lines that are not checks.
    void printSummary(std::ostream& out, std::size_t commandsSent, std::size_t otherErrors) const;

    std::chrono::milliseconds timeout() const { return timeout_; }

    // For tests: replaces std::this_thread::sleep_for in "wait".
    void setSleep(Sleep sleep) { sleep_ = std::move(sleep); }

    // When `*stop` becomes true, a "wait" or a check that is waiting ends
    // within kStopPoll (a check then FAILs with "(stopped)"). Null: no stop.
    static constexpr std::chrono::milliseconds kStopPoll{100};
    void setStop(const std::atomic<bool>* stop) { stop_ = stop; }
    bool stopped() const { return stop_ != nullptr && stop_->load(); }

    // A packet by "app.name" or catalog MID ("0x0948"); null if unknown.
    static const TlmPacketDef* findPacket(const TelemetryCatalog& catalog, const std::string& name);

private:
    struct EventFilter {
        std::string app;
        std::optional<std::int64_t> id;
        std::optional<std::int64_t> type;
        std::string typeName;
        std::string text;
        bool matches(const EventRecord& e) const;
        std::string describe() const;
    };
    struct Test {
        std::string id;
        std::string title;
        std::vector<std::string> requirements;
        std::size_t checksPassed = 0;
        std::size_t checksFailed = 0;
        std::size_t errors = 0;
        bool stopped = false;
    };

    bool error(const std::string& command, const std::string& message, std::ostream& out);
    bool passed(const std::string& line, std::ostream& out);
    bool failed(const std::string& line, std::ostream& out);
    bool needTelemetry(const std::string& command, std::ostream& out);
    const TlmPacketDef* packetOrThrow(const std::string& name) const;
    EventFilter parseEventFilter(const Args& args, const char* durationWord,
                                 std::chrono::milliseconds& duration) const;
    TlmTime windowStart() const;
    // history_->waitUntil, in slices of kStopPoll when a stop flag is set.
    bool waitFor(std::chrono::milliseconds limit, const TlmHistory::Check& check) const;
    std::string windowName() const;

    const TelemetryCatalog& catalog_;
    const TlmHistory* history_;
    std::string noTelemetry_;
    VariableTable& variables_;
    std::function<std::optional<TlmTime>()> lastCommand_;
    bool checkOnly_;
    bool valuesKnown_ = true;
    TlmTime sessionStart_;
    std::chrono::milliseconds timeout_ = kDefaultTimeout;
    Sleep sleep_;
    const std::atomic<bool>* stop_ = nullptr;
    Counts counts_;
    std::optional<Test> test_;
    std::set<std::string> testIds_;
};

// "2" -> 2 s, "0.5" or "0.5s" -> 500 ms, "500ms" -> 500 ms. Throws ParseError
// for anything else, negative values and more than a day.
std::chrono::milliseconds parseDuration(const std::string& text);

// "5 s", "0.5 s", "1.25 s".
std::string formatDuration(std::chrono::milliseconds d);

} // namespace mcs
