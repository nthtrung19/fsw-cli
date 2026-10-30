#pragma once

// The full-screen session's state and its methods (internal to ui/). The methods are
// defined in tui_session.cpp (lifecycle, keys, mouse, frame), tui_console_screen.cpp
// (F1) and tui_telemetry_screen.cpp (F2).

#include "service/command_service.hpp"
#include "ui/tlm_context.hpp"
#include "ui/tui_widgets.hpp"
#include "core/text.hpp"
#include "ui/completion.hpp"
#include "ui/help_format.hpp"
#include "ui/menu_builder.hpp"
#include "ui/script_runner.hpp"
#include "ui/transfer_format.hpp"

#include <cli/cli.h>

#include <ftxui/component/app.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <cctype>
#include <deque>
#include <optional>
#include <set>
#include <sstream>
#include <streambuf>
#include <string>
#include <vector>

namespace mcs::ui::tui {

using namespace ftxui;

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

class Tui {
public:
    Tui(cli::Cli& cli, CommandService& service, TlmContext& tlm)
        : service_(service), tlm_(tlm), out_(&output_), session_(cli, out_)
    {
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

    int run();

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

    void onTelemetry(bool arrived);

    static constexpr std::chrono::milliseconds kDataFrameGap{50};
    static constexpr std::chrono::milliseconds kIdleFrameGap{250};

    void requestRedraw(std::chrono::milliseconds minGap = kDataFrameGap);

    bool onEvent(const Event& e);

    bool onConsoleKey(const Event& e);

    bool onTelemetryKey(const Event& e);

    void submit();

    void complete();

    void updatePrompt();

    void quit();

    Element render(const Component& input);

    void refreshSnapshot();

    TlmTime displayNow() const;

    Element header();

    Element linkStatus();

    std::string topPane() const;

    Element hintLine();

    // ---- scrollbars: drag the thumb (┃) to scroll a panel --------------------------
    //
    // The log pane and COMMAND HISTORY on F1, the cards on F2. Each frame a bar
    // records where it is on screen (reflect) and what it shows; a left press on
    // it starts a drag, moves follow the mouse, a release ends it. The wheel is
    // not used.

    enum class Bar { Log, Output, Cards };
    static constexpr std::size_t kBars = 3;

    struct BarState {
        Box box;           // screen cells of the bar, set when drawn
        int total = 0;     // rows of content
        int visible = 0;   // rows shown
        int top = 0;       // first row shown, from the top
        bool shown = false;   // drawn in the last frame
    };

    static int thumbSize(const BarState& b);

    Element scrollBar(Bar which, int total, int visible, int top);

    void setTop(Bar which, int top);

    void dragTo(Bar which, int y);

    void onMouse(const Mouse& m);

    void clickChecklist(int x, int y);

    Element consoleScreen(int rows, const Component& input);

    // ---- F1 TRANSFERS strip: running and waiting file transfers ----------------------
    //
    //   ╭ TRANSFERS 2 active ───────────────────────────────── 'file list' ╮
    //   │ ↑ put 0.23_1729061234  test.bin → /cf/test.bin   c2  ████░░ 76%  48.2 KB/s  5 s │
    //   ╰──────────────────────────────────────────────────────────────────╯

    static constexpr int kMaxStripRows = 3;

    int activeTransfers() const;

    int transfersRows() const;

    Element stripRow(const StripRow& r);

    Element transfersPane();

    Element paneTitle(const std::string& name, const std::string& count);

    Element eventsPane(int box);

    Element consoleLink();

    Element fswConsolePane(int box);

    Element outputRow(const OutputLine& line, const std::string& row, bool whole);

    Element outputPane();

    Element inputLine(const Component& input);

    std::vector<std::string> completionLines() const;

    int completionRows(int rows) const;

    Element completionArea(int rows);

    Element telemetryScreen(int rows);

    void buildChecklist();

    void toggleCursorItem();

    Element checklist(int rows);

    Element cardTitle(Element badge, const std::string& name, std::uint16_t mid, const std::string& facts,
                      double age, std::size_t gaps, std::size_t lengthErrors);

    Element packetCard(const PacketState& p, int maxRows, int maxWidth, int& width, int& height);

    Element hexCard(const UnknownMid& u, int& width, int& height);

    Element waitingCard(const ChecklistItem& item, int& width, int& height);

    Element emptyState();

    Element displayArea(int rows, int areaWidth);

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
    bool scriptQuitWarned_ = false;                 // quit asked once while a script ran
    ScriptRunner::Status script_;                   // "run <script>" state of the last frame
    std::vector<cfdp::TransferStatus> transfers_;   // file transfers of the last frame
    FileTransferService::Counts fileCounts_;

    int width_ = 80;
    int outputRows_ = 5;                 // console: visible output lines
    int outputScroll_ = 0;               // console: display rows scrolled back
    int outputWidth_ = 100;              // console: columns of an output line (help layout)
    int eventScroll_ = 0;                // console: events scrolled back
    int consoleScroll_ = 0;              // console: FSW console lines scrolled back
    int displayRows_ = 10;               // telemetry: height of the card area
    int displayScroll_ = 0;              // telemetry: card area, in lines
    int listCursor_ = 0;
    int listScroll_ = 0;
    int listPage_ = 1;                   // visible checklist rows ("▲/▼ more" clicks page by this)
    static constexpr int kMoreAbove = -1;
    static constexpr int kMoreBelow = -2;
    std::vector<Box> listBoxes_;         // checklist lines of the last frame, for clicks
    std::vector<int> listRowItem_;       // per line: item index, kMoreAbove or kMoreBelow
    std::vector<ChecklistItem> items_;   // checklist of the last frame
    unsigned tick_ = 0;

    std::array<BarState, kBars> bars_{};   // scrollbars of the last frame
    std::optional<Bar> drag_;              // the bar being dragged

    TlmSnapshot snapshot_;
    ConsoleSnapshot console_;            // FSW console text of the last frame
    TlmTime now_{};
    bool frozen_ = false;
    std::uint64_t storeVersion_ = ~std::uint64_t{0};     // version of snapshot_ (none yet)
    std::uint64_t consoleVersion_ = ~std::uint64_t{0};   // version of console_ (none yet)
};

} // namespace mcs::ui::tui
