#include "verify/verify_service.hpp"

#include "core/errors.hpp"
#include "core/text.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <thread>

namespace mcs {

namespace {

using SteadyClock = std::chrono::steady_clock;

// "after 12 ms", "after 1.3 s": how long after `from` something arrived.
std::string after(TlmTime from, TlmTime t)
{
    const double s = std::max(0.0, std::chrono::duration<double>(t - from).count());
    std::ostringstream out;
    if (s < 1.0) {
        out << "after " << std::llround(s * 1000.0) << " ms";
    } else {
        out << "after " << std::fixed << std::setprecision(1) << s << " s";
    }
    return out.str();
}

std::size_t editDistance(const std::string& a, const std::string& b)
{
    std::vector<std::size_t> row(b.size() + 1);
    for (std::size_t j = 0; j <= b.size(); ++j) {
        row[j] = j;
    }
    for (std::size_t i = 1; i <= a.size(); ++i) {
        std::size_t diagonal = row[0];
        row[0] = i;
        for (std::size_t j = 1; j <= b.size(); ++j) {
            const std::size_t up = row[j];
            row[j] = std::min({row[j] + 1, row[j - 1] + 1, diagonal + (a[i - 1] == b[j - 1] ? 0 : 1)});
            diagonal = up;
        }
    }
    return row[b.size()];
}

// " (did you mean x?)" for the closest candidate within a few typos, else "".
std::string didYouMean(const std::string& wanted, const std::vector<std::string>& candidates)
{
    const std::string w = toLower(wanted);
    std::size_t best = std::max<std::size_t>(2, w.size() / 4) + 1;
    std::string bestName;
    for (const auto& c : candidates) {
        const std::size_t d = editDistance(w, toLower(c));
        if (d < best) {
            best = d;
            bestName = c;
        }
    }
    return bestName.empty() ? "" : " (did you mean " + bestName + "?)";
}

std::size_t visibleIndex(const TlmPacketDef& def, const std::string& fieldName)
{
    std::size_t index = 0;
    for (const auto& f : def.fields) {
        if (!f.visible()) {
            continue;
        }
        if (f.name == fieldName) {
            return index;
        }
        ++index;
    }
    return index;
}

// Removes a trailing "<word> <duration>" pair, if present.
void takeDuration(VerifyService::Args& args, const char* word, std::chrono::milliseconds& duration)
{
    if (args.size() >= 2 && iequals(args[args.size() - 2], word)) {
        duration = parseDuration(args.back());
        args.resize(args.size() - 2);
    }
}

const TlmFieldDef& fieldOrThrow(const TlmPacketDef& def, const std::string& name)
{
    const TlmFieldDef* f = def.findField(name);
    if (f == nullptr) {
        std::vector<std::string> names;
        for (const auto& field : def.fields) {
            if (field.visible()) {
                names.push_back(field.name);
            }
        }
        throw ParseError(def.fullName() + " has no field '" + name + "'" + didYouMean(name, names));
    }
    return *f;
}

// The event_type field of the catalog's EVS event packets (for type names).
const TlmFieldDef* eventTypeField(const TelemetryCatalog& catalog)
{
    for (const auto& p : catalog.packets()) {
        if (p.isEvent) {
            return p.findField("event_type");
        }
    }
    return nullptr;
}

std::string inQuotes(const std::string& text)
{
    return "\"" + text + "\"";
}

} // namespace

std::chrono::milliseconds parseDuration(const std::string& text)
{
    std::string t = toLower(trim(text));
    double scale = 1000.0;
    if (t.size() > 2 && t.compare(t.size() - 2, 2, "ms") == 0) {
        scale = 1.0;
        t.resize(t.size() - 2);
    } else if (t.size() > 1 && t.back() == 's') {
        t.pop_back();
    }
    const auto v = parseDouble(t);
    if (!v || *v < 0 || *v * scale > 86'400'000.0) {
        throw ParseError("invalid duration '" + text + "' (seconds, e.g. 2, 0.5 or 500ms)");
    }
    return std::chrono::milliseconds(std::llround(*v * scale));
}

std::string formatDuration(std::chrono::milliseconds d)
{
    std::ostringstream out;
    if (d.count() % 1000 == 0) {
        out << d.count() / 1000;
    } else {
        out << std::setprecision(6) << static_cast<double>(d.count()) / 1000.0;
    }
    return out.str() + " s";
}

// ---- event filter ------------------------------------------------------------------

bool VerifyService::EventFilter::matches(const EventRecord& e) const
{
    return iequals(e.app, app) && (!id || e.eventId == *id) && (!type || e.eventType == *type)
        && (text.empty() || toLower(e.message).find(toLower(text)) != std::string::npos);
}

std::string VerifyService::EventFilter::describe() const
{
    std::string s = app;
    if (id) {
        s += " id " + std::to_string(*id);
    }
    if (type) {
        s += " type " + typeName;
    }
    if (!text.empty()) {
        s += " text " + inQuotes(text);
    }
    return s;
}

// ---- service --------------------------------------------------------------------------

VerifyService::VerifyService(const TelemetryCatalog& catalog, const TlmHistory* history,
                             std::string noTelemetry, VariableTable& variables,
                             std::function<std::optional<TlmTime>()> lastCommand, bool checkOnly)
    : catalog_(catalog), history_(history), noTelemetry_(std::move(noTelemetry)), variables_(variables),
      lastCommand_(std::move(lastCommand)), checkOnly_(checkOnly), sessionStart_(TlmClock::now()),
      sleep_([](std::chrono::milliseconds d) { std::this_thread::sleep_for(d); })
{
}

const TlmPacketDef* VerifyService::findPacket(const TelemetryCatalog& catalog, const std::string& name)
{
    if (name.size() > 2 && name[0] == '0' && (name[1] == 'x' || name[1] == 'X')) {
        const auto mid = parseUnsigned(name);
        return mid && *mid <= 0xFFFF ? catalog.find(static_cast<std::uint16_t>(*mid)) : nullptr;
    }
    return catalog.findByName(name);
}

const TlmPacketDef* VerifyService::packetOrThrow(const std::string& name) const
{
    if (catalog_.packets().empty()) {
        throw ParseError(noTelemetry_.empty() ? "no telemetry definitions for this target" : noTelemetry_);
    }
    const TlmPacketDef* def = findPacket(catalog_, name);
    if (def == nullptr) {
        std::vector<std::string> names;
        for (const auto& p : catalog_.packets()) {
            names.push_back(p.fullName());
        }
        const std::string hint = didYouMean(name, names);
        throw ParseError("unknown telemetry packet '" + name + "'"
                         + (hint.empty() ? " (a name like ds.hk, or a MID like 0x0948)" : hint));
    }
    return def;
}

bool VerifyService::waitFor(std::chrono::milliseconds limit, const TlmHistory::Check& check) const
{
    const auto deadline = SteadyClock::now() + limit;
    if (stop_ == nullptr) {
        return history_->waitUntil(deadline, check);
    }
    for (;;) {
        if (history_->waitUntil(std::min(deadline, SteadyClock::now() + kStopPoll), check)) {
            return true;
        }
        if (stopped() || SteadyClock::now() >= deadline) {
            return false;
        }
    }
}

TlmTime VerifyService::windowStart() const
{
    const auto last = lastCommand_ ? lastCommand_() : std::nullopt;
    return last ? *last : sessionStart_;
}

std::string VerifyService::windowName() const
{
    const auto last = lastCommand_ ? lastCommand_() : std::nullopt;
    return last ? "the last command" : "the start";
}

bool VerifyService::error(const std::string& command, const std::string& message, std::ostream& out)
{
    ++counts_.errors;
    if (test_) {
        ++test_->errors;
    }
    out << "error: " << command << ": " << message << '\n';
    return false;
}

bool VerifyService::passed(const std::string& line, std::ostream& out)
{
    ++counts_.checksPassed;
    if (test_) {
        ++test_->checksPassed;
    }
    out << "PASS " << line << '\n';
    return true;
}

bool VerifyService::failed(const std::string& line, std::ostream& out)
{
    ++counts_.checksFailed;
    if (test_) {
        ++test_->checksFailed;
    }
    out << "FAIL " << line << '\n';
    return false;
}

bool VerifyService::needTelemetry(const std::string& command, std::ostream& out)
{
    if (history_ != nullptr) {
        return true;
    }
    return error(command, "telemetry is not received: " + noTelemetry_, out);
}

bool VerifyService::wait(const Args& args, std::ostream& out)
{
    try {
        if (args.size() != 1) {
            throw ParseError("usage: wait <seconds>");
        }
        const auto d = parseDuration(args[0]);
        if (checkOnly_) {
            return true;
        }
        if (stop_ == nullptr) {
            sleep_(d);
            return true;
        }
        const auto deadline = SteadyClock::now() + d;
        while (!stopped()) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - SteadyClock::now());
            if (left.count() <= 0) {
                break;
            }
            sleep_(std::min(left, kStopPoll));
        }
        return true;
    } catch (const ParseError& e) {
        return error("wait", e.what(), out);
    }
}

bool VerifyService::setTimeout(const Args& args, std::ostream& out)
{
    try {
        if (args.size() > 1) {
            throw ParseError("usage: timeout [<seconds>]");
        }
        if (args.size() == 1) {
            timeout_ = parseDuration(args[0]);
        }
        if (!checkOnly_) {
            out << "timeout: " << formatDuration(timeout_) << '\n';
        }
        return true;
    } catch (const ParseError& e) {
        return error("timeout", e.what(), out);
    }
}

bool VerifyService::expect(const Args& args, std::ostream& out)
{
    try {
        Args a = args;
        auto within = timeout_;
        takeDuration(a, "within", within);
        if (a.size() != 1 && a.size() != 4) {
            throw ParseError("usage: expect <packet> [<field> <op> <value>] [within <seconds>]");
        }
        const TlmPacketDef* def = packetOrThrow(a[0]);
        std::optional<Condition> condition;
        std::size_t index = 0;
        std::string what = def->fullName();
        if (a.size() == 4) {
            const TlmFieldDef& field = fieldOrThrow(*def, a[1]);
            index = visibleIndex(*def, field.name);
            condition = Condition::parse(field, a[2], a[3], valuesKnown_);
            what += " " + field.name + " " + condition->describe();
        }
        if (checkOnly_ || !needTelemetry("expect", out)) {
            return checkOnly_;
        }

        const TlmTime since = windowStart();
        std::size_t count = 0;
        std::string last;
        std::optional<TlmTime> matchedAt;
        const bool ok = waitFor(within, [&](const TlmHistory::View& v) {
            count = 0;
            for (const TlmSample& s : v.samples(def->mid)) {
                if (s.rxTime < since) {
                    continue;
                }
                ++count;
                if (!condition) {
                    matchedAt = s.rxTime;
                    return true;
                }
                if (index < s.values.size()) {
                    last = s.values[index].text;
                    if (condition->matches(s.values[index])) {
                        matchedAt = s.rxTime;
                        return true;
                    }
                }
            }
            return false;
        });

        const std::string line = "expect " + what;
        if (ok) {
            return passed(line + "  (" + (condition ? "got " + last + " " : "") + after(since, *matchedAt) + ")", out);
        }
        const std::string head = line + " within " + formatDuration(within);
        if (stopped()) {
            return failed(head + "  (stopped)", out);
        }
        if (count == 0) {
            return failed(head + "  (no " + def->fullName() + " packet since " + windowName() + ")", out);
        }
        return failed(head + "  (last " + last + " of " + std::to_string(count) + " packet(s) since "
                          + windowName() + ")",
                      out);
    } catch (const ParseError& e) {
        return error("expect", e.what(), out);
    }
}

VerifyService::EventFilter VerifyService::parseEventFilter(const Args& args, const char* durationWord,
                                                           std::chrono::milliseconds& duration) const
{
    const std::string usage = std::string("usage: <APP> [id <n>] [type <type>] [text \"<text>\"] [")
                            + durationWord + " <seconds>]";
    if (catalog_.packets().empty()) {
        throw ParseError(noTelemetry_.empty() ? "no telemetry definitions for this target" : noTelemetry_);
    }
    const TlmFieldDef* typeField = eventTypeField(catalog_);
    if (typeField == nullptr) {
        throw ParseError("no EVS event packet in the telemetry catalog (evs.json)");
    }
    if (args.empty() || (args.size() % 2) == 0) {
        throw ParseError(usage);
    }
    EventFilter f;
    f.app = args[0];
    for (std::size_t i = 1; i + 1 < args.size(); i += 2) {
        const std::string key = toLower(args[i]);
        const std::string& value = args[i + 1];
        if (key == "id") {
            const auto id = parseSigned(value);
            if (!id) {
                throw ParseError("event id '" + value + "' is not a number");
            }
            f.id = *id;
        } else if (key == "type") {
            for (const auto& e : typeField->enumValues) {
                if (iequals(e.name, value)) {
                    f.type = e.value;
                    f.typeName = e.name;
                }
            }
            if (!f.type) {
                const auto n = parseSigned(value);
                if (!n) {
                    std::string names;
                    for (const auto& e : typeField->enumValues) {
                        names += (names.empty() ? "" : ", ") + e.name;
                    }
                    throw ParseError("unknown event type '" + value + "' (" + names + ")");
                }
                f.type = *n;
                f.typeName = value;
            }
        } else if (key == "text") {
            f.text = value;
        } else if (key == durationWord) {
            duration = parseDuration(value);
        } else {
            throw ParseError("unknown word '" + args[i] + "'; " + usage);
        }
    }
    return f;
}

bool VerifyService::expectEvent(const Args& args, std::ostream& out)
{
    try {
        auto within = timeout_;
        const EventFilter filter = parseEventFilter(args, "within", within);
        if (checkOnly_ || !needTelemetry("expect-event", out)) {
            return checkOnly_;
        }

        const TlmTime since = windowStart();
        std::size_t count = 0;
        std::optional<EventRecord> match;
        const bool ok = waitFor(within, [&](const TlmHistory::View& v) {
            count = 0;
            for (const EventRecord& e : v.events()) {
                if (e.rxTime < since) {
                    continue;
                }
                ++count;
                if (filter.matches(e)) {
                    match = e;
                    return true;
                }
            }
            return false;
        });

        const std::string line = "expect-event " + filter.describe();
        if (ok) {
            return passed(line + "  (" + after(since, match->rxTime) + ": " + inQuotes(match->message) + ")", out);
        }
        if (stopped()) {
            return failed(line + " within " + formatDuration(within) + "  (stopped)", out);
        }
        return failed(line + " within " + formatDuration(within) + "  ("
                          + (count == 0 ? "no event" : "none of " + std::to_string(count) + " event(s)")
                          + " since " + windowName() + ")",
                      out);
    } catch (const ParseError& e) {
        return error("expect-event", e.what(), out);
    }
}

bool VerifyService::expectNoEvent(const Args& args, std::ostream& out)
{
    try {
        auto during = timeout_;
        const EventFilter filter = parseEventFilter(args, "for", during);
        if (checkOnly_ || !needTelemetry("expect-no-event", out)) {
            return checkOnly_;
        }

        const TlmTime since = windowStart();
        std::optional<EventRecord> match;
        const bool found = waitFor(during, [&](const TlmHistory::View& v) {
            for (const EventRecord& e : v.events()) {
                if (e.rxTime >= since && filter.matches(e)) {
                    match = e;
                    return true;
                }
            }
            return false;
        });

        const std::string line = "expect-no-event " + filter.describe() + " for " + formatDuration(during);
        if (!found && stopped()) {
            return failed(line + "  (stopped)", out);
        }
        if (!found) {
            return passed(line, out);
        }
        return failed(line + "  (id " + std::to_string(match->eventId) + " " + match->typeName + " "
                          + after(since, match->rxTime) + ": " + inQuotes(match->message) + ")",
                      out);
    } catch (const ParseError& e) {
        return error("expect-no-event", e.what(), out);
    }
}

bool VerifyService::save(const Args& args, std::ostream& out)
{
    try {
        Args a = args;
        auto within = timeout_;
        takeDuration(a, "within", within);
        if (a.size() != 3) {
            throw ParseError("usage: save <name> <packet> <field> [within <seconds>]");
        }
        if (!VariableTable::isName(a[0])) {
            throw ParseError("'" + a[0] + "' is not a variable name (letters, digits and _)");
        }
        const TlmPacketDef* def = packetOrThrow(a[1]);
        const TlmFieldDef& field = fieldOrThrow(*def, a[2]);
        const std::size_t index = visibleIndex(*def, field.name);
        if (checkOnly_) {
            // A placeholder of the field's kind: lines using it are checked without values.
            VariableValue placeholder{"x", std::nullopt, std::nullopt};
            if (isInteger(field.type)) {
                placeholder = {"0", 0, std::nullopt};
            } else if (isFloat(field.type)) {
                placeholder = {"0", std::nullopt, 0.0};
            }
            variables_.set(a[0], placeholder);
            return true;
        }
        if (!needTelemetry("save", out)) {
            return false;
        }

        std::optional<DecodedValue> value;
        waitFor(within, [&](const TlmHistory::View& v) {
            const auto& samples = v.samples(def->mid);
            if (samples.empty() || index >= samples.back().values.size()) {
                return false;
            }
            value = samples.back().values[index];
            return true;
        });

        const std::string line = "save " + a[0] + " " + def->fullName() + " " + field.name;
        if (!value) {
            return failed(line + " within " + formatDuration(within) + "  ("
                              + (stopped() ? "stopped" : "no " + def->fullName() + " packet received") + ")",
                          out);
        }
        if (value->missing) {
            return failed(line + "  (the last " + def->fullName() + " packet is too short for " + field.name
                              + ")",
                          out);
        }
        variables_.set(a[0], variableValue(*value));
        out << a[0] << " = " << value->text << "  (" << def->fullName() << " " << field.name << ")\n";
        return true;
    } catch (const ParseError& e) {
        return error("save", e.what(), out);
    }
}

bool VerifyService::beginTest(const Args& args, std::ostream& out)
{
    const char* usage = "usage: test <ID> [\"title\"] [req <R1,R2,...>]";
    if (args.empty()) {
        return error("test", usage, out);
    }
    if (test_) {
        return error("test", "test " + args[0] + " starts inside test " + test_->id + ": add 'end' before it",
                     out);
    }
    Test t;
    t.id = args[0];
    std::size_t i = 1;
    if (i < args.size() && !iequals(args[i], "req")) {
        t.title = args[i++];
    }
    if (i < args.size()) {
        if (!iequals(args[i], "req") || i + 1 >= args.size()) {
            return error("test", usage, out);
        }
        for (++i; i < args.size(); ++i) {
            std::stringstream words(args[i]);
            std::string r;
            while (std::getline(words, r, ',')) {
                if (!trim(r).empty()) {
                    t.requirements.push_back(trim(r));
                }
            }
        }
    }
    if (!testIds_.insert(t.id).second) {
        return error("test", "test " + t.id + " is defined twice", out);
    }
    if (!checkOnly_) {
        out << "=== test " << t.id << (t.title.empty() ? "" : "  " + t.title)
            << (t.requirements.empty() ? "" : "  [" + join(t.requirements, ", ") + "]") << '\n';
    }
    test_ = std::move(t);
    return true;
}

bool VerifyService::endTest(const Args& args, std::ostream& out)
{
    if (!args.empty()) {
        return error("end", "usage: end", out);
    }
    if (!test_) {
        return error("end", "'end' without 'test'", out);
    }
    const Test t = std::move(*test_);
    test_.reset();
    const bool ok = t.checksFailed == 0 && t.errors == 0 && !t.stopped;
    (ok ? counts_.testsPassed : counts_.testsFailed) += 1;
    if (!checkOnly_) {
        const std::size_t checks = t.checksPassed + t.checksFailed;
        std::string details = ok || t.checksFailed == 0 ? std::to_string(checks) + " check(s)"
                                                        : std::to_string(t.checksFailed) + " of "
                                                              + std::to_string(checks) + " check(s) failed";
        if (t.errors != 0) {
            details += ", " + std::to_string(t.errors) + " error(s)";
        }
        if (t.stopped) {
            details += ", stopped";
        }
        out << "=== " << t.id << (ok ? " PASS" : " FAIL") << "  (" << details << ")\n";
    }
    return ok;
}

void VerifyService::noteFailure()
{
    if (test_) {
        ++test_->errors;
    }
}

bool VerifyService::finish(std::ostream& out, bool stopped)
{
    if (!test_) {
        return true;
    }
    if (stopped) {
        test_->stopped = true;
        endTest({}, out);
        return false;
    }
    error("test", "test " + test_->id + " has no 'end'", out);
    endTest({}, out);
    return false;
}

bool VerifyService::anyRun() const
{
    return counts_.checksPassed + counts_.checksFailed + counts_.testsPassed + counts_.testsFailed > 0;
}

void VerifyService::printSummary(std::ostream& out, std::size_t commandsSent, std::size_t otherErrors) const
{
    const std::size_t errors = counts_.errors + otherErrors;
    const bool ok = counts_.checksFailed == 0 && counts_.testsFailed == 0 && errors == 0;
    out << "result: " << (ok ? "PASS" : "FAIL") << "  ";
    if (counts_.testsPassed + counts_.testsFailed > 0) {
        out << "tests: " << counts_.testsPassed << " passed, " << counts_.testsFailed << " failed; ";
    }
    out << "checks: " << counts_.checksPassed << " passed, " << counts_.checksFailed << " failed; "
        << "commands sent: " << commandsSent << "; errors: " << errors << '\n';
}

} // namespace mcs
