#include "ui/tui_session.hpp"

#include "core/text.hpp"
#include "ui/completion.hpp"
#include "ui/help_format.hpp"
#include "ui/menu_builder.hpp"
#include "ui/transfer_format.hpp"

#include <cli/cli.h>

#include <ftxui/component/app.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <cctype>
#include <deque>
#include <optional>
#include <streambuf>

namespace mcs::ui {

static_assert(kCompletionHelpSeparator == cli::CompletionHelpSeparator,
              "ui/completion.hpp must use the patched cli separator");
static_assert(kCompletionHintMarker == cli::CompletionHintMarker,
              "ui/completion.hpp must use the patched cli hint marker");

using namespace ftxui;

namespace {

// ---- command output ------------------------------------------------------------

// One line of command output; `names` marks command names (help) to colour.
struct OutputLine {
    std::string text;
    std::vector<TextSpan> names;
};

// Collects what the cli session and the commands print, as lines. ANSI escape
// sequences are dropped and tabs expanded. In capture mode text goes to a
// separate string instead (used to read the prompt).
class OutputBuffer : public std::streambuf {
public:
    static constexpr std::size_t kMaxLines = 2000;

    const std::deque<OutputLine>& lines() const { return lines_; }

    void addLine(std::string line, std::vector<TextSpan> names = {})
    {
        lines_.push_back({std::move(line), std::move(names)});
        if (lines_.size() > kMaxLines) {
            lines_.pop_front();
        }
    }

    void beginCapture() { capturing_ = true; captured_.clear(); }
    std::string endCapture() { capturing_ = false; return captured_; }

protected:
    int_type overflow(int_type ch) override
    {
        if (ch != traits_type::eof()) {
            put(static_cast<char>(ch));
        }
        return ch;
    }

    std::streamsize xsputn(const char* s, std::streamsize n) override
    {
        for (std::streamsize i = 0; i < n; ++i) {
            put(s[i]);
        }
        return n;
    }

private:
    void put(char c)
    {
        if (inEscape_) {
            // CSI: ESC '[' params... final byte in 0x40..0x7E
            if (c != '[' && static_cast<unsigned char>(c) >= 0x40 && static_cast<unsigned char>(c) <= 0x7E) {
                inEscape_ = false;
            }
            return;
        }
        if (c == '\x1b') {
            inEscape_ = true;
            return;
        }
        if (capturing_) {
            captured_ += c;
            return;
        }
        if (c == '\n') {
            addLine(std::move(partial_));
            partial_.clear();
        } else if (c == '\t') {
            partial_.append(4 - partial_.size() % 4, ' ');
        } else if (c != '\r') {
            partial_ += c;
        }
    }

    std::deque<OutputLine> lines_;
    std::string partial_;
    std::string captured_;
    bool capturing_ = false;
    bool inEscape_ = false;
};

// A cli session fed from the input line instead of a terminal.
class TuiCliSession : public cli::CliSession {
public:
    TuiCliSession(cli::Cli& owner, std::ostream& output) : cli::CliSession(owner, output, 100) {}
};

// ---- formatting ------------------------------------------------------------------

std::string clockText(TlmTime t)
{
    const std::time_t secs = TlmClock::to_time_t(t);
    std::tm local{};
    localtime_r(&secs, &local);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count() % 1000;
    char buf[32];
    std::snprintf(buf, sizeof buf, "%02d:%02d:%02d.%03d", local.tm_hour, local.tm_min, local.tm_sec,
                  static_cast<int>(ms));
    return buf;
}

double secondsBetween(TlmTime from, TlmTime to)
{
    return std::chrono::duration<double>(to - from).count();
}

std::string ageText(double seconds)
{
    char buf[32];
    if (seconds < 10) {
        std::snprintf(buf, sizeof buf, "%.1f s", seconds);
    } else if (seconds < 120) {
        std::snprintf(buf, sizeof buf, "%d s", static_cast<int>(seconds));
    } else if (seconds < 7200) {
        std::snprintf(buf, sizeof buf, "%d min", static_cast<int>(seconds / 60));
    } else {
        std::snprintf(buf, sizeof buf, "%d h", static_cast<int>(seconds / 3600));
    }
    return buf;
}

constexpr double kFreshSeconds = 5;
constexpr double kStaleSeconds = 30;

Color ageColor(double seconds)
{
    if (seconds < kFreshSeconds) {
        return Color::Default;
    }
    return seconds < kStaleSeconds ? Color::Yellow : Color::Red;
}

std::string upper(std::string s)
{
    for (char& c : s) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return s;
}

std::string repeat(const char* glyph, int count)
{
    std::string out;
    for (int i = 0; i < count; ++i) {
        out += glyph;
    }
    return out;
}

// Color constants are of different enum types; this picks between any two.
Color pick(bool condition, Color yes, Color no)
{
    return condition ? yes : no;
}

Element cell(const std::string& s, int width)
{
    return text(s) | size(WIDTH, EQUAL, width);
}

Color eventColor(std::int64_t type)
{
    switch (type) {
    case 1: return Color::GrayDark;   // DEBUG
    case 2: return Color::Green;      // INFO
    case 3: return Color::Red;        // ERROR
    case 4: return Color::RedLight;   // CRITICAL
    case 5: return Color::Yellow;     // WARNING
    default: return Color::Default;
    }
}

// A stable colour per app, so a card's border and badge identify its app at a glance.
Color appColor(const std::string& app)
{
    static const Color kPalette[] = {Color::Cyan,      Color::Green,     Color::Yellow,
                                     Color::BlueLight, Color::MagentaLight, Color::GreenLight,
                                     Color::CyanLight, Color::YellowLight};
    unsigned hash = 0;
    for (const char c : app) {
        hash = hash * 31U + static_cast<unsigned char>(c);
    }
    return kPalette[hash % (sizeof kPalette / sizeof kPalette[0])];
}

// Colour of an enum value by what its name says: good states green, off
// states grey, errors and unknown values red; anything else unchanged.
std::optional<Color> enumColor(const std::string& value)
{
    const std::string v = toLower(value);
    const auto has = [&v](const char* word) { return v.find(word) != std::string::npos; };
    if (v.rfind("?", 0) == 0 || has("err") || has("fail") || has("fault") || has("bad") || has("crit")) {
        return Color::Red;
    }
    if (has("disab") || has("inactive") || v == "off" || v == "false" || v == "none" || v == "idle") {
        return Color::GrayDark;
    }
    if (has("enab") || has("active") || v == "on" || v == "true" || v == "ok" || has("good")) {
        return Color::Green;
    }
    return std::nullopt;
}

const char* const kSpinner[] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
constexpr int kListWidth = 30;   // telemetry screen: packet checklist

// ---- the session -----------------------------------------------------------------

class Tui {
public:
    Tui(cli::Cli& cli, CommandService& service, TlmContext& tlm, const std::vector<std::string>& banner)
        : service_(service), tlm_(tlm), out_(&output_), session_(cli, out_)
    {
        for (const auto& line : banner) {
            output_.addLine(line);
        }
        session_.ExitAction([this](std::ostream&) { exitRequested_ = true; });
        // "help" is laid out for the output pane as it is when typed; names in colour.
        installHelp(cli, [this](std::ostream& out, const std::vector<HelpItem>& items) {
            out.flush();   // keep the order with what was printed before
            for (auto& line : formatHelp(items, outputWidth_)) {
                output_.addLine(std::move(line.text), std::move(line.names));
            }
        });
        updatePrompt();
    }

    int run()
    {
        App screen = App::Fullscreen();
        screen.ForceHandleCtrlC(false);   // Ctrl-C clears the line; quits when it is empty
        screen.TrackMouse(false);         // keep the terminal's own text selection for copying
        screen_ = &screen;

        InputOption opt;
        opt.content = &input_;
        opt.cursor_position = &cursor_;
        opt.multiline = false;
        opt.placeholder = "type a command (Tab completes, 'help' lists commands)";
        opt.transform = [](InputState s) {
            return s.is_placeholder ? s.element | dim : s.element;
        };
        opt.on_change = [this] { completion_ = {}; };
        Component input = Input(opt);

        Component root = Renderer(input, [this, input] { return render(input); });
        root |= CatchEvent([this](const Event& e) { return onEvent(e); });

        if (tlm_.service != nullptr) {
            tlm_.service->start([this](bool arrived) { onTelemetry(arrived); });
        }
        if (tlm_.files != nullptr) {
            tlm_.files->setOnChange([this] { requestRedraw(); });   // progress, notices
        }
        screen.Loop(root);
        if (tlm_.files != nullptr) {
            tlm_.files->setOnChange({});
        }
        if (tlm_.service != nullptr) {
            tlm_.service->stop();
        }
        screen_ = nullptr;
        return 0;
    }

private:
    // One row of the telemetry screen's checklist.
    struct ChecklistItem {
        std::uint16_t mid = 0;
        std::string app;                      // "" for a MID without definition
        std::string label;                    // "ds.hk" or "? 0x0803"
        const PacketState* packet = nullptr;  // defined and received
        const UnknownMid* unknown = nullptr;  // received without a definition
        const TlmPacketDef* def = nullptr;    // defined (received or not)
    };

    // Receiver thread: ask for a redraw. A skipped packet is drawn by the
    // next call (the receiver calls at least every 100 ms).
    void onTelemetry(bool /*arrived*/)
    {
        requestRedraw();
    }

    // Any thread (telemetry receiver, file transfer worker): ask for a
    // redraw, at most ~20 per second.
    void requestRedraw()
    {
        constexpr auto kMinGap = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                     std::chrono::milliseconds(50)).count();
        const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        if (now - lastPost_.load() < kMinGap) {
            return;
        }
        lastPost_ = now;
        if (App* s = screen_.load()) {
            s->PostEvent(Event::Custom);
        }
    }

    // ---- keys ----------------------------------------------------------------

    bool onEvent(const Event& e)
    {
        if (e == Event::Custom) {
            ++tick_;
            return false;
        }
        if (e == Event::F1 || e == Event::F2) {
            tlm_.view.view = e == Event::F1 ? TlmView::Console : TlmView::Telemetry;
            return true;
        }
        if (e == Event::F5) {
            tlm_.view.paused = !tlm_.view.paused;
            return true;
        }
        if (e == Event::CtrlC) {
            if (input_.empty()) {
                quit();
            } else {
                input_.clear();
                cursor_ = 0;
                completion_ = {};
            }
            return true;
        }
        if (tlm_.view.view == TlmView::Telemetry) {
            return onTelemetryKey(e);
        }
        return onConsoleKey(e);
    }

    // F1: the input line has the keys; the panes scroll with PgUp/PgDn (output)
    // and Ctrl+↑↓ / Alt+↑↓ (events).
    bool onConsoleKey(const Event& e)
    {
        if (e == Event::Return) {
            submit();
            return true;
        }
        if (e == Event::Tab) {
            complete();
            return true;
        }
        if (e == Event::ArrowUp || e == Event::ArrowDown) {
            input_ = e == Event::ArrowUp ? session_.PreviousCmd(input_) : session_.NextCmd();
            cursor_ = static_cast<int>(input_.size());
            completion_ = {};
            return true;
        }
        if (e == Event::PageUp || e == Event::PageDown) {
            const int step = std::max(1, outputRows_ - 1);
            outputScroll_ = std::max(0, outputScroll_ + (e == Event::PageUp ? step : -step));
            return true;   // outputPane() keeps it within the output
        }
        const bool older = e == Event::ArrowUpCtrl || e == Event::Special("\x1b[1;3A");
        const bool newer = e == Event::ArrowDownCtrl || e == Event::Special("\x1b[1;3B");
        if (older || newer) {
            eventScroll_ = std::max(0, eventScroll_ + (older ? 3 : -3));
            return true;
        }
        if (e == Event::CtrlD) {
            if (input_.empty()) {
                quit();
            }
            return true;
        }
        return false;
    }

    // F2: the checklist has the keys. A printable key or Esc goes back to the
    // console; the printable key is then typed into the input line.
    bool onTelemetryKey(const Event& e)
    {
        const int last = std::max(0, static_cast<int>(items_.size()) - 1);
        if (e == Event::ArrowUp) {
            listCursor_ = std::max(0, listCursor_ - 1);
        } else if (e == Event::ArrowDown) {
            listCursor_ = std::min(last, listCursor_ + 1);
        } else if (e == Event::Home) {
            listCursor_ = 0;
        } else if (e == Event::End) {
            listCursor_ = last;
        } else if (e == Event::Character(' ') || e == Event::Return) {
            toggleCursorItem();
        } else if (e == Event::PageUp || e == Event::PageDown) {
            const int step = std::max(1, displayRows_ - 2);
            displayScroll_ = std::max(0, displayScroll_ + (e == Event::PageUp ? -step : step));
        } else if (e == Event::ArrowUpCtrl || e == Event::Special("\x1b[1;3A")) {
            displayScroll_ = std::max(0, displayScroll_ - 3);
        } else if (e == Event::ArrowDownCtrl || e == Event::Special("\x1b[1;3B")) {
            displayScroll_ += 3;
        } else if (e == Event::Escape) {
            tlm_.view.view = TlmView::Console;
        } else if (e.is_character()) {
            tlm_.view.view = TlmView::Console;
            return false;   // the input line receives the character
        } else if (e == Event::CtrlD) {
            quit();
        }
        return true;   // nothing else reaches the hidden input line
    }

    void submit()
    {
        const std::string line = input_;
        input_.clear();
        cursor_ = 0;
        completion_ = {};
        outputScroll_ = 0;
        if (trim(line).empty()) {
            return;
        }
        output_.addLine("> " + line);
        session_.Feed(line);
        out_.flush();
        updatePrompt();
        if (exitRequested_) {
            quit();
        }
    }

    void complete()
    {
        completion_ = applyCompletion(input_, session_.GetCompletions(input_));
        if (completion_.newLine) {
            input_ = *completion_.newLine;
            cursor_ = static_cast<int>(input_.size());
        }
    }

    void updatePrompt()
    {
        output_.beginCapture();
        session_.Prompt();
        out_.flush();
        prompt_ = trim(output_.endCapture());
        if (prompt_.empty()) {
            prompt_ = ">";
        }
        prompt_ += ' ';
    }

    void quit()
    {
        // Quitting abandons running transfers: say so once, quit on the next try.
        if (tlm_.files != nullptr && !quitWarned_) {
            const auto c = tlm_.files->activeCounts();
            if (c.total() != 0) {
                quitWarned_ = true;
                exitRequested_ = false;
                output_.addLine("warning: " + std::to_string(c.total()) + " transfer"
                                + (c.total() == 1 ? " is" : "s are")
                                + " still running: quit again to abandon (or 'file list', 'file cancel <id>')");
                return;
            }
        }
        if (App* s = screen_.load()) {
            s->Exit();
        }
    }

    // ---- frame ---------------------------------------------------------------

    Element render(const Component& input)
    {
        const Dimensions dims = Terminal::Size();
        width_ = std::max(40, dims.dimx);
        const int bodyRows = std::max(8, dims.dimy - 2);   // header + hint line

        refreshSnapshot();
        Element body = tlm_.view.view == TlmView::Console ? consoleScreen(bodyRows, input)
                                                          : telemetryScreen(bodyRows);
        return vbox({header(), body | size(HEIGHT, EQUAL, bodyRows), hintLine()});
    }

    void refreshSnapshot()
    {
        if (tlm_.files != nullptr) {
            transfers_ = tlm_.files->transfers();
            fileCounts_ = tlm_.files->activeCounts();
            for (auto& line : tlm_.files->takeNotices()) {   // on the UI thread: output_ stays single-threaded
                output_.addLine(std::move(line));
            }
        }
        if (tlm_.store == nullptr || (tlm_.view.paused && frozen_)) {
            return;
        }
        now_ = TlmClock::now();
        snapshot_ = tlm_.store->snapshot();
        frozen_ = tlm_.view.paused;
    }

    TlmTime displayNow() const
    {
        return frozen_ ? now_ : TlmClock::now();
    }

    //  mcs · sil   F1 Console  F2 Telemetry          TLM udp://… ● 1532 pkts · last 0.2 s  ⠙
    Element header()
    {
        const auto tab = [this](const char* label, TlmView v) {
            Element e = text(label);
            return tlm_.view.view == v ? e | bold | inverted : e | dim;
        };
        Elements parts{text(" mcs ") | bold | color(Color::Cyan), text("· " + service_.targetName() + "  ") | dim,
                       tab(" F1 Console ", TlmView::Console), text(" "),
                       tab(" F2 Telemetry ", TlmView::Telemetry), text("  ")};
        if (const std::string badge = transferBadge(fileCounts_); !badge.empty()) {
            parts.push_back(text(" " + badge + " ") | bold | color(Color::Cyan));
            parts.push_back(text(" "));
        }
        if (frozen_) {
            parts.push_back(text(" PAUSED · F5 ") | bold | inverted | color(Color::Yellow));
        }
        parts.push_back(filler());
        parts.push_back(linkStatus() | flex_shrink);
        return hbox(std::move(parts));
    }

    Element linkStatus()
    {
        if (tlm_.store == nullptr) {
            return text("telemetry not configured ") | dim;
        }
        const TlmCounters& c = snapshot_.counters;
        const std::optional<double> age =
            c.lastRx ? std::optional<double>(secondsBetween(*c.lastRx, displayNow())) : std::nullopt;
        std::string error = tlm_.service == nullptr ? tlm_.receiverError : tlm_.service->lastError();

        Color dot = Color::GrayDark;
        if (!error.empty()) {
            dot = Color::Red;
        } else if (age) {
            dot = *age < kFreshSeconds ? Color::Green : Color::Yellow;
        }
        const bool compact = width_ < 120;   // drop the address, shorten the warnings
        const std::string where = tlm_.service != nullptr ? tlm_.service->describe() : "receiver closed";
        Elements parts{text("TLM ") | bold, text(compact ? "" : where + " ") | dim, text("●") | color(dot)};
        if (!error.empty()) {
            // Drop the "target 'x': telemetry: " context; the line has little room.
            if (const auto at = error.find("udp receiver: "); at != std::string::npos) {
                error = error.substr(at + 14);
            }
            parts.push_back(text(" " + error) | color(Color::Red));
        } else {
            parts.push_back(text(" " + std::to_string(c.received) + " pkts"));
            const auto warn = [&parts](std::size_t n, const std::string& what) {
                if (n != 0) {
                    parts.push_back(text(" · " + std::to_string(n) + " " + what) | color(Color::Yellow));
                }
            };
            if (compact) {
                const std::size_t issues = c.gaps + c.lengthErrors + c.unknown + c.malformed;
                if (issues != 0) {
                    parts.push_back(text(" · ⚠ " + std::to_string(issues)) | color(Color::Yellow));
                }
            } else {
                warn(c.gaps, c.gaps == 1 ? "gap" : "gaps");
                warn(c.lengthErrors, "len err");
                warn(c.unknown, "unknown");
                warn(c.malformed, "malformed");
            }
            parts.push_back(text(age ? " · last " + ageText(*age) : " · waiting") | dim);
        }
        const bool active = age && *age < 1.0 && !frozen_;
        parts.push_back(text(std::string(" ") + (active ? kSpinner[tick_ % 10] : "·") + " ") | color(Color::Cyan));
        return hbox(std::move(parts));
    }

    Element hintLine()
    {
        const bool narrow = width_ < 120;
        if (tlm_.view.view == TlmView::Telemetry) {
            return text(narrow ? " ↑↓ move · Space tick · PgUp/PgDn scroll · F1 console (or type) · F5 pause"
                               : " ↑↓ move · Space tick · PgUp/PgDn scroll cards · F5 pause · "
                                 "F1 console (or just type) · Ctrl-C quit")
                 | dim;
        }
        std::string hint = narrow ? " Tab complete · ↑↓ history · PgUp/PgDn output · Ctrl+↑↓ events · F2 telemetry"
                                  : " Enter run · Tab complete · ↑↓ history · PgUp/PgDn output · Ctrl+↑↓ events · "
                                    "F2 telemetry · F5 pause · Ctrl-C clear/quit";
        if (width_ >= 160 && fileCounts_.total() != 0) {
            hint += " · 'file cancel <id>' stops a transfer";
        }
        return text(hint) | dim;
    }

    // ---- F1 console -------------------------------------------------------------
    //
    //   ╭ EVENTS ─────────╮   ~40%
    //   ╰─────────────────╯
    //   ╭ COMMAND OUTPUT ─╮   the rest
    //   ╰─────────────────╯
    //    mcs> █              input line, then the Tab list

    Element consoleScreen(int rows, const Component& input)
    {
        const int listRows = completionRows(rows);
        const int stripBox = transfersRows();   // 0 when nothing is being transferred
        const int paneRows = std::max(8, rows - 1 - listRows - stripBox);
        const int eventsRows = std::max(4, paneRows * 2 / 5);
        const int outputBox = std::max(4, paneRows - eventsRows);
        outputRows_ = outputBox - 2;
        outputWidth_ = std::max(20, width_ - 3);   // border + leading space

        Elements panes{eventsPane(eventsRows) | size(HEIGHT, EQUAL, eventsRows)};
        if (stripBox > 0) {
            panes.push_back(transfersPane() | size(HEIGHT, EQUAL, stripBox));
        }
        panes.push_back(outputPane() | size(HEIGHT, EQUAL, outputBox));
        panes.push_back(inputLine(input));
        panes.push_back(completionArea(listRows));
        return vbox(std::move(panes));
    }

    // ---- F1 TRANSFERS strip: running and waiting file transfers ----------------------
    //
    //   ╭ TRANSFERS 2 active ───────────────────────────────── 'file list' ╮
    //   │ ↑ put 0.23_1729061234  test.bin → /cf/test.bin   c2  ████░░ 76%  48.2 KB/s  5 s │
    //   ╰──────────────────────────────────────────────────────────────────╯

    static constexpr int kMaxStripRows = 3;

    int activeTransfers() const
    {
        int n = 0;
        for (const auto& s : transfers_) {
            n += cfdp::isFinished(s.state) ? 0 : 1;
        }
        return n;
    }

    // Height of the strip with its border; 0 hides it.
    int transfersRows() const
    {
        const int n = activeTransfers();
        if (n == 0) {
            return 0;
        }
        return std::min(n, kMaxStripRows) + (n > kMaxStripRows ? 1 : 0) + 2;
    }

    Element stripRow(const StripRow& r)
    {
        const Color c = r.uplink ? Color::Cyan : Color::Green;
        Elements parts{text(" " + r.arrow + " ") | bold | color(c), text(r.kind + " ")};
        if (!r.id.empty()) {
            parts.push_back(text(r.id + "  ") | dim);
        }
        parts.push_back(text(r.files));
        parts.push_back(filler());
        parts.push_back(text("  " + r.cls + "  ") | dim);
        if (r.waiting) {
            parts.push_back(text(r.note + " ") | dim);
            return hbox(std::move(parts));
        }
        parts.push_back(text(r.bar) | color(c));
        parts.push_back(text(" " + r.percent) | bold);
        parts.push_back(text("  " + r.rate) | size(WIDTH, GREATER_THAN, 12));
        if (!r.eta.empty()) {
            parts.push_back(text("  " + r.eta) | dim | size(WIDTH, GREATER_THAN, 9));
        }
        if (r.suspended) {
            parts.push_back(text("  SUSPENDED") | bold | color(Color::Yellow));
        }
        parts.push_back(text(" "));
        return hbox(std::move(parts));
    }

    Element transfersPane()
    {
        const bool compact = width_ < 120;
        // Border (2), the leading/trailing space and the rate/ETA columns'
        // minimum widths (stripRow) take a few columns more than toText().
        const int rowWidth = std::max(40, width_ - 10);
        Elements rows;
        int shown = 0;
        for (const auto& s : transfers_) {
            if (cfdp::isFinished(s.state) || shown == kMaxStripRows) {
                continue;
            }
            ++shown;
            rows.push_back(stripRow(formatStripRow(s, rowWidth, compact)));
        }
        const int total = activeTransfers();
        if (total > kMaxStripRows) {
            rows.push_back(text(" +" + std::to_string(total - kMaxStripRows) + " more · 'file list'") | dim);
        }
        return window(paneTitle("TRANSFERS", std::to_string(total) + " active", ""), vbox(std::move(rows)))
             | color(Color::Default);
    }

    Element paneTitle(const std::string& name, const std::string& count, const std::string& note)
    {
        return hbox({text(" " + name + " ") | bold, text(count.empty() ? "" : count + " ") | dim, filler(),
                     text(note.empty() ? "" : " " + note + " ") | color(Color::Yellow)});
    }

    Element eventsPane(int box)
    {
        const int dataRows = std::max(1, box - 3);   // border + column header
        if (tlm_.store == nullptr) {
            return window(paneTitle("EVENTS", "", ""),
                          text(" telemetry is not configured for target '" + service_.targetName()
                               + "' (add a \"telemetry\" block in targets.json)") | dim);
        }
        const auto& events = snapshot_.events;
        const int total = static_cast<int>(events.size());
        eventScroll_ = std::clamp(eventScroll_, 0, std::max(0, total - dataRows));
        const int end = total - eventScroll_;
        const int begin = std::max(0, end - dataRows);

        Elements lines;
        lines.push_back(hbox({cell(" time", 15), cell("type", 7), cell("app", 12), cell("id", 6),
                              text("message")}) | dim | bold);
        for (int i = begin; i < end; ++i) {
            const EventRecord& e = events[static_cast<std::size_t>(i)];
            const Color c = eventColor(e.eventType);
            Element type = cell(e.typeName, 7) | color(c);
            Element message = text(e.message);
            if (e.eventType >= 3) {
                type = type | bold;
                message = message | color(c);
            }
            lines.push_back(hbox({cell(" " + clockText(e.rxTime), 15) | dim, type, cell(e.app, 12),
                                  cell(std::to_string(e.eventId), 6) | dim, message}));
        }
        if (events.empty()) {
            lines.push_back(text(" no events yet") | dim);
        }
        const std::string note = eventScroll_ > 0 ? "▼ " + std::to_string(eventScroll_) + " newer (Ctrl+↓)"
                               : begin > 0       ? "▲ " + std::to_string(begin) + " older (Ctrl+↑)"
                                                 : "";
        return window(paneTitle("EVENTS", std::to_string(total), note), vbox(std::move(lines)));
    }

    // One display row of a stored line: help names in cyan, the rest styled by
    // what the line is (an echoed command, an error, ...).
    Element outputRow(const OutputLine& line, const std::string& row, bool whole)
    {
        if (whole && !line.names.empty()) {
            Elements parts{text(" ")};
            std::size_t at = 0;
            for (const auto& span : line.names) {
                parts.push_back(text(row.substr(at, span.offset - at)));
                parts.push_back(text(row.substr(span.offset, span.length)) | color(Color::Cyan));
                at = span.offset + span.length;
            }
            parts.push_back(text(row.substr(std::min(at, row.size()))));
            return hbox(std::move(parts));
        }
        Element e = text(" " + row);
        const std::string& t = line.text;
        if (t.rfind("> ", 0) == 0) {   // the command you typed
            return e | bold | color(Color::Yellow);
        }
        if (t.rfind("error", 0) == 0 || t.rfind("Unknown command", 0) == 0) {
            return e | color(Color::Red);
        }
        if (t.rfind("warning", 0) == 0) {
            return e | color(Color::Yellow);
        }
        // File transfer notices (FileTransferService).
        if (t.rfind("✔", 0) == 0) {
            return e | color(Color::Green);
        }
        if (t.rfind("✘", 0) == 0) {
            return e | bold | color(Color::Red);
        }
        if (t.rfind("⊘", 0) == 0) {
            return e | color(Color::Yellow);
        }
        if (t.rfind("started ", 0) == 0 || t.rfind("waiting for CF", 0) == 0 || t.rfind("↓ ", 0) == 0) {
            return e | color(Color::Cyan);
        }
        if (t.rfind("[dry-run] cfdp", 0) == 0) {
            return e | dim;
        }
        return e;
    }

    // The newest output, wrapped to the pane: lines too long for it continue
    // on the next row under their own indent. Scrolling counts display rows.
    Element outputPane()
    {
        const auto& lines = output_.lines();
        const std::size_t rows = static_cast<std::size_t>(std::max(1, outputRows_));
        const std::size_t want = rows + static_cast<std::size_t>(outputScroll_);

        // (display row, index of its stored line), newest last; only as far back as needed.
        struct Row {
            std::string text;
            std::size_t line;
            bool whole;   // the stored line fits on this one row
        };
        std::deque<Row> display;
        for (std::size_t i = lines.size(); i-- > 0 && display.size() < want + 1;) {
            const auto wrapped = wrapLine(lines[i].text, outputWidth_);
            for (auto it = wrapped.rbegin(); it != wrapped.rend(); ++it) {
                display.push_front({*it, i, wrapped.size() == 1});
            }
        }
        const bool olderExist = display.size() > want || (!lines.empty() && !display.empty() && display.front().line > 0);
        outputScroll_ = std::min(outputScroll_, static_cast<int>(display.size() > rows ? display.size() - rows : 0));
        const std::size_t end = display.size() - static_cast<std::size_t>(outputScroll_);
        const std::size_t begin = end > rows ? end - rows : 0;

        Elements out;
        for (std::size_t i = begin; i < end; ++i) {
            out.push_back(outputRow(lines[display[i].line], display[i].text, display[i].whole));
        }
        while (out.size() < rows) {
            out.insert(out.begin(), text(""));
        }
        const std::string note = outputScroll_ > 0 ? "▼ " + std::to_string(outputScroll_) + " newer (PgDn)"
                               : (begin > 0 || olderExist) ? "▲ older (PgUp)"
                                                           : "";
        return window(paneTitle("COMMAND OUTPUT", "", note), vbox(std::move(out)));
    }

    Element inputLine(const Component& input)
    {
        return hbox({text(" " + prompt_) | bold | color(Color::Green), input->Render() | flex});
    }

    // The Tab list: candidate names in a grid (completion.hpp gridRows), or the
    // hints for an argument.
    std::vector<std::string> completionLines() const
    {
        if (completion_.items.empty()) {
            std::vector<std::string> out;
            for (const auto& h : completion_.hints) {
                out.push_back("   " + h);
            }
            return out;
        }
        std::vector<std::string> words;
        for (const auto& item : completion_.items) {
            words.push_back(item.word);
        }
        return gridRows(words, width_);
    }

    // Rows for the Tab list: all of it if it fits, at most half the screen.
    int completionRows(int rows) const
    {
        return std::min(std::max(6, rows / 2), static_cast<int>(completionLines().size()));
    }

    Element completionArea(int rows)
    {
        const auto lines = completionLines();
        const bool names = !completion_.items.empty();
        Elements out;
        for (std::size_t i = 0; i < lines.size() && i < static_cast<std::size_t>(rows); ++i) {
            if (i + 1 == static_cast<std::size_t>(rows) && lines.size() > static_cast<std::size_t>(rows)) {
                out.push_back(text("   … " + std::to_string(lines.size() - i) + " more rows: type more letters")
                              | dim);
                break;
            }
            out.push_back(names ? text(lines[i]) | color(Color::Cyan) : text(lines[i]) | dim);
        }
        return vbox(std::move(out));
    }

    // ---- F2 telemetry -----------------------------------------------------------
    //
    //   ╭ PACKETS 2/5 ─────╮ ╭ DS  hk 0x08B8 ── seq 7 · ×8 · 0.3 s ╮ ╭ ? 0x0899 ─────╮
    //   │ [x] ds.hk      ● │ │ cmd_accepted ·········· 9 ▲      │ │ 0000  08 99 … │
    //   │ [ ] to.hk      ○ │ ╰──────────────────────────────────╯ ╰───────────────╯
    //   ╰──────────────────╯
    //
    // Left: every non-event packet of the catalog (● received, ○ not yet), then
    // MIDs received without a definition. Right: the ticked ones as cards, in
    // checklist order, wrapped into rows; PgUp/PgDn scroll them.

    Element telemetryScreen(int rows)
    {
        if (tlm_.store == nullptr) {
            return vbox({text(""), text("Telemetry is not configured for target '" + service_.targetName() + "'.")
                                       | hcenter,
                         text("Add a \"telemetry\" block in targets.json. F1 returns to the console.") | dim
                             | hcenter});
        }
        buildChecklist();
        displayRows_ = rows;
        return hbox({checklist(rows), text(" "),
                     displayArea(rows, width_ - kListWidth - 1) | flex});
    }

    void buildChecklist()
    {
        items_.clear();
        for (const auto& def : tlm_.store->catalog().packets()) {
            if (def.isEvent) {
                continue;   // shown in the console's event pane
            }
            ChecklistItem item;
            item.mid = def.mid;
            item.app = def.app;
            item.label = def.fullName();
            item.def = &def;
            for (const auto& p : snapshot_.packets) {
                if (p.def == &def) {
                    item.packet = &p;
                }
            }
            items_.push_back(item);
        }
        for (const auto& u : snapshot_.unknown) {
            ChecklistItem item;
            item.mid = u.mid;
            item.label = "? " + toHexString(u.mid, 4);
            item.unknown = &u;
            items_.push_back(item);
        }
        // Ticked with `tlm show 0x....` before anything arrived on that MID.
        for (const std::uint16_t mid : tlm_.view.shown) {
            const bool listed = std::any_of(items_.begin(), items_.end(),
                                            [mid](const ChecklistItem& i) { return i.mid == mid; });
            if (!listed) {
                ChecklistItem item;
                item.mid = mid;
                item.label = "? " + toHexString(mid, 4);
                items_.push_back(item);
            }
        }
        listCursor_ = std::clamp(listCursor_, 0, std::max(0, static_cast<int>(items_.size()) - 1));
    }

    void toggleCursorItem()
    {
        if (items_.empty()) {
            return;
        }
        const std::uint16_t mid = items_[static_cast<std::size_t>(listCursor_)].mid;
        if (!tlm_.view.shown.erase(mid)) {
            tlm_.view.shown.insert(mid);
        }
    }

    Element checklist(int rows)
    {
        const int inner = std::max(1, rows - 2);
        const int count = static_cast<int>(items_.size());
        const bool overflow = count > inner;
        const int visible = overflow ? std::max(1, inner - 2) : inner;   // two lines for ▲/▼
        if (listCursor_ < listScroll_) {
            listScroll_ = listCursor_;
        } else if (listCursor_ >= listScroll_ + visible) {
            listScroll_ = listCursor_ - visible + 1;
        }
        listScroll_ = std::clamp(listScroll_, 0, std::max(0, count - visible));

        const TlmTime now = displayNow();
        constexpr std::size_t kLabelWidth = static_cast<std::size_t>(kListWidth) - 11;
        Elements lines;
        if (overflow) {
            lines.push_back(text(listScroll_ > 0 ? "  ▲ " + std::to_string(listScroll_) + " more" : "") | dim);
        }
        for (int i = listScroll_; i < std::min(count, listScroll_ + visible); ++i) {
            const ChecklistItem& item = items_[static_cast<std::size_t>(i)];
            const bool ticked = tlm_.view.shown.count(item.mid) != 0;

            // ● fresh / ● older (yellow) / ● stale (grey) / ○ never received
            std::optional<TlmTime> lastRx;
            if (item.packet != nullptr) {
                lastRx = item.packet->lastRx;
            } else if (item.unknown != nullptr) {
                lastRx = item.unknown->lastRx;
            }
            Element dot = text("○") | color(Color::GrayDark);
            if (lastRx) {
                const double age = secondsBetween(*lastRx, now);
                dot = text("●") | color(age < kFreshSeconds ? Color::Green
                                        : age < kStaleSeconds ? Color::Yellow : Color::GrayDark);
            }

            std::string label = item.label;
            if (label.size() > kLabelWidth) {
                label = label.substr(0, kLabelWidth - 1) + "…";
            }
            Element name;
            if (item.def != nullptr) {
                const std::string rest = label.size() > item.app.size() ? label.substr(item.app.size()) : "";
                name = hbox({text(label.substr(0, std::min(label.size(), item.app.size())))
                                 | color(appColor(item.app)),
                             text(rest)});
            } else {
                name = text(label) | color(Color::Magenta);
            }
            if (ticked) {
                name = name | bold;
            }
            Element line = hbox({text(ticked ? " ◉ " : " ○ ") | color(pick(ticked, Color::Cyan, Color::GrayDark)),
                                 name, filler(), dot, text(" ")});
            if (i == listCursor_) {
                line = line | inverted;
            }
            lines.push_back(line);
        }
        if (overflow) {
            const int below = count - listScroll_ - visible;
            lines.push_back(text(below > 0 ? "  ▼ " + std::to_string(below) + " more" : "") | dim);
        }
        if (items_.empty()) {
            lines.push_back(text(" no packets defined") | dim);
        }
        const std::string counts = std::to_string(tlm_.view.shown.size()) + "/" + std::to_string(count);
        return window(paneTitle("PACKETS", counts, ""), vbox(std::move(lines)) | yframe | flex)
             | size(WIDTH, EQUAL, kListWidth);
    }

    // The title of a card: badge, name, MID, then seq / count / age and warnings.
    Element cardTitle(Element badge, const std::string& name, std::uint16_t mid, const std::string& facts,
                      double age, std::size_t gaps, std::size_t lengthErrors)
    {
        Elements parts{badge, text(" " + name + " ") | bold | color(Color::Default),
                       text(toHexString(mid, 4) + " ") | dim, filler()};
        if (gaps != 0) {
            parts.push_back(text(" ⚠ " + std::to_string(gaps) + " gap" + (gaps > 1 ? "s" : "")) | color(Color::Yellow));
        }
        if (lengthErrors != 0) {
            parts.push_back(text(" ⚠ len") | color(Color::Yellow));
        }
        parts.push_back(text(" " + facts + " · ") | dim);
        parts.push_back(text(ageText(age) + " ") | color(ageColor(age)));
        return hbox(std::move(parts));
    }

    // A decoded packet: "name ······· value" rows, in as many columns as needed
    // to fit `maxRows` (within `maxWidth`). Columns are as wide as the longest
    // name and value. Border and badge in the app's colour, grey when stale.
    Element packetCard(const PacketState& p, int maxRows, int maxWidth, int& width, int& height)
    {
        const TlmTime now = displayNow();
        const double age = secondsBetween(p.lastRx, now);
        const bool stale = age >= kStaleSeconds;
        const Color accent = stale ? Color::GrayDark : appColor(p.def->app);

        std::vector<const TlmFieldDef*> fields;
        for (const auto& f : p.def->fields) {
            if (f.visible()) {
                fields.push_back(&f);
            }
        }

        std::size_t nameWidth = 4;
        std::size_t valueWidth = 4;
        for (const auto& v : p.values) {
            nameWidth = std::max(nameWidth, v.name.size());
            valueWidth = std::max(valueWidth, v.text.size());
        }
        // " name ··· value ▲ ": 1 + name + 1 + leader(>=3) + 1 + value + 3
        const auto fixed = valueWidth + 9;
        const auto widest = static_cast<std::size_t>(std::max(24, maxWidth - 2));
        if (nameWidth + fixed > widest) {
            nameWidth = widest > fixed + 4 ? widest - fixed : 4;
        }
        const int columnWidth = static_cast<int>(nameWidth + fixed);

        Elements rows;
        for (std::size_t i = 0; i < p.values.size(); ++i) {
            const DecodedValue& v = p.values[i];
            const TlmFieldDef* f = i < fields.size() ? fields[i] : nullptr;
            const bool recent = p.changedAt[i] != TlmTime{} && secondsBetween(p.changedAt[i], now) < 1.0;
            const char* arrow = p.trend[i] > 0 ? "▲" : p.trend[i] < 0 ? "▼" : "•";

            const bool cut = v.name.size() > nameWidth;
            const std::string name = cut ? v.name.substr(0, nameWidth - 1) + "…" : v.name;
            const int nameColumns = cut ? static_cast<int>(nameWidth) : static_cast<int>(name.size());
            // Units dimmed: "2048 B" -> "2048" + " B"
            std::string number = v.text;
            std::string units;
            if (f != nullptr && !f->units.empty() && number.size() > f->units.size() + 1
                && number.compare(number.size() - f->units.size(), f->units.size(), f->units) == 0) {
                units = number.substr(number.size() - f->units.size() - 1);
                number.resize(number.size() - units.size());
            }
            Element value = text(number);
            if (v.missing) {
                value = text("—") | dim;
            } else if (recent) {
                value = value | bold | color(Color::Yellow);
            } else if (f != nullptr && !f->enumValues.empty()) {
                if (const auto c = enumColor(v.text)) {
                    value = value | color(*c);
                }
            }
            const int leader = columnWidth - nameColumns - static_cast<int>(v.text.size()) - 6;
            rows.push_back(hbox({text(" " + name + " ") | dim, text(repeat("·", std::max(1, leader))) | color(Color::GrayDark),
                                 text(" "), value, text(units) | dim,
                                 text(std::string(" ") + (recent ? arrow : " ") + " ") | color(Color::Yellow)})
                           | size(WIDTH, EQUAL, columnWidth));
        }
        if (rows.empty()) {
            rows.push_back(text(" (no fields)") | dim | size(WIDTH, EQUAL, columnWidth));
        }

        const int n = static_cast<int>(rows.size());
        const int fitRows = std::max(3, maxRows - 2);
        const int maxColumns = std::max(1, (maxWidth - 2 + 1) / (columnWidth + 1));
        const int columns = std::clamp((n + fitRows - 1) / fitRows, 1, maxColumns);
        const int perColumn = (n + columns - 1) / columns;
        Elements cols;
        for (int c = 0; c < columns; ++c) {
            Elements col;
            for (int r = c * perColumn; r < std::min(n, (c + 1) * perColumn); ++r) {
                col.push_back(rows[static_cast<std::size_t>(r)]);
            }
            if (c > 0) {
                cols.push_back(separatorLight() | color(Color::GrayDark));
            }
            cols.push_back(vbox(std::move(col)));
        }
        const std::string facts = "seq " + std::to_string(p.sequence) + " · ×" + std::to_string(p.count);
        Element badge = text(" " + upper(p.def->app) + " ") | bold | inverted;
        Element title = cardTitle(badge, p.def->name, p.def->mid, facts, age, p.gaps, p.lengthErrors);
        width = std::max(columns * columnWidth + (columns - 1) + 2, 48);
        height = perColumn + 2;
        return window(title, hbox(std::move(cols)) | color(Color::Default)) | color(accent)
             | size(WIDTH, EQUAL, width);
    }

    // A MID without a definition: the last packet as hex, header bytes (0-11)
    // in cyan, with an ASCII column.
    Element hexCard(const UnknownMid& u, int& width, int& height)
    {
        const double age = secondsBetween(u.lastRx, displayNow());
        const Bytes& b = u.lastPacket;
        Elements lines;
        for (std::size_t row = 0; row < b.size(); row += 16) {
            Elements cells{text(" " + toHexString(row, 4).substr(2) + "  ") | color(Color::GrayDark)};
            std::string ascii;
            for (std::size_t i = row; i < row + 16; ++i) {
                if (i >= b.size()) {
                    cells.push_back(text("   "));
                    continue;
                }
                Element byte = text(toHexString(b[i], 2).substr(2) + " ");
                cells.push_back(i < kTlmHeaderSize ? byte | color(Color::Cyan) | dim : byte);
                ascii += (b[i] >= 0x20 && b[i] < 0x7F) ? static_cast<char>(b[i]) : '.';
            }
            cells.push_back(text(" " + ascii + " ") | dim);
            lines.push_back(hbox(std::move(cells)));
        }
        lines.push_back(text(" no definition · cyan: CCSDS header (12 bytes)") | dim);
        width = 6 + 16 * 3 + 18 + 2;
        height = static_cast<int>(lines.size()) + 2;
        const std::string facts = "seq " + std::to_string(u.sequence) + " · " + std::to_string(u.lastLength)
                                + " B · ×" + std::to_string(u.count);
        Element badge = text(" ? ") | bold | inverted;
        return window(cardTitle(badge, "unknown", u.mid, facts, age, u.gaps, 0),
                      vbox(std::move(lines)) | color(Color::Default))
             | color(Color::Magenta) | size(WIDTH, EQUAL, width);
    }

    Element waitingCard(const ChecklistItem& item, int& width, int& height)
    {
        width = std::max(40, static_cast<int>(item.label.size()) + 24);
        height = 3;
        return window(text(" " + item.label + " " + toHexString(item.mid, 4) + " "),
                      text(" waiting for the first packet…") | dim)
             | color(Color::GrayDark) | size(WIDTH, EQUAL, width);
    }

    Element emptyState()
    {
        Elements lines{text("No packets selected") | bold | hcenter, text(""),
                       text("↑↓ choose a packet, Space shows it") | hcenter,
                       text("or type  tlm show all") | dim | hcenter};
        if (snapshot_.counters.received == 0) {
            lines.push_back(text(""));
            lines.push_back(text("No telemetry yet. Start it with") | color(Color::Yellow) | hcenter);
            lines.push_back(text("fsw to enable_output 127.0.0.1 5011") | color(Color::Yellow) | hcenter);
        }
        return hbox({text("   "), vbox(std::move(lines)), text("   ")}) | borderStyled(ROUNDED, Color::GrayDark)
             | center;
    }

    Element displayArea(int rows, int areaWidth)
    {
        struct Placed {
            Element element;
            int width;
            int height;
        };
        std::vector<Placed> cards;
        for (const auto& item : items_) {
            if (tlm_.view.shown.count(item.mid) == 0) {
                continue;
            }
            Placed p{nullptr, 0, 0};
            if (item.packet != nullptr) {
                p.element = packetCard(*item.packet, rows, areaWidth, p.width, p.height);
            } else if (item.unknown != nullptr) {
                p.element = hexCard(*item.unknown, p.width, p.height);
            } else {
                p.element = waitingCard(item, p.width, p.height);
            }
            cards.push_back(std::move(p));
        }
        if (cards.empty()) {
            displayScroll_ = 0;
            return emptyState();
        }

        // Flow layout: left to right, wrapping when the next card does not fit.
        Elements lines;
        Elements line;
        int lineWidth = 0;
        int lineHeight = 0;
        int total = 0;
        for (auto& c : cards) {
            if (!line.empty() && lineWidth + c.width > areaWidth) {
                lines.push_back(hbox(std::move(line)));
                line = Elements{};
                total += lineHeight;
                lineWidth = 0;
                lineHeight = 0;
            }
            line.push_back(c.element);
            lineWidth += c.width;
            lineHeight = std::max(lineHeight, c.height);
        }
        lines.push_back(hbox(std::move(line)));
        total += lineHeight;

        displayScroll_ = std::clamp(displayScroll_, 0, std::max(0, total - rows));
        const int below = total - rows - displayScroll_;
        Element area = vbox(std::move(lines)) | focusPosition(0, displayScroll_ + (rows - 1) / 2) | yframe;
        if (displayScroll_ > 0 || below > 0) {
            area = vbox({area | flex,
                         hbox({filler(), text(" ▲ " + std::to_string(displayScroll_) + "  ▼ "
                                              + std::to_string(std::max(0, below)) + " lines · PgUp/PgDn ")
                                             | inverted | dim})});
        }
        return area;
    }

    CommandService& service_;
    TlmContext& tlm_;
    OutputBuffer output_;
    std::ostream out_;
    TuiCliSession session_;
    std::atomic<App*> screen_{nullptr};   // read by the receiver thread
    std::atomic<std::chrono::steady_clock::rep> lastPost_{0};

    std::string input_;
    int cursor_ = 0;
    std::string prompt_ = "> ";
    CompletionResult completion_;
    bool exitRequested_ = false;
    bool quitWarned_ = false;                       // quit asked once while transfers ran
    std::vector<cfdp::TransferStatus> transfers_;   // file transfers of the last frame
    FileTransferService::Counts fileCounts_;

    int width_ = 80;
    int outputRows_ = 5;                 // console: visible output lines
    int outputScroll_ = 0;               // console: display rows scrolled back
    int outputWidth_ = 100;              // console: columns of an output line (help layout)
    int eventScroll_ = 0;                // console: events scrolled back
    int displayRows_ = 10;               // telemetry: height of the card area
    int displayScroll_ = 0;              // telemetry: card area, in lines
    int listCursor_ = 0;
    int listScroll_ = 0;
    std::vector<ChecklistItem> items_;   // checklist of the last frame
    unsigned tick_ = 0;

    TlmSnapshot snapshot_;
    TlmTime now_{};
    bool frozen_ = false;
};

} // namespace

int runTui(cli::Cli& cli, CommandService& service, TlmContext& tlm, const std::vector<std::string>& banner)
{
    Tui tui(cli, service, tlm, banner);
    return tui.run();
}

} // namespace mcs::ui
