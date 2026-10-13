#include "ui/tui_session.hpp"

#include "core/hex.hpp"
#include "core/text.hpp"
#include "ui/completion.hpp"

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
#include <deque>
#include <streambuf>

namespace mcs::ui {

static_assert(kCompletionHelpSeparator == cli::CompletionHelpSeparator,
              "ui/completion.hpp must use the patched cli separator");
static_assert(kCompletionHintMarker == cli::CompletionHintMarker,
              "ui/completion.hpp must use the patched cli hint marker");

using namespace ftxui;

namespace {

// ---- command output ------------------------------------------------------------

// Collects what the cli session and the commands print, as lines. ANSI escape
// sequences are dropped and tabs expanded. In capture mode text goes to a
// separate string instead (used to read the prompt).
class OutputBuffer : public std::streambuf {
public:
    static constexpr std::size_t kMaxLines = 2000;

    const std::deque<std::string>& lines() const { return lines_; }

    void addLine(std::string line)
    {
        lines_.push_back(std::move(line));
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

    std::deque<std::string> lines_;
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

Color ageColor(double seconds)
{
    if (seconds < 5) {
        return Color::Default;
    }
    return seconds < 30 ? Color::Yellow : Color::Red;
}

std::string padRight(std::string s, std::size_t width)
{
    if (s.size() < width) {
        s.append(width - s.size(), ' ');
    }
    return s;
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

Color statusColor(PacketStatus s)
{
    switch (s) {
    case PacketStatus::Ok: return Color::Green;
    case PacketStatus::SeqGap: return Color::Yellow;
    case PacketStatus::LengthMismatch: return Color::Yellow;
    case PacketStatus::Unknown: return Color::Magenta;
    case PacketStatus::Malformed: return Color::Red;
    }
    return Color::Default;
}

const char* const kSpinner[] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
constexpr int kListWidth = 28;   // dashboard checklist
constexpr int kMaxCompletionRows = 6;

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
        screen.Loop(root);
        if (tlm_.service != nullptr) {
            tlm_.service->stop();
        }
        screen_ = nullptr;
        return 0;
    }

private:
    // Receiver thread: ask for a redraw, at most ~20 per second. A skipped
    // packet is drawn by the next call (the receiver calls at least every 100 ms).
    void onTelemetry(bool /*arrived*/)
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

    // ---- input ---------------------------------------------------------------

    bool onEvent(const Event& e)
    {
        if (e == Event::Custom) {
            ++tick_;
            return false;
        }
        if (e == Event::F6) {
            if (tlm_.store != nullptr) {
                listFocused_ = tlm_.view.view != TlmView::Dashboard || !listFocused_;
                tlm_.view.view = TlmView::Dashboard;
            }
            return true;
        }
        if (listFocused_ && tlm_.view.view == TlmView::Dashboard && onChecklistEvent(e)) {
            return true;
        }
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
            outputScroll_ += e == Event::PageUp ? step : -step;
            outputScroll_ = std::clamp(outputScroll_, 0,
                                       std::max(0, static_cast<int>(output_.lines().size()) - outputRows_));
            return true;
        }
        const bool topUp = e == Event::ArrowUpCtrl || e == Event::Special("\x1b[1;3A");
        const bool topDown = e == Event::ArrowDownCtrl || e == Event::Special("\x1b[1;3B");
        if (topUp || topDown) {
            if (tlm_.view.view == TlmView::Dashboard) {
                displayScroll_ = std::max(0, displayScroll_ + (topUp ? -3 : 3));
            } else {
                monitorScroll_ = std::max(0, monitorScroll_ + (topUp ? 5 : -5));
            }
            return true;
        }
        if (e == Event::F1 || e == Event::F2 || e == Event::F4) {
            tlm_.view.view = e == Event::F1 ? TlmView::Dashboard
                           : e == Event::F2 ? TlmView::Monitor : TlmView::Stats;
            listFocused_ = false;
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
        if (e == Event::CtrlD) {
            if (input_.empty()) {
                quit();
            }
            return true;
        }
        return false;
    }

    // Keys while the checklist has the focus. Returns false for keys it does
    // not use (F-keys, Ctrl-C, ...). A printable key gives the focus back to
    // the command line and is typed there.
    bool onChecklistEvent(const Event& e)
    {
        const int last = std::max(0, static_cast<int>(items_.size()) - 1);
        const int page = std::max(1, listPage_);
        if (e == Event::Escape) {
            listFocused_ = false;
        } else if (e == Event::ArrowUp) {
            listCursor_ = std::max(0, listCursor_ - 1);
        } else if (e == Event::ArrowDown) {
            listCursor_ = std::min(last, listCursor_ + 1);
        } else if (e == Event::PageUp) {
            listCursor_ = std::max(0, listCursor_ - page);
        } else if (e == Event::PageDown) {
            listCursor_ = std::min(last, listCursor_ + page);
        } else if (e == Event::Home) {
            listCursor_ = 0;
        } else if (e == Event::End) {
            listCursor_ = last;
        } else if (e == Event::Character(' ') || e == Event::Return) {
            toggleCursorItem();
        } else if (e == Event::Tab || e == Event::TabReverse) {
            // nothing to complete here
        } else if (e.is_character()) {
            listFocused_ = false;
            return false;   // the input line receives the character
        } else {
            return false;
        }
        return true;
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
        if (App* s = screen_.load()) {
            s->Exit();
        }
    }

    // ---- rendering -----------------------------------------------------------

    Element render(const Component& input)
    {
        const Dimensions dims = Terminal::Size();
        width_ = std::max(40, dims.dimx);
        outputRows_ = std::clamp(dims.dimy / 5, 3, 8);
        const int listRows = completionRows();
        const int bottomRows = outputRows_ + 1 + listRows + 1;
        const int topRows = std::max(4, dims.dimy - 2 - 1 - bottomRows);

        refreshSnapshot();

        Element top = vbox({linkLine(), separatorLight(), view(topRows - 2) | flex})
                    | size(HEIGHT, EQUAL, topRows);
        Element bottom = vbox({outputArea(), inputLine(input), completionArea(), hintLine()});
        return vbox({top, separatorDouble(), bottom}) | border;
    }

    void refreshSnapshot()
    {
        if (tlm_.store == nullptr) {
            return;
        }
        if (tlm_.view.paused && frozen_) {
            return;
        }
        now_ = TlmClock::now();
        snapshot_ = tlm_.store->snapshot(now_);
        frozen_ = tlm_.view.paused;
        frozenMonitor_.clear();
        if (frozen_) {
            frozenMonitor_ = tlm_.store->monitorTail(tlm_.view.monitorFilter, 2000);
        }
    }

    Element linkLine()
    {
        Elements parts;
        if (tlm_.store == nullptr) {
            return hbox({text(" no telemetry configured for target '" + service_.targetName()
                              + "' (add a \"telemetry\" block in targets.json)") | dim, filler(),
                         viewTabs()});
        }

        const TlmCounters& c = snapshot_.counters;
        const TlmTime now = frozen_ ? now_ : TlmClock::now();
        const std::optional<double> age = c.lastRx ? std::optional<double>(secondsBetween(*c.lastRx, now))
                                                   : std::nullopt;
        std::string error = tlm_.service == nullptr ? tlm_.receiverError : tlm_.service->lastError();

        Color dot = Color::GrayDark;
        if (!error.empty()) {
            dot = Color::Red;
        } else if (age) {
            dot = *age < 5 ? Color::Green : Color::Yellow;
        }
        const std::string where = tlm_.service != nullptr ? tlm_.service->describe() : "receiver closed";
        parts.push_back(text(" TLM ") | bold);
        parts.push_back(text(where + " "));
        parts.push_back(text("●") | color(dot));
        if (!error.empty()) {
            // Drop the "target 'x': telemetry: " context; the line has little room.
            if (const auto at = error.find("udp receiver: "); at != std::string::npos) {
                error = error.substr(at + 14);
            }
            parts.push_back(text(" " + error) | color(Color::Red) | flex_shrink);
        } else {
            std::string stats = " " + std::to_string(c.received) + " pkts";
            if (c.unknown != 0) stats += " · " + std::to_string(c.unknown) + " unknown";
            if (c.malformed != 0) stats += " · " + std::to_string(c.malformed) + " malformed";
            if (c.gaps != 0) stats += " · " + std::to_string(c.gaps) + " gaps";
            stats += age ? " · last " + ageText(*age) + " ago" : " · waiting for packets";
            parts.push_back(text(stats) | flex_shrink);
        }
        parts.push_back(filler());
        if (frozen_) {
            parts.push_back(text(" PAUSED (F5) ") | inverted | color(Color::Yellow));
            parts.push_back(text(" "));
        }
        parts.push_back(viewTabs());
        const bool active = age && *age < 1.0 && !frozen_;
        parts.push_back(text(" ") );
        parts.push_back(text(active ? kSpinner[tick_ % 10] : "·") | color(Color::Cyan));
        parts.push_back(text(" "));
        return hbox(std::move(parts));
    }

    Element viewTabs()
    {
        const auto tab = [this](const char* label, TlmView v) {
            Element e = text(label);
            return tlm_.view.view == v ? e | inverted : e | dim;
        };
        return hbox({tab(" F1 dash ", TlmView::Dashboard), text(" "), tab(" F2 monitor ", TlmView::Monitor),
                     text(" "), tab(" F4 stats ", TlmView::Stats)});
    }

    Element view(int rows)
    {
        if (tlm_.store == nullptr) {
            return vbox({text(""), text("  Telemetry is not configured for this target.") | dim,
                         text("  Commands work as usual below.") | dim}) | flex;
        }
        switch (tlm_.view.view) {
        case TlmView::Dashboard: return dashboard(rows);
        case TlmView::Monitor: return monitor(rows);
        case TlmView::Stats: return stats();
        }
        return text("");
    }

    // ---- F1 dashboard ----------------------------------------------------------
    //
    //   ┌ packets (F6) ──────┐┌ ds.hk 0x08B8 ─ seq 7 · 0.3 s ┐┌ ? 0x0803 ─ seq 4 ┐
    //   │ [x] ds.hk        ● ││ cmd_accepted          9 ▲   ││ 0000: 08 03 C0 …  │
    //   │ [ ] to.hk        ○ ││ ...                         ││                   │
    //   │ [x] ? 0x0803     ● │└─────────────────────────────┘└───────────────────┘
    //   └────────────────────┘
    //   ─ events ─────────────────────────────────────────────────────────────────
    //
    // Left: every non-event packet of the catalog (● received, ○ not yet), then
    // MIDs received without a definition. Right: the ticked ones, in checklist
    // order, wrapped into rows; Ctrl+↑↓ scrolls them by lines.

    struct ChecklistItem {
        std::uint16_t mid = 0;
        std::string label;                    // "ds.hk" or "? 0x0803"
        const PacketState* packet = nullptr;  // defined and received
        const UnknownMid* unknown = nullptr;  // received without a definition
        const TlmPacketDef* def = nullptr;    // defined (received or not)
    };

    void buildChecklist()
    {
        items_.clear();
        for (const auto& def : tlm_.store->catalog().packets()) {
            if (def.isEvent) {
                continue;   // shown in the event log
            }
            ChecklistItem item;
            item.mid = def.mid;
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
        listPage_ = visible;

        constexpr std::size_t kLabelWidth = static_cast<std::size_t>(kListWidth) - 10;
        Elements lines;
        if (overflow) {
            lines.push_back(text(listScroll_ > 0 ? " ▲ " + std::to_string(listScroll_) + " more" : "") | dim);
        }
        for (int i = listScroll_; i < std::min(count, listScroll_ + visible); ++i) {
            const ChecklistItem& item = items_[static_cast<std::size_t>(i)];
            const bool ticked = tlm_.view.shown.count(item.mid) != 0;
            const bool received = item.packet != nullptr || item.unknown != nullptr;
            std::string label = item.label;
            if (label.size() > kLabelWidth) {
                label = label.substr(0, kLabelWidth - 1) + "…";
            }
            Element line = hbox({text(ticked ? " [x] " : " [ ] ") | color(pick(ticked, Color::Cyan, Color::Default)),
                                 text(label) | color(pick(item.def == nullptr, Color::Magenta, Color::Default)),
                                 filler(),
                                 text(received ? "● " : "○ ") | color(pick(received, Color::Green, Color::GrayDark))});
            if (i == listCursor_ && listFocused_) {
                line = line | inverted;
            }
            lines.push_back(line);
        }
        if (overflow) {
            const int below = count - listScroll_ - visible;
            lines.push_back(text(below > 0 ? " ▼ " + std::to_string(below) + " more" : "") | dim);
        }
        if (items_.empty()) {
            lines.push_back(text(" no packets defined") | dim);
        }

        Element title = text(listFocused_ ? " packets ↑↓ Space " : " packets (F6) ");
        title = listFocused_ ? title | bold | color(Color::Cyan) : title | dim;
        Element box = window(title, vbox(std::move(lines)) | yframe | flex);
        if (listFocused_) {
            box = box | color(Color::Cyan);
        }
        return box | size(WIDTH, EQUAL, kListWidth);
    }

    // A decoded packet: fields in as many columns as needed to fit `maxRows`
    // (within `maxWidth`). Columns are as wide as the longest name and value.
    Element fieldsPanel(const PacketState& p, int maxRows, int maxWidth, int& width, int& height)
    {
        const TlmTime now = frozen_ ? now_ : TlmClock::now();
        const double age = secondsBetween(p.lastRx, now);

        std::size_t nameWidth = 4;
        std::size_t valueWidth = 4;
        for (const auto& v : p.values) {
            nameWidth = std::max(nameWidth, v.name.size());
            valueWidth = std::max(valueWidth, v.text.size());
        }
        // " name  value ▲ ": 1 + name + 2 + value + 3
        const auto fixed = valueWidth + 6;
        const auto widest = static_cast<std::size_t>(std::max(20, maxWidth - 2));
        if (nameWidth + fixed > widest) {
            nameWidth = widest > fixed + 4 ? widest - fixed : 4;
        }
        const int columnWidth = static_cast<int>(nameWidth + fixed);

        Elements rows;
        for (std::size_t i = 0; i < p.values.size(); ++i) {
            const DecodedValue& v = p.values[i];
            const bool recent = p.changedAt[i] != TlmTime{} && secondsBetween(p.changedAt[i], now) < 1.0;
            const char* arrow = p.trend[i] > 0 ? "▲" : p.trend[i] < 0 ? "▼" : " ";
            std::string name = v.name.size() > nameWidth ? v.name.substr(0, nameWidth - 1) + "…" : v.name;
            Element value = text(v.text);
            if (v.missing) {
                value = value | dim;
            } else if (recent) {
                value = value | bold | color(Color::Yellow);
            }
            rows.push_back(hbox({text(" " + padRight(name, nameWidth) + "  ") | dim, filler(), value,
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
                cols.push_back(separatorLight());
            }
            cols.push_back(vbox(std::move(col)));
        }
        width = columns * columnWidth + (columns - 1) + 2;
        height = perColumn + 2;

        Element head = hbox({text(" " + p.def->fullName() + " " + toHexString(p.def->mid, 4) + " ") | bold,
                             filler(), text("seq " + std::to_string(p.sequence) + " · ×"
                                            + std::to_string(p.count) + " · "),
                             text(ageText(age)) | color(ageColor(age)), text(" ")});
        return window(head, hbox(std::move(cols))) | size(WIDTH, EQUAL, width);
    }

    // A MID without a definition: header facts and the last packet as hex.
    Element hexPanel(const UnknownMid& u, int& width, int& height)
    {
        const TlmTime now = frozen_ ? now_ : TlmClock::now();
        const double age = secondsBetween(u.lastRx, now);
        Elements lines;
        std::size_t longest = 0;
        std::string dump = hexdump(u.lastPacket);
        std::size_t from = 0;
        while (from < dump.size()) {
            const std::size_t nl = dump.find('\n', from);
            std::string line = dump.substr(from, nl == std::string::npos ? std::string::npos : nl - from);
            longest = std::max(longest, line.size());
            lines.push_back(text(" " + line));
            from = nl == std::string::npos ? dump.size() : nl + 1;
        }
        lines.push_back(text(" no definition: bytes 0-11 are the header") | dim);
        width = std::max(44, static_cast<int>(longest) + 4);
        height = static_cast<int>(lines.size()) + 2;
        Element head = hbox({text(" ? " + toHexString(u.mid, 4) + " ") | bold | color(Color::Magenta), filler(),
                             text("seq " + std::to_string(u.sequence) + " · " + std::to_string(u.lastLength)
                                  + " B · ×" + std::to_string(u.count) + " · "),
                             text(ageText(age)) | color(ageColor(age)), text(" ")});
        return window(head, vbox(std::move(lines))) | size(WIDTH, EQUAL, width);
    }

    Element waitingPanel(const ChecklistItem& item, int& width, int& height)
    {
        width = std::max(30, static_cast<int>(item.label.size()) + 18);
        height = 3;
        return window(text(" " + item.label + " " + toHexString(item.mid, 4) + " ") | bold,
                      text(" waiting for the first packet") | dim)
             | size(WIDTH, EQUAL, width);
    }

    Element displayArea(int rows, int areaWidth)
    {
        struct Placed {
            Element element;
            int width;
            int height;
        };
        std::vector<Placed> panels;
        for (const auto& item : items_) {
            if (tlm_.view.shown.count(item.mid) == 0) {
                continue;
            }
            Placed p{nullptr, 0, 0};
            if (item.packet != nullptr) {
                p.element = fieldsPanel(*item.packet, rows, areaWidth, p.width, p.height);
            } else if (item.unknown != nullptr) {
                p.element = hexPanel(*item.unknown, p.width, p.height);
            } else {
                p.element = waitingPanel(item, p.width, p.height);
            }
            panels.push_back(std::move(p));
        }

        if (panels.empty()) {
            return vbox({text(""),
                         text("  Tick packets to show them here:") | dim,
                         text("    F6, then ↑↓ and Space   (Esc returns to the command line)") | dim,
                         text("    or type  tlm show ds.hk   /   tlm show all") | dim,
                         text(""),
                         text(snapshot_.counters.received == 0
                                  ? "  No telemetry yet: start it with e.g.  fsw to enable_output 127.0.0.1 5011"
                                  : "") | dim});
        }

        // Flow layout: left to right, wrapping when the next panel does not fit.
        Elements lines;
        Elements line;
        int lineWidth = 0;
        int lineHeight = 0;
        int total = 0;
        for (auto& p : panels) {
            if (!line.empty() && lineWidth + p.width > areaWidth) {
                lines.push_back(hbox(std::move(line)));
                line = Elements{};
                total += lineHeight;
                lineWidth = 0;
                lineHeight = 0;
            }
            line.push_back(p.element);
            lineWidth += p.width;
            lineHeight = std::max(lineHeight, p.height);
        }
        lines.push_back(hbox(std::move(line)));
        total += lineHeight;

        displayScroll_ = std::clamp(displayScroll_, 0, std::max(0, total - rows));
        displayMore_ = total - rows - displayScroll_;
        return vbox(std::move(lines)) | focusPosition(0, displayScroll_ + (rows - 1) / 2) | yframe;
    }

    Element dashboard(int rows)
    {
        const int eventRows = std::max(3, rows / 4);
        const int panelRows = std::max(3, rows - eventRows - 1);
        buildChecklist();

        std::string scrollNote;
        Element display = displayArea(panelRows, width_ - 2 - kListWidth);
        if (displayScroll_ > 0 || displayMore_ > 0) {
            scrollNote = " Ctrl+↑↓ scroll: " + std::to_string(displayScroll_) + " line(s) above, "
                       + std::to_string(std::max(0, displayMore_)) + " below ";
        }
        return vbox({hbox({checklist(panelRows), display | flex}) | size(HEIGHT, EQUAL, panelRows),
                     hbox({text("─ events ") | dim, separatorLight() | flex, text(scrollNote) | dim}),
                     eventLog(eventRows) | size(HEIGHT, EQUAL, eventRows)});
    }

    Element eventLog(int rows)
    {
        const auto& events = snapshot_.events;
        if (events.empty()) {
            return text("  no events yet") | dim;
        }
        Elements lines;
        const std::size_t first = events.size() > static_cast<std::size_t>(rows)
                                      ? events.size() - static_cast<std::size_t>(rows) : 0;
        for (std::size_t i = first; i < events.size(); ++i) {
            const EventRecord& e = events[i];
            const Color c = eventColor(e.eventType);
            Element type = text(padRight(e.typeName, 6)) | color(c);
            if (e.eventType == 4) {
                type = type | bold;
            }
            Element message = text(" " + e.message);
            if (e.eventType >= 3) {
                message = message | color(c);
            }
            lines.push_back(hbox({text(" " + clockText(e.rxTime) + "  ") | dim, type,
                                  text(" " + padRight(e.app, 10) + " "),
                                  text(padRight(std::to_string(e.eventId), 5)) | dim, message}));
        }
        return vbox(std::move(lines));
    }

    // ---- F2 monitor -------------------------------------------------------------

    Element monitor(int rows)
    {
        const int dataRows = std::max(1, rows - 1);
        std::vector<PacketRecord> records;
        if (frozen_) {
            records = frozenMonitor_;
        } else {
            records = tlm_.store->monitorTail(tlm_.view.monitorFilter,
                                              static_cast<std::size_t>(dataRows + monitorScroll_));
        }
        monitorScroll_ = std::min(monitorScroll_, std::max(0, static_cast<int>(records.size()) - dataRows));
        const std::size_t end = records.size() - static_cast<std::size_t>(monitorScroll_);
        const std::size_t begin = end > static_cast<std::size_t>(dataRows) ? end - static_cast<std::size_t>(dataRows) : 0;

        Elements lines;
        lines.push_back(hbox({cell(" time", 14), cell("dir", 4), cell("mid", 8), cell("seq", 7),
                              cell("len", 6), cell("status", 11), text("packet"), filler(),
                              text("filter: " + tlm_.view.monitorFilter.describe()
                                   + (monitorScroll_ > 0 ? "  ▲ " + std::to_string(monitorScroll_) + " newer below" : "")
                                   + " ")})
                        | bold);
        for (std::size_t i = begin; i < end; ++i) {
            const PacketRecord& r = records[i];
            const bool tx = r.dir == PacketDir::Tx;
            Element status = tx ? text("sent") | color(Color::Cyan)
                                : text(toString(r.status)) | color(statusColor(r.status));
            std::string name = r.name.empty() ? "?" : r.name;
            lines.push_back(hbox({cell(" " + clockText(r.time), 14),
                                  cell(tx ? "TX" : "RX", 4) | color(pick(tx, Color::Cyan, Color::Default)),
                                  cell(toHexString(r.mid, 4), 8), cell(std::to_string(r.sequence), 7),
                                  cell(std::to_string(r.length), 6), status | size(WIDTH, EQUAL, 11),
                                  text(name), text(r.detail.empty() ? "" : "  " + r.detail) | dim}));
        }
        if (records.empty()) {
            lines.push_back(text("  no packets" + std::string(tlm_.view.monitorFilter.describe() == "all"
                                                                   ? "" : " matching the filter")
                                 + " yet") | dim);
        }
        return vbox(std::move(lines));
    }

    // ---- F4 stats ----------------------------------------------------------------

    Element stats()
    {
        const TlmTime now = frozen_ ? now_ : TlmClock::now();
        Elements lines;
        lines.push_back(hbox({cell(" mid", 9), cell("packet", 22), cell("count", 9), cell("rate", 10),
                              cell("gaps", 7), cell("len err", 9), text("last seen")}) | bold);
        for (const auto& s : snapshot_.stats) {
            char rate[32];
            std::snprintf(rate, sizeof rate, "%.1f Hz", s.rateHz);
            const double age = secondsBetween(s.lastRx, now);
            lines.push_back(hbox({cell(" " + toHexString(s.mid, 4), 9),
                                  cell(s.name.empty() ? "? unknown" : s.name, 22)
                                      | color(pick(s.name.empty(), Color::Magenta, Color::Default)),
                                  cell(std::to_string(s.count), 9), cell(rate, 10),
                                  cell(std::to_string(s.gaps), 7) | color(pick(s.gaps != 0, Color::Yellow, Color::Default)),
                                  cell(std::to_string(s.lengthErrors), 9)
                                      | color(pick(s.lengthErrors != 0, Color::Yellow, Color::Default)),
                                  text(ageText(age) + " ago") | color(ageColor(age))}));
        }
        const TlmCounters& c = snapshot_.counters;
        lines.push_back(separatorLight());
        lines.push_back(text(" total " + std::to_string(c.received) + " received, " + std::to_string(c.sent)
                             + " sent, " + std::to_string(c.unknown) + " unknown, "
                             + std::to_string(c.malformed) + " malformed, " + std::to_string(c.gaps)
                             + " sequence gaps") | dim);
        return vbox(std::move(lines)) | frame;
    }

    // ---- bottom: commanding ----------------------------------------------------

    Element outputArea()
    {
        const auto& lines = output_.lines();
        const std::size_t end = lines.size() - std::min(lines.size(), static_cast<std::size_t>(outputScroll_));
        const std::size_t begin = end > static_cast<std::size_t>(outputRows_) ? end - static_cast<std::size_t>(outputRows_) : 0;
        Elements rows;
        for (std::size_t i = begin; i < end; ++i) {
            const std::string& l = lines[i];
            Element e = text(" " + l);
            if (l.rfind("> ", 0) == 0) {
                e = e | bold | color(Color::Cyan);
            } else if (l.rfind("error", 0) == 0 || l.rfind("Unknown command", 0) == 0) {
                e = e | color(Color::Red);
            } else if (l.rfind("warning", 0) == 0) {
                e = e | color(Color::Yellow);
            }
            rows.push_back(e);
        }
        while (rows.size() < static_cast<std::size_t>(outputRows_)) {
            rows.insert(rows.begin(), text(""));
        }
        if (outputScroll_ > 0) {
            rows.back() = hbox({rows.back(), filler(),
                                text(" ▼ " + std::to_string(outputScroll_) + " more (PgDn) ") | inverted | dim});
        }
        return vbox(std::move(rows));
    }

    Element inputLine(const Component& input)
    {
        Element line = hbox({text(" " + prompt_) | bold | color(Color::Green), input->Render() | flex});
        return listFocused_ ? line | dim : line;   // keys go to the checklist meanwhile
    }

    int completionRows() const
    {
        if (!completion_.items.empty()) {
            return std::min(kMaxCompletionRows, static_cast<int>(completion_.items.size()));
        }
        return std::min(kMaxCompletionRows, static_cast<int>(completion_.hints.size()));
    }

    Element completionArea()
    {
        Elements rows;
        if (!completion_.items.empty()) {
            std::size_t width = 0;
            for (const auto& item : completion_.items) {
                width = std::max(width, item.word.size());
            }
            const std::size_t shown = std::min(completion_.items.size(), static_cast<std::size_t>(kMaxCompletionRows));
            for (std::size_t i = 0; i < shown; ++i) {
                const auto& item = completion_.items[i];
                if (i + 1 == shown && shown < completion_.items.size()) {
                    rows.push_back(text("   … " + std::to_string(completion_.items.size() - shown + 1)
                                        + " more: type more letters") | dim);
                    break;
                }
                rows.push_back(hbox({text("   " + padRight(item.word, width + 3)) | color(Color::Cyan),
                                     text(item.description) | dim}));
            }
        } else {
            for (std::size_t i = 0; i < completion_.hints.size() && i < static_cast<std::size_t>(kMaxCompletionRows); ++i) {
                rows.push_back(text("   " + completion_.hints[i]) | dim);
            }
        }
        return vbox(std::move(rows));
    }

    Element hintLine()
    {
        if (listFocused_) {
            return text(" CHECKLIST: ↑↓ move · Space/Enter tick · PgUp/PgDn page · Esc/F6 back to the "
                        "command line (or just type)")
                 | color(Color::Cyan);
        }
        return text(" Enter run · Tab complete · ↑↓ history · PgUp/PgDn output · F6 packet list · "
                    "Ctrl+↑↓ scroll view · F1/F2/F4 views · F5 pause · Ctrl-C clear/quit")
             | dim;
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

    int width_ = 80;
    int outputRows_ = 5;
    int outputScroll_ = 0;
    int displayScroll_ = 0;              // dashboard display area, in lines
    int displayMore_ = 0;                // lines below the visible part
    bool listFocused_ = false;           // keys go to the checklist (F6)
    int listCursor_ = 0;
    int listScroll_ = 0;
    int listPage_ = 1;                   // visible checklist rows (PgUp/PgDn step)
    std::vector<ChecklistItem> items_;   // checklist of the last frame
    int monitorScroll_ = 0;
    unsigned tick_ = 0;

    TlmSnapshot snapshot_;
    TlmTime now_{};
    bool frozen_ = false;
    std::vector<PacketRecord> frozenMonitor_;
};

} // namespace

int runTui(cli::Cli& cli, CommandService& service, TlmContext& tlm, const std::vector<std::string>& banner)
{
    Tui tui(cli, service, tlm, banner);
    return tui.run();
}

} // namespace mcs::ui
