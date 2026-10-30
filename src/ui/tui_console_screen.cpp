// Full-screen session, F1 console: events / FSW console, transfers, command output, input, completion.

#include "ui/tui_app.hpp"

namespace mcs::ui::tui {

// ---- F1 console -------------------------------------------------------------
//
//   ╭ EVENTS ─────────╮   ~40%; F3 swaps in the FSW CONSOLE
//   ╰─────────────────╯
//   ╭ COMMAND HISTORY ─╮   the rest
//   ╰─────────────────╯
//    mcs> █              input line, then the Tab list

Element Tui::consoleScreen(int rows, const Component& input)
{
    const int listRows = completionRows(rows);
    const int stripBox = transfersRows();   // 0 when nothing is being transferred
    const int paneRows = std::max(8, rows - 1 - listRows - stripBox);
    const int eventsRows = std::max(4, paneRows * 2 / 5);
    const int outputBox = std::max(4, paneRows - eventsRows);
    outputRows_ = outputBox - 2;
    outputWidth_ = std::max(20, width_ - 4);   // border + leading space + scrollbar

    Element top = tlm_.view.pane == F1Pane::Console ? fswConsolePane(eventsRows) : eventsPane(eventsRows);
    Elements panes{std::move(top) | size(HEIGHT, EQUAL, eventsRows)};
    if (stripBox > 0) {
        panes.push_back(transfersPane() | size(HEIGHT, EQUAL, stripBox));
    }
    panes.push_back(outputPane() | size(HEIGHT, EQUAL, outputBox));
    panes.push_back(inputLine(input));
    panes.push_back(completionArea(listRows));
    return vbox(std::move(panes));
}

int Tui::activeTransfers() const
{
    int n = 0;
    for (const auto& s : transfers_) {
        n += cfdp::isFinished(s.state) ? 0 : 1;
    }
    return n;
}

// Height of the strip with its border; 0 hides it.
int Tui::transfersRows() const
{
    const int n = activeTransfers();
    if (n == 0) {
        return 0;
    }
    return std::min(n, kMaxStripRows) + (n > kMaxStripRows ? 1 : 0) + 2;
}

Element Tui::stripRow(const StripRow& r)
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

Element Tui::transfersPane()
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
    return window(paneTitle("TRANSFERS", std::to_string(total) + " active"), vbox(std::move(rows)))
         | color(Color::Default);
}

Element Tui::paneTitle(const std::string& name, const std::string& count)
{
    return hbox({text(" " + name + " ") | bold, text(count.empty() ? "" : count + " ") | dim, filler()});
}

Element Tui::eventsPane(int box)
{
    const int dataRows = std::max(1, box - 3);   // border + column header
    if (tlm_.store == nullptr) {
        return window(paneTitle("EVENTS", ""),
                      text(" telemetry is not configured for target '" + service_.targetName()
                           + "' (add a \"telemetry\" block in targets.json)") | dim);
    }
    const auto& events = snapshot_.events;
    const int total = static_cast<int>(events.size());
    eventScroll_ = std::clamp(eventScroll_, 0, std::max(0, total - dataRows));
    const int end = total - eventScroll_;
    const int begin = std::max(0, end - dataRows);

    Element header = hbox({cell(" time", 15), cell("type", 7), cell("app", 12), cell("id", 6),
                           text("message")}) | dim | bold;
    Elements lines;
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
    return window(paneTitle("EVENTS", ""),
                  vbox({std::move(header),
                        hbox({vbox(std::move(lines)) | flex,
                              scrollBar(Bar::Log, total, dataRows, total - dataRows - eventScroll_)})
                            | flex}));
}

// ---- F1 FSW CONSOLE (F3): text lines from the "console" receiver -----------------
//
//   ╭ FSW CONSOLE 1532 ──────────── ▲ 12 older   udp://0.0.0.0:8898 ● last 0.4 s ╮
//   │ 10:41:07.112  EVS Port1 66/1/MAIN 1: MAIN App Initialized.                   │

Element Tui::consoleLink()
{
    std::string error = tlm_.consoleService != nullptr ? tlm_.consoleService->lastError()
                      : tlm_.consoleError.empty()       ? "receiver not open"
                                                        : tlm_.consoleError;
    if (!error.empty()) {
        if (const auto at = error.find("udp receiver: "); at != std::string::npos) {
            error = error.substr(at + 14);   // drop the "target 'x': console: " context
        }
        return hbox({text(" ●") | color(Color::Red), text(" " + error + " ") | color(Color::Red)});
    }
    const bool received = console_.lastRx.has_value();
    const double age = received ? secondsBetween(*console_.lastRx, displayNow()) : 0.0;
    const Color dot = !received ? Color::GrayDark : age < kFreshSeconds ? Color::Green : Color::Yellow;
    const std::string where = width_ < 120 ? "" : tlm_.consoleService->describe() + " ";
    return hbox({text(" " + where) | dim, text("●") | color(dot),
                 text(received ? " last " + ageText(age) + " " : " waiting ") | dim});
}

Element Tui::fswConsolePane(int box)
{
    if (tlm_.console == nullptr) {
        return window(paneTitle("FSW CONSOLE", ""),
                      text(" no console configured for target '" + service_.targetName()
                           + "' (add a \"console\" block in targets.json)") | dim);
    }
    const int dataRows = std::max(1, box - 2);   // border
    const auto& lines = console_.lines;
    const int total = static_cast<int>(lines.size());
    consoleScroll_ = std::clamp(consoleScroll_, 0, std::max(0, total - dataRows));
    const int end = total - consoleScroll_;
    const int begin = std::max(0, end - dataRows);

    Elements rows;
    for (int i = begin; i < end; ++i) {
        const ConsoleLine& l = lines[static_cast<std::size_t>(i)];
        rows.push_back(hbox({cell(" " + clockText(l.rxTime), 15) | dim, text(l.text)}));
    }
    if (lines.empty()) {
        const bool open = tlm_.consoleService != nullptr;
        rows.push_back(text(open ? " no console text yet (waiting on " + tlm_.consoleService->describe() + ")"
                                 : " console receiver not open")
                       | dim);
    }
    Element title = hbox({paneTitle("FSW CONSOLE", "") | flex, consoleLink()});
    return window(std::move(title),
                  hbox({vbox(std::move(rows)) | flex,
                        scrollBar(Bar::Log, total, dataRows, total - dataRows - consoleScroll_)}));
}

// One display row of a stored line: help names in cyan, the rest styled by
// what the line is (an echoed command, an error, ...).
Element Tui::outputRow(const OutputLine& line, const std::string& row, bool whole)
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
    // Test scripts: PASS/FAIL lines (FAIL and error lines may start with "file.mcs:7: ").
    const auto has = [&t](const char* part) { return t.find(part) != std::string::npos; };
    if (t.rfind("PASS ", 0) == 0 || t.rfind("result: PASS", 0) == 0) {
        return e | color(Color::Green);
    }
    if (t.rfind("FAIL ", 0) == 0 || has(": FAIL ") || t.rfind("result: FAIL", 0) == 0) {
        return e | bold | color(Color::Red);
    }
    if (t.rfind("=== ", 0) == 0) {
        if (has(" FAIL")) {
            return e | bold | color(Color::Red);
        }
        return has(" PASS") ? e | bold | color(Color::Green) : e | bold;
    }
    if (t.rfind("error", 0) == 0 || t.rfind("Unknown command", 0) == 0 || has(": error: ")) {
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
Element Tui::outputPane()
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
    outputScroll_ = std::min(outputScroll_, static_cast<int>(display.size() > rows ? display.size() - rows : 0));
    const std::size_t end = display.size() - static_cast<std::size_t>(outputScroll_);
    const std::size_t begin = end > rows ? end - rows : 0;

    Elements out;
    for (std::size_t i = begin; i < end; ++i) {
        out.push_back(outputRow(lines[display[i].line], display[i].text, display[i].whole));
    }
    // Rows of the whole output, for the scrollbar: only long lines need wrapping.
    int total = 0;
    for (const auto& l : lines) {
        total += displayWidth(l.text) <= static_cast<std::size_t>(outputWidth_)
                     ? 1 : static_cast<int>(wrapLine(l.text, outputWidth_).size());
    }
    while (out.size() < rows) {
        out.insert(out.begin(), text(""));
    }
    const int shown = static_cast<int>(rows);
    return window(paneTitle("COMMAND HISTORY", ""),
                  hbox({vbox(std::move(out)) | flex,
                        scrollBar(Bar::Output, total, shown, total - shown - outputScroll_)}));
}

Element Tui::inputLine(const Component& input)
{
    return hbox({text(" " + prompt_) | bold | color(Color::Green), input->Render() | flex});
}

// The Tab list: candidate names in a grid (completion.hpp gridRows), or the
// hints for an argument.
std::vector<std::string> Tui::completionLines() const
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
int Tui::completionRows(int rows) const
{
    return std::min(std::max(6, rows / 2), static_cast<int>(completionLines().size()));
}

Element Tui::completionArea(int rows)
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

} // namespace mcs::ui::tui
