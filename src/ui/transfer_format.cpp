#include "ui/transfer_format.hpp"

#include <algorithm>
#include <cmath>
#include <ctime>

namespace mcs::ui {

using namespace cfdp;

namespace {

// Number of UTF-8 code points (every glyph used here is one column wide).
std::size_t columns(const std::string& text)
{
    std::size_t n = 0;
    for (const char c : text) {
        n += (static_cast<unsigned char>(c) & 0xC0) != 0x80 ? 1 : 0;
    }
    return n;
}

// The first `count` code points.
std::string headOf(const std::string& text, std::size_t count)
{
    std::size_t n = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) {
            if (n == count) {
                return text.substr(0, i);
            }
            ++n;
        }
    }
    return text;
}

// The last `count` code points.
std::string tailOf(const std::string& text, std::size_t count)
{
    std::size_t n = 0;
    for (std::size_t i = text.size(); i-- > 0;) {
        if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) {
            if (++n == count) {
                return text.substr(i);
            }
        }
    }
    return text;
}

std::string clockOf(TransferTime t)
{
    const std::time_t tt = TransferClock::to_time_t(t);
    std::tm tm{};
    ::localtime_r(&tt, &tm);
    char text[16];
    std::strftime(text, sizeof text, "%H:%M:%S", &tm);
    return text;
}

std::string kindOf(const TransferStatus& s)
{
    return s.direction == Direction::Uplink ? "put" : (s.requested ? "get" : "in");
}

std::string percentOf(const TransferStatus& s)
{
    if (!s.sizeKnown) {
        return "  ?%";
    }
    char text[8];
    std::snprintf(text, sizeof text, "%3d%%", static_cast<int>(s.percent()));
    return text;
}

std::string mark(State state)
{
    switch (state) {
    case State::Done: return "✔";
    case State::Cancelled:
    case State::Abandoned: return "⊘";
    case State::Failed: return "✘";
    default: return " ";
    }
}

std::string counts(const PduCounts& c)
{
    return "MD " + std::to_string(c.metadata) + " · FD " + std::to_string(c.fileData) + " · EOF "
         + std::to_string(c.eof) + " · ACK " + std::to_string(c.ack) + " · NAK " + std::to_string(c.nak)
         + " · FIN " + std::to_string(c.fin);
}

} // namespace

std::string transferBadge(const FileTransferService::Counts& c)
{
    if (c.total() == 0) {
        return "";
    }
    std::string text = "FILES";
    if (c.up != 0) {
        text += " ↑" + std::to_string(c.up);
    }
    if (c.down != 0) {
        text += " ↓" + std::to_string(c.down);
    }
    if (c.waiting != 0) {
        text += (c.up + c.down != 0 ? " ·" : "") + std::string(" ") + std::to_string(c.waiting) + " waiting";
    }
    return text;
}

std::string shortenMiddle(const std::string& text, std::size_t width)
{
    const std::size_t n = columns(text);
    if (n <= width) {
        return text;
    }
    if (width <= 1) {
        return "…";
    }
    // Keep more of the end: it holds the file name.
    const std::size_t tail = (width - 1) * 2 / 3;
    const std::size_t head = width - 1 - tail;
    return headOf(text, head) + "…" + tailOf(text, tail);
}

std::string progressBar(double percent, int cells)
{
    const double p = std::clamp(percent, 0.0, 100.0);
    const int full = static_cast<int>(std::lround(p * cells / 100.0));
    std::string bar;
    for (int i = 0; i < cells; ++i) {
        bar += i < full ? "█" : "░";
    }
    return bar;
}

std::string transferFiles(const TransferStatus& s)
{
    return s.direction == Direction::Uplink ? s.local + " → " + s.remote : s.remote + " → " + s.local;
}

StripRow formatStripRow(const TransferStatus& s, int width, bool compact)
{
    StripRow row;
    row.uplink = s.direction == Direction::Uplink;
    row.waiting = s.state == State::Waiting;
    row.suspended = s.state == State::Suspended;
    row.arrow = row.uplink ? "↑" : "↓";
    row.kind = kindOf(s);
    row.cls = "c" + std::to_string(s.cls);
    if (!compact && !row.waiting) {
        row.id = s.id.toString();
    }
    if (row.waiting) {
        row.note = s.note;
    } else {
        row.bar = progressBar(s.percent(), 20);
        row.percent = percentOf(s);
        row.rate = s.rateBps > 0 ? formatRate(s.rateBps) : "";
        if (!compact && s.etaSeconds) {
            row.eta = formatSeconds(*s.etaSeconds);
        }
        if (row.suspended) {
            row.note = "SUSPENDED";
        }
    }
    // The room left for the file names: the row measured without them.
    const std::size_t fixed = columns(toText(row));
    const std::size_t room = width > static_cast<int>(fixed) + 12 ? static_cast<std::size_t>(width) - fixed : 12;
    const std::string from = row.uplink ? s.local : s.remote;
    const std::string to = row.uplink ? s.remote : s.local;
    if (columns(from) + 3 + columns(to) <= room) {
        row.files = from + " → " + to;
    } else {
        // Shorten each side separately, so both file names stay visible.
        const std::string left = shortenMiddle(from, std::min(columns(from), (room - 3) / 2));
        row.files = left + " → " + shortenMiddle(to, room - 3 - columns(left));
    }
    return row;
}

std::string toText(const StripRow& r)
{
    std::string text = r.arrow + " " + r.kind;
    if (!r.id.empty()) {
        text += " " + r.id;
    }
    text += "  " + r.files + "  " + r.cls;
    if (r.waiting) {
        return text + "  " + r.note;
    }
    text += "  " + r.bar + " " + r.percent;
    if (!r.rate.empty()) {
        text += "  " + r.rate;
    }
    if (!r.eta.empty()) {
        text += "  " + r.eta;
    }
    if (!r.note.empty()) {
        text += "  " + r.note;
    }
    return text;
}

std::vector<std::string> listLines(const std::vector<TransferStatus>& transfers, std::size_t maxRecent)
{
    std::vector<std::string> active;
    std::vector<std::string> recent;
    for (const auto& s : transfers) {
        const std::string arrow = s.direction == Direction::Uplink ? "↑" : "↓";
        const std::string head = arrow + " c" + std::to_string(s.cls) + "  " + kindOf(s) + "  ";
        if (s.state == State::Waiting) {
            active.push_back("  –                 " + head + shortenMiddle(s.remote, 50) + "  " + s.note);
        } else if (!isFinished(s.state)) {
            std::string id = s.id.toString();
            id.resize(std::max<std::size_t>(id.size(), 17), ' ');
            std::string line = "  " + id + " " + head + shortenMiddle(transferFiles(s), 60) + "  " + percentOf(s)
                             + "  " + formatBytes(s.bytesDone) + "/" + (s.sizeKnown ? formatBytes(s.fileSize) : "?");
            if (s.rateBps > 0) {
                line += "  " + formatRate(s.rateBps);
            }
            if (s.state == State::Suspended) {
                line += "  SUSPENDED";
            } else if (!s.note.empty()) {
                line += "  " + s.note;
            }
            active.push_back(line);
        } else if (recent.size() < maxRecent) {
            std::string id = s.id.source.empty() ? std::string("–") : s.id.toString();
            id.resize(std::max<std::size_t>(id.size(), 17), ' ');
            std::string line = "  " + mark(s.state) + " " + id + " " + head + shortenMiddle(transferFiles(s), 60) + "  ";
            if (s.state == State::Done) {
                const double secs = std::chrono::duration<double>(s.end - s.start).count();
                line += (s.sizeKnown ? formatBytes(s.fileSize) : "?") + "  " + formatSeconds(secs);
            } else {
                line += std::string(toString(s.state)) + (s.condition.empty() ? "" : " " + s.condition);
            }
            if (!s.note.empty() && s.state != State::Done) {
                line += "  (" + s.note + ")";
            }
            recent.push_back(line);
        }
    }
    std::vector<std::string> out;
    if (active.empty() && recent.empty()) {
        out.push_back("no transfers yet ('file put <local> <remote>' or 'file get <remote>')");
        return out;
    }
    out.push_back("ACTIVE" + std::string(active.empty() ? "  (none)" : ""));
    out.insert(out.end(), active.begin(), active.end());
    if (!recent.empty()) {
        out.push_back("RECENT");
        out.insert(out.end(), recent.begin(), recent.end());
    }
    return out;
}

std::vector<std::string> statusLines(const TransferStatus& s)
{
    std::vector<std::string> out;
    std::string head = (s.id.source.empty() ? std::string("–") : s.id.toString()) + "  " + kindOf(s) + " · class "
                     + std::to_string(s.cls) + " · " + toString(s.state);
    if (!s.condition.empty() && s.condition != "NO_ERROR") {
        head += "  " + s.condition;
    }
    out.push_back(head);
    out.push_back("  remote   " + s.remote);
    out.push_back("  local    " + s.local);
    std::string bytes = "  bytes    " + std::to_string(s.bytesDone) + " / "
                      + (s.sizeKnown ? std::to_string(s.fileSize) : std::string("?"));
    if (s.sizeKnown) {
        bytes += " (" + percentOf(s).substr(percentOf(s).find_first_not_of(' ')) + ")";
    }
    if (s.rateBps > 0) {
        bytes += "  rate " + formatRate(s.rateBps);
    }
    if (s.etaSeconds && !isFinished(s.state)) {
        bytes += "  eta " + formatSeconds(*s.etaSeconds);
    }
    out.push_back(bytes);
    if (s.state != State::Waiting) {
        out.push_back("  sent     " + counts(s.sent));
        out.push_back("  received " + counts(s.received));
        if (s.bytesResent != 0) {
            out.push_back("  resent   " + formatBytes(s.bytesResent));
        }
    }
    if (!s.note.empty()) {
        out.push_back("  note     " + s.note);
    }
    std::string times = "  started  " + clockOf(s.start);
    if (isFinished(s.state)) {
        const double secs = std::chrono::duration<double>(s.end - s.start).count();
        times += " · ended " + clockOf(s.end) + " (" + formatSeconds(secs) + ")";
    }
    out.push_back(times);
    return out;
}

} // namespace mcs::ui
