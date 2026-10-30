#include "ui/tui_session.hpp"

#include "ui/tui_app.hpp"

namespace mcs::ui {

static_assert(kCompletionHelpSeparator == cli::CompletionHelpSeparator,
              "ui/completion.hpp must use the patched cli separator");
static_assert(kCompletionHintMarker == cli::CompletionHintMarker,
              "ui/completion.hpp must use the patched cli hint marker");

namespace tui {

namespace {

// While a script runs, typed lines may look and control, not command: the
// script's commands must not interleave with typed ones.
bool allowedWhileScriptRuns(const std::string& line)
{
    std::istringstream words(line);
    std::string first;
    std::string second;
    words >> first >> second;
    first = toLower(first);
    static const std::set<std::string> kAlways{"run", "tlm", "help", "target", "history", "exit"};
    static const std::set<std::string> kFileControl{"list", "status", "cancel", "abandon", "suspend", "resume"};
    return kAlways.count(first) != 0 || (first == "file" && kFileControl.count(toLower(second)) != 0);
}

} // namespace

int Tui::run()
{
    App screen = App::Fullscreen();
    screen.ForceHandleCtrlC(false);   // Ctrl-C clears the line; quits when it is empty
    // Mouse on for the panels' scrollbars (drag to scroll). Terminals still
    // select text for copying with Shift+drag.
    screen.TrackMouse(true);
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
    if (tlm_.consoleService != nullptr) {
        tlm_.consoleService->start([this](bool arrived) { onTelemetry(arrived); });
    }
    if (tlm_.files != nullptr) {
        tlm_.files->setOnChange([this] { requestRedraw(); });   // progress, notices
    }
    if (tlm_.script != nullptr) {
        tlm_.script->setOnChange([this] { requestRedraw(); });  // script output
    }
    screen.Loop(root);
    if (tlm_.script != nullptr) {
        // Before this session's cli session goes away (see ScriptRunner).
        tlm_.script->setOnChange({});
        tlm_.script->stop();
        tlm_.script->join();
    }
    if (tlm_.files != nullptr) {
        tlm_.files->setOnChange({});
    }
    if (tlm_.service != nullptr) {
        tlm_.service->stop();
    }
    if (tlm_.consoleService != nullptr) {
        tlm_.consoleService->stop();
    }
    screen_ = nullptr;
    return 0;
}

// Receiver threads (telemetry, console): ask for a redraw. Data is drawn
// at up to ~20 frames per second; a timeout without data (every 100 ms)
// only keeps the ages and the spinner moving, at ~4 frames per second. A
// skipped packet is drawn by the next call.
void Tui::onTelemetry(bool arrived)
{
    requestRedraw(arrived ? kDataFrameGap : kIdleFrameGap);
}

// Any thread (receivers, file transfer worker, script runner): ask for a
// redraw, at most one per `minGap`.
void Tui::requestRedraw(std::chrono::milliseconds minGap)
{
    const auto gap = std::chrono::duration_cast<std::chrono::steady_clock::duration>(minGap).count();
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    if (now - lastPost_.load() < gap) {
        return;
    }
    lastPost_ = now;
    if (App* s = screen_.load()) {
        s->PostEvent(Event::Custom);
    }
}

// ---- keys ----------------------------------------------------------------

bool Tui::onEvent(const Event& e)
{
    if (e == Event::Custom) {
        ++tick_;
        return false;
    }
    if (e.is_mouse()) {
        Event copy = e;
        onMouse(copy.mouse());
        return true;   // never reaches the input line
    }
    if (e == Event::F1 || e == Event::F2) {
        tlm_.view.view = e == Event::F1 ? TlmView::Console : TlmView::Telemetry;
        return true;
    }
    if (e == Event::F3) {   // from F2 too: show the other pane on F1
        tlm_.view.pane = tlm_.view.pane == F1Pane::Events ? F1Pane::Console : F1Pane::Events;
        tlm_.view.view = TlmView::Console;
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
bool Tui::onConsoleKey(const Event& e)
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
        int& scroll = tlm_.view.pane == F1Pane::Console ? consoleScroll_ : eventScroll_;
        scroll = std::max(0, scroll + (older ? 3 : -3));
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
bool Tui::onTelemetryKey(const Event& e)
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

void Tui::submit()
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
    if (tlm_.script != nullptr && tlm_.script->status().running && !allowedWhileScriptRuns(line)) {
        output_.addLine("error: a script is running: only run, tlm, help, target and file "
                        "list|status|cancel|abandon|suspend|resume now ('run stop' ends it)");
        return;
    }
    session_.Feed(line);
    out_.flush();
    updatePrompt();
    if (exitRequested_) {
        quit();
    }
}

void Tui::complete()
{
    completion_ = applyCompletion(input_, session_.GetCompletions(input_));
    if (completion_.newLine) {
        input_ = *completion_.newLine;
        cursor_ = static_cast<int>(input_.size());
    }
}

void Tui::updatePrompt()
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

void Tui::quit()
{
    // Quitting stops a running script: say so once, quit (and stop it) on the next try.
    if (tlm_.script != nullptr && !scriptQuitWarned_) {
        const ScriptRunner::Status st = tlm_.script->status();
        if (st.running) {
            scriptQuitWarned_ = true;
            exitRequested_ = false;
            output_.addLine("warning: " + st.name + " is still running: quit again to stop it (or 'run stop')");
            return;
        }
    }
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

Element Tui::render(const Component& input)
{
    const Dimensions dims = Terminal::Size();
    width_ = std::max(40, dims.dimx);
    const int bodyRows = std::max(8, dims.dimy - 2);   // header + hint line

    refreshSnapshot();
    for (auto& b : bars_) {
        b.shown = false;   // each pane that draws its bar sets it again
    }
    Element body = tlm_.view.view == TlmView::Console ? consoleScreen(bodyRows, input)
                                                      : telemetryScreen(bodyRows);
    return vbox({header(), body | size(HEIGHT, EQUAL, bodyRows), hintLine()});
}

void Tui::refreshSnapshot()
{
    if (tlm_.files != nullptr) {
        transfers_ = tlm_.files->transfers();
        fileCounts_ = tlm_.files->activeCounts();
        for (auto& line : tlm_.files->takeNotices()) {   // on the UI thread: output_ stays single-threaded
            output_.addLine(std::move(line));
        }
    }
    if (tlm_.script != nullptr) {
        for (auto& line : tlm_.script->takeOutput()) {
            output_.addLine(std::move(line));
        }
        script_ = tlm_.script->status();
    }
    if ((tlm_.store == nullptr && tlm_.console == nullptr) || (tlm_.view.paused && frozen_)) {
        return;
    }
    now_ = TlmClock::now();
    // Copy only what changed since the last frame.
    if (tlm_.store != nullptr && tlm_.store->version() != storeVersion_) {
        storeVersion_ = tlm_.store->version();
        snapshot_ = tlm_.store->snapshot();
    }
    if (tlm_.console != nullptr && tlm_.console->version() != consoleVersion_) {
        consoleVersion_ = tlm_.console->version();
        console_ = tlm_.console->snapshot();
    }
    frozen_ = tlm_.view.paused;
}

TlmTime Tui::displayNow() const
{
    return frozen_ ? now_ : TlmClock::now();
}

//  mcs   F1 Console  F2 Telemetry          TLM udp://… ● 1532 pkts · last 0.2 s  ⠙
Element Tui::header()
{
    const auto tab = [this](const char* label, TlmView v) {
        Element e = text(label);
        return tlm_.view.view == v ? e | bold | inverted : e | dim;
    };
    Elements parts{text(" mcs   ") | bold | color(Color::Cyan),
                   tab(" F1 Console ", TlmView::Console), text(" "),
                   tab(" F2 Telemetry ", TlmView::Telemetry), text("  ")};
    if (const std::string badge = transferBadge(fileCounts_); !badge.empty()) {
        parts.push_back(text(" " + badge + " ") | bold | color(Color::Cyan));
        parts.push_back(text(" "));
    }
    if (script_.running) {
        parts.push_back(text(" SCRIPT " + script_.name + " " + std::to_string(script_.line) + "/"
                             + std::to_string(script_.lines) + " ")
                        | bold | inverted | color(Color::Cyan));
        parts.push_back(text(" "));
    }
    if (frozen_) {
        parts.push_back(text(" PAUSED · F5 ") | bold | inverted | color(Color::Yellow));
    }
    parts.push_back(filler());
    parts.push_back(linkStatus() | flex_shrink);
    return hbox(std::move(parts));
}

Element Tui::linkStatus()
{
    if (tlm_.store == nullptr) {
        return text("telemetry not configured ") | dim;
    }
    const TlmCounters& c = snapshot_.counters;
    const bool received = c.lastRx.has_value();
    const double age = received ? secondsBetween(*c.lastRx, displayNow()) : 0.0;
    std::string error = tlm_.service == nullptr ? tlm_.receiverError : tlm_.service->lastError();

    Color dot = Color::GrayDark;
    if (!error.empty()) {
        dot = Color::Red;
    } else if (received) {
        dot = age < kFreshSeconds ? Color::Green : Color::Yellow;
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
        parts.push_back(text(received ? " · last " + ageText(age) : " · waiting") | dim);
    }
    const bool active = received && age < 1.0 && !frozen_;
    parts.push_back(text(std::string(" ") + (active ? kSpinner[tick_ % 10] : "·") + " ") | color(Color::Cyan));
    return hbox(std::move(parts));
}

std::string Tui::topPane() const
{
    return tlm_.view.pane == F1Pane::Console ? "console" : "events";
}

Element Tui::hintLine()
{
    const bool narrow = width_ < 120;
    if (tlm_.view.view == TlmView::Telemetry) {
        return text(narrow ? " ↑↓ move · Space or click tick · PgUp/PgDn or drag ┃ scroll · F1 console"
                           : " ↑↓ move · Space or click tick · PgUp/PgDn or drag ┃ scroll cards · F5 pause · "
                             "F1 console (or just type) · Shift+drag copies · Ctrl-C quit")
             | dim;
    }
    if (script_.running) {
        return text(" Script running: 'run stop' ends it · PgUp/PgDn output · Ctrl+↑↓ " + topPane()
                    + " · F3 events/console · F2 telemetry · Ctrl-C quit")
             | dim;
    }
    std::string hint = narrow ? " Tab complete · ↑↓ history · PgUp/PgDn output · Ctrl+↑↓ " + topPane()
                                    + " · F3 events/console"
                              : " Enter run · Tab complete · ↑↓ history · PgUp/PgDn output · Ctrl+↑↓ " + topPane()
                                    + " · drag ┃ to scroll · F3 events/console · F2 telemetry · F5 pause"
                                    + " · Ctrl-C clear/quit";
    if (width_ >= 160 && fileCounts_.total() != 0) {
        hint += " · 'file cancel <id>' stops a transfer";
    }
    return text(hint) | dim;
}

int Tui::thumbSize(const BarState& b)
{
    return std::clamp(b.visible * b.visible / std::max(1, b.total), 1, std::max(1, b.visible));
}

// A one-column bar `visible` rows high; blank when everything fits.
Element Tui::scrollBar(Bar which, int total, int visible, int top)
{
    BarState& b = bars_[static_cast<std::size_t>(which)];
    b.total = total;
    b.visible = std::max(1, visible);
    b.top = top;
    b.shown = true;
    Elements cells;
    if (total <= b.visible) {
        for (int i = 0; i < b.visible; ++i) {
            cells.push_back(text(" "));
        }
    } else {
        const int thumb = thumbSize(b);
        const int range = total - b.visible;
        const int pos = (std::clamp(top, 0, range) * (b.visible - thumb) + range / 2) / range;
        const bool dragging = drag_ && *drag_ == which;
        for (int i = 0; i < b.visible; ++i) {
            const bool onThumb = i >= pos && i < pos + thumb;
            cells.push_back(onThumb ? text("┃") | color(dragging ? Color::CyanLight : Color::Cyan) | bold
                                    : text("│") | color(Color::GrayDark));
        }
    }
    return vbox(std::move(cells)) | size(WIDTH, EQUAL, 1) | reflect(b.box);
}

// Scrolls the panel of `which` so its first visible row is `top`.
void Tui::setTop(Bar which, int top)
{
    const BarState& b = bars_[static_cast<std::size_t>(which)];
    top = std::clamp(top, 0, std::max(0, b.total - b.visible));
    const int back = b.total - b.visible - top;   // rows below the visible part
    switch (which) {
    case Bar::Log:
        (tlm_.view.pane == F1Pane::Console ? consoleScroll_ : eventScroll_) = back;
        break;
    case Bar::Output:
        outputScroll_ = back;
        break;
    case Bar::Cards:
        displayScroll_ = top;
        break;
    }
}

// Thumb centred on screen row `y` of the bar.
void Tui::dragTo(Bar which, int y)
{
    const BarState& b = bars_[static_cast<std::size_t>(which)];
    const int thumb = thumbSize(b);
    const int travel = b.visible - thumb;
    const int range = b.total - b.visible;
    if (travel <= 0 || range <= 0) {
        return;
    }
    const int pos = std::clamp(y - b.box.y_min - thumb / 2, 0, travel);
    setTop(which, (pos * range + travel / 2) / travel);
}

void Tui::onMouse(const Mouse& m)
{
    if (m.button == Mouse::Left && m.motion == Mouse::Pressed) {
        const auto bars = tlm_.view.view == TlmView::Console ? std::vector<Bar>{Bar::Log, Bar::Output}
                                                            : std::vector<Bar>{Bar::Cards};
        for (const Bar which : bars) {
            const BarState& b = bars_[static_cast<std::size_t>(which)];
            // The column left of the bar counts too, so it is easy to grab.
            if (b.shown && b.total > b.visible && m.x >= b.box.x_min - 1 && m.x <= b.box.x_max
                && m.y >= b.box.y_min && m.y <= b.box.y_max) {
                drag_ = which;
                dragTo(which, m.y);
                return;
            }
        }
        if (tlm_.view.view == TlmView::Telemetry) {
            clickChecklist(m.x, m.y);
        }
        return;
    }
    if (drag_ && m.motion == Mouse::Moved) {
        dragTo(*drag_, m.y);
        return;
    }
    if (m.motion == Mouse::Released) {
        drag_.reset();
    }
}

// F2: a click on a packet of the checklist ticks or unticks it (and puts
// the cursor there); a click on "▲/▼ N more" pages the list.
void Tui::clickChecklist(int x, int y)
{
    for (std::size_t k = 0; k < listBoxes_.size() && k < listRowItem_.size(); ++k) {
        const Box& box = listBoxes_[k];
        if (x < box.x_min || x > box.x_max || y < box.y_min || y > box.y_max) {
            continue;
        }
        const int last = std::max(0, static_cast<int>(items_.size()) - 1);
        const int row = listRowItem_[k];
        // A page up/down: the list moves by its height, the cursor goes to the
        // first row of the new page (checklist() keeps it in view).
        if (row == kMoreAbove) {
            listScroll_ = std::max(0, listScroll_ - listPage_);
            listCursor_ = listScroll_;
        } else if (row == kMoreBelow) {
            listScroll_ = std::min(std::max(0, last + 1 - listPage_), listScroll_ + listPage_);
            listCursor_ = listScroll_;
        } else if (row <= last) {
            listCursor_ = row;
            toggleCursorItem();
        }
        return;
    }
}

} // namespace tui

int runTui(cli::Cli& cli, CommandService& service, TlmContext& tlm)
{
    tui::Tui tui(cli, service, tlm);
    return tui.run();
}

} // namespace mcs::ui
